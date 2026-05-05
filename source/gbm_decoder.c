#include "gbm_decoder.h"
#include "m3_trace.h"
#include <string.h>

#define ROW_BYTES (FRAME_WIDTH * 2)

#if M3_TRACE && M3_TRACE_DETAIL
static u32 trace_copy_calls;
static u32 trace_copy_pixels;
static u32 trace_delta_calls;
static u32 trace_delta_pixels;
static u32 trace_fill_calls;
static u32 trace_fill_pixels;

static inline void trace_reset_frame_stats(void) {
    trace_copy_calls = 0;
    trace_copy_pixels = 0;
    trace_delta_calls = 0;
    trace_delta_pixels = 0;
    trace_fill_calls = 0;
    trace_fill_pixels = 0;
}

static inline void trace_emit_frame_stats(u32 flag_bytes, u32 palette_bytes, u32 payload_bytes) {
    m3_trace_value(M3_TRACE_ZONE_COPY_CALLS, trace_copy_calls);
    m3_trace_value(M3_TRACE_ZONE_COPY_PIXELS, trace_copy_pixels);
    m3_trace_value(M3_TRACE_ZONE_DELTA_CALLS, trace_delta_calls);
    m3_trace_value(M3_TRACE_ZONE_DELTA_PIXELS, trace_delta_pixels);
    m3_trace_value(M3_TRACE_ZONE_FILL_CALLS, trace_fill_calls);
    m3_trace_value(M3_TRACE_ZONE_FILL_PIXELS, trace_fill_pixels);
    m3_trace_value(M3_TRACE_ZONE_FLAG_BYTES, flag_bytes);
    m3_trace_value(M3_TRACE_ZONE_PALETTE_BYTES, palette_bytes);
    m3_trace_value(M3_TRACE_ZONE_PAYLOAD_BYTES, payload_bytes);
}

#define TRACE_COPY(rows, pixels_per_row) do { \
    ++trace_copy_calls; \
    trace_copy_pixels += (u32)(rows) * (u32)(pixels_per_row); \
} while (0)
#define TRACE_DELTA(rows, pixels_per_row) do { \
    ++trace_delta_calls; \
    trace_delta_pixels += (u32)(rows) * (u32)(pixels_per_row); \
} while (0)
#define TRACE_FILL(rows, pixels_per_row) do { \
    ++trace_fill_calls; \
    trace_fill_pixels += (u32)(rows) * (u32)(pixels_per_row); \
} while (0)
#else
#define trace_reset_frame_stats() ((void)0)
#define trace_emit_frame_stats(flag_bytes, palette_bytes, payload_bytes) ((void)0)
#define TRACE_COPY(rows, pixels_per_row) ((void)0)
#define TRACE_DELTA(rows, pixels_per_row) ((void)0)
#define TRACE_FILL(rows, pixels_per_row) ((void)0)
#endif

// XOR key for decoding flag_bytes (default to Gen1)
static u16 xor_key = 0xD669;

void gbm_set_version(u8 version) {
    if (version == GBM_VERSION_GEN3) {
        xor_key = 0xD6AC;
    } else if (version == GBM_VERSION_V130) {
        xor_key = 0x0000;  // No encryption
    } else {
        xor_key = 0xD669;  // Gen1 default
    }
}

// Codebook offsets - place in IWRAM for fast access (256 bytes)
// Use .iwram.rodata to avoid conflict with .iwram code section
__attribute__((section(".iwram.rodata"))) static const s16 CODEBOOK_OFFSETS[] = {
    -3856, -3854, -3852, -3850, -3848, -3846, -3844, -3842,
    -3840, -3838, -3836, -3834, -3832, -3830, -3828, -3826,
    -3376, -3374, -3372, -3370, -3368, -3366, -3364, -3362,
    -3360, -3358, -3356, -3354, -3352, -3350, -3348, -3346,
    -2896, -2894, -2892, -2890, -2888, -2886, -2884, -2882,
    -2880, -2878, -2876, -2874, -2872, -2870, -2868, -2866,
    -2416, -2414, -2412, -2410, -2408, -2406, -2404, -2402,
    -2400, -2398, -2396, -2394, -2392, -2390, -2388, -2386,
    -1936, -1934, -1932, -1930, -1928, -1926, -1924, -1922,
    -1920, -1918, -1916, -1914, -1912, -1910, -1908, -1906,
    -1456, -1454, -1452, -1450, -1448, -1446, -1444, -1442,
    -1440, -1438, -1436, -1434, -1432, -1430, -1428, -1426,
    -976, -974, -972, -970, -968, -966, -964, -962,
    -960, -958, -956, -954, -952, -950, -948, -946,
    -496, -494, -492, -490, -488, -486, -484, -482,
    -480, -478, -476, -474, -472, -470, -468, -466,
    -16, -14, -12, -10, -8, -6, -4, -2,
    0, 2, 4, 6, 8, 10, 12, 14,
    464, 466, 468, 470, 472, 474, 476, 478,
    480, 482, 484, 486, 488, 490, 492, 494,
    944, 946, 948, 950, 952, 954, 956, 958,
    960, 962, 964, 966, 968, 970, 972, 974,
    1424, 1426, 1428, 1430, 1432, 1434, 1436, 1438,
    1440, 1442, 1444, 1446, 1448, 1450, 1452, 1454,
    1904, 1906, 1908, 1910, 1912, 1914, 1916, 1918,
    1920, 1922, 1924, 1926, 1928, 1930, 1932, 1934,
    2384, 2386, 2388, 2390, 2392, 2394, 2396, 2398,
    2400, 2402, 2404, 2406, 2408, 2410, 2412, 2414,
    2864, 2866, 2868, 2870, 2872, 2874, 2876, 2878,
    2880, 2882, 2884, 2886, 2888, 2890, 2892, 2894,
    3344, 3346, 3348, 3350, 3352, 3354, 3356, 3358,
    3360, 3362, 3364, 3366, 3368, 3370, 3372, 3374,
};

// Inline helpers
static inline u32 read_u32_unaligned(const u8 *ptr) {
    // GBA supports unaligned loads? NO. ARM7TDMI does NOT support unaligned loads correctly (it rotates).
    // We must construct it.
    return ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
}

static inline u16 read_u16_unaligned(const u8 *ptr) {
    return ptr[0] | (ptr[1] << 8);
}

static inline u32 obfuscation_mix(u32 value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

static inline u16 frame_field_key(u32 frame_index, u32 salt) {
    return (u16)obfuscation_mix(0x5258544cu ^ salt ^ frame_index);
}

// Critical Path: next_bit
static inline __attribute__((always_inline)) int next_bit(DecodeContext *ctx) {
    if (ctx->state == (1u << 31)) {
        u32 word = read_u32_unaligned(ctx->flag_ptr);
        ctx->flag_ptr += 4;
        int bit = word >> 31;
        ctx->state = (word << 1) | 1;
        return bit;
    }
    int bit = ctx->state >> 31;
    ctx->state <<= 1;
    return bit;
}

// Read 2 bits at once - optimized for common decode patterns
static inline __attribute__((always_inline)) int next_2bits(DecodeContext *ctx) {
    u32 state = ctx->state;

    // Fast path: sentinel is in low 30 bits, we have at least 2 data bits
    if (state & 0x3FFFFFFF) {
        int bits = state >> 30;
        ctx->state = state << 2;
        return bits;
    }

    // Medium path: sentinel at bit 30, exactly 1 data bit at bit 31
    // state is either 0x40000000 (bit0=0) or 0xC0000000 (bit0=1)
    if (state & (1u << 30)) {
        int bit0 = state >> 31;  // Read the 1 available data bit
        // Refill and read second bit
        u32 word = read_u32_unaligned(ctx->flag_ptr);
        ctx->flag_ptr += 4;
        int bit1 = word >> 31;
        ctx->state = (word << 1) | 1;
        return (bit0 << 1) | bit1;
    }

    // Slow path: sentinel at bit 31 (no data bits), refill and read 2 bits
    u32 word = read_u32_unaligned(ctx->flag_ptr);
    ctx->flag_ptr += 4;
    int bits = word >> 30;
    ctx->state = (word << 2) | 2;
    return bits;
}

static inline u16 read_palette_color(DecodeContext *ctx) {
    u16 color = read_u16_unaligned(ctx->palette_ptr);
    ctx->palette_ptr += 2;
    return color;
}

static inline s16 to_signed16(u16 val) {
    return (s16)val;
}

static inline u8 read_code(DecodeContext *ctx) {
    u8 code = *ctx->payload_ptr;
    ctx->payload_ptr++;
    return code;
}

// Block operations - Hot path
// dst is always 4-byte aligned (block starts at 8x8 boundaries)
// Use 32-bit writes to EWRAM for better throughput
// Use pointer increment instead of recalculating offset each row
#define ROW_STRIDE (ROW_BYTES >> 1)  // stride in u16 units (240)
static IWRAM_CODE void copy_u32_block(DecodeContext *ctx, int dst_off, int ref_off, int rows, int words) {
    TRACE_COPY(rows, words * 2);
    u32 *d = (u32*)(ctx->dst + (dst_off >> 1));
    const u16 *s = ctx->ref + (ref_off >> 1);
    if ((ref_off & 2) == 0) {
        const u32 *sw = (const u32*)s;
        switch (words) {
        case 1:
            for (int r = 0; r < rows; r++) {
                d[0] = sw[0];
                d = (u32*)((u16*)d + ROW_STRIDE);
                sw = (const u32*)((const u16*)sw + ROW_STRIDE);
            }
            return;
        case 2:
            for (int r = 0; r < rows; r++) {
                d[0] = sw[0];
                d[1] = sw[1];
                d = (u32*)((u16*)d + ROW_STRIDE);
                sw = (const u32*)((const u16*)sw + ROW_STRIDE);
            }
            return;
        case 4:
            for (int r = 0; r < rows; r++) {
                d[0] = sw[0];
                d[1] = sw[1];
                d[2] = sw[2];
                d[3] = sw[3];
                d = (u32*)((u16*)d + ROW_STRIDE);
                sw = (const u32*)((const u16*)sw + ROW_STRIDE);
            }
            return;
        default:
            break;
        }
    }
    switch (words) {
    case 1:
        for (int r = 0; r < rows; r++) {
            d[0] = s[0] | ((u32)s[1] << 16);
            d = (u32*)((u16*)d + ROW_STRIDE);
            s += ROW_STRIDE;
        }
        break;
    case 2:
        for (int r = 0; r < rows; r++) {
            d[0] = s[0] | ((u32)s[1] << 16);
            d[1] = s[2] | ((u32)s[3] << 16);
            d = (u32*)((u16*)d + ROW_STRIDE);
            s += ROW_STRIDE;
        }
        break;
    case 4:
        for (int r = 0; r < rows; r++) {
            d[0] = s[0] | ((u32)s[1] << 16);
            d[1] = s[2] | ((u32)s[3] << 16);
            d[2] = s[4] | ((u32)s[5] << 16);
            d[3] = s[6] | ((u32)s[7] << 16);
            d = (u32*)((u16*)d + ROW_STRIDE);
            s += ROW_STRIDE;
        }
        break;
    default:
        for (int r = 0; r < rows; r++) {
            const u16 *sp = s;
            for (int i = 0; i < words; i++) {
                d[i] = sp[0] | ((u32)sp[1] << 16);
                sp += 2;
            }
            d = (u32*)((u16*)d + ROW_STRIDE);
            s += ROW_STRIDE;
        }
        break;
    }
}

static IWRAM_CODE void fill_u32_block(DecodeContext *ctx, int dst_off, int rows, int words, u16 color) {
    TRACE_FILL(rows, words * 2);
    u32 color32 = color | ((u32)color << 16);
    u32 *d = (u32*)(ctx->dst + (dst_off >> 1));
    switch (words) {
    case 1:
        for (int r = 0; r < rows; r++) {
            d[0] = color32;
            d = (u32*)((u16*)d + ROW_STRIDE);
        }
        break;
    case 2:
        for (int r = 0; r < rows; r++) {
            d[0] = color32;
            d[1] = color32;
            d = (u32*)((u16*)d + ROW_STRIDE);
        }
        break;
    case 4:
        for (int r = 0; r < rows; r++) {
            d[0] = color32;
            d[1] = color32;
            d[2] = color32;
            d[3] = color32;
            d = (u32*)((u16*)d + ROW_STRIDE);
        }
        break;
    default:
        for (int r = 0; r < rows; r++) {
            for (int i = 0; i < words; i++) {
                d[i] = color32;
            }
            d = (u32*)((u16*)d + ROW_STRIDE);
        }
        break;
    }
}

static IWRAM_CODE void delta_u32_block(DecodeContext *ctx, int dst_off, int ref_off, int rows, int words, s16 delta) {
    TRACE_DELTA(rows, words * 2);
    u32 *d = (u32*)(ctx->dst + (dst_off >> 1));
    const u16 *s = ctx->ref + (ref_off >> 1);
    // RGB555: bit15 is unused, can absorb carry from lower pixel
    // Pack delta into both halves, clear bit15/31 before add to prevent overflow propagation
    u32 delta32 = (u16)delta | ((u32)(u16)delta << 16);
    for (int r = 0; r < rows; r++) {
        const u16 *sp = s;
        for (int i = 0; i < words; i++) {
            u32 val = sp[0] | ((u32)sp[1] << 16);
            d[i] = (val & 0x7FFF7FFF) + delta32;
            sp += 2;
        }
        d = (u32*)((u16*)d + ROW_STRIDE);
        s += ROW_STRIDE;
    }
}

static IWRAM_CODE void copy_u16_block(DecodeContext *ctx, int dst_off, int ref_off, int rows, int halfwords) {
    TRACE_COPY(rows, halfwords);
    u16 *d = ctx->dst + (dst_off >> 1);
    const u16 *s = ctx->ref + (ref_off >> 1);
    for (int r = 0; r < rows; r++) {
        for (int i = 0; i < halfwords; i++) {
            d[i] = s[i];
        }
        d += ROW_STRIDE;
        s += ROW_STRIDE;
    }
}

static IWRAM_CODE void fill_u16_block(DecodeContext *ctx, int dst_off, int rows, int halfwords, u16 color) {
    TRACE_FILL(rows, halfwords);
    u16 *d = ctx->dst + (dst_off >> 1);
    for (int r = 0; r < rows; r++) {
        for (int i = 0; i < halfwords; i++) {
            d[i] = color;
        }
        d += ROW_STRIDE;
    }
}

static IWRAM_CODE void delta_u16_block(DecodeContext *ctx, int dst_off, int ref_off, int rows, int halfwords, s16 delta) {
    TRACE_DELTA(rows, halfwords);
    u16 *d = ctx->dst + (dst_off >> 1);
    const u16 *s = ctx->ref + (ref_off >> 1);
    for (int r = 0; r < rows; r++) {
        for (int i = 0; i < halfwords; i++) {
            d[i] = s[i] + delta;
        }
        d += ROW_STRIDE;
        s += ROW_STRIDE;
    }
}

#define GBM_ALWAYS_INLINE static inline __attribute__((always_inline))
#if M3_TRACE_DETAIL
#define GBM_2X2_ATTR static IWRAM_CODE
#else
#define GBM_2X2_ATTR GBM_ALWAYS_INLINE
#endif

// Forward declarations
static IWRAM_CODE void decode_block_8x4(DecodeContext *ctx);
static IWRAM_CODE void decode_block_4x8(DecodeContext *ctx);
GBM_ALWAYS_INLINE void decode_block_4x4(DecodeContext *ctx);
static IWRAM_CODE void decode_block_8x2(DecodeContext *ctx);
static IWRAM_CODE void decode_block_2x8(DecodeContext *ctx);
static IWRAM_CODE void decode_block_2x4(DecodeContext *ctx);
static IWRAM_CODE void decode_block_4x2(DecodeContext *ctx);
static IWRAM_CODE void decode_block_1x8(DecodeContext *ctx);
static IWRAM_CODE void decode_block_8x1(DecodeContext *ctx);
static IWRAM_CODE void decode_block_1x4(DecodeContext *ctx);
GBM_2X2_ATTR void decode_block_2x2(DecodeContext *ctx);
static IWRAM_CODE void decode_block_4x1(DecodeContext *ctx);
GBM_ALWAYS_INLINE void decode_block_1x2(DecodeContext *ctx);
GBM_ALWAYS_INLINE void decode_block_2x1(DecodeContext *ctx);


// Functions
static IWRAM_CODE void decode_block_8x8(DecodeContext *ctx) {
    switch (next_2bits(ctx)) {
    case 0: // 00: copy from same position
        // copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 8, 4); // no-op: VRAM==BUF
        break;
    case 1: // 01: copy with codebook offset
        {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 8, 4);
        }
        break;
    case 2: // 10: subdivide
        if (next_bit(ctx) == 0) {
            decode_block_8x4(ctx);
            decode_block_8x4(ctx);
        } else {
            decode_block_4x8(ctx);
            decode_block_4x8(ctx);
        }
        break;
    case 3: // 11: delta or fill
        if (next_bit(ctx) == 0) {
            u8 code = read_code(ctx);
            s16 color = to_signed16(read_palette_color(ctx));
            delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 8, 4, color);
        } else {
            u16 color = read_palette_color(ctx);
            fill_u32_block(ctx, ctx->block_offset, 8, 4, color);
        }
        break;
    }
}

static IWRAM_CODE void decode_block_8x4(DecodeContext *ctx) {
    switch (next_2bits(ctx)) {
    case 0: // 00: copy from same position
        // copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 4, 4); // no-op: VRAM==BUF
        ctx->block_offset += 0x780;
        break;
    case 1: // 01: copy with codebook offset
        {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 4, 4);
        }
        ctx->block_offset += 0x780;
        break;
    case 2: // 10: subdivide
        if (next_bit(ctx) == 0) {
            decode_block_8x2(ctx);
            decode_block_8x2(ctx);
        } else {
            decode_block_4x4(ctx);
            decode_block_4x4(ctx);
            ctx->block_offset += 0x770;
        }
        break;
    case 3: // 11: delta or fill
        if (next_bit(ctx) == 0) {
            u8 code = read_code(ctx);
            s16 color = to_signed16(read_palette_color(ctx));
            delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 4, 4, color);
        } else {
            u16 color = read_palette_color(ctx);
            fill_u32_block(ctx, ctx->block_offset, 4, 4, color);
        }
        ctx->block_offset += 0x780;
        break;
    }
}

static IWRAM_CODE void decode_block_4x8(DecodeContext *ctx) {
    switch (next_2bits(ctx)) {
    case 0: // 00: copy from same position
        // copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 8, 2); // no-op: VRAM==BUF
        ctx->block_offset += 8;
        break;
    case 1: // 01: copy with codebook offset
        {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 8, 2);
        }
        ctx->block_offset += 8;
        break;
    case 2: // 10: subdivide
        if (next_bit(ctx) == 0) {
            decode_block_4x4(ctx);
            ctx->block_offset += 0x778;
            decode_block_4x4(ctx);
            ctx->block_offset -= 0x780;
        } else {
            decode_block_2x8(ctx);
            decode_block_2x8(ctx);
        }
        break;
    case 3: // 11: delta or fill
        if (next_bit(ctx) == 0) {
            u8 code = read_code(ctx);
            s16 color = to_signed16(read_palette_color(ctx));
            delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 8, 2, color);
        } else {
            u16 color = read_palette_color(ctx);
            fill_u32_block(ctx, ctx->block_offset, 8, 2, color);
        }
        ctx->block_offset += 8;
        break;
    }
}

static IWRAM_CODE void decode_block_2x8(DecodeContext *ctx) {
    switch (next_2bits(ctx)) {
    case 0: // 00: copy from same position
        // copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 8, 1); // no-op: VRAM==BUF
        ctx->block_offset += 4;
        break;
    case 1: // 01: copy with codebook offset
        {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 8, 1);
        }
        ctx->block_offset += 4;
        break;
    case 2: // 10: subdivide
        if (next_bit(ctx) == 0) {
            decode_block_2x4(ctx);
            ctx->block_offset += 0x77C;
            decode_block_2x4(ctx);
            ctx->block_offset -= 0x780;
        } else {
            decode_block_1x8(ctx);
            decode_block_1x8(ctx);
        }
        break;
    case 3: // 11: delta or fill
        if (next_bit(ctx) == 0) {
            u8 code = read_code(ctx);
            s16 color = to_signed16(read_palette_color(ctx));
            delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 8, 1, color);
        } else {
            u16 color = read_palette_color(ctx);
            fill_u32_block(ctx, ctx->block_offset, 8, 1, color);
        }
        ctx->block_offset += 4;
        break;
    }
}

static IWRAM_CODE void decode_block_1x8(DecodeContext *ctx) {
    switch (next_2bits(ctx)) {
    case 0: // 00: copy from same position
        // copy_u16_block(ctx, ctx->block_offset, ctx->block_offset, 8, 1); // no-op: VRAM==BUF
        ctx->block_offset += 2;
        break;
    case 1: // 01: copy with codebook offset
        {
            u8 code = read_code(ctx);
            copy_u16_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 8, 1);
        }
        ctx->block_offset += 2;
        break;
    case 2: // 10: subdivide (only vertical split for 1xN)
        decode_block_1x4(ctx);
        ctx->block_offset += 0x77E;
        decode_block_1x4(ctx);
        ctx->block_offset -= 0x780;
        break;
    case 3: // 11: delta or fill
        if (next_bit(ctx) == 0) {
            u8 code = read_code(ctx);
            s16 color = to_signed16(read_palette_color(ctx));
            delta_u16_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 8, 1, color);
        } else {
            u16 color = read_palette_color(ctx);
            fill_u16_block(ctx, ctx->block_offset, 8, 1, color);
        }
        ctx->block_offset += 2;
        break;
    }
}

GBM_ALWAYS_INLINE void decode_block_4x4(DecodeContext *ctx) {
    switch (next_2bits(ctx)) {
    case 0: // 00: copy from same position
        // copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 4, 2); // no-op: VRAM==BUF
        ctx->block_offset += 8;
        break;
    case 1: // 01: copy with codebook offset
        {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 4, 2);
        }
        ctx->block_offset += 8;
        break;
    case 2: // 10: subdivide
        if (next_bit(ctx) == 0) {
            decode_block_4x2(ctx);
            ctx->block_offset += 0x3B8;
            decode_block_4x2(ctx);
            ctx->block_offset -= 0x3C0;
        } else {
            decode_block_2x4(ctx);
            decode_block_2x4(ctx);
        }
        break;
    case 3: // 11: delta or fill
        if (next_bit(ctx) == 0) {
            u8 code = read_code(ctx);
            s16 color = to_signed16(read_palette_color(ctx));
            delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 4, 2, color);
        } else {
            u16 color = read_palette_color(ctx);
            fill_u32_block(ctx, ctx->block_offset, 4, 2, color);
        }
        ctx->block_offset += 8;
        break;
    }
}

static IWRAM_CODE void decode_block_8x2(DecodeContext *ctx) {
    switch (next_2bits(ctx)) {
    case 0: // 00: copy from same position
        // copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 2, 4); // no-op: VRAM==BUF
        ctx->block_offset += 0x3C0;
        break;
    case 1: // 01: copy with codebook offset
        {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 2, 4);
        }
        ctx->block_offset += 0x3C0;
        break;
    case 2: // 10: subdivide
        if (next_bit(ctx) == 0) {
            decode_block_8x1(ctx);
            decode_block_8x1(ctx);
        } else {
            decode_block_4x2(ctx);
            decode_block_4x2(ctx);
            ctx->block_offset += 0x3B0;
        }
        break;
    case 3: // 11: delta or fill
        if (next_bit(ctx) == 0) {
            u8 code = read_code(ctx);
            s16 color = to_signed16(read_palette_color(ctx));
            delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 2, 4, color);
        } else {
            u16 color = read_palette_color(ctx);
            fill_u32_block(ctx, ctx->block_offset, 2, 4, color);
        }
        ctx->block_offset += 0x3C0;
        break;
    }
}

static IWRAM_CODE void decode_block_2x4(DecodeContext *ctx) {
    switch (next_2bits(ctx)) {
    case 0: // 00: copy from same position
        // copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 4, 1); // no-op: VRAM==BUF
        ctx->block_offset += 4;
        break;
    case 1: // 01: copy with codebook offset
        {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 4, 1);
        }
        ctx->block_offset += 4;
        break;
    case 2: // 10: subdivide
        if (next_bit(ctx) == 0) {
            decode_block_2x2(ctx);
            ctx->block_offset += 0x3BC;
            decode_block_2x2(ctx);
            ctx->block_offset -= 0x3C0;
        } else {
            decode_block_1x4(ctx);
            decode_block_1x4(ctx);
        }
        break;
    case 3: // 11: delta or fill
        if (next_bit(ctx) == 0) {
            u8 code = read_code(ctx);
            s16 color = to_signed16(read_palette_color(ctx));
            delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 4, 1, color);
        } else {
            u16 color = read_palette_color(ctx);
            fill_u32_block(ctx, ctx->block_offset, 4, 1, color);
        }
        ctx->block_offset += 4;
        break;
    }
}

static IWRAM_CODE void decode_block_4x2(DecodeContext *ctx) {
    switch (next_2bits(ctx)) {
    case 0: // 00: copy from same position
        // copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 2, 2); // no-op: VRAM==BUF
        ctx->block_offset += 8;
        break;
    case 1: // 01: copy with codebook offset
        {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 2, 2);
        }
        ctx->block_offset += 8;
        break;
    case 2: // 10: subdivide
        if (next_bit(ctx) == 0) {
            decode_block_4x1(ctx);
            ctx->block_offset += 0x1D8;
            decode_block_4x1(ctx);
            ctx->block_offset -= 0x1E0;
        } else {
            decode_block_2x2(ctx);
            decode_block_2x2(ctx);
        }
        break;
    case 3: // 11: delta or fill
        if (next_bit(ctx) == 0) {
            u8 code = read_code(ctx);
            s16 color = to_signed16(read_palette_color(ctx));
            delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 2, 2, color);
        } else {
            u16 color = read_palette_color(ctx);
            fill_u32_block(ctx, ctx->block_offset, 2, 2, color);
        }
        ctx->block_offset += 8;
        break;
    }
}

static IWRAM_CODE void decode_block_8x1(DecodeContext *ctx) {
    switch (next_2bits(ctx)) {
    case 0: // 00: copy from same position
        // copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 1, 4); // no-op: VRAM==BUF
        ctx->block_offset += 0x1E0;
        break;
    case 1: // 01: copy with codebook offset
        {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 1, 4);
        }
        ctx->block_offset += 0x1E0;
        break;
    case 2: // 10: subdivide (only horizontal split for Nx1)
        decode_block_4x1(ctx);
        decode_block_4x1(ctx);
        ctx->block_offset += 0x1D0;
        break;
    case 3: // 11: delta or fill
        if (next_bit(ctx) == 0) {
            u8 code = read_code(ctx);
            s16 color = to_signed16(read_palette_color(ctx));
            delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 1, 4, color);
        } else {
            u16 color = read_palette_color(ctx);
            fill_u32_block(ctx, ctx->block_offset, 1, 4, color);
        }
        ctx->block_offset += 0x1E0;
        break;
    }
}

static IWRAM_CODE void decode_block_1x4(DecodeContext *ctx) {
    switch (next_2bits(ctx)) {
    case 0: // 00: copy from same position
        // copy_u16_block(ctx, ctx->block_offset, ctx->block_offset, 4, 1); // no-op: VRAM==BUF
        ctx->block_offset += 2;
        break;
    case 1: // 01: copy with codebook offset
        {
            u8 code = read_code(ctx);
            copy_u16_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 4, 1);
        }
        ctx->block_offset += 2;
        break;
    case 2: // 10: subdivide (only vertical split for 1xN)
        decode_block_1x2(ctx);
        ctx->block_offset += 0x3BE;
        decode_block_1x2(ctx);
        ctx->block_offset -= 0x3C0;
        break;
    case 3: // 11: delta or fill
        if (next_bit(ctx) == 0) {
            u8 code = read_code(ctx);
            s16 color = to_signed16(read_palette_color(ctx));
            delta_u16_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 4, 1, color);
        } else {
            u16 color = read_palette_color(ctx);
            fill_u16_block(ctx, ctx->block_offset, 4, 1, color);
        }
        ctx->block_offset += 2;
        break;
    }
}

GBM_2X2_ATTR void decode_block_2x2(DecodeContext *ctx) {
    switch (next_2bits(ctx)) {
    case 0: // 00: copy from same position
        // copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 2, 1); // no-op: VRAM==BUF
        ctx->block_offset += 4;
        break;
    case 1: // 01: copy with codebook offset
        {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 2, 1);
        }
        ctx->block_offset += 4;
        break;
    case 2: // 10: subdivide
        if (next_bit(ctx) == 0) {
            decode_block_2x1(ctx);
            ctx->block_offset += 0x1DC;
            decode_block_2x1(ctx);
            ctx->block_offset -= 0x1E0;
        } else {
            decode_block_1x2(ctx);
            decode_block_1x2(ctx);
        }
        break;
    case 3: // 11: delta or fill
        if (next_bit(ctx) == 0) {
            u8 code = read_code(ctx);
            s16 color = to_signed16(read_palette_color(ctx));
            delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 2, 1, color);
        } else {
            u16 color = read_palette_color(ctx);
            fill_u32_block(ctx, ctx->block_offset, 2, 1, color);
        }
        ctx->block_offset += 4;
        break;
    }
}

static IWRAM_CODE void decode_block_4x1(DecodeContext *ctx) {
    switch (next_2bits(ctx)) {
    case 0: // 00: copy from same position
        // copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 1, 2); // no-op: VRAM==BUF
        ctx->block_offset += 8;
        break;
    case 1: // 01: copy with codebook offset
        {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 1, 2);
        }
        ctx->block_offset += 8;
        break;
    case 2: // 10: subdivide (only horizontal split for Nx1)
        decode_block_2x1(ctx);
        decode_block_2x1(ctx);
        break;
    case 3: // 11: delta or fill
        if (next_bit(ctx) == 0) {
            u8 code = read_code(ctx);
            s16 color = to_signed16(read_palette_color(ctx));
            delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 1, 2, color);
        } else {
            u16 color = read_palette_color(ctx);
            fill_u32_block(ctx, ctx->block_offset, 1, 2, color);
        }
        ctx->block_offset += 8;
        break;
    }
}

GBM_ALWAYS_INLINE void decode_block_1x2(DecodeContext *ctx) {
    switch (next_2bits(ctx)) {
    case 0: // 00: copy from same position
        // copy_u16_block(ctx, ctx->block_offset, ctx->block_offset, 2, 1); // no-op: VRAM==BUF
        ctx->block_offset += 2;
        break;
    case 1: // 01: copy with codebook offset
        {
            u8 code = read_code(ctx);
            copy_u16_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 2, 1);
        }
        ctx->block_offset += 2;
        break;
    case 2: // 10: delta
        {
            u8 code = read_code(ctx);
            s16 color = to_signed16(read_palette_color(ctx));
            delta_u16_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 2, 1, color);
        }
        ctx->block_offset += 2;
        break;
    case 3: // 11: fill (same or two colors)
        if (next_bit(ctx) == 0) {
            u16 color0 = read_palette_color(ctx);
            fill_u16_block(ctx, ctx->block_offset, 2, 1, color0);
        } else {
            u16 color0 = read_palette_color(ctx);
            u16 color1 = read_palette_color(ctx);
            ctx->dst[ctx->block_offset >> 1] = color0;
            ctx->dst[(ctx->block_offset + ROW_BYTES) >> 1] = color1;
        }
        ctx->block_offset += 2;
        break;
    }
}

GBM_ALWAYS_INLINE void decode_block_2x1(DecodeContext *ctx) {
    switch (next_2bits(ctx)) {
    case 0: // 00: copy from same position
        // copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 1, 1); // no-op: VRAM==BUF
        ctx->block_offset += 4;
        break;
    case 1: // 01: copy with codebook offset
        {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 1, 1);
        }
        ctx->block_offset += 4;
        break;
    case 2: // 10: delta
        {
            u8 code = read_code(ctx);
            s16 color = to_signed16(read_palette_color(ctx));
            delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 1, 1, color);
        }
        ctx->block_offset += 4;
        break;
    case 3: // 11: fill (same or two colors)
        if (next_bit(ctx) == 0) {
            u16 color0 = read_palette_color(ctx);
            // Fill 2 pixels width with same color
            ctx->dst[ctx->block_offset >> 1] = color0;
            ctx->dst[(ctx->block_offset >> 1) + 1] = color0;
        } else {
            u16 color0 = read_palette_color(ctx);
            u16 color1 = read_palette_color(ctx);
            ctx->dst[ctx->block_offset >> 1] = color0;
            ctx->dst[(ctx->block_offset >> 1) + 1] = color1;
        }
        ctx->block_offset += 4;
        break;
    }
}

// Also put the main decoder loop in IWRAM for good measure?
// It calls many IWRAM functions, so it's less critical, but looping overhead is reduced.
static u32 IWRAM_CODE gbm_decode_frame_internal(const u8 *data, u32 offset, u16 *dst, const u16 *ref,
                                                u32 frame_index, int obfuscated) {
    u16 frame_len = read_u16_unaligned(data + offset);
    u16 bit_enc = read_u16_unaligned(data + offset + 2);
    u16 palette_bytes = read_u16_unaligned(data + offset + 4);

    if (obfuscated) {
        frame_len ^= frame_field_key(frame_index, 0x101u);
        bit_enc ^= frame_field_key(frame_index, 0x202u);
        palette_bytes ^= frame_field_key(frame_index, 0x303u);
    }

    u32 next_offset = offset + 2 + frame_len;

    u16 flag_bytes = bit_enc ^ xor_key;
    trace_reset_frame_stats();

    DecodeContext ctx;
    ctx.state = 0x80000000; // Initial state
    
    u32 flag_start = offset + 6;
    u32 flag_end = flag_start + flag_bytes;
    u32 pal_start = flag_end;
    u32 pal_end = pal_start + palette_bytes;
    
    ctx.flag_ptr = data + flag_start;
    ctx.palette_ptr = data + pal_start;
    ctx.payload_ptr = data + pal_end;
    
    ctx.dst = dst;
    // If ref is null, use dst (intra prediction behavior)
    ctx.ref = ref ? ref : dst;

    // Decode loop
    for (int y_block = 0; y_block < 20; y_block++) {
        ctx.row_offset = y_block * 8 * ROW_BYTES;
        ctx.block_offset = ctx.row_offset;
        for (int x_block = 0; x_block < 30; x_block++) {
            ctx.block_offset = ctx.row_offset + x_block * 8 * 2; // 2 bytes per pixel
            decode_block_8x8(&ctx);
        }
    }

    trace_emit_frame_stats(flag_bytes, palette_bytes, next_offset - pal_end);

    return next_offset;
}

u32 IWRAM_CODE gbm_decode_frame(const u8 *data, u32 offset, u16 *dst, const u16 *ref) {
    return gbm_decode_frame_internal(data, offset, dst, ref, 0, 0);
}

u32 IWRAM_CODE gbm_decode_frame_obfuscated(const u8 *data, u32 offset, u16 *dst, const u16 *ref,
                                           u32 frame_index) {
    return gbm_decode_frame_internal(data, offset, dst, ref, frame_index, 1);
}
