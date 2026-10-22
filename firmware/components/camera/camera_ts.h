/*
 * MPEG-TS for one H.264 video stream, the format the push sends to go2rtc's /api/stream.ts?dst=
 * (camera_push.c). Shaped by what go2rtc 1.9.14 reads (pkg/mpegts/demuxer.go):
 * - PAT and PMT before every key frame. go2rtc reads them once; a viewer that joins later does not
 *   need them, but other readers do.
 * - One PES per frame with a PTS (90 kHz) and its length when it fits in 16 bits, so go2rtc passes the
 *   frame on as soon as its last byte arrives; with length 0 (frames over 64 KB) it waits for the next.
 * - The last TS packet of a frame is padded in the adaptation field: go2rtc does not cut padding off
 *   the payload.
 * - PCR in the first packet of every frame (go2rtc ignores it; other readers expect one).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TS_PACKET 188
#define TS_BATCH  32 /* packets handed to `out` at once (6 KB) */

/** Sends `len` bytes; false ends the stream. */
typedef bool (*ts_out_fn)(void *ctx, const uint8_t *data, size_t len);

typedef struct {
    uint8_t *buf; /* TS_BATCH * TS_PACKET bytes */
    size_t len;
    ts_out_fn out;
    void *ctx;
    uint8_t cc_pat, cc_pmt, cc_video; /* continuity counters */
} ts_mux_t;

/** `buf` holds TS_BATCH * TS_PACKET bytes and stays the caller's. */
void ts_mux_init(ts_mux_t *m, uint8_t *buf, ts_out_fn out, void *ctx);

/**
 * One H.264 access unit (Annex B, SPS and PPS before a key frame) with its presentation time in
 * 90 kHz units; sent through `out` before it returns. @return false if `out` failed
 */
bool ts_mux_frame(ts_mux_t *m, const uint8_t *au, size_t len, bool key, uint64_t pts);
