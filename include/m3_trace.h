#ifndef M3_TRACE_H
#define M3_TRACE_H

#include <gba_types.h>

#ifndef M3_TRACE
#define M3_TRACE 0
#endif

#ifndef M3_TRACE_DETAIL
#define M3_TRACE_DETAIL 0
#endif

enum {
    M3_TRACE_ZONE_VIDEO_DECODE = 0,
    M3_TRACE_ZONE_VRAM_COPY = 1,
    M3_TRACE_ZONE_AUDIO_CACHE = 2,
    M3_TRACE_ZONE_AUDIO_IRQ = 3,
    M3_TRACE_ZONE_FRAME_BYTES = 4,
    M3_TRACE_ZONE_SYNC_LAG = 5,
    M3_TRACE_ZONE_COPY_CALLS = 6,
    M3_TRACE_ZONE_COPY_PIXELS = 7,
    M3_TRACE_ZONE_DELTA_CALLS = 8,
    M3_TRACE_ZONE_DELTA_PIXELS = 9,
    M3_TRACE_ZONE_FILL_CALLS = 10,
    M3_TRACE_ZONE_FILL_PIXELS = 11,
    M3_TRACE_ZONE_FLAG_BYTES = 12,
    M3_TRACE_ZONE_PALETTE_BYTES = 13,
    M3_TRACE_ZONE_PAYLOAD_BYTES = 14,
};

#if M3_TRACE

#define M3_TRACE_EVENT_ADDR ((volatile u32*)0x0E00FFF0u)
#define M3_TRACE_DATA_ADDR  ((volatile u32*)0x0E00FFF4u)

#define M3_TRACE_MAGIC 0xA5000000u
#define M3_TRACE_OP_BEGIN 1u
#define M3_TRACE_OP_END   2u
#define M3_TRACE_OP_VALUE 3u
#define M3_TRACE_OP_FRAME 4u

static inline void m3_trace_event(u32 op, u32 zone, u32 value) {
    *M3_TRACE_DATA_ADDR = value;
    *M3_TRACE_EVENT_ADDR = M3_TRACE_MAGIC | ((op & 0xFu) << 20) | ((zone & 0xFFu) << 12);
}

static inline void m3_trace_begin(u32 zone) {
    m3_trace_event(M3_TRACE_OP_BEGIN, zone, 0);
}

static inline void m3_trace_end(u32 zone) {
    m3_trace_event(M3_TRACE_OP_END, zone, 0);
}

static inline void m3_trace_value(u32 zone, u32 value) {
    m3_trace_event(M3_TRACE_OP_VALUE, zone, value);
}

static inline void m3_trace_frame(u32 frame) {
    m3_trace_event(M3_TRACE_OP_FRAME, 0, frame);
}

#else

#define m3_trace_begin(zone) ((void)0)
#define m3_trace_end(zone) ((void)0)
#define m3_trace_value(zone, value) ((void)0)
#define m3_trace_frame(frame) ((void)0)

#endif

#endif
