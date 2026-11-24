# Web subtitle search for locally-played files

Goal: when a local / USB / SMB file has no usable subtitle track, search
subtitle websites from inside the subtitle picker, download a zip, unpack the
.srt and load it as the external subtitle - without leaving the picker.

Upstream tracking: roadmap.md, "Subtitles and text" section.

## Today

Online subtitles exist only for provider items: Stremio addons with a
"subtitles" resource (OpenSubtitles v3 via Stremio's proxy) fill
`evo_online_subs` while the provider resolves the title, keyed by item id
(`addons/src/evo_online_subs.c`, filled by `provider_stremio.c`). A local
file has no provider item, so the picker offers only LOAD FROM FILE.

## Design

New module `addons/src/evo_websub.c` + `addons/include/evo_websub.h`,
mirroring the `evo_online_subs` shape:

- `evo_websub_begin_search(media_path)` - UI thread kicks it; a detached
  worker (512 KiB stack, like `evo_osubs_fetch_async`) parses the filename
  (title / year / S01E01), computes the OpenSubtitles moviehash (64-bit size
  + head/tail 64 KiB chunk sums, little-endian), and queries every enabled
  site. Results land in one mutex-guarded list the picker reads.
- `evo_websub_results_count/get` - picker rows "ONLINE (WEB)", tagged with
  site + language.
- `evo_websub_fetch_async(i)` + `evo_websub_take_ready()` - the
  `evo_osubs_fetch_async` / `evo_osubs_take_ready` pattern: synchronous
  download on the worker (`evo_net_http_get_sync`), unzip (zlib, already
  linked via the SDK), write the .srt to `evo_data_path("websub_pick.srt")`,
  and `PlayerScreen::update` pumps it into
  `prospero_subtitle_load_file_quiet()` - the same path online Stremio subs
  already use.
- A SEARCH action row opens the virtual keyboard (`evo_keyboard_open`) and
  re-runs the search with a typed query when the filename parse was wrong.

## Sites (v1)

| Site | Search | Notes |
|---|---|---|
| OpenSubtitles.com | moviehash + query | official REST (`api.opensubtitles.com/api/v1`); free API key, stored as `/data/evoplayer/opensubtitles.json` (same JSON pattern as `evo_stremio_account.c`); download needs a user token for some files |
| Podnapisi.net | hash + query | no key needed; zip downloads |
| SubDL | query (+ film id) | free API key |

All three serve zipped SRTs over HTTPS. Skipped for v1: Subscene (dead),
Addic7ed (login + anti-bot), YIFY Subtitles (HTML scraping).

## Phases

1. Backend: hash, filename parse, per-site adapters, unzip, result store.
2. Picker: ONLINE (WEB) rows + SEARCH ONLINE... keyboard row; language
   filter via SUBTITLE LANGUAGE; auto-pick on open when the file has no
   embedded track.
3. Settings: per-site toggles + API keys; the save/load round-trip test
   grows with every new setting (roadmap constraint).

Nothing here touches the console without a hardware cycle: play a local
file with no embedded subs, search, download, load, verify render, then
the source-switch regression (the play_fmt race fixed in commit 8bc2451).
