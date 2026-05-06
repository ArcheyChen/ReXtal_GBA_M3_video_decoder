#include "banked_video.h"

#include <gba_base.h>
#include <string.h>

#define MAPPER_CONFIG1 ((volatile u8*) 0x0E000002)
#define MAPPER_CONFIG2 ((volatile u8*) 0x0E000003)
#define CART_BASE ((const u8*) 0x08000000)

#define INDEX_CACHE_FRAMES 256u

EWRAM_BSS static M3VHeader active_header;
static bool active;
static u32 current_bank = 0xffffffffu;
static u32 index_cache_start = 0xffffffffu;
static u32 index_cache_count;
EWRAM_BSS static u32 index_cache[INDEX_CACHE_FRAMES];

static u32 obfuscation_mix(u32 value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

static u32 index_key(u32 frame_index) {
    return obfuscation_mix(0x4d335649u ^ frame_index);
}

static u32 minute_key(u32 minute) {
    return obfuscation_mix(0x7258744du ^ minute);
}

static u32 checked_add_u32(u32 a, u32 b, bool* ok) {
    if (a > 0xffffffffu - b) {
        *ok = false;
        return 0;
    }
    return a + b;
}

static void banked_wait(void) {
    for (volatile int i = 0; i < 64; ++i) {
        __asm__ volatile("nop");
    }
}

static void banked_init_mapper(void) {
    current_bank = 0;
    *MAPPER_CONFIG1 = 0;
    *MAPPER_CONFIG2 = 0x40;
    banked_wait();
}

static void banked_select_raw(u32 bank) {
    bank &= 7u;
    if (bank == current_bank) {
        return;
    }
    current_bank = bank;
    *MAPPER_CONFIG1 = (u8) (bank << 4);
    banked_wait();
}

u32 banked_current_bank(void) {
    return current_bank;
}

void banked_select(u32 bank) {
    banked_select_raw(bank);
}

void banked_select_irq(u32 bank) {
    banked_select_raw(bank);
}

const u8* banked_rom_ptr(u32 rom_offset) {
    banked_select(rom_offset >> 25);
    return CART_BASE + (rom_offset & M3V_WINDOW_MASK);
}

const u8* banked_rom_ptr_irq(u32 rom_offset) {
    banked_select_irq(rom_offset >> 25);
    return CART_BASE + (rom_offset & M3V_WINDOW_MASK);
}

void banked_copy(u32 rom_offset, void* dst, u32 size) {
    u8* out = (u8*) dst;
    while (size > 0) {
        const u32 bank = rom_offset >> 25;
        const u32 in_bank = rom_offset & M3V_WINDOW_MASK;
        u32 chunk = M3V_WINDOW_SIZE - in_bank;
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
    bool ok = true;
    if (memcmp(header->magic, "M3V0", 4) != 0) {
        return false;
    }
    if (header->version != 1 || header->header_size != sizeof(M3VHeader)) {
        return false;
    }
    if ((header->flags & ~(M3V_FLAG_METADATA_OBFUSCATED | M3V_FLAG_COMPACT_FRAMES)) != 0) {
        return false;
    }
    if (header->fps == 0 || header->frame_count == 0) {
        return false;
    }
    if (header->frame_index_offset < M3V_HEADER_ROM_OFFSET + sizeof(M3VHeader)) {
        return false;
    }
    const u32 frame_index_bytes = header->frame_count * 4u;
    if ((frame_index_bytes / 4u) != header->frame_count) {
        return false;
    }
    const u32 frame_index_end =
        checked_add_u32(header->frame_index_offset, frame_index_bytes, &ok);
    if (!ok || header->minute_index_offset < frame_index_end) {
        return false;
    }
    const u32 minute_index_bytes = header->minute_count * 4u;
    if ((minute_index_bytes / 4u) != header->minute_count) {
        return false;
    }
    const u32 minute_index_end =
        checked_add_u32(header->minute_index_offset, minute_index_bytes, &ok);
    if (!ok || header->video_data_start < minute_index_end ||
        header->video_data_end <= header->video_data_start) {
        return false;
    }
    if (header->rom_size < header->video_data_end) {
        return false;
    }
    if (header->audio_size != 0) {
        const u32 audio_header_end = checked_add_u32(header->audio_header_offset, 0x200u, &ok);
        if (header->audio_header_offset < header->video_data_end ||
            !ok ||
            header->audio_block_offset < audio_header_end ||
            header->audio_data_end <= header->audio_block_offset ||
            header->rom_size < header->audio_data_end) {
            return false;
        }
    }
    return true;
}

bool banked_video_init(void) {
    banked_init_mapper();
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

bool banked_video_has_audio(void) {
    return active && active_header.audio_size != 0;
}

bool banked_video_is_metadata_obfuscated(void) {
    return active && (active_header.flags & M3V_FLAG_METADATA_OBFUSCATED) != 0;
}

bool banked_video_uses_compact_frames(void) {
    return active && (active_header.flags & M3V_FLAG_COMPACT_FRAMES) != 0;
}

u32 banked_video_audio_header_offset(void) {
    return active ? active_header.audio_header_offset : 0;
}

u32 banked_video_audio_block_offset(void) {
    return active ? active_header.audio_block_offset : 0;
}

u32 banked_video_audio_size(void) {
    return active ? active_header.audio_size : 0;
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
    if (banked_video_is_metadata_obfuscated()) {
        for (u32 i = 0; i < count; ++i) {
            index_cache[i] ^= index_key(start + i);
        }
    }
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
    return banked_rom_ptr(offset);
}

u32 banked_video_minute_frame(u32 minute) {
    if (!active || minute >= active_header.minute_count) {
        return 0;
    }
    u32 frame = 0;
    banked_copy(active_header.minute_index_offset + minute * 4u, &frame, sizeof(frame));
    if (banked_video_is_metadata_obfuscated()) {
        frame ^= minute_key(minute);
    }
    if (frame >= active_header.frame_count) {
        frame = active_header.frame_count - 1u;
    }
    return frame;
}
