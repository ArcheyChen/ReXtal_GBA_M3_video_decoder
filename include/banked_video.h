#ifndef BANKED_VIDEO_H
#define BANKED_VIDEO_H

#include <gba_types.h>
#include <stdbool.h>

#define M3V_HEADER_ROM_OFFSET 0x00100000u
#define M3V_BANK_SIZE 0x00400000u
#define M3V_BANK_MASK (M3V_BANK_SIZE - 1u)

typedef struct __attribute__((packed)) {
    char magic[4];
    u32 version;
    u32 header_size;
    u32 flags;
    u32 fps;
    u32 frame_count;
    u32 minute_count;
    u32 gbm_version;
    u32 frame_index_offset;
    u32 minute_index_offset;
    u32 video_data_start;
    u32 video_data_end;
    u32 rom_size;
    u32 audio_header_offset;
    u32 audio_block_offset;
    u32 audio_size;
    u32 audio_data_end;
    u32 reserved[15];
} M3VHeader;

bool banked_video_init(void);
bool banked_video_is_active(void);

u32 banked_video_frame_count(void);
u32 banked_video_minute_count(void);
u32 banked_video_fps(void);
u32 banked_video_total_size(void);
u8 banked_video_gbm_version(void);
bool banked_video_has_audio(void);
u32 banked_video_audio_header_offset(void);
u32 banked_video_audio_block_offset(void);
u32 banked_video_audio_size(void);

const u8* banked_video_frame_ptr(u32 frame_index);
u32 banked_video_minute_frame(u32 minute);

u32 banked_current_bank(void);
void banked_select(u32 bank);
void banked_select_irq(u32 bank);
const u8* banked_rom_ptr(u32 rom_offset);
const u8* banked_rom_ptr_irq(u32 rom_offset);
void banked_copy(u32 rom_offset, void* dst, u32 size);

#endif
