/*
 * Multibank video packager.
 *
 * Usage:
 *   mb_video_packager output.gba M3_Movie_Player_mb.gba movie.gbm [movie.gbs] [rom_mb]
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M3V_HEADER_ROM_OFFSET 0x00040000u
#define M3V_MEDIA_ALIGNMENT 0x00001000u
#define M3V_WINDOW_SIZE 0x02000000u
#define M3V_WINDOW_MASK (M3V_WINDOW_SIZE - 1u)
#define M3V_DEFAULT_ROM_MB 256u
#define M3V_FLAG_COMPACT_FRAMES 0x00000002u
#define GBM_HEADER_SIZE 0x200u
#define GBS_HEADER_SIZE 0x200u
#define FRAMES_PER_MINUTE 600u

typedef struct {
    char magic[4];
    uint32_t version;
    uint32_t header_size;
    uint32_t flags;
    uint32_t fps;
    uint32_t frame_count;
    uint32_t minute_count;
    uint32_t gbm_version;
    uint32_t frame_index_offset;
    uint32_t minute_index_offset;
    uint32_t video_data_start;
    uint32_t video_data_end;
    uint32_t rom_size;
    uint32_t audio_header_offset;
    uint32_t audio_block_offset;
    uint32_t audio_size;
    uint32_t audio_data_end;
    uint32_t reserved[15];
} __attribute__((packed)) M3VHeader;

static uint32_t align4(uint32_t value) {
    return (value + 3u) & ~3u;
}

static uint8_t* read_file(const char* path, uint32_t* out_size) {
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    uint8_t* data = malloc((size_t) size);
    if (!data) {
        fclose(fp);
        return NULL;
    }
    if (size > 0 && fread(data, 1, (size_t) size, fp) != (size_t) size) {
        free(data);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *out_size = (uint32_t) size;
    return data;
}

static uint16_t read_le16(const uint8_t* data) {
    return (uint16_t) (data[0] | (data[1] << 8));
}

static uint32_t read_le32(const uint8_t* data) {
    return (uint32_t) data[0] |
           ((uint32_t) data[1] << 8) |
           ((uint32_t) data[2] << 16) |
           ((uint32_t) data[3] << 24);
}

static uint32_t align_to(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static uint32_t minute_count_for_frames(uint32_t frame_count) {
    return frame_count == 0 ? 0 : (frame_count + FRAMES_PER_MINUTE - 1u) / FRAMES_PER_MINUTE;
}

static uint32_t metadata_end_for_frames(uint32_t frame_count) {
    const uint32_t frame_index_offset = align4(M3V_HEADER_ROM_OFFSET + sizeof(M3VHeader));
    const uint32_t minute_index_offset = align4(frame_index_offset + frame_count * 4u);
    return align4(minute_index_offset + minute_count_for_frames(frame_count) * 4u);
}

static uint32_t video_start_for_frames(uint32_t frame_count) {
    return align_to(metadata_end_for_frames(frame_count), M3V_MEDIA_ALIGNMENT);
}

static uint32_t advance_window_bounded(uint32_t offset, uint32_t size) {
    if ((offset & M3V_WINDOW_MASK) + size > M3V_WINDOW_SIZE) {
        offset = (offset + M3V_WINDOW_MASK) & ~M3V_WINDOW_MASK;
    }
    return offset;
}

static uint32_t gbs_block_size(const uint8_t* gbs, uint32_t gbs_size) {
    if (gbs_size < GBS_HEADER_SIZE || memcmp(gbs, "GBAL", 4) != 0 ||
        memcmp(gbs + 8, "MUSI", 4) != 0) {
        return 0;
    }
    const uint32_t mode = read_le32(gbs + 0x10);
    switch (mode) {
        case 0:
        case 1:
            return 0x400u;
        case 2:
        case 3:
            return 0x200u;
        case 4:
            return 0x100u;
        default:
            return 0;
    }
}

static int append_u32(uint32_t** data, uint32_t* count, uint32_t* capacity, uint32_t value) {
    if (*count >= *capacity) {
        uint32_t new_capacity = *capacity ? *capacity * 2u : 1024u;
        uint32_t* resized = realloc(*data, (size_t) new_capacity * sizeof(uint32_t));
        if (!resized) {
            return -1;
        }
        *data = resized;
        *capacity = new_capacity;
    }
    (*data)[(*count)++] = value;
    return 0;
}

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s output.gba M3_Movie_Player_mb.gba movie.gbm [movie.gbs] [rom_mb]\n", prog);
}

int main(int argc, char** argv) {
    if (argc < 4 || argc > 6) {
        print_usage(argv[0]);
        return 1;
    }

    const char* out_path = argv[1];
    const char* player_path = argv[2];
    const char* gbm_path = argv[3];
    const char* gbs_path = NULL;
    uint32_t rom_mb = M3V_DEFAULT_ROM_MB;
    if (argc == 5) {
        char* end = NULL;
        const uint32_t value = (uint32_t) strtoul(argv[4], &end, 0);
        if (end && *end == '\0') {
            rom_mb = value;
        } else {
            gbs_path = argv[4];
        }
    } else if (argc == 6) {
        gbs_path = argv[4];
        rom_mb = (uint32_t) strtoul(argv[5], NULL, 0);
    }
    if (rom_mb < 8 || rom_mb > 256 || (rom_mb % 4) != 0) {
        fprintf(stderr, "Error: rom_mb must be 8..256 and divisible by 4\n");
        return 1;
    }
    const uint32_t rom_size = rom_mb * 1024u * 1024u;

    uint32_t player_size = 0;
    uint8_t* player = read_file(player_path, &player_size);
    if (!player) {
        fprintf(stderr, "Error: failed to read player: %s\n", player_path);
        return 1;
    }
    if (player_size > M3V_HEADER_ROM_OFFSET) {
        fprintf(stderr, "Error: player is too large for fixed M3V header offset\n");
        free(player);
        return 1;
    }

    uint32_t gbm_size = 0;
    uint8_t* gbm = read_file(gbm_path, &gbm_size);
    if (!gbm) {
        fprintf(stderr, "Error: failed to read GBM: %s\n", gbm_path);
        free(player);
        return 1;
    }
    if (gbm_size < GBM_HEADER_SIZE || memcmp(gbm, "GBAM", 4) != 0) {
        fprintf(stderr, "Error: invalid GBM file\n");
        free(player);
        free(gbm);
        return 1;
    }

    uint32_t gbs_size = 0;
    uint8_t* gbs = NULL;
    uint32_t audio_block_size = 0;
    if (gbs_path) {
        gbs = read_file(gbs_path, &gbs_size);
        if (!gbs) {
            fprintf(stderr, "Error: failed to read GBS: %s\n", gbs_path);
            free(player);
            free(gbm);
            return 1;
        }
        audio_block_size = gbs_block_size(gbs, gbs_size);
        if (audio_block_size == 0) {
            fprintf(stderr, "Error: invalid GBS file\n");
            free(player);
            free(gbm);
            free(gbs);
            return 1;
        }
    }

    uint8_t* rom = malloc(rom_size);
    if (!rom) {
        fprintf(stderr, "Error: failed to allocate ROM image\n");
        free(player);
        free(gbm);
        free(gbs);
        return 1;
    }
    memset(rom, 0xFF, rom_size);
    memcpy(rom, player, player_size);

    uint32_t* frame_offsets = NULL;
    uint32_t frame_count = 0;
    uint32_t frame_capacity = 0;
    uint32_t* minute_frames = NULL;
    uint32_t minute_count = 0;
    uint32_t minute_capacity = 0;
    uint32_t* frame_src_offsets = NULL;
    uint32_t frame_src_count = 0;
    uint32_t frame_src_capacity = 0;
    uint32_t* frame_sizes = NULL;
    uint32_t frame_size_count = 0;
    uint32_t frame_size_capacity = 0;

    uint32_t gbm_offset = GBM_HEADER_SIZE;
    while (gbm_offset + 2u <= gbm_size) {
        const uint16_t frame_len = read_le16(gbm + gbm_offset);
        if (frame_len == 0 || frame_len == 0xFFFF) {
            break;
        }
        const uint32_t frame_size = frame_len;
        const uint32_t gbm_record_size = 2u + frame_len;
        if (gbm_offset + gbm_record_size > gbm_size) {
            fprintf(stderr, "Error: truncated GBM frame at 0x%X\n", gbm_offset);
            free(player);
            free(gbm);
            free(rom);
            free(frame_offsets);
            free(minute_frames);
            free(frame_src_offsets);
            free(frame_sizes);
            return 1;
        }
        if (frame_size > M3V_WINDOW_SIZE) {
            fprintf(stderr, "Error: frame too large for one ROM window\n");
            free(player);
            free(gbm);
            free(rom);
            free(frame_offsets);
            free(minute_frames);
            free(frame_src_offsets);
            free(frame_sizes);
            return 1;
        }
        if ((frame_size_count % FRAMES_PER_MINUTE) == 0) {
            if (append_u32(&minute_frames, &minute_count, &minute_capacity, frame_size_count) != 0) {
                fprintf(stderr, "Error: out of memory\n");
                free(player);
                free(gbm);
                free(rom);
                free(frame_offsets);
                free(minute_frames);
                free(frame_src_offsets);
                free(frame_sizes);
                return 1;
            }
        }
        if (append_u32(&frame_src_offsets, &frame_src_count, &frame_src_capacity, gbm_offset + 2u) != 0 ||
            append_u32(&frame_sizes, &frame_size_count, &frame_size_capacity, frame_size) != 0) {
            fprintf(stderr, "Error: out of memory\n");
            free(player);
            free(gbm);
            free(rom);
            free(frame_offsets);
            free(minute_frames);
            free(frame_src_offsets);
            free(frame_sizes);
            return 1;
        }
        gbm_offset += gbm_record_size;
    }

    if (frame_size_count == 0) {
        fprintf(stderr, "Error: no frames found in GBM\n");
        free(player);
        free(gbm);
        free(gbs);
        free(rom);
        free(frame_offsets);
        free(minute_frames);
        free(frame_src_offsets);
        free(frame_sizes);
        return 1;
    }

    const uint32_t video_data_start = video_start_for_frames(frame_size_count);
    uint32_t rom_offset = video_data_start;
    for (uint32_t i = 0; i < frame_size_count; ++i) {
        const uint32_t frame_size = frame_sizes[i];
        rom_offset = align4(rom_offset);
        rom_offset = advance_window_bounded(rom_offset, frame_size);
        if (rom_offset + frame_size > rom_size) {
            fprintf(stderr, "Error: ROM full after %u frames\n", frame_count);
            free(player);
            free(gbm);
            free(rom);
            free(frame_offsets);
            free(minute_frames);
            free(frame_src_offsets);
            free(frame_sizes);
            return 1;
        }
        if (append_u32(&frame_offsets, &frame_count, &frame_capacity, rom_offset) != 0) {
            fprintf(stderr, "Error: out of memory\n");
            free(player);
            free(gbm);
            free(rom);
            free(frame_offsets);
            free(minute_frames);
            free(frame_src_offsets);
            free(frame_sizes);
            return 1;
        }
        memcpy(rom + rom_offset, gbm + frame_src_offsets[i], frame_size);
        rom_offset += frame_size;
    }

    const uint32_t video_data_end = rom_offset;
    uint32_t audio_header_offset = 0;
    uint32_t audio_block_offset = 0;
    uint32_t audio_data_end = 0;
    if (gbs) {
        rom_offset = align4(rom_offset);
        rom_offset = advance_window_bounded(rom_offset, GBS_HEADER_SIZE);
        audio_header_offset = rom_offset;
        if (audio_header_offset + GBS_HEADER_SIZE > rom_size) {
            fprintf(stderr, "Error: ROM full before GBS header\n");
            free(player);
            free(gbm);
            free(gbs);
            free(rom);
            free(frame_offsets);
            free(minute_frames);
            free(frame_src_offsets);
            free(frame_sizes);
            return 1;
        }
        memcpy(rom + audio_header_offset, gbs, GBS_HEADER_SIZE);

        audio_block_offset = align_to(audio_header_offset + GBS_HEADER_SIZE, audio_block_size);
        audio_block_offset = advance_window_bounded(audio_block_offset, audio_block_size);

        uint32_t src = GBS_HEADER_SIZE;
        uint32_t dst = audio_block_offset;
        while (src + audio_block_size <= gbs_size) {
            dst = advance_window_bounded(dst, audio_block_size);
            if (dst + audio_block_size > rom_size) {
                fprintf(stderr, "Error: ROM full while writing GBS data\n");
                free(player);
                free(gbm);
                free(gbs);
                free(rom);
                free(frame_offsets);
                free(minute_frames);
                free(frame_src_offsets);
                free(frame_sizes);
                return 1;
            }
            memcpy(rom + dst, gbs + src, audio_block_size);
            src += audio_block_size;
            dst += audio_block_size;
        }
        audio_data_end = dst;
        rom_offset = dst;
    }

    const uint32_t header_offset = M3V_HEADER_ROM_OFFSET;
    const uint32_t frame_index_offset = align4(header_offset + sizeof(M3VHeader));
    const uint32_t minute_index_offset = align4(frame_index_offset + frame_count * 4u);
    const uint32_t index_end = align4(minute_index_offset + minute_count * 4u);
    if (index_end > video_data_start) {
        fprintf(stderr, "Error: internal M3V metadata layout overlap\n");
        free(player);
        free(gbm);
        free(rom);
        free(frame_offsets);
        free(minute_frames);
        free(frame_src_offsets);
        free(frame_sizes);
        return 1;
    }

    M3VHeader header;
    memset(&header, 0, sizeof(header));
    const uint32_t output_size = align4(rom_offset);
    memcpy(header.magic, "M3V0", 4);
    header.version = 1;
    header.header_size = sizeof(M3VHeader);
    header.flags = M3V_FLAG_COMPACT_FRAMES;
    header.fps = 10;
    header.frame_count = frame_count;
    header.minute_count = minute_count;
    header.gbm_version = gbm[0x10];
    header.frame_index_offset = frame_index_offset;
    header.minute_index_offset = minute_index_offset;
    header.video_data_start = video_data_start;
    header.video_data_end = video_data_end;
    header.rom_size = output_size;
    header.audio_header_offset = audio_header_offset;
    header.audio_block_offset = audio_block_offset;
    header.audio_size = gbs_size;
    header.audio_data_end = audio_data_end;

    memcpy(rom + header_offset, &header, sizeof(header));
    memcpy(rom + frame_index_offset, frame_offsets, frame_count * 4u);
    memcpy(rom + minute_index_offset, minute_frames, minute_count * 4u);

    FILE* out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "Error: cannot create output: %s\n", out_path);
        free(player);
        free(gbm);
        free(rom);
        free(frame_offsets);
        free(minute_frames);
        free(frame_src_offsets);
        free(frame_sizes);
        return 1;
    }
    fwrite(rom, 1, output_size, out);
    fclose(out);

    printf("Created: %s\n", out_path);
    printf("  capacity=%u MiB frames=%u minutes=%u audio=%s used=%u bytes\n",
           rom_mb, frame_count, minute_count, gbs ? "yes" : "no", output_size);

    free(player);
    free(gbm);
    free(gbs);
    free(rom);
    free(frame_offsets);
    free(minute_frames);
    free(frame_src_offsets);
    free(frame_sizes);
    return 0;
}
