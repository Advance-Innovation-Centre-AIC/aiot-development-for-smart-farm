/*
 * sf3_02_practise.c -- Code Quest ระดับ 3: เติมช่อง A B C ใน is_event()
 *
 * โปรแกรมหูฟังเล้าไก่เดียวกับ sf3_02_coop_ears แต่ is_event() มีช่องว่าง
 * สามช่อง (A B C) ให้เติม ก่อนเปิดจอโปรแกรมตรวจ is_event() 5 กรณี
 * ผ่าน = Console ขึ้น "ผ่าน!" แล้วฟังเล้าต่อ ไม่ผ่าน = บอกกรณีที่ผิด
 * แล้วจบ ติดขัด? ดูใบงาน เฉลย: practise/solutions/
 * sf3_02_practise_solution (ลองเองก่อน)
 *
 * The same coop ears as sf3_02_coop_ears, but is_event() has three
 * blanks (A, B, C) to fill in. Before the screen opens the program
 * checks is_event() on 5 cases: pass = the Console shows "ผ่าน!" and
 * the ears run; fail = it names the wrong case and ends. Stuck? See
 * the worksheet; the answer is in practise/solutions (try first).
 *
 * Twin of sf3_02_practise.py
 * (aiot-development-for-smart-farm/s3/practise/sf3_02_practise.py).
 * Style: CLEAN_C.md.
 * Differs from the original:
 *   - Each blank ____ is BLANK_A, BLANK_B or BLANK_C (-1, never a
 *     right answer): C does not compile an empty blank, so the
 *     self-test says ยังไม่ถูก until the blank is filled, where
 *     MicroPython stops with a NameError.
 *   - The C API has no mic module (contract_map.json: missing). The
 *     few lines of the board's frozen mic.py that the program uses
 *     are written out here over bento_machine_pdm_*: the same rate,
 *     decimation, MONO_RIGHT channel, sensitivity table, two
 *     discarded reads at start, stats() and the log2 level().
 *     bento_machine_pdm is a PSoC Edge API: no reTerminal run.
 *   - gc.collect(): C has no garbage collector; the call is dropped.
 *   - hasattr(ui, "volume"): the volume is always set, and a build
 *     without sound refuses it, which is ignored.
 *   - try/except OSError around the RGB matrix: its status is not
 *     read, as mx() passes it; the 30 s list of loud moments is a
 *     fixed array sized for the most a 30 s window can hold.
 *   - A self-test case is printed as print() prints its values.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_fmt.h"
#include "bento_machine.h"
#include "bento_pots.h"
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_time.h"

/* ---- แก้ก่อนรัน: settings (MISRA deviation CT02) ---- */
enum {
    VOLUME = 25,
    SPEAKER = 40,
    SENS = 3, /* microphone sensitivity 1-5 */
    PEAK_MIN = 3000,
    PEAK_SPAN = 27000,  /* VR3 sets 3000-30000 */
    PEAK_FULL = 32768,  /* full scale; the .py never reads it either */
    EVENT_GAP_MS = 400, /* ms before a sound may count again */
    MUTE_MS = 800,      /* ms deaf after our own speaker */
    WINDOW_MS = 30000,  /* ms of the count looking back */
    ALARM_EVENTS = 5,   /* loud moments = panic */
    QUIET_LV = 20,
    RUN_MS = 90000,
    TICK_MS = 500,
    MIC_MS = 20,
    MATRIX_MS = 3000,
    SCROLL_MS = 80
};
/* ---- end of settings ---- */

/* The board's microphone, as the frozen mic.py opens it. */
enum {
    MIC_RATE = 16000,
    MIC_DECIMATION = 96,
    MIC_BITS = 16,
    MIC_SAMPLES = 256,
    MIC_BUFFER_BYTES = MIC_SAMPLES * 2, /* 16-bit samples */
    MIC_READ_TIMEOUT_MS = 2000,
    MIC_DISCARDS = 2, /* the first reads after start are not steady */
    MIC_SETTLE_MS = 20,
    LEVEL_FLOOR = 16,     /* rms below this is level 0 */
    LEVEL_LOW_Q8 = 1024,  /* log2(16) x 256 */
    LEVEL_SPAN_Q8 = 2816, /* log2(32768) x 256 minus LEVEL_LOW_Q8 */
    LOG2_Q8_ONE = 256,
    LOG2_MANTISSA_BITS = 9,
    PERCENT = 100
};

/* The self-test's numbers. */
enum {
    TEST_NOW_MS = 100000,
    TEST_PEAK = 5000, /* the threshold of every case */
    TEST_STEP = 1000,
    TEST_SINCE_MS = 1000
};

/* The blanks to fill in: -1 is never a right answer. */
enum {
    BLANK_A = -1,
    BLANK_B = -1,
    BLANK_C = -1
};

/* MIDI notes of the beeps. */
enum {
    NOTE_E5 = 76,
    NOTE_G5 = 79,
    NOTE_C6 = 84
};

/* Hardware, timing and the matrix. */
enum {
    THRESHOLD_POT = 2, /* VR3 */
    POT_FULL = 4095,
    BEEP_MS = 120,
    SCREEN_SETTLE_MS = 200,
    MATRIX_BYTES = 64,
    MATRIX_BOTTOM_ROW = 56, /* first byte of the bottom row */
    MATRIX_LEVELS = 8,
    MATRIX_YELLOW_FROM = 4,
    NIBBLE_BITS = 4,
    PANIC_FRAME = -1,
    EVENTS_MAX = (WINDOW_MS / EVENT_GAP_MS) + 2,
    TEXT_BYTES = 128
};

/* Layout on the 800 x 400 canvas. */
enum {
    TITLE_X = 12,
    TITLE_Y = 6,
    TITLE_PX = 24,
    MOOD_X = 200,
    MOOD_Y = 10,
    MOOD_PX = 20,
    TEXT_PX = 16,
    CARD_Y = 44,
    CARD_H = 150,
    CARD_RADIUS = 12,
    CARD_BORDER = 1,
    CARD_TITLE_DX = 12,
    CARD_TITLE_DY = 6,
    SEG7_Y = 72,
    SEG7_H = 56,
    LOUD_X = 12,
    LOUD_W = 250,
    LEVEL_X = 24,
    LEVEL_W = 120,
    BAR_Y = 138,
    BAR_W = 226,
    BAR_H = 18,
    LOWER_Y = 164,
    PEAK_CARD_X = 272,
    PEAK_CARD_W = 300,
    PEAK_X = 284,
    PEAK_W = 150,
    THRESHOLD_X = 444,
    THRESHOLD_Y = 76,
    VR3_Y = 100,
    WINDOW_CARD_X = 582,
    WINDOW_CARD_W = 198,
    WINDOW_X = 594,
    WINDOW_W = 90,
    OUT_OF_X = 694,
    OUT_OF_Y = 90,
    TOTAL_Y = 136,
    ALARMS_Y = 162,
    LED_X = 12,
    LED_Y = 216,
    LED_SIZE = 24,
    EAR_X = 44,
    EAR_Y = 218,
    HELP_X = 12,
    HELP_Y = 352
};

/* Colours, 0xRRGGBB. */
enum {
    COLOR_TEXT = 0xE8EAED,
    COLOR_DIM = 0x9AA3AF,
    COLOR_CARD = 0x171B22,
    COLOR_OK = 0x30A46C,
    COLOR_WARN = 0xF5A623,
    COLOR_BAD = 0xE5484D,
    COLOR_INFO = 0x4A9EFF
};

/* The microphone: open or not, its handle. */
typedef struct {
    bool open;
    uint32_t handle;
} mic_t;

typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
} rect_t;

/* The widgets the program changes after it has drawn them. */
typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t mood;
    bento_ui_id_t level;
    bento_ui_id_t bar;
    bento_ui_id_t average;
    bento_ui_id_t peak;
    bento_ui_id_t threshold;
    bento_ui_id_t window;
    bento_ui_id_t total;
    bento_ui_id_t alarms;
    bento_ui_id_t led;
    bento_ui_id_t ear;
    bento_ui_id_t help;
} ears_screen_t;

/* The ears: the loud moments of the last 30 s and when to listen. */
typedef struct {
    uint32_t events[EVENTS_MAX];
    uint32_t event_count;
    int32_t total;
    uint32_t last_ms;
    uint32_t mute_until_ms;
    int32_t top;
} ears_t;

/* The farm's mood over the whole run. */
typedef struct {
    bento_real_t average;
    int32_t alarms;
    bool panic;
    bool has_drawn;
    int32_t drawn;
    uint32_t sent_ms;
} coop_t;

/* One read of samples, the microphone's work area: file scope, zeroed
 * by the loader on every load and set again at the start of main()
 * (CLEAN_C.md C9, deviation BENTO-MISRA-E03/E04). */
extern uint8_t g_mic_samples[MIC_BUFFER_BYTES];
uint8_t g_mic_samples[MIC_BUFFER_BYTES];

static int16_t px(int32_t value)
{
    return (int16_t)value;
}

/* Sleeps, and turns Stop into BENTO_E_CANCELLED. */
static bento_status_t pause_ms(uint32_t duration_ms)
{
    bento_status_t status = bento_time_sleep_ms(duration_ms);

    if ((status == BENTO_OK) && !bento_runtime_is_running()) {
        status = BENTO_E_CANCELLED;
    }
    return status;
}

/* ui.poll() here only keeps the queue empty; it answers no events
 * when it fails. */
static void drain_events(bento_ui_t *ui)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    (void)bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                        NULL);
}

/* A setter's refusal is ignored, as MicroPython's widget setters
 * ignore it (modui.c). */
static void set_text(bento_ui_t *ui, bento_ui_id_t widget,
                     const char *text)
{
    (void)bento_ui_set_text(ui, widget, text, (uint32_t)strlen(text));
}

static void set_number(bento_ui_t *ui, bento_ui_id_t widget,
                       const char *format, int32_t value)
{
    char text[TEXT_BYTES];

    (void)snprintf(text, sizeof(text), format, value);
    set_text(ui, widget, text);
}

static bento_status_t add_label(bento_ui_t *ui, const char *text,
                                const rect_t *place, uint32_t color,
                                int32_t size_px, bento_ui_id_t *out_id)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.text = text;
    args.text_len = (uint32_t)strlen(text);
    args.x = place->x;
    args.y = place->y;
    args.color = color;
    args.font_px = size_px;
    return bento_ui_label_create(ui, &args, out_id);
}

/* A label whose id nobody needs again. */
static bento_status_t add_text(bento_ui_t *ui, const char *text,
                               int32_t x, int32_t y, uint32_t color,
                               int32_t size_px)
{
    const rect_t place = {px(x), px(y), 0, 0};
    bento_ui_id_t unused;

    return add_label(ui, text, &place, color, size_px, &unused);
}

/* A dark card with its title. */
static bento_status_t add_card(bento_ui_t *ui, int32_t x, int32_t w,
                               const char *title)
{
    bento_ui_widget_args_t args;
    bento_ui_id_t unused;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = px(x);
    args.y = CARD_Y;
    args.w = px(w);
    args.h = CARD_H;
    args.color = COLOR_CARD;
    args.min_val = COLOR_DIM; /* a panel's min= is its border */
    args.max_val = CARD_RADIUS;
    args.font_px = CARD_BORDER;
    status = bento_ui_panel_create(ui, &args, &unused);
    if (status == BENTO_OK) {
        status = add_text(ui, title, x + CARD_TITLE_DX,
                          CARD_Y + CARD_TITLE_DY, COLOR_INFO, TEXT_PX);
    }
    return status;
}

static bento_status_t add_seg7(bento_ui_t *ui, const rect_t *place,
                               uint32_t color, bento_ui_id_t *out_id)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.text = "0";
    args.text_len = (uint32_t)strlen(args.text);
    args.x = place->x;
    args.y = place->y;
    args.w = place->w;
    args.h = place->h;
    args.color = color;
    return bento_ui_seg7_create(ui, &args, out_id);
}

/* Soft MIDI notes (ui.sfx cannot be made quieter), 120 ms apart.
 * ui.tone() is sent and forgotten, so its status is not read. */
static bento_status_t beep(bento_ui_t *ui, uint8_t first,
                           uint8_t second)
{
    bento_status_t status;

    (void)bento_ui_sfx_tone(ui, first, (uint8_t)BENTO_UI_WAVE_SINE,
                            (uint8_t)VOLUME, (uint16_t)BEEP_MS);
    status = pause_ms(BEEP_MS);
    if (status == BENTO_OK) {
        (void)bento_ui_sfx_tone(ui, second, (uint8_t)BENTO_UI_WAVE_SINE,
                                (uint8_t)VOLUME, (uint16_t)BEEP_MS);
        status = pause_ms(BEEP_MS);
    }
    return status;
}

/* The peak threshold from VR3. */
static bento_status_t read_threshold(int32_t *threshold)
{
    uint16_t raw = 0U;
    const bento_status_t status = bento_pots_read(THRESHOLD_POT, &raw);

    *threshold =
        (int32_t)PEAK_MIN +
        (((int32_t)raw * (int32_t)PEAK_SPAN) / (int32_t)POT_FULL);
    return status;
}

/* mic.start(sens): the chip's gain scale runs backwards (0 = +83 dB,
 * 25 = -67 dB), so sensitivity 1-5 maps to 17 down to 9. */
static bento_status_t mic_start(mic_t *mic, int32_t sensitivity)
{
    static const uint32_t gains[] = {17U, 15U, 13U, 11U, 9U};
    const uint32_t gain = ((sensitivity >= 1) && (sensitivity <= 5))
                              ? gains[sensitivity - 1]
                              : gains[2];
    bento_machine_pdm_cfg_t config;
    uint32_t got = 0U;
    int32_t i;
    bento_status_t status;

    bento_machine_pdm_cfg_init(&config);
    config.sample_rate = MIC_RATE;
    config.decimation_rate = MIC_DECIMATION;
    config.bits = MIC_BITS;
    /* P8_5 / P8_6 are PDM channel 3: the right channel */
    config.format = BENTO_MACHINE_PDM_MONO_RIGHT;
    config.left_gain = gain;
    config.right_gain = gain;
    status = bento_machine_pdm_open(&config, &mic->handle);
    mic->open = status == BENTO_OK;
    if (status == BENTO_OK) {
        status = bento_machine_pdm_start(mic->handle);
    }
    for (i = 0; (i < MIC_DISCARDS) && (status == BENTO_OK); i++) {
        /* a first read that fails is thrown away like the rest */
        (void)bento_machine_pdm_read(mic->handle, g_mic_samples,
                                     MIC_BUFFER_BYTES,
                                     MIC_READ_TIMEOUT_MS, &got);
        status = pause_ms(MIC_SETTLE_MS);
    }
    return status;
}

/* mic.stats(): one fresh read; rms and peak of it without its DC. */
static bento_status_t mic_stats(const mic_t *mic,
                                bento_machine_pdm_stats_t *stats)
{
    bento_machine_pdm_stats_init(stats);
    return bento_machine_pdm_stats(mic->handle, g_mic_samples,
                                   MIC_BUFFER_BYTES, true, stats);
}

/* log2(value) x 256 in whole numbers; the mantissa is read
 * linearly, within 3 % of the true value. */
static int32_t log2_q8(uint32_t value)
{
    uint32_t rest = value;
    int32_t bits = 0;
    uint32_t mantissa;

    while (rest != 0U) {
        rest >>= 1U;
        bits++;
    }
    mantissa = (bits <= LOG2_MANTISSA_BITS)
                   ? (value << (uint32_t)(LOG2_MANTISSA_BITS - bits))
                   : (value >> (uint32_t)(bits - LOG2_MANTISSA_BITS));
    return ((bits - 1) * LOG2_Q8_ONE) +
           ((int32_t)mantissa - LOG2_Q8_ONE);
}

/* mic.level(): loudness 0-100 on the ear's scale, in octaves from
 * rms 16 to full scale (quiet 7, talk 54, a clap 92-100). */
static bento_status_t mic_level(const mic_t *mic, int32_t *level)
{
    bento_machine_pdm_stats_t stats;
    const bento_status_t status = mic_stats(mic, &stats);
    int32_t value = 0;

    if ((status == BENTO_OK) && (stats.rms >= (uint32_t)LEVEL_FLOOR)) {
        value = ((log2_q8(stats.rms) - LEVEL_LOW_Q8) * PERCENT) /
                LEVEL_SPAN_Q8;
    }
    if (value < 0) {
        value = 0;
    } else if (value > PERCENT) {
        value = PERCENT;
    } else {
        /* already on the scale */
    }
    *level = value;
    return status;
}

static void mic_stop(mic_t *mic)
{
    if (mic->open) {
        /* mic.stop() passes any failure of deinit() */
        (void)bento_machine_pdm_close(mic->handle);
    }
    mic->open = false;
}

/* -1 = PANIC, 0-8 = loudness on the bottom row. A refused picture
 * is drawn again at the next change or MATRIX_MS resend. */
static void draw_matrix(int32_t frame)
{
    static const char panic_text[] = "PANIC";
    uint8_t picture[MATRIX_BYTES];
    const uint8_t color = (frame >= MATRIX_YELLOW_FROM)
                              ? (uint8_t)BENTO_RGBMATRIX_YELLOW
                              : (uint8_t)BENTO_RGBMATRIX_GREEN;
    int32_t x;

    if (frame < 0) {
        (void)bento_rgbmatrix_scroll(panic_text,
                                     (uint32_t)strlen(panic_text),
                                     BENTO_RGBMATRIX_RED, SCROLL_MS);
    } else {
        (void)memset(picture, 0, sizeof(picture));
        for (x = 0; x < (frame * 2); x++) {
            /* two pixels a byte, the left one in the low nibble */
            const uint32_t shift =
                (uint32_t)NIBBLE_BITS * ((uint32_t)x % 2U);

            picture[MATRIX_BOTTOM_ROW + (x / 2)] |=
                (uint8_t)(color << shift);
        }
        (void)bento_rgbmatrix_blit(picture, MATRIX_BYTES);
    }
}

/* A new ears: nothing counted, listening from now. */
static void ears_init(ears_t *ears)
{
    const uint32_t now_ms = bento_time_ticks_ms();

    ears->event_count = 0U;
    ears->total = 0;
    ears->last_ms = bento_time_ticks_add(now_ms, -EVENT_GAP_MS);
    ears->mute_until_ms = now_ms;
    ears->top = 0;
}

/* A sudden loud sound counts when the ears are not muted, the peak
 * reaches the threshold and the last one is far enough back. */
static bool is_event(int32_t peak, int32_t threshold, uint32_t now_ms,
                     uint32_t last_ms, uint32_t mute_until_ms)
{
    /* while a blank is empty a parameter may go unused, which the
     * build refuses: each one is marked used here */
    (void)peak;
    (void)threshold;
    (void)now_ms;
    (void)last_ms;
    (void)mute_until_ms;
    /* ช่อง A: พ้นช่วงปิดหูแล้ว ผลต่างเวลาต้องไม่ติดลบ */
    return (bento_time_ticks_diff(now_ms, mute_until_ms) >= BLANK_A) &&
           /* ช่อง B: ยอดเสียงต้องถึง "อะไร"? */
           (peak >= BLANK_B) &&
           /* ช่อง C: ห่างครั้งก่อนอย่างน้อยกี่ ms? (ส่วน 1) */
           (bento_time_ticks_diff(now_ms, last_ms) >= BLANK_C);
}

/* "True" or "False", as print() writes a bool. */
static const char *bool_text(bool value)
{
    return value ? "True" : "False";
}

/* One case of the self-test (threshold 5000): the peak, how far
 * back the last event is, how long the ears stay deaf, the answer. */
typedef struct {
    int32_t peak;
    int32_t since_ms;
    int32_t mute_ms;
    bool want;
} event_case_t;

/* Checks is_event() on 5 cases; the first sits on every edge. */
static bool self_test(void)
{
    static const event_case_t cases[] = {
        {TEST_PEAK, EVENT_GAP_MS, 0, true},
        {TEST_PEAK - 1, EVENT_GAP_MS, 1, false},
        {TEST_PEAK + TEST_STEP, EVENT_GAP_MS - 1, 1, false},
        {TEST_PEAK + TEST_STEP, TEST_SINCE_MS, -1, false},
        {TEST_PEAK + TEST_STEP, TEST_SINCE_MS, 1, true}};
    const uint32_t count = (uint32_t)(sizeof(cases) / sizeof(cases[0]));
    const uint32_t now_ms = TEST_NOW_MS;
    bool passed = true;
    uint32_t i;

    for (i = 0U; (i < count) && passed; i++) {
        const event_case_t *check = &cases[i];
        const bool got =
            is_event(check->peak, TEST_PEAK, now_ms,
                     bento_time_ticks_add(now_ms, -check->since_ms),
                     bento_time_ticks_add(now_ms, -check->mute_ms));

        passed = got == check->want;
        if (!passed) {
            (void)printf("ยังไม่ถูก: ยอด %" PRId32 " ห่าง %" PRId32
                         " ปิดหูอีก %" PRId32 " ควรได้ %s\n",
                         check->peak, check->since_ms, -check->mute_ms,
                         bool_text(check->want));
        }
    }
    if (passed) {
        (void)printf("ผ่าน! is_event() ถูกทั้ง 5 กรณี\n");
    }
    return passed;
}

static void remember_event(ears_t *ears, uint32_t now_ms)
{
    uint32_t i;

    if (ears->event_count == (uint32_t)EVENTS_MAX) {
        for (i = 1U; i < ears->event_count; i++) {
            ears->events[i - 1U] = ears->events[i];
        }
        ears->event_count--;
    }
    ears->events[ears->event_count] = now_ms;
    ears->event_count++;
    ears->last_ms = now_ms;
    ears->total++;
}

/* One read: the loudest peak so far, and a loud moment counted. */
static void hear(ears_t *ears, int32_t peak, int32_t threshold)
{
    const uint32_t now_ms = bento_time_ticks_ms();

    if (peak > ears->top) {
        ears->top = peak;
    }
    if (is_event(peak, threshold, now_ms, ears->last_ms,
                 ears->mute_until_ms)) {
        remember_event(ears, now_ms);
    }
}

/* Listens for duration_ms: the loudest peak, and each loud moment. */
static bento_status_t listen(ears_t *ears, const mic_t *mic,
                             uint32_t duration_ms, int32_t threshold)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_machine_pdm_stats_t stats;
    bento_status_t status = BENTO_OK;

    ears->top = 0;
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            (int32_t)duration_ms)) {
        status = mic_stats(mic, &stats);
        if (status == BENTO_OK) {
            hear(ears, (int32_t)stats.peak, threshold);
            status = pause_ms(MIC_MS);
        }
    }
    return status;
}

/* Every time the speaker sounds, the ears close for MUTE_MS. */
static void mute(ears_t *ears)
{
    ears->mute_until_ms =
        bento_time_ticks_add(bento_time_ticks_ms(), MUTE_MS);
}

static bool listening(const ears_t *ears)
{
    return bento_time_ticks_diff(bento_time_ticks_ms(),
                                 ears->mute_until_ms) >= 0;
}

/* Forgets the loud moments older than 30 s. */
static void forget_old(ears_t *ears, uint32_t now_ms)
{
    uint32_t old = 0U;
    uint32_t i;

    while ((old < ears->event_count) &&
           (bento_time_ticks_diff(now_ms, ears->events[old]) >
            WINDOW_MS)) {
        old++;
    }
    for (i = old; i < ears->event_count; i++) {
        ears->events[i - old] = ears->events[i];
    }
    ears->event_count -= old;
}

static const char *mood(bool panic, int32_t average, uint32_t *color)
{
    const char *text = "Normal coop sound";

    *color = COLOR_OK;
    if (panic) {
        text = "PANIC! Check the coop";
        *color = COLOR_BAD;
    } else if (average < QUIET_LV) {
        text = "Quiet - hens resting";
        *color = COLOR_DIM;
    } else {
        /* normal */
    }
    return text;
}

/* The loudness card and the peak card. */
static bento_status_t build_meters(ears_screen_t *screen)
{
    const rect_t level = {LEVEL_X, SEG7_Y, LEVEL_W, SEG7_H};
    const rect_t average = {LEVEL_X, LOWER_Y, 0, 0};
    const rect_t peak = {PEAK_X, SEG7_Y, PEAK_W, SEG7_H};
    const rect_t threshold = {THRESHOLD_X, THRESHOLD_Y, 0, 0};
    bento_ui_widget_args_t args;
    bento_status_t status =
        add_card(screen->ui, LOUD_X, LOUD_W, "Loudness 0-100");

    if (status == BENTO_OK) {
        status = add_seg7(screen->ui, &level, COLOR_OK, &screen->level);
    }
    if (status == BENTO_OK) {
        bento_ui_widget_args_init(&args);
        args.x = LEVEL_X;
        args.y = BAR_Y;
        args.w = BAR_W;
        args.h = BAR_H;
        args.min_val = 0;
        args.max_val = PERCENT;
        status = bento_ui_bar_create(screen->ui, &args, &screen->bar);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "avg 0", &average, COLOR_DIM,
                           TEXT_PX, &screen->average);
    }
    if (status == BENTO_OK) {
        status = add_card(screen->ui, PEAK_CARD_X, PEAK_CARD_W,
                          "Raw peak vs threshold");
    }
    if (status == BENTO_OK) {
        status = add_seg7(screen->ui, &peak, COLOR_INFO, &screen->peak);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "th -", &threshold, COLOR_TEXT,
                           TEXT_PX, &screen->threshold);
    }
    if (status == BENTO_OK) {
        status = add_text(screen->ui, "(VR3)", THRESHOLD_X, VR3_Y,
                          COLOR_DIM, TEXT_PX);
    }
    if (status == BENTO_OK) {
        status = add_text(screen->ui, "red = counted", PEAK_X, LOWER_Y,
                          COLOR_DIM, TEXT_PX);
    }
    return status;
}

/* The 30 s window card. */
static bento_status_t build_window(ears_screen_t *screen)
{
    const rect_t window = {WINDOW_X, SEG7_Y, WINDOW_W, SEG7_H};
    const rect_t total = {WINDOW_X, TOTAL_Y, 0, 0};
    const rect_t alarms = {WINDOW_X, ALARMS_Y, 0, 0};
    char text[TEXT_BYTES];
    bento_status_t status = add_card(screen->ui, WINDOW_CARD_X,
                                     WINDOW_CARD_W, "Loud in 30 s");

    if (status == BENTO_OK) {
        status =
            add_seg7(screen->ui, &window, COLOR_WARN, &screen->window);
    }
    if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text), "/ %d", ALARM_EVENTS);
        status = add_text(screen->ui, text, OUT_OF_X, OUT_OF_Y,
                          COLOR_DIM, MOOD_PX);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "total 0", &total, COLOR_TEXT,
                           TEXT_PX, &screen->total);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "alarms 0", &alarms, COLOR_DIM,
                           TEXT_PX, &screen->alarms);
    }
    return status;
}

/* The listening light, its words and the help line. */
static bento_status_t build_ear(ears_screen_t *screen)
{
    const rect_t ear = {EAR_X, EAR_Y, 0, 0};
    const rect_t help = {HELP_X, HELP_Y, 0, 0};
    bento_ui_widget_args_t args;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = LED_X;
    args.y = LED_Y;
    args.w = LED_SIZE;
    args.h = LED_SIZE;
    args.color = COLOR_OK;
    args.font_px = 0;
    status = bento_ui_led_create(screen->ui, &args, &screen->led);
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "mic starting", &ear, COLOR_DIM,
                           TEXT_PX, &screen->ear);
    }
    if (status == BENTO_OK) {
        status =
            add_label(screen->ui,
                      "VR3 = threshold   clap 5-6 times, ~0.5 s apart",
                      &help, COLOR_DIM, TEXT_PX, &screen->help);
    }
    return status;
}

static bento_status_t build_screen(ears_screen_t *screen)
{
    const rect_t mood_place = {MOOD_X, MOOD_Y, 0, 0};
    bento_status_t status =
        bento_ui_screen(screen->ui, BENTO_RUNTIME_SCREEN_WIDTH_PX,
                        BENTO_RUNTIME_SCREEN_HEIGHT_PX);

    if (status == BENTO_OK) {
        status = pause_ms(SCREEN_SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_text(screen->ui, "Coop Ears", TITLE_X, TITLE_Y,
                          COLOR_TEXT, TITLE_PX);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "Starting mic...", &mood_place,
                           COLOR_WARN, MOOD_PX, &screen->mood);
    }
    if (status == BENTO_OK) {
        status = build_meters(screen);
    }
    if (status == BENTO_OK) {
        status = build_window(screen);
    }
    if (status == BENTO_OK) {
        status = build_ear(screen);
    }
    if (status == BENTO_OK) {
        drain_events(screen->ui);
    }
    return status;
}

static void show(const ears_screen_t *screen, int32_t level,
                 int32_t threshold, const ears_t *ears,
                 const coop_t *coop)
{
    bento_ui_t *ui = screen->ui;
    const int32_t average = (int32_t)coop->average;
    uint32_t color = COLOR_OK;
    const bool on = listening(ears);

    set_text(ui, screen->mood, mood(coop->panic, average, &color));
    (void)bento_ui_set_color(ui, screen->mood, color);
    set_number(ui, screen->level, "%" PRId32, level);
    (void)bento_ui_set_value(ui, screen->bar, level);
    set_number(ui, screen->average, "avg %" PRId32, average);
    set_number(ui, screen->peak, "%" PRId32, ears->top);
    (void)bento_ui_set_color(ui, screen->peak,
                             (ears->top >= threshold) ? COLOR_BAD
                                                      : COLOR_INFO);
    set_number(ui, screen->threshold, "th %" PRId32, threshold);
    set_number(ui, screen->window, "%" PRId32,
               (int32_t)ears->event_count);
    (void)bento_ui_set_color(ui, screen->window,
                             coop->panic ? COLOR_BAD : COLOR_WARN);
    set_number(ui, screen->total, "total %" PRId32, ears->total);
    set_number(ui, screen->alarms, "alarms %" PRId32, coop->alarms);
    (void)bento_ui_set_value(ui, screen->led, on ? 1 : 0);
    set_text(ui, screen->ear, on ? "listening" : "muted (own beep)");
}

/* The sound only when the state changes; every sound mutes the ears
 * for MUTE_MS. */
static bento_status_t on_mood(const ears_screen_t *screen, ears_t *ears,
                              coop_t *coop)
{
    const bool was_panic = coop->panic;
    bento_status_t status = BENTO_OK;

    coop->panic = ears->event_count >= (uint32_t)ALARM_EVENTS;
    if (coop->panic != was_panic) {
        status =
            coop->panic
                ? beep(screen->ui, (uint8_t)NOTE_C6, (uint8_t)NOTE_E5)
                : beep(screen->ui, (uint8_t)NOTE_G5, (uint8_t)NOTE_C6);
        mute(ears);
        if (coop->panic) {
            coop->alarms++;
            (void)printf("ฝูงตื่นตกใจ! เสียงดัง %" PRIu32 " ครั้งใน 30 วินาที\n",
                         ears->event_count);
        }
    }
    return status;
}

/* Drawn when the frame changes, and sent again every MATRIX_MS. */
static void matrix_tick(coop_t *coop, int32_t level, uint32_t now_ms)
{
    const int32_t frame =
        coop->panic
            ? (int32_t)PANIC_FRAME
            : ((level * (int32_t)MATRIX_LEVELS) / (int32_t)PERCENT);

    if (!coop->has_drawn || (frame != coop->drawn) ||
        (bento_time_ticks_diff(now_ms, coop->sent_ms) >= MATRIX_MS)) {
        draw_matrix(frame);
        coop->has_drawn = true;
        coop->drawn = frame;
        coop->sent_ms = now_ms;
    }
}

/* One TICK_MS round: listen, decide, act, show. */
static bento_status_t ears_round(const ears_screen_t *screen,
                                 const mic_t *mic, ears_t *ears,
                                 coop_t *coop)
{
    const bento_real_t keep = (bento_real_t)0.7;
    const bento_real_t take = (bento_real_t)0.3;
    int32_t threshold = 0;
    int32_t level = 0;
    bento_real_t kept;
    bento_status_t status = read_threshold(&threshold);

    if (status == BENTO_OK) {
        status = listen(ears, mic, TICK_MS, threshold);
    }
    if (status == BENTO_OK) {
        status = mic_level(mic, &level);
    }
    if (status == BENTO_OK) {
        kept = coop->average * keep;
        coop->average = kept + ((bento_real_t)level * take);
        forget_old(ears, bento_time_ticks_ms());
        status = on_mood(screen, ears, coop);
    }
    if (status == BENTO_OK) {
        matrix_tick(coop, level, bento_time_ticks_ms());
        show(screen, level, threshold, ears, coop);
        drain_events(screen->ui);
    }
    return status;
}

/* Until RUN_MS; whatever ends it, the microphone is closed and the
 * matrix cleared. */
static bento_status_t run_coop(const ears_screen_t *screen, mic_t *mic,
                               ears_t *ears)
{
    coop_t coop;
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    (void)memset(&coop, 0, sizeof(coop));
    coop.sent_ms = start_ms;
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = ears_round(screen, mic, ears, &coop);
    }
    mic_stop(mic);
    (void)bento_rgbmatrix_clear(); /* mx() passes a refusal */
    if (status == BENTO_OK) {
        set_text(screen->ui, screen->help,
                 "Done - press Program to Device to run again");
        drain_events(screen->ui);
        (void)printf("ครบเวลา: เสียงดัง %" PRId32 " ครั้ง เตือน %" PRId32
                     " รอบ\n",
                     ears->total, coop.alarms);
    }
    return status;
}

int main(void)
{
    ears_screen_t screen;
    ears_t ears;
    mic_t mic = {false, 0U};
    bento_status_t status = BENTO_OK;

    (void)memset(g_mic_samples, 0, sizeof(g_mic_samples));
    (void)memset(&screen, 0, sizeof(screen));
    screen.ui = bento_runtime_get_ui();
    /* a build without sound refuses it, as hasattr() skips it */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    if (self_test()) {
        status = mic_start(&mic, SENS); /* the mic before the screen */
        if (status == BENTO_OK) {
            status = build_screen(&screen);
        }
        if (status == BENTO_OK) {
            (void)bento_rgbmatrix_clear(); /* mx() passes a refusal */
            ears_init(&ears);
            status = run_coop(&screen, &mic, &ears);
        }
    }
    mic_stop(&mic);
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
