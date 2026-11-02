/*
 * sf1_02_crop_comfort.c -- พืชของเรา "สบายดี" ไหม
 * (Crop comfort: is our crop comfortable)
 *
 * ภารกิจ: ตั้งช่วงอุณหภูมิ/ความชื้นที่พืชชอบ บอร์ดตัดสินทุกวินาที:
 * สบาย / เริ่มเครียด / แย่แล้ว ลองเล่น: ปัดวงล้อเลือกพืช แล้วเป่าลม/
 * จับบอร์ดให้หลุดช่วง (ตั้ง TEMP_OFFSET ก่อน) ใช้ SHT40, ไฟ RGB_RED,
 * จอไฟ RGB = หน้าพืช และลำโพง (เฉพาะตอนสถานะเปลี่ยน)
 * แนวคิด AIoT: Sense (อ่าน) -> Decide (กฎ) -> Act (ไฟ/จอ/เสียง)
 *
 * Set the temperature and humidity ranges a crop likes; every second
 * the board judges: comfortable, getting stressed, or bad. Swipe the
 * roller to pick a crop, then blow on or hold the board to leave the
 * range (set TEMP_OFFSET first). It uses the SHT40, the RGB_RED LED,
 * a crop face on the RGB matrix and the speaker (only when the level
 * changes). It ends by itself after 2 minutes.
 *
 * Twin of sf1_02_crop_comfort.py (aiot-development-for-smart-farm/s1/sf1_02_crop_comfort.py). Style: CLEAN_C.md.
 * Differs from the .py:
 *   - None ("no value yet") is a known = false flag.
 *   - A sensor read that raises is a status here; the same fallback
 *     is taken (SHT40 tried 3 times, then no reading this round).
 *   - The LED is found by name through bento_gpio_led_name; the
 *     .py's try/except around it is the same "no LED" fallback.
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

#include "bento_fmt.h"
#include "bento_gpio.h"
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_sensors.h"
#include "bento_time.h"

/* ---- 1) ตั้งค่า (แก้ได้) ---- */
enum {
    SPEAKER = 40, /* ความดังลำโพงรวม 0-100% (fw 2.4.2+) */
    VOLUME = 25,  /* ความดังเสียง 0-127 */
    RUN_MS = 120000,
    TICK_MS = 1000,
    CHART_MAX_C = 50,
    SOUND_GAP_MS = 3000 /* เสียงเตือนห่างกันอย่างน้อยเท่านี้ */
};

/* ช่วงที่พืชชอบ (ตัวเลขเพื่อการเรียน): ชื่อ, T ต่ำ, T สูง, RH ต่ำ, RH สูง */
typedef struct {
    const char *name;
    int32_t t_lo;
    int32_t t_hi;
    int32_t h_lo;
    int32_t h_hi;
} crop_t;

static const crop_t CROPS[] = {{"มะเขือเทศ", 20, 30, 60, 80},
                               {"ผักสลัด", 15, 25, 50, 70},
                               {"เห็ดนางฟ้า", 22, 28, 80, 95},
                               {"กล้วยไม้", 22, 32, 60, 80}};
/* พืชตอนเริ่ม (เปลี่ยนด้วยวงล้อบนจอ) */
static const char START_CROP[] = "มะเขือเทศ";
/* ค่าชดเชย: เทียบเทอร์โมมิเตอร์ในห้อง เช่น -9.5 */
static const bento_real_t TEMP_OFFSET = (bento_real_t)0.0;
/* แปลงความชื้นเป็นของห้อง (สูงเกินจริง = false) */
static const bool HUM_FIX = true;

enum { /* colours, 0xRRGGBB */
    COL_TEXT = 0xE8EAED,
    COL_DIM = 0x9AA3AF,
    COL_CARD = 0x171B22,
    COL_OK = 0x30A46C,
    COL_WARN = 0xF5A623,
    COL_BAD = 0xE5484D,
    COL_INFO = 0x4A9EFF
};

enum { /* levels, sound, sensors, the matrix and the screen */
    LEVELS = 3,
    TUNE_NOTES_MAX = 3,
    TONE_MS = 90,
    NOTE_GAP_MS = 100,
    SHT40_TRIES = 3,
    RETRY_MS = 20,
    SETTLE_MS = 200,
    PERCENT_MAX = 100,
    CHART_POINTS = 400,
    COLD_MARGIN_C = 3, /* this far outside the range = bad */
    DRY_MARGIN_RH = 10,
    SCORE_GOOD = 80,
    SCORE_FAIR = 50,
    FACE_ROWS = 8,
    FACE_LEFT_X = 4, /* the 8x8 face sits in the middle of 16x8 */
    MATRIX_ROW_BYTES = 8,
    NIBBLE_BITS = 4,
    TEXT_CHARS = 128
};

static const uint32_t LEVEL_COLORS[LEVELS] = {COL_OK, COL_WARN,
                                              COL_BAD};
static const char *const MOODS[LEVELS] = {"สบายดี :)", "เริ่มเครียด",
                                          "แย่แล้ว!"};

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

/* ui.tone, not ui.sfx: only a tone takes a loudness on this firmware */
static const tune_notes_t TUNES[] = {
    {1U, {76U, 0U, 0U}},   {2U, {72U, 79U, 0U}}, {2U, {79U, 72U, 0U}},
    {3U, {72U, 79U, 84U}}, {2U, {84U, 76U, 0U}}, {3U, {84U, 76U, 69U}},
    {1U, {88U, 0U, 0U}}};

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
    const tune_notes_t *tune = &TUNES[name];
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

/* The LED you can see is an RGB_* one; an unknown name falls back to
 * LED 0, and a board that refuses it has no LED here. */
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

static bento_status_t show_led(const board_led_t *led, int32_t level,
                               int32_t tick)
{
    bento_status_t status = BENTO_OK;

    if (led->present) {
        if ((level == 0) || ((level == 1) && ((tick % 2) == 0))) {
            status = bento_gpio_led_off(led->index);
        } else {
            status = bento_gpio_led_on(led->index);
        }
    }
    return status;
}

/* The crop's 8x8 face in the middle of the RGB matrix ('#' = lit). */
static const char *const FACES[LEVELS][FACE_ROWS] = {
    {"..####..", ".#....#.", "#.#..#.#", "#......#", "#.#..#.#",
     "#..##..#", ".#....#.", "..####.."},
    {"..####..", ".#....#.", "#.#..#.#", "#......#", "#......#",
     "#.####.#", ".#....#.", "..####.."},
    {"..####..", ".#....#.", "#.#..#.#", "#......#", "#..##..#",
     "#.#..#.#", ".#....#.", "..####.."}};
static const uint8_t FACE_COLORS[LEVELS] = {
    (uint8_t)BENTO_RGBMATRIX_GREEN, (uint8_t)BENTO_RGBMATRIX_YELLOW,
    (uint8_t)BENTO_RGBMATRIX_RED};

/* Two pixels per byte: an even x in the low nibble, an odd x in the
 * high one. */
static void draw_face_row(uint8_t *frame, int32_t y, const char *row,
                          uint8_t color)
{
    int32_t i;

    for (i = 0; row[i] != '\0'; i++) {
        if (row[i] == '#') {
            const int32_t x = FACE_LEFT_X + i;
            const uint32_t at =
                (uint32_t)((y * MATRIX_ROW_BYTES) + (x / 2));

            frame[at] |= ((x % 2) != 0)
                             ? (uint8_t)(color << (uint8_t)NIBBLE_BITS)
                             : color;
        }
    }
}

static void draw_face(int32_t level)
{
    uint8_t frame[BENTO_RGBMATRIX_FRAME_LEN];
    int32_t y;

    (void)memset(frame, 0, sizeof(frame));
    for (y = 0; y < FACE_ROWS; y++) {
        draw_face_row(frame, y, FACES[level][y], FACE_COLORS[level]);
    }
    /* the .py ignores an OSError of the matrix */
    (void)bento_rgbmatrix_blit(frame, BENTO_RGBMATRIX_FRAME_LEN);
}

/* ---- 3) สมอง (ตัดสินใจ) ---- */
static uint32_t find_crop(const char *name)
{
    const uint32_t count = (uint32_t)(sizeof(CROPS) / sizeof(CROPS[0]));
    uint32_t found = count;
    uint32_t i;

    for (i = 0U; (i < count) && (found == count); i++) {
        if (strcmp(CROPS[i].name, name) == 0) {
            found = i;
        }
    }
    return (found == count) ? 0U : found;
}

/* Joins the next problem to the reason with " + ". */
static void add_problem(char *why, uint32_t cap, uint32_t *problems,
                        const char *problem)
{
    const size_t used = strlen(why);

    (void)snprintf(&why[used], cap - used, "%s%s",
                   (*problems > 0U) ? " + " : "", problem);
    (*problems)++;
}

/* The level (0 = comfortable, 1 = getting stressed, 2 = bad) and
 * the reason, written into why. */
static int32_t judge(bento_real_t t, bento_real_t h, const crop_t *crop,
                     char *why, uint32_t cap)
{
    uint32_t problems = 0U;
    bool far;
    int32_t level = 0;

    why[0] = '\0';
    if (t < (bento_real_t)crop->t_lo) {
        add_problem(why, cap, &problems, "หนาวไป");
    }
    if (t > (bento_real_t)crop->t_hi) {
        add_problem(why, cap, &problems, "ร้อนไป");
    }
    if (h < (bento_real_t)crop->h_lo) {
        add_problem(why, cap, &problems, "แห้งไป");
    }
    if (h > (bento_real_t)crop->h_hi) {
        add_problem(why, cap, &problems, "ชื้นไป");
    }
    if (problems == 0U) {
        (void)snprintf(why, cap, "อยู่ในช่วงที่ชอบ");
    } else {
        far = (t < (bento_real_t)(crop->t_lo - COLD_MARGIN_C)) ||
              (t > (bento_real_t)(crop->t_hi + COLD_MARGIN_C)) ||
              (h < (bento_real_t)(crop->h_lo - DRY_MARGIN_RH)) ||
              (h > (bento_real_t)(crop->h_hi + DRY_MARGIN_RH));
        level = far ? 2 : 1;
    }
    return level;
}

/* A score 0-100: the share of readings that were comfortable. */
static int32_t comfort_score(int32_t good, int32_t total)
{
    return (good * PERCENT_MAX) / ((total > 1) ? total : 1);
}

static uint32_t score_color(int32_t score)
{
    uint32_t color = COL_BAD;

    if (score >= SCORE_GOOD) {
        color = COL_OK;
    } else if (score >= SCORE_FAIR) {
        color = COL_WARN;
    } else {
        /* below fair: red */
    }
    return color;
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
    bento_ui_id_t crop;
    bento_ui_id_t roller;
    bento_ui_id_t lbl_t;
    bento_ui_id_t lbl_h;
    bento_ui_id_t mood;
    bento_ui_id_t why;
    bento_ui_id_t arc;
    bento_ui_id_t score;
    bento_ui_id_t leds[LEVELS];
    bento_ui_id_t chart;
    uint8_t s_temp;
    uint8_t s_lo;
    uint8_t s_hi;
    bento_ui_id_t status;
} comfort_screen_t;

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

static void crop_text(char *out, uint32_t cap, const crop_t *crop)
{
    (void)snprintf(out, cap,
                   "พืช: %s   ชอบ %" PRId32 "-%" PRId32 " C, %" PRId32
                   "-%" PRId32 " %%RH",
                   crop->name, crop->t_lo, crop->t_hi, crop->h_lo,
                   crop->h_hi);
}

/* A Roller's value= at creation is its font size; the row is chosen
 * with a value set afterwards. */
static bento_status_t build_picker(comfort_screen_t *screen,
                                   uint32_t crop)
{
    static const widget_spec_t box = {bento_ui_panel_create,
                                      NULL,
                                      12,
                                      64,
                                      196,
                                      276,
                                      COL_CARD,
                                      COL_DIM,
                                      12,
                                      1};
    static const widget_spec_t roller = {bento_ui_roller_create,
                                         NULL,
                                         24,
                                         98,
                                         172,
                                         232,
                                         COL_TEXT,
                                         0,
                                         0,
                                         0};
    const uint32_t count = (uint32_t)(sizeof(CROPS) / sizeof(CROPS[0]));
    uint32_t i;
    bento_status_t status = add_card(screen->ui, &box, "เลือกพืช (ปัด)");

    if (status == BENTO_OK) {
        status = add_widget(screen->ui, &roller, &screen->roller);
    }
    for (i = 0U; (i < count) && (status == BENTO_OK); i++) {
        status = bento_ui_add_option(screen->ui, screen->roller,
                                     CROPS[i].name,
                                     (uint32_t)strlen(CROPS[i].name));
    }
    if (status == BENTO_OK) {
        /* a widget setter's refusal was ignored in MicroPython too */
        (void)bento_ui_set_value(screen->ui, screen->roller,
                                 (int32_t)crop);
    }
    return status;
}

static bento_status_t build_now_card(comfort_screen_t *screen)
{
    static const widget_spec_t box = {bento_ui_panel_create,
                                      NULL,
                                      218,
                                      64,
                                      300,
                                      190,
                                      COL_CARD,
                                      COL_DIM,
                                      12,
                                      1};
    static const widget_spec_t specs[] = {
        {bento_ui_label_create, "อุณหภูมิ -- C", 230, 96, 0, 0, COL_TEXT,
         0, 0, 24},
        {bento_ui_label_create, "ความชื้น -- %", 230, 128, 0, 0, COL_TEXT,
         0, 0, 24},
        {bento_ui_label_create, "...", 230, 164, 0, 0, COL_WARN, 0, 0,
         28},
        {bento_ui_label_create, "", 230, 206, 0, 0, COL_DIM, 0, 0, 20}};
    bento_ui_id_t *const outs[] = {&screen->lbl_t, &screen->lbl_h,
                                   &screen->mood, &screen->why};
    bento_status_t status = add_card(screen->ui, &box, "ตอนนี้");

    if (status == BENTO_OK) {
        status =
            add_widgets(screen->ui, specs, outs,
                        (uint32_t)(sizeof(specs) / sizeof(specs[0])));
    }
    return status;
}

/* The 3 status lamps, one lit at a time. */
static bento_status_t add_level_leds(comfort_screen_t *screen)
{
    static const char *const names[LEVELS] = {"สบาย", "เครียด", "แย่"};
    static const widget_spec_t led = {
        bento_ui_led_create, NULL, 540, 202, 20, 20, 0, 0, 0, 0};
    static const widget_spec_t label = {
        bento_ui_label_create, NULL, 566, 202, 0, 0, COL_DIM, 0, 0, 20};
    const int16_t step_x = 84;
    widget_spec_t place;
    bento_ui_id_t widget;
    int32_t i;
    bento_status_t status = BENTO_OK;

    for (i = 0; (i < LEVELS) && (status == BENTO_OK); i++) {
        place = led;
        place.x = (int16_t)(led.x + (step_x * i));
        place.color = LEVEL_COLORS[i];
        status = add_widget(screen->ui, &place, &screen->leds[i]);
        if (status == BENTO_OK) {
            place = label;
            place.x = (int16_t)(label.x + (step_x * i));
            status =
                add_text_widget(screen->ui, &place, names[i], &widget);
        }
    }
    return status;
}

static bento_status_t build_score_card(comfort_screen_t *screen)
{
    static const widget_spec_t box = {bento_ui_panel_create,
                                      NULL,
                                      528,
                                      64,
                                      252,
                                      190,
                                      COL_CARD,
                                      COL_DIM,
                                      12,
                                      1};
    static const widget_spec_t arc = {
        bento_ui_arc_create, NULL, 540, 94, 104, 104, 0, 0, 100, 100};
    static const widget_spec_t score = {bento_ui_label_create,
                                        "100",
                                        660,
                                        118,
                                        0,
                                        0,
                                        COL_TEXT,
                                        0,
                                        0,
                                        28};
    static const widget_spec_t note = {bento_ui_label_create,
                                       "100 = สบายทุกครั้งที่อ่าน",
                                       540,
                                       228,
                                       0,
                                       0,
                                       COL_DIM,
                                       0,
                                       0,
                                       20};
    bento_ui_id_t widget;
    bento_status_t status = add_card(screen->ui, &box, "คะแนนความสบาย");

    if (status == BENTO_OK) {
        status = add_widget(screen->ui, &arc, &screen->arc);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_color(screen->ui, screen->arc, COL_OK);
        status = add_widget(screen->ui, &score, &screen->score);
    }
    if (status == BENTO_OK) {
        status = add_level_leds(screen);
    }
    if (status == BENTO_OK) {
        status = add_widget(screen->ui, &note, &widget);
    }
    return status;
}

/* The chart: the orange line is the temperature, the green ones the
 * edges of the range the crop likes. */
static bento_status_t build_chart(comfort_screen_t *screen)
{
    static const widget_spec_t chart = {
        bento_ui_chart_create, NULL, 218, 262, 380, 76, COL_WARN, 0,
        CHART_MAX_C,           0};
    static const widget_spec_t legends[] = {
        {bento_ui_label_create, "เส้นส้ม = อุณหภูมิ", 608, 266, 0, 0,
         COL_WARN, 0, 0, 20},
        {bento_ui_label_create, "เส้นเขียว = ช่วงที่ชอบ", 608, 294, 0, 0,
         COL_OK, 0, 0, 20}};
    bento_ui_id_t *const outs[] = {NULL, NULL};
    bento_status_t status =
        add_widget(screen->ui, &chart, &screen->chart);

    if (status == BENTO_OK) {
        status =
            bento_ui_set_prop(screen->ui, screen->chart,
                              BENTO_UI_PROP_CHART_POINTS, CHART_POINTS);
    }
    if (status == BENTO_OK) {
        screen->s_temp = 0U;
        status = bento_ui_chart_add_series(screen->ui, screen->chart,
                                           COL_OK, &screen->s_lo);
    }
    if (status == BENTO_OK) {
        status = bento_ui_chart_add_series(screen->ui, screen->chart,
                                           COL_OK, &screen->s_hi);
    }
    if (status == BENTO_OK) {
        status = add_widgets(screen->ui, legends, outs, 2U);
    }
    return status;
}

static bento_status_t build_heading(comfort_screen_t *screen,
                                    uint32_t crop)
{
    static const widget_spec_t specs[] = {
        {bento_ui_label_create, "พืชของเราสบายดีไหม", 12, 6, 0, 0,
         COL_TEXT, 0, 0, 24},
        {bento_ui_label_create, NULL, 12, 38, 0, 0, COL_INFO, 0, 0,
         20}};
    char text[TEXT_CHARS];
    bento_ui_id_t widget;
    bento_status_t status = add_widget(screen->ui, &specs[0], &widget);

    if (status == BENTO_OK) {
        crop_text(text, sizeof(text), &CROPS[crop]);
        status =
            add_text_widget(screen->ui, &specs[1], text, &screen->crop);
    }
    return status;
}

static bento_status_t build_screen(comfort_screen_t *screen,
                                   uint32_t crop)
{
    static const widget_spec_t line = {bento_ui_label_create,
                                       "ปัดวงล้อด้านซ้ายเพื่อเปลี่ยนพืช",
                                       12,
                                       352,
                                       0,
                                       0,
                                       COL_DIM,
                                       0,
                                       0,
                                       20};
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = sleep_ms(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = build_heading(screen, crop);
    }
    if (status == BENTO_OK) {
        status = build_picker(screen, crop);
    }
    if (status == BENTO_OK) {
        status = build_now_card(screen);
    }
    if (status == BENTO_OK) {
        status = build_score_card(screen);
    }
    if (status == BENTO_OK) {
        status = build_chart(screen);
    }
    if (status == BENTO_OK) {
        status = add_widget(screen->ui, &line, &screen->status);
    }
    if (status == BENTO_OK) {
        refresh(screen->ui);
    }
    return status;
}

/* A new crop swiped on the roller (it sends value_changed), else the
 * crop it had. A failed poll answers no events. */
static uint32_t picked_crop(const comfort_screen_t *screen,
                            uint32_t crop)
{
    const int32_t count = (int32_t)(sizeof(CROPS) / sizeof(CROPS[0]));
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t received = 0U;
    uint32_t picked = crop;
    uint32_t i;

    if (bento_ui_poll(screen->ui, events, BENTO_UI_POLL_EVENTS_MAX,
                      &received, NULL) != BENTO_OK) {
        received = 0U;
    }
    for (i = 0U; i < received; i++) {
        if ((events[i].id == screen->roller) &&
            (events[i].type == (uint8_t)BENTO_UI_EVENT_VALUE_CHANGED) &&
            (events[i].value >= 0) && (events[i].value < count)) {
            picked = (uint32_t)events[i].value;
        }
    }
    return picked;
}

static void show_reading(const comfort_screen_t *screen,
                         bento_ui_id_t label, const char *name,
                         bento_real_t value, const char *unit)
{
    char digits[BENTO_FMT_BUF_SIZE];
    char text[TEXT_CHARS];
    uint32_t len = 0U;

    if (bento_fmt_real_precision(digits, sizeof(digits), value, 1,
                                 &len) != BENTO_OK) {
        digits[0] = '\0';
    }
    (void)snprintf(text, sizeof(text), "%s %s %s", name, digits, unit);
    set_text(screen->ui, label, text);
}

/* What one round shows. */
typedef struct {
    const climate_t *climate;
    int32_t level;
    const char *why;
    int32_t score;
    const crop_t *crop;
} comfort_view_t;

static void show(const comfort_screen_t *screen,
                 const comfort_view_t *view)
{
    const bento_real_t t = view->climate->t;
    char text[TEXT_CHARS];
    int32_t i;

    show_reading(screen, screen->lbl_t, "อุณหภูมิ", t, "C");
    show_reading(screen, screen->lbl_h, "ความชื้น", view->climate->h,
                 "%");
    (void)bento_ui_set_color(screen->ui, screen->mood,
                             LEVEL_COLORS[view->level]);
    set_text(screen->ui, screen->mood, MOODS[view->level]);
    set_text(screen->ui, screen->why, view->why);
    (void)bento_ui_set_value(screen->ui, screen->arc, view->score);
    (void)bento_ui_set_color(screen->ui, screen->arc,
                             score_color(view->score));
    (void)snprintf(text, sizeof(text), "%" PRId32, view->score);
    set_text(screen->ui, screen->score, text);
    for (i = 0; i < LEVELS; i++) {
        (void)bento_ui_set_value(screen->ui, screen->leds[i],
                                 (i == view->level) ? 1 : 0);
    }
    (void)bento_ui_chart_set_next(
        screen->ui, screen->chart, screen->s_temp,
        (t < (bento_real_t)0)
            ? 0
            : ((t > (bento_real_t)CHART_MAX_C) ? CHART_MAX_C
                                               : (int32_t)t));
    (void)bento_ui_chart_set_next(screen->ui, screen->chart,
                                  screen->s_lo, view->crop->t_lo);
    (void)bento_ui_chart_set_next(screen->ui, screen->chart,
                                  screen->s_hi, view->crop->t_hi);
}

static void show_sensor_error(const comfort_screen_t *screen)
{
    (void)bento_ui_set_color(screen->ui, screen->mood, COL_BAD);
    set_text(screen->ui, screen->mood, "อ่านเซนเซอร์ไม่ได้");
}

/* ---- 5) โปรแกรมหลัก ---- */
typedef struct {
    uint32_t crop;
    int32_t good;
    int32_t total;
    bool level_known;
    int32_t last_level;
    uint32_t last_sound_ms;
    board_led_t led;
} comfort_t;

static bento_status_t finish(const comfort_screen_t *screen,
                             const comfort_t *comfort)
{
    bento_status_t status = BENTO_OK;

    if (comfort->led.present) {
        status = bento_gpio_led_off(comfort->led.index);
    }
    if (status == BENTO_OK) {
        /* the .py ignores an OSError of the matrix */
        (void)bento_rgbmatrix_clear();
        (void)bento_ui_set_color(screen->ui, screen->status, COL_WARN);
        set_text(screen->ui, screen->status,
                 "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่");
        refresh(screen->ui);
        (void)printf("คะแนนความสบายของ %s = %" PRId32 " จาก %" PRId32
                     " ครั้ง\n",
                     CROPS[comfort->crop].name,
                     comfort_score(comfort->good, comfort->total),
                     comfort->total);
    }
    return status;
}

/* Only when the level changes: the face, and a sound no more often
 * than SOUND_GAP_MS. */
static bento_status_t act_on_level(const comfort_screen_t *screen,
                                   comfort_t *comfort, int32_t level)
{
    bool quiet;
    bento_status_t status = BENTO_OK;

    if (!comfort->level_known || (level != comfort->last_level)) {
        draw_face(level);
        quiet = bento_time_ticks_diff(bento_time_ticks_ms(),
                                      comfort->last_sound_ms) >
                SOUND_GAP_MS;
        if (comfort->level_known && quiet) {
            status = beep(screen->ui, (level > comfort->last_level)
                                          ? TUNE_BAD
                                          : TUNE_GOOD);
            comfort->last_sound_ms = bento_time_ticks_ms();
        }
        comfort->level_known = true;
        comfort->last_level = level;
    }
    return status;
}

static bento_status_t change_crop(const comfort_screen_t *screen,
                                  comfort_t *comfort)
{
    const uint32_t picked = picked_crop(screen, comfort->crop);
    char text[TEXT_CHARS];
    bento_status_t status = BENTO_OK;

    if (picked != comfort->crop) {
        comfort->crop = picked;
        comfort->good = 0;
        comfort->total = 0;
        crop_text(text, sizeof(text), &CROPS[picked]);
        set_text(screen->ui, screen->crop, text);
        status = beep(screen->ui, TUNE_TAP);
    }
    return status;
}

/* Judge, score, show, act. */
static bento_status_t judge_and_act(const comfort_screen_t *screen,
                                    comfort_t *comfort,
                                    const climate_t *climate)
{
    char why[TEXT_CHARS];
    comfort_view_t view;
    bento_status_t status;

    view.climate = climate;
    view.crop = &CROPS[comfort->crop];
    view.level =
        judge(climate->t, climate->h, view.crop, why, sizeof(why));
    view.why = why;
    comfort->total++;
    if (view.level == 0) {
        comfort->good++;
    }
    view.score = comfort_score(comfort->good, comfort->total);
    show(screen, &view);
    status = act_on_level(screen, comfort, view.level);
    if (status == BENTO_OK) {
        status = show_led(&comfort->led, view.level, comfort->total);
    }
    return status;
}

static bento_status_t run_loop(const comfort_screen_t *screen,
                               comfort_t *comfort)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    climate_t climate;
    bento_status_t status = BENTO_OK;

    comfort->last_sound_ms = start_ms;
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = change_crop(screen, comfort);
        if (status == BENTO_OK) {
            status = read_climate(&climate);
        }
        if ((status == BENTO_OK) && !climate.known) {
            show_sensor_error(screen);
        } else if (status == BENTO_OK) {
            status = judge_and_act(screen, comfort, &climate);
        } else {
            /* a failed or stopped step ends the loop */
        }
        if (status == BENTO_OK) {
            status = sleep_ms(TICK_MS);
        }
    }
    return status;
}

int main(void)
{
    comfort_screen_t screen;
    comfort_t comfort;
    bento_status_t status;

    (void)memset(&screen, 0, sizeof(screen));
    (void)memset(&comfort, 0, sizeof(comfort));
    screen.ui = bento_runtime_get_ui();
    /* a build without a speaker volume refuses it, as hasattr() skips
     * it */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    comfort.crop = find_crop(START_CROP);
    if (strcmp(CROPS[comfort.crop].name, START_CROP) != 0) {
        (void)printf("ไม่รู้จักพืช %s - ใช้ %s แทน (สะกดให้ตรงกับใน "
                     "CROPS)\n",
                     START_CROP, CROPS[comfort.crop].name);
    }
    status = build_screen(&screen, comfort.crop);
    if (status == BENTO_OK) {
        led_named("RGB_RED", &comfort.led);
        status = run_loop(&screen, &comfort);
    }
    if (status == BENTO_OK) {
        status = finish(&screen, &comfort);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
