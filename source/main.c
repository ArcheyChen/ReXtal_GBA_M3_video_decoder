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
 * - SELECT/START menu for resume, restart, and copyright notice
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
#include "m3_trace.h"

static void copy_frame_to_vram(const void* src, void* dst, u32 size) {
    REG_DMA3CNT = 0;
    REG_DMA3SAD = (u32)src;
    REG_DMA3DAD = (u32)dst;
    REG_DMA3CNT = DMA_SRC_INC | DMA_DST_INC | DMA32 | DMA_ENABLE | (size >> 2);
    while (REG_DMA3CNT & DMA_ENABLE) {
    }
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
static u32 total_frames = 0;

// For tracking current minute (for sync and seeking)
static u32 current_minute = 0;
static bool decoded_frame_invalidated = false;

// Pause state
static bool is_paused = false;
static bool menu_requested = false;
static bool resume_audio_after_seek_frame = false;
static volatile u16 input_down_latch = 0;
static volatile u16 input_held_snapshot = 0;

#define CONSOLE_COLS 30
#define MENU_LEFT_PAD 7
#define INPUT_KEY_MASK 0x03ffu

static u16 read_raw_keys_held(void) {
    return (u16)((~REG_KEYINPUT) & INPUT_KEY_MASK);
}

static void vblank_handler(void) {
    u16 held = read_raw_keys_held();
    input_down_latch |= (u16)(held & ~input_held_snapshot);
    input_held_snapshot = held;

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

static u16 consume_input_down(void) {
    u16 keys;
    u16 old_ime = REG_IME;
    REG_IME = 0;
    keys = input_down_latch;
    input_down_latch = 0;
    REG_IME = old_ime;
    return keys;
}

static void clear_input_latch(void) {
    u16 old_ime = REG_IME;
    REG_IME = 0;
    input_down_latch = 0;
    input_held_snapshot = read_raw_keys_held();
    REG_IME = old_ime;
}

static void show_error(const char* msg) {
    consoleDemoInit();
    iprintf("\x1b[2J");
    iprintf("Ausar's M3 Media Player\n");
    iprintf("================\n\n");
    iprintf("Error: %s\n", msg);
    while (1) VBlankIntrWait();
}

static void print_centered(const char* text) {
    int len = (int)strlen(text);
    int pad = 0;
    if (len < CONSOLE_COLS) {
        pad = (CONSOLE_COLS - len) / 2;
    }
    for (int i = 0; i < pad; i++) {
        iprintf(" ");
    }
    iprintf("%s\n", text);
}

static void print_separator(void) {
    iprintf("==============================\n");
}

static void print_menu_line(const char* text) {
    for (int i = 0; i < MENU_LEFT_PAD; i++) {
        iprintf(" ");
    }
    iprintf("%s\n", text);
}

static void format_duration(u32 seconds, char* out, size_t out_size) {
    u32 minutes = seconds / 60;
    seconds -= minutes * 60;

    if (minutes >= 60) {
        u32 hours = minutes / 60;
        minutes -= hours * 60;
        snprintf(out, out_size, "%luh %02lum %02lus",
                 (unsigned long)hours,
                 (unsigned long)minutes,
                 (unsigned long)seconds);
    } else if (minutes > 0) {
        snprintf(out, out_size, "%lum %02lus",
                 (unsigned long)minutes,
                 (unsigned long)seconds);
    } else {
        snprintf(out, out_size, "%lus", (unsigned long)seconds);
    }
}

static void format_size(u32 bytes, char* out, size_t out_size) {
    if (bytes >= 1024u * 1024u) {
        u32 mb10 = (bytes * 10u + 512u * 1024u) / (1024u * 1024u);
        snprintf(out, out_size, "%lu.%lu MB",
                 (unsigned long)(mb10 / 10u),
                 (unsigned long)(mb10 % 10u));
    } else {
        snprintf(out, out_size, "%lu KB",
                 (unsigned long)((bytes + 1023u) / 1024u));
    }
}

static const char* audio_mode_name(GbsMode mode) {
    switch (mode) {
        case GBS_MODE_STEREO_4BIT:
            return "Stereo 4-bit";
        case GBS_MODE_MONO_3BIT:
            return "Mono 3-bit";
        case GBS_MODE_MONO_4BIT:
            return "Mono 4-bit";
        case GBS_MODE_MONO_2BIT:
            return "Mono 2-bit";
        case GBS_MODE_MONO_2BIT_SM:
            return "Mono 2-bit sm";
        default:
            return "Unknown";
    }
}

static void show_info(void) {
    char line[32];
    char value[20];
    u32 duration_seconds = 0;

    iprintf("\x1b[2J");
    print_centered("ReXtal:Ausar's M3 Decoder");
    print_separator();
    iprintf("\n");
    print_menu_line("Info");
    iprintf("\n");

    if (has_video && total_frames > 0) {
        duration_seconds = total_frames / 10u;
    } else if (has_audio) {
        const GbsAudioInfo* info = gbs_audio_get_info();
        duration_seconds = info->total_samples / info->sample_rate;
    }

    format_duration(duration_seconds, value, sizeof(value));
    snprintf(line, sizeof(line), "Length: %s", value);
    print_menu_line(line);

    if (has_video) {
        u32 video_bytes = use_banked_video ? banked_video_total_size() : video_size;
        format_size(video_bytes, value, sizeof(value));
        snprintf(line, sizeof(line), "Video : %s", value);
        print_menu_line(line);
        snprintf(line, sizeof(line), "Frames: %lu", (unsigned long)total_frames);
        print_menu_line(line);
    } else {
        print_menu_line("Video : Not found");
    }

    if (has_audio) {
        const GbsAudioInfo* info = gbs_audio_get_info();
        snprintf(line, sizeof(line), "Audio : Mode %lu", (unsigned long)info->mode);
        print_menu_line(line);
        print_menu_line(audio_mode_name(info->mode));
    } else {
        print_menu_line("Audio : Not found");
    }

    iprintf("\n\n");
    print_centered("A/B/SELECT: Return");
}

static void format_time(u32 frames, char* out) {
    u32 seconds = frames / 10;
    u32 minutes = seconds / 60;
    seconds -= minutes * 60;
    if (minutes > 99) {
        u32 hours = minutes / 60;
        minutes -= hours * 60;
        if (hours > 9) hours = 9;
        out[0] = '0' + hours;
        out[1] = ':';
        out[2] = '0' + (minutes / 10);
        out[3] = '0' + (minutes % 10);
        out[4] = ':';
        out[5] = '0' + (seconds / 10);
        out[6] = '0' + (seconds % 10);
        out[7] = 0;
    } else {
        out[0] = '0' + (minutes / 10);
        out[1] = '0' + (minutes % 10);
        out[2] = ':';
        out[3] = '0' + (seconds / 10);
        out[4] = '0' + (seconds % 10);
        out[5] = 0;
    }
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

static void restore_video_display(void) {
    SetMode(MODE_3 | BG2_ENABLE);
    if (has_video) {
        copy_frame_to_vram(frame_buffer, (void*)0x06000000, 240 * 160 * 2);
    }
}

// Pre-calculated I-frame offsets (one per minute)
// Maximum 256 minutes (~4 hours) should be enough
#define MAX_MINUTES 256
EWRAM_BSS static u32 iframe_offsets[MAX_MINUTES];
static u32 total_minutes = 0;

// Scan video to find I-frame offsets (every 600 frames)
static void scan_iframe_offsets(void) {
    if (use_banked_video) {
        total_minutes = banked_video_minute_count();
        total_frames = banked_video_frame_count();
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
    total_frames = frame_count;
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

    bool resume_after_video_frame = false;
    if (has_video) {
        video_seek_minute(minute);
        resume_after_video_frame = has_audio && !is_paused;
    }

    if (has_audio) {
        if (has_video) {
            gbs_audio_seek_minute_paused(minute);
            resume_audio_after_seek_frame = resume_after_video_frame;
        } else {
            gbs_audio_seek_minute(minute);
        }
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

static void set_pause_state(bool pause) {
    if (is_paused == pause) return;
    toggle_pause();
}

static void wait_for_key_release(void) {
    do {
        VBlankIntrWait();
        scanKeys();
    } while (keysHeld());
    clear_input_latch();
}

static void show_copyright_notice(void) {
    iprintf("\x1b[2J");
    print_centered("ReXtal:Ausar's M3 Decoder");
    print_separator();
    iprintf("\n");
    print_menu_line("Copyright");
    iprintf("\n\n");
    print_menu_line("Free to use.");
    print_menu_line("No commercial use.");
    iprintf("\n");
    print_menu_line("Author: Ausar");
    print_menu_line("GitHub : archeychen");
    iprintf("\n\n\n");
    print_centered("A/B/SELECT: Return");

    wait_for_key_release();
    while (1) {
        VBlankIntrWait();
        scanKeys();
        u16 keys = keysDown();
        if (keys & (KEY_A | KEY_B | KEY_SELECT | KEY_START)) {
            wait_for_key_release();
            return;
        }
    }
}

typedef enum {
    MENU_ACTION_RESUME = 0,
    MENU_ACTION_RESTART = 1,
} MenuAction;

static void wait_for_menu_page_return(void) {
    wait_for_key_release();
    while (1) {
        VBlankIntrWait();
        scanKeys();
        u16 keys = keysDown();
        if (keys & (KEY_A | KEY_B | KEY_SELECT | KEY_START)) {
            wait_for_key_release();
            return;
        }
    }
}

static void draw_pause_menu(u32 selected) {
    char elapsed[8];
    char total[8];
    char status[24];
    format_time(current_frame, elapsed);
    format_time(total_frames, total);
    snprintf(status, sizeof(status), "%s / %s", elapsed, total);

    iprintf("\x1b[2J");
    print_centered("ReXtal:Ausar's M3 Decoder");
    print_separator();
    iprintf("\n");
    print_centered(status);
    iprintf("\n\n");
    print_menu_line(selected == 0 ? "> Resume" : "  Resume");
    print_menu_line(selected == 1 ? "> Restart" : "  Restart");
    print_menu_line(selected == 2 ? "> Info" : "  Info");
    print_menu_line(selected == 3 ? "> Copyright" : "  Copyright");
    iprintf("\n\n");
    print_centered("UP/DOWN: Move");
    print_centered("A: Select  B: Resume");
}

static MenuAction show_pause_menu(void) {
    set_pause_state(true);
    consoleDemoInit();
    wait_for_key_release();

    u32 selected = 0;
    draw_pause_menu(selected);

    while (1) {
        VBlankIntrWait();
        scanKeys();
        u16 keys = keysDown();

        if (keys & KEY_UP) {
            selected = (selected + 3) % 4;
            draw_pause_menu(selected);
        } else if (keys & KEY_DOWN) {
            selected = (selected + 1) % 4;
            draw_pause_menu(selected);
        }

        if (keys & (KEY_B | KEY_SELECT)) {
            wait_for_key_release();
            return MENU_ACTION_RESUME;
        }

        if (keys & KEY_A) {
            wait_for_key_release();
            if (selected == 0) {
                return MENU_ACTION_RESUME;
            }
            if (selected == 1) {
                return MENU_ACTION_RESTART;
            }
            if (selected == 2) {
                show_info();
                wait_for_menu_page_return();
            } else {
                show_copyright_notice();
            }
            draw_pause_menu(selected);
        }
    }
}

static void update_current_minute_after_display(void) {
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

static inline bool is_minute_iframe(void) {
    return current_frame == current_minute * FRAMES_PER_MINUTE;
}

static void display_decoded_frame(void) {
    m3_trace_begin(M3_TRACE_ZONE_VRAM_COPY);
    copy_frame_to_vram(frame_buffer, (void*)0x06000000, 240 * 160 * 2);
    m3_trace_end(M3_TRACE_ZONE_VRAM_COPY);
    current_frame++;
    update_current_minute_after_display();
}

static bool run_pause_menu_after_stable_frame(void) {
    menu_requested = false;
    MenuAction action = show_pause_menu();
    if (action == MENU_ACTION_RESTART) {
        seek_to_minute(0);
        init_video_display();
        set_pause_state(false);
        return true;
    }

    restore_video_display();
    set_pause_state(false);
    return false;
}

static void run_pause_menu_audio_only(void) {
    menu_requested = false;
    MenuAction action = show_pause_menu();
    if (action == MENU_ACTION_RESTART) {
        seek_to_minute(0);
    }
    set_pause_state(false);
}

// Decode next frame into frame_buffer (does not display)
static void decode_next_frame(void) {
    if (!has_video) return;

    m3_trace_frame(current_frame);
    m3_trace_value(M3_TRACE_ZONE_SYNC_LAG, target_frame > current_frame ? target_frame - current_frame : 0);
    m3_trace_begin(M3_TRACE_ZONE_VIDEO_DECODE);

    if (use_banked_video) {
        const u32 frame_count = banked_video_frame_count();
        if (frame_count == 0) {
            m3_trace_end(M3_TRACE_ZONE_VIDEO_DECODE);
            return;
        }
        if (current_frame >= frame_count) {
            current_frame = 0;
            target_frame = 0;
            current_minute = 0;
        }
        const u8* frame_ptr = banked_video_frame_ptr(current_frame);
        if (frame_ptr) {
            const bool compact_frame = banked_video_uses_compact_frames();
            m3_trace_value(M3_TRACE_ZONE_FRAME_BYTES,
                           compact_frame ? 0 : (frame_ptr[0] | (frame_ptr[1] << 8)));
            if (is_minute_iframe()) {
                memset(frame_buffer, 0, sizeof(frame_buffer));
                if (compact_frame && banked_video_is_metadata_obfuscated()) {
                    gbm_decode_frame_body_obfuscated(frame_ptr, 0, 0, frame_buffer, NULL,
                                                     current_frame);
                } else if (compact_frame) {
                    gbm_decode_frame_body(frame_ptr, 0, 0, frame_buffer, NULL);
                } else if (banked_video_is_metadata_obfuscated()) {
                    gbm_decode_frame_obfuscated(frame_ptr, 0, frame_buffer, NULL, current_frame);
                } else {
                    gbm_decode_frame(frame_ptr, 0, frame_buffer, NULL);
                }
            } else {
                if (compact_frame && banked_video_is_metadata_obfuscated()) {
                    gbm_decode_frame_body_obfuscated(frame_ptr, 0, 0, frame_buffer,
                                                    (const u16*)0x06000000, current_frame);
                } else if (compact_frame) {
                    gbm_decode_frame_body(frame_ptr, 0, 0, frame_buffer,
                                          (const u16*)0x06000000);
                } else if (banked_video_is_metadata_obfuscated()) {
                    gbm_decode_frame_obfuscated(frame_ptr, 0, frame_buffer, (const u16*)0x06000000,
                                                current_frame);
                } else {
                    gbm_decode_frame(frame_ptr, 0, frame_buffer, (const u16*)0x06000000);
                }
            }
        }
        m3_trace_end(M3_TRACE_ZONE_VIDEO_DECODE);
        return;
    }

    if (!video_data) {
        m3_trace_end(M3_TRACE_ZONE_VIDEO_DECODE);
        return;
    }

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

    m3_trace_value(M3_TRACE_ZONE_FRAME_BYTES, frame_len);

    // Decode frame (dst = EWRAM buffer, ref = VRAM for delta)
    if (is_minute_iframe()) {
        memset(frame_buffer, 0, sizeof(frame_buffer));
        video_offset = gbm_decode_frame(video_data, video_offset, frame_buffer, NULL);
    } else {
        video_offset = gbm_decode_frame(video_data, video_offset, frame_buffer, (const u16*)0x06000000);
    }
    m3_trace_end(M3_TRACE_ZONE_VIDEO_DECODE);
}

// Check if audio triggered a sync point (called from main loop)
static void check_audio_sync(void) {
    if (!has_audio) return;

    int32_t sync_minute = gbs_audio_check_minute_sync();
    if (sync_minute >= 0 && (u32)sync_minute < total_minutes) {
        // Audio reached a new minute, force video to sync
        video_seek_minute((u32)sync_minute);
    }
}

// Handle input, returns true if pause state changed
static bool handle_input(void) {
    u16 keys = consume_input_down();
    if (!keys) {
        return false;
    }

    // A: toggle pause/resume
    if (keys & KEY_A) {
        toggle_pause();
        return true;
    }

    // SELECT/START: open pause menu. Restart is inside the menu to avoid mis-taps.
    if (keys & (KEY_SELECT | KEY_START)) {
        menu_requested = true;
        return false;
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

static void resume_audio_after_seek_frame_if_needed(void) {
    if (resume_audio_after_seek_frame) {
        resume_audio_after_seek_frame = false;
        if (has_audio && !is_paused) {
            gbs_audio_resume();
        }
    }
}

static bool display_invalidated_frame(void) {
    if (!decoded_frame_invalidated) {
        return false;
    }

    decoded_frame_invalidated = false;
    decode_next_frame();
    display_decoded_frame();
    resume_audio_after_seek_frame_if_needed();
    return true;
}

static bool service_pause_or_menu(void) {
    if (menu_requested) {
        run_pause_menu_after_stable_frame();
        return true;
    }

    while (is_paused) {
        VBlankIntrWait();
        if (has_audio) {
            gbs_audio_update();
        }
        handle_input();
        if (display_invalidated_frame()) {
            return true;
        }
        if (menu_requested) {
            run_pause_menu_after_stable_frame();
            return true;
        }
    }

    return false;
}

// Process video frames with frame rate control
// Flow: decode -> wait for timing -> display -> repeat
static void process_video(void) {
    if (has_audio) {
        gbs_audio_update();
    }

    if (display_invalidated_frame()) {
        return;
    }
    handle_input();
    if (display_invalidated_frame() || service_pause_or_menu()) {
        return;
    }

    // Decode next frame first (into frame_buffer)
    decode_next_frame();
    decoded_frame_invalidated = false;

    if (has_audio) {
        gbs_audio_update();
    }

    handle_input();
    if (display_invalidated_frame()) {
        return;
    }
    if (menu_requested || is_paused) {
        display_decoded_frame();
        service_pause_or_menu();
        return;
    }

    // Wait until it's time to display
    // Also check input during wait so pause can be toggled
    while (current_frame >= target_frame) {
        VBlankIntrWait();
        if (has_audio) {
            gbs_audio_update();
        }
        handle_input();
        if (display_invalidated_frame()) {
            return;
        }
        if (menu_requested) {
            display_decoded_frame();
            run_pause_menu_after_stable_frame();
            return;
        }
        if (is_paused && service_pause_or_menu()) {
            return;
        }
    }

    // Display the pre-decoded frame
    display_decoded_frame();

    if (menu_requested) {
        run_pause_menu_after_stable_frame();
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
    if (use_banked_video && banked_video_has_audio()) {
        if (gbs_audio_init_banked(banked_video_audio_header_offset(),
                                  banked_video_audio_block_offset(),
                                  banked_video_audio_size())) {
            has_audio = true;
        }
    } else if (!use_banked_video && media_source_find_gbs(&audio_info)) {
        if (gbs_audio_init(audio_info.data, audio_info.size)) {
            has_audio = true;
        }
    }

    // Must have at least one media type
    if (!has_video && !has_audio) {
        show_error("No media files found!\nAdd .gbm or .gbs files.");
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
        if (has_audio) {
            gbs_audio_update();
        }

        // Check for audio-driven sync (audio reached a minute boundary)
        if (has_video) {
            check_audio_sync();
        }

        if (has_video) {
            process_video();
        } else {
            // Audio only - just wait for VBlank and handle input
            VBlankIntrWait();
            if (has_audio) {
                gbs_audio_update();
            }
            handle_input();
            if (menu_requested) {
                run_pause_menu_audio_only();
            }
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
