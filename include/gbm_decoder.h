#ifndef GBM_DECODER_H
#define GBM_DECODER_H

#include <gba_types.h>

#define FRAME_WIDTH 240
#define FRAME_HEIGHT 160
#define GBM_HEADER_SIZE 0x200

// GBM format versions
#define GBM_VERSION_GEN1 0x06  // XOR key 0xD669
#define GBM_VERSION_GEN3 0x05  // XOR key 0xD6AC
#define GBM_VERSION_V130 0x04  // No XOR (key 0x0000)
#define GBM_VERSION_SC   0x01  // SuperCard/FilmPlay clone, no XOR

#define IWRAM_CODE __attribute__((section(".iwram"), long_call))

// Context for decoding a single frame
typedef struct {
    u32 state;
    const u8 *flag_ptr; // Current position in flag stream (must be 4-byte aligned reads)

    const u8 *palette_ptr; // Current position in palette stream

    const u8 *payload_ptr; // Current position in payload stream

    u16 *dst;       // Destination buffer (current frame)
    const u16 *ref; // Reference buffer (previous frame)

    int row_offset;   // Current macroblock row offset in bytes
    int block_offset; // Current block offset in bytes
} DecodeContext;

// Set XOR key based on GBM version (call once after loading GBM header)
// version: 0x06 for Gen1, 0x05 for Gen3, 0x04/0x01 for no-XOR variants
void gbm_set_version(u8 version);

// Initialize and decode a frame
// returns the offset of the next frame, or 0 on error
u32 gbm_decode_frame(const u8 *data, u32 offset, u16 *dst, const u16 *ref);
u32 gbm_decode_frame_obfuscated(const u8 *data, u32 offset, u16 *dst, const u16 *ref, u32 frame_index);

#endif // GBM_DECODER_H
