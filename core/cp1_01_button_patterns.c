/*
 * cp1_01_button_patterns.c -- หลักการ 1.1: ปุ่มหนึ่งปุ่ม อ่านได้สามแบบ
 * (ระดับ ขอบ รูปแบบ)
 *
 * "ระดับ" = ตอนนี้กดอยู่ไหม "ขอบ" = เพิ่งเปลี่ยนจากปล่อยเป็นกด "รูปแบบ" =
 * คลิก กดค้าง ดับเบิลคลิก ซึ่งต้องดูเวลาประกอบ เฟิร์มแวร์กรองสั่นของปุ่ม
 * (50 ms) ทุกครั้งที่อ่าน จึงต้องอ่านถี่ทุก SAMPLE_MS กด SW5 หรือ SW6
 * สั้น ๆ = คลิก กดค้างเกิน 0.8 วิ = ค้าง กดสองครั้งติดกันเร็ว ๆ = ดับเบิล
 * ดูไฟบนจอ (ระดับ) ตัวเลขใหญ่ (จำนวนขอบ) และแถวล่าง (รูปแบบ) เปลี่ยนตามกัน
 * โปรแกรมเล่น 3 นาทีแล้วจบเอง ใช้กับ TESAIoT Dev Kit และ BENTO Emulator
 *
 * One button read three ways: its level (is it down now), its edge (it
 * has just gone down) and its pattern (click, long press or double
 * click, which need the clock too). Tap SW5 or SW6 for a click, hold
 * it past 0.8 s for a long press, tap twice quickly for a double. The
 * screen LED shows the level, the big digits count the edges and the
 * bottom line names the pattern, with a tone for each. It runs for
 * three minutes.
 *
 * Twin of cp1_01_button_patterns.py (aiot-development-for-smart-farm/core/cp1_01_button_patterns.py). Style: CLEAN_C.md.
 * What C does differently:
 *   - the Pattern class is a struct and its feed() a function; None
 *     (no first click waiting) is a bool beside the time;
 *   - the TUNES, NAMES and SOUNDS dicts are small tables indexed by an
 *     enum;
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

#include "bento_buttons.h"
#include "bento_runtime.h"
#include "bento_time.h"

/* Settings a student may change. */
enum {
    LONG_MS = 800,   /* held this long or longer = a long press */
    DOUBLE_MS = 400, /* second release within this = a double */
    SAMPLE_MS = 10,  /* read the buttons this often (short taps) */
    TICK_MS = 500,   /* refresh the digits this often */
    RUN_MS = 180000, /* play for three minutes, then end */
    SPEAKER = 40,    /* speaker level 0-100 % (firmware 2.4.2+) */
    VOLUME = 25      /* tone loudness 0-127 (about 20 %) */
};

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
    CARD_Y = 66,
    CARD_W = 378,
    CARD_H = 272,
    CARD_LEFT_X = 12,
    CARD_RIGHT_X = 402,
    CARD_RADIUS = 12,
    CARD_BORDER = 1,
    CARD_TITLE_DX = 12,
    CARD_TITLE_DY = 6,
    INSET_X = 14,
    LED_Y = 100,
    LED_SIZE = 40,
    LEVEL_DX = 66,
    LEVEL_Y = 108,
    EDGE_CAPTION_Y = 154,
    SEG_Y = 178,
    SEG_W = 150,
    SEG_H = 56,
    COUNTS_Y = 246,
    LAST_Y = 280
};

/* Font sizes, in pixels. */
enum {
    FONT_TITLE = 24,
    FONT_TEXT = 16,
    FONT_LEVEL = 20,
    FONT_LAST = 28
};

/* Sound and timing that are not settings. */
enum {
    SETTLE_MS = 200,   /* let the cleared screen settle */
    TONE_MS = 90,      /* each note of a tune */
    NOTE_GAP_MS = 100, /* one note to the next */
    TUNE_NOTES = 3,    /* the longest tune */
    TEXT_BYTES = 128,  /* a SET_TEXT carries at most 126 bytes */
    BUTTON_COUNT = 2
};

/* The patterns a button can make; PATTERN_NONE is "nothing yet". */
enum {
    PATTERN_CLICK = 0,
    PATTERN_LONG = 1,
    PATTERN_DOUBLE = 2,
    PATTERN_KINDS = 3,
    PATTERN_NONE = 3
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

/* Names on the panel: SW5 = lower button = pressed(1), SW6 = upper
 * button = pressed(0). */
static const char *const BUTTON_NAMES[BUTTON_COUNT] = {"SW5", "SW6"};

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

/* One button's pattern finder: fed (is it down, now) every round. */
typedef struct {
    bool down;
    uint32_t down_ms;
    bool
        up_waiting; /* a first click waits to see if a second follows */
    uint32_t up_ms;
    bool held; /* already counted as long: the release is no click */
    int32_t edges;
} pattern_t;

/* The widgets of one button's card that change later. */
typedef struct {
    bento_ui_id_t led;
    bento_ui_id_t level;
    bento_ui_id_t seg;
    bento_ui_id_t counts;
    bento_ui_id_t last;
} button_card_t;

typedef struct {
    bento_ui_t *ui;
    button_card_t cards[BUTTON_COUNT];
    bento_ui_id_t help;
    pattern_t patterns[BUTTON_COUNT];
    int32_t counts[BUTTON_COUNT][PATTERN_KINDS];
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

/* The level of button i (0 = SW5, 1 = SW6): true = held down. The
 * firmware numbers them the other way round, so it reads 1 - i. */
static bento_status_t is_down(uint32_t i, bool *down)
{
    return bento_buttons_pressed((uint32_t)(1U - i), down);
}

/* Tones go through ui.tone because its loudness can be set. */
static bento_status_t beep(bento_ui_t *ui, int32_t tune)
{
    static const uint8_t notes[TUNE_KINDS][TUNE_NOTES] = {
        {76U},      {72U, 79U},      {79U, 72U}, {72U, 79U, 84U},
        {84U, 76U}, {84U, 76U, 69U}, {88U}};
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

/* A release after a short press: a double if a first click is still
 * waiting, else the start of the wait. */
static int32_t feed_release(pattern_t *pattern, uint32_t now_ms)
{
    int32_t found = PATTERN_NONE;

    if (pattern->up_waiting &&
        (bento_time_ticks_diff(now_ms, pattern->up_ms) <= DOUBLE_MS)) {
        pattern->up_waiting = false;
        found = PATTERN_DOUBLE;
    } else {
        pattern->up_waiting = true;
        pattern->up_ms = now_ms;
    }
    return found;
}

/* Feeds one reading; answers the pattern it completes, or
 * PATTERN_NONE. */
static int32_t feed(pattern_t *pattern, bool down, uint32_t now_ms)
{
    int32_t found = PATTERN_NONE;

    if (down && !pattern->down) {
        pattern->edges++;
        pattern->down_ms = now_ms;
        pattern->held = false;
    } else if (down && !pattern->held &&
               (bento_time_ticks_diff(now_ms, pattern->down_ms) >=
                LONG_MS)) {
        pattern->held = true;
        pattern->up_waiting = false;
        found = PATTERN_LONG;
    } else if (pattern->down && !down && !pattern->held) {
        found = feed_release(pattern, now_ms);
    } else if (!down && pattern->up_waiting &&
               (bento_time_ticks_diff(now_ms, pattern->up_ms) >
                DOUBLE_MS)) {
        pattern->up_waiting = false; /* the wait ran out: one click */
        found = PATTERN_CLICK;
    } else {
        /* nothing changed */
    }
    pattern->down = down;
    return found;
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

static bento_status_t add_led(bento_ui_t *ui, int32_t x,
                              bento_ui_id_t *out_id)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.x = px(x);
    args.y = LED_Y;
    args.w = LED_SIZE;
    args.h = LED_SIZE;
    args.color = COL_OK;
    args.font_px = 0;
    return bento_ui_led_create(ui, &args, out_id);
}

static bento_status_t add_seg(bento_ui_t *ui, int32_t x,
                              bento_ui_id_t *out_id)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.text = "0";
    args.text_len = 1U;
    args.x = px(x);
    args.y = SEG_Y;
    args.w = SEG_W;
    args.h = SEG_H;
    args.color = COL_INFO;
    return bento_ui_seg7_create(ui, &args, out_id);
}

/* The labels of one card, below its title and LED. */
static bento_status_t add_card_labels(bento_ui_t *ui, int32_t x,
                                      button_card_t *card)
{
    const int32_t inner_x = x + INSET_X;
    const label_spec_t level = {x + LEVEL_DX, LEVEL_Y, FONT_LEVEL,
                                COL_TEXT};
    const label_spec_t caption = {inner_x, EDGE_CAPTION_Y, FONT_TEXT,
                                  COL_DIM};
    const label_spec_t counts = {inner_x, COUNTS_Y, FONT_TEXT,
                                 COL_TEXT};
    const label_spec_t last = {inner_x, LAST_Y, FONT_LAST, COL_WARN};
    bento_ui_id_t unused;
    bento_status_t status =
        add_label(ui, "ระดับ: ปล่อย", &level, &card->level);

    if (status == BENTO_OK) {
        status = add_label(ui, "ขอบกดลง", &caption, &unused);
    }
    if (status == BENTO_OK) {
        status = add_seg(ui, inner_x, &card->seg);
    }
    if (status == BENTO_OK) {
        status = add_label(ui, "คลิก 0  ค้าง 0  ดับเบิล 0", &counts,
                           &card->counts);
    }
    if (status == BENTO_OK) {
        status = add_label(ui, " ", &last, &card->last);
    }
    return status;
}

static bento_status_t build_button_card(bento_ui_t *ui, uint32_t i,
                                        int32_t x, button_card_t *card)
{
    const rect_t box = {x, CARD_Y, CARD_W, CARD_H};
    char title[TEXT_BYTES];
    bento_status_t status;

    (void)snprintf(title, sizeof(title), "%s%s", BUTTON_NAMES[i],
                   (i == 0U) ? " (ปุ่มล่าง)" : " (ปุ่มบน)");
    status = add_card(ui, &box, title);
    if (status == BENTO_OK) {
        status = add_led(ui, x + INSET_X, &card->led);
    }
    if (status == BENTO_OK) {
        status = add_card_labels(ui, x, card);
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

static bento_status_t build_screen(app_t *app)
{
    const label_spec_t title = {MARGIN_X, TITLE_Y, FONT_TITLE,
                                COL_TEXT};
    const label_spec_t subtitle = {MARGIN_X, SUBTITLE_Y, FONT_TEXT,
                                   COL_DIM};
    const label_spec_t help = {MARGIN_X, HELP_Y, FONT_TEXT, COL_DIM};
    bento_ui_t *ui = app->ui;
    bento_ui_id_t unused;
    bento_status_t status =
        bento_ui_screen(ui, BENTO_RUNTIME_SCREEN_WIDTH_PX,
                        BENTO_RUNTIME_SCREEN_HEIGHT_PX);

    if (status == BENTO_OK) {
        status = pause_ms((uint32_t)SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_label(ui, "ปุ่มเดียว อ่านได้ 3 แบบ", &title, &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(ui,
                           "ระดับ -> ขอบ -> รูปแบบ  (กรองสั่น 50 ms "
                           "ในเฟิร์มแวร์)",
                           &subtitle, &unused);
    }
    if (status == BENTO_OK) {
        status = build_button_card(ui, 0U, CARD_LEFT_X, &app->cards[0]);
    }
    if (status == BENTO_OK) {
        status =
            build_button_card(ui, 1U, CARD_RIGHT_X, &app->cards[1]);
    }
    if (status == BENTO_OK) {
        status = add_label(ui,
                           "สั้น = คลิก  ค้าง 0.8 วิ = ค้าง  2 ครั้งเร็ว = "
                           "ดับเบิล",
                           &help, &app->help);
    }
    if (status == BENTO_OK) {
        poll_screen(ui);
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

static void show_level(bento_ui_t *ui, const button_card_t *card,
                       bool down)
{
    (void)bento_ui_set_value(ui, card->led, down ? 1 : 0);
    set_text(ui, card->level, down ? "ระดับ: กด" : "ระดับ: ปล่อย");
}

static void show_counts(const app_t *app, uint32_t i)
{
    const int32_t *counts = app->counts[i];
    char text[TEXT_BYTES];

    (void)snprintf(text, sizeof(text), "%ld",
                   (long)app->patterns[i].edges);
    set_text(app->ui, app->cards[i].seg, text);
    (void)snprintf(text, sizeof(text), "คลิก %ld  ค้าง %ld  ดับเบิล %ld",
                   (long)counts[PATTERN_CLICK],
                   (long)counts[PATTERN_LONG],
                   (long)counts[PATTERN_DOUBLE]);
    set_text(app->ui, app->cards[i].counts, text);
}

/* A pattern was made: count it, name it, and one tune for it. */
static bento_status_t act_on(app_t *app, uint32_t i, int32_t found)
{
    static const char *const names[PATTERN_KINDS] = {"คลิก", "ค้าง",
                                                     "ดับเบิล"};
    static const int32_t sounds[PATTERN_KINDS] = {TUNE_TAP, TUNE_GOOD,
                                                  TUNE_START};
    char text[TEXT_BYTES];

    app->counts[i][found]++;
    (void)snprintf(text, sizeof(text), "%s!", names[found]);
    set_text(app->ui, app->cards[i].last, text);
    return beep(app->ui, sounds[found]);
}

/* Level, then pattern, then the action, for one button. */
static bento_status_t sample_button(app_t *app, uint32_t i,
                                    uint32_t now_ms)
{
    pattern_t *pattern = &app->patterns[i];
    bool down = false;
    bento_status_t status = is_down(i, &down);

    if (status == BENTO_OK) {
        int32_t found;

        if (down != pattern->down) {
            /* the screen LED changes only when the level does */
            show_level(app->ui, &app->cards[i], down);
        }
        found = feed(pattern, down, now_ms);
        if (found != PATTERN_NONE) {
            status = act_on(app, i, found);
        }
    }
    return status;
}

static bento_status_t run_loop(app_t *app)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    uint32_t shown_ms = start_ms;
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        const uint32_t now_ms = bento_time_ticks_ms();
        uint32_t i;

        for (i = 0U;
             (i < (uint32_t)BUTTON_COUNT) && (status == BENTO_OK);
             i++) {
            status = sample_button(app, i, now_ms);
        }
        if ((status == BENTO_OK) &&
            (bento_time_ticks_diff(now_ms, shown_ms) >= TICK_MS)) {
            shown_ms = now_ms;
            show_counts(app, 0U);
            show_counts(app, 1U);
            poll_screen(app->ui);
        }
        if (status == BENTO_OK) {
            status = pause_ms((uint32_t)SAMPLE_MS);
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
