/*
 * sf1_03_auto_irrigation.c -- ระบบรดน้ำอัตโนมัติ (จำลอง)
 * (Automatic irrigation, simulated)
 *
 * VR1 = ความชื้นดิน VR2 = เกณฑ์ VR3 = น้ำในถัง ดินแห้งกว่าเกณฑ์ = เปิดปั๊ม
 * ชื้นเกินเกณฑ์ + HYST = ปิด ถังต่ำกว่า 10 % = ห้ามเดิน หมุน VR1 ลงช้า ๆ
 * แล้วกลับ ปั๊มไม่สั่นเพราะ hysteresis สวิตช์บนจอ = อัตโนมัติ/มือ
 * SW5 = รดเอง/หยุด SW6 = ล้างตัวนับ ไฟ RGB_BLUE แทนรีเลย์ปั๊ม จอไฟ RGB
 * วาดดิน ปั๊ม และถัง เล่น 3 นาทีแล้วจบเอง
 *
 * VR1 is the soil moisture, VR2 the threshold and VR3 the water
 * tank. Soil drier than the threshold starts the pump, wetter than
 * threshold + HYST stops it, and a tank below 10 % never lets it
 * run. Turn VR1 down slowly and back: hysteresis keeps the pump
 * from chattering. The on-screen switch picks auto or manual; SW5
 * starts or stops a manual watering, SW6 clears the counters. The
 * RGB_BLUE LED stands for the pump relay and the RGB matrix draws
 * the soil, the pump and the tank. It ends after 3 minutes.
 *
 * Twin of sf1_03_auto_irrigation.py (aiot-development-for-smart-farm/s1/sf1_03_auto_irrigation.py). Style: CLEAN_C.md.
 * Differs from the .py:
 *   - Button is a struct with sample and pressed_now functions.
 *   - led_named() answers "no LED" for any gpio failure, as the
 *     .py's except answers None.
 *   - A refused rgbmatrix.blit() or clear() is ignored, as the .py
 *     catches its OSError.
 *   - hasattr(ui, "volume") is a fact of the build: the volume is
 *     always set, and a build without it refuses, which is ignored.
 *   - ui.tone() is fire-and-forget there, so its status is ignored.
 *   - Stop ends the program quietly where MicroPython raises
 *     KeyboardInterrupt; as there, finish() then does not run.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_buttons.h"
#include "bento_fmt.h"
#include "bento_gpio.h"
#include "bento_pots.h"
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_time.h"

/* ---- แก้ก่อนรัน: settings (MISRA deviation CT02) ---- */
enum {
    SPEAKER = 40, /* ความดังลำโพงรวม 0-100% (fw 2.4.2+) */
    VOLUME = 25,  /* ความดังเสียง 0-127 */
    HYST = 5,     /* ช่องกันกระพือ: ปิดเมื่อชื้นเกิน เกณฑ์ + HYST */
    TANK_MIN = 10 /* ถังต่ำกว่านี้ (%) ห้ามเดินปั๊ม */
};
/* ปั๊มไหลกี่ลิตรต่อวินาที */
static const bento_real_t FLOW_L_PER_S = (bento_real_t)0.5;
static const bool AUTO_AT_START = true;
enum {
    MANUAL_S = 10 /* กด SW5 = รดเองกี่วินาที */
};
/* .py: BTN_NAMES. ชื่อบนบอร์ด: SW5 = ปุ่มล่าง, SW6 = ปุ่มบน */
static const char *const BUTTON_NAMES[] = {"SW5", "SW6"};
enum {
    RUN_MS = 180000,
    TICK_MS = 500,
    CHART_EVERY = 1
};
/* ---- end of settings ---- */

enum { /* colours, 0xRRGGBB */
    COL_TEXT = 0xE8EAED,
    COL_DIM = 0x9AA3AF,
    COL_CARD = 0x171B22,
    COL_OK = 0x30A46C,
    COL_WARN = 0xF5A623,
    COL_BAD = 0xE5484D,
    COL_INFO = 0x4A9EFF
};

enum { /* hardware and units */
    SW5_INDEX = 1,
    SW6_INDEX = 0,
    VR1_INDEX = 0,
    VR2_INDEX = 1,
    VR3_INDEX = 2,
    RAW_MAX = 4095,
    PERCENT = 100,
    THRESHOLD_LOW = 20, /* VR2 sets the threshold to 20-70 % */
    THRESHOLD_SPAN = 50,
    MS_PER_S = 1000,
    BUTTON_SAMPLE_MS = 20
};

enum { /* the RGB matrix: 16 x 8, two pixels per byte */
    MATRIX_ROWS = 8,
    MATRIX_BYTES_PER_ROW = 8,
    LAST_ROW = 7,
    SOIL_X_END = 7,
    PUMP_X = 8,
    TANK_X = 11,
    TANK_X_END = 16,
    NIBBLE_BITS = 4,
    LOW_NIBBLE = 0x0F,
    HIGH_NIBBLE = 0xF0
};

enum { /* sound */
    TONE_MS = 90,
    NOTE_GAP_MS = 100,
    TUNE_NOTES_MAX = 3
};

enum { /* screen */
    SETTLE_MS = 200,
    CARD_RADIUS = 12,
    CARD_TITLE_DX = 12,
    CARD_TITLE_DY = 6,
    FONT_CARD_TITLE = 20,
    CHART_POINTS = 400,
    RULER_TICKS = 11,
    TEXT_CHARS = 128,
    NUMBER_CHARS = 36
};

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

/* ui.tone, not ui.sfx: only a tone takes a loudness on this firmware */
static bento_status_t beep(bento_ui_t *ui, tune_t name)
{
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

/* A knob scaled to span, floor division as in the .py. */
static bento_status_t read_knob(uint32_t knob, int32_t span,
                                int32_t *value)
{
    uint16_t raw = 0U;
    const bento_status_t status = bento_pots_read(knob, &raw);

    *value = ((int32_t)raw * span) / RAW_MAX;
    return status;
}

typedef struct {
    int32_t soil;      /* VR1: 0 = แห้งสนิท, 100 = แฉะ */
    int32_t threshold; /* VR2: ตั้งเกณฑ์ได้ 20-70 % */
    int32_t tank;      /* VR3: น้ำในถัง 0-100 % */
} farm_reading_t;

static bento_status_t read_farm(farm_reading_t *reading)
{
    int32_t threshold_part = 0;
    bento_status_t status =
        read_knob(VR1_INDEX, PERCENT, &reading->soil);

    if (status == BENTO_OK) {
        status = read_knob(VR2_INDEX, THRESHOLD_SPAN, &threshold_part);
    }
    reading->threshold = THRESHOLD_LOW + threshold_part;
    if (status == BENTO_OK) {
        status = read_knob(VR3_INDEX, PERCENT, &reading->tank);
    }
    return status;
}

/* An LED found by its name, or none. */
typedef struct {
    bool present;
    uint32_t index;
} board_led_t;

/* Any gpio failure means no LED, as the .py's except answers None;
 * a name the board lacks falls back to LED 0. */
static board_led_t led_named(const char *name)
{
    bento_gpio_board_info_t info;
    board_led_t led = {false, 0U};
    const char *led_name = NULL;
    bool found = false;
    uint32_t i;
    bento_status_t status;

    bento_gpio_board_info_init(&info);
    status = bento_gpio_board_info(&info);
    for (i = 0U; (status == BENTO_OK) && !found && (i < info.leds);
         i++) {
        status = bento_gpio_led_name(i, &led_name);
        found = (status == BENTO_OK) && (strcmp(led_name, name) == 0);
        if (found) {
            led.index = i;
        }
    }
    if ((status == BENTO_OK) && (led.index < info.leds)) {
        status = bento_gpio_led_off(led.index);
        led.present = status == BENTO_OK;
    }
    return led;
}

/* The blue LED stands for the pump relay. */
static bento_status_t set_pump(const board_led_t *pump, bool running)
{
    bento_status_t status = BENTO_OK;

    if (pump->present) {
        status = running ? bento_gpio_led_on(pump->index)
                         : bento_gpio_led_off(pump->index);
    }
    return status;
}

/* A base button that never misses a short press: the firmware
 * debounces for 50 ms, so it is sampled often while waiting.
 * SW5 = index 1 (lower, P17.7), SW6 = index 0 (upper, P17.5). */
typedef struct {
    uint32_t index;
    bool down;
    bool clicked;
} button_t;

static bento_status_t sample_button(button_t *button)
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

static bool pressed_now(button_t *button)
{
    const bool fired = button->clicked;

    button->clicked = false;
    return fired;
}

static bento_status_t wait_ms(int32_t ms, button_t *sw5, button_t *sw6)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    int32_t left_ms = ms;
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) && (left_ms > 0)) {
        status = sample_button(sw5);
        if (status == BENTO_OK) {
            status = sample_button(sw6);
        }
        left_ms =
            ms - bento_time_ticks_diff(bento_time_ticks_ms(), start_ms);
        if ((status == BENTO_OK) && (left_ms > 0)) {
            status = sleep_ms((left_ms < BUTTON_SAMPLE_MS)
                                  ? (uint32_t)left_ms
                                  : (uint32_t)BUTTON_SAMPLE_MS);
        }
    }
    return status;
}

static void put_pixel(uint8_t frame[], int32_t x, int32_t y,
                      int32_t color)
{
    const int32_t i = (y * MATRIX_BYTES_PER_ROW) + (x / 2);
    const uint32_t old = frame[i];

    if ((x % 2) == 1) {
        frame[i] =
            (uint8_t)((old & (uint32_t)LOW_NIBBLE) |
                      ((uint32_t)color << (uint32_t)NIBBLE_BITS));
    } else {
        frame[i] =
            (uint8_t)((old & (uint32_t)HIGH_NIBBLE) | (uint32_t)color);
    }
}

static void paint_span(uint8_t frame[], int32_t y, int32_t x_from,
                       int32_t x_end, int32_t color)
{
    int32_t x;

    for (x = x_from; x < x_end; x++) {
        put_pixel(frame, x, y, color);
    }
}

/* What the matrix shows; it is drawn again only when this changes. */
typedef struct {
    bool known;
    int32_t soil_rows;
    int32_t threshold_rows;
    int32_t tank_rows;
    bool dry;
    bool tank_ok;
    bool running;
} farm_frame_t;

static void draw_farm(const farm_frame_t *want)
{
    uint8_t frame[BENTO_RGBMATRIX_FRAME_LEN];
    const int32_t soil_color =
        want->dry ? BENTO_RGBMATRIX_YELLOW : BENTO_RGBMATRIX_BLUE;
    const int32_t tank_color =
        want->tank_ok ? BENTO_RGBMATRIX_CYAN : BENTO_RGBMATRIX_RED;
    const int32_t threshold_row =
        LAST_ROW - ((want->threshold_rows < LAST_ROW)
                        ? want->threshold_rows
                        : LAST_ROW);
    int32_t y;

    (void)memset(frame, 0, sizeof(frame));
    for (y = 0; y < MATRIX_ROWS; y++) {
        if (y >= (MATRIX_ROWS - want->soil_rows)) {
            paint_span(frame, y, 0, SOIL_X_END, soil_color);
        }
        if (y >= (MATRIX_ROWS - want->tank_rows)) {
            paint_span(frame, y, TANK_X, TANK_X_END, tank_color);
        }
        if (want->running) {
            paint_span(frame, y, PUMP_X, PUMP_X + 2,
                       BENTO_RGBMATRIX_GREEN);
        }
    }
    paint_span(frame, threshold_row, 0, SOIL_X_END,
               BENTO_RGBMATRIX_RED);
    /* the .py catches the OSError of a refused blit and goes on */
    (void)bento_rgbmatrix_blit(frame, BENTO_RGBMATRIX_FRAME_LEN);
}

static bool same_frame(const farm_frame_t *a, const farm_frame_t *b)
{
    return (a->known == b->known) && (a->soil_rows == b->soil_rows) &&
           (a->threshold_rows == b->threshold_rows) &&
           (a->tank_rows == b->tank_rows) && (a->dry == b->dry) &&
           (a->tank_ok == b->tank_ok) && (a->running == b->running);
}

static void matrix_update(const farm_reading_t *reading, bool running,
                          farm_frame_t *drawn)
{
    farm_frame_t want;

    want.known = true;
    want.soil_rows =
        (reading->soil * (int32_t)MATRIX_ROWS) / (int32_t)PERCENT;
    want.threshold_rows =
        (reading->threshold * (int32_t)MATRIX_ROWS) / (int32_t)PERCENT;
    want.tank_rows =
        (reading->tank * (int32_t)MATRIX_ROWS) / (int32_t)PERCENT;
    want.dry = reading->soil < reading->threshold;
    want.tank_ok = reading->tank >= TANK_MIN;
    want.running = running;
    if (!same_frame(&want, drawn)) {
        draw_farm(&want);
    }
    *drawn = want;
}

/* ---- 3) สมอง (ตัดสินใจ) ---- */

/* The pump rule with hysteresis: on when drier than the threshold,
 * off when wetter than threshold + HYST, unchanged in between. */
static bool pump_decision(bool pump_on, int32_t soil, int32_t threshold)
{
    bool result = pump_on;

    if (!pump_on && (soil < threshold)) {
        result = true;
    } else if (pump_on && (soil > (threshold + HYST))) {
        result = false;
    } else {
        /* between the two lines the pump keeps its state */
    }
    return result;
}

/* The pump runs when the rule or a manual SW5 watering asks, and the
 * tank has enough water. */
static bool should_run(bool auto_wants, bool manual, bool tank_ok)
{
    return (auto_wants || manual) && tank_ok;
}

/* ---- 4) หน้าจอ ---- */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
} box_t;

typedef struct {
    const char *text;
    int16_t x;
    int16_t y;
    uint32_t color;
    int32_t font_px;
} label_spec_t;

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t soil_bar;
    bento_ui_id_t soil_label;
    bento_ui_id_t rule_label;
    bento_ui_id_t pump_led;
    bento_ui_id_t pump_label;
    bento_ui_id_t mode_switch;
    bento_ui_id_t mode_label;
    bento_ui_id_t stats_label;
    bento_ui_id_t chart;
    uint8_t threshold_on_series;
    uint8_t threshold_off_series;
    bento_ui_id_t tank_bar;
    bento_ui_id_t tank_label;
    bento_ui_id_t help;
} farm_screen_t;

static void box_args(bento_ui_widget_args_t *args, const box_t *box)
{
    bento_ui_widget_args_init(args);
    args->x = box->x;
    args->y = box->y;
    args->w = box->w;
    args->h = box->h;
}

static bento_status_t
add_label(bento_ui_t *ui, const label_spec_t *spec, bento_ui_id_t *out)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.text = spec->text;
    args.text_len = (uint32_t)strlen(spec->text);
    args.x = spec->x;
    args.y = spec->y;
    args.color = spec->color;
    args.font_px = spec->font_px;
    return bento_ui_label_create(ui, &args, out);
}

/* A card: a dark box with a grey edge and a blue title. */
static bento_status_t add_card(bento_ui_t *ui, const box_t *box,
                               const char *title)
{
    bento_ui_widget_args_t args;
    bento_ui_id_t widget;
    label_spec_t heading = {title, 0, 0, COL_INFO, FONT_CARD_TITLE};
    bento_status_t status;

    box_args(&args, box);
    args.color = COL_CARD;
    args.min_val = COL_DIM;
    args.max_val = CARD_RADIUS;
    args.font_px = 1;
    status = bento_ui_panel_create(ui, &args, &widget);
    heading.x = (int16_t)(box->x + CARD_TITLE_DX);
    heading.y = (int16_t)(box->y + CARD_TITLE_DY);
    if (status == BENTO_OK) {
        status = add_label(ui, &heading, &widget);
    }
    return status;
}

/* A bar of 0-100 %; its colour is set after creation, as the .py
 * does, because a Bar ignores the colour it is created with. */
static bento_status_t add_percent_bar(bento_ui_t *ui, const box_t *box,
                                      bento_ui_id_t *out)
{
    bento_ui_widget_args_t args;

    box_args(&args, box);
    args.min_val = 0;
    args.max_val = PERCENT;
    return bento_ui_bar_create(ui, &args, out);
}

/* A widget setter's refusal was ignored in MicroPython too (modui.c). */
static void set_text(bento_ui_t *ui, bento_ui_id_t widget,
                     const char *text)
{
    (void)bento_ui_set_text(ui, widget, text, (uint32_t)strlen(text));
}

static void set_color(bento_ui_t *ui, bento_ui_id_t widget,
                      uint32_t color)
{
    (void)bento_ui_set_color(ui, widget, color);
}

static void set_value(bento_ui_t *ui, bento_ui_id_t widget,
                      int32_t value)
{
    (void)bento_ui_set_value(ui, widget, value);
}

/* The .py's ui.poll() only lets the screen catch up here; its events
 * are not read, and a failed poll answers none. */
static void refresh(bento_ui_t *ui)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    (void)bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                        NULL);
}

static bento_status_t build_soil_card(farm_screen_t *screen)
{
    static const box_t card = {12, 64, 470, 166};
    static const box_t bar = {24, 100, 446, 26};
    static const box_t ruler = {24, 128, 446, 34};
    static const label_spec_t value = {"-- %", 24, 164, COL_TEXT, 20};
    static const label_spec_t rule = {"", 24, 196, COL_WARN, 20};
    bento_ui_widget_args_t args;
    bento_ui_id_t scale;
    bento_status_t status =
        add_card(screen->ui, &card, "ความชื้นดิน (VR1 = โหนดไร้สาย)");

    if (status == BENTO_OK) {
        status = add_percent_bar(screen->ui, &bar, &screen->soil_bar);
    }
    if (status == BENTO_OK) {
        set_color(screen->ui, screen->soil_bar, COL_INFO);
        box_args(&args, &ruler);
        args.color = COL_DIM;
        args.min_val = 0;
        args.max_val = PERCENT;
        status = bento_ui_scale_create(screen->ui, &args, &scale);
    }
    if (status == BENTO_OK) {
        /* a ruler under the bar, a major tick every 2 */
        (void)bento_ui_set_ticks(screen->ui, scale, RULER_TICKS, 2);
        status = add_label(screen->ui, &value, &screen->soil_label);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &rule, &screen->rule_label);
    }
    return status;
}

static bento_status_t add_pump_led_and_label(farm_screen_t *screen)
{
    static const box_t led = {504, 100, 36, 36};
    static const label_spec_t state = {"ปิด", 552, 102, COL_DIM, 28};
    bento_ui_widget_args_t args;
    bento_status_t status;

    box_args(&args, &led);
    args.color = COL_INFO;
    status = bento_ui_led_create(screen->ui, &args, &screen->pump_led);
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &state, &screen->pump_label);
    }
    return status;
}

static bento_status_t build_pump_card(farm_screen_t *screen)
{
    static const box_t card = {492, 64, 288, 166};
    static const box_t mode_switch = {504, 146, 64, 32};
    static const label_spec_t mode = {"", 578, 150, COL_TEXT, 20};
    static const label_spec_t stats = {"เปิด 0 ครั้ง  น้ำ 0.0 ลิตร", 504,
                                       194, COL_DIM, 20};
    bento_ui_widget_args_t args;
    bento_status_t status =
        add_card(screen->ui, &card, "ปั๊มน้ำ (PLC ต่อ Wi-Fi)");

    if (status == BENTO_OK) {
        status = add_pump_led_and_label(screen);
    }
    if (status == BENTO_OK) {
        box_args(&args, &mode_switch);
        args.font_px = AUTO_AT_START ? 1 : 0;
        status = bento_ui_switch_create(screen->ui, &args,
                                        &screen->mode_switch);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &mode, &screen->mode_label);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &stats, &screen->stats_label);
    }
    return status;
}

/* The chart: blue = soil, red = the on line, green = the off line. */
static bento_status_t build_chart(farm_screen_t *screen)
{
    static const label_spec_t legend = {
        "ฟ้า = ดิน   แดง = เกณฑ์เปิด   เขียว = เกณฑ์ปิด", 12, 232, COL_DIM,
        20};
    static const box_t chart = {12, 262, 400, 76};
    bento_ui_widget_args_t args;
    bento_ui_id_t widget;
    bento_status_t status = add_label(screen->ui, &legend, &widget);

    if (status == BENTO_OK) {
        box_args(&args, &chart);
        args.color = COL_INFO;
        args.min_val = 0;
        args.max_val = PERCENT;
        status =
            bento_ui_chart_create(screen->ui, &args, &screen->chart);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_prop(screen->ui, screen->chart,
                                BENTO_UI_PROP_CHART_POINTS,
                                CHART_POINTS);
        status = bento_ui_chart_add_series(
            screen->ui, screen->chart, COL_BAD,
            &screen->threshold_on_series);
    }
    if (status == BENTO_OK) {
        status =
            bento_ui_chart_add_series(screen->ui, screen->chart, COL_OK,
                                      &screen->threshold_off_series);
    }
    return status;
}

static bento_status_t build_tank_card(farm_screen_t *screen)
{
    static const box_t card = {422, 262, 358, 86};
    static const box_t bar = {434, 298, 334, 14};
    static const label_spec_t level = {"-- %", 434, 316, COL_INFO, 20};
    bento_status_t status = add_card(
        screen->ui, &card, "น้ำในถัง (VR3 = เซนเซอร์ระดับน้ำไร้สาย)");

    if (status == BENTO_OK) {
        status = add_percent_bar(screen->ui, &bar, &screen->tank_bar);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &level, &screen->tank_label);
    }
    return status;
}

static bento_status_t add_titles(farm_screen_t *screen)
{
    static const label_spec_t title = {"ระบบรดน้ำอัตโนมัติ", 12, 4,
                                       COL_TEXT, 24};
    static const label_spec_t subtitle = {
        "งานจริง: VR1, VR3 = เซนเซอร์ไร้สาย  ไฟปั๊ม = PLC (MQTT)", 12, 34,
        COL_DIM, 20};
    bento_ui_id_t widget;
    bento_status_t status = add_label(screen->ui, &title, &widget);

    if (status == BENTO_OK) {
        status = add_label(screen->ui, &subtitle, &widget);
    }
    return status;
}

static bento_status_t build_screen(farm_screen_t *screen)
{
    char text[TEXT_CHARS];
    const label_spec_t help = {text, 12, 352, COL_DIM, 20};
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = sleep_ms(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_titles(screen);
    }
    if (status == BENTO_OK) {
        status = build_soil_card(screen);
    }
    if (status == BENTO_OK) {
        status = build_pump_card(screen);
    }
    if (status == BENTO_OK) {
        status = build_chart(screen);
    }
    if (status == BENTO_OK) {
        status = build_tank_card(screen);
    }
    if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text),
                       "%s (ล่าง) = รดเอง/หยุด   %s (บน) = ล้างตัวนับ",
                       BUTTON_NAMES[0], BUTTON_NAMES[1]);
        status = add_label(screen->ui, &help, &screen->help);
    }
    if (status == BENTO_OK) {
        refresh(screen->ui);
    }
    return status;
}

/* The on-screen switch: on = auto, off = manual. */
static bento_status_t read_auto_switch(const farm_screen_t *screen,
                                       bool *auto_mode)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;
    uint32_t i;
    bento_status_t status = BENTO_OK;

    /* a failed poll answers no events */
    if (bento_ui_poll(screen->ui, events, BENTO_UI_POLL_EVENTS_MAX,
                      &count, NULL) != BENTO_OK) {
        count = 0U;
    }
    for (i = 0U; (i < count) && (status == BENTO_OK); i++) {
        if ((events[i].id == screen->mode_switch) &&
            (events[i].type == (uint8_t)BENTO_UI_EVENT_TOGGLED)) {
            *auto_mode = events[i].value == 1;
            status = beep(screen->ui, TUNE_TAP);
        }
    }
    return status;
}

static void show_soil(const farm_screen_t *screen,
                      const farm_reading_t *reading)
{
    const bool dry = reading->soil < reading->threshold;
    char text[TEXT_CHARS];

    set_value(screen->ui, screen->soil_bar, reading->soil);
    set_color(screen->ui, screen->soil_bar, dry ? COL_BAD : COL_INFO);
    (void)snprintf(text, sizeof(text), "%" PRId32 " %%  %s",
                   reading->soil, dry ? "ดินแห้ง!" : "ดินชื้นพอ");
    set_text(screen->ui, screen->soil_label, text);
    (void)snprintf(text, sizeof(text),
                   "เปิดปั๊มเมื่อต่ำกว่า %" PRId32
                   " %% (VR2)  ปิดเมื่อเกิน %" PRId32 " %%",
                   reading->threshold, reading->threshold + HYST);
    set_text(screen->ui, screen->rule_label, text);
}

/* "%.1f" as MicroPython writes it on this board. */
static void real_text(char *text, uint32_t cap, bento_real_t value)
{
    uint32_t len = 0U;

    if (bento_fmt_real_precision(text, cap, value, 1, &len) !=
        BENTO_OK) {
        text[0] = '\0';
    }
}

static void show_pump(const farm_screen_t *screen, bool running,
                      bool tank_ok, bool auto_mode, int32_t runs,
                      bento_real_t water_l)
{
    char number[NUMBER_CHARS];
    char text[TEXT_CHARS];
    const char *state = "ปิด";
    uint32_t color = COL_DIM;

    if (running) {
        state = "เปิด";
        color = COL_OK;
    } else if (!tank_ok) {
        state = "ถังหมด";
        color = COL_BAD;
    } else {
        /* off, with water in the tank */
    }
    set_value(screen->ui, screen->pump_led, running ? 1 : 0);
    set_text(screen->ui, screen->pump_label, state);
    set_color(screen->ui, screen->pump_label, color);
    (void)snprintf(text, sizeof(text), "มือ: กด %s รดน้ำ",
                   BUTTON_NAMES[0]);
    set_text(screen->ui, screen->mode_label,
             auto_mode ? "อัตโนมัติ" : text);
    real_text(number, sizeof(number), water_l);
    (void)snprintf(text, sizeof(text), "เปิด %" PRId32 " ครั้ง  น้ำ %s ลิตร",
                   runs, number);
    set_text(screen->ui, screen->stats_label, text);
}

static void show_tank(const farm_screen_t *screen, int32_t tank,
                      bool tank_ok)
{
    const uint32_t color = tank_ok ? COL_INFO : COL_BAD;
    char text[TEXT_CHARS];

    set_value(screen->ui, screen->tank_bar, tank);
    set_color(screen->ui, screen->tank_bar, color);
    (void)snprintf(text, sizeof(text), "%" PRId32 " %%%s", tank,
                   tank_ok ? "" : "  น้อยเกินไป!");
    set_text(screen->ui, screen->tank_label, text);
    set_color(screen->ui, screen->tank_label, color);
}

/* Drawing a chart point is a setter: a refusal is ignored. */
static void show_chart(const farm_screen_t *screen,
                       const farm_reading_t *reading)
{
    const int32_t off_line = reading->threshold + HYST;

    (void)bento_ui_chart_set_next(screen->ui, screen->chart, 0U,
                                  reading->soil);
    (void)bento_ui_chart_set_next(screen->ui, screen->chart,
                                  screen->threshold_on_series,
                                  reading->threshold);
    (void)bento_ui_chart_set_next(
        screen->ui, screen->chart, screen->threshold_off_series,
        (off_line < PERCENT) ? off_line : PERCENT);
}

/* ---- 5) โปรแกรมหลัก ---- */
typedef struct {
    board_led_t pump;
    button_t sw5;
    button_t sw6;
    bool auto_mode;
    bool manual;
    uint32_t manual_ms;
    bool pump_on; /* what the rule wants */
    bool running; /* what the pump does */
    bool tank_ok;
    bool tank_was_ok;
    int32_t runs;
    bento_real_t water_l;
    int32_t tick;
    farm_frame_t drawn;
} irrigation_t;

/* SW5 starts or stops a manual watering, which ends by itself after
 * MANUAL_S seconds. */
static void update_manual(irrigation_t *farm)
{
    if (pressed_now(&farm->sw5)) {
        farm->manual = !farm->manual;
        farm->manual_ms = bento_time_ticks_ms();
    }
    if (farm->manual &&
        (bento_time_ticks_diff(bento_time_ticks_ms(), farm->manual_ms) >
         ((int32_t)MANUAL_S * (int32_t)MS_PER_S))) {
        farm->manual = false;
    }
}

static bento_status_t switch_pump(const farm_screen_t *screen,
                                  irrigation_t *farm)
{
    const bool now_running = should_run(
        farm->auto_mode && farm->pump_on, farm->manual, farm->tank_ok);
    bento_status_t status = BENTO_OK;

    if (now_running != farm->running) {
        status = beep(screen->ui, now_running ? TUNE_START : TUNE_STOP);
        if (now_running) {
            farm->runs++;
        }
    }
    farm->running = now_running;
    if ((status == BENTO_OK) && farm->tank_was_ok && !farm->tank_ok) {
        status = beep(screen->ui, TUNE_EMPTY);
    }
    farm->tank_was_ok = farm->tank_ok;
    return status;
}

/* SW6 clears the counters; water flows while the pump runs. */
static bento_status_t count_water(const farm_screen_t *screen,
                                  irrigation_t *farm)
{
    bento_status_t status = BENTO_OK;

    if (pressed_now(&farm->sw6)) {
        farm->runs = 0;
        farm->water_l = (bento_real_t)0.0;
        status = beep(screen->ui, TUNE_TAP);
    }
    if (farm->running) {
        bento_real_t litres = FLOW_L_PER_S * (bento_real_t)TICK_MS;

        litres = litres / (bento_real_t)MS_PER_S;
        farm->water_l = farm->water_l + litres;
    }
    return status;
}

static bento_status_t run_tick(const farm_screen_t *screen,
                               irrigation_t *farm)
{
    farm_reading_t reading;
    bento_status_t status = read_auto_switch(screen, &farm->auto_mode);

    if (status == BENTO_OK) {
        status = read_farm(&reading);
    }
    if (status == BENTO_OK) {
        farm->pump_on = pump_decision(farm->pump_on, reading.soil,
                                      reading.threshold);
        farm->tank_ok = reading.tank >= TANK_MIN;
        update_manual(farm);
        status = switch_pump(screen, farm);
    }
    if (status == BENTO_OK) {
        status = count_water(screen, farm);
    }
    if (status == BENTO_OK) {
        status = set_pump(&farm->pump, farm->running);
    }
    if (status == BENTO_OK) {
        matrix_update(&reading, farm->running, &farm->drawn);
        show_soil(screen, &reading);
        show_pump(screen, farm->running, farm->tank_ok, farm->auto_mode,
                  farm->runs, farm->water_l);
        show_tank(screen, reading.tank, farm->tank_ok);
        if ((farm->tick % CHART_EVERY) == 0) {
            show_chart(screen, &reading);
        }
        farm->tick++;
        status = wait_ms(TICK_MS, &farm->sw5, &farm->sw6);
    }
    return status;
}

static bento_status_t finish(const farm_screen_t *screen,
                             const irrigation_t *farm)
{
    char number[NUMBER_CHARS];
    const bento_status_t status = set_pump(&farm->pump, false);

    if (status == BENTO_OK) {
        /* the .py catches the OSError of a refused clear */
        (void)bento_rgbmatrix_clear();
        set_color(screen->ui, screen->help, COL_WARN);
        set_text(screen->ui, screen->help,
                 "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่");
        refresh(screen->ui);
        real_text(number, sizeof(number), farm->water_l);
        (void)printf("ปั๊มเปิด %" PRId32 " ครั้ง ใช้น้ำ %s ลิตร\n", farm->runs,
                     number);
    }
    return status;
}

static bento_status_t run_farm(const farm_screen_t *screen,
                               irrigation_t *farm)
{
    int32_t tank = 0;
    uint32_t start_ms;
    bento_status_t status = read_knob(VR3_INDEX, PERCENT, &tank);

    farm->tank_was_ok = tank >= TANK_MIN;
    start_ms = bento_time_ticks_ms();
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = run_tick(screen, farm);
    }
    if (status == BENTO_OK) {
        status = finish(screen, farm);
    }
    return status;
}

int main(void)
{
    farm_screen_t screen;
    irrigation_t farm;
    bento_status_t status;

    (void)memset(&screen, 0, sizeof(screen));
    (void)memset(&farm, 0, sizeof(farm));
    screen.ui = bento_runtime_get_ui();
    /* a build without a speaker volume refuses it, as hasattr() skips
     * it */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    status = build_screen(&screen);
    if (status == BENTO_OK) {
        farm.pump = led_named("RGB_BLUE");
        farm.sw5.index = SW5_INDEX;
        farm.sw6.index = SW6_INDEX;
        farm.auto_mode = AUTO_AT_START;
        farm.water_l = (bento_real_t)0.0;
        status = run_farm(&screen, &farm);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
