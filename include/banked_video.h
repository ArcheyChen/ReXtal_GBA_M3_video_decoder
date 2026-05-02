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
    u32 reserved[19];
} M3VHeader;

bool banked_video_init(void);
bool banked_video_is_active(void);

u32 banked_video_frame_count(void);
u32 banked_video_minute_count(void);
u32 banked_video_fps(void);
u32 banked_video_total_size(void);
u8 banked_video_gbm_version(void);

const u8* banked_video_frame_ptr(u32 frame_index);
u32 banked_video_minute_frame(u32 minute);

void banked_select(u32 bank);
void banked_copy(u32 rom_offset, void* dst, u32 size);

#endif
