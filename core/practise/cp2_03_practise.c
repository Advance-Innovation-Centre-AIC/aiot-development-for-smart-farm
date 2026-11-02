/*
 * cp2_03_practise.c -- แบบฝึกเติมโค้ด (Code Quest ระดับ 3):
 * เครื่องสถานะของเตือน
 *
 * ไฟล์นี้เหมือน cp2_03_decision_ladder ทุกอย่าง ยกเว้น next_state() ใน
 * ส่วน "3) สมอง" ที่เว้นช่องไว้ 3 ช่อง: A, B, C แต่ละช่องเขียนว่า BLANK
 * หรือ blank_flag เติมคำตอบแทนให้ครบแล้วรัน โปรแกรมตรวจการเปลี่ยนสถานะ
 * 8 กรณีก่อนเปิดจอ (self_test) ผ่านครบ = Console ขึ้น "ผ่าน!" แล้วเล่นต่อได้
 * ยังไม่ถูก = Console บอกว่ากรณีไหนผิด แล้วหยุด (ยังไม่เปิดจอ)
 * OK -> WATCH (เกินเส้น) -> ALARM (เกินต่อเนื่อง HOLD_S วิ) ALARM ค้างไว้
 * แม้ค่าจะกลับปกติ จนกว่าคนจะกด SW5 รับทราบ -> COOLDOWN (พัก COOLDOWN_S
 * วิ) -> OK เฉลยอยู่ที่ practise/solutions/cp2_03_practise_solution
 *
 * Fill-in practice (Code Quest level 3): the alarm state machine. The
 * program is cp2_03_decision_ladder except next_state(), which leaves
 * three blanks, A, B and C, written BLANK or blank_flag. Fill them in
 * and run: self_test checks 8 transitions before the screen opens.
 * All pass = "ผ่าน!" on the console and the board runs as the full
 * example; a wrong case is printed and the program stops before the
 * screen. The answer is practise/solutions/cp2_03_practise_solution.
 *
 * Twin of cp2_03_practise.py (aiot-development-for-smart-farm/core/practise/cp2_03_practise.py). Style: CLEAN_C.md.
 * Differs from the .py:
 *   - C cannot leave a name undefined and still build: each blank
 *     ____ is BLANK (0) or blank_flag (false) here, so an unfilled
 *     file builds, self_test names the first wrong case and stops
 *     before the screen, as the .py stops at its NameError.
 *   - The states are an enum and the test seconds are tenths, so a
 *     case prints as the .py's tuple does (2.9, 3, 99).
 *   - hasattr(ui, "volume") is a fact of the build: the volume is
 *     always set, and a build without it refuses, which is ignored.
 *   - ui.tone() is fire-and-forget there, so its status is ignored.
 *   - Stop ends the program quietly where MicroPython raises
 *     KeyboardInterrupt.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_buttons.h"
#include "bento_fmt.h"
#include "bento_pots.h"
#include "bento_random.h"
#include "bento_runtime.h"
#include "bento_time.h"

/* ---- แก้ก่อนรัน: settings (MISRA deviation CT02) ---- */
enum {
    LIMIT = 50, /* เส้นเตือน (%) */
    /* ขั้น 2: ติดเมื่อเกิน LIMIT + HYST ดับเมื่อต่ำกว่า LIMIT - HYST */
    HYST = 5,
    HOLD_S = 3,      /* ขั้น 3 และ 4: ต้องเห็นต่อเนื่องกี่วินาที */
    COOLDOWN_S = 10, /* ขั้น 4: หลังรับทราบ พักเตือนกี่วินาที */
    NOISE = 2,       /* สั่นจำลองบวกลบกี่ % (0 = ลูกบิดล้วน) */
    DECIDE_MS = 100, /* ตัดสินทุกกี่ ms (4 วิธีเห็นค่าเดียวกัน) */
    /* อ่านปุ่มทุกกี่ ms (เฟิร์มแวร์กรองสั่นทุกครั้งที่อ่าน) */
    SAMPLE_MS = 20,
    TICK_MS = 500,   /* อัปเดตจอทุกกี่ ms (เฉพาะช่องที่เปลี่ยน) */
    RUN_MS = 180000, /* เล่นนาน 3 นาทีแล้วจบเอง */
    SPEAKER = 40,    /* ความดังลำโพงรวม 0-100% (firmware 2.4.2+) */
    VOLUME = 25      /* ความดังเสียง 0-127 (ราว 20%) ทุกเสียงในไฟล์ */
};
/* .py: BTN_NAMES. ชื่อบนแผง: SW5 = ปุ่มล่าง = pressed(1),
 * SW6 = ปุ่มบน = pressed(0) */
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

enum { /* hardware and units */
    SW5_INDEX = 1,
    VR1_INDEX = 0,
    RAW_MAX = 4095,
    PERCENT = 100,
    MS_PER_S = 1000,
    TENTHS_PER_S = 10
};

enum { /* sound */
    TONE_MS = 90,
    NOTE_GAP_MS = 100,
    TUNE_NOTES_MAX = 3
};

enum { /* screen */
    SETTLE_MS = 200,
    WAYS = 4,
    CARD_X = 12,
    CARD_STEP_X = 194, /* 4 cards side by side, 186 wide, 194 apart */
    CARD_Y = 66,
    CARD_W = 186,
    CARD_H = 272,
    CARD_RADIUS = 12,
    CARD_TITLE_DX = 12,
    CARD_TITLE_DY = 6,
    INSET_X = 14,
    STATE_DX = 64,
    LED_Y = 100,
    LED_PX = 44,
    STATE_Y = 114,
    FLIPS_Y = 156,
    SEG_Y = 186,
    SEG_W = 150,
    SEG_H = 56,
    RULE_Y = 262,
    VALUE_Y = 38,
    HELP_Y = 352,
    FONT_TITLE = 24,
    FONT_TEXT = 20, /* Thai text is 20 px or larger on the board */
    TEXT_CHARS = 96,
    HELP_CHARS = 128 /* the Thai help line is 105 bytes in UTF-8 */
};

/* ---- 2) Hardware ---- */
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

/* VR1 as the measured 0-100 %, plus the simulated jitter. */
static bento_status_t read_input(bento_random_t *noise, int32_t *value)
{
    uint16_t raw = 0U;
    int32_t jitter = 0;
    bento_status_t status = bento_pots_read(VR1_INDEX, &raw);

    if (status == BENTO_OK) {
        status = bento_random_randint(noise, -NOISE, NOISE, &jitter);
    }
    *value = (((int32_t)raw * PERCENT) / RAW_MAX) + jitter;
    return status;
}

/* ---- 3) The brain: decides, never touches the hardware ---- */

/* Each blank ____ of the .py: write your answer in place of BLANK
 * (a number), or of blank_flag (a yes or no) in next_state(). */
enum {
    BLANK = 0
};

typedef enum {
    STATE_OK,
    STATE_WATCH,
    STATE_ALARM,
    STATE_COOLDOWN
} alarm_state_t;

static const char *const STATE_NAMES[] = {"OK", "WATCH", "ALARM",
                                          "COOLDOWN"};

/* Step 2: between the two lines the old state stays. */
static bool hysteresis(bool on, int32_t value)
{
    bool result = on;

    if (value > (LIMIT + HYST)) {
        result = true;
    } else if (value < (LIMIT - HYST)) {
        result = false;
    } else {
        /* between the lines: keep the old state */
    }
    return result;
}

/* Step 3: *since_ms is when `over` began to differ from `on`; the
 * result changes once it has differed for HOLD_S seconds. */
static bool hold(bool on, bool over, uint32_t *since_ms,
                 uint32_t now_ms)
{
    bool result = on;

    if (over == on) {
        *since_ms = now_ms;
    } else if (bento_time_ticks_diff(now_ms, *since_ms) >=
               ((int32_t)HOLD_S * (int32_t)MS_PER_S)) {
        result = over;
        *since_ms = now_ms;
    } else {
        /* still waiting for the hold time */
    }
    return result;
}

/* Step 4: the state after `state`, given whether the value is over
 * the line, whether SW5 was just pressed, and how many seconds the
 * board has been in this state. */
static alarm_state_t next_state(alarm_state_t state, bool over,
                                bool acknowledged, bento_real_t t_s)
{
    const bool blank_flag = false; /* blank B: replace it in the line */
    alarm_state_t next = STATE_COOLDOWN;

    /* acknowledged is read once blank B is filled */
    (void)acknowledged;
    if (state == STATE_OK) {
        next = over ? STATE_WATCH : STATE_OK;
    } else if ((state == STATE_WATCH) && !over) {
        next = STATE_OK;
    } else if (state == STATE_WATCH) {
        /* ช่อง A: เกินต่อเนื่องนานเท่าไรจึงเตือน? */
        next = (t_s >= (bento_real_t)BLANK) ? STATE_ALARM : STATE_WATCH;
    } else if (state == STATE_ALARM) {
        /* ช่อง B: อะไรเท่านั้นที่ปลด ALARM ได้? */
        next = blank_flag ? STATE_COOLDOWN : STATE_ALARM;
    } else {
        /* ช่อง C: พักนานเท่าไรจึงกลับ OK? */
        next = (t_s >= (bento_real_t)BLANK) ? STATE_OK : STATE_COOLDOWN;
    }
    return next;
}

typedef struct {
    alarm_state_t state;
    bool over;
    bool acknowledged;
    int32_t tenths; /* seconds in the state, in tenths */
    alarm_state_t want;
} state_case_t;

static const char *bool_text(bool value)
{
    return value ? "True" : "False";
}

static void print_case(const state_case_t *test, alarm_state_t got)
{
    const int32_t whole_s = test->tenths / TENTHS_PER_S;
    const int32_t tenth = test->tenths % TENTHS_PER_S;
    char seconds[TEXT_CHARS];

    if (tenth == 0) {
        (void)snprintf(seconds, sizeof(seconds), "%" PRId32, whole_s);
    } else {
        (void)snprintf(seconds, sizeof(seconds), "%" PRId32 ".%" PRId32,
                       whole_s, tenth);
    }
    (void)printf("ยังไม่ถูก: next_state ('%s', %s, %s, %s) ควรได้ %s "
                 "แต่ได้ %s\n",
                 STATE_NAMES[test->state], bool_text(test->over),
                 bool_text(test->acknowledged), seconds,
                 STATE_NAMES[test->want], STATE_NAMES[got]);
}

/* Checks next_state() on 8 cases before the screen opens. */
static bool self_test(void)
{
    static const state_case_t cases[] = {
        {STATE_WATCH, true, false,
         ((int32_t)HOLD_S * (int32_t)TENTHS_PER_S) - 1, STATE_WATCH},
        {STATE_WATCH, true, false,
         (int32_t)HOLD_S * (int32_t)TENTHS_PER_S, STATE_ALARM},
        {STATE_WATCH, false, false, TENTHS_PER_S, STATE_OK},
        /* back to normal, but nobody has acknowledged it yet */
        {STATE_ALARM, false, false, 990, STATE_ALARM},
        {STATE_ALARM, true, false, 990, STATE_ALARM},
        {STATE_ALARM, false, true, 0, STATE_COOLDOWN},
        {STATE_COOLDOWN, true, false,
         ((int32_t)COOLDOWN_S * (int32_t)TENTHS_PER_S) - 1,
         STATE_COOLDOWN},
        {STATE_COOLDOWN, true, false,
         (int32_t)COOLDOWN_S * (int32_t)TENTHS_PER_S, STATE_OK}};
    const uint32_t count = (uint32_t)(sizeof(cases) / sizeof(cases[0]));
    bool passed = true;
    uint32_t i;

    for (i = 0U; passed && (i < count); i++) {
        const state_case_t *test = &cases[i];
        const alarm_state_t got = next_state(
            test->state, test->over, test->acknowledged,
            (bento_real_t)test->tenths / (bento_real_t)TENTHS_PER_S);

        if (got != test->want) {
            print_case(test, got);
            passed = false;
        }
    }
    if (passed) {
        (void)printf("ผ่าน! เครื่องสถานะถูกทั้ง 8 กรณี\n");
    }
    return passed;
}

/* ---- 4) Network (this file uses none) ---- */

/* ---- 5) The screen ---- */
typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t value_label;
    bento_ui_id_t leds[WAYS];
    bento_ui_id_t states[WAYS];
    bento_ui_id_t segs[WAYS];
    bento_ui_id_t help;
} ladder_screen_t;

static int16_t px(int32_t value)
{
    return (int16_t)value;
}

static bento_status_t add_label(bento_ui_t *ui, const char *text,
                                int32_t x, int32_t y, uint32_t color,
                                bento_ui_id_t *out)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.text = text;
    args.text_len = (uint32_t)strlen(text);
    args.x = px(x);
    args.y = px(y);
    args.color = color;
    args.font_px = FONT_TEXT;
    return bento_ui_label_create(ui, &args, out);
}

/* A card: a dark box with a grey edge and a blue title. */
static bento_status_t add_card(bento_ui_t *ui, int32_t x,
                               const char *title)
{
    bento_ui_widget_args_t args;
    bento_ui_id_t widget;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = px(x);
    args.y = px(CARD_Y);
    args.w = px(CARD_W);
    args.h = px(CARD_H);
    args.color = COL_CARD;
    args.min_val = COL_DIM;
    args.max_val = CARD_RADIUS;
    args.font_px = 1;
    status = bento_ui_panel_create(ui, &args, &widget);
    if (status == BENTO_OK) {
        status = add_label(ui, title, x + CARD_TITLE_DX,
                           CARD_Y + CARD_TITLE_DY, COL_INFO, &widget);
    }
    return status;
}

static bento_status_t add_led_and_seg(ladder_screen_t *screen,
                                      int32_t i, int32_t x)
{
    bento_ui_widget_args_t args;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = px(x + INSET_X);
    args.y = px(LED_Y);
    args.w = px(LED_PX);
    args.h = px(LED_PX);
    args.color = COL_BAD;
    status = bento_ui_led_create(screen->ui, &args, &screen->leds[i]);
    if (status == BENTO_OK) {
        bento_ui_widget_args_init(&args);
        args.text = "0";
        args.text_len = 1U;
        args.x = px(x + INSET_X);
        args.y = px(SEG_Y);
        args.w = px(SEG_W);
        args.h = px(SEG_H);
        args.color = COL_INFO;
        status =
            bento_ui_seg7_create(screen->ui, &args, &screen->segs[i]);
    }
    return status;
}

/* One of the 4 cards: its LED, state, flip counter and rule. */
static bento_status_t add_way(ladder_screen_t *screen, int32_t i,
                              const char *rule)
{
    static const char *const titles[] = {"1 เกณฑ์", "2 กันกระพือ",
                                         "3 รอให้นาน", "4 สถานะ"};
    const int32_t x = CARD_X + (CARD_STEP_X * i);
    bento_ui_id_t widget;
    bento_status_t status = add_card(screen->ui, x, titles[i]);

    if (status == BENTO_OK) {
        status = add_led_and_seg(screen, i, x);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "ดับ", x + STATE_DX, STATE_Y,
                           COL_TEXT, &screen->states[i]);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "สลับ (ครั้ง)", x + INSET_X,
                           FLIPS_Y, COL_DIM, &widget);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, rule, x + INSET_X, RULE_Y,
                           COL_DIM, &widget);
    }
    return status;
}

static bento_status_t add_ways(ladder_screen_t *screen)
{
    char rules[WAYS][TEXT_CHARS];
    int32_t i;
    bento_status_t status = BENTO_OK;

    (void)snprintf(rules[0], sizeof(rules[0]), "> %d", LIMIT);
    (void)snprintf(rules[1], sizeof(rules[1]), "> %d ดับ < %d",
                   LIMIT + HYST, LIMIT - HYST);
    (void)snprintf(rules[2], sizeof(rules[2]), "ต่อเนื่อง %d วิ", HOLD_S);
    (void)snprintf(rules[WAYS - 1], sizeof(rules[0]), "ค้างจนกด %s",
                   BUTTON_NAMES[0]);
    for (i = 0; (i < WAYS) && (status == BENTO_OK); i++) {
        status = add_way(screen, i, rules[i]);
    }
    return status;
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

static bento_status_t build_screen(ladder_screen_t *screen)
{
    bento_ui_widget_args_t args;
    bento_ui_id_t title;
    char help[HELP_CHARS];
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = sleep_ms(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        bento_ui_widget_args_init(&args);
        args.text = "บันไดตัดสิน: ค่าเดียว 4 วิธี";
        args.text_len = (uint32_t)strlen(args.text);
        args.x = px(CARD_X);
        args.y = px(CARD_TITLE_DY);
        args.color = COL_TEXT;
        args.font_px = FONT_TITLE;
        status = bento_ui_label_create(screen->ui, &args, &title);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", CARD_X, VALUE_Y, COL_WARN,
                           &screen->value_label);
    }
    if (status == BENTO_OK) {
        status = add_ways(screen);
    }
    if (status == BENTO_OK) {
        (void)snprintf(help, sizeof(help),
                       "หมุน VR1 ค้างแถวเส้น ดูตัวนับ   %s = รับทราบ",
                       BUTTON_NAMES[0]);
        status = add_label(screen->ui, help, CARD_X, HELP_Y, COL_DIM,
                           &screen->help);
    }
    if (status == BENTO_OK) {
        refresh(screen->ui);
    }
    return status;
}

/* A widget setter's refusal was ignored in MicroPython too (modui.c). */
static void set_text(bento_ui_t *ui, bento_ui_id_t widget,
                     const char *text)
{
    (void)bento_ui_set_text(ui, widget, text, (uint32_t)strlen(text));
}

static void show_way(const ladder_screen_t *screen, int32_t i, bool on,
                     int32_t flips, alarm_state_t state)
{
    static const uint32_t state_colors[] = {COL_OK, COL_WARN, COL_BAD,
                                            COL_INFO};
    char count[TEXT_CHARS];

    (void)snprintf(count, sizeof(count), "%" PRId32, flips);
    set_text(screen->ui, screen->segs[i], count);
    (void)bento_ui_set_value(screen->ui, screen->leds[i], on ? 1 : 0);
    if (i < (WAYS - 1)) {
        set_text(screen->ui, screen->states[i], on ? "ติด" : "ดับ");
    } else {
        set_text(screen->ui, screen->states[i], STATE_NAMES[state]);
        (void)bento_ui_set_color(screen->ui, screen->leds[i],
                                 state_colors[state]);
        (void)bento_ui_set_value(screen->ui, screen->leds[i],
                                 (state == STATE_OK) ? 0 : 1);
    }
}

/* ---- 6) The main program ---- */

/* What the screen shows for one way; it is drawn again only when
 * this changes. */
typedef struct {
    bool known;
    bool on;
    int32_t flips;
    alarm_state_t state;
} way_shown_t;

typedef struct {
    bool on[WAYS];       /* each way's result; way 4: on = in ALARM */
    int32_t flips[WAYS]; /* how often each way switched on or off */
    way_shown_t shown[WAYS];
    alarm_state_t state;
    bool
        acknowledged; /* SW5 was pressed, kept until the next decision */
    bool last_down;
    int32_t value;
    uint32_t hold_ms; /* when step 3's hold began */
    uint32_t state_ms;
    uint32_t decide_ms;
    uint32_t show_ms;
    bento_random_t noise;
} ladder_t;

/* The button is read every round, so a short tap is not missed. */
static bento_status_t sample_button(const ladder_screen_t *screen,
                                    ladder_t *ladder)
{
    bool down = false;
    bento_status_t status = bento_buttons_pressed(SW5_INDEX, &down);

    if ((status == BENTO_OK) && down && !ladder->last_down) {
        ladder->acknowledged = true;
        status = beep(screen->ui, TUNE_TAP);
    }
    ladder->last_down = down;
    return status;
}

/* A sound only when the state changes. */
static bento_status_t change_state(const ladder_screen_t *screen,
                                   ladder_t *ladder, alarm_state_t next,
                                   uint32_t now_ms)
{
    bento_status_t status = BENTO_OK;

    if (next != ladder->state) {
        if (next == STATE_ALARM) {
            status = beep(screen->ui, TUNE_BAD);
        } else if ((next == STATE_OK) &&
                   (ladder->state == STATE_COOLDOWN)) {
            status = beep(screen->ui, TUNE_START);
        } else {
            /* WATCH and COOLDOWN come in silently */
        }
        ladder->state = next;
        ladder->state_ms = now_ms;
    }
    return status;
}

/* Counts every way whose result switched, then keeps the results. */
static void take_results(ladder_t *ladder, const bool fresh[WAYS])
{
    int32_t i;

    for (i = 0; i < WAYS; i++) {
        if (fresh[i] != ladder->on[i]) {
            ladder->flips[i]++;
        }
        ladder->on[i] = fresh[i];
    }
}

/* One value, judged 4 ways. */
static bento_status_t decide(const ladder_screen_t *screen,
                             ladder_t *ladder, uint32_t now_ms)
{
    bool fresh[WAYS];
    bool over;
    alarm_state_t next;
    bento_status_t status = read_input(&ladder->noise, &ladder->value);

    if (status == BENTO_OK) {
        over = ladder->value > LIMIT;
        fresh[2] = hold(ladder->on[2], over, &ladder->hold_ms, now_ms);
        next = next_state(ladder->state, over, ladder->acknowledged,
                          (bento_real_t)bento_time_ticks_diff(
                              now_ms, ladder->state_ms) /
                              (bento_real_t)MS_PER_S);
        ladder->acknowledged = false;
        status = change_state(screen, ladder, next, now_ms);
        fresh[0] = over;
        fresh[1] = hysteresis(ladder->on[1], ladder->value);
        fresh[WAYS - 1] = ladder->state == STATE_ALARM;
        take_results(ladder, fresh);
    }
    return status;
}

static void show_all(const ladder_screen_t *screen, ladder_t *ladder)
{
    char line[TEXT_CHARS];
    int32_t i;

    (void)snprintf(line, sizeof(line), "ค่า %" PRId32 "   เส้น %d",
                   ladder->value, LIMIT);
    set_text(screen->ui, screen->value_label, line);
    for (i = 0; i < WAYS; i++) {
        way_shown_t now;

        now.known = true;
        now.on = ladder->on[i];
        now.flips = ladder->flips[i];
        now.state = (i == (WAYS - 1)) ? ladder->state : STATE_OK;
        if (!ladder->shown[i].known ||
            (now.on != ladder->shown[i].on) ||
            (now.flips != ladder->shown[i].flips) ||
            (now.state != ladder->shown[i].state)) {
            show_way(screen, i, now.on, now.flips, ladder->state);
            ladder->shown[i] = now;
        }
    }
    refresh(screen->ui);
}

static bento_status_t run_loop(const ladder_screen_t *screen,
                               ladder_t *ladder)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    uint32_t now_ms;
    bento_status_t status = BENTO_OK;

    ladder->hold_ms = start_ms;
    ladder->state_ms = start_ms;
    ladder->decide_ms = start_ms;
    ladder->show_ms = start_ms;
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        now_ms = bento_time_ticks_ms();
        status = sample_button(screen, ladder);
        if ((status == BENTO_OK) &&
            (bento_time_ticks_diff(now_ms, ladder->decide_ms) >=
             DECIDE_MS)) {
            ladder->decide_ms = now_ms;
            status = decide(screen, ladder, now_ms);
        }
        if ((status == BENTO_OK) &&
            (bento_time_ticks_diff(now_ms, ladder->show_ms) >=
             TICK_MS)) {
            ladder->show_ms = now_ms;
            show_all(screen, ladder);
        }
        if (status == BENTO_OK) {
            status = sleep_ms(SAMPLE_MS);
        }
    }
    return status;
}

int main(void)
{
    ladder_screen_t screen;
    ladder_t ladder;
    bento_status_t status;

    (void)memset(&screen, 0, sizeof(screen));
    (void)memset(&ladder, 0, sizeof(ladder));
    screen.ui = bento_runtime_get_ui();
    /* a build without a speaker volume refuses it, as hasattr() skips
     * it */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    status = bento_random_init(&ladder.noise);
    /* next_state() is checked first; a failed test ends the program
     * before the screen opens */
    if ((status == BENTO_OK) && self_test()) {
        status = build_screen(&screen);
        if (status == BENTO_OK) {
            status = run_loop(&screen, &ladder);
        }
        if (status == BENTO_OK) {
            (void)bento_ui_set_color(screen.ui, screen.help, COL_WARN);
            set_text(screen.ui, screen.help,
                     "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่");
            refresh(screen.ui);
        }
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
