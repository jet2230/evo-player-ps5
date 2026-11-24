# FIX: D-pad DOWN (subtitle picker) freezes / crashes the app

**Status:** root cause identified AND confirmed from the console's own logs;
fix implemented, compile check running.
**Symptom:** during playback, pressing D-pad DOWN to open the subtitle picker
sometimes freezes or crashes the whole app. Timing-dependent ("sometimes"),
not file-specific.

## Console evidence (2026-11-23, pulled over the HTTP file manager on :8888)

`/data/evoplayer/evo.prev.log` (the crashed run): an MKV with **two PGS
subtitle streams** (`hdmv_pgs_subtitle`, streams 3 and 4). The playback
decoder closed and re-opened 7 times in ~5 s (each open logs the same PGS
probe warnings); the log's **last line is a probe warning mid-open** — the
app died while a file open/teardown was in flight. `/data/evoplayer/evo_report.txt`
confirms: "last run: ended without closing (PS button or crash)".

PGS tracks never fill the embedded **text** ring, so every picker open fell
into the synchronous file re-open path, and the repeated re-opens are exactly
the teardown/reopen window the picker's unlocked `play_fmt` walk loses to.

## How DOWN reaches the crash

`PlayerScreen.cpp:513` → `screenMgr->navigateTo(ScreenId::SubtitlePicker)` →
`SubtitlePickerScreen::onEnter()` (`SubtitlePickerScreen.cpp:27`) which, on the
**UI thread**, walks live playback state:

1. `refreshTracks()` (`SubtitlePickerScreen.cpp:42-181`) reads the global
   `AVFormatContext *play_fmt` (`:71-127`) — `nb_streams`, `streams[i]`,
   `codecpar`, metadata dicts.
2. `rollRandomCues()` (`:322-328`) → `prospero_subtitle_sample_random_cues()`
   (`evo_subtitle.c:3272`) → `prospero_subtitle_get_cue_count/_get_cue_info`
   (`evo_subtitle.c:3121-3230`) which read the embedded subtitle ring
   (`prospero_sub_slot[]`), the external cue array (`prospero_subtitle_cues[]` /
   `prospero_subtitle_count`), and can fall back to
   `prospero_subtitle_extract_samples_from_file()` (`evo_subtitle.c:2991`).

## Root cause (confirmed): unsynchronised teardown on background workers

Playback is started **on worker threads**, not the UI thread:

- `Application.cpp:637-644` — `web_start_worker` (system-browser handoff,
  YouTube link, phone remote play, cast): comment says it itself —
  "startPlaybackSource() blocks on network I/O, so it runs on a worker".
- `ProviderHostScreen.cpp:169-179, 217` — `start_playback_worker` (every
  provider episode start / auto-next).

Both call `PlaybackController::startPlaybackSource()` → `stopPlayback()`
(`PlaybackController.cpp:573`), which on that **worker thread**:

- joins the demux/decode threads and calls `prospero_embedded_subtitle_close()`
  (`PlaybackController.cpp:606`) — frees the subtitle ring/slot state the
  picker samples;
- does `avformat_close_input(&play_fmt); play_fmt = nullptr;`
  (`PlaybackController.cpp:703-706`) — frees the context the picker walks.

None of this is synchronised against the UI thread. Press DOWN while a
background source switch (provider auto-next, cast, web handoff, episode
advance) is tearing down the previous file and the picker reads freed memory:
SIGSEGV (crash) or a hang inside torn-down decoder/AV state (freeze).
Worker teardown + UI press must overlap only within a small window, hence
"sometimes".

Secondary readers of `play_fmt` on the UI thread share the exposure:
`PlayerScreen.cpp:716-1017`, `MediaInfoScreen.cpp:67,132-135`.

## Contributing defect (freeze even without the race)

`prospero_subtitle_get_cue_count()` (`evo_subtitle.c:3147-3159`) falls back to
`prospero_subtitle_extract_samples_from_file()`, which **opens the media file
again and decodes up to 500 packets synchronously on the UI thread**
(`evo_subtitle.c:3010-3061`). Network URLs are already excluded
(`evo_subtitle.c:2999`, added after the 75 s dead-host freeze), but a large
local MKV on USB still stalls the UI for a noticeable chunk of a second every
time the ring and sample pool are empty (e.g. a PGS/bitmap track that never
fills the text ring). This runs exactly when the picker opens.

## Fix plan

1. **Serialize teardown vs the UI thread (the crash).** — IMPLEMENTED
   - Global `play_fmt` lock: pthread mutex in `Bridge.cpp`; C-visible helpers
     `evo_play_fmt_acquire()` / `evo_play_fmt_release()` /
     `evo_play_fmt_swap_out()`, declared in `evo/PlayFmtLock.hpp`.
   - `stopPlayback()` and the three early-fail closes in
     `startPlaybackSource()` now swap `play_fmt` out under the lock
     (`close_play_fmt_safely()`) and close it **outside** the lock (close can
     block on network I/O — the UI must never wait on it).
   - `SubtitlePickerScreen::refreshTracks()` walks the context between
     acquire/release; so do `activateSelection()` /
     `activateSecondary()` (their select paths read `play_fmt` inside
     `evo_subtitle.c`).
2. **Never stall the picker (the freeze).** — IMPLEMENTED
   - `prospero_subtitle_preview_random_cues()` (`evo_subtitle.c`): same
     sampling as before, but while its `s_preview_only` flag is set the cue
     getters skip the synchronous file re-open
     (`extract_samples_from_file`). The picker's `rollRandomCues()` uses it.
     The player's own display path keeps the old behavior.
3. **Harden the other UI readers of `play_fmt`** — IMPLEMENTED
   - `PlayerScreen` (performance HUD + OSD badges) and `MediaInfoScreen`
     (metadata + color-trc read) now acquire/release; the local shadows the
     global so the block bodies are unchanged.

## Verification plan

1. Host compile check: `./scripts/build-evoplayer.sh` in the pinned container.
2. Host UI render: `./tools/uiview.sh --all` (picker screen still renders).
3. Hardware cycle on the PS5 (192.168.0.19): deploy `--ffpfsc --usb-remote`,
   play a file with embedded subtitles, open the picker with DOWN while a
   provider episode auto-advances / a second source is started from the phone
   page, repeat the overlap ~10x, confirm no panic; pull `/mnt/usb0/evo.log`
   and check ShadowMount lifecycle (one cycle at a time, quit → close before
   redeploy).
4. Regression: picker still lists tracks, AUTO-SYNC row, secondary selection,
   "LOAD FROM FILE..." navigation.

## Evidence still needed

- `/mnt/usb0/evo.log` (+ kept last-run log) from a crashed run to confirm the
  faulting frame — **blocked: FTP on 192.168.0.19:2121 currently refuses
  connections** (payload not running?).
- Docker is not installed on this host; the pinned toolchain container is
  required to build (`docker compose run --rm ps5-dev ...`). Pending user
  approval to install Docker.

## Files that will change

| File | Change |
| --- | --- |
| `projects/evoplayer/core/src/Bridge.cpp` | play_fmt lock + acquire/release helpers |
| `projects/evoplayer/core/src/services/PlaybackController.cpp` | swap-out-then-close in the 4 teardown sites |
| `projects/evoplayer/core/src/screens/SubtitlePickerScreen.cpp` | acquire/release around the walk; no synchronous file re-open |
| `projects/evoplayer/media/src/evo_subtitle.c` | close-vs-reader guard; preview path without file re-open |
| `projects/evoplayer/core/src/screens/PlayerScreen.cpp`, `MediaInfoScreen.cpp` | acquire/release around play_fmt reads |
