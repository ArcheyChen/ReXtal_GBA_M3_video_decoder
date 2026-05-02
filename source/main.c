/*
 * GBA Media Player
 *
 * Combined video (GBM) and audio (GBS) player.
 * Loads media files from GBFS filesystem.
 *
 * Features:
 * - 10 FPS video with frame rate control
 * - A/V sync every 600 frames (1 minute) at I-frames
 * - A button to pause/resume
 * - L/R buttons for seeking by minute
 * - START button to restart from beginning
 */

#include <gba.h>
#include <gba_console.h>
#include <gba_interrupt.h>
#include <gba_input.h>
#include <gba_systemcalls.h>
#include <gba_video.h>
#include <stdio.h>
#include <string.h>

#include "media_source.h"
#include "banked_video.h"
#include "gbs_audio.h"
#include "gbm_decoder.h"

// Fast frame copy using ARM ldmia/stmia
// Unlike DMA, CPU memory access doesn't monopolize the bus and won't
// starve audio FIFO DMA. Audio DMA has higher priority and can interleave.
//
// Copies 128 bytes per iteration (4x unrolled, 32 bytes each).
// Total: 76800 bytes = 600 iterations.
__attribute__((target("arm"), noinline))
static void copy_frame_to_vram(const void* src, void* dst, u32 size) {
    asm volatile(
        "1:                         \n"
        "   ldmia %[src]!, {r2-r9}  \n"
        "   stmia %[dst]!, {r2-r9}  \n"
        "   ldmia %[src]!, {r2-r9}  \n"
        "   stmia %[dst]!, {r2-r9}  \n"
        "   ldmia %[src]!, {r2-r9}  \n"
        "   stmia %[dst]!, {r2-r9}  \n"
        "   ldmia %[src]!, {r2-r9}  \n"
        "   stmia %[dst]!, {r2-r9}  \n"
        "   subs  %[size], %[size], #128 \n"
        "   bgt   1b                \n"
        : [src] "+r" (src), [dst] "+r" (dst), [size] "+r" (size)
        :
        : "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "memory", "cc"
    );
}

// EWRAM buffer for video frame (240 * 160 = 38400 pixels)
EWRAM_BSS u16 frame_buffer[38400];

// State
static bool has_video = false;
static bool has_audio = false;
static bool use_banked_video = false;
static const uint8_t* video_data = NULL;
static uint32_t video_offset = GBM_HEADER_SIZE;
static uint32_t video_size = 0;

// Frame rate control
// Video is 10 FPS, VBlank is 60 Hz, so 1 frame = 6 VBlanks
#define VBLANKS_PER_FRAME 6

// I-frame interval: 600 frames = 1 minute at 10 FPS
#define FRAMES_PER_MINUTE 600

// target_frame: incremented by VBlank ISR, represents "should have displayed this many frames"
// current_frame: maintained by main loop, represents "have decoded this many frames"
static volatile u32 target_frame = 0;
static u32 current_frame = 0;

// For tracking current minute (for sync and seeking)
static u32 current_minute = 0;
static bool decoded_frame_invalidated = false;

// Pause state
static bool is_paused = false;

static void vblank_handler(void) {
    // Called at 60 Hz, increment target_frame every 6 VBlanks (10 FPS)
    // Don't increment when paused
    if (is_paused) return;

    static u8 vblank_counter = 0;
    vblank_counter++;
    if (vblank_counter >= VBLANKS_PER_FRAME) {
        vblank_counter = 0;
        target_frame++;
    }
}

static void show_error(const char* msg) {
    consoleDemoInit();
    iprintf("\x1b[2J");
    iprintf("Ausar's M3 Media Player\n");
    iprintf("================\n\n");
    iprintf("Error: %s\n", msg);
    while (1) VBlankIntrWait();
}

static void show_info(void) {
    iprintf("\x1b[2J");
    iprintf("Ausar's M3 Media Player\n");
    iprintf("================\n\n");

    if (has_video) {
        if (use_banked_video) {
            iprintf("Video: Banked (%lu KB)\n",
                    (unsigned long)(banked_video_total_size() / 1024));
            iprintf("Frames: %lu\n", (unsigned long)banked_video_frame_count());
        } else {
            iprintf("Video: Yes (%lu KB)\n", (unsigned long)(video_size / 1024));
        }
    } else {
        iprintf("Video: Not found\n");
    }

    if (has_audio) {
        const GbsAudioInfo* info = gbs_audio_get_info();
        uint32_t duration = info->total_samples / info->sample_rate;
        iprintf("Audio: Mode %d, %lu sec\n", info->mode, (unsigned long)duration);
    } else {
        iprintf("Audio: Not found\n");
    }

    iprintf("\nStarting playback...\n");
}

static void init_video_display(void) {
    // Mode 3: 240x160, 15-bit color
    SetMode(MODE_3 | BG2_ENABLE);

    // Clear buffers
    memset(frame_buffer, 0, sizeof(frame_buffer));

    // Clear VRAM
    u16* vram = (u16*)0x06000000;
    for (int i = 0; i < 38400; i++) {
        vram[i] = 0;
    }
}

// Pre-calculated I-frame offsets (one per minute)
// Maximum 256 minutes (~4 hours) should be enough
#define MAX_MINUTES 256
static u32 iframe_offsets[MAX_MINUTES];
static u32 total_minutes = 0;

// Scan video to find I-frame offsets (every 600 frames)
static void scan_iframe_offsets(void) {
    if (use_banked_video) {
        total_minutes = banked_video_minute_count();
        return;
    }
    if (!has_video || !video_data) return;

    u32 offset = GBM_HEADER_SIZE;
    u32 frame_count = 0;
    u32 minute = 0;

    while (offset + 2 < video_size && minute < MAX_MINUTES) {
        // Record offset at start of each minute (every 600 frames)
        if (frame_count % FRAMES_PER_MINUTE == 0) {
            iframe_offsets[minute] = offset;
            minute++;
        }

        // Read frame length and skip to next frame
        uint16_t frame_len = video_data[offset] | (video_data[offset + 1] << 8);
        if (frame_len == 0 || frame_len == 0xFFFF) break;

        offset = offset + 2 + frame_len;
        frame_count++;
    }

    total_minutes = minute;
}

// Seek video to a specific minute (jumps to I-frame)
// I-frame will fully redraw the screen, no need to clear VRAM
static void video_seek_minute(u32 minute) {
    if (!has_video || minute >= total_minutes) return;

    decoded_frame_invalidated = true;

    if (use_banked_video) {
        current_frame = banked_video_minute_frame(minute);
        target_frame = current_frame;
        current_minute = minute;
        return;
    }

    video_offset = iframe_offsets[minute];
    current_minute = minute;

    // Reset frame counters to match the new position
    // Use addition loop instead of multiplication
    current_frame = 0;
    for (u32 i = 0; i < minute; i++) {
        current_frame += FRAMES_PER_MINUTE;
    }
    target_frame = current_frame;
}

// Seek both audio and video to a specific minute
static void seek_to_minute(u32 minute) {
    if (minute >= total_minutes && total_minutes > 0) {
        minute = total_minutes - 1;
    }

    if (has_video) {
        video_seek_minute(minute);
    }

    if (has_audio) {
        gbs_audio_seek_minute(minute);
    }

    current_minute = minute;
}

// Toggle pause state for both audio and video
static void toggle_pause(void) {
    if (is_paused) {
        // Resume
        is_paused = false;
        if (has_audio) {
            gbs_audio_resume();
        }
    } else {
        // Pause
        is_paused = true;
        if (has_audio) {
            gbs_audio_pause();
        }
    }
}

// Decode next frame into frame_buffer (does not display)
static void decode_next_frame(void) {
    if (!has_video) return;

    if (use_banked_video) {
        const u32 frame_count = banked_video_frame_count();
        if (frame_count == 0) {
            return;
        }
        if (current_frame >= frame_count) {
            current_frame = 0;
            target_frame = 0;
            current_minute = 0;
        }
        const u8* frame_ptr = banked_video_frame_ptr(current_frame);
        if (frame_ptr) {
            if ((current_frame % FRAMES_PER_MINUTE) == 0) {
                memset(frame_buffer, 0, sizeof(frame_buffer));
                gbm_decode_frame(frame_ptr, 0, frame_buffer, NULL);
            } else {
                gbm_decode_frame(frame_ptr, 0, frame_buffer, (const u16*)0x06000000);
            }
        }
        return;
    }

    if (!video_data) return;

    // Check for end of video
    if (video_offset + 2 >= video_size) {
        // Loop video
        video_offset = GBM_HEADER_SIZE;
        current_frame = 0;
        target_frame = 0;
        current_minute = 0;
    }

    // Read frame length
    uint16_t frame_len = video_data[video_offset] | (video_data[video_offset + 1] << 8);

    // Check for invalid frame
    if (frame_len == 0 || frame_len == 0xFFFF) {
        video_offset = GBM_HEADER_SIZE;
        current_frame = 0;
        target_frame = 0;
        current_minute = 0;
        frame_len = video_data[video_offset] | (video_data[video_offset + 1] << 8);
    }

    // Decode frame (dst = EWRAM buffer, ref = VRAM for delta)
    if ((current_frame % FRAMES_PER_MINUTE) == 0) {
        memset(frame_buffer, 0, sizeof(frame_buffer));
        video_offset = gbm_decode_frame(video_data, video_offset, frame_buffer, NULL);
    } else {
        video_offset = gbm_decode_frame(video_data, video_offset, frame_buffer, (const u16*)0x06000000);
    }
}

// Check if audio triggered a sync point (called from main loop)
static void check_audio_sync(void) {
    if (!has_audio || use_banked_video) return;

    int32_t sync_minute = gbs_audio_check_minute_sync();
    if (sync_minute >= 0 && (u32)sync_minute < total_minutes) {
        // Audio reached a new minute, force video to sync
        video_seek_minute((u32)sync_minute);
    }
}

// Handle input, returns true if pause state changed
static bool handle_input(void) {
    scanKeys();
    u16 keys = keysDown();

    // A: toggle pause/resume
    if (keys & KEY_A) {
        toggle_pause();
        return true;
    }

    // START: restart from beginning
    if (keys & KEY_START) {
        if (is_paused) {
            toggle_pause();  // Resume first
        }
        seek_to_minute(0);
    }

    // R: skip forward 1 minute
    if (keys & KEY_R) {
        u32 next_minute = current_minute + 1;
        if (next_minute < total_minutes) {
            seek_to_minute(next_minute);
        }
    }

    // L: skip backward 1 minute
    if (keys & KEY_L) {
        if (current_minute > 0) {
            seek_to_minute(current_minute - 1);
        } else {
            seek_to_minute(0);  // Go to start
        }
    }

    return false;
}

// Process video frames with frame rate control
// Flow: decode -> wait for timing -> display -> repeat
static void process_video(void) {
    // Decode next frame first (into frame_buffer)
    decode_next_frame();
    decoded_frame_invalidated = false;

    // Wait until it's time to display
    // Also check input during wait so pause can be toggled
    while (current_frame >= target_frame) {
        VBlankIntrWait();
        handle_input();
        if (decoded_frame_invalidated) {
            return;
        }
    }

    // Display the pre-decoded frame
    copy_frame_to_vram(frame_buffer, (void*)0x06000000, 240 * 160 * 2);
    current_frame++;

    // Update current minute (using subtraction loop instead of division)
    if (use_banked_video) {
        u32 minute_count = banked_video_minute_count();
        u32 minute = current_frame / FRAMES_PER_MINUTE;
        if (minute_count > 0 && minute >= minute_count) {
            minute = minute_count - 1;
        }
        current_minute = minute;
    } else {
        u32 frame = current_frame;
        current_minute = 0;
        while (frame >= FRAMES_PER_MINUTE) {
            frame -= FRAMES_PER_MINUTE;
            current_minute++;
        }
    }
}

int main(void) {
    // Initialize interrupts
    irqInit();
    irqSet(IRQ_VBLANK, vblank_handler);
    irqEnable(IRQ_VBLANK);

    if (banked_video_init()) {
        has_video = true;
        use_banked_video = true;
        gbm_set_version(banked_video_gbm_version());
    } else if (!media_source_init()) {
        show_error("No GBFS or M3V found!");
    }

    // Try to load video from GBFS if no banked video was found
    MediaSourceInfo video_info;
    if (!use_banked_video && media_source_find_gbm(&video_info)) {
        // Validate GBM header
        if (video_info.size >= GBM_HEADER_SIZE &&
            video_info.data[0] == 'G' && video_info.data[1] == 'B' &&
            video_info.data[2] == 'A' && video_info.data[3] == 'M') {
            has_video = true;
            video_data = video_info.data;
            video_size = video_info.size;
            // Set decoder version based on header (offset 0x10)
            // Gen1 = 0x06, Gen3 = 0x05
            gbm_set_version(video_info.data[0x10]);
        }
    }

    // Try to load audio
    MediaSourceInfo audio_info;
    if (!use_banked_video && media_source_find_gbs(&audio_info)) {
        if (gbs_audio_init(audio_info.data, audio_info.size)) {
            has_audio = true;
        }
    }

    // Must have at least one media type
    if (!has_video && !has_audio) {
        show_error("No media files found!\nAdd .gbm or .gbs files.");
    }

    // Show info briefly
    consoleDemoInit();
    show_info();

    // Wait a moment to show info
    for (int i = 0; i < 30; i++) {
        VBlankIntrWait();
    }

    // Start playback
    if (has_video) {
        init_video_display();
        scan_iframe_offsets();  // Build I-frame offset table for seeking
    }

    if (has_audio) {
        gbs_audio_start();
    }

    // Reset frame counters
    target_frame = 0;
    current_frame = 0;
    current_minute = 0;

    // Main loop
    while (1) {
        // Check for audio-driven sync (audio reached a minute boundary)
        if (has_video) {
            check_audio_sync();
        }

        if (has_video) {
            process_video();
        } else {
            // Audio only - just wait for VBlank and handle input
            VBlankIntrWait();
            handle_input();
        }

        // Handle audio looping
        if (has_audio && gbs_audio_is_finished()) {
            gbs_audio_restart();
            if (has_video) {
                seek_to_minute(0);  // Sync video when audio loops
            }
        }
    }

    return 0;
}
