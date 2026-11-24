/*
 * evo_websub - subtitles searched and downloaded from subtitle websites,
 * for files that play without a provider item (USB, SMB, internal).
 *
 * Docs: docs/planning/websubs.md. Sites (v1): OpenSubtitles.com (needs a
 * free API key, saved via Settings to /data/evoplayer/opensubtitles.json),
 * Podnapisi.net (no key), SubDL (free API key).
 *
 * Flow, mirroring evo_online_subs:
 *   evo_websub_begin_search(media_path or query)  kicks one detached worker;
 *   it parses the filename, hashes the file (OpenSubtitles moviehash) and
 *   queries every enabled site. Results land in one mutex-guarded list.
 *   evo_websub_results_get()     the picker lists them as ONLINE (WEB) rows.
 *   evo_websub_fetch_async(i)    downloads zip/gz, unpacks the first .srt,
 *                                writes it to evo_data_path("websub_pick.srt")
 *   evo_websub_take_ready()      the player pumps it into the loader, the
 *                                same way evo_osubs_take_ready does.
 *
 * Thread use: search results are written by the worker and read by the UI;
 * everything here takes one mutex. Requests never run on the UI thread.
 */
#ifndef EVO_WEBSUB_H
#define EVO_WEBSUB_H

#ifdef __cplusplus
extern "C" {
#endif

#define EVO_WEBSUB_MAX_RESULTS 40
#define EVO_WEBSUB_URL 512
#define EVO_WEBSUB_LABEL 64
#define EVO_WEBSUB_LANG 8

typedef struct evo_websub_result {
    char label[EVO_WEBSUB_LABEL];     /* release / file name shown on the row */
    char detail[96];                  /* "PODNAPISI · eng · 1234 downloads" */
    char lang_tag[16];                /* the site's language tag */
    char site[16];                    /* "PODNAPISI" / "SUBDL" / "OPENSUBS" */
    char url[EVO_WEBSUB_URL];         /* the download URL (zip/gz/srt) */
} evo_websub_result_t;

/* Search status: 0 idle, 1 searching, 2 done with results, 3 done with
 * nothing (see evo_websub_stage() for why). */
int evo_websub_status(void);

/* Live one-liner for the picker row: the site being asked right now with
 * elapsed seconds while searching, then the outcome. */
const char *evo_websub_stage(void);

/* The file the current results belong to ("" none). The picker shows rows
 * only while this matches the file being played, so changing movies never
 * shows the previous movie's search. */
const char *evo_websub_owner(void);

/* Start a search for this media file (hash + filename parse). A second call
 * while one is running returns -1; results from the running search land in
 * the same list. */
int evo_websub_begin_search(const char *media_path);

/* Manual search: same, but the keyboard text replaces the filename parse. */
int evo_websub_search_query(const char *query);

int evo_websub_results_count(void);
/* Snapshot of result i into *out; 0 when out of range. */
int evo_websub_results_get(int i, evo_websub_result_t *out);

/* Download result i off-thread. 0 started, -1 already fetching. The picked
 * subtitle lands at evo_data_path("websub_pick.srt") and
 * evo_websub_take_ready() reports it once. */
int evo_websub_fetch_async(int i);

/* 1 when a downloaded subtitle is ready (path + label copied out, flag
 * cleared), -1 when the last fetch failed, 0 while none is pending.
 * out_label may be NULL; it names the row ("PODNAPISI eng"). */
int evo_websub_take_ready(char *out_path, int cap, char *out_label, int lcap);

/* 1 when the last completed take was served from the download database
 * (the file was already on disk - no network was used). */
int evo_websub_take_was_cached(void);

/* 1 when result i is already on disk per the download database - the picker
 * marks such rows ON DISK. */
int evo_websub_is_downloaded(int i);

/* Sidecar subtitles for the movie being played: every <movie>*.srt in the
 * movie's folder. paths/names are filled (name = the part after the movie
 * stem, e.g. "en" or "en.2"); returns how many, up to max. 0 when the file
 * is not a local path. */
int evo_websub_sidecars(char paths[][768], char names[][64], int max);

/* 1 when this file was downloaded by EVO (recorded in the download db) -
 * delete mode refuses to touch anything else. */
int evo_websub_is_our_file(const char *path);

/* OpenSubtitles API key, persisted at evo_data_path("opensubtitles.json").
 * Empty string = site disabled. The download endpoint also needs the
 * account's user token: the module logs in with the username/password below
 * when it first downloads (the token lives 24 h, in memory only). */
void evo_websub_set_os_key(const char *key);
void evo_websub_get_os_key(char *out, int cap);
void evo_websub_set_os_user(const char *user, const char *password);
void evo_websub_get_os_user(char *out_user, int ucap, char *out_pass, int pcap);

/* SubDL API key, persisted at evo_data_path("subdl.json"). */
void evo_websub_set_subdl_key(const char *key);
void evo_websub_get_subdl_key(char *out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* EVO_WEBSUB_H */
