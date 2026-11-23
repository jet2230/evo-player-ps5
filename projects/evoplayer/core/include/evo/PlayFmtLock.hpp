#pragma once

/*
 * Synchronized access to the resident AVFormatContext (play_fmt).
 *
 * play_fmt is closed and replaced on playback worker threads (provider
 * auto-next, the web/cast handoff worker in Application.cpp) as well as on
 * the UI thread, while UI screens walk it - the subtitle picker reads
 * nb_streams / streams[] the moment DOWN opens it. A walk that overlaps a
 * teardown used to read a freed context and crash the app (see FIX.md).
 *
 * Usage on a UI screen - hold it only across a memory-only walk
 * (av_dict_get / avcodec_get_name / codecpar fields); never network I/O:
 *
 *     if (AVFormatContext *fmt = evo_play_fmt_acquire()) {
 *         ... read fmt ...
 *         evo_play_fmt_release();
 *     }
 *
 * Teardown uses evo_play_fmt_swap_out() and closes the old context outside
 * the lock: avformat_close_input can block on network I/O and the UI must
 * never wait on it.
 */

/* Wrapped in extern "C" so this header can be included before a .cpp's own
 * avformat include without flipping FFmpeg's declarations to C++ linkage -
 * the library symbols are plain C and the C++-mangled references do not link. */
extern "C" {
#include <libavformat/avformat.h>
}

extern "C" {
/* Locks the play_fmt mutex and returns the current context (may be null).
 * Every acquire must be paired with exactly one evo_play_fmt_release(),
 * with no early return in between. */
AVFormatContext *evo_play_fmt_acquire(void);
void evo_play_fmt_release(void);

/* Atomically takes the current context out of play_fmt (which becomes
 * null) so the caller can close it without holding the lock. */
void evo_play_fmt_swap_out(AVFormatContext **out);
}
