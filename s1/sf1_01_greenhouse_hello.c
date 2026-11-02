/*
 * sf1_01_greenhouse_hello.c -- โรงเรือนของเราตอนนี้เป็นยังไง
 * (Greenhouse hello: how is our greenhouse right now)
 *
 * ภารกิจ: อ่านอุณหภูมิ ความชื้น ความกดอากาศจากเซนเซอร์จริง (SHT40,
 * DPS368) โชว์เป็นแผงหน้าปัดโรงเรือน พร้อมกราฟ และอุณหภูมิตัวใหญ่บน
 * จอไฟ RGB 16x8 สีตามระดับ ลองเล่น: ตั้ง TEMP_OFFSET ก่อน แล้วเป่าลม
 * ใส่บอร์ด (ความชื้นพุ่ง) จับเซนเซอร์ค้าง (อุณหภูมิขึ้น) แข่งค่าต่ำ/
 * สูงสุด กด SW5 (ปุ่มล่าง) = เริ่มใหม่ ทำงาน 2 นาทีแล้วจบเอง
 *
 * Reads temperature, humidity and air pressure from the real sensors
 * (SHT40, DPS368) and shows them as a greenhouse dashboard, with a
 * trend chart and the temperature in big digits on the 16x8 RGB
 * matrix, coloured by level. Set TEMP_OFFSET first, then blow on the
 * board (humidity jumps) or hold the sensor (temperature rises), and
 * race the lowest and highest values; SW5 (lower button) starts the
 * race again. It ends by itself after 2 minutes.
 *
 * Twin of sf1_01_greenhouse_hello.py (aiot-development-for-smart-farm/s1/sf1_01_greenhouse_hello.py). Style: CLEAN_C.md.
 * Differs from the .py:
 *   - None ("no value yet") is a reading with known = false.
 *   - A sensor read that raises is a status here; the same fallback
 *     is taken (SHT40 tried 3 times, DPS368 left without a value).
 *   - rgbmatrix failures are ignored, as the .py ignores OSError.
 *   - math.exp is the C library's exp in double, rounded to the
 *     board's float, so a humidity may differ in the last bit.
 *   - hasattr(ui, "volume") is a fact of the build: the volume is
 *     always set, and a build without it refuses, which is ignored.
 *   - ui.tone() is fire-and-forget there, so its status is ignored.
 *   - Stop ends the program quietly where MicroPython raises
 *     KeyboardInterrupt.
 */
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_buttons.h"
#include "bento_fmt.h"
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_sensors.h"
#include "bento_time.h"

/* ---- 1) ตั้งค่า (แก้ได้) ---- */
enum {
    SPEAKER = 40, /* ความดังลำโพงรวม 0-100% (fw 2.4.2+) */
    VOLUME = 25,  /* ความดังเสียง 0-127 */
    RUN_MS = 120000,
    TICK_MS = 500,
    T_WARM = 30, /* อุ่นกว่านี้ = เหลือง */
    T_HOT = 35,  /* ร้อนกว่านี้ = แดง */
    T_GAUGE_MAX = 50,
    NEEDLE_LEN = 32
};

/* ค่าชดเชย: เทียบเทอร์โมมิเตอร์ในห้อง เช่น -9.5 */
static const bento_real_t TEMP_OFFSET = (bento_real_t)0.0;
/* HUM_FIX (แปลงความชื้นเป็นของห้อง) อยู่ใน read_climate() ชื่อ hum_fix */

enum { /* colours, 0xRRGGBB */
    COL_TEXT = 0xE8EAED,
    COL_DIM = 0x9AA3AF,
    COL_CARD = 0x171B22,
    COL_OK = 0x30A46C,
    COL_WARN = 0xF5A623,
    COL_BAD = 0xE5484D,
    COL_INFO = 0x4A9EFF
};

enum { /* heat levels, sound, sensors and the screen */
    LEVELS = 3,
    TUNE_NOTES_MAX = 3,
    TONE_MS = 90,
    NOTE_GAP_MS = 100,
    SHT40_TRIES = 3,
    RETRY_MS = 20,
    SAMPLE_MS = 20,
    SW5_INDEX = 1,
    SETTLE_MS = 200,
    PERCENT_MAX = 100,
    CHART_POINTS = 400,
    NEEDLE_SHIFT = 16,
    NEEDLE_VALUE_MASK = 0xFFFF,
    GAUGE_TICKS = 11,
    GAUGE_MAJOR_EVERY = 2,
    TEXT_CHARS = 128
};

/* A sensor value, or none yet (the .py's None). */
typedef struct {
    bool known;
    bento_real_t value;
} reading_t;

/* ---- 2) ฮาร์ดแวร์ ---- */
typedef enum {
    TUNE_TAP,
    TUNE_START,
    TUNE_STOP,
    TUNE_GOOD,
    TUNE_BAD,
    TUNE_EMPTY,
    TUNE_HIT
} tune_t;

typedef struct {
    uint32_t count;
    uint8_t notes[TUNE_NOTES_MAX];
} tune_notes_t;

/* Stop is a status: the chain of steps ends at once. */
static bento_status_t sleep_ms(uint32_t ms)
{
    bento_status_t status = bento_time_sleep_ms(ms);

    if ((status == BENTO_OK) && !bento_runtime_is_running()) {
        status = BENTO_E_CANCELLED;
    }
    return status;
}

static bento_status_t beep(bento_ui_t *ui, tune_t name)
{
    /* ui.tone, not ui.sfx: only a tone takes a loudness on this
     * firmware */
    static const tune_notes_t tunes[] = {
        {1U, {76U, 0U, 0U}},  {2U, {72U, 79U, 0U}},
        {2U, {79U, 72U, 0U}}, {3U, {72U, 79U, 84U}},
        {2U, {84U, 76U, 0U}}, {3U, {84U, 76U, 69U}},
        {1U, {88U, 0U, 0U}}};
    const tune_notes_t *tune = &tunes[name];
    uint32_t i;
    bento_status_t status = BENTO_OK;

    for (i = 0U; (i < tune->count) && (status == BENTO_OK); i++) {
        /* a tone is fire-and-forget in MicroPython */
        (void)bento_ui_sfx_tone(ui, tune->notes[i], BENTO_UI_WAVE_SINE,
                                (uint8_t)VOLUME, (uint16_t)TONE_MS);
        status = sleep_ms(NOTE_GAP_MS);
    }
    return status;
}

/* ---- 3) สมอง (ตัดสินใจ) ---- */
static bento_real_t clamp(bento_real_t value, bento_real_t lo,
                          bento_real_t hi)
{
    bento_real_t result = value;

    if (result > hi) {
        result = hi;
    }
    if (result < lo) {
        result = lo;
    }
    return result;
}

static int32_t heat_level(bento_real_t t)
{
    int32_t level = 0;

    if (t > (bento_real_t)T_HOT) {
        level = 2;
    } else if (t > (bento_real_t)T_WARM) {
        level = 1;
    } else {
        /* cool enough: level 0 */
    }
    return level;
}

/* "--" for no value yet (a real pressure is around 1010 anyway). */
static void format_reading(char *out, uint32_t cap,
                           const reading_t *reading, int32_t digits)
{
    uint32_t len = 0U;

    if (!reading->known ||
        (bento_fmt_real_precision(out, cap, reading->value, digits,
                                  &len) != BENTO_OK)) {
        (void)snprintf(out, cap, "--");
    }
}

static void format_signed(char *out, uint32_t cap, bento_real_t value)
{
    char digits[BENTO_FMT_BUF_SIZE];
    uint32_t len = 0U;

    if (bento_fmt_real_precision(digits, sizeof(digits), value, 1,
                                 &len) != BENTO_OK) {
        digits[0] = '\0';
    }
    (void)snprintf(out, cap, "%s%s",
                   (value >= (bento_real_t)0) ? "+" : "", digits);
}

/* The saturation vapour pressure (hPa), Magnus formula. */
static bento_real_t sat_pressure(bento_real_t t)
{
    const bento_real_t base_hpa = (bento_real_t)6.112;
    const bento_real_t slope = (bento_real_t)17.62;
    const bento_real_t offset_c = (bento_real_t)243.12;
    const bento_real_t power = (slope * t) / (offset_c + t);

    return base_hpa * (bento_real_t)exp((double)power);
}

/* The air around the warm sensor is warmer than the room, so its %RH
 * reads low; the ratio of saturation pressures turns it back. */
static bento_real_t room_humidity(bento_real_t h_raw,
                                  bento_real_t t_raw,
                                  bento_real_t t_room)
{
    const bento_real_t room =
        (h_raw * sat_pressure(t_raw)) / sat_pressure(t_room);

    return (room < (bento_real_t)PERCENT_MAX)
               ? room
               : (bento_real_t)PERCENT_MAX;
}

/* The lowest and highest values seen (unknown = not seen yet). */
typedef struct {
    reading_t lo;
    reading_t hi;
} min_max_t;

static void min_max_reset(min_max_t *record)
{
    record->lo.known = false;
    record->hi.known = false;
}

static void min_max_add(min_max_t *record, bento_real_t value)
{
    if (!record->lo.known || (value < record->lo.value)) {
        record->lo.value = value;
    }
    if (!record->hi.known || (value > record->hi.value)) {
        record->hi.value = value;
    }
    record->lo.known = true;
    record->hi.known = true;
}

static void min_max_text(char *out, uint32_t cap,
                         const min_max_t *record)
{
    char lo[BENTO_FMT_BUF_SIZE];
    char hi[BENTO_FMT_BUF_SIZE];

    format_reading(lo, sizeof(lo), &record->lo, 1);
    format_reading(hi, sizeof(hi), &record->hi, 1);
    (void)snprintf(out, cap, "ต่ำสุด %s  สูงสุด %s", lo, hi);
}

/* ---- 2) ฮาร์ดแวร์: อ่านอากาศ ---- */
typedef struct {
    reading_t t; /* offset applied */
    reading_t t_raw;
    reading_t h;
    reading_t p;
} climate_t;

/* One SHT40 try; a temperature read before a failed humidity read
 * is kept, as in the .py. */
static void try_sht40(climate_t *climate)
{
    float value = 0.0F;

    if (bento_sensors_sht40_temperature(&value) == BENTO_OK) {
        climate->t_raw.known = true;
        climate->t_raw.value = (bento_real_t)value;
        if (bento_sensors_sht40_humidity(&value) == BENTO_OK) {
            climate->h.known = true;
            climate->h.value = (bento_real_t)value;
        }
    }
}

/* SHT40 may miss a read now and then, so it is tried 3 times. */
static bento_status_t read_sht40(climate_t *climate)
{
    uint32_t tries = 0U;
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) && !climate->h.known &&
           (tries < (uint32_t)SHT40_TRIES)) {
        tries++;
        try_sht40(climate);
        if (!climate->h.known) {
            status = sleep_ms(RETRY_MS);
        }
    }
    return status;
}

static bento_status_t read_climate(climate_t *climate)
{
    /* แปลงความชื้นเป็นของห้อง (สูงเกินจริง = false) */
    static const bool hum_fix = true;
    float pressure = 0.0F;
    bento_status_t status;

    (void)memset(climate, 0, sizeof(*climate));
    status = read_sht40(climate);
    if (bento_sensors_dps368_pressure(&pressure) == BENTO_OK) {
        climate->p.known = true;
        climate->p.value = (bento_real_t)pressure;
    }
    if (climate->t_raw.known) {
        climate->t.known = true;
        climate->t.value = climate->t_raw.value + TEMP_OFFSET;
    }
    if (climate->h.known && climate->t.known &&
        (TEMP_OFFSET != (bento_real_t)0) && hum_fix) {
        climate->h.value = room_humidity(
            climate->h.value, climate->t_raw.value, climate->t.value);
    }
    return status;
}

/* A button on the base (1 = SW5 lower, 0 = SW6 upper) that misses no
 * short press: the firmware debounces 50 ms, so it is read often. */
typedef struct {
    uint32_t index;
    bool down;
    bool clicked;
} button_t;

static bento_status_t button_sample(button_t *button)
{
    bool now_down = false;
    const bento_status_t status =
        bento_buttons_pressed(button->index, &now_down);

    if ((status == BENTO_OK) && now_down && !button->down) {
        button->clicked = true;
    }
    button->down = now_down;
    return status;
}

static bool button_pressed_now(button_t *button)
{
    const bool fired = button->clicked;

    button->clicked = false;
    return fired;
}

/* Waits ms while it keeps reading the button. */
static bento_status_t wait_ms(int32_t ms, button_t *button)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    int32_t left = ms;
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) && (left > 0)) {
        status = button_sample(button);
        left =
            ms - bento_time_ticks_diff(bento_time_ticks_ms(), start_ms);
        if ((status == BENTO_OK) && (left > 0)) {
            status = sleep_ms(
                (uint32_t)((left < SAMPLE_MS) ? left : SAMPLE_MS));
        }
    }
    return status;
}

/* What the RGB matrix shows: drawn again only when it changes. */
typedef struct {
    bool known;
    int32_t degrees;
    int32_t level;
} matrix_shown_t;

static void matrix_show(bento_real_t t, matrix_shown_t *shown)
{
    /* on the RGB matrix */
    static const int32_t matrix_colors[LEVELS] = {
        BENTO_RGBMATRIX_GREEN, BENTO_RGBMATRIX_YELLOW,
        BENTO_RGBMATRIX_RED};
    const int32_t degrees = (int32_t)(t + (bento_real_t)0.5);
    const int32_t level = heat_level(t);

    if (!shown->known || (degrees != shown->degrees) ||
        (level != shown->level)) {
        /* the .py ignores an OSError of the matrix */
        (void)bento_rgbmatrix_score(degrees, matrix_colors[level]);
    }
    shown->known = true;
    shown->degrees = degrees;
    shown->level = level;
}

/* ---- 4) หน้าจอ ---- */
typedef bento_ui_status_t (*widget_maker_t)(
    bento_ui_t *ui, const bento_ui_widget_args_t *args,
    bento_ui_id_t *out);

/* One widget as the .py writes it; w or h 0 = not given. */
typedef struct {
    widget_maker_t make;
    const char *text;
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    uint32_t color;
    int32_t min;
    int32_t max;
    int32_t value;
} widget_spec_t;

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t gauge;
    bento_ui_id_t seg_t;
    bento_ui_id_t raw_t;
    bento_ui_id_t mm_t;
    bento_ui_id_t arc_h;
    bento_ui_id_t seg_h;
    bento_ui_id_t mm_h;
    bento_ui_id_t seg_p;
    bento_ui_id_t trend_p;
    bento_ui_id_t live;
    bento_ui_id_t chart;
    uint8_t s_hum;
    uint8_t s_temp;
    bento_ui_id_t status;
} greenhouse_screen_t;

static bento_status_t add_text_widget(bento_ui_t *ui,
                                      const widget_spec_t *spec,
                                      const char *text,
                                      bento_ui_id_t *out)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    if (text != NULL) {
        args.text = text;
        args.text_len = (uint32_t)strlen(text);
    }
    args.x = spec->x;
    args.y = spec->y;
    if (spec->w > 0) {
        args.w = spec->w;
    }
    if (spec->h > 0) {
        args.h = spec->h;
    }
    args.color = spec->color;
    args.min_val = spec->min;
    args.max_val = spec->max;
    args.font_px = spec->value;
    return spec->make(ui, &args, out);
}

static bento_status_t add_widget(bento_ui_t *ui,
                                 const widget_spec_t *spec,
                                 bento_ui_id_t *out)
{
    return add_text_widget(ui, spec, spec->text, out);
}

/* A card: a dark box with a grey edge and a blue title. */
static bento_status_t add_card(bento_ui_t *ui, const widget_spec_t *box,
                               const char *title)
{
    const int16_t title_dx = 12;
    const int16_t title_dy = 6;
    static const widget_spec_t label = {
        bento_ui_label_create, NULL, 0, 0, 0, 0, COL_INFO, 0, 0, 20};
    widget_spec_t place = label;
    bento_ui_id_t widget;
    bento_status_t status = add_widget(ui, box, &widget);

    place.x = (int16_t)(box->x + title_dx);
    place.y = (int16_t)(box->y + title_dy);
    if (status == BENTO_OK) {
        status = add_text_widget(ui, &place, title, &widget);
    }
    return status;
}

/* A widget setter's refusal was ignored in MicroPython too (modui.c). */
static void set_text(bento_ui_t *ui, bento_ui_id_t widget,
                     const char *text)
{
    (void)bento_ui_set_text(ui, widget, text, (uint32_t)strlen(text));
}

/* The .py's ui.poll() only lets the screen catch up; its events are
 * not read, and a failed poll answers none. */
static void refresh(bento_ui_t *ui)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    (void)bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                        NULL);
}

/* A Scale has no needle of its own: the needle length sits in the
 * upper 16 bits, the value it points at in the lower 16. */
static void set_needle(bento_ui_t *ui, bento_ui_id_t scale,
                       bento_real_t value)
{
    const uint32_t word =
        ((uint32_t)NEEDLE_LEN << (uint32_t)NEEDLE_SHIFT) |
        ((uint32_t)(int32_t)value & (uint32_t)NEEDLE_VALUE_MASK);

    (void)bento_ui_set_prop(ui, scale, BENTO_UI_PROP_SCALE_NEEDLE,
                            (int32_t)word);
}

/* Creates count widgets in order; outs[i] NULL = not needed later. */
static bento_status_t add_widgets(bento_ui_t *ui,
                                  const widget_spec_t *specs,
                                  bento_ui_id_t *const *outs,
                                  uint32_t count)
{
    bento_ui_id_t widget;
    uint32_t i;
    bento_status_t status = BENTO_OK;

    for (i = 0U; (i < count) && (status == BENTO_OK); i++) {
        status = add_widget(ui, &specs[i],
                            (outs[i] != NULL) ? outs[i] : &widget);
    }
    return status;
}

/* The round gauge: 0..T_GAUGE_MAX, 11 ticks, an orange needle. */
static bento_status_t style_gauge(bento_ui_t *ui, bento_ui_id_t gauge)
{
    bento_status_t status = bento_ui_set_prop(
        ui, gauge, BENTO_UI_PROP_SCALE_MODE, BENTO_UI_SCALE_ROUND_IN);

    if (status == BENTO_OK) {
        status = bento_ui_set_ticks(ui, gauge, GAUGE_TICKS,
                                    GAUGE_MAJOR_EVERY);
    }
    if (status == BENTO_OK) {
        status = bento_ui_set_prop(
            ui, gauge, BENTO_UI_PROP_SCALE_NEEDLE_COLOR, COL_WARN);
    }
    return status;
}

static bento_status_t add_offset_label(bento_ui_t *ui)
{
    static const widget_spec_t place = {
        bento_ui_label_create, NULL, 156, 184, 0, 0, COL_DIM, 0, 0, 20};
    const reading_t offset = {true, TEMP_OFFSET};
    char digits[BENTO_FMT_BUF_SIZE];
    char text[TEXT_CHARS];
    bento_ui_id_t widget;

    format_reading(digits, sizeof(digits), &offset, 1);
    (void)snprintf(text, sizeof(text), "ชดเชย %s", digits);
    return add_text_widget(ui, &place, text, &widget);
}

static bento_status_t build_temp_card(greenhouse_screen_t *screen)
{
    static const widget_spec_t box = {bento_ui_panel_create,
                                      NULL,
                                      12,
                                      64,
                                      252,
                                      208,
                                      COL_CARD,
                                      COL_DIM,
                                      12,
                                      1};
    static const widget_spec_t specs[] = {
        {bento_ui_scale_create, NULL, 24, 100, 120, 120, COL_TEXT, 0,
         T_GAUGE_MAX, 0},
        {bento_ui_seg7_create, "--", 156, 104, 100, 40, COL_OK, 0, 0,
         0},
        {bento_ui_label_create, "ดิบ --", 156, 156, 0, 0, COL_DIM, 0, 0,
         20}};
    static const widget_spec_t record = {bento_ui_label_create,
                                         "ต่ำสุด --  สูงสุด --",
                                         24,
                                         240,
                                         0,
                                         0,
                                         COL_DIM,
                                         0,
                                         0,
                                         20};
    bento_ui_id_t *const outs[] = {&screen->gauge, &screen->seg_t,
                                   &screen->raw_t};
    bento_status_t status = add_card(screen->ui, &box, "อุณหภูมิ (C)");

    if (status == BENTO_OK) {
        status = add_widget(screen->ui, &specs[0], outs[0]);
    }
    if (status == BENTO_OK) {
        status = style_gauge(screen->ui, screen->gauge);
    }
    if (status == BENTO_OK) {
        status = add_widgets(screen->ui, &specs[1], &outs[1], 2U);
    }
    if (status == BENTO_OK) {
        status = add_offset_label(screen->ui);
    }
    if (status == BENTO_OK) {
        status = add_widget(screen->ui, &record, &screen->mm_t);
    }
    return status;
}

static bento_status_t build_humid_card(greenhouse_screen_t *screen)
{
    static const widget_spec_t box = {bento_ui_panel_create,
                                      NULL,
                                      272,
                                      64,
                                      252,
                                      208,
                                      COL_CARD,
                                      COL_DIM,
                                      12,
                                      1};
    static const widget_spec_t arc = {
        bento_ui_arc_create, NULL, 284, 100, 120, 120, 0, 0, 100, 0};
    static const widget_spec_t specs[] = {
        {bento_ui_seg7_create, "--", 416, 104, 100, 40, COL_INFO, 0, 0,
         0},
        {bento_ui_label_create, "เป่าลม = พุ่ง", 416, 156, 0, 0, COL_DIM,
         0, 0, 20},
        {bento_ui_label_create, "ต่ำสุด --  สูงสุด --", 284, 240, 0, 0,
         COL_DIM, 0, 0, 20}};
    bento_ui_id_t *const outs[] = {&screen->seg_h, NULL, &screen->mm_h};
    bento_status_t status =
        add_card(screen->ui, &box, "ความชื้นอากาศ (%)");

    if (status == BENTO_OK) {
        status = add_widget(screen->ui, &arc, &screen->arc_h);
    }
    if (status == BENTO_OK) {
        /* a widget setter's refusal was ignored in MicroPython too */
        (void)bento_ui_set_color(screen->ui, screen->arc_h, COL_INFO);
        status =
            add_widgets(screen->ui, specs, outs,
                        (uint32_t)(sizeof(specs) / sizeof(specs[0])));
    }
    return status;
}

static bento_status_t build_pressure_card(greenhouse_screen_t *screen)
{
    static const widget_spec_t box = {bento_ui_panel_create,
                                      NULL,
                                      532,
                                      64,
                                      248,
                                      208,
                                      COL_CARD,
                                      COL_DIM,
                                      12,
                                      1};
    static const widget_spec_t specs[] = {
        {bento_ui_seg7_create, "--", 544, 100, 200, 40, COL_WARN, 0, 0,
         0},
        {bento_ui_label_create, "เทียบตอนเริ่ม --", 544, 152, 0, 0,
         COL_TEXT, 0, 0, 20},
        {bento_ui_label_create, "ลดเรื่อย ๆ = ฝนอาจมา", 544, 184, 0, 0,
         COL_DIM, 0, 0, 20},
        {bento_ui_led_create, NULL, 546, 236, 20, 20, COL_OK, 0, 0, 0},
        {bento_ui_label_create, "ไฟกะพริบ = กำลังอ่านสด", 576, 236, 0, 0,
         COL_DIM, 0, 0, 20}};
    bento_ui_id_t *const outs[] = {&screen->seg_p, &screen->trend_p,
                                   NULL, &screen->live, NULL};
    bento_status_t status =
        add_card(screen->ui, &box, "ความกดอากาศ (hPa)");

    if (status == BENTO_OK) {
        status =
            add_widgets(screen->ui, specs, outs,
                        (uint32_t)(sizeof(specs) / sizeof(specs[0])));
    }
    return status;
}

/* The chart: the blue line is humidity %, the orange one temperature C. */
static bento_status_t build_trend(greenhouse_screen_t *screen)
{
    static const widget_spec_t chart = {bento_ui_chart_create,
                                        NULL,
                                        12,
                                        280,
                                        CHART_POINTS,
                                        62,
                                        COL_INFO,
                                        0,
                                        100,
                                        0};
    static const widget_spec_t legends[] = {
        {bento_ui_label_create, "เส้นฟ้า = ความชื้น %", 428, 282, 0, 0,
         COL_INFO, 0, 0, 20},
        {bento_ui_label_create, "เส้นส้ม = อุณหภูมิ C", 428, 310, 0, 0,
         COL_WARN, 0, 0, 20}};
    bento_ui_id_t *const outs[] = {NULL, NULL};
    bento_status_t status =
        add_widget(screen->ui, &chart, &screen->chart);

    if (status == BENTO_OK) {
        status =
            bento_ui_set_prop(screen->ui, screen->chart,
                              BENTO_UI_PROP_CHART_POINTS, CHART_POINTS);
    }
    if (status == BENTO_OK) {
        screen->s_hum = 0U;
        status = bento_ui_chart_add_series(screen->ui, screen->chart,
                                           COL_WARN, &screen->s_temp);
    }
    if (status == BENTO_OK) {
        status = add_widgets(screen->ui, legends, outs, 2U);
    }
    return status;
}

static bento_status_t build_title(bento_ui_t *ui)
{
    static const widget_spec_t title = {bento_ui_label_create,
                                        "โรงเรือนของเราตอนนี้",
                                        12,
                                        6,
                                        0,
                                        0,
                                        COL_TEXT,
                                        0,
                                        0,
                                        24};
    static const widget_spec_t hint = {
        bento_ui_label_create, NULL, 12, 38, 0, 0, 0, 0, 0, 20};
    widget_spec_t place = hint;
    const char *text = "ชดเชยแล้ว - ลองเป่าลมใส่บอร์ดดูสิ";
    bento_ui_id_t widget;
    bento_status_t status = add_widget(ui, &title, &widget);

    place.color = COL_DIM;
    if (TEMP_OFFSET == (bento_real_t)0) {
        text = "ยังไม่ชดเชย: แก้ TEMP_OFFSET ก่อน (ดูใบงาน กิจกรรม 1)";
        place.color = COL_WARN;
    }
    if (status == BENTO_OK) {
        status = add_text_widget(ui, &place, text, &widget);
    }
    return status;
}

static bento_status_t build_screen(greenhouse_screen_t *screen)
{
    static const widget_spec_t line = {bento_ui_label_create,
                                       "กำลังอ่านเซนเซอร์...",
                                       12,
                                       356,
                                       0,
                                       0,
                                       COL_WARN,
                                       0,
                                       0,
                                       20};
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = sleep_ms(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = build_title(screen->ui);
    }
    if (status == BENTO_OK) {
        status = build_temp_card(screen);
    }
    if (status == BENTO_OK) {
        status = build_humid_card(screen);
    }
    if (status == BENTO_OK) {
        status = build_pressure_card(screen);
    }
    if (status == BENTO_OK) {
        status = build_trend(screen);
    }
    if (status == BENTO_OK) {
        status = add_widget(screen->ui, &line, &screen->status);
    }
    if (status == BENTO_OK) {
        refresh(screen->ui);
    }
    return status;
}

static void show_temp(const greenhouse_screen_t *screen,
                      const climate_t *climate, const min_max_t *record)
{
    /* on the screen */
    static const uint32_t level_colors[LEVELS] = {COL_OK, COL_WARN,
                                                  COL_BAD};
    const bento_real_t t = climate->t.value;
    char text[TEXT_CHARS];
    char digits[BENTO_FMT_BUF_SIZE];

    if (climate->t.known) {
        format_reading(digits, sizeof(digits), &climate->t, 1);
        set_text(screen->ui, screen->seg_t, digits);
        (void)bento_ui_set_color(screen->ui, screen->seg_t,
                                 level_colors[heat_level(t)]);
        set_needle(
            screen->ui, screen->gauge,
            clamp(t, (bento_real_t)0, (bento_real_t)T_GAUGE_MAX));
        format_reading(digits, sizeof(digits), &climate->t_raw, 1);
        (void)snprintf(text, sizeof(text), "ดิบ %s", digits);
        set_text(screen->ui, screen->raw_t, text);
        min_max_text(text, sizeof(text), record);
        set_text(screen->ui, screen->mm_t, text);
        (void)bento_ui_chart_set_next(
            screen->ui, screen->chart, screen->s_temp,
            (int32_t)clamp(t, (bento_real_t)0,
                           (bento_real_t)PERCENT_MAX));
    }
}

static void show_humid(const greenhouse_screen_t *screen,
                       const reading_t *h, const min_max_t *record)
{
    char text[TEXT_CHARS];

    if (h->known) {
        format_reading(text, sizeof(text), h, 1);
        set_text(screen->ui, screen->seg_h, text);
        (void)bento_ui_set_value(screen->ui, screen->arc_h,
                                 (int32_t)h->value);
        min_max_text(text, sizeof(text), record);
        set_text(screen->ui, screen->mm_h, text);
        (void)bento_ui_chart_set_next(screen->ui, screen->chart,
                                      screen->s_hum, (int32_t)h->value);
    }
}

static void show_pressure(const greenhouse_screen_t *screen,
                          const reading_t *p, const reading_t *p_start)
{
    char text[TEXT_CHARS];
    char change[BENTO_FMT_BUF_SIZE];

    if (p->known) {
        format_reading(text, sizeof(text), p, 0);
        set_text(screen->ui, screen->seg_p, text);
        format_signed(change, sizeof(change),
                      p->value - p_start->value);
        (void)snprintf(text, sizeof(text), "เทียบตอนเริ่ม %s hPa", change);
        set_text(screen->ui, screen->trend_p, text);
    }
}

static void show_status(const greenhouse_screen_t *screen, bool ok,
                        int32_t rounds)
{
    char text[TEXT_CHARS];

    (void)bento_ui_set_value(screen->ui, screen->live,
                             ok ? (rounds % 2) : 0);
    if (ok) {
        (void)bento_ui_set_color(screen->ui, screen->status, COL_OK);
        (void)snprintf(text, sizeof(text),
                       "อ่านไปแล้ว %" PRId32 " รอบ - ลองเป่าลมใส่บอร์ดดูสิ",
                       rounds);
        set_text(screen->ui, screen->status, text);
    } else {
        (void)bento_ui_set_color(screen->ui, screen->status, COL_BAD);
        set_text(screen->ui, screen->status,
                 "อ่าน SHT40 ไม่ได้ - เช็กว่าใช้ firmware Dev Kit 2.4.1");
    }
}

/* ---- 5) โปรแกรมหลัก ---- */
typedef struct {
    button_t sw5;
    min_max_t temp_record;
    min_max_t hum_record;
    reading_t p_start;
    matrix_shown_t shown;
    int32_t rounds;
} greenhouse_t;

static void finish(const greenhouse_screen_t *screen,
                   const greenhouse_t *house)
{
    char t_lo[BENTO_FMT_BUF_SIZE];
    char t_hi[BENTO_FMT_BUF_SIZE];
    char h_lo[BENTO_FMT_BUF_SIZE];
    char h_hi[BENTO_FMT_BUF_SIZE];

    /* the .py ignores an OSError of the matrix */
    (void)bento_rgbmatrix_clear();
    (void)bento_ui_set_color(screen->ui, screen->status, COL_WARN);
    set_text(screen->ui, screen->status,
             "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่");
    refresh(screen->ui);
    format_reading(t_lo, sizeof(t_lo), &house->temp_record.lo, 1);
    format_reading(t_hi, sizeof(t_hi), &house->temp_record.hi, 1);
    format_reading(h_lo, sizeof(h_lo), &house->hum_record.lo, 1);
    format_reading(h_hi, sizeof(h_hi), &house->hum_record.hi, 1);
    (void)printf("สรุป: T %s - %s C  RH %s - %s %%\n", t_lo, t_hi, h_lo,
                 h_hi);
}

/* One round: read, SW5 restarts the race, the matrix, the screen. */
static bento_status_t run_round(const greenhouse_screen_t *screen,
                                greenhouse_t *house)
{
    climate_t climate;
    bento_status_t status = read_climate(&climate);

    house->rounds++;
    if ((status == BENTO_OK) && button_pressed_now(&house->sw5)) {
        min_max_reset(&house->temp_record);
        min_max_reset(&house->hum_record);
        status = beep(screen->ui, TUNE_TAP);
    }
    if (status == BENTO_OK) {
        if (climate.t.known) {
            min_max_add(&house->temp_record, climate.t.value);
            matrix_show(climate.t.value, &house->shown);
        }
        if (climate.h.known) {
            min_max_add(&house->hum_record, climate.h.value);
        }
        if (climate.p.known && !house->p_start.known) {
            house->p_start = climate.p;
        }
        show_temp(screen, &climate, &house->temp_record);
        show_humid(screen, &climate.h, &house->hum_record);
        show_pressure(screen, &climate.p, &house->p_start);
        show_status(screen, climate.t.known || climate.h.known,
                    house->rounds);
        refresh(screen->ui);
    }
    return status;
}

static bento_status_t run_loop(const greenhouse_screen_t *screen,
                               greenhouse_t *house)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = run_round(screen, house);
        if (status == BENTO_OK) {
            status = wait_ms(TICK_MS, &house->sw5);
        }
    }
    return status;
}

int main(void)
{
    greenhouse_screen_t screen;
    greenhouse_t house;
    bento_status_t status;

    (void)memset(&screen, 0, sizeof(screen));
    (void)memset(&house, 0, sizeof(house));
    screen.ui = bento_runtime_get_ui();
    house.sw5.index = SW5_INDEX;
    /* a build without a speaker volume refuses it, as hasattr() skips
     * it */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    status = build_screen(&screen);
    if (status == BENTO_OK) {
        status = run_loop(&screen, &house);
    }
    if (status == BENTO_OK) {
        finish(&screen, &house);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
