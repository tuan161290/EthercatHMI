/*
 * MPEG-TS muxer for one H.264 stream (camera_ts.h). ISO/IEC 13818-1: TS packet header 2.4.3.2,
 * adaptation field 2.4.3.4, PES header 2.4.3.6, PAT 2.4.4.3, PMT 2.4.4.8; the PIDs are FFmpeg's
 * defaults (PMT 0x1000, first stream 0x100).
 */

#include <string.h>
#include "camera_ts.h"

#define SYNC_BYTE   0x47
#define PID_PAT     0x0000
#define PID_PMT     0x1000
#define PID_VIDEO   0x0100
#define TYPE_H264   0x1B
#define STREAM_ID   0xE0     /* video stream 0 */
#define PTS_MASK    0x1FFFFFFFFULL /* 33 bits */
#define PCR_LEAD    9000     /* the PCR runs 100 ms behind the PTS: time for the reader to decode */
#define PAYLOAD_MAX (TS_PACKET - 4)
#define PCR_AF_LEN  8        /* adaptation field with a PCR: length byte, flags, 6 bytes of PCR */

/* PSI sections from table_id to the last byte before the CRC; one program, one stream. */
static const uint8_t PAT[] = {
    0x00,                                  /* table_id: PAT */
    0xB0, 0x0D,                            /* section_syntax_indicator, section_length 13 */
    0x00, 0x01,                            /* transport_stream_id */
    0xC1,                                  /* version 0, current */
    0x00, 0x00,                            /* section 0 of 0 */
    0x00, 0x01,                            /* program_number 1 */
    0xE0 | (PID_PMT >> 8), PID_PMT & 0xFF, /* its PMT */
};
static const uint8_t PMT[] = {
    0x02,                                      /* table_id: PMT */
    0xB0, 0x12,                                /* section_syntax_indicator, section_length 18 */
    0x00, 0x01,                                /* program_number 1 */
    0xC1,                                      /* version 0, current */
    0x00, 0x00,                                /* section 0 of 0 */
    0xE0 | (PID_VIDEO >> 8), PID_VIDEO & 0xFF, /* PCR_PID */
    0xF0, 0x00,                                /* program_info_length 0 */
    TYPE_H264,                                 /* stream_type */
    0xE0 | (PID_VIDEO >> 8), PID_VIDEO & 0xFF, /* elementary_PID */
    0xF0, 0x00,                                /* ES_info_length 0 */
};

/** CRC-32/MPEG-2: polynomial 0x04C11DB7, initial value all ones, no reflection, no final XOR. */
static uint32_t crc32_mpeg(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= (uint32_t)*p++ << 24;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
        }
    }
    return crc;
}

static bool flush(ts_mux_t *m)
{
    if (m->len == 0) {
        return true;
    }
    const bool ok = m->out(m->ctx, m->buf, m->len);
    m->len = 0;
    return ok;
}

/** The next packet in the batch, the batch sent first if it is full. @return NULL if sending failed */
static uint8_t *packet(ts_mux_t *m)
{
    if (m->len == TS_BATCH * TS_PACKET && !flush(m)) {
        return NULL;
    }
    uint8_t *p = m->buf + m->len;
    m->len += TS_PACKET;
    return p;
}

static void header(uint8_t *p, uint16_t pid, bool start, bool adaptation, uint8_t *cc)
{
    p[0] = SYNC_BYTE;
    p[1] = (start ? 0x40 : 0x00) | (uint8_t)(pid >> 8);
    p[2] = (uint8_t)pid;
    p[3] = (adaptation ? 0x30 : 0x10) | *cc; /* payload, with or without an adaptation field */
    *cc = (*cc + 1) & 0x0F;
}

/** One PSI section in one packet: pointer field, section, CRC, then 0xFF to the end. */
static bool psi(ts_mux_t *m, uint16_t pid, uint8_t *cc, const uint8_t *section, size_t n)
{
    uint8_t *p = packet(m);
    if (p == NULL) {
        return false;
    }
    header(p, pid, true, false, cc);
    p[4] = 0x00; /* pointer_field */
    memcpy(p + 5, section, n);
    const uint32_t crc = crc32_mpeg(section, n);
    p[5 + n] = (uint8_t)(crc >> 24);
    p[6 + n] = (uint8_t)(crc >> 16);
    p[7 + n] = (uint8_t)(crc >> 8);
    p[8 + n] = (uint8_t)crc;
    memset(p + 9 + n, 0xFF, TS_PACKET - 9 - n);
    return true;
}

void ts_mux_init(ts_mux_t *m, uint8_t *buf, ts_out_fn out, void *ctx)
{
    *m = (ts_mux_t){.buf = buf, .out = out, .ctx = ctx};
}

bool ts_mux_frame(ts_mux_t *m, const uint8_t *au, size_t len, bool key, uint64_t pts)
{
    if (key && !(psi(m, PID_PAT, &m->cc_pat, PAT, sizeof(PAT)) && psi(m, PID_PMT, &m->cc_pmt, PMT, sizeof(PMT)))) {
        return false;
    }
    pts &= PTS_MASK;
    const uint64_t pcr = (pts - PCR_LEAD) & PTS_MASK;

    /* PES header with the PTS only (no B-frames, so DTS = PTS) */
    const size_t pes_len = 8 + len; /* bytes after PES_packet_length: flags, header length, PTS, frame */
    const uint8_t head[14] = {
        0x00, 0x00, 0x01, STREAM_ID,
        pes_len > 0xFFFF ? 0 : (uint8_t)(pes_len >> 8), /* 0: unbounded, allowed for video */
        pes_len > 0xFFFF ? 0 : (uint8_t)pes_len,
        0x84, /* '10', data_alignment_indicator: the frame starts here */
        0x80, /* PTS only */
        0x05, /* PES_header_data_length */
        (uint8_t)(0x21 | ((pts >> 29) & 0x0E)), /* '0010', PTS[32..30], marker */
        (uint8_t)(pts >> 22),
        (uint8_t)(((pts >> 14) & 0xFE) | 1),    /* PTS[22..15], marker */
        (uint8_t)(pts >> 7),
        (uint8_t)(((pts << 1) & 0xFE) | 1),     /* PTS[7..0], marker */
    };

    const size_t total = sizeof(head) + len;
    size_t done = 0;
    bool first = true;
    while (done < total) {
        uint8_t *p = packet(m);
        if (p == NULL) {
            return false;
        }
        size_t af = first ? PCR_AF_LEN : 0; /* adaptation field bytes, its length byte included */
        size_t n = total - done;
        if (n > PAYLOAD_MAX - af) {
            n = PAYLOAD_MAX - af;
        } else {
            af = PAYLOAD_MAX - n; /* the last packet: the adaptation field takes the rest */
        }
        header(p, PID_VIDEO, first, af > 0, &m->cc_video);
        uint8_t *q = p + 4;
        if (af > 0) {
            q[0] = (uint8_t)(af - 1); /* adaptation_field_length; 0 is a single stuffing byte */
            size_t used = 1;
            if (af > 1) {
                q[1] = first ? (uint8_t)(0x10 | (key ? 0x40 : 0x00)) : 0x00; /* PCR_flag, random_access */
                used = 2;
                if (first) {
                    q[2] = (uint8_t)(pcr >> 25);
                    q[3] = (uint8_t)(pcr >> 17);
                    q[4] = (uint8_t)(pcr >> 9);
                    q[5] = (uint8_t)(pcr >> 1);
                    q[6] = (uint8_t)(((pcr & 1) << 7) | 0x7E); /* 6 reserved bits */
                    q[7] = 0x00;                               /* PCR extension 0 */
                    used = PCR_AF_LEN;
                }
            }
            memset(q + used, 0xFF, af - used);
            q += af;
        }
        /* n bytes of the PES: the rest of the header, then the frame */
        size_t i = 0;
        if (done < sizeof(head)) {
            i = sizeof(head) - done < n ? sizeof(head) - done : n;
            memcpy(q, head + done, i);
        }
        if (i < n) {
            memcpy(q + i, au + (done + i - sizeof(head)), n - i);
        }
        done += n;
        first = false;
    }
    return flush(m);
}
