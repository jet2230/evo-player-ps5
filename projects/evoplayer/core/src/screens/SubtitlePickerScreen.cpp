#include "evo/screens/SubtitlePickerScreen.hpp"
#include "evo/Application.hpp"
#include "evo/PlayFmtLock.hpp"
#include "evo_subtitle.h"
#include "evo_langname.h"
#include "evo_subsync.h"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_toast.h"
#include "evo_online_subs.h"
#include "evo_websub.h"
#include "evo_keyboard.h"

#include <algorithm>

extern "C" {
#include <libavformat/avformat.h>
extern AVFormatContext *play_fmt;
extern char current_media_path[768];
}

#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>

namespace evo {

SubtitlePickerScreen::SubtitlePickerScreen()
    : StatefulScreen("SubtitlePickerScreen") {
}

void SubtitlePickerScreen::onEnter() {
    StatefulScreen::onEnter();
    /* docs/planning/websubs.md: the web search does NOT start on open - the
     * SEARCH ONLINE... row starts it, so the picker opens instantly and no
     * request goes out until the user asks for one. */
    refreshTracks();
    m_lastWebStatus = evo_websub_status();
    /* seed the external-state cache: an already-loaded subtitle on open is
     * not news, so no LOADED toast until something new lands */
    m_seenExtCount = prospero_subtitle_count;
    m_seenExtActive = prospero_subtitle_enabled && prospero_subtitle_use_external;
    std::snprintf(m_seenExtLabel, sizeof m_seenExtLabel, "%s", prospero_subtitle_external_label);
    m_selectedIndex = m_activeTrackIndex;
    m_scrollOffset = 0;
    if (m_selectedIndex >= kVisibleRows) {
        m_scrollOffset = m_selectedIndex - kVisibleRows + 1;
    }
    rollRandomCues();
}

void SubtitlePickerScreen::onExit() {
    StatefulScreen::onExit();
    m_deleteMode = false;
    m_r2HoldMs = 0;
    m_r2Held = false;
}

void SubtitlePickerScreen::refreshTracks() {
    m_tracks.clear();
    m_activeTrackIndex = 0;

    // 1. Subtitles OFF
    SubtitleTrackEntry offTrack;
    offTrack.trackId = -2;
    offTrack.label = "SUBTITLES OFF";
    offTrack.detail = "Disable subtitles";
    m_tracks.push_back(std::move(offTrack));

    // 2b. Multiple sidecars next to the movie (downloaded .en.srt, .sq.srt,
    // .en.2.srt...): one row each so the user picks which is active. Only
    // when there is more than one - with a single file the plain EXTERNAL
    // row below stays the simple case.
    {
        char scp[kMaxExtSidecars][768];
        char scn[kMaxExtSidecars][64];
        const int nsc = evo_websub_sidecars(scp, scn, kMaxExtSidecars);
        if (nsc > 1) {
            for (int i = 0; i < nsc; ++i) {
                SubtitleTrackEntry row;
                row.trackId = kExtSidecarBase - i;
                row.label = std::string("EXTERNAL · ") + scn[i];
                row.detail = "EXTERNAL SRT FILE NEXT TO THE MOVIE";
                row.extPath = scp[i];
                m_tracks.push_back(std::move(row));
            }
            m_extSidecarCount = nsc;
        } else {
            m_extSidecarCount = 0;
        }
    }

    // 2. External SRT if available (a sidecar, a media server's, or an online one)
    if (prospero_subtitle_count > 0 && m_extSidecarCount == 0) {
        SubtitleTrackEntry extTrack;
        extTrack.trackId = -1;
        extTrack.label = prospero_subtitle_external_label[0] ? prospero_subtitle_external_label
                                                             : "EXTERNAL SRT";
        char cueBuf[48];
        std::snprintf(cueBuf, sizeof(cueBuf), "%s%d CUES",
                      prospero_subtitle_external_label[0] ? "ONLINE · " : "", prospero_subtitle_count);
        extTrack.detail = cueBuf;
        if (prospero_subtitle_enabled && prospero_subtitle_use_external) {
            m_activeTrackIndex = static_cast<int>(m_tracks.size());
        }
        extTrack.isSecondary = prospero_secondary_use_external && prospero_secondary_subtitle_active();
        m_tracks.push_back(std::move(extTrack));
    }

    // 3. Embedded subtitle streams
    /*
     * FIX.md: the context is closed on the playback worker threads (provider
     * auto-next, web/cast handoff). Hold the play_fmt lock across the walk so
     * it cannot be freed under us; the walk is memory-only, so the critical
     * section is microseconds and the worker's close never blocks behind it
     * for long. The pointer goes null mid-teardown - no tracks, no crash.
     */
    if (AVFormatContext* fmt = evo_play_fmt_acquire()) {
        for (unsigned int i = 0; i < fmt->nb_streams; ++i) {
            AVStream* stream = fmt->streams[i];
            if (!stream || !stream->codecpar || stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
                continue;
            }

            SubtitleTrackEntry streamTrack;
            streamTrack.trackId = static_cast<int>(i);

            const AVDictionaryEntry* lang = av_dict_get(stream->metadata, "language", nullptr, 0);
            const AVDictionaryEntry* title = av_dict_get(stream->metadata, "title", nullptr, 0);

            /* The language's own name ("Русский", "日本語") when it is known. */
            const char* native = (lang && lang->value) ? evo_language_native_name(lang->value) : nullptr;
            std::string label = native ? native : (lang && lang->value) ? lang->value : "Track";
            if (title && title->value) {
                label += " - ";
                label += title->value;
            }

            streamTrack.label = label;
            /* What the track is, so a forced or picture track is not a surprise */
            std::string detail;
            const AVCodecID cid = stream->codecpar->codec_id;
            if (cid == AV_CODEC_ID_HDMV_PGS_SUBTITLE)      detail = "PGS (PICTURE)";
            else if (cid == AV_CODEC_ID_SUBRIP)            detail = "SRT";
            else if (cid == AV_CODEC_ID_ASS || cid == AV_CODEC_ID_SSA) detail = "ASS";
            else if (cid == AV_CODEC_ID_WEBVTT)            detail = "WEBVTT";
            else if (cid == AV_CODEC_ID_MOV_TEXT)          detail = "MP4 TEXT";
            else {
                const char* cn = avcodec_get_name(cid);
                detail = cn ? cn : "EMBEDDED";
                for (auto& ch : detail) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            }
            if (!prospero_embedded_subtitle_supported(cid) && !prospero_embedded_subtitle_is_bitmap(cid))
                detail += " · NOT SUPPORTED";
            if (prospero_subtitle_stream_is_forced(stream))
                detail += " · FORCED (SIGNS ONLY)";
            const int cues = prospero_subtitle_declared_cues(stream);
            if (cues >= 0) {
                char cb[32];
                std::snprintf(cb, sizeof cb, " · %d CUES", cues);
                detail += cb;
            }
            streamTrack.detail = detail;
            streamTrack.isSecondary =
                prospero_secondary_subtitle_stream_index == static_cast<int>(i);

            if (prospero_subtitle_enabled && !prospero_subtitle_use_external &&
                prospero_embedded_subtitle_stream_index == static_cast<int>(i)) {
                m_activeTrackIndex = static_cast<int>(m_tracks.size());
            }

            m_tracks.push_back(std::move(streamTrack));
        }
        evo_play_fmt_release();
    }

    // 4. Online subtitles from the Stremio addons for this title (not yet loaded)
    bool onlineForThis = false;
    if (auto pb = Application::getInstance().getPlaybackController()) {
        char key[512];
        evo_osubs_key(key, sizeof key);
        const PlaybackSource& src = pb->getSource();
        onlineForThis = src.isProvider() && !src.item_id.empty() && src.item_id == key;
    }
    if (onlineForThis) {
        const int n = evo_osubs_count();
        const int active = (prospero_subtitle_count > 0 && prospero_subtitle_external_label[0])
                               ? evo_osubs_active() : -1;
        for (int i = 0; i < n && i < kMaxOnlineRows; ++i) {
            if (i == active) continue;          /* it is the external row above */
            evo_osub_t s;
            if (!evo_osubs_get(i, &s)) continue;
            SubtitleTrackEntry row;
            row.trackId = kOnlineBase - i;
            char lang[64];
            evo_osubs_label(i, lang, sizeof lang);
            row.label = lang;
            row.detail = std::string("ONLINE · ") + s.source;
            m_tracks.push_back(std::move(row));
        }
    }

    // 4b. Subtitle websites (docs/planning/websubs.md): idle until the user
    // presses SEARCH ONLINE...; then a live stage row (which site is being
    // asked, seconds elapsed), then the results - or a plain "nothing" row.
    // Results are tagged with the file they were searched for: another
    // movie's rows must never show here.
    {
        const char *owner = evo_websub_owner();
        const bool mine = owner[0] && current_media_path[0] &&
                          strcmp(owner, current_media_path) == 0;
        const int wstatus = mine ? evo_websub_status() : 0;
        if (wstatus == 1) {
            SubtitleTrackEntry row;
            row.trackId = kWebSearchTrack;
            row.label = "SEARCHING WEB SITES...";
            row.detail = evo_websub_stage();
            m_tracks.push_back(std::move(row));
        } else if (wstatus == 3) {
            SubtitleTrackEntry row;
            row.trackId = kWebSearchTrack;
            row.label = "NO SUBS FOUND";
            row.detail = evo_websub_stage();
            m_tracks.push_back(std::move(row));
            SubtitleTrackEntry searchRow;
            searchRow.trackId = kWebSearchTrack;
            searchRow.label = "TRY SEARCHING AGAIN...";
            searchRow.detail = "ASK THE WEB SITES ONCE MORE";
            m_tracks.push_back(std::move(searchRow));
        } else {
            const int n = evo_websub_results_count();
            for (int i = 0; i < n && i < kMaxWebRows; ++i) {
                evo_websub_result_t r;
                if (!evo_websub_results_get(i, &r)) continue;
                SubtitleTrackEntry row;
                row.trackId = kWebSubBase - i;
                row.label = r.label;
                row.detail = std::string("WEB · ") + r.site +
                             (r.lang_tag[0] ? std::string(" · ") + r.lang_tag : std::string());
                if (evo_websub_is_downloaded(i))
                    row.detail += " · ON DISK";
                m_tracks.push_back(std::move(row));
            }
            SubtitleTrackEntry searchRow;
            searchRow.trackId = kWebSearchTrack;
            searchRow.label = "SEARCH ONLINE...";
            searchRow.detail = n > 0 ? "SEARCH THE WEB SITES AGAIN"
                                     : "SEARCH PODNAPISI, OPENSUBTITLES AND SUBDL";
            m_tracks.push_back(std::move(searchRow));
        }
    }

    // AUTO-SYNC (#102): matches the active track - external SRT or an
    // embedded text track - to the audio. Second row, so a long track list
    // never scrolls it away. Its detail is live (progress); render() fills it.
    if (m_tracks.size() > 1) {
        SubtitleTrackEntry syncRow;
        syncRow.trackId = kAutoSyncTrack;
        syncRow.label = "AUTO-SYNC";
        m_tracks.insert(m_tracks.begin() + 1, std::move(syncRow));
        if (m_activeTrackIndex >= 1) {
            ++m_activeTrackIndex;
        }
    }

    // LOAD FROM FILE...: an .srt from anywhere on the console. Last, so the
    // tracks this file already has come first.
    {
        SubtitleTrackEntry loadRow;
        loadRow.trackId = kLoadFileTrack;
        loadRow.label = "LOAD FROM FILE...";
        loadRow.detail = "Pick an .srt from a drive or folder";
        m_tracks.push_back(std::move(loadRow));
    }

    if (!prospero_subtitle_enabled) {
        m_activeTrackIndex = 0;
    }
}

void SubtitlePickerScreen::navigate(int delta) {
    if (m_tracks.empty()) return;
    int count = static_cast<int>(m_tracks.size());
    int next = m_selectedIndex + delta;
    if (next < 0) {
        m_selectedIndex = 0;
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    } else if (next >= count) {
        m_selectedIndex = count - 1;
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    }
    m_selectedIndex = next;

    if (m_selectedIndex >= m_scrollOffset + kVisibleRows) {
        m_scrollOffset = m_selectedIndex - kVisibleRows + 1;
    } else if (m_selectedIndex < m_scrollOffset) {
        m_scrollOffset = m_selectedIndex;
    }

    evo_feedback(EVO_FB_MOVE);
}

void SubtitlePickerScreen::activateSelection() {
    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_tracks.size())) {
        return;
    }

    int trackId = m_tracks[m_selectedIndex].trackId;

    if (trackId <= kExtSidecarBase && trackId > kExtSidecarBase - kMaxExtSidecars) {
        /* pick this sidecar as the active external subtitle */
        const std::string &path = m_tracks[m_selectedIndex].extPath;
        evo_feedback(EVO_FB_CONFIRM);
        if (!prospero_subtitle_load_file_quiet(path.c_str()) ||
            !prospero_subtitle_select_primary(-1)) {
            evo_feedback(EVO_FB_ERROR);
            toast("SUBTITLES", "COULD NOT LOAD THAT FILE");
            return;
        }
        std::snprintf(prospero_subtitle_external_label,
                      sizeof prospero_subtitle_external_label, "%s",
                      m_tracks[m_selectedIndex].label.c_str());
        prospero_subtitle_use_external = 1;
        prospero_subtitle_enabled = 1;
        toast("SUBTITLES", "EXTERNAL SRT SET");
        refreshTracks();
        return;
    }

    if (trackId == kWebSearchTrack) {
        evo_feedback(EVO_FB_OPEN);
        /* A local file: search the web sites for it right now (the hash and
         * the parsed name come from the file). No local path (a provider
         * stream): fall back to typing a name. */
        if (current_media_path[0]) {
            if (evo_websub_begin_search(current_media_path) != 0) {
                toast("SUBTITLES", "A SEARCH IS ALREADY RUNNING");
                return;
            }
            m_lastWebStatus = 1;
            refreshTracks();
            toast("SUBTITLES", "SEARCHING...");
        } else {
            evo_keyboard_open("SEARCH SUBTITLES ONLINE", "", 96,
                              &SubtitlePickerScreen::OnWebSearchSubmitted, this);
        }
        return;
    }

    if (trackId <= kWebSubBase) {
        /* docs/planning/websubs.md: the download runs off-thread and the
         * player loads it via evo_websub_take_ready when it lands, so the
         * picker never blocks. */
        evo_feedback(EVO_FB_CONFIRM);
        const int idx = kWebSubBase - trackId;
        if (evo_websub_fetch_async(idx) != 0) {
            evo_feedback(EVO_FB_ERROR);
            toast("SUBTITLES", "A DOWNLOAD IS ALREADY RUNNING");
            return;
        }
        toast("SUBTITLES", "DOWNLOADING...");
        return;
    }

    if (trackId == kLoadFileTrack) {
        evo_feedback(EVO_FB_OPEN);
        if (auto sm = Application::getInstance().getScreenManager()) sm->navigateTo(ScreenId::SubtitleFile);
        return;
    }

    if (trackId == kAutoSyncTrack) {
        // Start, or cancel a run. Stay on the picker so the progress shows;
        // the result is applied by prospero_subtitle_autosync_pump().
        if (!prospero_subtitle_autosync_running() && !prospero_subtitle_autosync_available()) {
            evo_feedback(EVO_FB_ERROR);
            return;
        }
        evo_feedback(EVO_FB_CONFIRM);
        prospero_subtitle_autosync_toggle();
        return;
    }

    evo_feedback(EVO_FB_CONFIRM);
    if (m_selectedIndex != m_activeTrackIndex) {
        // A run measures the active track; switching away ends it.
        evo_subsync_cancel();
    }

    if (trackId <= kOnlineBase) {
        // An online subtitle: fetched off the UI thread, loaded by the player
        // when it lands (PlayerScreen::update -> pumpOnlineSubtitles).
        if (evo_osubs_fetch_async(kOnlineBase - trackId) != 0) {
            evo_feedback(EVO_FB_ERROR);
            toast("SUBTITLES", "A DOWNLOAD IS ALREADY RUNNING");
            return;
        }
        toast("SUBTITLES", "DOWNLOADING...");
    } else if (trackId == -2) {
        // Off
        prospero_subtitle_enabled = 0;
        toast("SUBTITLES", "OFF");
    } else {
        /* FIX.md: the select path reads play_fmt; hold the lock so a worker
         * teardown cannot free it mid-select. Null (teardown in flight)
         * simply skips the switch. */
        if (AVFormatContext* play_fmt = evo_play_fmt_acquire()) {
            const bool ok = prospero_subtitle_select_primary(trackId) != 0;
            evo_play_fmt_release();
            if (!ok) {
                evo_feedback(EVO_FB_ERROR);
                return;
            }
            if (trackId == -1) {
                toast("SUBTITLES", "EXTERNAL SRT");
            }
        }
    }
    // An embedded track toasts its own language and codec as it opens.

    if (auto sm = Application::getInstance().getScreenManager()) {
        if (!sm->navigateBack()) {
            sm->navigateTo(ScreenId::Player);
        }
    }
}

// #110: Square makes the focused track the secondary (the second line of
// dialogue), or clears it when the track already is. Unlike Cross this stays
// on the picker, so the [SECONDARY] tag is seen moving.
void SubtitlePickerScreen::activateSecondary() {
    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_tracks.size())) {
        return;
    }

    const SubtitleTrackEntry& entry = m_tracks[m_selectedIndex];

    if (entry.trackId == kAutoSyncTrack || entry.trackId == kLoadFileTrack ||
        entry.trackId <= kOnlineBase) {
        evo_feedback(EVO_FB_ERROR);   // an online row must be loaded first
        return;
    }

    if (entry.trackId == -2 || entry.isSecondary) {
        // The OFF row, or the secondary itself: clear the second line.
        if (!prospero_secondary_subtitle_active()) {
            evo_feedback(EVO_FB_ERROR);
            return;
        }
        prospero_secondary_subtitle_select(PROSPERO_SECONDARY_NONE);
    } else {
        /* FIX.md: the select path reads play_fmt; hold the lock across it. */
        if (AVFormatContext* play_fmt = evo_play_fmt_acquire()) {
            const bool ok = prospero_secondary_subtitle_select(entry.trackId) != 0;
            evo_play_fmt_release();
            if (!ok) {
                evo_feedback(EVO_FB_ERROR);     // it toasted why
                return;
            }
        }
    }

    evo_feedback(EVO_FB_CONFIRM);
    refreshTracks();
}

void SubtitlePickerScreen::cycleSize() {
    /*
     * prospero_subtitle_face is 1=SMALL, 2=MEDIUM, 3=LARGE, 4=EXTRA LARGE
     * See its declaration in evo_subtitle.c.
     */
    if (prospero_subtitle_face < 1 || prospero_subtitle_face > PROSPERO_SUBTITLE_SIZES)
        prospero_subtitle_face = 2;              /* MEDIUM, the documented default */
    prospero_subtitle_face = (prospero_subtitle_face % PROSPERO_SUBTITLE_SIZES) + 1;

    const char* names[] = { "SMALL", "MEDIUM", "LARGE", "EXTRA LARGE" };
    toast("PRIMARY SIZE", names[prospero_subtitle_face - 1]);
    evo_feedback(EVO_FB_MOVE);
}

void SubtitlePickerScreen::cycleSecondarySize() {
    if (prospero_secondary_face < 1 || prospero_secondary_face > PROSPERO_SUBTITLE_SIZES)
        prospero_secondary_face = 2;
    prospero_secondary_face = (prospero_secondary_face % PROSPERO_SUBTITLE_SIZES) + 1;

    const char* names[] = { "SMALL", "MEDIUM", "LARGE", "EXTRA LARGE" };
    toast("2ND SUBTITLE SIZE", names[prospero_secondary_face - 1]);
    evo_feedback(EVO_FB_MOVE);
}

/* The keyboard's submit runs on the main thread (evo_keyboard.h). Re-run the
 * search with the typed text and stay on the picker to watch it land. */
void SubtitlePickerScreen::OnWebSearchSubmitted(const char *text, void *userdata) {
    auto *self = static_cast<SubtitlePickerScreen *>(userdata);
    if (!self || !text || !text[0]) return;
    if (evo_websub_search_query(text) != 0) {
        toast("SUBTITLES", "A SEARCH IS ALREADY RUNNING");
        return;
    }
    self->refreshTracks();
    self->m_selectedIndex = 0;
    self->m_scrollOffset = 0;
    toast("SUBTITLES", "SEARCHING...");
}

void SubtitlePickerScreen::rollRandomCues() {
    char pBuf[PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE] = {0};
    char sBuf[PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE] = {0};
    /* FIX.md: the preview must never re-open the file on the UI thread (a
     * large local MKV stalled the picker for seconds; a PGS track never fills
     * the text ring, so every open paid it). The preview variant samples only
     * cues already decoded or pooled. */
    prospero_subtitle_preview_random_cues(pBuf, sizeof(pBuf), sBuf, sizeof(sBuf));
    m_previewPrimary = pBuf;
    m_previewSecondary = sBuf;
}

bool SubtitlePickerScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)released;

    /* DELETE MODE (hold R2 ~3 s): Up/Down still pick the row, Cross deletes
     * the loaded subtitle, Circle (or a new R2 tap) leaves the mode. */
    if (m_deleteMode) {
        if (pressed & PadButtons::Up) {
            navigate(-1);
            return true;
        }
        if (pressed & PadButtons::Down) {
            navigate(1);
            return true;
        }
        if (pressed & PadButtons::Cross) {
            deleteFocusedSubtitle();
            return true;
        }
        if (pressed & PadButtons::Circle) {
            m_deleteMode = false;
            evo_feedback(EVO_FB_CANCEL);
            toast("SUBTITLES", "DELETE MODE OFF");
            return true;
        }
        if (pressed & PadButtons::R2) {
            m_deleteMode = false;
            evo_feedback(EVO_FB_CANCEL);
            toast("SUBTITLES", "DELETE MODE OFF");
            return true;
        }
        return true;
    }

    /* hold detection runs on the held mask; update() accumulates time */
    m_r2Held = (held & PadButtons::R2) != 0;
    if (pressed & PadButtons::R2)
        m_r2HoldMs = 0;

    if (pressed & PadButtons::Up) {
        navigate(-1);
        return true;
    }
    if (pressed & PadButtons::Down) {
        navigate(1);
        return true;
    }
    if (pressed & PadButtons::Cross) {
        activateSelection();
        return true;
    }
    if (pressed & PadButtons::Triangle) {
        if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_tracks.size()) &&
            m_tracks[m_selectedIndex].isSecondary) {
            cycleSecondarySize();
        } else {
            cycleSize();
        }
        return true;
    }
    if (pressed & PadButtons::R2) {
        cycleSecondarySize();
        return true;
    }
    if (pressed & PadButtons::Square) {
        activateSecondary();
        rollRandomCues();
        return true;
    }
    if (pressed & (PadButtons::L1 | PadButtons::R1)) {
        // The secondary track keeps its own sync (#110).
        if (!prospero_secondary_subtitle_active()) {
            evo_feedback(EVO_FB_ERROR);
            return true;
        }
        prospero_secondary_nudge_delay((pressed & PadButtons::R1) ? +100 : -100);
        evo_feedback(EVO_FB_MOVE);
        return true;
    }
    if (pressed & (PadButtons::Left | PadButtons::TouchPadLeft)) {
        prospero_subtitle_nudge_delay(-100);
        evo_feedback(EVO_FB_MOVE);
        return true;
    }
    if (pressed & (PadButtons::Right | PadButtons::TouchPadRight)) {
        prospero_subtitle_nudge_delay(+100);
        evo_feedback(EVO_FB_MOVE);
        return true;
    }
    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto sm = Application::getInstance().getScreenManager()) {
            if (!sm->navigateBack()) {
                sm->navigateTo(ScreenId::Player);
            }
        }
        return true;
    }

    return false;
}

/* DELETE MODE list: every subtitle EVO downloaded for this movie (sidecars
 * next to the file, plus a data-dir pick when the movie folder was
 * read-only). The user's own sidecars from elsewhere are never listed. */
void SubtitlePickerScreen::rebuildDeleteList() {
    m_delList.clear();
    char scp[kMaxExtSidecars][768];
    char scn[kMaxExtSidecars][64];
    const int nsc = evo_websub_sidecars(scp, scn, kMaxExtSidecars);
    for (int i = 0; i < nsc; ++i) {
        if (!evo_websub_is_our_file(scp[i])) continue;
        DeleteRow r;
        r.path = scp[i];
        r.name = scn[i];
        m_delList.push_back(std::move(r));
    }
    /* a data-dir pick (movie folder was read-only) is not a sidecar */
    const char *loaded = prospero_subtitle_external_path();
    if (loaded[0]) {
        const char *root = "/data/evoplayer/";
        if (strncmp(loaded, root, strlen(root)) == 0 &&
            std::none_of(m_delList.begin(), m_delList.end(),
                         [&](const DeleteRow &r) { return r.path == loaded; })) {
            DeleteRow r;
            r.path = loaded;
            r.name = "downloaded";
            m_delList.push_back(std::move(r));
        }
    }
}

/* DELETE MODE: Cross unloads/deletes the focused downloaded subtitle. */
void SubtitlePickerScreen::deleteFocusedSubtitle() {
    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_delList.size())) {
        evo_feedback(EVO_FB_ERROR);
        return;
    }
    const std::string path = m_delList[m_selectedIndex].path;
    const bool loaded = path == prospero_subtitle_external_path();
    if (loaded) {
        if (!prospero_subtitle_external_delete()) {
            evo_feedback(EVO_FB_ERROR);
            toast("SUBTITLES", "NOTHING LOADED TO DELETE");
            return;
        }
    } else if (remove(path.c_str()) != 0) {
        evo_feedback(EVO_FB_ERROR);
        toast("SUBTITLES", "COULD NOT DELETE THE FILE");
        return;
    }
    evo_feedback(EVO_FB_CONFIRM);
    toast("SUBTITLES", "SUBTITLES DELETED");
    rebuildDeleteList();
    if (m_delList.empty()) {
        m_deleteMode = false;
        toast("SUBTITLES", "DELETE MODE OFF - NOTHING MORE TO DELETE");
    }
    if (m_selectedIndex >= static_cast<int>(m_delList.size()))
        m_selectedIndex = static_cast<int>(m_delList.size()) - 1;
    if (m_selectedIndex < 0) m_selectedIndex = 0;
    refreshTracks();
}

void SubtitlePickerScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);

    /* DELETE MODE trigger: R2 held ~3 s. handleInput sets m_r2Held from the
     * held mask every frame; this accumulates the time. */
    if (m_r2Held) {
        m_r2HoldMs += static_cast<int>(deltaMs);
        if (m_r2HoldMs >= 3000) {
            m_r2HoldMs = 0;
            m_r2Held = false;
            rebuildDeleteList();
            if (m_delList.empty()) {
                evo_feedback(EVO_FB_ERROR);
                toast("SUBTITLES", "NO DOWNLOADED SUBS TO DELETE");
            } else {
                m_deleteMode = true;
                m_selectedIndex = 0;
                evo_feedback(EVO_FB_OPEN);
                char msg[96];
                std::snprintf(msg, sizeof msg,
                              "DELETE MODE - %d TO DELETE, CROSS DELETES, CIRCLE CANCELS",
                              (int)m_delList.size());
                toast("SUBTITLES", msg);
            }
        }
    } else {
        m_r2HoldMs = 0;
    }
    /* The web search finishes on its own thread; refreshTracks only runs on
     * enter and on activation, so without this the rows would keep saying
     * SEARCHING long after the results landed. Rows from another movie are
     * invisible (refreshTracks gates on the owner), so only poll while the
     * search belongs to the file being played. */
    const char *owner = evo_websub_owner();
    const bool mine = owner[0] && current_media_path[0] &&
                      strcmp(owner, current_media_path) == 0;
    const int ws = mine ? evo_websub_status() : 0;
    if (ws != m_lastWebStatus) {
        if (m_lastWebStatus == 1 && ws == 2)
            toast("SUBTITLES", evo_websub_stage());     /* "12 FOUND" */
        else if (m_lastWebStatus == 1 && ws == 3)
            toast("SUBTITLES", "NO SUBS FOUND");
        m_lastWebStatus = ws;
        const int keep = m_selectedIndex;
        refreshTracks();
        if (keep < static_cast<int>(m_tracks.size()))
            m_selectedIndex = keep;
    }

    /* A download landing while the picker is open: the player's pump does
     * NOT run while this screen is on top, so consume the ready event here
     * and load it now - the state watch below then announces it and repaints
     * with the new subtitle marked selected. */
    char dpath[512];
    char dlabel[96];
    const int dr = evo_websub_take_ready(dpath, (int)sizeof dpath, dlabel, (int)sizeof dlabel);
    if (dr == 1) {
        if (prospero_secondary_use_external)
            prospero_secondary_subtitle_select(PROSPERO_SECONDARY_NONE);
        if (prospero_subtitle_load_file_quiet(dpath) <= 0) {
            toast("SUBTITLES", "THAT FILE HAS NO SUBTITLES");
            return;
        }
        std::snprintf(prospero_subtitle_external_label,
                      sizeof prospero_subtitle_external_label, "%s",
                      dlabel[0] ? dlabel : "WEB");
        prospero_subtitle_use_external = 1;
        prospero_subtitle_enabled = 1;
        evo_feedback(EVO_FB_CONFIRM);
    } else if (dr < 0) {
        toast("SUBTITLES", evo_websub_stage());
        return;
    }

    /* Watch the external-subtitle state: when a new subtitle has been
     * loaded, announce it and repaint so the EXTERNAL row shows as the
     * selected track. */
    const int ext_count = prospero_subtitle_count;
    const int ext_active = prospero_subtitle_enabled && prospero_subtitle_use_external;
    const char *ext_label = prospero_subtitle_external_label;
    if (ext_count != m_seenExtCount || ext_active != m_seenExtActive ||
        strcmp(ext_label, m_seenExtLabel) != 0) {
        if (ext_active && ext_count > 0 && m_seenExtLabel[0] &&
            strcmp(ext_label, m_seenExtLabel) != 0) {
            char msg[128];
            std::snprintf(msg, sizeof msg, "%s LOADED", ext_label);
            toast("SUBTITLES", msg);
        }
        m_seenExtCount = ext_count;
        m_seenExtActive = ext_active;
        std::snprintf(m_seenExtLabel, sizeof m_seenExtLabel, "%s", ext_label);
        const int keep = m_selectedIndex;
        refreshTracks();
        if (keep < static_cast<int>(m_tracks.size()))
            m_selectedIndex = keep;
    }
}

void SubtitlePickerScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_subtitles_params_t params;
    std::memset(&params, 0, sizeof(params));

    /* Convert 1-based face indices to 0-based preview face */
    const int size_idx = (prospero_subtitle_face >= 1 && prospero_subtitle_face <= PROSPERO_SUBTITLE_SIZES)
                       ? (prospero_subtitle_face - 1) : 1;
    const int size2_idx = (prospero_secondary_face >= 1 && prospero_secondary_face <= PROSPERO_SUBTITLE_SIZES)
                        ? (prospero_secondary_face - 1) : 1;
    const char* sizeNames[] = { "SMALL", "MEDIUM", "LARGE", "EXTRA LARGE" };
    params.size_str = sizeNames[size_idx];
    params.size2_str = sizeNames[size2_idx];

    static char s_syncBuf[64];
    if (prospero_subtitle_delay_ms == 0) {
        std::snprintf(s_syncBuf, sizeof(s_syncBuf), "SYNC: 0 ms");
    } else {
        std::snprintf(s_syncBuf, sizeof(s_syncBuf), "SYNC: %+d ms", prospero_subtitle_delay_ms);
    }
    if (prospero_subtitle_time_scale != 1.0) {
        // A framerate fix from AUTO-SYNC; the nudge still fine-tunes the offset.
        if (const char* ratio = evo_subsync_ratio_label(prospero_subtitle_time_scale)) {
            size_t len = std::strlen(s_syncBuf);
            std::snprintf(s_syncBuf + len, sizeof(s_syncBuf) - len, "  %s", ratio);
        }
    }
    params.sync_str = s_syncBuf;

    // The secondary track's own sync pill, only while there is one (#110).
    static char s_sync2Buf[32];
    if (prospero_subtitle_enabled && prospero_secondary_subtitle_active()) {
        if (prospero_secondary_delay_ms == 0)
            std::snprintf(s_sync2Buf, sizeof(s_sync2Buf), "2ND: 0 ms");
        else
            std::snprintf(s_sync2Buf, sizeof(s_sync2Buf), "2ND: %+d ms", prospero_secondary_delay_ms);
        params.sync2_str = s_sync2Buf;
    } else {
        params.sync2_str = "";
    }

    if (m_previewPrimary.empty() && m_previewSecondary.empty()) {
        rollRandomCues();
    }

    params.preview_text = m_previewPrimary.c_str();
    params.preview_text2 = m_previewSecondary.c_str();
    params.has_secondary = (prospero_subtitle_enabled && prospero_secondary_subtitle_active()) ? 1 : 0;
    params.secondary_position = prospero_secondary_position;
    params.secondary_color = prospero_secondary_color;
    params.primary_color = prospero_subtitle_color;
    params.preview_face = size_idx;
    params.preview_face2 = size2_idx;
    params.cue_str = "";

    params.eyebrow = m_deleteMode ? "DELETE MODE - CROSS DELETES, CIRCLE CANCELS"
                                  : "SUBTITLE CONFIGURATION";
    params.title = m_deleteMode ? "DELETE SUBTITLES" : "SUBTITLE TRACKS";

    /* DELETE MODE: the track list is replaced by everything EVO downloaded
     * for this movie, in red. */
    if (m_deleteMode) {
        static char s_delLabel[96];
        const int n = std::min(static_cast<int>(m_delList.size()), kVisibleRows);
        params.track_count = n;
        for (int i = 0; i < n; ++i) {
            std::snprintf(s_delLabel, sizeof s_delLabel, "DEL: %s",
                          m_delList[i].name.c_str());
            params.tracks[i].label = s_delLabel;
            params.tracks[i].detail = "DOWNLOADED BY EVO - CROSS DELETES";
            params.tracks[i].is_focused = (i == m_selectedIndex);
            params.tracks[i].is_current = 0;
            params.tracks[i].is_action = 1;
            /* is_disabled == 2 draws the row in red (see subtitles.rcss) */
            params.tracks[i].is_disabled = 2;
            params.tracks[i].tag = "";
        }
        evo_rmlui_update_subtitles(&params);
        evo_rmlui_render_subtitles(framebuffer, width, height);
        return;
    }

    int total = static_cast<int>(m_tracks.size());
    int rowsToDisplay = std::min(kVisibleRows, std::max(0, total - m_scrollOffset));
    static char s_autoSyncDetail[48];
    /* The selected marker is derived from live subtitle state every frame,
     * not from the track-building pass: when a download lands mid-session
     * the EXTERNAL row is current immediately, and whatever was marked
     * before stops being marked. */
    const bool ext_active = prospero_subtitle_enabled && prospero_subtitle_use_external &&
                            prospero_subtitle_count > 0;
    params.track_count = rowsToDisplay;
    for (int i = 0; i < rowsToDisplay; ++i) {
        int idx = m_scrollOffset + i;
        const int tid = m_tracks[idx].trackId;
        params.tracks[i].label = m_tracks[idx].label.c_str();
        params.tracks[i].detail = m_tracks[idx].detail.c_str();
        params.tracks[i].is_focused = (idx == m_selectedIndex);
        bool current;
        if (tid <= kExtSidecarBase && tid > kExtSidecarBase - kMaxExtSidecars) {
            /* the sidecar row whose file is the loaded external subtitle */
            current = ext_active && m_tracks[idx].extPath == prospero_subtitle_external_path();
        } else if (tid == -1)            /* the plain EXTERNAL row */
            current = ext_active;
        else if (tid == -2)              /* OFF */
            current = !prospero_subtitle_enabled;
        else if (tid >= 0)               /* an embedded stream */
            current = !ext_active && (idx == m_activeTrackIndex);
        else                             /* action / web rows */
            current = false;
        params.tracks[i].is_current = current ? 1 : 0;
        // [PRIMARY] / [SECONDARY]: which line of dialogue each track is (#110).
        params.tracks[i].tag = m_tracks[idx].isSecondary ? "SECONDARY"
                             : (current && idx != 0 &&
                                m_tracks[idx].trackId != kAutoSyncTrack) ? "PRIMARY" : "";
        if (m_tracks[idx].trackId == kLoadFileTrack) {
            params.tracks[i].is_current = 0;
            params.tracks[i].is_action = 1;
            params.tracks[i].tag = "";
        }
        if (m_tracks[idx].trackId == kAutoSyncTrack) {
            params.tracks[i].detail = prospero_subtitle_autosync_detail(s_autoSyncDetail, sizeof(s_autoSyncDetail));
            params.tracks[i].is_current = 0;
            params.tracks[i].is_action = 1;
            params.tracks[i].tag = "";
            params.tracks[i].is_disabled = !prospero_subtitle_autosync_running() &&
                                           !prospero_subtitle_autosync_available();
        }
        if (m_tracks[idx].trackId == kWebSearchTrack) {
            params.tracks[i].is_current = 0;
            params.tracks[i].is_action = 1;
            params.tracks[i].tag = "";
        }
        /* A web row is not this file's track: no PRIMARY/SECONDARY badge
         * until the download lands and becomes the external SRT. */
        if (m_tracks[idx].trackId <= kWebSubBase) {
            params.tracks[i].tag = "";
        }
    }

    evo_rmlui_update_subtitles(&params);
    evo_rmlui_render_subtitles(framebuffer, width, height);
}

} // namespace evo
