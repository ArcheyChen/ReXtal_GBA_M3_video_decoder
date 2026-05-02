/*
 * Multibank video packager.
 *
 * Usage:
 *   mb_video_packager output.gba M3_Movie_Player_mb.gba movie.gbm [rom_mb]
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M3V_HEADER_ROM_OFFSET 0x00100000u
#define M3V_BANK_SIZE 0x00400000u
#define M3V_BANK_MASK (M3V_BANK_SIZE - 1u)
#define M3V_DEFAULT_ROM_MB 256u
#define M3V_DATA_START M3V_BANK_SIZE
#define GBM_HEADER_SIZE 0x200u
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
    uint32_t reserved[19];
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
    fprintf(stderr, "Usage: %s output.gba M3_Movie_Player_mb.gba movie.gbm [rom_mb]\n", prog);
}

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        print_usage(argv[0]);
        return 1;
    }

    const char* out_path = argv[1];
    const char* player_path = argv[2];
    const char* gbm_path = argv[3];
    uint32_t rom_mb = M3V_DEFAULT_ROM_MB;
    if (argc == 5) {
        rom_mb = (uint32_t) strtoul(argv[4], NULL, 0);
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

    uint8_t* rom = malloc(rom_size);
    if (!rom) {
        fprintf(stderr, "Error: failed to allocate ROM image\n");
        free(player);
        free(gbm);
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

    uint32_t gbm_offset = GBM_HEADER_SIZE;
    uint32_t rom_offset = M3V_DATA_START;
    while (gbm_offset + 2u <= gbm_size) {
        const uint16_t frame_len = read_le16(gbm + gbm_offset);
        if (frame_len == 0 || frame_len == 0xFFFF) {
            break;
        }
        const uint32_t frame_size = 2u + frame_len;
        if (gbm_offset + frame_size > gbm_size) {
            fprintf(stderr, "Error: truncated GBM frame at 0x%X\n", gbm_offset);
            free(player);
            free(gbm);
            free(rom);
            free(frame_offsets);
            free(minute_frames);
            return 1;
        }
        if (frame_size > M3V_BANK_SIZE) {
            fprintf(stderr, "Error: frame too large for one bank\n");
            free(player);
            free(gbm);
            free(rom);
            free(frame_offsets);
            free(minute_frames);
            return 1;
        }
        if ((rom_offset & M3V_BANK_MASK) + frame_size > M3V_BANK_SIZE) {
            rom_offset = (rom_offset + M3V_BANK_MASK) & ~M3V_BANK_MASK;
        }
        if (rom_offset + frame_size > rom_size) {
            fprintf(stderr, "Error: ROM full after %u frames\n", frame_count);
            free(player);
            free(gbm);
            free(rom);
            free(frame_offsets);
            free(minute_frames);
            return 1;
        }
        if ((frame_count % FRAMES_PER_MINUTE) == 0) {
            if (append_u32(&minute_frames, &minute_count, &minute_capacity, frame_count) != 0) {
                fprintf(stderr, "Error: out of memory\n");
                free(player);
                free(gbm);
                free(rom);
                free(frame_offsets);
                free(minute_frames);
                return 1;
            }
        }
        if (append_u32(&frame_offsets, &frame_count, &frame_capacity, rom_offset) != 0) {
            fprintf(stderr, "Error: out of memory\n");
            free(player);
            free(gbm);
            free(rom);
            free(frame_offsets);
            free(minute_frames);
            return 1;
        }
        memcpy(rom + rom_offset, gbm + gbm_offset, frame_size);
        gbm_offset += frame_size;
        rom_offset += frame_size;
    }

    if (frame_count == 0) {
        fprintf(stderr, "Error: no frames found in GBM\n");
        free(player);
        free(gbm);
        free(rom);
        free(frame_offsets);
        free(minute_frames);
        return 1;
    }

    const uint32_t header_offset = M3V_HEADER_ROM_OFFSET;
    const uint32_t frame_index_offset = align4(header_offset + sizeof(M3VHeader));
    const uint32_t minute_index_offset = align4(frame_index_offset + frame_count * 4u);
    const uint32_t index_end = align4(minute_index_offset + minute_count * 4u);
    if (index_end > M3V_BANK_SIZE) {
        fprintf(stderr, "Error: index does not fit in bank0\n");
        free(player);
        free(gbm);
        free(rom);
        free(frame_offsets);
        free(minute_frames);
        return 1;
    }

    M3VHeader header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, "M3V0", 4);
    header.version = 1;
    header.header_size = sizeof(M3VHeader);
    header.flags = 0;
    header.fps = 10;
    header.frame_count = frame_count;
    header.minute_count = minute_count;
    header.gbm_version = gbm[0x10];
    header.frame_index_offset = frame_index_offset;
    header.minute_index_offset = minute_index_offset;
    header.video_data_start = M3V_DATA_START;
    header.video_data_end = rom_offset;
    header.rom_size = rom_size;

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
        return 1;
    }
    fwrite(rom, 1, rom_size, out);
    fclose(out);

    printf("Created: %s\n", out_path);
    printf("  rom=%u MiB frames=%u minutes=%u used=%u bytes\n",
           rom_mb, frame_count, minute_count, rom_offset);

    free(player);
    free(gbm);
    free(rom);
    free(frame_offsets);
    free(minute_frames);
    return 0;
}
