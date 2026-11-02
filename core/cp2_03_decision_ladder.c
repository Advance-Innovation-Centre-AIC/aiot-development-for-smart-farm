/*
 * cp2_03_decision_ladder.c -- หลักการ 2.3: บันไดการตัดสิน
 * ใช้ขั้นที่ต่ำที่สุดที่ได้ผล
 *
 * ค่าเดียวกัน ตัดสิน 4 วิธีพร้อมกัน แล้วนับว่าแต่ละวิธี "สลับ" ติด/ดับ
 * กี่ครั้ง 1 เกณฑ์ = เกิน LIMIT ติดทันที 2 กันกระพือ (hysteresis) = ติดเมื่อ
 * เกิน LIMIT + HYST ดับเมื่อต่ำกว่า LIMIT - HYST 3 รอให้นาน (hold-time) =
 * ต้องเห็นต่างจากเดิมต่อเนื่อง HOLD_S วินาที 4 เครื่องสถานะ OK -> WATCH ->
 * ALARM ค้างไว้จนกว่าคนจะกด SW5 รับทราบ -> COOLDOWN -> OK หมุน VR1
 * ไปค้างไว้แถวเส้น 50 ดูตัวนับ หมุนขึ้นไป 70 ค้างไว้ 3 วิ แล้วหมุนกลับลง
 * ขั้น 4 ยังค้าง ALARM จนกด SW5 (ปุ่มล่าง) สั่นบวกลบ NOISE % เป็นของจำลอง
 * ในโปรแกรม เล่น 3 นาทีแล้วจบเอง ใช้กับ TESAIoT Dev Kit และ BENTO Emulator
 *
 * A decision ladder: one value (knob VR1 plus simulated jitter) judged
 * four ways at once, each card counting how often its answer flipped:
 * a plain threshold, hysteresis, a hold time, and a state machine whose
 * ALARM stays until SW5 acknowledges it, then rests in COOLDOWN.
 * Tones mark ALARM, the acknowledgement and the return to OK. It runs
 * for three minutes.
 *
 * Twin of cp2_03_decision_ladder.py (aiot-development-for-smart-farm/core/cp2_03_decision_ladder.py). Style: CLEAN_C.md.
 * What C does differently:
 *   - the state names are an enum with a table of their texts and
 *     colours, compared as numbers, not strings;
 *   - the time in a state is compared in whole ms (HOLD_S * 1000)
 *     where the .py divides by 1000 first: the same answer for every
 *     whole ms;
 *   - None (nothing shown yet) is a bool in each card's last shown
 *     values;
 *   - random keeps its state in a struct, started as `import random`
 *     leaves it on this board;
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
#include "bento_pots.h"
#include "bento_random.h"
#include "bento_runtime.h"
#include "bento_time.h"

/* Settings a student may change. */
enum {
    LIMIT = 50, /* the warning line, % */
    HYST = 5,   /* rule 2: on above LIMIT + HYST, off below - */
    HOLD_S = 3, /* rules 3 and 4: seen this many seconds in a row */
    COOLDOWN_S = 10, /* rule 4: rest this long after SW5 */
    NOISE = 2,       /* simulated jitter of +- this many % */
    DECIDE_MS = 100, /* judge this often (all four see one value) */
    SAMPLE_MS = 20,  /* read the button this often */
    TICK_MS = 500,   /* refresh the screen this often */
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

/* Screen layout, in pixels: four cards 186 wide, 194 apart. */
enum {
    MARGIN_X = 12,
    TITLE_Y = 6,
    VALUE_Y = 38,
    HELP_Y = 352,
    CARD_X = 12,
    CARD_STEP_X = 194,
    CARD_Y = 66,
    CARD_W = 186,
    CARD_H = 272,
    CARD_RADIUS = 12,
    CARD_BORDER = 1,
    CARD_TITLE_DX = 12,
    CARD_TITLE_DY = 6,
    INSET_X = 14,
    LED_Y = 100,
    LED_SIZE = 44,
    STATE_DX = 64,
    STATE_Y = 114,
    FLIPS_CAPTION_Y = 156,
    SEG_Y = 186,
    SEG_W = 150,
    SEG_H = 56,
    RULE_Y = 262
};

/* Font sizes, in pixels. */
enum {
    FONT_TITLE = 24,
    FONT_TEXT = 20 /* Thai is 20 px or more on the board */
};

/* The rules, the states, the knob and the button. */
enum {
    RULES = 4,
    RULE_LINE = 0,
    RULE_HYSTERESIS = 1,
    RULE_HOLD = 2,
    RULE_STATE = 3,
    STATE_OK = 0,
    STATE_WATCH = 1,
    STATE_ALARM = 2,
    STATE_COOLDOWN = 3,
    STATES = 4,
    BUTTON_SW5 = 1, /* the lower button is pressed(1) */
    RAW_MAX = 4095, /* a 12-bit ADC */
    PERCENT = 100,
    MS_PER_S = 1000
};

/* Sound, text and timing that are not settings. */
enum {
    SETTLE_MS = 200,   /* let the cleared screen settle */
    TONE_MS = 90,      /* each note of a tune */
    NOTE_GAP_MS = 100, /* one note to the next */
    TUNE_NOTES = 3,    /* the longest tune */
    TEXT_BYTES = 128   /* a SET_TEXT carries at most 126 bytes */
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

/* What a card shows; known = false before the first time. */
typedef struct {
    bool known;
    bool on;
    int32_t flips;
    int32_t state;
} shown_t;

typedef struct {
    bento_ui_id_t led;
    bento_ui_id_t state;
    bento_ui_id_t seg;
} rule_card_t;

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t value_text;
    bento_ui_id_t help;
    rule_card_t cards[RULES];
    bento_random_t random;
    bool on[RULES]; /* rule 4 is on while in ALARM */
    int32_t flips[RULES];
    shown_t shown[RULES];
    int32_t state;
    bool acknowledged;
    bool last_down;
    int32_t value;
    uint32_t hold_ms;  /* when the value began to differ (rule 3) */
    uint32_t state_ms; /* when the state machine entered its state */
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

/* VR1 = the measured value 0-100 %, plus simulated jitter. */
static bento_status_t read_input(app_t *app, int32_t *value)
{
    uint16_t raw = 0U;
    int32_t jitter = 0;
    bento_status_t status = bento_pots_read(0U, &raw);

    if (status == BENTO_OK) {
        status =
            bento_random_randint(&app->random, -NOISE, NOISE, &jitter);
    }
    *value = (((int32_t)raw * PERCENT) / RAW_MAX) + jitter;
    return status;
}

/* Rule 2: between the two lines the answer stays as it was. */
static bool hysteresis(bool on, int32_t value)
{
    bool answer = on;

    if (value > (LIMIT + HYST)) {
        answer = true;
    } else if (value < (LIMIT - HYST)) {
        answer = false;
    } else {
        /* between the lines: unchanged */
    }
    return answer;
}

/* Rule 3: *since_ms = when `over` began to differ from `on`; the
 * answer changes once it has differed for HOLD_S seconds. */
static bool hold(bool on, bool over, uint32_t *since_ms,
                 uint32_t now_ms)
{
    bool answer = on;

    if (over == on) {
        *since_ms = now_ms;
    } else if (bento_time_ticks_diff(now_ms, *since_ms) >=
               (HOLD_S * MS_PER_S)) {
        answer = over;
        *since_ms = now_ms;
    } else {
        /* still waiting */
    }
    return answer;
}

/* Rule 4: the next state from this one, whether the value is over the
 * line, whether SW5 was just pressed, and the ms spent in this state. */
static int32_t next_state(int32_t state, bool over, bool acknowledged,
                          int32_t in_state_ms)
{
    int32_t next = state;

    switch (state) {
    case STATE_OK:
        next = over ? STATE_WATCH : STATE_OK;
        break;
    case STATE_WATCH:
        if (!over) {
            next = STATE_OK;
        } else if (in_state_ms >= (HOLD_S * MS_PER_S)) {
            next = STATE_ALARM;
        } else {
            next = STATE_WATCH;
        }
        break;
    case STATE_ALARM:
        /* held until someone acknowledges it */
        next = acknowledged ? STATE_COOLDOWN : STATE_ALARM;
        break;
    default:
        next = (in_state_ms >= (COOLDOWN_S * MS_PER_S))
                   ? STATE_OK
                   : STATE_COOLDOWN;
        break;
    }
    return next;
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
    args.color = COL_BAD;
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

/* The rule under each card, from the settings. */
static void rule_text(uint32_t i, char *text, uint32_t cap)
{
    if (i == (uint32_t)RULE_LINE) {
        (void)snprintf(text, cap, "> %d", LIMIT);
    } else if (i == (uint32_t)RULE_HYSTERESIS) {
        (void)snprintf(text, cap, "> %d ดับ < %d", LIMIT + HYST,
                       LIMIT - HYST);
    } else if (i == (uint32_t)RULE_HOLD) {
        (void)snprintf(text, cap, "ต่อเนื่อง %d วิ", HOLD_S);
    } else {
        (void)snprintf(text, cap, "ค้างจนกด SW5");
    }
}

/* One rule's card: its LED, its answer, its flip counter, its rule. */
static bento_status_t build_rule_card(bento_ui_t *ui, uint32_t i,
                                      rule_card_t *card)
{
    static const char *const titles[RULES] = {"1 เกณฑ์", "2 กันกระพือ",
                                              "3 รอให้นาน", "4 สถานะ"};
    const int32_t x = CARD_X + (CARD_STEP_X * (int32_t)i);
    const rect_t box = {x, CARD_Y, CARD_W, CARD_H};
    const label_spec_t state = {x + STATE_DX, STATE_Y, FONT_TEXT,
                                COL_TEXT};
    const label_spec_t caption = {x + INSET_X, FLIPS_CAPTION_Y,
                                  FONT_TEXT, COL_DIM};
    const label_spec_t rule = {x + INSET_X, RULE_Y, FONT_TEXT, COL_DIM};
    char text[TEXT_BYTES];
    bento_ui_id_t unused;
    bento_status_t status = add_card(ui, &box, titles[i]);

    if (status == BENTO_OK) {
        status = add_led(ui, x + INSET_X, &card->led);
    }
    if (status == BENTO_OK) {
        status = add_label(ui, "ดับ", &state, &card->state);
    }
    if (status == BENTO_OK) {
        status = add_label(ui, "สลับ (ครั้ง)", &caption, &unused);
    }
    if (status == BENTO_OK) {
        status = add_seg(ui, x + INSET_X, &card->seg);
    }
    if (status == BENTO_OK) {
        rule_text(i, text, sizeof(text));
        status = add_label(ui, text, &rule, &unused);
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
    const label_spec_t value = {MARGIN_X, VALUE_Y, FONT_TEXT, COL_WARN};
    const label_spec_t help = {MARGIN_X, HELP_Y, FONT_TEXT, COL_DIM};
    bento_ui_id_t unused;
    uint32_t i;
    bento_status_t status =
        bento_ui_screen(app->ui, BENTO_RUNTIME_SCREEN_WIDTH_PX,
                        BENTO_RUNTIME_SCREEN_HEIGHT_PX);

    if (status == BENTO_OK) {
        status = pause_ms((uint32_t)SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_label(app->ui, "บันไดตัดสิน: ค่าเดียว 4 วิธี", &title,
                           &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(app->ui, " ", &value, &app->value_text);
    }
    for (i = 0U; (i < (uint32_t)RULES) && (status == BENTO_OK); i++) {
        status = build_rule_card(app->ui, i, &app->cards[i]);
    }
    if (status == BENTO_OK) {
        status =
            add_label(app->ui, "หมุน VR1 ค้างแถวเส้น ดูตัวนับ   SW5 = รับทราบ",
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

/* One card: its counter, its LED, and its answer or state. */
static void show(const app_t *app, uint32_t i)
{
    static const char *const state_names[STATES] = {
        "OK", "WATCH", "ALARM", "COOLDOWN"};
    static const uint32_t state_colors[STATES] = {COL_OK, COL_WARN,
                                                  COL_BAD, COL_INFO};
    const rule_card_t *card = &app->cards[i];
    char text[TEXT_BYTES];

    (void)snprintf(text, sizeof(text), "%ld", (long)app->flips[i]);
    set_text(app->ui, card->seg, text);
    (void)bento_ui_set_value(app->ui, card->led, app->on[i] ? 1 : 0);
    if (i < (uint32_t)RULE_STATE) {
        set_text(app->ui, card->state, app->on[i] ? "ติด" : "ดับ");
    } else {
        set_text(app->ui, card->state, state_names[app->state]);
        (void)bento_ui_set_color(app->ui, card->led,
                                 state_colors[app->state]);
        (void)bento_ui_set_value(app->ui, card->led,
                                 (app->state == STATE_OK) ? 0 : 1);
    }
}

/* The value line and each card whose shown values changed. */
static void show_all(app_t *app)
{
    char text[TEXT_BYTES];
    uint32_t i;

    (void)snprintf(text, sizeof(text), "ค่า %ld   เส้น %d",
                   (long)app->value, LIMIT);
    set_text(app->ui, app->value_text, text);
    for (i = 0U; i < (uint32_t)RULES; i++) {
        shown_t now = {true, app->on[i], app->flips[i], 0};
        shown_t *last = &app->shown[i];

        if (i == (uint32_t)RULE_STATE) {
            now.state = app->state;
        }
        if (!last->known || (last->on != now.on) ||
            (last->flips != now.flips) || (last->state != now.state)) {
            show(app, i);
            *last = now;
        }
    }
}

/* Rule 4 moves on; a tone when it enters ALARM, or OK after resting. */
static bento_status_t step_state(app_t *app, bool over, uint32_t now_ms)
{
    const int32_t next =
        next_state(app->state, over, app->acknowledged,
                   bento_time_ticks_diff(now_ms, app->state_ms));
    bento_status_t status = BENTO_OK;

    app->acknowledged = false;
    if (next != app->state) {
        if (next == STATE_ALARM) {
            status = beep(app->ui, TUNE_BAD);
        } else if ((next == STATE_OK) &&
                   (app->state == STATE_COOLDOWN)) {
            status = beep(app->ui, TUNE_START);
        } else {
            /* no tone */
        }
        app->state = next;
        app->state_ms = now_ms;
    }
    return status;
}

/* Each answer that differs from the last one is a flip. */
static void count_flips(app_t *app, const bool *answers)
{
    uint32_t i;

    for (i = 0U; i < (uint32_t)RULES; i++) {
        if (answers[i] != app->on[i]) {
            app->flips[i]++;
        }
        app->on[i] = answers[i];
    }
}

/* One value, judged four ways; each flip of an answer is counted. */
static bento_status_t decide(app_t *app, uint32_t now_ms)
{
    bool answers[RULES];
    bento_status_t status = read_input(app, &app->value);

    if (status == BENTO_OK) {
        const bool over = app->value > LIMIT;

        answers[RULE_HOLD] =
            hold(app->on[RULE_HOLD], over, &app->hold_ms, now_ms);
        status = step_state(app, over, now_ms);
        answers[RULE_LINE] = over;
        answers[RULE_HYSTERESIS] =
            hysteresis(app->on[RULE_HYSTERESIS], app->value);
        answers[RULE_STATE] = app->state == STATE_ALARM;
        count_flips(app, answers);
    }
    return status;
}

/* Read SW5 every round; a press is remembered until the next
 * decision. */
static bento_status_t read_acknowledge(app_t *app)
{
    bool down = false;
    bento_status_t status = bento_buttons_pressed(BUTTON_SW5, &down);

    if ((status == BENTO_OK) && down && !app->last_down) {
        app->acknowledged = true;
        status = beep(app->ui, TUNE_TAP);
    }
    app->last_down = down;
    return status;
}

static bento_status_t run_loop(app_t *app)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    uint32_t decided_ms = start_ms;
    uint32_t shown_ms = start_ms;
    bento_status_t status = BENTO_OK;

    app->hold_ms = start_ms;
    app->state_ms = start_ms;
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        const uint32_t now_ms = bento_time_ticks_ms();

        status = read_acknowledge(app);
        if ((status == BENTO_OK) &&
            (bento_time_ticks_diff(now_ms, decided_ms) >= DECIDE_MS)) {
            decided_ms = now_ms;
            status = decide(app, now_ms);
        }
        if ((status == BENTO_OK) &&
            (bento_time_ticks_diff(now_ms, shown_ms) >= TICK_MS)) {
            shown_ms = now_ms;
            show_all(app);
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
    app.state = STATE_OK;
    status = bento_random_init(&app.random);
    /* firmware 2.4.1 has no volume and refuses it; the .py skips it */
    (void)bento_ui_sfx_volume_set(app.ui, SPEAKER);
    if (status == BENTO_OK) {
        status = build_screen(&app);
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
