/*
 * cp3_01_status_language.c -- หลักการ 3.1: สถานะหนึ่ง ภาษาเดียว
 * (One status, one language: colour, pattern and sound)
 *
 * หมุน VR1 เลือกสถานะ 0-3 (ปกติ เฝ้าดู เตือน อันตราย) ทุกช่องทางเปลี่ยน
 * พร้อมกัน: ไฟ RGB บนบอร์ด จอไฟ 16x8 จอภาพ และลำโพง ยิ่งรุนแรงยิ่งเร่ง
 * ติดนิ่ง กะพริบช้า กะพริบเร็ว เสียงดังเฉพาะตอนแย่ลงและตอนกลับมาปกติ
 * "อันตราย" ดังซ้ำไม่ถี่กว่า REPEAT_S วินาที กด SW5 (ปุ่มล่าง) = รับทราบ
 * เสียงเงียบแต่ไฟยังบอกสถานะ เล่น 3 นาทีแล้วจบเอง
 *
 * Turn VR1 to pick a status from 0 to 3 (normal, watch, warning,
 * danger). Every channel changes at once: the board's RGB light, the
 * 16x8 light matrix, the screen and the speaker. The worse the status,
 * the faster the pattern: steady, slow blink, fast blink. A sound plays
 * only when things get worse or come back to normal. "Danger" repeats
 * no more often than REPEAT_S seconds until SW5 (the lower button)
 * acknowledges it; the lights keep telling the status. The program
 * ends by itself after three minutes.
 *
 * Twin of cp3_01_status_language.py
 * (aiot-development-for-smart-farm/core/cp3_01_status_language.py).
 * Style: CLEAN_C.md.
 * Differs from the original:
 *   - ui.volume has no hasattr test: a board without it answers
 *     BENTO_E_UNSUPPORTED, which is skipped as the .py skips the line.
 *   - Every light matrix status is ignored, as the .py's try/except
 *     OSError ignores a failed draw.
 *   - An RGB light whose off() fails is left out, as the .py keeps
 *     None for it; a failed value() later ends the program as its
 *     exception does.
 *   - The tune table keeps only the tunes this program plays.
 *   - Stop ends the program at once, without the closing screen, as
 *     KeyboardInterrupt does in the .py.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_buttons.h"
#include "bento_gpio.h"
#include "bento_pots.h"
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_time.h"

/* Settings a student may change. */
enum {
    REPEAT_S = 10,   /* "danger" repeats no more often than this */
    HYST = 3,        /* % kept around a band edge against flutter */
    SAMPLE_MS = 20,  /* read the button and the knob this often */
    TICK_MS = 1000,  /* refresh the countdown this often */
    RUN_MS = 180000, /* the program ends after three minutes */
    SPEAKER = 40,    /* speaker volume, 0-100 % */
    VOLUME = 25,     /* tone velocity, 0-127 (about 20 %) */
    MS_PER_S = 1000,
    STATE_BAND = 25, /* VR1 % per status */
    STATE_COUNT = 4,
    STATE_WARNING = 2, /* from here SW5 acknowledges */
    STATE_DANGER = 3,
    STATE_NONE = -1, /* before the first status, and lights out */
    PERCENT = 100,
    ADC_FULL = 4095, /* pots.read() at the right end */
    VR1 = 0,
    SW5 = 1,        /* the lower button is buttons.pressed(1) */
    SCROLL_MS = 80, /* light matrix scroll speed */
    LIGHT_COUNT = 3 /* RGB_RED, RGB_GREEN, RGB_BLUE */
};

/* Sound. */
enum {
    NOTE_MS = 90,
    NOTE_GAP_MS = 100,
    TUNE_NOTES_MAX = 3
};

/* The tunes this program plays; TUNE_NONE plays nothing. */
typedef enum {
    TUNE_NONE = 0,
    TUNE_TAP,
    TUNE_GOOD,
    TUNE_BAD
} tune_t;

/* Screen layout. */
enum {
    MARGIN_X = 12,
    TITLE_Y = 6,
    HINT_Y = 38,
    CARD_Y = 64,
    CARD_H = 274,
    NOW_W = 300,
    TABLE_X = 322,
    TABLE_W = 458,
    CARD_TEXT_X = 12,
    CARD_TEXT_Y = 6,
    CARD_RADIUS = 12,
    LED_X = 28,
    LED_Y = 96,
    LED_SIZE = 80,
    NAME_X = 124,
    NAME_Y = 116,
    MESSAGE_X = 28,
    MESSAGE_Y = 204,
    ROW_X = 338,
    ROW_Y = 98,
    HOW_Y = 126,
    ROW_STEP = 58,
    HELP_Y = 352,
    FONT_TEXT = 20, /* Thai is 20 px or larger on the board */
    FONT_TITLE = 24,
    FONT_NAME = 28,
    SETTLE_MS = 200, /* let the cleared screen settle */
    TEXT_BYTES = 64  /* "ดังซ้ำอีก %d วิ" with any int32_t */
};

/* Colours, 0xRRGGBB. */
enum {
    COL_TEXT = 0xE8EAED,
    COL_DIM = 0x9AA3AF,
    COL_CARD = 0x171B22,
    COL_OK = 0x30A46C,
    COL_WARN = 0xF5A623,
    COL_BAD = 0xE5484D,
    COL_INFO = 0x4A9EFF
};

/* The status language: one row per status, read by several
 * functions; a row only one function reads lives in it. */
static const char *const NAMES[STATE_COUNT] = {"ปกติ", "เฝ้าดู", "เตือน",
                                               "อันตราย"};
/* "" fills the light matrix; a word scrolls (Latin letters and
 * digits only) */
static const char *const WORDS[STATE_COUNT] = {"", "", "WARN", "ALARM"};
static const uint32_t COLS[STATE_COUNT] = {COL_OK, COL_INFO, COL_WARN,
                                           COL_BAD};
/* Which of RGB_RED, RGB_GREEN, RGB_BLUE light per status: red and
 * green together make yellow. */
static const bool LIT[STATE_COUNT][LIGHT_COUNT] = {
    {false, true, false},
    {false, false, true},
    {true, true, false},
    {true, false, false}};

typedef struct {
    int16_t x;
    int16_t y;
} point_t;

typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
} rect_t;

/* The board LEDs by name; a light that is not there stays out. */
typedef struct {
    uint32_t led[LIGHT_COUNT];
    bool present[LIGHT_COUNT];
} lights_t;

/* The widgets the program changes after building the screen. */
typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t led;
    bento_ui_id_t name;
    bento_ui_id_t message;
    bento_ui_id_t help;
} status_screen_t;

typedef struct {
    status_screen_t screen;
    lights_t lights;
    int32_t state;
    bool acked;
    bool lit;
    bool was_down;
    uint32_t show_ms;
    uint32_t blink_ms;
    uint32_t repeat_ms;
} alarm_t;

/* The one place a layout number narrows to the API's int16_t. */
static int16_t px(int32_t value)
{
    return (int16_t)value;
}

static bento_status_t check_running(bento_status_t status)
{
    bento_status_t result = status;

    if ((result == BENTO_OK) && !bento_runtime_is_running()) {
        result = BENTO_E_CANCELLED;
    }
    return result;
}

static bento_status_t sleep_ms(uint32_t ms)
{
    return check_running(bento_time_sleep_ms(ms));
}

/* A screen refresh whose events nobody reads. */
static void present(bento_ui_t *ui)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    /* ui.poll() answers no events when it fails, and this program
     * reads none */
    (void)bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                        NULL);
}

static void set_text(bento_ui_t *ui, bento_ui_id_t widget,
                     const char *text)
{
    /* a setter's refusal was ignored before too (modui.c) */
    (void)bento_ui_set_text(ui, widget, text, (uint32_t)strlen(text));
}

/* Plays a tune note by note, 100 ms apart. */
static bento_status_t beep(bento_ui_t *ui, tune_t tune)
{
    static const uint8_t notes[][TUNE_NOTES_MAX] = {
        {0U, 0U, 0U}, {76U, 0U, 0U}, {72U, 79U, 84U}, {84U, 76U, 0U}};
    static const uint32_t note_count[] = {0U, 1U, 3U, 2U};
    const uint32_t count = note_count[tune];
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < count) && (status == BENTO_OK); i++) {
        /* ui.tone() sends and waits for no answer (modui.c) */
        (void)bento_ui_sfx_tone(ui, notes[tune][i],
                                (uint8_t)BENTO_UI_WAVE_SINE,
                                (uint8_t)VOLUME, (uint16_t)NOTE_MS);
        status = sleep_ms((uint32_t)NOTE_GAP_MS);
    }
    return status;
}

/* Finds an LED by its exact name, else LED 0: on the Dev Kit LED1
 * and LED2 sit on the SoM, out of sight. */
static bool find_light(const char *wanted, uint32_t *led)
{
    const uint32_t count = bento_gpio_num_leds();
    const char *name = NULL;
    uint32_t i;

    *led = 0U;
    for (i = count; i > 0U; i--) {
        if ((bento_gpio_led_name(i - 1U, &name) == BENTO_OK) &&
            (name != NULL) && (strcmp(name, wanted) == 0)) {
            *led = i - 1U;
        }
    }
    return (*led < count) && (bento_gpio_led_off(*led) == BENTO_OK);
}

static void find_lights(lights_t *lights)
{
    static const char *const light_names[LIGHT_COUNT] = {
        "RGB_RED", "RGB_GREEN", "RGB_BLUE"};
    uint32_t i;

    for (i = 0U; i < (uint32_t)LIGHT_COUNT; i++) {
        lights->present[i] =
            find_light(light_names[i], &lights->led[i]);
    }
}

/* The lights of the set on, every other light off; lit false is the
 * dark half of a blink. */
static bento_status_t show_light(const lights_t *lights,
                                 const bool set[LIGHT_COUNT], bool lit)
{
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < (uint32_t)LIGHT_COUNT) && (status == BENTO_OK);
         i++) {
        if (lights->present[i]) {
            status = bento_gpio_led_set_value(
                lights->led[i], (lit && set[i]) ? 1U : 0U);
        }
    }
    return status;
}

/* Written only when the status changes; every draw stops a scroll
 * still running. STATE_NONE clears (fill() takes colours 1-7). */
static void show_matrix(int32_t state)
{
    static const int32_t matrix_colors[STATE_COUNT] = {
        BENTO_RGBMATRIX_GREEN, BENTO_RGBMATRIX_BLUE,
        BENTO_RGBMATRIX_YELLOW, BENTO_RGBMATRIX_RED};

    /* the light matrix is on the display core's bus; a draw that
     * does not get through is skipped, as the .py's except OSError
     * does */
    if (state < 0) {
        (void)bento_rgbmatrix_clear();
    } else if (WORDS[state][0] != '\0') {
        (void)bento_rgbmatrix_scroll(WORDS[state],
                                     (uint32_t)strlen(WORDS[state]),
                                     matrix_colors[state], SCROLL_MS);
    } else {
        (void)bento_rgbmatrix_fill(matrix_colors[state]);
    }
}

/* VR1 0-100 % to a status, 25 % per status; inside the old band
 * (widened by HYST) the status stays. */
static int32_t pick_state(int32_t percent, int32_t now)
{
    int32_t state = percent / STATE_BAND;

    if ((((now * STATE_BAND) - HYST) <= percent) &&
        (percent < (((now + 1) * STATE_BAND) + HYST))) {
        state = now;
    } else if (state > STATE_DANGER) {
        state = STATE_DANGER;
    } else {
        /* the band the knob is in */
    }
    return state;
}

/* A sound only when things get worse or come back to normal;
 * better but not yet normal stays quiet. */
static tune_t sound_for(int32_t old, int32_t next)
{
    tune_t tune = TUNE_NONE;

    if (next == 0) {
        tune = (old != 0) ? TUNE_GOOD : TUNE_NONE;
    } else if (next > old) {
        tune = (next == 1) ? TUNE_TAP : TUNE_BAD;
    } else {
        /* better but not normal: quiet */
    }
    return tune;
}

static bento_status_t read_percent(int32_t *percent)
{
    uint16_t raw = 0U;
    const bento_status_t status = bento_pots_read(VR1, &raw);

    *percent = ((int32_t)raw * PERCENT) / ADC_FULL;
    return status;
}

static bento_status_t add_label(bento_ui_t *ui, const char *text,
                                point_t at, uint32_t color,
                                int32_t font_px, bento_ui_id_t *out)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.text = text;
    args.text_len = (uint32_t)strlen(text);
    args.x = at.x;
    args.y = at.y;
    args.color = color;
    args.font_px = font_px;
    return bento_ui_label_create(ui, &args, out);
}

/* A card: a dark box with a grey edge and a blue title. */
static bento_status_t add_card(bento_ui_t *ui, rect_t box,
                               const char *title)
{
    bento_ui_widget_args_t args;
    bento_ui_id_t unused;
    const point_t at = {px(box.x + CARD_TEXT_X),
                        px(box.y + CARD_TEXT_Y)};
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = box.x;
    args.y = box.y;
    args.w = box.w;
    args.h = box.h;
    args.color = COL_CARD;
    args.min_val = COL_DIM;
    args.max_val = CARD_RADIUS;
    args.font_px = 1;
    status = bento_ui_panel_create(ui, &args, &unused);
    if (status == BENTO_OK) {
        status = add_label(ui, title, at, COL_INFO, FONT_TEXT, &unused);
    }
    return status;
}

/* One row of the table: name and word in the status colour, then its
 * pattern. */
static bento_status_t add_row(bento_ui_t *ui, int32_t state)
{
    static const char *const how[STATE_COUNT] = {
        "เขียว ติดนิ่ง - เงียบ", "ฟ้า ติดนิ่ง - ติ๊ด", "เหลือง กะพริบช้า - เตือน",
        "แดง กะพริบเร็ว - เตือนซ้ำ"};
    char text[TEXT_BYTES];
    bento_ui_id_t unused;
    const point_t name_at = {ROW_X, px(ROW_Y + (state * ROW_STEP))};
    const point_t how_at = {ROW_X, px(HOW_Y + (state * ROW_STEP))};
    bento_status_t status;

    (void)snprintf(text, sizeof(text), "%s  %s", NAMES[state],
                   WORDS[state]);
    status =
        add_label(ui, text, name_at, COLS[state], FONT_TEXT, &unused);
    if (status == BENTO_OK) {
        status = add_label(ui, how[state], how_at, COL_DIM, FONT_TEXT,
                           &unused);
    }
    return status;
}

static bento_status_t add_now_card(status_screen_t *screen)
{
    const rect_t box = {MARGIN_X, CARD_Y, NOW_W, CARD_H};
    const point_t name_at = {NAME_X, NAME_Y};
    const point_t message_at = {MESSAGE_X, MESSAGE_Y};
    bento_ui_widget_args_t args;
    bento_status_t status = add_card(screen->ui, box, "ตอนนี้");

    bento_ui_widget_args_init(&args);
    args.x = LED_X;
    args.y = LED_Y;
    args.w = LED_SIZE;
    args.h = LED_SIZE;
    args.color = COL_OK;
    if (status == BENTO_OK) {
        status = bento_ui_led_create(screen->ui, &args, &screen->led);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", name_at, COL_TEXT,
                           FONT_NAME, &screen->name);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", message_at, COL_TEXT,
                           FONT_TEXT, &screen->message);
    }
    return status;
}

static bento_status_t add_table_card(bento_ui_t *ui)
{
    const rect_t box = {TABLE_X, CARD_Y, TABLE_W, CARD_H};
    bento_status_t status =
        add_card(ui, box, "สถานะ - สี - รูปแบบ - เสียง");
    int32_t state;

    for (state = 0; (state < STATE_COUNT) && (status == BENTO_OK);
         state++) {
        status = add_row(ui, state);
    }
    return status;
}

static bento_status_t build_screen(status_screen_t *screen)
{
    bento_ui_id_t unused;
    const point_t title_at = {MARGIN_X, TITLE_Y};
    const point_t hint_at = {MARGIN_X, HINT_Y};
    const point_t help_at = {MARGIN_X, HELP_Y};
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = sleep_ms((uint32_t)SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "สถานะหนึ่ง ภาษาเดียว", title_at,
                           COL_TEXT, FONT_TITLE, &unused);
    }
    if (status == BENTO_OK) {
        status =
            add_label(screen->ui, "หมุน VR1 เลือกสถานะ - SW5 = รับทราบ",
                      hint_at, COL_DIM, FONT_TEXT, &unused);
    }
    if (status == BENTO_OK) {
        status = add_now_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_table_card(screen->ui);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui,
                           "แย่ลง/กลับมาปกติ = ดัง - ดีขึ้นเฉย ๆ = เงียบ",
                           help_at, COL_DIM, FONT_TEXT, &screen->help);
    }
    if (status == BENTO_OK) {
        present(screen->ui);
    }
    return status;
}

static void show_state(const status_screen_t *screen, int32_t state)
{
    /* the widget setters ignore a refusal, as modui.c does */
    (void)bento_ui_set_color(screen->ui, screen->led, COLS[state]);
    (void)bento_ui_set_value(screen->ui, screen->led, 1);
    (void)bento_ui_set_color(screen->ui, screen->name, COLS[state]);
    set_text(screen->ui, screen->name, NAMES[state]);
    set_text(screen->ui, screen->message, " ");
}

/* A new status: every channel changes together. */
static bento_status_t change_state(alarm_t *alarm, int32_t next,
                                   uint32_t now)
{
    const int32_t old = (alarm->state < 0) ? 0 : alarm->state;
    const tune_t tune = sound_for(old, next);
    bento_status_t status;

    alarm->state = next;
    alarm->acked = false;
    alarm->lit = true;
    alarm->blink_ms = now;
    alarm->repeat_ms = now;
    status = show_light(&alarm->lights, LIT[next], true);
    if (status == BENTO_OK) {
        show_matrix(next);
        show_state(&alarm->screen, next);
        status = beep(alarm->screen.ui, tune);
    }
    return status;
}

/* The RGB light and the Led on screen blink together. */
static bento_status_t blink(alarm_t *alarm, uint32_t now)
{
    /* 0 = steady; else on/off every so many ms (500 or more, so the
     * screen keeps up) */
    static const int32_t blink_period_ms[STATE_COUNT] = {0, 0, 1000,
                                                         500};
    const int32_t period_ms = blink_period_ms[alarm->state];
    bento_status_t status = BENTO_OK;

    if ((period_ms != 0) &&
        (bento_time_ticks_diff(now, alarm->blink_ms) >= period_ms)) {
        alarm->lit = !alarm->lit;
        alarm->blink_ms = now;
        status =
            show_light(&alarm->lights, LIT[alarm->state], alarm->lit);
        (void)bento_ui_set_value(alarm->screen.ui, alarm->screen.led,
                                 alarm->lit ? 1 : 0);
    }
    return status;
}

/* A press edge of SW5 acknowledges a warning or a danger. SW5 is
 * debounced at every read, so it is read every SAMPLE_MS. */
static bento_status_t take_acknowledge(alarm_t *alarm)
{
    bool down = false;
    bento_status_t status = bento_buttons_pressed(SW5, &down);

    if ((status == BENTO_OK) && down && !alarm->was_down &&
        (alarm->state >= STATE_WARNING) && !alarm->acked) {
        alarm->acked = true;
        set_text(alarm->screen.ui, alarm->screen.message,
                 "รับทราบ: เงียบ ไฟยังอยู่");
        status = beep(alarm->screen.ui, TUNE_TAP);
    }
    alarm->was_down = down;
    return status;
}

static bool danger_unacked(const alarm_t *alarm)
{
    return (alarm->state == STATE_DANGER) && !alarm->acked;
}

/* Danger repeats, but not more often than REPEAT_S. */
static bento_status_t repeat_alarm(alarm_t *alarm, uint32_t now)
{
    bento_status_t status = BENTO_OK;

    if (danger_unacked(alarm) &&
        (bento_time_ticks_diff(now, alarm->repeat_ms) >=
         (REPEAT_S * MS_PER_S))) {
        alarm->repeat_ms = now;
        status = beep(alarm->screen.ui, TUNE_BAD);
    }
    return status;
}

/* The countdown on screen, once a second. */
static void tick_screen(alarm_t *alarm, uint32_t now)
{
    char text[TEXT_BYTES];

    if (bento_time_ticks_diff(now, alarm->show_ms) >= TICK_MS) {
        alarm->show_ms = now;
        if (danger_unacked(alarm)) {
            const int32_t left_s =
                REPEAT_S -
                (bento_time_ticks_diff(now, alarm->repeat_ms) /
                 MS_PER_S);

            (void)snprintf(text, sizeof(text), "ดังซ้ำอีก %" PRId32 " วิ",
                           left_s);
            set_text(alarm->screen.ui, alarm->screen.message, text);
        }
        present(alarm->screen.ui);
    }
}

static bento_status_t run_round(alarm_t *alarm)
{
    const uint32_t now = bento_time_ticks_ms();
    const int32_t old = (alarm->state < 0) ? 0 : alarm->state;
    int32_t percent = 0;
    bento_status_t status = read_percent(&percent);

    if (status == BENTO_OK) {
        const int32_t next = pick_state(percent, old);

        if (next != alarm->state) {
            status = change_state(alarm, next, now);
        }
    }
    if (status == BENTO_OK) {
        status = blink(alarm, now);
    }
    if (status == BENTO_OK) {
        status = take_acknowledge(alarm);
    }
    if (status == BENTO_OK) {
        status = repeat_alarm(alarm, now);
    }
    if (status == BENTO_OK) {
        tick_screen(alarm, now);
        status = sleep_ms((uint32_t)SAMPLE_MS);
    }
    return status;
}

static bento_status_t run_loop(alarm_t *alarm)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    alarm->show_ms = start_ms;
    alarm->blink_ms = start_ms;
    alarm->repeat_ms = start_ms;
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = run_round(alarm);
    }
    return status;
}

/* Lights out and the closing line. */
static bento_status_t finish(const alarm_t *alarm)
{
    static const bool all_dark[LIGHT_COUNT] = {false, false, false};
    const char *text = "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่";
    bento_status_t status = show_light(&alarm->lights, all_dark, false);

    if (status == BENTO_OK) {
        show_matrix(STATE_NONE);
        (void)bento_ui_set_color(alarm->screen.ui, alarm->screen.help,
                                 COL_WARN);
        set_text(alarm->screen.ui, alarm->screen.help, text);
        status = beep(alarm->screen.ui, TUNE_GOOD);
    }
    if (status == BENTO_OK) {
        present(alarm->screen.ui);
    }
    return status;
}

int main(void)
{
    alarm_t alarm;
    bento_status_t status;

    (void)memset(&alarm, 0, sizeof(alarm));
    alarm.screen.ui = bento_runtime_get_ui();
    alarm.state = STATE_NONE;
    alarm.lit = true;
    /* a board without ui.volume answers BENTO_E_UNSUPPORTED and is
     * skipped as the .py skips it; elsewhere it waits for no answer */
    (void)bento_ui_sfx_volume_set(alarm.screen.ui, SPEAKER);
    status = build_screen(&alarm.screen);
    if (status == BENTO_OK) {
        find_lights(&alarm.lights);
        status = run_loop(&alarm);
    }
    if (status == BENTO_OK) {
        status = finish(&alarm);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
