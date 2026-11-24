/*
 * evo_websub - subtitle search + download from subtitle websites.
 * See evo_websub.h and docs/planning/websubs.md.
 *
 * Sites (v1): Podnapisi (no key, JSON /search/advanced, zip at /<pid>/download),
 * SubDL (free key, JSON /api/v1/subtitles, downloads under dl.subdl.com),
 * OpenSubtitles.com (free key, hash+query search, downloads via the
 * /api/v1/download endpoint - rows carry the file_id and the fetch worker
 * does the two-step when picked).
 */
#include "evo_websub.h"
#include "evo_net.h"
#include "evo_data_path.h"
#include "evo_provider_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <libgen.h>
#include <inttypes.h>
#include <zlib.h>

#include "cJSON.h"

/* ------------------------------------------------------------------ */
/* state                                                              */

#define MAX_RESULTS EVO_WEBSUB_MAX_RESULTS

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static evo_websub_result_t g_results[MAX_RESULTS];
static int g_result_count = 0;
static int g_status = 0;            /* 0 idle, 1 searching, 2 done, 3 failed */
static int g_search_running = 0;
static char g_stage[64] = {0};      /* what the picker shows while searching */
static uint64_t g_started_ms = 0;

/* The whole search is bounded: each site's request has its own socket
 * timeouts, and this deadline stops the tail sites from stacking after a
 * slow one. The picker turns 3 into "no answer - check the internet". */
#define EVO_WEBSUB_DEADLINE_MS 45000

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void set_stage(const char *s)
{
    pthread_mutex_lock(&g_mu);
    snprintf(g_stage, sizeof g_stage, "%s", s);
    pthread_mutex_unlock(&g_mu);
}

static int g_fetch_busy = 0;
static int g_fetch_done = 0;        /* 0 none, 1 ready, -1 failed */
static char g_fetch_path[512] = {0};
static char g_fetch_label[96] = {0};
static int g_fetch_cached = 0;      /* last take came from the download db */

/* OpenSubtitles user token from /api/v1/login; valid ~24 h, memory only. */
static char g_os_token[512] = {0};

/* The file the current result list belongs to. The picker compares it with
 * the file being played, so a new movie never shows the old movie's rows. */
static char g_owner[768] = {0};

/* ------------------------------------------------------------------ */
/* Download database (evo_data_path("websub_downloads.json"))          */
/*
 * {"downloads": {"<site>|<url>": "<saved path>", ...}}
 *
 * Every saved subtitle is recorded here. A row that is picked again is
 * served from its recorded file - no network, and no second copy of the
 * same subtitle with an incremented name. New downloads pick a free
 * increment (<movie>.en.srt -> <movie>.en.2.srt) so nothing is overwritten.
 */

static cJSON *websub_db_read(void)
{
    FILE *f = fopen(evo_data_path("websub_downloads.json"), "rb");
    if (!f) return NULL;
    char buf[16384];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return cJSON_Parse(buf);
}

static void websub_db_write(cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    if (!s) return;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", evo_data_path("websub_downloads.json"));
    FILE *f = fopen(tmp, "wb");
    if (f) {
        fputs(s, f);
        fclose(f);
        rename(tmp, evo_data_path("websub_downloads.json"));
    }
    free(s);
}

/* key = "<site>|<url>" -> saved path */
static int websub_db_get(const char *key, char *out, int cap)
{
    out[0] = 0;
    cJSON *root = websub_db_read();
    if (!root) return 0;
    const cJSON *dl = cJSON_GetObjectItemCaseSensitive(root, "downloads");
    const cJSON *e = dl ? cJSON_GetObjectItemCaseSensitive(dl, key) : NULL;
    int ok = cJSON_IsString(e) && e->valuestring && e->valuestring[0];
    if (ok) snprintf(out, (size_t)cap, "%s", e->valuestring);
    cJSON_Delete(root);
    return ok;
}

static void websub_db_put(const char *key, const char *path)
{
    cJSON *root = websub_db_read();
    if (!root) root = cJSON_CreateObject();
    if (!root) return;
    cJSON *dl = cJSON_GetObjectItemCaseSensitive(root, "downloads");
    if (!cJSON_IsObject(dl)) {
        dl = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "downloads", dl);
    }
    cJSON_DeleteItemFromObject(dl, key);
    cJSON_AddStringToObject(dl, key, path);
    /* the db is a convenience cache, not an archive: cap at 200 entries */
    while (cJSON_GetArraySize(dl) > 200) {
        cJSON *first = cJSON_GetArrayItem(dl, 0);
        if (!first) break;
        cJSON_DeleteItemFromObject(dl, first->string ? first->string : "");
    }
    websub_db_write(root);
    cJSON_Delete(root);
}

static int websub_file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int evo_websub_is_our_file(const char *path)
{
    if (!path || !path[0]) return 0;
    cJSON *root = websub_db_read();
    if (!root) return 0;
    const cJSON *dl = cJSON_GetObjectItemCaseSensitive(root, "downloads");
    int found = 0;
    const cJSON *e = NULL;
    cJSON_ArrayForEach(e, dl) {
        if (cJSON_IsString(e) && e->valuestring && strcmp(e->valuestring, path) == 0) {
            found = 1;
            break;
        }
    }
    cJSON_Delete(root);
    return found;
}

/* query context captured when a search starts */
static char g_search_file[768] = {0};    /* the real media path (hashing) */
static char g_search_title[128] = {0};   /* filename parsed for keyword search */
static char g_search_query[128] = {0};   /* keyboard query when set */
static int  g_search_season = -1;
static int  g_search_episode = -1;
static int  g_search_year = -1;

/* ------------------------------------------------------------------ */
/* API keys (tiny JSON files under the data dir)                      */

static void key_load(const char *leaf, const char *field, char *out, int cap)
{
    out[0] = 0;
    FILE *f = fopen(evo_data_path(leaf), "rb");
    if (!f) return;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    cJSON *j = cJSON_Parse(buf);
    if (!j) return;
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(j, field);
    if (cJSON_IsString(k) && k->valuestring)
        snprintf(out, (size_t)cap, "%s", k->valuestring);
    cJSON_Delete(j);
}

static void key_save(const char *leaf, const char *field, const char *value)
{
    /* read-modify-write: the OpenSubtitles account sits next to its key in
     * the same file */
    char body[1024] = "{ }";
    size_t blen = 3;
    FILE *f = fopen(evo_data_path(leaf), "rb");
    if (f) {
        blen = fread(body, 1, sizeof(body) - 1, f);
        fclose(f);
        body[blen] = 0;
        if (!body[0]) strcpy(body, "{ }");
    }
    cJSON *j = cJSON_Parse(body);
    if (!j) j = cJSON_CreateObject();
    if (!j) return;
    /* this bundled cJSON has no ReplaceItem or case-sensitive delete -
     * plain delete-then-add has the same effect (delete of a missing key
     * is a no-op) */
    cJSON_DeleteItemFromObject(j, field);
    cJSON_AddStringToObject(j, field, value ? value : "");
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", evo_data_path(leaf));
    f = fopen(tmp, "wb");
    if (f) {
        fputs(s, f);
        fclose(f);
        rename(tmp, evo_data_path(leaf));
    }
    /* this bundled cJSON keeps cJSON_free private - its allocations are
     * plain malloc, so free() is correct */
    free(s);
}

void evo_websub_set_os_key(const char *key)    { key_save("opensubtitles.json", "api_key", key); }
void evo_websub_get_os_key(char *out, int cap) { key_load("opensubtitles.json", "api_key", out, cap); }
void evo_websub_set_subdl_key(const char *key) { key_save("subdl.json", "api_key", key); }
void evo_websub_get_subdl_key(char *out, int cap) { key_load("subdl.json", "api_key", out, cap); }

/* OpenSubtitles account: the /api/v1/download endpoint needs a user token,
 * the key alone only searches. Same plaintext-file convention as the stored
 * network-share passwords. */
void evo_websub_set_os_user(const char *user, const char *password)
{
    key_save("opensubtitles.json", "username", user);
    key_save("opensubtitles.json", "password", password);
    g_os_token[0] = 0;      /* force a fresh login with the new account */
}

void evo_websub_get_os_user(char *out_user, int ucap, char *out_pass, int pcap)
{
    key_load("opensubtitles.json", "username", out_user, ucap);
    key_load("opensubtitles.json", "password", out_pass, pcap);
}

/* ------------------------------------------------------------------ */
/* OpenSubtitles moviehash: 64-bit filesize + head/tail 64 KiB sums    */

static int os_hash_file(const char *path, char *out /* >= 21 bytes */)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long long size = ftell(f);
    if (size <= 0) { fclose(f); return -1; }

    const long long CHUNK = 65536;
    uint64_t h = (uint64_t)size;
    unsigned char buf[65536];
    for (int pass = 0; pass < 2; ++pass) {
        long long off = (pass == 0) ? 0 : (size > CHUNK ? size - CHUNK : 0);
        long long n = (size < CHUNK) ? size : CHUNK;
        if (fseek(f, off, SEEK_SET) != 0 || fread(buf, 1, (size_t)n, f) != (size_t)n) {
            fclose(f);
            return -1;
        }
        for (long long i = 0; i + 8 <= n; i += 8) {
            uint64_t v = 0;
            for (int b = 7; b >= 0; --b)          /* little-endian load */
                v = (v << 8) | buf[i + b];
            h += v;
        }
    }
    fclose(f);
    /* the API takes the moviehash as a DECIMAL string (hex gets a 400) and
     * the parameter is named moviehash */
    snprintf(out, 21, "%" PRIu64, h);
    return 0;
}

/* ------------------------------------------------------------------ */
/* filename parse: title, year, SxxEyy                                */

static int to_int(const char *s, int n)
{
    int v = 0;
    for (int i = 0; i < n && isdigit((unsigned char)s[i]); ++i)
        v = v * 10 + (s[i] - '0');
    return v;
}

/* Bare strcasestr is not in EVO's import set - a missing libc import is a
 * null call on the app module (see CLAUDE.md), so roll the small wheel. */
static const char *ws_strcasestr(const char *hay, const char *needle)
{
    if (!hay || !needle || !needle[0]) return NULL;
    for (; *hay; ++hay) {
        const char *h = hay, *nn = needle;
        while (*h && *nn && tolower((unsigned char)*h) == tolower((unsigned char)*nn)) {
            ++h;
            ++nn;
        }
        if (!*nn) return hay;
    }
    return NULL;
}

static void parse_media_name(const char *path, char *title, int tcap,
                             int *season, int *episode, int *year)
{
    *season = *episode = *year = -1;
    title[0] = 0;

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    char work[512];
    snprintf(work, sizeof work, "%s", base);
    char *dot = strrchr(work, '.');
    if (dot && dot != work) *dot = 0;            /* strip extension */

    /* season/episode markers: S01E02 or 1x02 */
    char *cut = NULL;
    for (char *p = work; p[0] && p[1] && p[2] && p[3]; ++p) {
        if ((p[0] == 'S' || p[0] == 's') && isdigit((unsigned char)p[1]) &&
            isdigit((unsigned char)p[2]) &&
            (p[3] == 'E' || p[3] == 'e') && isdigit((unsigned char)p[4])) {
            *season = to_int(p + 1, 2);
            *episode = to_int(p + 4, 2);
            cut = p;
            break;
        }
        if (isdigit((unsigned char)p[0]) && p[1] == 'x' && isdigit((unsigned char)p[2])) {
            *season = to_int(p, 1);
            *episode = to_int(p + 2, 2);
            cut = p;
            break;
        }
    }

    /* a standalone year anywhere */
    for (char *p = work; p[0] && p[1] && p[2] && p[3]; ++p) {
        if (isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]) &&
            isdigit((unsigned char)p[2]) && isdigit((unsigned char)p[3]) &&
            (p[4] == 0 || !isdigit((unsigned char)p[4])) &&
            (p == work || !isdigit((unsigned char)p[-1])) &&
            (strncmp(p, "1920", 4) >= 0 && strncmp(p, "2099", 4) <= 0)) {
            *year = to_int(p, 4);
            if (!cut) cut = p;
            break;
        }
    }

    char *t = work;
    if (cut) *cut = 0;
    for (char *p = t; *p; ++p)
        if (*p == '.' || *p == '_') *p = ' ';

    /* strip the usual release tags from the tail */
    static const char *tags[] = {
        "1080p", "720p", "2160p", "4k", "x264", "x265", "h264", "h265",
        "hevc", "avc", "av1", "web-dl", "webdl", "webrip", "bluray", "blu-ray",
        "brrip", "bdrip", "dvdrip", "hdtv", "aac", "ac3", "ddp5", "dd5",
        "10bit", "hdr", "dv", "remux", "proper", "repack", "extended",
        "multi", "dual", "amzn", "nf", "atvp", "hmax", "dsnp", NULL
    };
    for (int done = 0; !done;) {
        done = 1;
        for (int i = 0; tags[i]; ++i) {
            char *p = (char *)ws_strcasestr(t, tags[i]);
            if (p && (p == t || p[-1] == ' ')) {
                *p = 0;
                done = 0;
            }
        }
    }
    while (t[0] == ' ') ++t;
    char *e = t + strlen(t);
    while (e > t && (e[-1] == ' ' || e[-1] == '-' || e[-1] == '(')) *--e = 0;
    snprintf(title, (size_t)tcap, "%s", t);
}

/* ------------------------------------------------------------------ */
/* URL encoding                                                       */

static void url_encode(const char *in, char *out, int cap)
{
    int o = 0;
    for (const char *p = in; *p && o < cap - 4; ++p) {
        if (isalnum((unsigned char)*p) || strchr("-_.~", *p)) {
            out[o++] = *p;
        } else if (*p == ' ') {
            out[o++] = '+';
        } else {
            snprintf(out + o, 4, "%%%02X", (unsigned char)*p);
            o += 3;
        }
    }
    out[o] = 0;
}

/* ------------------------------------------------------------------ */
/* unzip / gunzip: find the first .srt in an archive                  */

/* Returns a malloc'd buffer with the .srt bytes (nul-terminated) or NULL.
 * Handles: plain .srt content, single gzip stream, zip archives. */
static unsigned char *srt_from_payload(const unsigned char *in, size_t n, size_t *out_len)
{
    *out_len = 0;
    if (!in || n < 4) return NULL;

    /* plain srt: printable text */
    if (!(in[0] == 0x1f && in[1] == 0x8b) && memcmp(in, "PK\x03\x04", 4) != 0) {
        unsigned char *s = malloc(n + 1);
        if (!s) return NULL;
        memcpy(s, in, n);
        s[n] = 0;
        *out_len = n;
        return s;
    }

    z_stream zs;
    memset(&zs, 0, sizeof zs);

    /* single gzip stream */
    if (in[0] == 0x1f && in[1] == 0x8b) {
        if (inflateInit2(&zs, 15 + 32) != Z_OK) return NULL;
        size_t cap = n * 8 + 65536;
        unsigned char *out = malloc(cap);
        if (!out) { inflateEnd(&zs); return NULL; }
        zs.next_in = (Bytef *)in;
        zs.avail_in = (uInt)n;
        zs.next_out = out;
        zs.avail_out = (uInt)cap;
        int rc = inflate(&zs, Z_FINISH);
        *out_len = zs.total_out;
        inflateEnd(&zs);
        if (rc < 0 && rc != Z_BUF_ERROR) { free(out); return NULL; }
        if (*out_len == 0) { free(out); return NULL; }
        out[*out_len] = 0;
        return out;
    }

    /* zip: walk the central directory for the first .srt */
    if (n < 22) return NULL;
    long long eocd = -1;
    for (long long i = (long long)n - 22; i >= 0 && i > (long long)n - 66000; --i) {
        if (in[i] == 0x50 && in[i+1] == 0x4b && in[i+2] == 0x05 && in[i+3] == 0x06) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) return NULL;
    unsigned entries = (unsigned)in[eocd+10] | ((unsigned)in[eocd+11] << 8);
    long long cd_off = (long long)((unsigned)in[eocd+16] | ((unsigned)in[eocd+17] << 8) |
                       ((unsigned)in[eocd+18] << 16) | ((unsigned)in[eocd+19] << 24));

    const unsigned char *p = in + cd_off;
    for (unsigned e = 0; e < entries && (const unsigned char *)p + 46 <= in + n; ++e) {
        if (p[0] != 0x50 || p[1] != 0x4b || p[2] != 0x01 || p[3] != 0x02) break;
        unsigned method = (unsigned)p[10] | ((unsigned)p[11] << 8);
        unsigned csize  = (unsigned)p[20] | ((unsigned)p[21] << 8) | ((unsigned)p[22] << 16) | ((unsigned)p[23] << 24);
        unsigned usize  = (unsigned)p[24] | ((unsigned)p[25] << 8) | ((unsigned)p[26] << 16) | ((unsigned)p[27] << 24);
        unsigned nlen   = (unsigned)p[28] | ((unsigned)p[29] << 8);
        unsigned elen   = (unsigned)p[30] | ((unsigned)p[31] << 8);
        unsigned clen   = (unsigned)p[32] | ((unsigned)p[33] << 8);
        long long lho   = (long long)((unsigned)p[42] | ((unsigned)p[43] << 8) |
                          ((unsigned)p[44] << 16) | ((unsigned)p[45] << 24));
        const unsigned char *name = p + 46;
        int is_srt = (nlen >= 4) &&
            (strncasecmp((const char *)name + nlen - 4, ".srt", 4) == 0);
        if (is_srt) {
            /* local header: its own name/extra lengths may differ */
            if (lho < 0 || lho + 30 > (long long)n) return NULL;
            const unsigned char *lh = in + lho;
            if (lh[0] != 0x50 || lh[1] != 0x4b || lh[2] != 0x03 || lh[3] != 0x04) return NULL;
            unsigned lnlen = (unsigned)lh[26] | ((unsigned)lh[27] << 8);
            unsigned lelen = (unsigned)lh[28] | ((unsigned)lh[29] << 8);
            const unsigned char *data = lh + 30 + lnlen + lelen;
            if (data + csize > in + n) return NULL;

            unsigned char *out = malloc((size_t)usize + 1);
            if (!out) return NULL;
            if (method == 0) {
                if (csize != usize) { free(out); return NULL; }
                memcpy(out, data, usize);
                *out_len = usize;
            } else if (method == 8) {
                memset(&zs, 0, sizeof zs);
                if (inflateInit2(&zs, -15) != Z_OK) { free(out); return NULL; }
                zs.next_in = (Bytef *)data;
                zs.avail_in = csize;
                zs.next_out = out;
                zs.avail_out = usize;
                int rc = inflate(&zs, Z_FINISH);
                inflateEnd(&zs);
                if (rc != Z_STREAM_END) { free(out); return NULL; }
                *out_len = zs.total_out;
            } else {
                free(out);
                return NULL;
            }
            out[*out_len] = 0;
            return out;
        }
        p = name + nlen + elen + clen;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* per-site search (worker thread)                                    */

/* Release names arrive in whatever encoding the site's users used - SubDL
 * rows regularly carry decorative Unicode (mathematical bold etc.) that EVO's
 * UI font has no glyphs for, which rendered as rows of "?". Keep the plain
 * ASCII core and fall back to site + language when nothing survives. */
static void sanitize_label(char *label, int cap)
{
    if (!label) return;
    char out[128];
    int o = 0;
    int spaced = 0;
    for (const char *p = label; *p && o < (int)sizeof out - 1; ++p) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || (c > 32 && c < 127 && c != '"')) {
            if (c == ' ' && spaced) continue;
            spaced = (c == ' ');
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
    while (o > 0 && out[o - 1] == ' ') out[--o] = 0;
    /* a name whose letters were all decorative Unicode leaves bracket junk
     * like "[]()" - below 4 real letters it is not a usable label, so the
     * caller falls back to site + language */
    int alnum = 0;
    for (const char *p = out; *p; ++p)
        if (isalnum((unsigned char)*p)) ++alnum;
    if (alnum < 4) out[0] = 0;
    snprintf(label, (size_t)cap, "%s", out);
}

static void add_result_locked(const char *label, const char *lang,
                              const char *site, const char *url)
{
    if (g_result_count >= MAX_RESULTS) return;
    if (!url || !url[0]) return;
    /* duplicate URLs are dropped, like evo_online_subs */
    for (int i = 0; i < g_result_count; ++i)
        if (strcmp(g_results[i].url, url) == 0) return;
    char clean[128] = {0};
    snprintf(clean, sizeof clean, "%s", (label && label[0]) ? label : "");
    sanitize_label(clean, (int)sizeof clean);
    evo_websub_result_t *r = &g_results[g_result_count++];
    if (clean[0])
        snprintf(r->label, sizeof r->label, "%s", clean);
    else
        snprintf(r->label, sizeof r->label, "%s SUBTITLE%s%s",
                 site, lang && lang[0] ? " · " : "", lang && lang[0] ? lang : "");
    snprintf(r->lang_tag, sizeof r->lang_tag, "%s", lang ? lang : "");
    snprintf(r->site, sizeof r->site, "%s", site);
    snprintf(r->url, sizeof r->url, "%s", url);
    snprintf(r->detail, sizeof r->detail, "%s%s%s",
             site, lang && lang[0] ? " · " : "", lang && lang[0] ? lang : "");
}

/* Each search_* returns how many rows it added; 0 is a valid answer, <0 a
 * request/parse failure (logged, so an empty picker is explainable). */
static int search_podnapisi(void)
{
    const char *q = g_search_query[0] ? g_search_query : g_search_title;
    if (!q[0]) return -1;
    char qenc[256];
    url_encode(q, qenc, sizeof qenc);
    char url[1024];
    snprintf(url, sizeof url,
             "https://www.podnapisi.net/subtitles/search/advanced?keywords=%s",
             qenc);
    if (g_search_season >= 0 && g_search_episode >= 0) {
        char extra[96];
        snprintf(extra, sizeof extra, "&seasons=%d&episodes=%d",
                 g_search_season, g_search_episode);
        strcat(url, extra);
    } else if (g_search_year > 0) {
        char extra[32];
        snprintf(extra, sizeof extra, "&year=%d", g_search_year);
        strcat(url, extra);
    }

    char *body = NULL;
    size_t len = 0;
    int status = 0;
    int rc = evo_net_http_get_sync(url, NULL, 0, &body, &len, &status);
    if (rc != 0 || !body) {
        PROV_WARN("websub: podnapisi request failed rc=%d", rc);
        return -1;
    }

    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) {
        PROV_WARN("websub: podnapisi reply was not JSON (%d bytes)", (int)len);
        return -1;
    }
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(j, "data");
    int added = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, data) {
        if (added >= 12) break;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
        const cJSON *lang = cJSON_GetObjectItemCaseSensitive(it, "language");
        const cJSON *rel = cJSON_GetObjectItemCaseSensitive(it, "releases");
        char label[64] = {0};
        if (cJSON_IsArray(rel)) {
            const cJSON *r0 = cJSON_GetArrayItem(rel, 0);
            if (cJSON_IsString(r0) && r0->valuestring)
                snprintf(label, sizeof label, "%s", r0->valuestring);
        }
        if (!label[0]) {
            const cJSON *mv = cJSON_GetObjectItemCaseSensitive(it, "movie");
            const cJSON *mt = mv ? cJSON_GetObjectItemCaseSensitive(mv, "title") : NULL;
            if (cJSON_IsString(mt) && mt->valuestring)
                snprintf(label, sizeof label, "%s", mt->valuestring);
        }
        if (cJSON_IsNumber(id)) {
            char dl[256];
            snprintf(dl, sizeof dl,
                     "https://www.podnapisi.net/subtitles/%" PRIi64 "/download",
                     (int64_t)id->valuedouble);
            pthread_mutex_lock(&g_mu);
            add_result_locked(label,
                              cJSON_IsString(lang) && lang->valuestring ? lang->valuestring : "",
                              "PODNAPISI", dl);
            pthread_mutex_unlock(&g_mu);
            ++added;
        }
    }
    cJSON_Delete(j);
    return added;
}

static int search_subdl(const char *api_key)
{
    if (!api_key || !api_key[0]) return 0;
    const char *q = g_search_query[0] ? g_search_query : g_search_title;
    if (!q[0]) return -1;
    char qenc[256];
    url_encode(q, qenc, sizeof qenc);
    char url[1400];
    snprintf(url, sizeof url,
             "https://api.subdl.com/api/v1/subtitles?api_key=%s&film_name=%s&subs_per_page=10",
             api_key, qenc);
    if (g_search_season >= 0 && g_search_episode >= 0) {
        char extra[96];
        snprintf(extra, sizeof extra, "&season_number=%d&episode_number=%d&type=tv",
                 g_search_season, g_search_episode);
        strcat(url, extra);
    }

    char *body = NULL;
    size_t len = 0;
    int status = 0;
    int rc = evo_net_http_get_sync(url, NULL, 0, &body, &len, &status);
    if (rc != 0 || !body) {
        PROV_WARN("websub: subdl request failed rc=%d", rc);
        return -1;
    }

    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) {
        PROV_WARN("websub: subdl reply was not JSON (%d bytes)", (int)len);
        return -1;
    }
    const cJSON *subs = cJSON_GetObjectItemCaseSensitive(j, "subtitles");
    int added = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, subs) {
        if (added >= 10) break;
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(it, "url");
        if (!cJSON_IsString(u) || !u->valuestring || !u->valuestring[0]) continue;
        const cJSON *rn = cJSON_GetObjectItemCaseSensitive(it, "release_name");
        const cJSON *lg = cJSON_GetObjectItemCaseSensitive(it, "language");
        char dl[600];
        snprintf(dl, sizeof dl, "https://dl.subdl.com%s", u->valuestring);
        pthread_mutex_lock(&g_mu);
        add_result_locked(cJSON_IsString(rn) && rn->valuestring ? rn->valuestring : "",
                          cJSON_IsString(lg) && lg->valuestring ? lg->valuestring : "",
                          "SUBDL", dl);
        pthread_mutex_unlock(&g_mu);
        ++added;
    }
    cJSON_Delete(j);
    return added;
}

/* OS rows encode their file_id as "os-file://<id>"; the fetch worker
 * performs the /api/v1/download exchange when one is picked. */
static int search_opensubtitles(const char *api_key)
{
    if (!api_key || !api_key[0]) return 0;
    char hash[21] = {0};
    int have_hash = (g_search_file[0] == '/') && os_hash_file(g_search_file, hash) == 0;

    const char *q = g_search_query[0] ? g_search_query : g_search_title;
    char qenc[256] = {0};
    url_encode(q, qenc, sizeof qenc);
    char url[1400];
    if (have_hash && !g_search_query[0])
        snprintf(url, sizeof url,
                 "https://api.opensubtitles.com/api/v1/subtitles/?moviehash=%s&subs_per_page=10",
                 hash);
    else
        snprintf(url, sizeof url,
                 "https://api.opensubtitles.com/api/v1/subtitles/?query=%s&subs_per_page=10",
                 qenc);
    if (g_search_season >= 0 && g_search_episode >= 0) {
        char extra[96];
        snprintf(extra, sizeof extra, "&season_number=%d&episode_number=%d",
                 g_search_season, g_search_episode);
        strcat(url, extra);
    }

    const char *hdrs[2] = {
        "Api-Key:",
        "User-Agent: EVOPlayer v0.13.7",
    };
    char keyhdr[256];
    snprintf(keyhdr, sizeof keyhdr, "Api-Key: %s", api_key);
    hdrs[0] = keyhdr;

    char *body = NULL;
    size_t len = 0;
    int status = 0;
    int rc = evo_net_http_get_sync(url, hdrs, 2, &body, &len, &status);
    if (rc != 0 || !body) {
        PROV_WARN("websub: opensubtitles request failed rc=%d", rc);
        return -1;
    }

    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) {
        PROV_WARN("websub: opensubtitles reply was not JSON (%d bytes)", (int)len);
        return -1;
    }
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(j, "data");
    int added = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, data) {
        if (added >= 10) break;
        const cJSON *attr = cJSON_GetObjectItemCaseSensitive(it, "attributes");
        if (!attr) continue;
        const cJSON *files = cJSON_GetObjectItemCaseSensitive(attr, "files");
        const cJSON *f0 = cJSON_IsArray(files) ? cJSON_GetArrayItem(files, 0) : NULL;
        const cJSON *fid = f0 ? cJSON_GetObjectItemCaseSensitive(f0, "file_id") : NULL;
        if (!cJSON_IsNumber(fid)) continue;
        const cJSON *rel = cJSON_GetObjectItemCaseSensitive(attr, "release");
        const cJSON *lg = cJSON_GetObjectItemCaseSensitive(attr, "language");
        char osurl[96];
        snprintf(osurl, sizeof osurl, "os-file://%" PRIi64, (int64_t)fid->valuedouble);
        char label[64] = {0};
        if (cJSON_IsString(rel) && rel->valuestring)
            snprintf(label, sizeof label, "%s", rel->valuestring);
        char lang[16] = "";
        if (cJSON_IsString(lg) && lg->valuestring) {
            snprintf(lang, sizeof lang, "%s", lg->valuestring);   /* some rows: a plain string */
        } else if (cJSON_IsObject(lg)) {
            const cJSON *lc = cJSON_GetObjectItemCaseSensitive(lg, "language_code");
            if (cJSON_IsString(lc) && lc->valuestring)
                snprintf(lang, sizeof lang, "%s", lc->valuestring);
        }
        pthread_mutex_lock(&g_mu);
        add_result_locked(label, lang, "OPENSUBS", osurl);
        pthread_mutex_unlock(&g_mu);
        ++added;
    }
    cJSON_Delete(j);
    return added;
}

/* ------------------------------------------------------------------ */
/* search worker                                                      */

static void *search_worker(void *arg)
{
    (void)arg;
    char subdl_key[128] = {0}, os_key[128] = {0};
    key_load("subdl.json", "api_key", subdl_key, sizeof subdl_key);
    key_load("opensubtitles.json", "api_key", os_key, sizeof os_key);

    /* Podnapisi needs no key - run it first so the picker always has a free
     * source, then the keyed sites. Each stage updates g_stage so the picker
     * can show which site is being asked. */
    set_stage("PODNAPISI...");
    int pn = search_podnapisi();
    int hit_deadline = 0;
    {
        pthread_mutex_lock(&g_mu);
        hit_deadline = (now_ms() - g_started_ms) > EVO_WEBSUB_DEADLINE_MS;
        pthread_mutex_unlock(&g_mu);
    }
    int os = 0, sd = 0;
    if (!hit_deadline && os_key[0]) {
        set_stage("OPENSUBTITLES...");
        os = search_opensubtitles(os_key);
    }
    if (!hit_deadline && subdl_key[0]) {
        set_stage("SUBDL...");
        sd = search_subdl(subdl_key);
    }

    pthread_mutex_lock(&g_mu);
    /* Recommend the best rows: language first, then how much of the media
     * name the release echoes (title words, year). Stable insertion sort on
     * one score, so same-score rows keep their arrival order. */
    for (int i = 1; i < g_result_count; ++i) {
        const evo_websub_result_t *r = &g_results[i];
        int score = 0;
        const char *t = r->lang_tag;
        if ((t[0] == 'e' || t[0] == 'E') && (t[1] == 'n' || t[1] == 'N') &&
            (t[2] == 0 || t[2] == '-'))
            score += 45;
        char low[128];
        snprintf(low, sizeof low, "%s", r->label);
        for (char *p = low; *p; ++p) *p = (char)tolower((unsigned char)*p);
        /* title words of 3+ letters: 10 each, capped at 30 */
        char buf[128];
        snprintf(buf, sizeof buf, "%s", g_search_title);
        int words = 0, matched = 0;
        for (char *w = buf, *end; *w; w = end) {
            while (*w && !isalnum((unsigned char)*w)) ++w;
            if (!*w) break;
            end = w;
            while (*end && isalnum((unsigned char)*end)) ++end;
            size_t len = (size_t)(end - w);
            if (len < 3) continue;
            char word[64];
            snprintf(word, sizeof word, "%.*s", (int)len, w);
            if (++words <= 10 && ws_strcasestr(low, word)) ++matched;
        }
        score += (words ? matched * 30 / words : 0);
        if (g_search_year > 0) {
            char ys[8];
            snprintf(ys, sizeof ys, "%d", g_search_year);
            if (strstr(low, ys)) score += 10;
        }
        int ri = score;

        evo_websub_result_t tmp = g_results[i];
        int j = i - 1;
        for (;;) {
            const evo_websub_result_t *q = &g_results[j];
            int rj = 0;
            const char *u = q->lang_tag;
            if ((u[0] == 'e' || u[0] == 'E') && (u[1] == 'n' || u[1] == 'N') &&
                (u[2] == 0 || u[2] == '-'))
                rj += 45;
            char qlow[128];
            snprintf(qlow, sizeof qlow, "%s", q->label);
            for (char *p = qlow; *p; ++p) *p = (char)tolower((unsigned char)*p);
            int qwords = 0, qmatched = 0;
            for (char *w = buf, *end; *w; w = end) {
                while (*w && !isalnum((unsigned char)*w)) ++w;
                if (!*w) break;
                end = w;
                while (*end && isalnum((unsigned char)*end)) ++end;
                size_t len = (size_t)(end - w);
                if (len < 3) continue;
                char word[64];
                snprintf(word, sizeof word, "%.*s", (int)len, w);
                if (++qwords <= 10 && ws_strcasestr(qlow, word)) ++qmatched;
            }
            rj += (qwords ? qmatched * 30 / qwords : 0);
            if (g_search_year > 0) {
                char ys[8];
                snprintf(ys, sizeof ys, "%d", g_search_year);
                if (strstr(qlow, ys)) rj += 10;
            }
            if (rj >= ri) break;         /* stop at the first row already as good: descending */
            g_results[j + 1] = g_results[j];
            --j;
            if (j < 0) break;
        }
        g_results[j + 1] = tmp;
    }
    const int count = g_result_count;
    const uint64_t elapsed = now_ms() - g_started_ms;
    if (count > 0) {
        g_status = 2;
        snprintf(g_stage, sizeof g_stage, "%d FOUND", count);
    } else if (hit_deadline || pn < 0 || os < 0 || sd < 0) {
        g_status = 3;
        snprintf(g_stage, sizeof g_stage, "SITES DID NOT ANSWER - CHECK THE INTERNET");
    } else {
        g_status = 3;
        snprintf(g_stage, sizeof g_stage, "NOTHING FOR THIS NAME");
    }
    g_search_running = 0;
    pthread_mutex_unlock(&g_mu);
    PROV_LOG("websub: search done in %" PRIu64 " ms: podnapisi=%d opensubtitles=%d subdl=%d -> %d results",
             elapsed, pn, os, sd, count);
    return NULL;
}

int evo_websub_begin_search(const char *media_path)
{
    if (!media_path) return -1;
    pthread_mutex_lock(&g_mu);
    /* a search that just finished still releases its worker briefly - give
     * it up to 3 s before refusing, so a second SEARCH ONLINE right after
     * the first is not turned away */
    for (int wait = 0; g_search_running && wait < 20; ++wait) {
        pthread_mutex_unlock(&g_mu);
        usleep(150 * 1000);
        pthread_mutex_lock(&g_mu);
    }
    if (g_search_running) {
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    g_search_running = 1;
    g_status = 1;
    g_result_count = 0;
    snprintf(g_stage, sizeof g_stage, "STARTING...");
    g_started_ms = now_ms();
    /* keep the real path: OpenSubtitles hashes it; Podnapisi/SubDL search
     * the cleaned title in g_search_title */
    snprintf(g_search_file, sizeof g_search_file, "%s", media_path);
    snprintf(g_owner, sizeof g_owner, "%s", media_path);
    g_search_query[0] = 0;
    {
        char title[128];
        int s, e, y;
        parse_media_name(media_path, title, (int)sizeof title, &s, &e, &y);
        g_search_season = s;
        g_search_episode = e;
        g_search_year = y;
        snprintf(g_search_title, sizeof g_search_title, "%s", title);
        PROV_LOG("websub: begin search: title='%s' s%d e%d y%d",
                 g_search_title, s, e, y);
    }
    pthread_mutex_unlock(&g_mu);

    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    int rc = pthread_create(&t, &attr, search_worker, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        pthread_mutex_lock(&g_mu);
        g_search_running = 0;
        g_status = 0;
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    pthread_detach(t);
    return 0;
}

int evo_websub_search_query(const char *query)
{
    if (!query || !query[0]) return -1;
    pthread_mutex_lock(&g_mu);
    for (int wait = 0; g_search_running && wait < 20; ++wait) {
        pthread_mutex_unlock(&g_mu);
        usleep(150 * 1000);
        pthread_mutex_lock(&g_mu);
    }
    if (g_search_running) {
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    g_search_running = 1;
    g_status = 1;
    g_result_count = 0;
    snprintf(g_stage, sizeof g_stage, "STARTING...");
    g_started_ms = now_ms();
    snprintf(g_search_query, sizeof g_search_query, "%s", query);
    /* a typed query replaces the parsed title for keyword sites */
    snprintf(g_search_title, sizeof g_search_title, "%s", query);
    g_search_season = g_search_episode = g_search_year = -1;
    pthread_mutex_unlock(&g_mu);
    PROV_LOG("websub: begin query search: '%s'", query);

    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    int rc = pthread_create(&t, &attr, search_worker, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        pthread_mutex_lock(&g_mu);
        g_search_running = 0;
        g_status = 0;
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    pthread_detach(t);
    return 0;
}

int evo_websub_status(void)
{
    pthread_mutex_lock(&g_mu);
    int s = g_status;
    pthread_mutex_unlock(&g_mu);
    return s;
}

/* Live one-liner for the picker row: which site is being asked, how long,
 * and the outcome ("12 FOUND", "NOTHING FOR THIS NAME", "SITES DID NOT
 * ANSWER - CHECK THE INTERNET"). */
const char *evo_websub_stage(void)
{
    pthread_mutex_lock(&g_mu);
    static char stage[80];
    if (g_status == 1)
        snprintf(stage, sizeof stage, "%s · %" PRIu64 "s",
                 g_stage, (now_ms() - g_started_ms) / 1000);
    else
        snprintf(stage, sizeof stage, "%s", g_stage);
    pthread_mutex_unlock(&g_mu);
    return stage;
}

const char *evo_websub_owner(void)
{
    pthread_mutex_lock(&g_mu);
    static char owner[768];
    snprintf(owner, sizeof owner, "%s", g_owner);
    pthread_mutex_unlock(&g_mu);
    return owner;
}

int evo_websub_results_count(void)
{
    pthread_mutex_lock(&g_mu);
    int n = g_result_count;
    pthread_mutex_unlock(&g_mu);
    return n;
}

int evo_websub_results_get(int i, evo_websub_result_t *out)
{
    if (!out) return 0;
    pthread_mutex_lock(&g_mu);
    int ok = (i >= 0 && i < g_result_count);
    if (ok) *out = g_results[i];
    pthread_mutex_unlock(&g_mu);
    return ok;
}

/* ------------------------------------------------------------------ */
/* download worker: zip -> .srt -> the loader path                    */

static int fetch_to(const char *url, char **body, size_t *len)
{
    if (strncmp(url, "os-file://", 10) == 0)
        return -2;                              /* handled by fetch_os */
    const char *hdrs[1] = { "Accept: */*" };
    int status = 0;
    return evo_net_http_get_sync(url, hdrs, 1, body, len, &status);
}

static int fetch_os(const char *file_id, char **body, size_t *len)
{
    char key[128] = {0};
    key_load("opensubtitles.json", "api_key", key, sizeof key);
    if (!key[0]) return -3;

    char keyhdr[256];
    snprintf(keyhdr, sizeof keyhdr, "Api-Key: %s", key);
    const char *UA = "User-Agent: EVOPlayer v0.13.7";

    /* /api/v1/download needs the account's user token; log in on first use
     * and again when the ~24 h token has expired. */
    char user[128], pass[128];
    evo_websub_get_os_user(user, (int)sizeof user, pass, (int)sizeof pass);

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!g_os_token[0]) {
            if (!user[0] || !pass[0]) {
                pthread_mutex_lock(&g_mu);
                snprintf(g_stage, sizeof g_stage,
                         "ADD YOUR OPENSUBTITLES ACCOUNT IN SETTINGS");
                pthread_mutex_unlock(&g_mu);
                return -7;
            }
            cJSON *post = cJSON_CreateObject();
            cJSON_AddStringToObject(post, "username", user);
            cJSON_AddStringToObject(post, "password", pass);
            char *pd = cJSON_PrintUnformatted(post);
            cJSON_Delete(post);
            if (!pd) return -8;
            const char *lhdrs[2] = { keyhdr, UA };
            char *resp = NULL;
            size_t rlen = 0;
            int status = 0;
            int rc = evo_net_http_post_sync(
                "https://api.opensubtitles.com/api/v1/login",
                pd, lhdrs, 2, &resp, &rlen, &status);
            free(pd);
            if (rc != 0 || !resp) {
                PROV_WARN("websub: opensubtitles login failed rc=%d", rc);
                pthread_mutex_lock(&g_mu);
                snprintf(g_stage, sizeof g_stage,
                         "OPENSUBTITLES LOGIN FAILED - CHECK USER AND PASSWORD");
                pthread_mutex_unlock(&g_mu);
                return -9;
            }
            cJSON *j = cJSON_Parse(resp);
            free(resp);
            const cJSON *t = j ? cJSON_GetObjectItemCaseSensitive(j, "token") : NULL;
            if (cJSON_IsString(t) && t->valuestring)
                snprintf(g_os_token, sizeof g_os_token, "%s", t->valuestring);
            if (j) cJSON_Delete(j);
            if (!g_os_token[0]) {
                pthread_mutex_lock(&g_mu);
                snprintf(g_stage, sizeof g_stage,
                         "OPENSUBTITLES LOGIN REFUSED - CHECK USER AND PASSWORD");
                pthread_mutex_unlock(&g_mu);
                return -10;
            }
            PROV_LOG("websub: opensubtitles login ok");
        }

        char authhdr[600];
        snprintf(authhdr, sizeof authhdr, "Authorization: Bearer %s", g_os_token);
        const char *hdrs[3] = { keyhdr, UA, authhdr };
        char post[128];
        snprintf(post, sizeof post, "{\"file_id\": %s}", file_id);

        char *resp = NULL;
        size_t rlen = 0;
        int status = 0;
        int rc = evo_net_http_post_sync(
            "https://api.opensubtitles.com/api/v1/download",
            post, hdrs, 3, &resp, &rlen, &status);
        if (rc != 0 || !resp) {
            if (status == 401) {
                g_os_token[0] = 0;      /* token expired: log in and retry */
                continue;
            }
            PROV_WARN("websub: opensubtitles download request failed rc=%d http=%d", rc, status);
            pthread_mutex_lock(&g_mu);
            snprintf(g_stage, sizeof g_stage,
                     status == 406 ? "OPENSUBTITLES DAILY DOWNLOAD LIMIT REACHED"
                                   : "OPENSUBTITLES DOWNLOAD FAILED");
            pthread_mutex_unlock(&g_mu);
            return -11;
        }
        char *link = NULL;
        cJSON *j = cJSON_Parse(resp);
        free(resp);
        if (!j) return -12;
        const cJSON *l = cJSON_GetObjectItemCaseSensitive(j, "link");
        if (cJSON_IsString(l) && l->valuestring)
            link = strdup(l->valuestring);
        cJSON_Delete(j);
        if (!link) return -13;

        const char *ghdrs[3] = { keyhdr, UA, authhdr };
        rc = evo_net_http_get_sync(link, ghdrs, 3, body, len, &status);
        free(link);
        return rc;
    }
    return -14;
}

static void *fetch_worker(void *arg)
{
    long idx = (long)arg;
    char url[EVO_WEBSUB_URL] = {0};
    char label[96] = {0};
    char lang[16] = {0};
    char site[16] = {0};
    pthread_mutex_lock(&g_mu);
    if (idx >= 0 && idx < g_result_count) {
        snprintf(url, sizeof url, "%s", g_results[idx].url);
        snprintf(label, sizeof label, "%s%s%s",
                 g_results[idx].site,
                 g_results[idx].lang_tag[0] ? " " : "",
                 g_results[idx].lang_tag);
        snprintf(lang, sizeof lang, "%s", g_results[idx].lang_tag);
        snprintf(site, sizeof site, "%s", g_results[idx].site);
    }
    pthread_mutex_unlock(&g_mu);

    /* Already on disk per the download db? Serve the saved file, no
     * network - and no duplicate copy with an incremented name. */
    char dbkey[700];
    snprintf(dbkey, sizeof dbkey, "%s|%s", site, url);
    char cached[768];
    pthread_mutex_lock(&g_mu);
    g_fetch_cached = 0;
    pthread_mutex_unlock(&g_mu);
    if (websub_db_get(dbkey, cached, (int)sizeof cached) && websub_file_exists(cached)) {
        pthread_mutex_lock(&g_mu);
        snprintf(g_fetch_path, sizeof g_fetch_path, "%s", cached);
        snprintf(g_fetch_label, sizeof g_fetch_label, "%s", label);
        g_fetch_cached = 1;
        g_fetch_done = 1;
        pthread_mutex_unlock(&g_mu);
        PROV_LOG("websub: %s already downloaded, serving %s", label, cached);
        return NULL;
    }
    if (!url[0]) {
        pthread_mutex_lock(&g_mu);
        g_fetch_done = -1;
        pthread_mutex_unlock(&g_mu);
        return NULL;
    }

    char *body = NULL;
    size_t blen = 0;
    int rc;
    if (strncmp(url, "os-file://", 10) == 0)
        rc = fetch_os(url + 10, &body, &blen);
    else
        rc = fetch_to(url, &body, &blen);

    unsigned char *srt = NULL;
    size_t slen = 0;
    if (rc == 0 && body)
        srt = srt_from_payload((const unsigned char *)body, blen, &slen);
    if (body) free(body);

    if (!srt || slen == 0) {
        if (srt) free(srt);
        pthread_mutex_lock(&g_mu);
        g_fetch_done = -1;
        pthread_mutex_unlock(&g_mu);
        return NULL;
    }

    /* Save next to the movie as <movie>.<lang>.srt when that location is
     * writable (exFAT/FAT32 USB, internal) - EVO's own sidecar detection
     * then reloads it on every future play of that movie. Read-only mounts
     * (NTFS, some shares) fall back to the data dir. */
    extern char current_media_path[768];
    char path[768] = {0};
    char candidate[768] = {0};
    if (current_media_path[0] == '/') {
        const char *slash = strrchr(current_media_path, '/');
        if (slash && slash[1]) {
            char base[256] = {0};
            snprintf(base, sizeof base, "%s", slash + 1);
            char *dot = strrchr(base, '.');
            if (dot && dot != base) *dot = 0;
            char tag[32] = {0};
            const char *p = lang;
            char *q = tag;
            for (; *p && q < tag + sizeof tag - 1; ++p)
                if (isalnum((unsigned char)*p)) *q++ = (char)tolower((unsigned char)*p);
            if (!tag[0]) snprintf(tag, sizeof tag, "srt");
            snprintf(candidate, sizeof candidate, "%.*s%s.%s.srt",
                     (int)(slash - current_media_path + 1), current_media_path, base, tag);
            /* never overwrite: increment <movie>.<lang>.2.srt, .3.srt ... */
            char probe[768];
            snprintf(probe, sizeof probe, "%s", candidate);
            for (int n = 2; websub_file_exists(probe) && n < 100; ++n)
                snprintf(probe, sizeof probe, "%.*s.%d.srt",
                         (int)strlen(candidate) - 4, candidate, n);
            snprintf(candidate, sizeof candidate, "%s", probe);
            FILE *t = fopen(candidate, "wb");
            if (t) {
                fclose(t);
                snprintf(path, sizeof path, "%s", candidate);
            } else {
                PROV_WARN("websub: movie folder not writable, saving to the data dir");
            }
        }
    }
    if (!path[0]) {
        /* evo_data_path's buffer rotates - copy before any second call */
        snprintf(path, sizeof path, "%s", evo_data_path("websub_pick.srt"));
    }

    FILE *f = fopen(path, "wb");
    int ok = f != NULL;
    if (f) {
        ok = fwrite(srt, 1, slen, f) == slen;
        fclose(f);
    }
    free(srt);
    if (ok)
        websub_db_put(dbkey, path);

    pthread_mutex_lock(&g_mu);
    if (ok) {
        snprintf(g_fetch_path, sizeof g_fetch_path, "%s", path);
        snprintf(g_fetch_label, sizeof g_fetch_label, "%s", label);
        g_fetch_done = 1;
    } else {
        g_fetch_done = -1;
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

int evo_websub_fetch_async(int i)
{
    pthread_mutex_lock(&g_mu);
    if (g_fetch_busy) {
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    if (i < 0 || i >= g_result_count) {
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    g_fetch_busy = 1;
    g_fetch_done = 0;
    pthread_mutex_unlock(&g_mu);

    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    int rc = pthread_create(&t, &attr, fetch_worker, (void *)(long)i);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        pthread_mutex_lock(&g_mu);
        g_fetch_busy = 0;
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    pthread_detach(t);
    return 0;
}

int evo_websub_take_ready(char *out_path, int cap, char *out_label, int lcap)
{
    pthread_mutex_lock(&g_mu);
    int v = g_fetch_done;
    if (v == 1 && out_path && cap > 0) {
        snprintf(out_path, (size_t)cap, "%s", g_fetch_path);
        g_fetch_path[0] = 0;
        if (out_label && lcap > 0) {
            snprintf(out_label, (size_t)lcap, "%s", g_fetch_label);
            g_fetch_label[0] = 0;
        }
    }
    if (v != 0) {
        g_fetch_done = 0;
        g_fetch_busy = 0;
    }
    pthread_mutex_unlock(&g_mu);
    return v;
}

int evo_websub_take_was_cached(void)
{
    pthread_mutex_lock(&g_mu);
    int c = g_fetch_cached;
    g_fetch_cached = 0;
    pthread_mutex_unlock(&g_mu);
    return c;
}

int evo_websub_is_downloaded(int i)
{
    char url[EVO_WEBSUB_URL] = {0}, site[16] = {0}, cached[768];
    pthread_mutex_lock(&g_mu);
    if (i >= 0 && i < g_result_count) {
        snprintf(url, sizeof url, "%s", g_results[i].url);
        snprintf(site, sizeof site, "%s", g_results[i].site);
    }
    pthread_mutex_unlock(&g_mu);
    if (!url[0]) return 0;
    char key[700];
    snprintf(key, sizeof key, "%s|%s", site, url);
    return websub_db_get(key, cached, (int)sizeof cached) && websub_file_exists(cached);
}

int evo_websub_sidecars(char paths[][768], char names[][64], int max)
{
    extern char current_media_path[768];
    if (!current_media_path[0] || current_media_path[0] != '/' || max <= 0) return 0;
    const char *slash = strrchr(current_media_path, '/');
    if (!slash || !slash[1]) return 0;

    /* the movie's own name without extension: the sidecar prefix */
    char stem[256];
    snprintf(stem, sizeof stem, "%s", slash + 1);
    char *dot = strrchr(stem, '.');
    if (dot && dot != stem) *dot = 0;
    size_t stem_len = strlen(stem);

    char dir[600];
    snprintf(dir, sizeof dir, "%.*s", (int)(slash - current_media_path + 1),
             current_media_path);

    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        const char *name = e->d_name;
        size_t nl = strlen(name);
        if (strncmp(name, stem, stem_len) != 0) continue;
        if (nl <= stem_len + 4) continue;
        const char *rest = name + stem_len;      /* ".srt" or ".en.srt" */
        size_t rl = strlen(rest);
        if (strcmp(rest, ".srt") != 0 && !(rl > 4 && strcmp(rest + rl - 4, ".srt") == 0))
            continue;
        snprintf(paths[n], 768, "%s%s", dir, name);
        /* display name: the part after the movie stem, minus .srt */
        char disp[64];
        snprintf(disp, sizeof disp, "%.*s", (int)(rl - 4), rest);
        if (!disp[0]) snprintf(disp, sizeof disp, "default");
        snprintf(names[n], 64, "%s", disp);
        ++n;
    }
    closedir(d);
    return n;
}
