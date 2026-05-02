#include "banked_video.h"

#include <string.h>

#define MAPPER_CONFIG1 ((volatile u8*) 0x0E000002)
#define MAPPER_CONFIG2 ((volatile u8*) 0x0E000003)
#define CART_BASE ((const u8*) 0x08000000)

#define INDEX_CACHE_FRAMES 256u

static M3VHeader active_header;
static bool active;
static u32 current_bank = 0xffffffffu;
static u32 index_cache_start = 0xffffffffu;
static u32 index_cache_count;
static u32 index_cache[INDEX_CACHE_FRAMES];

static void banked_wait(void) {
    for (volatile int i = 0; i < 64; ++i) {
        __asm__ volatile("nop");
    }
}

void banked_select(u32 bank) {
    bank &= 63u;
    if (bank == current_bank) {
        return;
    }
    *MAPPER_CONFIG1 = (u8) (((bank >> 3) & 7u) << 4);
    *MAPPER_CONFIG2 = (u8) (0x40u + ((bank & 7u) << 3));
    current_bank = bank;
    banked_wait();
}

void banked_copy(u32 rom_offset, void* dst, u32 size) {
    u8* out = (u8*) dst;
    while (size > 0) {
        const u32 bank = rom_offset >> 22;
        const u32 in_bank = rom_offset & M3V_BANK_MASK;
        u32 chunk = M3V_BANK_SIZE - in_bank;
        if (chunk > size) {
            chunk = size;
        }
        banked_select(bank);
        memcpy(out, CART_BASE + in_bank, chunk);
        out += chunk;
        rom_offset += chunk;
        size -= chunk;
    }
}

static bool header_is_valid(const M3VHeader* header) {
    if (memcmp(header->magic, "M3V0", 4) != 0) {
        return false;
    }
    if (header->version != 1 || header->header_size != sizeof(M3VHeader)) {
        return false;
    }
    if (header->fps == 0 || header->frame_count == 0) {
        return false;
    }
    if (header->frame_index_offset < M3V_HEADER_ROM_OFFSET + sizeof(M3VHeader)) {
        return false;
    }
    if (header->minute_index_offset < header->frame_index_offset + header->frame_count * 4u) {
        return false;
    }
    if (header->video_data_start < M3V_BANK_SIZE || header->video_data_end <= header->video_data_start) {
        return false;
    }
    if (header->rom_size < header->video_data_end) {
        return false;
    }
    return true;
}

bool banked_video_init(void) {
    banked_select(0);
    const M3VHeader* header = (const M3VHeader*) (CART_BASE + M3V_HEADER_ROM_OFFSET);
    if (!header_is_valid(header)) {
        active = false;
        return false;
    }
    memcpy(&active_header, header, sizeof(active_header));
    active = true;
    index_cache_start = 0xffffffffu;
    index_cache_count = 0;
    return true;
}

bool banked_video_is_active(void) {
    return active;
}

u32 banked_video_frame_count(void) {
    return active ? active_header.frame_count : 0;
}

u32 banked_video_minute_count(void) {
    return active ? active_header.minute_count : 0;
}

u32 banked_video_fps(void) {
    return active ? active_header.fps : 0;
}

u32 banked_video_total_size(void) {
    return active ? active_header.video_data_end - active_header.video_data_start : 0;
}

u8 banked_video_gbm_version(void) {
    return active ? (u8) active_header.gbm_version : 0;
}

static bool load_index_cache(u32 frame_index) {
    if (!active || frame_index >= active_header.frame_count) {
        return false;
    }
    const u32 start = frame_index & ~(INDEX_CACHE_FRAMES - 1u);
    u32 count = active_header.frame_count - start;
    if (count > INDEX_CACHE_FRAMES) {
        count = INDEX_CACHE_FRAMES;
    }
    banked_copy(active_header.frame_index_offset + start * 4u, index_cache, count * 4u);
    index_cache_start = start;
    index_cache_count = count;
    return true;
}

static bool get_frame_offset(u32 frame_index, u32* out_offset) {
    if (!active || frame_index >= active_header.frame_count) {
        return false;
    }
    if (frame_index < index_cache_start ||
        frame_index >= index_cache_start + index_cache_count) {
        if (!load_index_cache(frame_index)) {
            return false;
        }
    }
    *out_offset = index_cache[frame_index - index_cache_start];
    return true;
}

const u8* banked_video_frame_ptr(u32 frame_index) {
    u32 offset = 0;
    if (!get_frame_offset(frame_index, &offset)) {
        return 0;
    }
    banked_select(offset >> 22);
    return CART_BASE + (offset & M3V_BANK_MASK);
}

u32 banked_video_minute_frame(u32 minute) {
    if (!active || minute >= active_header.minute_count) {
        return 0;
    }
    u32 frame = 0;
    banked_copy(active_header.minute_index_offset + minute * 4u, &frame, sizeof(frame));
    if (frame >= active_header.frame_count) {
        frame = active_header.frame_count - 1u;
    }
    return frame;
}
