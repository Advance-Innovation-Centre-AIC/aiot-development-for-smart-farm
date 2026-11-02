/*
 * sf1_05_my_farm_dashboard.c -- ภารกิจกลุ่ม: แผงควบคุมฟาร์มของเรา
 * (Group mission: our farm dashboard)
 *
 * ภารกิจ: รวมอากาศ (SHT40) + ดิน (VR1) + ปั๊มอัตโนมัติ + คะแนนสุขภาพ
 * ฟาร์ม แล้วแก้ทุกบรรทัด TODO ให้เป็นฟาร์มกลุ่ม ลองเล่น: เป่าลม จับบอร์ด
 * หมุน VR1 แตะแท็บ ภาพรวม / กราฟ SW5 = รดเอง SW6 = สลับจอไฟ RGB
 * (คะแนน / อุณหภูมิ) ส่งงาน: ถ่ายรูปจอตอนคะแนนสูงสุดและต่ำสุด แนบในใบงาน
 * ทำงาน 5 นาทีแล้วจบเอง
 *
 * Combines the air (SHT40), the soil (VR1), an automatic pump (the
 * blue LED) and a farm health score; the group edits every TODO line
 * to make it their farm. Blow on or hold the board, turn VR1, tap the
 * Overview / Chart tabs; SW5 waters by hand for MANUAL_S seconds and
 * SW6 switches the RGB matrix between the score and the temperature.
 * It ends by itself after 5 minutes.
 *
 * Twin of sf1_05_my_farm_dashboard.py (aiot-development-for-smart-farm/s1/sf1_05_my_farm_dashboard.py). Style: CLEAN_C.md.
 * Differs from the .py:
 *   - None ("no value yet") is a known = false flag.
 *   - A sensor read that raises is a status here; the same fallback
 *     is taken (SHT40 tried 3 times, then no reading this round).
 *   - The pump LED is found by name through bento_gpio_led_name; the
 *     .py's try/except around it is the same "no LED" fallback.
 *   - The advice list is counted, not kept: only its first tip and
 *     the number of the others are shown, as in the .py.
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
#include "bento_gpio.h"
#include "bento_pots.h"
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_sensors.h"
#include "bento_time.h"

/* ---- แก้ก่อนรัน: settings (MISRA deviation CT02) ---- */
enum {
    SPEAKER = 40, /* ความดังลำโพงรวม 0-100% (fw 2.4.2+) */
    VOLUME = 25   /* ความดังเสียง 0-127 */
};
static const char FARM_NAME[] = "ฟาร์มกลุ่มที่ ?"; /* TODO: ตั้งชื่อฟาร์มของกลุ่ม */
static const char CROP[] = "มะเขือเทศ";        /* TODO: พืชหรือสัตว์ที่ดูแล */
enum {
    T_LO = 20, /* TODO: ช่วงอุณหภูมิที่เหมาะ (C) */
    T_HI = 30,
    H_LO = 60, /* TODO: ช่วงความชื้นอากาศที่เหมาะ (%) */
    H_HI = 80,
    SOIL_MIN = 40, /* TODO: ดินแห้งกว่านี้ให้รดน้ำ (%) */
    MANUAL_S = 10, /* กด SW5 = รดเองกี่วินาที */
    SOIL_HYST = 5  /* ช่องกันกระพือของปั๊ม (%) เหมือน sf1_03 */
};
/* TODO: ค่าชดเชยเดียวกับที่กลุ่มหาได้ใน sf1_01 เช่น -9.5 */
static const bento_real_t TEMP_OFFSET = (bento_real_t)0.0;
/* แปลงความชื้นเป็นของห้อง (สูงเกินจริง = false) */
static const bool HUM_FIX = true;
enum {
    RUN_MS = 300000,
    TICK_MS = 500,
    TAB_BAR_H = 44,
    ALERT_GAP_MS = 3000
};
/* .py: BTN_NAMES. ชื่อบนบอร์ด: SW5 = ปุ่มล่าง, SW6 = ปุ่มบน */
static const char *const BUTTON_NAMES[] = {"SW5", "SW6"};
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

enum { /* zones, sound, sensors, buttons and the screen */
    ZONES = 3,
    TUNE_NOTES_MAX = 3,
    TONE_MS = 90,
    NOTE_GAP_MS = 100,
    SHT40_TRIES = 3,
    RETRY_MS = 20,
    SAMPLE_MS = 20,
    SW5_INDEX = 1,
    SW6_INDEX = 0,
    VR1_INDEX = 0,
    RAW_MAX = 4095,
    MS_PER_S = 1000,
    SETTLE_MS = 200,
    PERCENT_MAX = 100,
    CHART_POINTS = 400,
    COST_AIR_T = 35, /* TODO: ปรับน้ำหนักตามความสำคัญของงานกลุ่ม */
    COST_AIR_H = 25,
    COST_SOIL = 40,
    ZONE_GOOD = 80,
    ZONE_FAIR = 50,
    TEXT_CHARS = 128
};

/* The zone colours live in show() (screen) and matrix_show() (RGB
 * matrix), each the one function that reads them. */

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

/* ---- 3) สมอง: room humidity, used by the reading below ---- */
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

/* The room's temperature and humidity, or none this round. */
typedef struct {
    bool known;
    bento_real_t t;
    bento_real_t h;
} climate_t;

/* SHT40 may miss a read now and then, so it is tried 3 times; after
 * 3 misses there is no reading this round. */
static bento_status_t read_climate(climate_t *climate)
{
    float t_raw = 0.0F;
    float h = 0.0F;
    uint32_t tries = 0U;
    bento_status_t status = BENTO_OK;

    climate->known = false;
    climate->t = (bento_real_t)0;
    climate->h = (bento_real_t)0;
    while ((status == BENTO_OK) && !climate->known &&
           (tries < (uint32_t)SHT40_TRIES)) {
        tries++;
        climate->known =
            (bento_sensors_sht40_temperature(&t_raw) == BENTO_OK) &&
            (bento_sensors_sht40_humidity(&h) == BENTO_OK);
        if (!climate->known) {
            status = sleep_ms(RETRY_MS);
        }
    }
    if (climate->known) {
        climate->t = (bento_real_t)t_raw + TEMP_OFFSET;
        climate->h = (bento_real_t)h;
        if ((TEMP_OFFSET != (bento_real_t)0) && HUM_FIX) {
            climate->h = room_humidity(climate->h, (bento_real_t)t_raw,
                                       climate->t);
        }
    }
    return status;
}

/* An LED found by its name, or none (the .py's None). */
typedef struct {
    bool present;
    uint32_t index;
} board_led_t;

/* An unknown name falls back to LED 0, and a board that refuses it
 * has no LED here. */
static void led_named(const char *name, board_led_t *led)
{
    const uint32_t count = bento_gpio_num_leds();
    const char *led_name = NULL;
    bool found = false;
    uint32_t i;

    led->index = 0U;
    for (i = 0U; (i < count) && !found; i++) {
        found = (bento_gpio_led_name(i, &led_name) == BENTO_OK) &&
                (strcmp(led_name, name) == 0);
        if (found) {
            led->index = i;
        }
    }
    led->present = bento_gpio_led_off(led->index) == BENTO_OK;
}

static bento_status_t set_pump(const board_led_t *pump, bool running)
{
    bento_status_t status = BENTO_OK;

    if (pump->present) {
        status = running ? bento_gpio_led_on(pump->index)
                         : bento_gpio_led_off(pump->index);
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

/* Waits ms while it keeps reading both buttons. */
static bento_status_t wait_ms(int32_t ms, button_t *sw5, button_t *sw6)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    while (
        (status == BENTO_OK) &&
        (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) < ms)) {
        status = button_sample(sw5);
        if (status == BENTO_OK) {
            status = button_sample(sw6);
        }
        if (status == BENTO_OK) {
            status = sleep_ms(SAMPLE_MS);
        }
    }
    return status;
}

/* What the RGB matrix shows: drawn again only when it changes. */
typedef struct {
    bool known;
    int32_t value;
    int32_t color;
} matrix_shown_t;

static bento_status_t matrix_show(bool show_temp,
                                  const climate_t *climate,
                                  int32_t score, int32_t zone,
                                  matrix_shown_t *shown)
{
    static const int32_t matrix_colors[ZONES] = {BENTO_RGBMATRIX_GREEN,
                                                 BENTO_RGBMATRIX_YELLOW,
                                                 BENTO_RGBMATRIX_RED};
    int32_t value = score;
    int32_t color = matrix_colors[zone];
    bento_status_t status = BENTO_OK;

    if (show_temp && climate->known) {
        const bento_real_t half = (bento_real_t)0.5;
        const bento_real_t rounded = climate->t + half;

        value = (int32_t)rounded;
        color = BENTO_RGBMATRIX_CYAN;
    }
    if (!shown->known || (value != shown->value) ||
        (color != shown->color)) {
        status = bento_rgbmatrix_score(value, color);
    }
    shown->known = true;
    shown->value = value;
    shown->color = color;
    return status;
}

/* ---- 3) สมอง (ตัดสินใจ) ---- */

/* The pump rule with hysteresis: on below SOIL_MIN, off above
 * SOIL_MIN + SOIL_HYST. */
static bool pump_decision(bool pump_on, int32_t soil)
{
    bool result = pump_on;

    if (!pump_on && (soil < SOIL_MIN)) {
        result = true;
    } else if (pump_on && (soil > (SOIL_MIN + SOIL_HYST))) {
        result = false;
    } else {
        /* between the lines: the pump stays as it is */
    }
    return result;
}

static bool out_of(bento_real_t value, int32_t lo, int32_t hi)
{
    return (value < (bento_real_t)lo) || (value > (bento_real_t)hi);
}

/* The farm health score 0-100. */
static int32_t health(const climate_t *climate, int32_t soil)
{
    int32_t score = 0;

    if (climate->known) {
        score = PERCENT_MAX;
        if (out_of(climate->t, T_LO, T_HI)) {
            score -= COST_AIR_T;
        }
        if (out_of(climate->h, H_LO, H_HI)) {
            score -= COST_AIR_H;
        }
        if (soil < SOIL_MIN) {
            score -= COST_SOIL;
        }
    }
    return (score > 0) ? score : 0;
}

/* 0 = good (80+), 1 = fair, 2 = bad (below 50). */
static int32_t zone_of(int32_t score)
{
    int32_t zone = 2;

    if (score >= ZONE_GOOD) {
        zone = 0;
    } else if (score >= ZONE_FAIR) {
        zone = 1;
    } else {
        /* below fair: bad */
    }
    return zone;
}

/* The tips for the farm right now: the first one and how many there
 * are (TODO: เพิ่มคำแนะนำของกลุ่มคุณเอง). */
typedef struct {
    const char *first;
    int32_t count;
} advice_t;

static void add_tip(advice_t *advice, bool needed, const char *tip)
{
    if (needed) {
        if (advice->count == 0) {
            advice->first = tip;
        }
        advice->count++;
    }
}

static void advice_parts(const climate_t *climate, int32_t soil,
                         advice_t *advice)
{
    const bool known = climate->known;

    advice->first = NULL;
    advice->count = 0;
    add_tip(advice, known && (climate->t > (bento_real_t)T_HI),
            "เปิดพัดลม/พ่นหมอก");
    add_tip(advice, known && (climate->t < (bento_real_t)T_LO),
            "ปิดม่านกันลม");
    add_tip(advice, known && (climate->h < (bento_real_t)H_LO),
            "เพิ่มความชื้น");
    add_tip(advice, known && (climate->h > (bento_real_t)H_HI),
            "เปิดระบายอากาศ");
    add_tip(advice, soil < SOIL_MIN, "กำลังรดน้ำ");
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
    bento_ui_id_t arc;
    bento_ui_id_t score;
    bento_ui_id_t air;
    bento_ui_id_t bar_soil;
    bento_ui_id_t lbl_soil;
    bento_ui_id_t led;
    bento_ui_id_t lbl_pump;
    bento_ui_id_t event;
    bento_ui_id_t chart;
    uint8_t s_soil;
    bento_ui_id_t advice;
} farm_screen_t;

/* parent 0 = the screen; else a tab page. */
static bento_status_t add_child(bento_ui_t *ui,
                                const widget_spec_t *spec,
                                const char *text, bento_ui_id_t parent,
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
    args.parent = parent;
    return spec->make(ui, &args, out);
}

static bento_status_t add_widget(bento_ui_t *ui,
                                 const widget_spec_t *spec,
                                 bento_ui_id_t *out)
{
    return add_child(ui, spec, spec->text, 0U, out);
}

/* A widget setter's refusal was ignored in MicroPython too (modui.c). */
static void set_text(bento_ui_t *ui, bento_ui_id_t widget,
                     const char *text)
{
    (void)bento_ui_set_text(ui, widget, text, (uint32_t)strlen(text));
}

/* The .py's ui.poll() only lets the screen catch up; its events are
 * not read (the tabs switch by themselves), and a failed poll answers
 * none. */
static void refresh(bento_ui_t *ui)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    (void)bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                        NULL);
}

static bento_status_t build_overview_tab(farm_screen_t *screen,
                                         bento_ui_id_t tab)
{
    static const widget_spec_t specs[] = {
        {bento_ui_arc_create, NULL, 0, 0, 150, 150, 0, 0, 100, 0},
        {bento_ui_label_create, "0", 50, 54, 0, 0, COL_TEXT, 0, 0, 28},
        {bento_ui_label_create, "สุขภาพฟาร์ม", 20, 156, 0, 0, COL_INFO, 0,
         0, 20},
        {bento_ui_label_create, "อากาศ -- C  -- %RH", 170, 0, 0, 0,
         COL_TEXT, 0, 0, 24},
        {bento_ui_label_create, "ความชื้นดิน (VR1)", 170, 36, 0, 0,
         COL_INFO, 0, 0, 20},
        {bento_ui_bar_create, NULL, 170, 66, 360, 24, 0, 0, 100, 0},
        {bento_ui_label_create, "-- %", 544, 62, 0, 0, COL_TEXT, 0, 0,
         20},
        {bento_ui_led_create, NULL, 170, 104, 32, 32, COL_INFO, 0, 0,
         0},
        {bento_ui_label_create, "ปั๊มน้ำ: ปิด", 214, 108, 0, 0, COL_DIM, 0,
         0, 20}};
    static const widget_spec_t event = {
        bento_ui_label_create, NULL, 170, 156, 0, 0, COL_DIM, 0, 0, 20};
    bento_ui_id_t *const outs[] = {
        &screen->arc,      &screen->score, NULL,
        &screen->air,      NULL,           &screen->bar_soil,
        &screen->lbl_soil, &screen->led,   &screen->lbl_pump};
    const uint32_t count = (uint32_t)(sizeof(specs) / sizeof(specs[0]));
    char text[TEXT_CHARS];
    bento_ui_id_t widget;
    uint32_t i;
    bento_status_t status = BENTO_OK;

    for (i = 0U; (i < count) && (status == BENTO_OK); i++) {
        status = add_child(screen->ui, &specs[i], specs[i].text, tab,
                           (outs[i] != NULL) ? outs[i] : &widget);
    }
    if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text), "%s (บน) = สลับจอไฟ RGB",
                       BUTTON_NAMES[1]);
        status =
            add_child(screen->ui, &event, text, tab, &screen->event);
    }
    return status;
}

static bento_status_t build_chart_tab(farm_screen_t *screen,
                                      bento_ui_id_t tab)
{
    static const widget_spec_t chart = {bento_ui_chart_create,
                                        NULL,
                                        0,
                                        0,
                                        CHART_POINTS,
                                        180,
                                        COL_WARN,
                                        0,
                                        100,
                                        0};
    static const widget_spec_t legend = {bento_ui_label_create,
                                         "ส้ม = อุณหภูมิ\nเขียว = ชื้นดิน",
                                         420,
                                         10,
                                         0,
                                         0,
                                         COL_TEXT,
                                         0,
                                         0,
                                         20};
    bento_ui_id_t widget;
    bento_status_t status =
        add_child(screen->ui, &chart, NULL, tab, &screen->chart);

    if (status == BENTO_OK) {
        status =
            bento_ui_set_prop(screen->ui, screen->chart,
                              BENTO_UI_PROP_CHART_POINTS, CHART_POINTS);
    }
    if (status == BENTO_OK) {
        status = bento_ui_chart_add_series(screen->ui, screen->chart,
                                           COL_OK, &screen->s_soil);
    }
    if (status == BENTO_OK) {
        status =
            add_child(screen->ui, &legend, legend.text, tab, &widget);
    }
    return status;
}

static bento_status_t add_tab(bento_ui_t *ui, bento_ui_id_t tabs,
                              const char *name, bento_ui_id_t *page)
{
    return bento_ui_add_tab(ui, tabs, name, (uint32_t)strlen(name),
                            page);
}

static bento_status_t build_tabs(farm_screen_t *screen)
{
    static const widget_spec_t tabview = {bento_ui_tabview_create,
                                          NULL,
                                          12,
                                          44,
                                          768,
                                          294,
                                          0,
                                          0,
                                          0,
                                          TAB_BAR_H};
    bento_ui_id_t tabs;
    bento_ui_id_t page;
    bento_status_t status = add_widget(screen->ui, &tabview, &tabs);

    if (status == BENTO_OK) {
        status = add_tab(screen->ui, tabs, "ภาพรวม", &page);
    }
    if (status == BENTO_OK) {
        status = build_overview_tab(screen, page);
    }
    if (status == BENTO_OK) {
        status = add_tab(screen->ui, tabs, "กราฟ", &page);
    }
    if (status == BENTO_OK) {
        status = build_chart_tab(screen, page);
    }
    return status;
}

static bento_status_t build_screen(farm_screen_t *screen)
{
    static const widget_spec_t title = {
        bento_ui_label_create, NULL, 12, 6, 0, 0, COL_TEXT, 0, 0, 24};
    static const widget_spec_t advice = {
        bento_ui_label_create, " ", 12, 352, 0, 0, COL_WARN, 0, 0, 20};
    char text[TEXT_CHARS];
    bento_ui_id_t widget;
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = sleep_ms(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text), "%s - %s", FARM_NAME, CROP);
        status = add_child(screen->ui, &title, text, 0U, &widget);
    }
    if (status == BENTO_OK) {
        status = build_tabs(screen);
    }
    if (status == BENTO_OK) {
        status = add_widget(screen->ui, &advice, &screen->advice);
    }
    if (status == BENTO_OK) {
        refresh(screen->ui);
    }
    return status;
}

static bento_status_t note_event(const farm_screen_t *screen,
                                 int32_t sec, const char *what,
                                 int32_t score, tune_t sound)
{
    char text[TEXT_CHARS];
    const bento_status_t status = beep(screen->ui, sound);

    if (status == BENTO_OK) {
        (void)bento_ui_set_color(screen->ui, screen->event, COL_TEXT);
        (void)snprintf(text, sizeof(text),
                       "วินาที %" PRId32 ": %s (คะแนน %" PRId32 ")", sec,
                       what, score);
        set_text(screen->ui, screen->event, text);
    }
    return status;
}

/* "--" when there is no reading. */
static void format_air(char *out, uint32_t cap,
                       const climate_t *climate, bento_real_t value,
                       int32_t digits)
{
    uint32_t len = 0U;

    if (!climate->known ||
        (bento_fmt_real_precision(out, cap, value, digits, &len) !=
         BENTO_OK)) {
        (void)snprintf(out, cap, "--");
    }
}

/* What one round shows. */
typedef struct {
    climate_t climate;
    int32_t soil;
    bool running;
    int32_t score;
    int32_t zone;
} farm_view_t;

static void show_air_and_soil(const farm_screen_t *screen,
                              const farm_view_t *view)
{
    char t_text[BENTO_FMT_BUF_SIZE];
    char h_text[BENTO_FMT_BUF_SIZE];
    char text[TEXT_CHARS];

    format_air(t_text, sizeof(t_text), &view->climate, view->climate.t,
               1);
    format_air(h_text, sizeof(h_text), &view->climate, view->climate.h,
               0);
    (void)snprintf(text, sizeof(text), "อากาศ %s C  %s %%RH", t_text,
                   h_text);
    set_text(screen->ui, screen->air, text);
    (void)bento_ui_set_value(screen->ui, screen->bar_soil, view->soil);
    (void)bento_ui_set_color(screen->ui, screen->bar_soil,
                             (view->soil < SOIL_MIN) ? COL_BAD
                                                     : COL_INFO);
    (void)snprintf(text, sizeof(text), "%" PRId32 " %%", view->soil);
    set_text(screen->ui, screen->lbl_soil, text);
}

static void show_trend_and_advice(const farm_screen_t *screen,
                                  const farm_view_t *view)
{
    const bento_real_t t = view->climate.t;
    advice_t advice;
    char text[TEXT_CHARS];

    if (view->climate.known) {
        (void)bento_ui_chart_set_next(
            screen->ui, screen->chart, 0U,
            (t < (bento_real_t)0)
                ? 0
                : ((t > (bento_real_t)PERCENT_MAX) ? PERCENT_MAX
                                                   : (int32_t)t));
    }
    (void)bento_ui_chart_set_next(screen->ui, screen->chart,
                                  screen->s_soil, view->soil);
    advice_parts(&view->climate, view->soil, &advice);
    if (advice.count > 1) {
        (void)snprintf(text, sizeof(text), "แนะนำ: %s (+%" PRId32 ")",
                       advice.first, advice.count - 1);
    } else if (advice.count == 1) {
        (void)snprintf(text, sizeof(text), "แนะนำ: %s", advice.first);
    } else {
        (void)snprintf(text, sizeof(text), "แนะนำ: ทุกอย่างปกติ");
    }
    set_text(screen->ui, screen->advice, text);
    (void)bento_ui_set_color(screen->ui, screen->advice,
                             (advice.count > 0) ? COL_WARN : COL_OK);
}

static void show(const farm_screen_t *screen, const farm_view_t *view)
{
    static const uint32_t zone_colors[ZONES] = {COL_OK, COL_WARN,
                                                COL_BAD};
    char text[TEXT_CHARS];

    (void)bento_ui_set_value(screen->ui, screen->arc, view->score);
    (void)bento_ui_set_color(screen->ui, screen->arc,
                             zone_colors[view->zone]);
    (void)snprintf(text, sizeof(text), "%" PRId32, view->score);
    set_text(screen->ui, screen->score, text);
    show_air_and_soil(screen, view);
    (void)bento_ui_set_value(screen->ui, screen->led,
                             view->running ? 1 : 0);
    set_text(screen->ui, screen->lbl_pump,
             view->running ? "ปั๊มน้ำ: เปิด" : "ปั๊มน้ำ: ปิด");
    (void)bento_ui_set_color(screen->ui, screen->lbl_pump,
                             view->running ? COL_OK : COL_DIM);
    show_trend_and_advice(screen, view);
}

/* ---- 5) โปรแกรมหลัก ---- */
typedef struct {
    board_led_t pump;
    button_t sw5;
    button_t sw6;
    bool pump_on;
    bool running;
    bool show_temp;
    bool manual;
    uint32_t manual_ms;
    matrix_shown_t shown;
    bool zone_known;
    int32_t zone_was;
    uint32_t last_alert_ms;
} farm_t;

/* SW5 starts or stops watering by hand, for MANUAL_S seconds. */
static void update_manual(farm_t *farm)
{
    if (button_pressed_now(&farm->sw5)) {
        farm->manual = !farm->manual;
        farm->manual_ms = bento_time_ticks_ms();
    }
    if (farm->manual &&
        (bento_time_ticks_diff(bento_time_ticks_ms(), farm->manual_ms) >
         ((int32_t)MANUAL_S * (int32_t)MS_PER_S))) {
        farm->manual = false;
    }
}

/* The pump acts only when it changes: the automatic rule or watering
 * by hand. */
static bento_status_t act_on_pump(const farm_screen_t *screen,
                                  farm_t *farm, int32_t sec,
                                  int32_t score)
{
    const bool now_running = farm->pump_on || farm->manual;
    bento_status_t status = BENTO_OK;

    if (now_running != farm->running) {
        status =
            note_event(screen, sec, now_running ? "ปั๊มเปิด" : "ปั๊มปิด",
                       score, now_running ? TUNE_START : TUNE_STOP);
        if (status == BENTO_OK) {
            status = set_pump(&farm->pump, now_running);
        }
    }
    farm->running = now_running;
    return status;
}

static bento_status_t act_on_zone(const farm_screen_t *screen,
                                  farm_t *farm, int32_t sec,
                                  const farm_view_t *view)
{
    bool worse;
    bento_status_t status = BENTO_OK;

    if (!farm->zone_known) {
        farm->zone_known = true;
        farm->zone_was = view->zone;
    } else if ((view->zone != farm->zone_was) &&
               (bento_time_ticks_diff(bento_time_ticks_ms(),
                                      farm->last_alert_ms) >
                ALERT_GAP_MS)) {
        worse = view->zone > farm->zone_was;
        status = note_event(screen, sec, worse ? "ฟาร์มแย่ลง" : "ฟาร์มดีขึ้น",
                            view->score, worse ? TUNE_BAD : TUNE_GOOD);
        farm->zone_was = view->zone;
        farm->last_alert_ms = bento_time_ticks_ms();
    } else {
        /* the same zone, or too soon after the last alert */
    }
    return status;
}

/* Read, think, act, show. */
static bento_status_t run_round(const farm_screen_t *screen,
                                farm_t *farm, int32_t sec)
{
    farm_view_t view;
    uint16_t raw = 0U;
    bento_status_t status = read_climate(&view.climate);

    if (status == BENTO_OK) {
        status = bento_pots_read(VR1_INDEX, &raw);
    }
    if (status == BENTO_OK) {
        /* VR1: 0 = bone dry, 100 = soaked */
        view.soil = ((int32_t)raw * PERCENT_MAX) / RAW_MAX;
        view.score = health(&view.climate, view.soil);
        view.zone = zone_of(view.score);
        farm->pump_on = pump_decision(farm->pump_on, view.soil);
        update_manual(farm);
        status = act_on_pump(screen, farm, sec, view.score);
    }
    if (status == BENTO_OK) {
        status = act_on_zone(screen, farm, sec, &view);
    }
    if ((status == BENTO_OK) && button_pressed_now(&farm->sw6)) {
        farm->show_temp = !farm->show_temp;
        status = beep(screen->ui, TUNE_TAP);
    }
    if (status == BENTO_OK) {
        status = matrix_show(farm->show_temp, &view.climate, view.score,
                             view.zone, &farm->shown);
    }
    if (status == BENTO_OK) {
        view.running = farm->running;
        show(screen, &view);
        refresh(screen->ui);
    }
    return status;
}

static bento_status_t run_loop(const farm_screen_t *screen,
                               farm_t *farm)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    int32_t sec;
    bento_status_t status = BENTO_OK;

    farm->last_alert_ms = start_ms;
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        sec = bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) /
              MS_PER_S;
        status = run_round(screen, farm, sec);
        if (status == BENTO_OK) {
            status = wait_ms(TICK_MS, &farm->sw5, &farm->sw6);
        }
    }
    return status;
}

static bento_status_t finish(const farm_screen_t *screen,
                             const farm_t *farm)
{
    const bento_status_t status = set_pump(&farm->pump, false);

    if (status == BENTO_OK) {
        /* the .py ignores an OSError of the matrix here */
        (void)bento_rgbmatrix_clear();
        (void)bento_ui_set_color(screen->ui, screen->advice, COL_WARN);
        set_text(screen->ui, screen->advice,
                 "จบรอบ - กด Program to Device เพื่อเล่นใหม่");
        refresh(screen->ui);
    }
    return status;
}

int main(void)
{
    farm_screen_t screen;
    farm_t farm;
    bento_status_t status;

    (void)memset(&screen, 0, sizeof(screen));
    (void)memset(&farm, 0, sizeof(farm));
    screen.ui = bento_runtime_get_ui();
    farm.sw5.index = SW5_INDEX;
    farm.sw6.index = SW6_INDEX;
    /* a build without a speaker volume refuses it, as hasattr() skips
     * it */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    status = build_screen(&screen);
    if (status == BENTO_OK) {
        led_named("RGB_BLUE", &farm.pump); /* the blue LED = the pump */
        status = run_loop(&screen, &farm);
    }
    if (status == BENTO_OK) {
        status = finish(&screen, &farm);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
