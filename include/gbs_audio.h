/*
 * GBS Audio Decoder for GBA
 *
 * Public interface for GBS audio playback.
 * Supports all 5 GBS modes from the M3 Movie Player.
 */

#ifndef GBS_AUDIO_H
#define GBS_AUDIO_H

#include <gba_base.h>
#include <stdbool.h>
#include <stdint.h>

// GBS audio modes
typedef enum {
    GBS_MODE_STEREO_4BIT   = 0,  // Stereo 4-bit IMA ADPCM, 22050 Hz, block 0x400
    GBS_MODE_MONO_3BIT     = 1,  // Mono 3-bit ADPCM, 11025 Hz, block 0x400
    GBS_MODE_MONO_4BIT     = 2,  // Mono 4-bit IMA ADPCM, 11025 Hz, block 0x200
    GBS_MODE_MONO_2BIT     = 3,  // Mono 2-bit ADPCM, 22050 Hz, block 0x200
    GBS_MODE_MONO_2BIT_SM  = 4,  // Mono 2-bit ADPCM, 22050 Hz, block 0x100 (small)
    GBS_MODE_INVALID       = 255
} GbsMode;

// Audio state (read-only for external use)
typedef struct {
    GbsMode mode;
    uint32_t sample_rate;
    uint8_t channels;           // 1=mono, 2=stereo
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t total_samples;     // Per channel for stereo
    uint32_t samples_decoded;
    bool is_playing;
    bool is_finished;
} GbsAudioInfo;

/*
 * Initialize the GBS audio system with embedded data.
 *
 * @param gbs_data    Pointer to GBS file data (typically from bin2o)
 * @param gbs_size    Size of GBS data in bytes
 * @return            true if initialization successful
 */
bool gbs_audio_init(const uint8_t* gbs_data, uint32_t gbs_size);

/*
 * Initialize GBS audio from a multibank ROM layout.
 *
 * @param header_offset  Absolute ROM offset of the original 0x200-byte GBS header
 * @param block_offset   Absolute ROM offset of the first compressed GBS block
 * @param gbs_size       Original GBS file size, including the 0x200-byte header
 * @return               true if initialization successful
 */
bool gbs_audio_init_banked(uint32_t header_offset, uint32_t block_offset, uint32_t gbs_size);

/*
 * Start audio playback.
 * Call this after gbs_audio_init().
 */
void gbs_audio_start(void);

/*
 * Refill the compressed-audio block cache.
 * Call this regularly from the main loop while audio is playing.
 */
void gbs_audio_update(void);

/*
 * Stop audio playback.
 */
void gbs_audio_stop(void);

/*
 * Pause audio playback (can be resumed).
 */
void gbs_audio_pause(void);

/*
 * Resume audio playback after pause.
 */
void gbs_audio_resume(void);

/*
 * Check if audio is paused.
 */
bool gbs_audio_is_paused(void);

/*
 * Restart playback from beginning.
 */
void gbs_audio_restart(void);

/*
 * Check if audio is currently playing.
 */
bool gbs_audio_is_playing(void);

/*
 * Check if audio playback has finished.
 */
bool gbs_audio_is_finished(void);

/*
 * Get current playback progress (0-100).
 */
uint32_t gbs_audio_get_progress(void);

/*
 * Get audio information.
 * Returns pointer to internal state (do not modify).
 */
const GbsAudioInfo* gbs_audio_get_info(void);

/*
 * Shutdown audio system and release resources.
 */
void gbs_audio_shutdown(void);

/*
 * Seek to a specific minute in the audio.
 * Audio will stop, seek, and restart.
 *
 * @param minute  Target minute (0-based)
 */
void gbs_audio_seek_minute(uint32_t minute);

/*
 * Get current playback position in minutes.
 */
uint32_t gbs_audio_get_current_minute(void);

/*
 * Get total duration in minutes.
 */
uint32_t gbs_audio_get_total_minutes(void);

/*
 * Check if audio crossed a minute boundary since last check.
 * Returns the new minute number if crossed, or -1 if not.
 * Calling this clears the pending flag.
 */
int32_t gbs_audio_check_minute_sync(void);

#endif // GBS_AUDIO_H
