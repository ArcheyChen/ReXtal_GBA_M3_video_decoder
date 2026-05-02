/*
 * GBS Audio Decoder for GBA
 *
 * Implementation of GBS audio playback supporting all 5 modes.
 * Based on reverse engineering of savemu.dll from M3 Movie Player.
 */

#include "gbs_audio.h"

#include "banked_video.h"

#include <gba_dma.h>
#include <gba_interrupt.h>
#include <gba_sound.h>
#include <gba_timers.h>
#include <string.h>

// IWRAM placement for performance-critical code
#define IWRAM_CODE __attribute__((section(".iwram"), long_call))

// ============================================================================
// Constants
// ============================================================================

#define GBA_MASTER_CLOCK    16777216
#define GBS_HEADER_SIZE     0x200

#ifndef TIMER_CASCADE
#define TIMER_CASCADE       0x0004
#endif
#ifndef TIMER_IRQ
#define TIMER_IRQ           0x0040
#endif
#define SOUNDCNT_X_ENABLE   0x0080

// Buffer configuration
// AUDIO_BUFFER_SAMPLES controls the buffer swap frequency and interrupt rate.
//
// Timer0 overflows at sample_rate, Timer1 cascades and counts Timer0 overflows.
// When Timer1 counts AUDIO_BUFFER_SAMPLES overflows, it triggers IRQ to swap buffers.
//
// Swap frequency = sample_rate / AUDIO_BUFFER_SAMPLES
// Examples at 22050Hz: 368->60Hz, 512->43Hz, 736->30Hz, 1024->21.5Hz, 1472->15Hz
// Examples at 11025Hz: 368->30Hz, 512->21.5Hz, 736->15Hz, 1024->10.8Hz
//
// Larger buffer = fewer interrupts but higher latency and more memory usage.
// Buffer memory = AUDIO_BUFFER_SAMPLES * 2 (double buffering) * channels bytes.
// Source data per interrupt varies by mode:
//   Mode 0 (stereo 4bit): 1 byte/sample -> 1024 bytes
//   Mode 1 (mono 3bit):   3/8 byte/sample -> 384 bytes
//   Mode 2 (mono 4bit):   0.5 byte/sample -> 512 bytes
//   Mode 3/4 (mono 2bit): 0.25 byte/sample -> 256 bytes
//
// Value should be divisible by 8 for Mode 1 compatibility (8 samples per 3 bytes).
#define AUDIO_BUFFER_SAMPLES    1024
#define AUDIO_BUFFER_COUNT      2

// Compressed GBS block cache.
// The main loop fills this ring from ROM; the audio IRQ decodes from RAM.
#define GBS_BLOCK_CACHE_BLOCKS      16
#define GBS_BLOCK_CACHE_MAX_SIZE    0x400
#define GBS_BLOCK_CACHE_UPDATE_MAX  4

// ============================================================================
// ADPCM Tables
// ============================================================================

// Standard IMA ADPCM step table (89 entries)
__attribute__((section(".iwram.rodata"))) static const int16_t ima_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

// Standard IMA ADPCM index adjustment table (4-bit)
__attribute__((section(".iwram.rodata"))) static const int8_t ima_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

// 3-bit ADPCM index adjustment table (from savemu.dll)
__attribute__((section(".iwram.rodata"))) static const int8_t adpcm3_index_table[8] = {
    -1, -1, 2, 6, -1, -1, 2, 6
};

// IMA ADPCM diff table: 89 steps * 16 nibbles = 1424 entries (2848 bytes)
// Index = step_index * 16 + nibble, Value = signed diff to add to predictor
// Replaces branch-heavy diff calculation in decode_ima_4bit
__attribute__((section(".iwram.rodata"))) static const int16_t ima_diff_table[89 * 16] = {
    // step_index=0, step=7
         0,      1,      3,      4,      7,      8,     10,     11,      0,     -1,     -3,     -4,     -7,     -8,    -10,    -11,
    // step_index=1, step=8
         1,      2,      4,      5,      8,      9,     11,     12,     -1,     -2,     -4,     -5,     -8,     -9,    -11,    -12,
    // step_index=2, step=9
         1,      2,      4,      5,      9,     10,     12,     13,     -1,     -2,     -4,     -5,     -9,    -10,    -12,    -13,
    // step_index=3, step=10
         1,      2,      5,      6,     10,     11,     14,     15,     -1,     -2,     -5,     -6,    -10,    -11,    -14,    -15,
    // step_index=4, step=11
         1,      3,      5,      7,     11,     13,     15,     17,     -1,     -3,     -5,     -7,    -11,    -13,    -15,    -17,
    // step_index=5, step=12
         1,      3,      6,      8,     12,     14,     17,     19,     -1,     -3,     -6,     -8,    -12,    -14,    -17,    -19,
    // step_index=6, step=13
         1,      3,      6,      8,     13,     15,     18,     20,     -1,     -3,     -6,     -8,    -13,    -15,    -18,    -20,
    // step_index=7, step=14
         1,      4,      7,      9,     14,     17,     20,     22,     -1,     -4,     -7,     -9,    -14,    -17,    -20,    -22,
    // step_index=8, step=16
         2,      4,      8,     10,     16,     18,     22,     24,     -2,     -4,     -8,    -10,    -16,    -18,    -22,    -24,
    // step_index=9, step=17
         2,      4,      8,     10,     17,     19,     23,     25,     -2,     -4,     -8,    -10,    -17,    -19,    -23,    -25,
    // step_index=10, step=19
         2,      5,      9,     12,     19,     22,     26,     29,     -2,     -5,     -9,    -12,    -19,    -22,    -26,    -29,
    // step_index=11, step=21
         2,      5,     10,     13,     21,     24,     29,     32,     -2,     -5,    -10,    -13,    -21,    -24,    -29,    -32,
    // step_index=12, step=23
         2,      6,     11,     14,     23,     27,     32,     35,     -2,     -6,    -11,    -14,    -23,    -27,    -32,    -35,
    // step_index=13, step=25
         3,      6,     12,     15,     25,     28,     34,     37,     -3,     -6,    -12,    -15,    -25,    -28,    -34,    -37,
    // step_index=14, step=28
         3,      7,     14,     17,     28,     32,     39,     42,     -3,     -7,    -14,    -17,    -28,    -32,    -39,    -42,
    // step_index=15, step=31
         3,      8,     15,     19,     31,     36,     43,     47,     -3,     -8,    -15,    -19,    -31,    -36,    -43,    -47,
    // step_index=16, step=34
         4,      8,     17,     21,     34,     38,     47,     51,     -4,     -8,    -17,    -21,    -34,    -38,    -47,    -51,
    // step_index=17, step=37
         4,      9,     18,     23,     37,     42,     51,     56,     -4,     -9,    -18,    -23,    -37,    -42,    -51,    -56,
    // step_index=18, step=41
         5,     10,     20,     25,     41,     46,     56,     61,     -5,    -10,    -20,    -25,    -41,    -46,    -56,    -61,
    // step_index=19, step=45
         5,     11,     22,     28,     45,     51,     62,     68,     -5,    -11,    -22,    -28,    -45,    -51,    -62,    -68,
    // step_index=20, step=50
         6,     12,     25,     31,     50,     56,     69,     75,     -6,    -12,    -25,    -31,    -50,    -56,    -69,    -75,
    // step_index=21, step=55
         6,     14,     27,     34,     55,     62,     76,     83,     -6,    -14,    -27,    -34,    -55,    -62,    -76,    -83,
    // step_index=22, step=60
         7,     15,     30,     37,     60,     67,     82,     90,     -7,    -15,    -30,    -37,    -60,    -67,    -82,    -90,
    // step_index=23, step=66
         8,     16,     33,     41,     66,     74,     91,     99,     -8,    -16,    -33,    -41,    -66,    -74,    -91,    -99,
    // step_index=24, step=73
         9,     18,     36,     45,     73,     82,    100,    109,     -9,    -18,    -36,    -45,    -73,    -82,   -100,   -109,
    // step_index=25, step=80
        10,     20,     40,     50,     80,     90,    110,    120,    -10,    -20,    -40,    -50,    -80,    -90,   -110,   -120,
    // step_index=26, step=88
        11,     22,     44,     55,     88,     99,    121,    132,    -11,    -22,    -44,    -55,    -88,    -99,   -121,   -132,
    // step_index=27, step=97
        12,     24,     48,     60,     97,    109,    133,    145,    -12,    -24,    -48,    -60,    -97,   -109,   -133,   -145,
    // step_index=28, step=107
        13,     27,     53,     67,    107,    121,    147,    161,    -13,    -27,    -53,    -67,   -107,   -121,   -147,   -161,
    // step_index=29, step=118
        14,     29,     59,     73,    118,    132,    162,    177,    -14,    -29,    -59,    -73,   -118,   -132,   -162,   -177,
    // step_index=30, step=130
        16,     32,     65,     81,    130,    146,    179,    195,    -16,    -32,    -65,    -81,   -130,   -146,   -179,   -195,
    // step_index=31, step=143
        17,     36,     71,     89,    143,    161,    196,    214,    -17,    -36,    -71,    -89,   -143,   -161,   -196,   -214,
    // step_index=32, step=157
        19,     39,     78,     98,    157,    176,    216,    235,    -19,    -39,    -78,    -98,   -157,   -176,   -216,   -235,
    // step_index=33, step=173
        21,     43,     86,    108,    173,    195,    238,    260,    -21,    -43,    -86,   -108,   -173,   -195,   -238,   -260,
    // step_index=34, step=190
        23,     48,     95,    119,    190,    214,    261,    285,    -23,    -48,    -95,   -119,   -190,   -214,   -261,   -285,
    // step_index=35, step=209
        26,     52,    104,    130,    209,    235,    287,    313,    -26,    -52,   -104,   -130,   -209,   -235,   -287,   -313,
    // step_index=36, step=230
        28,     58,    115,    144,    230,    259,    316,    345,    -28,    -58,   -115,   -144,   -230,   -259,   -316,   -345,
    // step_index=37, step=253
        31,     63,    126,    158,    253,    285,    348,    380,    -31,    -63,   -126,   -158,   -253,   -285,   -348,   -380,
    // step_index=38, step=279
        34,     70,    139,    174,    279,    314,    383,    418,    -34,    -70,   -139,   -174,   -279,   -314,   -383,   -418,
    // step_index=39, step=307
        38,     77,    153,    191,    307,    345,    421,    460,    -38,    -77,   -153,   -191,   -307,   -345,   -421,   -460,
    // step_index=40, step=337
        42,     84,    168,    210,    337,    379,    463,    505,    -42,    -84,   -168,   -210,   -337,   -379,   -463,   -505,
    // step_index=41, step=371
        46,     93,    185,    232,    371,    418,    510,    557,    -46,    -93,   -185,   -232,   -371,   -418,   -510,   -557,
    // step_index=42, step=408
        51,    102,    204,    255,    408,    459,    561,    612,    -51,   -102,   -204,   -255,   -408,   -459,   -561,   -612,
    // step_index=43, step=449
        56,    112,    224,    280,    449,    505,    617,    673,    -56,   -112,   -224,   -280,   -449,   -505,   -617,   -673,
    // step_index=44, step=494
        61,    124,    247,    309,    494,    556,    679,    741,    -61,   -124,   -247,   -309,   -494,   -556,   -679,   -741,
    // step_index=45, step=544
        68,    136,    272,    340,    544,    612,    748,    816,    -68,   -136,   -272,   -340,   -544,   -612,   -748,   -816,
    // step_index=46, step=598
        74,    150,    299,    374,    598,    673,    822,    897,    -74,   -150,   -299,   -374,   -598,   -673,   -822,   -897,
    // step_index=47, step=658
        82,    164,    329,    411,    658,    740,    905,    987,    -82,   -164,   -329,   -411,   -658,   -740,   -905,   -987,
    // step_index=48, step=724
        90,    181,    362,    452,    724,    814,    996,   1086,    -90,   -181,   -362,   -452,   -724,   -814,   -996,  -1086,
    // step_index=49, step=796
        99,    199,    398,    497,    796,    895,   1094,   1194,    -99,   -199,   -398,   -497,   -796,   -895,  -1094,  -1194,
    // step_index=50, step=876
       109,    219,    438,    547,    876,    985,   1204,   1314,   -109,   -219,   -438,   -547,   -876,   -985,  -1204,  -1314,
    // step_index=51, step=963
       120,    240,    481,    601,    963,   1083,   1324,   1444,   -120,   -240,   -481,   -601,   -963,  -1083,  -1324,  -1444,
    // step_index=52, step=1060
       132,    265,    530,    662,   1060,   1192,   1457,   1590,   -132,   -265,   -530,   -662,  -1060,  -1192,  -1457,  -1590,
    // step_index=53, step=1166
       145,    291,    583,    728,   1166,   1311,   1603,   1749,   -145,   -291,   -583,   -728,  -1166,  -1311,  -1603,  -1749,
    // step_index=54, step=1282
       160,    320,    641,    801,   1282,   1442,   1763,   1923,   -160,   -320,   -641,   -801,  -1282,  -1442,  -1763,  -1923,
    // step_index=55, step=1411
       176,    352,    705,    881,   1411,   1587,   1940,   2116,   -176,   -352,   -705,   -881,  -1411,  -1587,  -1940,  -2116,
    // step_index=56, step=1552
       194,    388,    776,    970,   1552,   1746,   2134,   2328,   -194,   -388,   -776,   -970,  -1552,  -1746,  -2134,  -2328,
    // step_index=57, step=1707
       213,    427,    853,   1067,   1707,   1920,   2346,   2560,   -213,   -427,   -853,  -1067,  -1707,  -1920,  -2346,  -2560,
    // step_index=58, step=1878
       234,    469,    939,   1173,   1878,   2112,   2583,   2817,   -234,   -469,   -939,  -1173,  -1878,  -2112,  -2583,  -2817,
    // step_index=59, step=2066
       258,    516,   1033,   1291,   2066,   2324,   2841,   3099,   -258,   -516,  -1033,  -1291,  -2066,  -2324,  -2841,  -3099,
    // step_index=60, step=2272
       284,    568,   1136,   1420,   2272,   2556,   3124,   3408,   -284,   -568,  -1136,  -1420,  -2272,  -2556,  -3124,  -3408,
    // step_index=61, step=2499
       312,    625,   1249,   1562,   2499,   2811,   3436,   3748,   -312,   -625,  -1249,  -1562,  -2499,  -2811,  -3436,  -3748,
    // step_index=62, step=2749
       343,    687,   1374,   1718,   2749,   3093,   3780,   4123,   -343,   -687,  -1374,  -1718,  -2749,  -3093,  -3780,  -4123,
    // step_index=63, step=3024
       378,    756,   1512,   1890,   3024,   3402,   4158,   4536,   -378,   -756,  -1512,  -1890,  -3024,  -3402,  -4158,  -4536,
    // step_index=64, step=3327
       415,    832,   1663,   2079,   3327,   3743,   4575,   4990,   -415,   -832,  -1663,  -2079,  -3327,  -3743,  -4575,  -4990,
    // step_index=65, step=3660
       457,    915,   1830,   2287,   3660,   4117,   5032,   5490,   -457,   -915,  -1830,  -2287,  -3660,  -4117,  -5032,  -5490,
    // step_index=66, step=4026
       503,   1006,   2013,   2516,   4026,   4529,   5536,   6039,   -503,  -1006,  -2013,  -2516,  -4026,  -4529,  -5536,  -6039,
    // step_index=67, step=4428
       553,   1107,   2214,   2767,   4428,   4981,   5535,   6642,   -553,  -1107,  -2214,  -2767,  -4428,  -4981,  -5535,  -6642,
    // step_index=68, step=4871
       608,   1218,   2435,   3044,   4871,   5480,   6088,   7306,   -608,  -1218,  -2435,  -3044,  -4871,  -5480,  -6088,  -7306,
    // step_index=69, step=5358
       669,   1339,   2679,   3348,   5358,   6027,   6697,   8037,   -669,  -1339,  -2679,  -3348,  -5358,  -6027,  -6697,  -8037,
    // step_index=70, step=5894
       736,   1474,   2947,   3683,   5894,   6631,   7367,   8841,   -736,  -1474,  -2947,  -3683,  -5894,  -6631,  -7367,  -8841,
    // step_index=71, step=6484
       810,   1621,   3242,   4052,   6484,   7294,   8105,   9726,   -810,  -1621,  -3242,  -4052,  -6484,  -7294,  -8105,  -9726,
    // step_index=72, step=7132
       891,   1783,   3566,   4457,   7132,   8023,   8915,  10698,   -891,  -1783,  -3566,  -4457,  -7132,  -8023,  -8915, -10698,
    // step_index=73, step=7845
       980,   1961,   3922,   4903,   7845,   8826,   9807,  11767,   -980,  -1961,  -3922,  -4903,  -7845,  -8826,  -9807, -11767,
    // step_index=74, step=8630
      1078,   2158,   4315,   5394,   8630,   9709,  10787,  12945,  -1078,  -2158,  -4315,  -5394,  -8630,  -9709, -10787, -12945,
    // step_index=75, step=9493
      1186,   2373,   4746,   5933,   9493,  10680,  11866,  14239,  -1186,  -2373,  -4746,  -5933,  -9493, -10680, -11866, -14239,
    // step_index=76, step=10442
      1305,   2610,   5221,   6526,  10442,  11747,  13052,  15663,  -1305,  -2610,  -5221,  -6526, -10442, -11747, -13052, -15663,
    // step_index=77, step=11487
      1435,   2872,   5743,   7179,  11487,  12922,  14358,  17230,  -1435,  -2872,  -5743,  -7179, -11487, -12922, -14358, -17230,
    // step_index=78, step=12635
      1579,   3159,   6317,   7896,  12635,  14214,  15793,  18952,  -1579,  -3159,  -6317,  -7896, -12635, -14214, -15793, -18952,
    // step_index=79, step=13899
      1737,   3475,   6949,   8686,  13899,  15636,  17373,  20848,  -1737,  -3475,  -6949,  -8686, -13899, -15636, -17373, -20848,
    // step_index=80, step=15289
      1911,   3822,   7644,   9555,  15289,  17200,  19111,  22933,  -1911,  -3822,  -7644,  -9555, -15289, -17200, -19111, -22933,
    // step_index=81, step=16818
      2102,   4204,   8409,  10511,  16818,  18920,  21022,  25227,  -2102,  -4204,  -8409, -10511, -16818, -18920, -21022, -25227,
    // step_index=82, step=18500
      2312,   4625,   9250,  11562,  18500,  20812,  23124,  27750,  -2312,  -4625,  -9250, -11562, -18500, -20812, -23124, -27750,
    // step_index=83, step=20350
      2543,   5087,  10175,  12718,  20350,  22893,  25437,  30525,  -2543,  -5087, -10175, -12718, -20350, -22893, -25437, -30525,
    // step_index=84, step=22385
      2798,   5596,  11192,  13990,  22385,  25183,  27981,  32767,  -2798,  -5596, -11192, -13990, -22385, -25183, -27981, -32767,
    // step_index=85, step=24623
      3077,   6156,  12311,  15389,  24623,  27701,  30778,  32767,  -3077,  -6156, -12311, -15389, -24623, -27701, -30778, -32767,
    // step_index=86, step=27086
      3385,   6771,  13543,  16928,  27086,  30471,  32767,  32767,  -3385,  -6771, -13543, -16928, -27086, -30471, -32767, -32767,
    // step_index=87, step=29794
      3724,   7449,  14897,  18621,  29794,  32767,  32767,  32767,  -3724,  -7449, -14897, -18621, -29794, -32767, -32767, -32767,
    // step_index=88, step=32767
      4095,   8191,  16383,  20479,  32767,  32767,  32767,  32767,  -4095,  -8191, -16383, -20479, -32767, -32767, -32767, -32767,
};

// 2-bit ADPCM delta table (from savemu.dll 0x1000e388)
// 356 entries: 89 step levels * 4 codes
__attribute__((section(".iwram.rodata"))) static const int16_t adpcm2_delta_table[356] = {
    3, 10, -3, -10, 4, 12, -4, -12,
    4, 13, -4, -13, 5, 15, -5, -15,
    5, 16, -5, -16, 6, 18, -6, -18,
    6, 19, -6, -19, 7, 21, -7, -21,
    8, 24, -8, -24, 8, 25, -8, -25,
    9, 28, -9, -28, 10, 31, -10, -31,
    11, 34, -11, -34, 12, 37, -12, -37,
    14, 42, -14, -42, 15, 46, -15, -46,
    17, 51, -17, -51, 18, 55, -18, -55,
    20, 61, -20, -61, 22, 67, -22, -67,
    25, 75, -25, -75, 27, 82, -27, -82,
    30, 90, -30, -90, 33, 99, -33, -99,
    36, 109, -36, -109, 40, 120, -40, -120,
    44, 132, -44, -132, 48, 145, -48, -145,
    53, 160, -53, -160, 59, 177, -59, -177,
    65, 195, -65, -195, 71, 214, -71, -214,
    78, 235, -78, -235, 86, 259, -86, -259,
    95, 285, -95, -285, 104, 313, -104, -313,
    115, 345, -115, -345, 126, 379, -126, -379,
    139, 418, -139, -418, 153, 460, -153, -460,
    168, 505, -168, -505, 185, 556, -185, -556,
    204, 612, -204, -612, 224, 673, -224, -673,
    247, 741, -247, -741, 272, 816, -272, -816,
    299, 897, -299, -897, 329, 987, -329, -987,
    362, 1086, -362, -1086, 398, 1194, -398, -1194,
    438, 1314, -438, -1314, 481, 1444, -481, -1444,
    530, 1590, -530, -1590, 583, 1749, -583, -1749,
    641, 1923, -641, -1923, 705, 2116, -705, -2116,
    776, 2328, -776, -2328, 853, 2560, -853, -2560,
    939, 2817, -939, -2817, 1033, 3099, -1033, -3099,
    1136, 3408, -1136, -3408, 1249, 3748, -1249, -3748,
    1374, 4123, -1374, -4123, 1512, 4536, -1512, -4536,
    1663, 4990, -1663, -4990, 1830, 5490, -1830, -5490,
    2013, 6039, -2013, -6039, 2214, 6642, -2214, -6642,
    2435, 7306, -2435, -7306, 2679, 8037, -2679, -8037,
    2947, 8841, -2947, -8841, 3242, 9726, -3242, -9726,
    3566, 10698, -3566, -10698, 3922, 11767, -3922, -11767,
    4315, 12945, -4315, -12945, 4746, 14239, -4746, -14239,
    5221, 15663, -5221, -15663, 5743, 17230, -5743, -17230,
    6317, 18952, -6317, -18952, 6949, 20848, -6949, -20848,
    7644, 22933, -7644, -22933, 8409, 25227, -8409, -25227,
    9250, 27750, -9250, -27750, 10175, 30525, -10175, -30525,
    11179, -31999, -11179, 31999, 12316, -28587, -12316, 28587,
    13543, -24907, -13543, 24907, 14897, -20845, -14897, 20845
};

// ============================================================================
// Internal State
// ============================================================================

// GBS file header structure
typedef struct {
    char magic[4];          // "GBAL"
    uint32_t file_size;
    char marker[4];         // "MUSI"
    uint32_t reserved1;
    uint32_t mode;
    uint32_t reserved2[59]; // Padding to 0x200
} __attribute__((packed)) GbsHeader;

static uint8_t obfuscated_gbs_header_key(uint32_t index) {
    uint32_t value = 0x47425352u + index * 0x1f3du;
    value ^= value >> 13;
    value *= 0x45d9f3bu;
    value ^= value >> 16;
    return (uint8_t)value;
}

static void decode_obfuscated_gbs_header(GbsHeader* header) {
    uint8_t* bytes = (uint8_t*)header;
    for (uint32_t i = 0; i < GBS_HEADER_SIZE; ++i) {
        bytes[i] ^= obfuscated_gbs_header_key(i);
    }
}

// Per-channel decoder state
typedef struct {
    int32_t predictor;      // Current predictor (unsigned 16-bit range for 2/3-bit)
    int32_t step_index;     // Current step index
} ChannelState;

// Internal audio state
static struct {
    // GBS file info
    const uint8_t* gbs_data;
    uint32_t gbs_size;
    bool is_banked;
    uint32_t banked_header_offset;
    uint32_t banked_block_offset;
    GbsAudioInfo info;

    // Decoder state
    ChannelState left;
    ChannelState right;     // Only used for stereo

    // Block tracking - current_block_ptr caches gbs_data + header + block_index * block_size
    const uint8_t* current_block_ptr;
    uint32_t current_block_offset;
    uint32_t block_index;
    uint32_t byte_in_block;
    uint32_t block_header_size;

    // Compressed block ring buffer. Valid range is [cache_start_block, cache_fill_block).
    bool cache_enabled;
    volatile uint32_t cache_start_block;
    volatile uint32_t cache_fill_block;
    volatile uint32_t cache_underflows;

    // Buffered samples for multi-sample decoders
    // Mode 1 (3-bit): 8 samples per 3 bytes
    int16_t buffered_samples[8];
    uint8_t samples_buffered;

    // Mode 2 (4-bit mono): high nibble buffering
    int16_t high_nibble_sample;
    bool have_high_nibble;

    // Playback state
    volatile uint8_t active_buffer;
    bool is_paused;

    // A/V sync: track minute boundaries using addition instead of division
    // samples_per_minute = sample_rate * 60 (precomputed at init)
    // next_minute_sample = threshold for next minute boundary
    // current_audio_minute = which minute we're currently in (0-based)
    uint32_t samples_per_minute;
    uint32_t next_minute_sample;
    uint32_t current_audio_minute;      // Current minute (0, 1, 2, ...)
    volatile int32_t sync_minute;       // New minute to sync to, or -1 if none pending
} state;

// Double buffers for decoded PCM (8-bit signed)
// For stereo: left channel in buffer_left, right in buffer_right
IWRAM_DATA static int8_t audio_buffer_left[AUDIO_BUFFER_COUNT][AUDIO_BUFFER_SAMPLES] __attribute__((aligned(4)));
IWRAM_DATA static int8_t audio_buffer_right[AUDIO_BUFFER_COUNT][AUDIO_BUFFER_SAMPLES] __attribute__((aligned(4)));
EWRAM_BSS static uint8_t gbs_block_cache[GBS_BLOCK_CACHE_BLOCKS][GBS_BLOCK_CACHE_MAX_SIZE] __attribute__((aligned(4)));

// ============================================================================
// ADPCM Decoding Functions
// ============================================================================

// Decode single 4-bit IMA ADPCM sample using lookup table
static IWRAM_CODE int16_t decode_ima_4bit(uint8_t nibble, ChannelState* ch) {
    // Single table lookup replaces branch-heavy diff calculation
    int diff = ima_diff_table[(ch->step_index << 4) + nibble];
    ch->predictor += diff;

    // Clamp to signed 16-bit
    if (ch->predictor > 32767) ch->predictor = 32767;
    else if (ch->predictor < -32768) ch->predictor = -32768;

    // Update step index
    ch->step_index += ima_index_table[nibble];
    if (ch->step_index < 0) ch->step_index = 0;
    else if (ch->step_index > 88) ch->step_index = 88;

    return (int16_t)ch->predictor;
}

// Decode single 3-bit ADPCM sample
static IWRAM_CODE int16_t decode_adpcm_3bit(uint8_t code, ChannelState* ch) {
    int step = ima_step_table[ch->step_index];

    int diff = step >> 2;
    if (code & 2) diff += step;
    if (code & 1) diff += step >> 1;

    if (code & 4) {
        ch->predictor -= diff;
    } else {
        ch->predictor += diff;
    }

    // Clamp to unsigned 16-bit (0-65535)
    if (ch->predictor < 0) ch->predictor = 0;
    else if (ch->predictor > 65535) ch->predictor = 65535;

    // Update step index
    ch->step_index += adpcm3_index_table[code & 7];
    if (ch->step_index < 0) ch->step_index = 0;
    else if (ch->step_index > 88) ch->step_index = 88;

    // Return as signed (centered at 0x8000)
    return (int16_t)(ch->predictor - 0x8000);
}

// Decode single 2-bit ADPCM sample
static IWRAM_CODE int16_t decode_adpcm_2bit(uint8_t code, ChannelState* ch) {
    int32_t table_index = code + ch->step_index;
    if (table_index > 352) table_index = 352;

    int16_t delta = adpcm2_delta_table[table_index];
    ch->predictor += delta;

    // Clamp to unsigned 16-bit
    if (ch->predictor < 0) ch->predictor = 0;
    else if (ch->predictor > 65535) ch->predictor = 65535;

    // Update step index: bit0=1 -> +4, bit0=0 -> -4
    if (code & 1) {
        ch->step_index += 4;
        if (ch->step_index > 0x160) ch->step_index = 0x160;
    } else {
        ch->step_index -= 4;
        if (ch->step_index < 0) ch->step_index = 0;
    }

    return (int16_t)(ch->predictor - 0x8000);
}

// ============================================================================
// Block Management
// ============================================================================

static IWRAM_CODE uint8_t* cache_slot(uint32_t block_index) {
    return gbs_block_cache[block_index & (GBS_BLOCK_CACHE_BLOCKS - 1)];
}

static IWRAM_CODE const uint8_t* cache_lookup(uint32_t block_index) {
    if (!state.cache_enabled) {
        return NULL;
    }

    const uint32_t start = state.cache_start_block;
    const uint32_t fill = state.cache_fill_block;
    if (block_index < start || block_index >= fill) {
        state.cache_underflows++;
        return NULL;
    }
    if (block_index - start >= GBS_BLOCK_CACHE_BLOCKS) {
        state.cache_underflows++;
        return NULL;
    }
    return cache_slot(block_index);
}

static IWRAM_CODE void cache_retire_to(uint32_t block_index) {
    if (!state.cache_enabled) {
        return;
    }

    if (block_index > state.cache_start_block) {
        state.cache_start_block = block_index;
    }
    if (state.cache_fill_block < state.cache_start_block) {
        state.cache_fill_block = state.cache_start_block;
    }
}

static void cache_reset(uint32_t block_index) {
    state.cache_start_block = block_index;
    state.cache_fill_block = block_index;
    state.cache_underflows = 0;
}

static void copy_source_block(uint32_t block_index, uint8_t* dst) {
    const uint32_t size = state.info.block_size;
    if (state.is_banked) {
        const uint32_t rom_offset = state.banked_block_offset + block_index * size;
        const uint8_t* src = banked_rom_ptr_irq(rom_offset);
        memcpy(dst, src, size);
    } else {
        const uint8_t* src = state.gbs_data + GBS_HEADER_SIZE + block_index * size;
        memcpy(dst, src, size);
    }
}

static void cache_fill_blocks(uint32_t max_blocks) {
    if (!state.cache_enabled || state.info.total_blocks == 0) {
        return;
    }

    const u32 saved_bank = banked_current_bank();
    uint32_t copied = 0;

    while (copied < max_blocks) {
        const uint32_t start = state.cache_start_block;
        const uint32_t fill = state.cache_fill_block;
        if (fill >= state.info.total_blocks || fill - start >= GBS_BLOCK_CACHE_BLOCKS) {
            break;
        }

        uint8_t* dst = cache_slot(fill);
        copy_source_block(fill, dst);

        if (state.cache_fill_block == fill &&
            fill >= state.cache_start_block &&
            fill - state.cache_start_block < GBS_BLOCK_CACHE_BLOCKS) {
            state.cache_fill_block = fill + 1;
            copied++;
        }
    }

    if (state.is_banked) {
        banked_select(saved_bank);
    }
}

static IWRAM_CODE const uint8_t* get_current_block(void) {
    const uint8_t* cached = cache_lookup(state.block_index);
    if (cached) {
        return cached;
    }

    if (state.is_banked) {
        return banked_rom_ptr_irq(state.current_block_offset);
    }
    return state.current_block_ptr;
}

static IWRAM_CODE void set_current_block(uint32_t block_index) {
    state.block_index = block_index;
    state.byte_in_block = 0;
    cache_retire_to(block_index);
    if (state.is_banked) {
        state.current_block_offset =
            state.banked_block_offset + block_index * state.info.block_size;
    } else {
        state.current_block_ptr =
            state.gbs_data + GBS_HEADER_SIZE + block_index * state.info.block_size;
    }
}

static IWRAM_CODE void parse_block_header_mono(const uint8_t* block, ChannelState* ch) {
    uint16_t predictor = block[0] | (block[1] << 8);
    uint16_t step_idx = block[2] | (block[3] << 8);

    // Mode 2 uses IMA ADPCM with signed predictor
    if (state.info.mode == GBS_MODE_MONO_4BIT) {
        ch->predictor = (int16_t)(predictor - 0x8000);
    } else {
        ch->predictor = predictor;
    }
    ch->step_index = step_idx;

    // Clamp step index based on mode
    if (state.info.mode == GBS_MODE_MONO_2BIT ||
        state.info.mode == GBS_MODE_MONO_2BIT_SM) {
        if (ch->step_index > 0x160) ch->step_index = 0x160;
    } else {
        if (ch->step_index > 88) ch->step_index = 88;
    }
}

static IWRAM_CODE void parse_block_header_stereo(const uint8_t* block) {
    // Left channel: bytes 0-3
    uint16_t pred_l = block[0] | (block[1] << 8);
    uint16_t step_l = block[2] | (block[3] << 8);
    state.left.predictor = (int16_t)(pred_l - 0x8000);
    state.left.step_index = (step_l > 88) ? 88 : step_l;

    // Right channel: bytes 4-7
    uint16_t pred_r = block[4] | (block[5] << 8);
    uint16_t step_r = block[6] | (block[7] << 8);
    state.right.predictor = (int16_t)(pred_r - 0x8000);
    state.right.step_index = (step_r > 88) ? 88 : step_r;
}

static IWRAM_CODE void advance_to_next_block(void) {
    const uint32_t next_block = state.block_index + 1;
    if (next_block >= state.info.total_blocks) {
        state.block_index = next_block;
        state.byte_in_block = 0;
        state.info.is_finished = true;
        return;
    }

    set_current_block(next_block);
    const uint8_t* block = get_current_block();

    if (state.info.channels == 2) {
        parse_block_header_stereo(block);
    } else {
        parse_block_header_mono(block, &state.left);
    }
}

// ============================================================================
// Buffer Decoding Functions
// ============================================================================

// Mode 0: Stereo 4-bit IMA ADPCM
static IWRAM_CODE void decode_buffer_stereo_4bit(int8_t* left, int8_t* right, uint32_t count) {
    const uint8_t* data = get_current_block() + state.block_header_size;
    uint32_t data_per_block = state.info.block_size - state.block_header_size;
    uint32_t byte_pos = state.byte_in_block;
    uint32_t decoded = 0;

    while (!state.info.is_finished && decoded < count) {
        uint32_t remaining_in_block = data_per_block - byte_pos;
        uint32_t remaining_to_decode = count - decoded;
        uint32_t to_decode = remaining_in_block < remaining_to_decode ? remaining_in_block : remaining_to_decode;

        for (uint32_t j = 0; j < to_decode; j++) {
            uint32_t byte = data[byte_pos++];
            left[decoded] = (int8_t)(decode_ima_4bit(byte & 0x0F, &state.left) >> 8);
            right[decoded] = (int8_t)(decode_ima_4bit(byte >> 4, &state.right) >> 8);
            decoded++;
        }

        if (byte_pos >= data_per_block) {
            state.byte_in_block = byte_pos;
            advance_to_next_block();
            data = get_current_block() + state.block_header_size;
            byte_pos = 0;
        }
    }

    // Fill remaining with silence if finished early
    while (decoded < count) {
        left[decoded] = 0;
        right[decoded] = 0;
        decoded++;
    }

    state.byte_in_block = byte_pos;
    state.info.samples_decoded += decoded;
}

// Mode 1: Mono 3-bit ADPCM (8 samples per 3 bytes)
static IWRAM_CODE void decode_buffer_mono_3bit(int8_t* dest, uint32_t count) {
    const uint8_t* data = get_current_block() + state.block_header_size;
    uint32_t data_per_block = state.info.block_size - state.block_header_size;
    uint32_t byte_pos = state.byte_in_block;
    uint32_t decoded = 0;

    // First, drain any buffered samples from previous call
    while (state.samples_buffered > 0 && decoded < count) {
        dest[decoded++] = (int8_t)(state.buffered_samples[8 - state.samples_buffered] >> 8);
        state.samples_buffered--;
    }

    // Main decode loop: process 8 samples at a time
    while (!state.info.is_finished && decoded + 8 <= count) {
        if (byte_pos + 3 > data_per_block) {
            state.byte_in_block = byte_pos;
            advance_to_next_block();
            if (state.info.is_finished) break;
            data = get_current_block() + state.block_header_size;
            byte_pos = 0;
        }

        // Encoder packs MSB-first: sample7 at top, sample0 at bottom
        // Then writes big-endian: byte[0]=bits23-16, byte[1]=bits15-8, byte[2]=bits7-0
        uint32_t packed = (data[byte_pos] << 16) | (data[byte_pos + 1] << 8) | data[byte_pos + 2];
        byte_pos += 3;

        // Decode 8 samples from LSB (sample 0) to MSB (sample 7)
        dest[decoded++] = (int8_t)(decode_adpcm_3bit(packed & 0x07, &state.left) >> 8);
        packed >>= 3;
        dest[decoded++] = (int8_t)(decode_adpcm_3bit(packed & 0x07, &state.left) >> 8);
        packed >>= 3;
        dest[decoded++] = (int8_t)(decode_adpcm_3bit(packed & 0x07, &state.left) >> 8);
        packed >>= 3;
        dest[decoded++] = (int8_t)(decode_adpcm_3bit(packed & 0x07, &state.left) >> 8);
        packed >>= 3;
        dest[decoded++] = (int8_t)(decode_adpcm_3bit(packed & 0x07, &state.left) >> 8);
        packed >>= 3;
        dest[decoded++] = (int8_t)(decode_adpcm_3bit(packed & 0x07, &state.left) >> 8);
        packed >>= 3;
        dest[decoded++] = (int8_t)(decode_adpcm_3bit(packed & 0x07, &state.left) >> 8);
        packed >>= 3;
        dest[decoded++] = (int8_t)(decode_adpcm_3bit(packed & 0x07, &state.left) >> 8);
    }

    // Handle remaining samples (less than 8 needed)
    if (!state.info.is_finished && decoded < count) {
        if (byte_pos + 3 > data_per_block) {
            state.byte_in_block = byte_pos;
            advance_to_next_block();
            if (!state.info.is_finished) {
                data = get_current_block() + state.block_header_size;
                byte_pos = 0;
            }
        }
        if (!state.info.is_finished) {
            // Big-endian read (same as main loop)
            uint32_t packed = (data[byte_pos] << 16) | (data[byte_pos + 1] << 8) | data[byte_pos + 2];
            byte_pos += 3;

            state.buffered_samples[0] = decode_adpcm_3bit(packed & 0x07, &state.left);
            packed >>= 3;
            state.buffered_samples[1] = decode_adpcm_3bit(packed & 0x07, &state.left);
            packed >>= 3;
            state.buffered_samples[2] = decode_adpcm_3bit(packed & 0x07, &state.left);
            packed >>= 3;
            state.buffered_samples[3] = decode_adpcm_3bit(packed & 0x07, &state.left);
            packed >>= 3;
            state.buffered_samples[4] = decode_adpcm_3bit(packed & 0x07, &state.left);
            packed >>= 3;
            state.buffered_samples[5] = decode_adpcm_3bit(packed & 0x07, &state.left);
            packed >>= 3;
            state.buffered_samples[6] = decode_adpcm_3bit(packed & 0x07, &state.left);
            packed >>= 3;
            state.buffered_samples[7] = decode_adpcm_3bit(packed & 0x07, &state.left);

            state.samples_buffered = 8;
            while (state.samples_buffered > 0 && decoded < count) {
                dest[decoded++] = (int8_t)(state.buffered_samples[8 - state.samples_buffered] >> 8);
                state.samples_buffered--;
            }
        }
    }

    while (decoded < count) dest[decoded++] = 0;

    state.byte_in_block = byte_pos;
    state.info.samples_decoded += decoded;
}

// Mode 2: Mono 4-bit IMA ADPCM
static IWRAM_CODE void decode_buffer_mono_4bit(int8_t* dest, uint32_t count) {
    const uint8_t* data = get_current_block() + state.block_header_size;
    uint32_t data_per_block = state.info.block_size - state.block_header_size;
    uint32_t byte_pos = state.byte_in_block;
    uint32_t decoded = 0;

    // Drain buffered high nibble from previous call
    if (state.have_high_nibble && decoded < count) {
        dest[decoded++] = (int8_t)(state.high_nibble_sample >> 8);
        state.have_high_nibble = false;
    }

    // Main loop: decode 2 samples per byte
    while (!state.info.is_finished && decoded + 2 <= count) {
        if (byte_pos >= data_per_block) {
            state.byte_in_block = byte_pos;
            advance_to_next_block();
            if (state.info.is_finished) break;
            data = get_current_block() + state.block_header_size;
            byte_pos = 0;
        }

        uint32_t byte = data[byte_pos++];
        dest[decoded++] = (int8_t)(decode_ima_4bit(byte & 0x0F, &state.left) >> 8);
        dest[decoded++] = (int8_t)(decode_ima_4bit(byte >> 4, &state.left) >> 8);
    }

    // Handle odd sample at end
    if (!state.info.is_finished && decoded < count) {
        if (byte_pos >= data_per_block) {
            state.byte_in_block = byte_pos;
            advance_to_next_block();
            if (!state.info.is_finished) {
                data = get_current_block() + state.block_header_size;
                byte_pos = 0;
            }
        }
        if (!state.info.is_finished) {
            uint32_t byte = data[byte_pos++];
            dest[decoded++] = (int8_t)(decode_ima_4bit(byte & 0x0F, &state.left) >> 8);
            state.high_nibble_sample = decode_ima_4bit(byte >> 4, &state.left);
            state.have_high_nibble = true;
        }
    }

    while (decoded < count) dest[decoded++] = 0;

    state.byte_in_block = byte_pos;
    state.info.samples_decoded += decoded;
}

// Mode 3/4: Mono 2-bit ADPCM (4 samples per byte)
static IWRAM_CODE void decode_buffer_mono_2bit(int8_t* dest, uint32_t count) {
    const uint8_t* data = get_current_block() + state.block_header_size;
    uint32_t data_per_block = state.info.block_size - state.block_header_size;
    uint32_t byte_pos = state.byte_in_block;
    uint32_t decoded = 0;

    // Drain buffered samples from previous call
    while (state.samples_buffered > 0 && decoded < count) {
        dest[decoded++] = (int8_t)(state.buffered_samples[4 - state.samples_buffered] >> 8);
        state.samples_buffered--;
    }

    // Main loop: decode 4 samples per byte
    while (!state.info.is_finished && decoded + 4 <= count) {
        if (byte_pos >= data_per_block) {
            state.byte_in_block = byte_pos;
            advance_to_next_block();
            if (state.info.is_finished) break;
            data = get_current_block() + state.block_header_size;
            byte_pos = 0;
        }

        uint32_t byte = data[byte_pos++];
        dest[decoded++] = (int8_t)(decode_adpcm_2bit(byte & 0x03, &state.left) >> 8);
        byte >>= 2;
        dest[decoded++] = (int8_t)(decode_adpcm_2bit(byte & 0x03, &state.left) >> 8);
        byte >>= 2;
        dest[decoded++] = (int8_t)(decode_adpcm_2bit(byte & 0x03, &state.left) >> 8);
        byte >>= 2;
        dest[decoded++] = (int8_t)(decode_adpcm_2bit(byte & 0x03, &state.left) >> 8);
    }

    // Handle remaining samples (less than 4 needed)
    if (!state.info.is_finished && decoded < count) {
        if (byte_pos >= data_per_block) {
            state.byte_in_block = byte_pos;
            advance_to_next_block();
            if (!state.info.is_finished) {
                data = get_current_block() + state.block_header_size;
                byte_pos = 0;
            }
        }
        if (!state.info.is_finished) {
            uint32_t byte = data[byte_pos++];
            state.buffered_samples[0] = decode_adpcm_2bit(byte & 0x03, &state.left);
            byte >>= 2;
            state.buffered_samples[1] = decode_adpcm_2bit(byte & 0x03, &state.left);
            byte >>= 2;
            state.buffered_samples[2] = decode_adpcm_2bit(byte & 0x03, &state.left);
            byte >>= 2;
            state.buffered_samples[3] = decode_adpcm_2bit(byte & 0x03, &state.left);

            state.samples_buffered = 4;
            while (state.samples_buffered > 0 && decoded < count) {
                dest[decoded++] = (int8_t)(state.buffered_samples[4 - state.samples_buffered] >> 8);
                state.samples_buffered--;
            }
        }
    }

    while (decoded < count) dest[decoded++] = 0;

    state.byte_in_block = byte_pos;
    state.info.samples_decoded += decoded;
}

// Dispatch to appropriate decoder
// Note: For mono modes, right is always NULL (DMA2 not used), so no need to clear it
static IWRAM_CODE void decode_buffer(int8_t* left, int8_t* right, uint32_t count) {
    switch (state.info.mode) {
        case GBS_MODE_STEREO_4BIT:
            decode_buffer_stereo_4bit(left, right, count);
            break;
        case GBS_MODE_MONO_3BIT:
            decode_buffer_mono_3bit(left, count);
            break;
        case GBS_MODE_MONO_4BIT:
            decode_buffer_mono_4bit(left, count);
            break;
        case GBS_MODE_MONO_2BIT:
        case GBS_MODE_MONO_2BIT_SM:
            decode_buffer_mono_2bit(left, count);
            break;
        default:
            memset(left, 0, count);
            break;
    }
}

static IWRAM_CODE void decode_buffer_preserve_bank(int8_t* left, int8_t* right, uint32_t count) {
    const u32 saved_bank = banked_current_bank();
    decode_buffer(left, right, count);
    if (state.is_banked) {
        banked_select_irq(saved_bank);
    }
}

// ============================================================================
// Interrupt Handler
// ============================================================================

static IWRAM_CODE void audio_timer1_handler(void) {
    REG_IF = IRQ_TIMER1;

    if (state.info.is_finished) {
        REG_DMA1CNT = 0;
        REG_DMA2CNT = 0;
        state.info.is_playing = false;
        return;
    }

    // Swap buffers
    uint8_t play_buffer = state.active_buffer;
    uint8_t decode_buffer_idx = play_buffer ^ 1;
    state.active_buffer = decode_buffer_idx;

    // Restart DMA for new buffer
    REG_DMA1CNT = 0;
    REG_DMA1SAD = (uint32_t)audio_buffer_left[decode_buffer_idx];
    REG_DMA1DAD = (uint32_t)&REG_FIFO_A;
    REG_DMA1CNT = DMA_DST_FIXED | DMA_SRC_INC | DMA_REPEAT | DMA32 | DMA_SPECIAL | DMA_ENABLE;

    if (state.info.channels == 2) {
        REG_DMA2CNT = 0;
        REG_DMA2SAD = (uint32_t)audio_buffer_right[decode_buffer_idx];
        REG_DMA2DAD = (uint32_t)&REG_FIFO_B;
        REG_DMA2CNT = DMA_DST_FIXED | DMA_SRC_INC | DMA_REPEAT | DMA32 | DMA_SPECIAL | DMA_ENABLE;
    }

    // Decode into buffer that just finished playing
    decode_buffer_preserve_bank(audio_buffer_left[play_buffer],
                                state.info.channels == 2 ? audio_buffer_right[play_buffer] : NULL,
                                AUDIO_BUFFER_SAMPLES);

    // Check if we crossed a minute boundary (using comparison instead of division)
    if (state.info.samples_decoded >= state.next_minute_sample) {
        // Crossed into next minute
        state.current_audio_minute++;
        state.next_minute_sample += state.samples_per_minute;
        // Signal sync to the new minute
        state.sync_minute = (int32_t)state.current_audio_minute;
    }
}

static bool init_from_header(const GbsHeader* header, uint32_t gbs_size) {
    if (gbs_size < GBS_HEADER_SIZE) {
        return false;
    }

    if (memcmp(header->magic, "GBAL", 4) != 0 ||
        memcmp(header->marker, "MUSI", 4) != 0) {
        return false;
    }

    if (header->mode > 4) {
        return false;
    }

    // Configure based on mode
    state.info.mode = (GbsMode)header->mode;

    switch (state.info.mode) {
        case GBS_MODE_STEREO_4BIT:
            state.info.sample_rate = 22050;
            state.info.channels = 2;
            state.info.block_size = 0x400;
            state.block_header_size = 8;  // 4 bytes per channel
            break;
        case GBS_MODE_MONO_3BIT:
            state.info.sample_rate = 44100;  // 11:1 compression vs 44.1kHz 16-bit stereo
            state.info.channels = 1;
            state.info.block_size = 0x400;
            state.block_header_size = 4;
            break;
        case GBS_MODE_MONO_4BIT:
            state.info.sample_rate = 22050;
            state.info.channels = 1;
            state.info.block_size = 0x200;
            state.block_header_size = 4;
            break;
        case GBS_MODE_MONO_2BIT:
            state.info.sample_rate = 22050;
            state.info.channels = 1;
            state.info.block_size = 0x200;
            state.block_header_size = 4;
            break;
        case GBS_MODE_MONO_2BIT_SM:
            state.info.sample_rate = 11025;
            state.info.channels = 1;
            state.info.block_size = 0x100;
            state.block_header_size = 4;
            break;
        default:
            return false;
    }

    // Calculate totals
    uint32_t data_size = gbs_size - GBS_HEADER_SIZE;
    state.info.total_blocks = data_size / state.info.block_size;

    // Calculate samples per block based on mode
    uint32_t data_per_block = state.info.block_size - state.block_header_size;
    uint32_t samples_per_block;

    switch (state.info.mode) {
        case GBS_MODE_STEREO_4BIT:
            samples_per_block = data_per_block;  // 1 sample pair per byte
            break;
        case GBS_MODE_MONO_3BIT:
            samples_per_block = (data_per_block / 3) * 8;  // 8 samples per 3 bytes
            break;
        case GBS_MODE_MONO_4BIT:
            samples_per_block = data_per_block * 2;  // 2 samples per byte
            break;
        case GBS_MODE_MONO_2BIT:
        case GBS_MODE_MONO_2BIT_SM:
            samples_per_block = data_per_block * 4;  // 4 samples per byte
            break;
        default:
            samples_per_block = 0;
    }

    state.info.total_samples = state.info.total_blocks * samples_per_block;

    state.cache_enabled = (state.info.total_blocks > 0);
    cache_reset(0);
    cache_fill_blocks(GBS_BLOCK_CACHE_BLOCKS);

    set_current_block(0);

    // Initialize first block
    if (state.info.total_blocks > 0) {
        const uint8_t* block = get_current_block();
        if (state.info.channels == 2) {
            parse_block_header_stereo(block);
        } else {
            parse_block_header_mono(block, &state.left);
        }
    }

    state.info.is_finished = (state.info.total_blocks == 0);

    // Initialize A/V sync tracking
    // Precompute samples_per_minute to avoid runtime multiplication
    state.samples_per_minute = state.info.sample_rate * 60;
    state.next_minute_sample = state.samples_per_minute;  // First boundary at minute 1
    state.current_audio_minute = 0;  // Start at minute 0
    state.sync_minute = -1;  // No sync pending initially

    return true;
}

// ============================================================================
// Public API
// ============================================================================

bool gbs_audio_init(const uint8_t* gbs_data, uint32_t gbs_size) {
    memset(&state, 0, sizeof(state));

    state.gbs_data = gbs_data;
    state.gbs_size = gbs_size;
    state.info.mode = GBS_MODE_INVALID;

    if (gbs_size < GBS_HEADER_SIZE) {
        return false;
    }

    return init_from_header((const GbsHeader*)gbs_data, gbs_size);
}

bool gbs_audio_init_banked(uint32_t header_offset, uint32_t block_offset, uint32_t gbs_size) {
    const u32 saved_bank = banked_current_bank();
    memset(&state, 0, sizeof(state));

    state.is_banked = true;
    state.gbs_size = gbs_size;
    state.banked_header_offset = header_offset;
    state.banked_block_offset = block_offset;
    state.info.mode = GBS_MODE_INVALID;

    GbsHeader header;
    banked_copy(header_offset, &header, sizeof(header));
    if (banked_video_is_metadata_obfuscated()) {
        decode_obfuscated_gbs_header(&header);
    }
    const bool ok = init_from_header(&header, gbs_size);
    banked_select(saved_bank);
    return ok;
}

void gbs_audio_start(void) {
    if (state.info.mode == GBS_MODE_INVALID || state.info.is_finished) {
        return;
    }

    cache_fill_blocks(GBS_BLOCK_CACHE_BLOCKS);

    // Pre-decode both buffers
    decode_buffer_preserve_bank(audio_buffer_left[0],
                                state.info.channels == 2 ? audio_buffer_right[0] : NULL,
                                AUDIO_BUFFER_SAMPLES);
    decode_buffer_preserve_bank(audio_buffer_left[1],
                                state.info.channels == 2 ? audio_buffer_right[1] : NULL,
                                AUDIO_BUFFER_SAMPLES);
    cache_fill_blocks(GBS_BLOCK_CACHE_BLOCKS);

    state.active_buffer = 0;

    // Calculate timer reload
    uint16_t timer_reload = 65536 - (GBA_MASTER_CLOCK / state.info.sample_rate);

    // Enable sound
    REG_SOUNDCNT_X = SOUNDCNT_X_ENABLE;

    if (state.info.channels == 2) {
        // Stereo: Channel A = left, Channel B = right
        REG_SOUNDCNT_H = DSOUNDCTRL_DMG100 |
                         DSOUNDCTRL_A100 | DSOUNDCTRL_AL | DSOUNDCTRL_ATIMER(0) | DSOUNDCTRL_ARESET |
                         DSOUNDCTRL_B100 | DSOUNDCTRL_BR | DSOUNDCTRL_BTIMER(0) | DSOUNDCTRL_BRESET;
    } else {
        // Mono: Channel A only, output to both speakers
        REG_SOUNDCNT_H = DSOUNDCTRL_DMG100 |
                         DSOUNDCTRL_A100 | DSOUNDCTRL_AR | DSOUNDCTRL_AL |
                         DSOUNDCTRL_ATIMER(0) | DSOUNDCTRL_ARESET;
    }
    REG_SOUNDCNT_L = 0;

    // Clear FIFOs
    for (int i = 0; i < 16; i++) {
        REG_FIFO_A = 0;
        REG_FIFO_B = 0;
    }

    // Setup Timer0 for sample rate
    REG_TM0CNT_H = 0;
    REG_TM0CNT_L = timer_reload;
    REG_TM0CNT_H = TIMER_START;

    // Setup Timer1 cascade for buffer swap
    REG_TM1CNT_H = 0;
    REG_TM1CNT_L = 65536 - AUDIO_BUFFER_SAMPLES;
    REG_TM1CNT_H = TIMER_IRQ | TIMER_CASCADE | TIMER_START;

    // Setup interrupt
    irqSet(IRQ_TIMER1, audio_timer1_handler);
    irqEnable(IRQ_TIMER1);

    // Start DMA
    REG_DMA1CNT = 0;
    REG_DMA1SAD = (uint32_t)audio_buffer_left[0];
    REG_DMA1DAD = (uint32_t)&REG_FIFO_A;
    REG_DMA1CNT = DMA_DST_FIXED | DMA_SRC_INC | DMA_REPEAT | DMA32 | DMA_SPECIAL | DMA_ENABLE;

    if (state.info.channels == 2) {
        REG_DMA2CNT = 0;
        REG_DMA2SAD = (uint32_t)audio_buffer_right[0];
        REG_DMA2DAD = (uint32_t)&REG_FIFO_B;
        REG_DMA2CNT = DMA_DST_FIXED | DMA_SRC_INC | DMA_REPEAT | DMA32 | DMA_SPECIAL | DMA_ENABLE;
    }

    state.info.is_playing = true;
}

void gbs_audio_update(void) {
    cache_fill_blocks(GBS_BLOCK_CACHE_UPDATE_MAX);
}

void gbs_audio_stop(void) {
    REG_DMA1CNT = 0;
    REG_DMA2CNT = 0;
    REG_TM0CNT_H = 0;
    REG_TM1CNT_H = 0;
    irqDisable(IRQ_TIMER1);
    REG_SOUNDCNT_X = 0;
    state.info.is_playing = false;
    state.is_paused = false;
}

void gbs_audio_pause(void) {
    if (!state.info.is_playing || state.is_paused) return;

    // Stop timers (this stops DMA triggers)
    REG_TM0CNT_H = 0;
    REG_TM1CNT_H = 0;

    state.is_paused = true;
}

void gbs_audio_resume(void) {
    if (!state.info.is_playing || !state.is_paused) return;

    uint16_t timer_reload = 65536 - (GBA_MASTER_CLOCK / state.info.sample_rate);

    // Reset FIFOs to clear any stale data
    if (state.info.channels == 2) {
        REG_SOUNDCNT_H = DSOUNDCTRL_DMG100 |
                         DSOUNDCTRL_A100 | DSOUNDCTRL_AL | DSOUNDCTRL_ATIMER(0) | DSOUNDCTRL_ARESET |
                         DSOUNDCTRL_B100 | DSOUNDCTRL_BR | DSOUNDCTRL_BTIMER(0) | DSOUNDCTRL_BRESET;
    } else {
        REG_SOUNDCNT_H = DSOUNDCTRL_DMG100 |
                         DSOUNDCTRL_A100 | DSOUNDCTRL_AR | DSOUNDCTRL_AL |
                         DSOUNDCTRL_ATIMER(0) | DSOUNDCTRL_ARESET;
    }

    // Restart DMA from current buffer position
    REG_DMA1CNT = 0;
    REG_DMA1SAD = (uint32_t)audio_buffer_left[state.active_buffer];
    REG_DMA1DAD = (uint32_t)&REG_FIFO_A;
    REG_DMA1CNT = DMA_DST_FIXED | DMA_SRC_INC | DMA_REPEAT | DMA32 | DMA_SPECIAL | DMA_ENABLE;

    if (state.info.channels == 2) {
        REG_DMA2CNT = 0;
        REG_DMA2SAD = (uint32_t)audio_buffer_right[state.active_buffer];
        REG_DMA2DAD = (uint32_t)&REG_FIFO_B;
        REG_DMA2CNT = DMA_DST_FIXED | DMA_SRC_INC | DMA_REPEAT | DMA32 | DMA_SPECIAL | DMA_ENABLE;
    }

    // Restart timers
    REG_TM0CNT_H = 0;
    REG_TM0CNT_L = timer_reload;
    REG_TM0CNT_H = TIMER_START;

    REG_TM1CNT_H = 0;
    REG_TM1CNT_L = 65536 - AUDIO_BUFFER_SAMPLES;
    REG_TM1CNT_H = TIMER_IRQ | TIMER_CASCADE | TIMER_START;

    state.is_paused = false;
}

bool gbs_audio_is_paused(void) {
    return state.is_paused;
}

void gbs_audio_restart(void) {
    gbs_audio_stop();
    const u32 saved_bank = banked_current_bank();

    // Reset decoder state
    state.block_index = 0;
    state.byte_in_block = 0;
    state.info.samples_decoded = 0;
    state.info.is_finished = false;
    cache_reset(0);
    cache_fill_blocks(GBS_BLOCK_CACHE_BLOCKS);
    set_current_block(0);

    // Re-parse first block header
    if (state.info.total_blocks > 0) {
        const uint8_t* block = get_current_block();
        if (state.info.channels == 2) {
            parse_block_header_stereo(block);
        } else {
            parse_block_header_mono(block, &state.left);
        }
    }

    gbs_audio_start();
    if (state.is_banked) {
        banked_select(saved_bank);
    }
}

bool gbs_audio_is_playing(void) {
    return state.info.is_playing;
}

bool gbs_audio_is_finished(void) {
    return state.info.is_finished;
}

uint32_t gbs_audio_get_progress(void) {
    if (state.info.total_samples == 0) return 0;
    return (state.info.samples_decoded * 100) / state.info.total_samples;
}

const GbsAudioInfo* gbs_audio_get_info(void) {
    return &state.info;
}

void gbs_audio_shutdown(void) {
    gbs_audio_stop();
    memset(&state, 0, sizeof(state));
    state.info.mode = GBS_MODE_INVALID;
}

void gbs_audio_seek_minute(uint32_t minute) {
    if (state.info.mode == GBS_MODE_INVALID) return;

    gbs_audio_stop();
    const u32 saved_bank = banked_current_bank();

    // Calculate target block based on minute
    // samples_per_minute = sample_rate * 60
    uint32_t samples_per_minute = state.info.sample_rate * 60;
    uint32_t target_sample = minute * samples_per_minute;

    // Clamp to valid range
    if (target_sample >= state.info.total_samples) {
        target_sample = 0;  // Wrap to beginning
        minute = 0;
    }

    // Calculate samples per block
    uint32_t data_per_block = state.info.block_size - state.block_header_size;
    uint32_t samples_per_block;

    switch (state.info.mode) {
        case GBS_MODE_STEREO_4BIT:
            samples_per_block = data_per_block;
            break;
        case GBS_MODE_MONO_3BIT:
            samples_per_block = (data_per_block / 3) * 8;
            break;
        case GBS_MODE_MONO_4BIT:
            samples_per_block = data_per_block * 2;
            break;
        case GBS_MODE_MONO_2BIT:
        case GBS_MODE_MONO_2BIT_SM:
            samples_per_block = data_per_block * 4;
            break;
        default:
            samples_per_block = 1;
    }

    // Calculate target block index
    uint32_t target_block = target_sample / samples_per_block;
    if (target_block >= state.info.total_blocks) {
        target_block = 0;
    }

    // Reset decoder state to target block
    cache_reset(target_block);
    cache_fill_blocks(GBS_BLOCK_CACHE_BLOCKS);
    set_current_block(target_block);
    state.info.samples_decoded = target_block * samples_per_block;
    state.info.is_finished = false;
    state.samples_buffered = 0;
    state.have_high_nibble = false;

    // Reset sync tracking for new position
    // next_minute_sample = (minute + 1) * samples_per_minute
    // Use addition loop to avoid multiplication
    state.next_minute_sample = 0;
    for (uint32_t i = 0; i <= minute; i++) {
        state.next_minute_sample += state.samples_per_minute;
    }
    state.current_audio_minute = minute;
    state.sync_minute = -1;  // Clear any pending sync

    // Parse block header
    const uint8_t* block = get_current_block();
    if (state.info.channels == 2) {
        parse_block_header_stereo(block);
    } else {
        parse_block_header_mono(block, &state.left);
    }

    gbs_audio_start();
    if (state.is_banked) {
        banked_select(saved_bank);
    }
}

uint32_t gbs_audio_get_current_minute(void) {
    if (state.info.sample_rate == 0) return 0;
    return state.info.samples_decoded / (state.info.sample_rate * 60);
}

uint32_t gbs_audio_get_total_minutes(void) {
    if (state.info.sample_rate == 0) return 0;
    return (state.info.total_samples + state.info.sample_rate * 60 - 1) / (state.info.sample_rate * 60);
}

int32_t gbs_audio_check_minute_sync(void) {
    int32_t minute = state.sync_minute;
    if (minute >= 0) {
        state.sync_minute = -1;  // Clear pending flag
    }
    return minute;
}
