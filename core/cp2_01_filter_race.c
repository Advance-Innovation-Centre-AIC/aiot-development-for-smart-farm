/*
 * cp2_01_filter_race.c -- หลักการ 2.1: สัญญาณรบกวนมีหลายรูป
 * เลือกตัวกรองให้ตรงรูป
 *
 * ค่าจากเซนเซอร์มี "สั่นเล็ก ๆ" (jitter) กับ "กระโดดทีเดียว" (spike) ปนมา
 * EMA เฉลี่ยแบบถ่วงน้ำหนัก เรียบขึ้นแต่ตามช้าลง และยังปล่อยค่ากระโดดผ่านมา
 * Median เอาค่ากลางของ MED_WIN ตัวล่าสุด ค่ากระโดดตัวเดียวหายไปทั้งตัว
 * Kalman1D ผสมค่าเดิมกับค่าใหม่ตาม q และ r หมุน VR1 = ค่าจริง ดูว่าเส้นไหน
 * ตามทันก่อน หมุน VR2 = alpha ของ EMA (0.05-0.90) ดูแถว "ผ่าน" ทางขวาว่า
 * แต่ละเส้นปล่อยค่ากระโดดผ่านกี่ครั้ง (มีเสียงเตือนเมื่อ EMA ปล่อยผ่าน)
 * สั่นและค่ากระโดดเป็นของจำลองในโปรแกรม (random) เล่น 3 นาทีแล้วจบเอง
 * ใช้กับ TESAIoT Dev Kit และ BENTO Emulator
 *
 * A filter race: the same simulated sensor (knob VR1 as the true value,
 * plus a little jitter and a spike every SPIKE_EVERY samples) goes
 * through the firmware's EMA, Median and Kalman1D filters, drawn as four
 * lines on one chart. Turn VR2 to set the EMA's alpha; the right card
 * counts how often each line let a spike through, and a tone sounds
 * when the EMA does. It runs for three minutes.
 *
 * Twin of cp2_01_filter_race.py (aiot-development-for-smart-farm/core/cp2_01_filter_race.py). Style: CLEAN_C.md.
 * What C does differently:
 *   - dsp.EMA, dsp.Median and dsp.Kalman1D are the same filters held by
 *     value (bento_dsp_*_state_*), so there is nothing to free;
 *   - make_ema() starts a new EMA state with the old one's last value,
 *     as the .py builds a new object; None (no EMA yet) is a bool;
 *   - random keeps its state in a struct, started as `import random`
 *     leaves it on this board;
 *   - round() is Python's: a half goes to the even neighbour;
 *   - "%.2f" is bento_fmt_real_precision, the board's float text;
 *   - hasattr(ui, "volume") is not asked: the volume call is always
 *     made, and a firmware without it refuses it, which is ignored;
 *   - ui.tone() is fire-and-forget in MicroPython, so its status is
 *     ignored here too;
 *   - Stop ends the program at the next sleep and main returns 0,
 *     without the closing line (MicroPython's KeyboardInterrupt).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_dsp.h"
#include "bento_fmt.h"
#include "bento_pots.h"
#include "bento_random.h"
#include "bento_runtime.h"
#include "bento_time.h"

/* ---- แก้ก่อนรัน: settings (MISRA deviation CT02) ---- */
enum {
    SPIKE_EVERY = 12, /* one spike every this many samples */
    SPIKE = 45,       /* spike height in %, towards the free side */
    NOISE = 3,        /* jitter of +- this many % (0 = none) */
    SPIKE_PASS = 10,  /* further from the truth = let through */
    MED_WIN = 5       /* Median window (firmware: 3-15, made odd) */
};
/* Kalman1D: how fast the truth may change (more = follows faster) */
static const bento_real_t KAL_Q = (bento_real_t)0.1;
/* Kalman1D: how much the sensor shakes (more = steadier line) */
static const bento_real_t KAL_R = (bento_real_t)4.0;
enum {
    TICK_MS = 500,   /* one sample and one screen update this often */
    RUN_MS = 180000, /* play for three minutes, then end */
    SPEAKER = 40,    /* speaker level 0-100 % (firmware 2.4.2+) */
    VOLUME = 25      /* tone loudness 0-127 (about 20 %) */
};
/* ---- end of settings ---- */

/* The course palette, 0xRRGGBB. */
enum {
    COL_TEXT = 0xE8EAED,
    COL_DIM = 0x9AA3AF,
    COL_CARD = 0x171B22,
    COL_OK = 0x30A46C,
    COL_WARN = 0xF5A623,
    COL_BAD = 0xE5484D,
    COL_INFO = 0x4A9EFF
};

/* Screen layout, in pixels. */
enum {
    MARGIN_X = 12,
    TITLE_Y = 6,
    SUBTITLE_Y = 38,
    HELP_Y = 352,
    CHART_X = 12,
    CHART_Y = 66,
    CHART_W = 400,
    CHART_H = 272,
    CHART_MAX_W = 400,  /* wider charts draw a dot on every point */
    CHART_POINTS = 400, /* the firmware takes 10-400 */
    CARD_X = 424,
    CARD_Y = 66,
    CARD_W = 356,
    CARD_H = 272,
    CARD_RADIUS = 12,
    CARD_BORDER = 1,
    CARD_TITLE_DX = 12,
    CARD_TITLE_DY = 6,
    ROW_X = 436,
    ROW_Y = 100,
    ROW_STEP_Y = 46,
    PASS_Y = 296
};

/* Font sizes, in pixels. */
enum {
    FONT_TITLE = 24,
    FONT_TEXT = 20, /* Thai is 20 px or more on the board */
    FONT_ROW = 20
};

/* The knobs, the chart range and the EMA steps. */
enum {
    KNOB_TRUE = 0,  /* VR1 */
    KNOB_ALPHA = 1, /* VR2 */
    RAW_MAX = 4095, /* a 12-bit ADC */
    PERCENT = 100,
    HALF_PERCENT = 50, /* below this, a spike jumps up */
    ALPHA_STEPS = 17,  /* VR2 gives steps 1-18 */
    LINES = 4,         /* raw, EMA, Median, Kalman */
    LINE_RAW = 0,
    LINE_EMA = 1,
    LINE_MEDIAN = 2,
    LINE_KALMAN = 3
};

/* Sound and timing that are not settings. */
enum {
    SETTLE_MS = 200,   /* let the cleared screen settle */
    TONE_MS = 90,      /* each note of a tune */
    NOTE_GAP_MS = 100, /* one note to the next */
    TUNE_NOTES = 3,    /* the longest tune */
    TEXT_BYTES = 128,  /* a SET_TEXT carries at most 126 bytes */
    NAME_BYTES = 42    /* "EMA a=" and a 35-char number, or "Median" */
};

/* The tunes every course file shares. */
enum {
    TUNE_TAP = 0,
    TUNE_START = 1,
    TUNE_STOP = 2,
    TUNE_GOOD = 3,
    TUNE_BAD = 4,
    TUNE_EMPTY = 5,
    TUNE_HIT = 6,
    TUNE_KINDS = 7
};

/* The names on the right; a line's colour is its name's colour. */
static const char *const NAMES[LINES] = {"ดิบ", "EMA", "Median",
                                         "Kalman"};
static const uint32_t COLORS[LINES] = {COL_DIM, COL_WARN, COL_OK,
                                       COL_INFO};

typedef struct {
    int32_t x;
    int32_t y;
    int32_t font_px;
    uint32_t color;
} label_spec_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} rect_t;

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t chart;
    uint8_t series[LINES];
    bento_ui_id_t rows[LINES];
    bento_ui_id_t help;
    bento_random_t random;
    bento_dsp_ema_state_t ema;
    bool has_ema; /* no EMA before VR2 is first read */
    bento_dsp_median_state_t median;
    bento_dsp_kalman1d_state_t kalman;
    int32_t step; /* the EMA's alpha step, 0 = none yet */
    int32_t got[LINES];
    int32_t spikes;
    int32_t samples;
} app_t;

static int16_t px(int32_t value)
{
    return (int16_t)value;
}

/* A sleep that turns Stop into BENTO_E_CANCELLED. */
static bento_status_t pause_ms(uint32_t delay_ms)
{
    bento_status_t status = bento_time_sleep_ms(delay_ms);

    if ((status == BENTO_OK) && !bento_runtime_is_running()) {
        status = BENTO_E_CANCELLED;
    }
    return status;
}

/* Tones go through ui.tone because its loudness can be set. */
static bento_status_t beep(bento_ui_t *ui, int32_t tune)
{
    static const uint8_t notes[TUNE_KINDS][TUNE_NOTES] = {
        {76U, 0U, 0U},   {72U, 79U, 0U}, {79U, 72U, 0U},
        {72U, 79U, 84U}, {84U, 76U, 0U}, {84U, 76U, 69U},
        {88U, 0U, 0U}};
    static const uint8_t lengths[TUNE_KINDS] = {1U, 2U, 2U, 3U,
                                                2U, 3U, 1U};
    bento_status_t status = BENTO_OK;
    uint8_t i;

    for (i = 0U; (i < lengths[tune]) && (status == BENTO_OK); i++) {
        /* fire-and-forget in MicroPython too */
        (void)bento_ui_sfx_tone(ui, notes[tune][i],
                                (uint8_t)BENTO_UI_WAVE_SINE,
                                (uint8_t)VOLUME, (uint16_t)TONE_MS);
        status = pause_ms((uint32_t)NOTE_GAP_MS);
    }
    return status;
}

/* Python's round(): the nearest integer, a half to the even one. */
static int32_t round_even(float value)
{
    const float half = 0.5F;
    int32_t lower = (int32_t)value;
    float rest;

    if ((float)lower > value) {
        lower--; /* the cast went towards zero, round() goes down */
    }
    rest = value - (float)lower;
    if ((rest > half) || ((rest == half) && ((lower % 2) != 0))) {
        lower++;
    }
    return lower;
}

/* VR1 = the true value, 0-100 %, before any noise. */
static bento_status_t read_clean(int32_t *clean)
{
    uint16_t raw = 0U;
    const bento_status_t status = bento_pots_read(KNOB_TRUE, &raw);

    *clean = ((int32_t)raw * PERCENT) / RAW_MAX;
    return status;
}

/* VR2 -> step 1-18 (alpha 0.05 each). The step changes only when the
 * knob is more than 0.7 of a step away, so a knob resting on a border
 * does not flip back and forth. */
static bento_status_t read_alpha_step(int32_t old, int32_t *step)
{
    const float band = 0.7F;
    uint16_t raw = 0U;
    const bento_status_t status = bento_pots_read(KNOB_ALPHA, &raw);
    const int32_t scaled = (int32_t)raw * ALPHA_STEPS;
    const float wanted = 1.0F + ((float)scaled / (float)RAW_MAX);
    const float away = wanted - (float)old;

    *step =
        ((away > band) || (away < -band)) ? round_even(wanted) : old;
    return status;
}

/* The simulated sensor: the truth, jitter, and a spike when asked. */
static bento_status_t read_sensor(app_t *app, int32_t clean, bool spike,
                                  int32_t *raw)
{
    int32_t jitter = 0;
    const bento_status_t status =
        bento_random_randint(&app->random, -NOISE, NOISE, &jitter);
    int32_t value = clean + jitter;

    if (spike) {
        value += (clean < HALF_PERCENT) ? SPIKE : -SPIKE;
    }
    if (value < 0) {
        value = 0;
    }
    if (value > PERCENT) {
        value = PERCENT;
    }
    *raw = value;
    return status;
}

/* The EMA has no way to change alpha, so a new one starts when VR2
 * changes step, fed the old one's last value so the line does not
 * jump. */
static bento_status_t make_ema(app_t *app, int32_t step)
{
    const float alpha_per_step = 0.05F;
    bento_dsp_ema_config_t config;
    bento_dsp_ema_state_t fresh;
    float last = 0.0F;
    float unused = 0.0F;
    bento_status_t status = bento_dsp_ema_config_init(&config);

    if (status == BENTO_OK) {
        config.alpha = (float)step * alpha_per_step;
        status = bento_dsp_ema_state_init(&fresh, &config);
    }
    if ((status == BENTO_OK) && app->has_ema) {
        status = bento_dsp_ema_state_value(&app->ema, &last);
        if (status == BENTO_OK) {
            status = bento_dsp_ema_state_update(&fresh, last, &unused);
        }
    }
    if (status == BENTO_OK) {
        app->ema = fresh;
        app->has_ema = true;
        app->step = step;
    }
    return status;
}

/* The Median and the Kalman filter, set up once. */
static bento_status_t start_filters(app_t *app)
{
    bento_dsp_median_config_t median;
    bento_dsp_kalman1d_config_t kalman;
    bento_status_t status = bento_random_init(&app->random);

    if (status == BENTO_OK) {
        status = bento_dsp_median_config_init(&median);
    }
    if (status == BENTO_OK) {
        median.window = MED_WIN;
        status = bento_dsp_median_state_init(&app->median, &median);
    }
    if (status == BENTO_OK) {
        status = bento_dsp_kalman1d_config_init(&kalman);
    }
    if (status == BENTO_OK) {
        kalman.q = (float)KAL_Q;
        kalman.r = (float)KAL_R;
        status = bento_dsp_kalman1d_state_init(&app->kalman, &kalman);
    }
    return status;
}

/* One raw value through all three filters. */
static bento_status_t filter(app_t *app, int32_t raw, float *outs)
{
    const float value = (float)raw;
    bento_status_t status =
        bento_dsp_ema_state_update(&app->ema, value, &outs[LINE_EMA]);

    outs[LINE_RAW] = value;
    if (status == BENTO_OK) {
        status = bento_dsp_median_state_update(&app->median, value,
                                               &outs[LINE_MEDIAN]);
    }
    if (status == BENTO_OK) {
        status = bento_dsp_kalman1d_state_update(&app->kalman, value,
                                                 &outs[LINE_KALMAN]);
    }
    return status;
}

/* During a spike: is this line further than SPIKE_PASS from the truth
 * (the spike got through)? */
static bool leaked(float out, int32_t clean)
{
    const float away = out - (float)clean;

    return (away > (float)SPIKE_PASS) || (away < -(float)SPIKE_PASS);
}

/* Counts which lines let the spike through; a tone when the EMA did. */
static bento_status_t count_leaks(app_t *app, const float *outs,
                                  int32_t clean)
{
    bento_status_t status = BENTO_OK;
    uint32_t i;

    app->spikes++;
    for (i = 0U; i < (uint32_t)LINES; i++) {
        if (leaked(outs[i], clean)) {
            app->got[i]++;
        }
    }
    if (leaked(outs[LINE_EMA], clean)) {
        status = beep(app->ui, TUNE_BAD);
    }
    return status;
}

static bento_status_t add_label(bento_ui_t *ui, const char *text,
                                const label_spec_t *spec,
                                bento_ui_id_t *out_id)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.text = text;
    args.text_len = (uint32_t)strlen(text);
    args.x = px(spec->x);
    args.y = px(spec->y);
    args.color = spec->color;
    args.font_px = spec->font_px;
    return bento_ui_label_create(ui, &args, out_id);
}

/* A card: a dark box with a grey border and a blue title. */
static bento_status_t add_card(bento_ui_t *ui, const rect_t *box,
                               const char *title)
{
    const label_spec_t spec = {box->x + CARD_TITLE_DX,
                               box->y + CARD_TITLE_DY, FONT_TEXT,
                               COL_INFO};
    bento_ui_widget_args_t args;
    bento_ui_id_t unused;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = px(box->x);
    args.y = px(box->y);
    args.w = px(box->w);
    args.h = px(box->h);
    args.color = COL_CARD;
    args.min_val = COL_DIM;
    args.max_val = CARD_RADIUS;
    args.font_px = CARD_BORDER;
    status = bento_ui_panel_create(ui, &args, &unused);
    if (status == BENTO_OK) {
        status = add_label(ui, title, &spec, &unused);
    }
    return status;
}

/* A smooth line chart with no dots: LVGL draws no dot once the points
 * are at least the chart's width, so it is at most 400 wide with 400
 * points. */
static bento_status_t add_chart(bento_ui_t *ui, const rect_t *box,
                                int32_t low, int32_t high,
                                uint32_t color, bento_ui_id_t *out_id)
{
    bento_ui_widget_args_t args;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = px(box->x);
    args.y = px(box->y);
    args.w = px((box->w < CHART_MAX_W) ? box->w : CHART_MAX_W);
    args.h = px(box->h);
    args.color = color;
    args.min_val = low;
    args.max_val = high;
    status = bento_ui_chart_create(ui, &args, out_id);
    if (status == BENTO_OK) {
        /* a setter: MicroPython ignores its refusal too */
        (void)bento_ui_set_prop(ui, *out_id,
                                (uint8_t)BENTO_UI_PROP_CHART_POINTS,
                                CHART_POINTS);
    }
    return status;
}

/* ui.poll() here only keeps the screen drawn: its events are not
 * read, and a failed poll answers no events. */
static void poll_screen(bento_ui_t *ui)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    (void)bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                        NULL);
}

/* The chart and its three extra lines (series 0 comes with it). */
static bento_status_t build_chart(app_t *app)
{
    const rect_t box = {CHART_X, CHART_Y, CHART_W, CHART_H};
    bento_status_t status = add_chart(app->ui, &box, 0, PERCENT,
                                      COLORS[LINE_RAW], &app->chart);
    uint32_t i;

    app->series[LINE_RAW] = 0U;
    for (i = 1U; (i < (uint32_t)LINES) && (status == BENTO_OK); i++) {
        status = bento_ui_chart_add_series(app->ui, app->chart,
                                           COLORS[i], &app->series[i]);
    }
    return status;
}

/* The right card: one row per line, and what "through" means. */
static bento_status_t build_rows(app_t *app)
{
    const rect_t box = {CARD_X, CARD_Y, CARD_W, CARD_H};
    const label_spec_t pass = {ROW_X, PASS_Y, FONT_TEXT, COL_DIM};
    char text[TEXT_BYTES];
    bento_ui_id_t unused;
    bento_status_t status =
        add_card(app->ui, &box, "ค่า  /  ปล่อยผ่านกี่ครั้ง");
    uint32_t i;

    for (i = 0U; (i < (uint32_t)LINES) && (status == BENTO_OK); i++) {
        const label_spec_t row = {ROW_X,
                                  ROW_Y + (ROW_STEP_Y * (int32_t)i),
                                  FONT_ROW, COLORS[i]};

        status = add_label(app->ui, NAMES[i], &row, &app->rows[i]);
    }
    if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text),
                       "ผ่าน = ห่างค่าจริงเกิน %d ตอนกระโดด", SPIKE_PASS);
        status = add_label(app->ui, text, &pass, &unused);
    }
    return status;
}

static bento_status_t build_screen(app_t *app)
{
    const label_spec_t title = {MARGIN_X, TITLE_Y, FONT_TITLE,
                                COL_TEXT};
    const label_spec_t subtitle = {MARGIN_X, SUBTITLE_Y, FONT_TEXT,
                                   COL_DIM};
    const label_spec_t help = {MARGIN_X, HELP_Y, FONT_TEXT, COL_DIM};
    char text[TEXT_BYTES];
    bento_ui_id_t unused;
    bento_status_t status =
        bento_ui_screen(app->ui, BENTO_RUNTIME_SCREEN_WIDTH_PX,
                        BENTO_RUNTIME_SCREEN_HEIGHT_PX);

    if (status == BENTO_OK) {
        status = pause_ms((uint32_t)SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_label(app->ui, "แข่งกรอง: ดิบ EMA Median Kalman",
                           &title, &unused);
    }
    if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text),
                       "ค่ากระโดดทุก %d ตัวอย่าง - เส้นไหนปล่อยผ่าน",
                       SPIKE_EVERY);
        status = add_label(app->ui, text, &subtitle, &unused);
    }
    if (status == BENTO_OK) {
        status = build_chart(app);
    }
    if (status == BENTO_OK) {
        status = build_rows(app);
    }
    if (status == BENTO_OK) {
        status = add_label(app->ui, "VR1 = ค่าจริง   VR2 = alpha ของ EMA",
                           &help, &app->help);
    }
    if (status == BENTO_OK) {
        poll_screen(app->ui);
    }
    return status;
}

/* MicroPython's widget setters ignore a refusal (modui.c), so the
 * twin does too. */
static void set_text(bento_ui_t *ui, bento_ui_id_t widget,
                     const char *text)
{
    (void)bento_ui_set_text(ui, widget, text, (uint32_t)strlen(text));
}

/* "EMA a=0.25": the board's text of the alpha, two decimals. */
static void ema_name(int32_t step, char *name, uint32_t cap)
{
    const float alpha_per_step = 0.05F;
    const float value = (float)step * alpha_per_step;
    char alpha[BENTO_FMT_BUF_SIZE];
    uint32_t length = 0U;

    if (bento_fmt_real_precision(alpha, sizeof(alpha),
                                 (bento_real_t)value, 2,
                                 &length) != BENTO_OK) {
        alpha[0] = '\0'; /* cannot happen with this buffer */
    }
    (void)snprintf(name, cap, "EMA a=%s", alpha);
}

/* Each line's next point on the chart and its row on the right. */
static void show(const app_t *app, const float *outs)
{
    char name[NAME_BYTES];
    char text[TEXT_BYTES];
    uint32_t i;

    for (i = 0U; i < (uint32_t)LINES; i++) {
        /* the chart takes whole numbers only */
        const int32_t value = round_even(outs[i]);

        (void)bento_ui_chart_set_next(app->ui, app->chart,
                                      app->series[i], value);
        if (i == (uint32_t)LINE_EMA) {
            ema_name(app->step, name, sizeof(name));
        } else {
            (void)snprintf(name, sizeof(name), "%s", NAMES[i]);
        }
        (void)snprintf(text, sizeof(text), "%s  %ld  ผ่าน %ld/%ld", name,
                       (long)value, (long)app->got[i],
                       (long)app->spikes);
        set_text(app->ui, app->rows[i], text);
    }
}

/* One sample: read, filter, count the spikes let through, show. */
static bento_status_t take_sample(app_t *app)
{
    const bool spike =
        (app->samples % SPIKE_EVERY) == (SPIKE_EVERY - 1);
    float outs[LINES];
    int32_t step = 0;
    int32_t clean = 0;
    int32_t raw = 0;
    bento_status_t status = read_alpha_step(app->step, &step);

    if ((status == BENTO_OK) && (step != app->step)) {
        status = make_ema(app, step);
    }
    if (status == BENTO_OK) {
        status = read_clean(&clean);
    }
    if (status == BENTO_OK) {
        status = read_sensor(app, clean, spike, &raw);
    }
    if (status == BENTO_OK) {
        status = filter(app, raw, outs);
    }
    if ((status == BENTO_OK) && spike) {
        status = count_leaks(app, outs, clean);
    }
    if (status == BENTO_OK) {
        show(app, outs);
        poll_screen(app->ui);
        app->samples++;
    }
    return status;
}

static bento_status_t run_loop(app_t *app)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = take_sample(app);
        if (status == BENTO_OK) {
            status = pause_ms((uint32_t)TICK_MS);
        }
    }
    return status;
}

int main(void)
{
    app_t app;
    bento_status_t status;

    (void)memset(&app, 0, sizeof(app));
    app.ui = bento_runtime_get_ui();
    /* firmware 2.4.1 has no volume and refuses it; the .py skips it */
    (void)bento_ui_sfx_volume_set(app.ui, SPEAKER);
    status = build_screen(&app);
    if (status == BENTO_OK) {
        status = start_filters(&app);
    }
    if (status == BENTO_OK) {
        status = run_loop(&app);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_color(app.ui, app.help, COL_WARN);
        set_text(app.ui, app.help,
                 "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่");
        poll_screen(app.ui);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
