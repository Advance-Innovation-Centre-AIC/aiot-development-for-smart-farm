/*
 * cp2_02_rolling_stats.c -- หลักการ 2.2: สถิติของหน้าต่างเลื่อน
 * บอกว่าค่าล่าสุด "ผิดปกติแค่ไหน"
 *
 * เก็บ WIN ค่าล่าสุดไว้ ค่าใหม่เข้าท้าย ค่าเก่าสุดออกหัว = หน้าต่างเลื่อน
 * จากหน้าต่างได้ค่าเฉลี่ย SD ต่ำสุด สูงสุด และความชันต่อนาที
 * z = (ค่าล่าสุด - ค่าเฉลี่ย) / SD = ห่างจากปกติกี่ SD ค่าที่หลุด Z_MAX
 * จึงน่าสงสัย (ไฟแดงบนจอติด มีเสียงเตือน) เริ่มที่ VR1 รอให้เก็บครบ 30 ค่า
 * (15 วิ) แล้วหมุน VR1 เร็ว ๆ ดู z พุ่ง กด SW5 (ปุ่มล่าง) สลับเป็นอุณหภูมิ
 * SHT40 แล้วเป่าลมหายใจใส่เซนเซอร์ เล่น 3 นาทีแล้วจบเอง ใช้กับ TESAIoT
 * Dev Kit และ BENTO Emulator
 *
 * A sliding window of the last WIN values (knob VR1 or the SHT40
 * temperature) gives the mean, the SD, the lowest, the highest and the
 * slope per minute. The newest value's z score says how many SDs it
 * lies from the window before it; past Z_MAX the screen LED lights and
 * a tone sounds. The chart shows the value with a mean +- BAND_SD SD
 * band. SW5 switches the source and starts a new window. It runs for
 * three minutes.
 *
 * Twin of cp2_02_rolling_stats.py (aiot-development-for-smart-farm/core/cp2_02_rolling_stats.py). Style: CLEAN_C.md.
 * What C does differently:
 *   - the window is a fixed array of WIN values and a count, shifted
 *     down by one when full, as the .py pops the list's head;
 *   - None (SHT40 not read) is a bool beside the value; a failed read
 *     is retried three times, as the .py's try/except;
 *   - `read_value(1) or base` keeps the old base when the read fails or
 *     reads exactly 0.0, as `or` does;
 *   - math.sqrt is sqrtf, in the board's float;
 *   - round() is Python's: a half goes to the even neighbour;
 *   - "%.1f", "%.2f" and their "%+" forms are bento_fmt_real_precision,
 *     the board's float text, with the plus sign added here;
 *   - hasattr(ui, "volume") is not asked: the volume call is always
 *     made, and a firmware without it refuses it, which is ignored;
 *   - ui.tone() is fire-and-forget in MicroPython, so its status is
 *     ignored here too;
 *   - Stop ends the program at the next sleep and main returns 0,
 *     without the closing line (MicroPython's KeyboardInterrupt).
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_buttons.h"
#include "bento_fmt.h"
#include "bento_pots.h"
#include "bento_runtime.h"
#include "bento_sensors.h"
#include "bento_time.h"

/* ---- แก้ก่อนรัน: settings (MISRA deviation CT02) ---- */
enum {
    WIN = 30 /* values in the window (30 x 0.5 s = 15 s) */
};
/* |z| above this = unusual, the screen LED lights. */
static const bento_real_t Z_MAX = (bento_real_t)2.5;
enum {
    BAND_SD = 2 /* the yellow lines: mean +- this many SD */
};
/* The smallest SD divided by (VR1 in %, the temperature in C): a still
 * value has an SD near 0, which would make z shoot up. */
static const bento_real_t SD_MIN[2] = {(bento_real_t)1.0,
                                       (bento_real_t)0.1};
/* The board warms itself: compare with a room thermometer and put the
 * difference here, such as -9.5. */
static const bento_real_t TEMP_OFFSET = (bento_real_t)0.0;
enum {
    TICK_MS = 500,   /* one value and one screen update this often */
    SAMPLE_MS = 20,  /* read the button this often while waiting */
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
    SCALE_Y = 38,
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
    INNER_X = 436,
    LED_Y = 100,
    LED_SIZE = 48,
    Z_X = 500,
    Z_Y = 108,
    NOW_Y = 170,
    MEAN_Y = 214,
    RANGE_Y = 248
};

/* Font sizes, in pixels. */
enum {
    FONT_TITLE = 24,
    FONT_TEXT = 20, /* Thai is 20 px or more on the board */
    FONT_NOW = 20,
    FONT_Z = 28
};

/* The sources, the knob and the chart's scale. */
enum {
    SOURCE_KNOB = 0,
    SOURCE_SHT40 = 1,
    BUTTON_SW5 = 1, /* the lower button is pressed(1) */
    RAW_MAX = 4095, /* a 12-bit ADC */
    PERCENT = 100,
    CHART_MIDDLE = 50,     /* the starting temperature sits here */
    CHART_PER_DEGREE = 10, /* one step = 0.1 C, edges +- 5 C */
    SHT40_TRIES = 3,
    RETRY_MS = 50,
    MS_PER_MINUTE = 60000
};

/* Sound, text and timing that are not settings. */
enum {
    SETTLE_MS = 200,   /* let the cleared screen settle */
    TONE_MS = 90,      /* each note of a tune */
    NOTE_GAP_MS = 100, /* one note to the next */
    TUNE_NOTES = 3,    /* the longest tune */
    TEXT_BYTES = 128,  /* a SET_TEXT carries at most 126 bytes */
    REAL_CHARS = 40    /* a number's text, its sign and the NUL */
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

/* Source 0 = the knob (the default, easy in class), 1 = the
 * temperature. */
static const char *const SOURCES[2] = {"VR1", "SHT40"};

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

/* A value that may be missing (None in the .py). */
typedef struct {
    float value;
    bool known;
} reading_t;

typedef struct {
    float mean;
    float sd;
    float z;
} stats_t;

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t scale;
    bento_ui_id_t chart;
    uint8_t high_series;
    uint8_t low_series;
    bento_ui_id_t led;
    bento_ui_id_t z_text;
    bento_ui_id_t now;
    bento_ui_id_t mean_text;
    bento_ui_id_t range_text;
    bento_ui_id_t help;
    uint32_t source;
    float base; /* the starting temperature, the chart's middle */
    float window[WIN];
    uint32_t count;
    bool alarm;
    bool last_down;
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

/* The SHT40 misses a read now and then (the bus is busy), so it is
 * tried three times; still nothing = not known. */
static bento_status_t read_temperature(reading_t *reading)
{
    bento_status_t status = BENTO_OK;
    int32_t tries;

    reading->known = false;
    for (tries = 0; (tries < SHT40_TRIES) && !reading->known &&
                    (status == BENTO_OK);
         tries++) {
        float celsius = 0.0F;

        if (bento_sensors_sht40_temperature(&celsius) == BENTO_OK) {
            reading->value = celsius + (float)TEMP_OFFSET;
            reading->known = true;
        } else {
            status = pause_ms((uint32_t)RETRY_MS);
        }
    }
    return status;
}

/* The newest value of a source: VR1 in %, the SHT40 in C. */
static bento_status_t read_value(uint32_t source, reading_t *reading)
{
    bento_status_t status = BENTO_OK;

    if (source == (uint32_t)SOURCE_KNOB) {
        uint16_t raw = 0U;
        int32_t scaled;

        status = bento_pots_read(0U, &raw);
        scaled = (int32_t)raw * PERCENT;
        reading->value = (float)scaled / (float)RAW_MAX;
        reading->known = true;
    } else {
        status = read_temperature(reading);
    }
    return status;
}

/* Waits delay_ms but reads SW5 every SAMPLE_MS: the firmware filters
 * the bounce only while it is read, so a slower read misses a tap.
 * *clicked tells whether it went down in that time. */
static bento_status_t wait_click(app_t *app, uint32_t delay_ms,
                                 bool *clicked)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    *clicked = false;
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            (int32_t)delay_ms)) {
        bool down = false;

        status = bento_buttons_pressed(BUTTON_SW5, &down);
        if (status == BENTO_OK) {
            *clicked = *clicked || (down && !app->last_down);
            app->last_down = down;
            status = pause_ms((uint32_t)SAMPLE_MS);
        }
    }
    return status;
}

/* Welford: one pass gives the mean and the sample SD (divided by
 * n - 1), without the big sum of squares that loses digits in the
 * board's 32-bit float. */
static void mean_sd(const float *window, uint32_t count, float *mean,
                    float *sd)
{
    float running = 0.0F;
    float squares = 0.0F;
    uint32_t i;

    for (i = 0U; i < count; i++) {
        const uint32_t seen = i + 1U;
        const float delta = window[i] - running;

        running += delta / (float)seen;
        squares += delta * (window[i] - running);
    }
    *mean = running;
    *sd = 0.0F;
    if (count > 1U) {
        const uint32_t degrees = count - 1U;

        *sd = sqrtf(squares / (float)degrees);
    }
}

/* The slope: (mean of the second half - mean of the first half) over
 * the distance of their middles, in values, then per minute. Steadier
 * than "last - first", because each half averages many values. */
static float slope_per_min(const float *window, uint32_t count)
{
    const uint32_t half = count / 2U;
    const uint32_t rest = count - half;
    float first = 0.0F;
    float second = 0.0F;
    uint32_t i;

    for (i = 0U; i < half; i++) {
        second += window[(count - half) + i];
        first += window[i];
    }
    return ((((second - first) / (float)half) / (float)rest) *
            (float)MS_PER_MINUTE) /
           (float)TICK_MS;
}

static float larger(float first, float second)
{
    return (second > first) ? second : first;
}

/* The chart takes whole numbers 0-100: VR1 is the % itself; the
 * temperature puts the start in the middle, 0.1 C per step. */
static int32_t to_chart(const app_t *app, float value)
{
    float point = value;

    if (app->source == (uint32_t)SOURCE_SHT40) {
        point = (float)CHART_MIDDLE +
                ((value - app->base) * (float)CHART_PER_DEGREE);
    }
    if (point > (float)PERCENT) {
        point = (float)PERCENT;
    }
    if (point < 0.0F) {
        point = 0.0F;
    }
    return round_even(point);
}

/* The board's text of a number with this many decimals, with a plus
 * sign in front of a positive one when asked. */
static void format_real(char *text, uint32_t cap, float value,
                        int32_t digits, bool sign)
{
    char digits_text[BENTO_FMT_BUF_SIZE];
    uint32_t length = 0U;

    if (bento_fmt_real_precision(digits_text, sizeof(digits_text),
                                 (bento_real_t)value, digits,
                                 &length) != BENTO_OK) {
        digits_text[0] = '\0'; /* cannot happen with this buffer */
    }
    (void)snprintf(text, cap, "%s%s",
                   (sign && (digits_text[0] != '-')) ? "+" : "",
                   digits_text);
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

static bento_status_t add_led(bento_ui_t *ui, bento_ui_id_t *out_id)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.x = INNER_X;
    args.y = LED_Y;
    args.w = LED_SIZE;
    args.h = LED_SIZE;
    args.color = COL_BAD;
    args.font_px = 0;
    return bento_ui_led_create(ui, &args, out_id);
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

/* The chart with its two band lines (series 0 comes with it). */
static bento_status_t build_chart(app_t *app)
{
    const rect_t box = {CHART_X, CHART_Y, CHART_W, CHART_H};
    bento_status_t status =
        add_chart(app->ui, &box, 0, PERCENT, COL_INFO, &app->chart);

    if (status == BENTO_OK) {
        status = bento_ui_chart_add_series(app->ui, app->chart,
                                           COL_WARN, &app->high_series);
    }
    if (status == BENTO_OK) {
        status = bento_ui_chart_add_series(app->ui, app->chart,
                                           COL_WARN, &app->low_series);
    }
    return status;
}

/* The right card: the LED, z, the value, the mean and the range. */
static bento_status_t build_card(app_t *app)
{
    const rect_t box = {CARD_X, CARD_Y, CARD_W, CARD_H};
    const label_spec_t z_spec = {Z_X, Z_Y, FONT_Z, COL_TEXT};
    const label_spec_t now = {INNER_X, NOW_Y, FONT_NOW, COL_INFO};
    const label_spec_t mean = {INNER_X, MEAN_Y, FONT_TEXT, COL_WARN};
    const label_spec_t range = {INNER_X, RANGE_Y, FONT_TEXT, COL_TEXT};
    char limit[REAL_CHARS];
    char title[TEXT_BYTES];
    bento_status_t status;

    format_real(limit, sizeof(limit), (float)Z_MAX, 1, false);
    (void)snprintf(title, sizeof(title),
                   "เทียบ %d ค่าก่อนหน้า  ไฟ = |z| เกิน %s", WIN, limit);
    status = add_card(app->ui, &box, title);
    if (status == BENTO_OK) {
        status = add_led(app->ui, &app->led);
    }
    if (status == BENTO_OK) {
        status = add_label(app->ui, " ", &z_spec, &app->z_text);
    }
    if (status == BENTO_OK) {
        status = add_label(app->ui, " ", &now, &app->now);
    }
    if (status == BENTO_OK) {
        status = add_label(app->ui, " ", &mean, &app->mean_text);
    }
    if (status == BENTO_OK) {
        status = add_label(app->ui, " ", &range, &app->range_text);
    }
    return status;
}

static bento_status_t build_screen(app_t *app)
{
    const label_spec_t title = {MARGIN_X, TITLE_Y, FONT_TITLE,
                                COL_TEXT};
    const label_spec_t scale = {MARGIN_X, SCALE_Y, FONT_TEXT, COL_DIM};
    const label_spec_t help = {MARGIN_X, HELP_Y, FONT_TEXT, COL_DIM};
    bento_ui_id_t unused;
    bento_status_t status =
        bento_ui_screen(app->ui, BENTO_RUNTIME_SCREEN_WIDTH_PX,
                        BENTO_RUNTIME_SCREEN_HEIGHT_PX);

    if (status == BENTO_OK) {
        status = pause_ms((uint32_t)SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_label(app->ui, "หน้าต่างเลื่อน -> z", &title, &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(app->ui, " ", &scale, &app->scale);
    }
    if (status == BENTO_OK) {
        status = build_chart(app);
    }
    if (status == BENTO_OK) {
        status = build_card(app);
    }
    if (status == BENTO_OK) {
        status =
            add_label(app->ui, "SW5 = สลับ VR1/SHT40   เป่าลม -> z พุ่ง",
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

static void show_source(const app_t *app)
{
    char base[REAL_CHARS];
    char scale[TEXT_BYTES];
    char text[TEXT_BYTES];

    if (app->source == (uint32_t)SOURCE_KNOB) {
        (void)snprintf(scale, sizeof(scale), "กราฟ 0-100 %%");
    } else {
        format_real(base, sizeof(base), app->base, 1, false);
        (void)snprintf(scale, sizeof(scale), "กลาง %s C", base);
    }
    (void)snprintf(text, sizeof(text),
                   "ฟ้า = %s  เหลือง = เฉลี่ย +-%d SD  %s",
                   SOURCES[app->source], BAND_SD, scale);
    set_text(app->ui, app->scale, text);
}

/* Mean, SD, lowest, highest and slope, once the window is full. */
static void show_window(const app_t *app, const stats_t *stats,
                        int32_t digits, const char *unit)
{
    char first[REAL_CHARS];
    char second[REAL_CHARS];
    char slope[REAL_CHARS];
    char text[TEXT_BYTES];
    float lowest = app->window[0];
    float highest = app->window[0];
    uint32_t i;

    format_real(first, sizeof(first), stats->mean, digits, false);
    format_real(second, sizeof(second), stats->sd, digits, false);
    (void)snprintf(text, sizeof(text), "เฉลี่ย %s  SD %s%s", first,
                   second, unit);
    set_text(app->ui, app->mean_text, text);
    for (i = 1U; i < app->count; i++) {
        lowest = (app->window[i] < lowest) ? app->window[i] : lowest;
        highest = (app->window[i] > highest) ? app->window[i] : highest;
    }
    format_real(first, sizeof(first), lowest, digits, false);
    format_real(second, sizeof(second), highest, digits, false);
    format_real(slope, sizeof(slope),
                slope_per_min(app->window, app->count), 2, true);
    (void)snprintf(text, sizeof(text), "ต่ำ %s  สูง %s  ชัน %s/นาที", first,
                   second, slope);
    set_text(app->ui, app->range_text, text);
}

static void show_stats(const app_t *app, float value,
                       const stats_t *stats)
{
    static const char *const units[2] = {"%", "C"};
    /* VR1 with one decimal, the temperature with two */
    const int32_t digits =
        (app->source == (uint32_t)SOURCE_KNOB) ? 1 : 2;
    char unit[REAL_CHARS];
    char number[REAL_CHARS];
    char text[TEXT_BYTES];

    (void)snprintf(unit, sizeof(unit), " %s", units[app->source]);
    format_real(number, sizeof(number), value, digits, false);
    (void)snprintf(text, sizeof(text), "%s %s%s", SOURCES[app->source],
                   number, unit);
    set_text(app->ui, app->now, text);
    if (app->count < (uint32_t)WIN) {
        (void)snprintf(text, sizeof(text), "เก็บ %lu/%d",
                       (unsigned long)app->count, WIN);
        set_text(app->ui, app->z_text, text);
    } else {
        format_real(number, sizeof(number), stats->z, 1, true);
        (void)snprintf(text, sizeof(text), "z = %s", number);
        set_text(app->ui, app->z_text, text);
        show_window(app, stats, digits, unit);
    }
}

/* The statistics of the window before the value (not counting it). */
static void window_stats(const app_t *app, float value, stats_t *stats)
{
    const float sd_min = (float)SD_MIN[app->source];

    stats->mean = value;
    stats->sd = 0.0F;
    stats->z = 0.0F;
    if (app->count > 1U) {
        mean_sd(app->window, app->count, &stats->mean, &stats->sd);
    }
    if (app->count >= (uint32_t)WIN) {
        stats->z = (value - stats->mean) / larger(stats->sd, sd_min);
    }
}

/* The value joins the window's tail; past WIN the oldest leaves. */
static void remember(app_t *app, float value)
{
    uint32_t i;

    if (app->count == (uint32_t)WIN) {
        for (i = 1U; i < (uint32_t)WIN; i++) {
            app->window[i - 1U] = app->window[i];
        }
        app->count--;
    }
    app->window[app->count] = value;
    app->count++;
}

/* Judge one value: the LED and a tone on the rising edge only. */
static bento_status_t judge(app_t *app, float value)
{
    const float sd_min = (float)SD_MIN[app->source];
    stats_t stats;
    bool alarm;
    float band;
    bento_status_t status = BENTO_OK;

    window_stats(app, value, &stats);
    alarm = (stats.z > (float)Z_MAX) || (stats.z < -(float)Z_MAX);
    if (alarm && !app->alarm) {
        status = beep(app->ui, TUNE_BAD);
    }
    if (alarm != app->alarm) {
        (void)bento_ui_set_value(app->ui, app->led, alarm ? 1 : 0);
    }
    app->alarm = alarm;
    show_stats(app, value, &stats);
    band = (float)BAND_SD * larger(stats.sd, sd_min);
    /* chart points: setters, ignored by MicroPython too */
    (void)bento_ui_chart_set_next(app->ui, app->chart, 0U,
                                  to_chart(app, value));
    (void)bento_ui_chart_set_next(app->ui, app->chart, app->high_series,
                                  to_chart(app, stats.mean + band));
    (void)bento_ui_chart_set_next(app->ui, app->chart, app->low_series,
                                  to_chart(app, stats.mean - band));
    remember(app, value);
    return status;
}

/* SW5: the other source and a new window. */
static bento_status_t switch_source(app_t *app)
{
    reading_t start = {0.0F, false};
    bento_status_t status = BENTO_OK;

    app->source = 1U - app->source;
    app->count = 0U;
    if (app->source == (uint32_t)SOURCE_SHT40) {
        status = read_value(SOURCE_SHT40, &start);
        if (start.known && (start.value != 0.0F)) {
            app->base = start.value;
        }
    }
    if (status == BENTO_OK) {
        show_source(app);
        set_text(app->ui, app->mean_text, " ");
        set_text(app->ui, app->range_text, " ");
        status = beep(app->ui, TUNE_TAP);
    }
    return status;
}

static bento_status_t take_sample(app_t *app)
{
    reading_t reading = {0.0F, false};
    bool clicked = false;
    bento_status_t status = read_value(app->source, &reading);

    if ((status == BENTO_OK) && !reading.known) {
        set_text(app->ui, app->now, "อ่านไม่ได้");
    } else if (status == BENTO_OK) {
        status = judge(app, reading.value);
    } else {
        /* the status ends the program */
    }
    if (status == BENTO_OK) {
        poll_screen(app->ui);
        status = wait_click(app, (uint32_t)TICK_MS, &clicked);
    }
    if ((status == BENTO_OK) && clicked) {
        status = switch_source(app);
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
    }
    return status;
}

/* The temperature at the start is the chart's middle once SHT40 is
 * chosen; unread, it is 0.0. */
static bento_status_t read_base(app_t *app)
{
    reading_t start = {0.0F, false};
    const bento_status_t status = read_value(SOURCE_SHT40, &start);

    app->base = start.known ? start.value : 0.0F;
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
        status = read_base(&app);
    }
    if (status == BENTO_OK) {
        show_source(&app);
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
