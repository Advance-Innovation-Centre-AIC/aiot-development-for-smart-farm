/*
 * cp4_03_practise.c -- แบบฝึกเติมโค้ด (Code Quest ระดับ 3 โบนัส):
 * ตารางส่งต่อเหตุการณ์
 *
 * ไฟล์นี้เหมือน cp4_03_event_router ทุกอย่าง ยกเว้นตารางใน route() ในส่วน
 * "3) สมอง" ที่เว้นช่องไว้ 3 ช่อง: A, B, C ช่องชนิดเหตุการณ์เขียนว่า BLANK
 * ช่องตัวจัดการเขียนว่า COMMAND_NONE เติมคำตอบแทนให้ครบแล้วรัน โปรแกรม
 * ป้อนเหตุการณ์ปลอม 5 กรณีเข้าตารางก่อนเปิดจอ (self_test) ผ่านครบ =
 * Console ขึ้น "ผ่าน!" แล้วเล่นต่อได้เหมือนไฟล์ตัวอย่าง ยังไม่ถูก = Console
 * บอกว่ากรณีไหนผิด แล้วหยุด (ยังไม่เปิดจอ) ตารางใช้คู่ (widget, ชนิด
 * เหตุการณ์) เป็นกุญแจ โจทย์โบนัส เฉลยอยู่กับผู้สอน
 *
 * Fill-in practice (Code Quest level 3, bonus): the event routing
 * table. The program is cp4_03_event_router except the table in
 * route(), which leaves three blanks, A, B and C: an event type
 * written BLANK, or a handler written COMMAND_NONE. Fill them in and run: self_test
 * feeds 5 made-up events through the table before the screen opens.
 * All pass = "ผ่าน!" on the console and the board runs as the full
 * example; a wrong case is printed and the program stops before the
 * screen.
 *
 * Twin of cp4_03_practise.py (aiot-development-for-smart-farm/core/practise/cp4_03_practise.py). Style: CLEAN_C.md.
 * Differs from the original:
 *   - C cannot leave a name undefined and still build: each blank
 *     ____ is BLANK (0, an event type no event has) or COMMAND_NONE
 *     (no handler) here, so an unfilled file builds, self_test names
 *     the first wrong case and stops before the screen, as the .py
 *     stops at its NameError.
 *   - build_router(ids) is route()'s table read with a widget id
 *     list: self_test passes the made-up ids 1-5, the program the
 *     ids its screen gave.
 *   - ui.volume has no hasattr test: a board without it answers
 *     BENTO_E_UNSUPPORTED, which is skipped as the .py skips the line.
 *   - The router is a static const table of (widget, BENTO_UI_EVENT_*,
 *     command) and a switch, not a dict of functions; events are
 *     compared by enum and id, never by name. The event's name is
 *     only written on screen, from the same names modui.c gives.
 *   - TH_START is THRESHOLD_START here (the lint refuses "th").
 *   - The roller's three options are one text joined with newlines
 *     in the source, as the .py's "\n".join builds it.
 *   - A watering method outside the three (the .py's IndexError)
 *     ends the program with BENTO_E_INVALID_ARGUMENT.
 *   - Every light matrix status is ignored, as the .py's try/except
 *     OSError ignores a failed draw.
 *   - The tune table keeps only the tunes this program plays.
 *   - Stop ends the program at once, without the closing screen, as
 *     KeyboardInterrupt does in the .py.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_pots.h"
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_time.h"

/* Settings a student may change. */
enum {
    THRESHOLD_START =
        40,          /* THRESHOLD_START: the starting threshold, % */
    LOOP_MS = 30,    /* read the events this often */
    TICK_MS = 500,   /* read the soil and refresh its line this often */
    RUN_MS = 180000, /* the program ends after three minutes */
    SPEAKER = 40,    /* speaker volume, 0-100 % */
    VOLUME = 25      /* tone velocity, 0-127 (about 20 %) */
};

/* Knob and modes. */
enum {
    PERCENT = 100,
    ADC_FULL = 4095, /* pots.read() at the right end */
    VR1 = 0,
    SOIL_START = 100,
    MODE_COUNT = 3
};

/* Sound. */
enum {
    NOTE_MS = 90,
    NOTE_GAP_MS = 100,
    TUNE_NOTES_MAX = 2
};

/* The tunes this program plays. */
typedef enum {
    TUNE_TAP = 0,
    TUNE_START,
    TUNE_STOP
} tune_t;

/* Screen layout. */
enum {
    MARGIN_X = 12,
    TITLE_Y = 6,
    BUTTON_Y = 56,
    BUTTON_H = 80,
    PUMP_W = 250,
    STOP_X = 274,
    STOP_W = 150,
    ROLLER_X = 440,
    ROLLER_W = 160,
    ROLLER_H = 130,
    LED_X = 620,
    LED_Y = 60,
    LED_SIZE = 56,
    PUMP_LABEL_X = 690,
    PUMP_LABEL_Y = 74,
    SWITCH_X = 620,
    SWITCH_Y = 140,
    SWITCH_W = 64,
    SWITCH_H = 32,
    AUTO_LABEL_X = 694,
    AUTO_LABEL_Y = 146,
    SOIL_Y = 150,
    SLIDER_X = 24,
    SLIDER_Y = 190,
    SLIDER_W = 388,
    SLIDER_H = 24,
    LAST_Y = 236,
    NOTE_Y = 278,
    HELP_Y = 352,
    FONT_TEXT = 20,
    FONT_TITLE = 24,
    SETTLE_MS = 200, /* let the cleared screen settle */
    TEXT_BYTES = 64  /* "ดิน %d %%  เกณฑ์ %d %%" with any int32_t */
};

/* Colours, 0xRRGGBB. */
enum {
    COL_TEXT = 0xE8EAED,
    COL_DIM = 0x9AA3AF,
    COL_OK = 0x30A46C,
    COL_WARN = 0xF5A623,
    COL_BAD = 0xE5484D,
    COL_INFO = 0x4A9EFF
};

/* The widgets the router knows, by their place in screen.routed. */
typedef enum {
    ROUTED_PUMP = 0,
    ROUTED_STOP,
    ROUTED_THRESHOLD,
    ROUTED_MODE,
    ROUTED_AUTO,
    ROUTED_COUNT
} routed_t;

/* What a routed event asks the program to do. */
typedef enum {
    COMMAND_HOLD = 0,
    COMMAND_TAP,
    COMMAND_STOP,
    COMMAND_THRESHOLD,
    COMMAND_MODE,
    COMMAND_AUTO,
    COMMAND_NONE
} command_t;

/* Each blank ____ of the .py that is an event type: write your
 * answer (BENTO_UI_EVENT_...) in place of BLANK; no event has type 0. */
enum {
    BLANK = 0
};

typedef struct {
    routed_t widget;
    uint8_t type;
    command_t command;
} route_t;

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

/* The widgets the program changes or routes after building. */
typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t routed[ROUTED_COUNT];
    bento_ui_id_t led;
    bento_ui_id_t soil;
    bento_ui_id_t last;
    bento_ui_id_t note;
    bento_ui_id_t help;
} router_screen_t;

/* The program's state: the screen is only a picture of it. */
typedef struct {
    bool pump;
    bool held;
    int32_t threshold;
    int32_t mode;
    bool automatic;
} pump_state_t;

/* What the screen and the light matrix show now. */
typedef struct {
    bool known;
    bool pump;
    int32_t mode;
    bool automatic;
} picture_t;

typedef struct {
    bool known;
    int32_t soil;
    int32_t threshold;
} soil_line_t;

typedef struct {
    router_screen_t screen;
    pump_state_t state;
    picture_t shown;
    soil_line_t soil_shown;
    int32_t soil;
    uint32_t tick_ms;
} router_t;

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

static void set_text(bento_ui_t *ui, bento_ui_id_t widget,
                     const char *text)
{
    /* a setter's refusal was ignored before too (modui.c) */
    (void)bento_ui_set_text(ui, widget, text, (uint32_t)strlen(text));
}

/* One ui.poll(): the events waiting, at most 8. */
static uint32_t poll_events(bento_ui_t *ui, bento_ui_event_t *events)
{
    uint32_t count = 0U;

    /* a failed poll answers no events, as ui.poll() does */
    if (bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                      NULL) != BENTO_OK) {
        count = 0U;
    }
    return count;
}

static bool event_is(const bento_ui_event_t *event, uint8_t kind,
                     bento_ui_id_t widget)
{
    return (event->type == kind) && (event->id == widget);
}

/* The name modui.c gives an event type, for the screen only. */
static const char *event_name(uint8_t type)
{
    static const char *const names[] = {
        "unknown",    "clicked",      "value_changed",
        "toggled",    "pressed",      "released",
        "press_lost", "long_pressed", "long_pressed_repeat",
        "ready",      "cancel",       "focused",
        "defocused",  "scroll_begin", "scroll_end",
        "gesture"};
    const uint32_t count = sizeof(names) / sizeof(names[0]);

    return names[((uint32_t)type < count) ? type : 0U];
}

/* Plays a tune note by note, 100 ms apart. */
static bento_status_t beep(bento_ui_t *ui, tune_t tune)
{
    static const uint8_t notes[][TUNE_NOTES_MAX] = {
        {76U, 0U}, {72U, 79U}, {79U, 72U}};
    static const uint32_t note_count[] = {1U, 2U, 2U};
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

static bento_status_t read_percent(int32_t *percent)
{
    uint16_t raw = 0U;
    const bento_status_t status = bento_pots_read(VR1, &raw);

    *percent = ((int32_t)raw * PERCENT) / ADC_FULL;
    return status;
}

/* Pump running: the whole matrix in the method's colour; stopped:
 * dark. Drawn only when the picture changes. */
static bento_status_t draw_matrix(bool pump, int32_t mode)
{
    static const int32_t colors[MODE_COUNT] = {BENTO_RGBMATRIX_BLUE,
                                               BENTO_RGBMATRIX_CYAN,
                                               BENTO_RGBMATRIX_GREEN};
    bento_status_t status = BENTO_OK;

    /* a draw that does not get through is skipped, as the .py's
     * except OSError does */
    if (!pump) {
        (void)bento_rgbmatrix_clear();
    } else if ((mode >= 0) && (mode < MODE_COUNT)) {
        (void)bento_rgbmatrix_fill(colors[mode]);
    } else {
        status = BENTO_E_INVALID_ARGUMENT;
    }
    return status;
}

/* A long press is the confirmation: the pump starts by hand and the
 * automatic mode ends. The click that follows on release is no new
 * tap. */
static const char *on_hold(pump_state_t *state)
{
    state->held = true;
    state->pump = true;
    state->automatic = false;
    return "เปิดปั๊มแล้ว";
}

/* A short tap does nothing yet: it says how. */
static const char *on_tap(pump_state_t *state)
{
    const char *note = "กดค้างเพื่อยืนยัน";

    if (state->held) {
        state->held = false;
        note = NULL;
    }
    return note;
}

/* Stop beats everything. */
static const char *on_stop(pump_state_t *state)
{
    state->pump = false;
    state->automatic = false;
    state->held = false;
    return "หยุดแล้ว";
}

/* Runs a command; answers the line for the person, or NULL. The
 * slider and the roller send their value, the switch 1 for on. */
static const char *run_command(pump_state_t *state, command_t command,
                               int32_t value)
{
    const char *note = NULL;

    switch (command) {
    case COMMAND_HOLD:
        note = on_hold(state);
        break;
    case COMMAND_TAP:
        note = on_tap(state);
        break;
    case COMMAND_STOP:
        note = on_stop(state);
        break;
    case COMMAND_THRESHOLD:
        state->threshold = value;
        break;
    case COMMAND_MODE:
        state->mode = value;
        break;
    case COMMAND_AUTO:
        state->automatic = value == 1;
        break;
    default:
        /* an event the table does not route */
        break;
    }
    return note;
}

/* Looks the pair (widget, type) up in the table; ids holds each
 * routed widget's id. */
static command_t route(const bento_ui_id_t ids[ROUTED_COUNT],
                       const bento_ui_event_t *event)
{
    /* (widget, event type) -> command; an event not in it is not
     * wanted. Each blank ____ of the .py: BLANK is an event type to
     * write (BENTO_UI_EVENT_...), and blank B's COMMAND_NONE a
     * handler to write (COMMAND_...). */
    static const route_t routes[] = {
        {ROUTED_PUMP, BENTO_UI_EVENT_LONG_PRESSED, COMMAND_HOLD},
        /* ช่อง A: แตะสั้นแล้วปล่อย ส่งเหตุการณ์ชื่ออะไร (มาเสมอ) */
        {ROUTED_PUMP, BLANK, COMMAND_TAP},
        /* ช่อง B: ปุ่มหยุดควรไปหาตัวจัดการตัวไหน */
        {ROUTED_STOP, BENTO_UI_EVENT_CLICKED, COMMAND_NONE},
        {ROUTED_THRESHOLD, BENTO_UI_EVENT_VALUE_CHANGED,
         COMMAND_THRESHOLD},
        {ROUTED_MODE, BENTO_UI_EVENT_VALUE_CHANGED, COMMAND_MODE},
        /* ช่อง C: Switch ส่งเหตุการณ์ชื่ออะไร */
        {ROUTED_AUTO, BLANK, COMMAND_AUTO}};
    command_t command = COMMAND_NONE;
    uint32_t i;

    for (i = 0U; i < (sizeof(routes) / sizeof(routes[0])); i++) {
        if (event_is(event, routes[i].type, ids[routes[i].widget])) {
            command = routes[i].command;
        }
    }
    return command;
}

typedef struct {
    bento_ui_id_t id;
    uint8_t type;
    bool pump; /* should the pump be on */
    bool note; /* should there be a line for the person */
    bool automatic;
} route_case_t;

static const char *bool_text(bool value)
{
    return value ? "True" : "False";
}

/* Feeds made-up events through the table before the screen opens
 * (made-up ids: pump 1, stop 2, switch 5). Cases: 1 tap the pump,
 * 2 long press, 3 release after the long press, 4 tap stop, 5 switch
 * the automatic mode on. Every case sends value 1 (switch: 1 = on);
 * no other handler here reads it. */
static bool self_test(void)
{
    static const bento_ui_id_t ids[ROUTED_COUNT] = {1U, 2U, 3U, 4U, 5U};
    static const route_case_t cases[] = {
        {1U, BENTO_UI_EVENT_CLICKED, false, true, false},
        {1U, BENTO_UI_EVENT_LONG_PRESSED, true, true, false},
        {1U, BENTO_UI_EVENT_CLICKED, true, false, false},
        {2U, BENTO_UI_EVENT_CLICKED, false, true, false},
        {5U, BENTO_UI_EVENT_TOGGLED, false, false, true}};
    const uint32_t count = (uint32_t)(sizeof(cases) / sizeof(cases[0]));
    pump_state_t state;
    bento_ui_event_t event;
    bool passed = true;
    uint32_t i;

    (void)memset(&state, 0, sizeof(state));
    (void)memset(&event, 0, sizeof(event));
    for (i = 0U; passed && (i < count); i++) {
        const route_case_t *test = &cases[i];
        const char *note = NULL;

        event.id = test->id;
        event.type = test->type;
        event.value = 1;
        note = run_command(&state, route(ids, &event), event.value);
        passed = (state.pump == test->pump) &&
                 ((note != NULL) == test->note) &&
                 (state.automatic == test->automatic);
        if (!passed) {
            (void)printf(
                "ยังไม่ถูก: (%" PRIu32 ", '%s') ควรได้ (%s, %s, %s)\n",
                test->id, event_name(test->type), bool_text(test->pump),
                bool_text(test->note), bool_text(test->automatic));
        }
    }
    if (passed) {
        (void)printf("ผ่าน! ครบ 5 กรณี\n");
    }
    return passed;
}

static void place(bento_ui_widget_args_t *args, rect_t box)
{
    bento_ui_widget_args_init(args);
    args->x = box.x;
    args->y = box.y;
    args->w = box.w;
    args->h = box.h;
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

static bento_status_t add_button(bento_ui_t *ui, const char *text,
                                 rect_t box, uint32_t color,
                                 bento_ui_id_t *out)
{
    bento_ui_widget_args_t args;

    place(&args, box);
    args.text = text;
    args.text_len = (uint32_t)strlen(text);
    args.color = color;
    args.font_px = FONT_TEXT;
    return bento_ui_button_create(ui, &args, out);
}

/* The pump button asks for long_pressed first: only clicked,
 * value_changed and toggled come without asking. */
static bento_status_t add_buttons(router_screen_t *screen)
{
    const rect_t pump_box = {MARGIN_X, BUTTON_Y, PUMP_W, BUTTON_H};
    const rect_t stop_box = {STOP_X, BUTTON_Y, STOP_W, BUTTON_H};
    bento_status_t status =
        add_button(screen->ui, "เปิดปั๊ม (กดค้าง)", pump_box, COL_INFO,
                   &screen->routed[ROUTED_PUMP]);

    if (status == BENTO_OK) {
        /* listen() ignores the answer, as modui.c does */
        (void)bento_ui_listen_compat(
            screen->ui, screen->routed[ROUTED_PUMP],
            (uint32_t)1U << BENTO_UI_EVENT_LONG_PRESSED);
        status = add_button(screen->ui, "หยุด", stop_box, COL_BAD,
                            &screen->routed[ROUTED_STOP]);
    }
    return status;
}

/* The roller (watering method, its options joined by newlines), the
 * pump light and its caption. */
static bento_status_t add_mode_and_led(router_screen_t *screen)
{
    const char *modes = "น้ำหยด\n"
                        "พ่นฝอย\n"
                        "สปริงเกอร์";
    const rect_t roller_box = {ROLLER_X, BUTTON_Y, ROLLER_W, ROLLER_H};
    const rect_t led_box = {LED_X, LED_Y, LED_SIZE, LED_SIZE};
    const point_t caption_at = {PUMP_LABEL_X, PUMP_LABEL_Y};
    bento_ui_widget_args_t args;
    bento_ui_id_t unused;
    bento_status_t status;

    place(&args, roller_box);
    args.text = modes;
    args.text_len = (uint32_t)strlen(modes);
    args.color = COL_TEXT;
    status = bento_ui_roller_create(screen->ui, &args,
                                    &screen->routed[ROUTED_MODE]);
    if (status == BENTO_OK) {
        place(&args, led_box);
        args.color = COL_INFO;
        status = bento_ui_led_create(screen->ui, &args, &screen->led);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "ปั๊ม", caption_at, COL_DIM,
                           FONT_TEXT, &unused);
    }
    return status;
}

/* The automatic switch with its caption, and the threshold slider. */
static bento_status_t add_auto_and_slider(router_screen_t *screen)
{
    const rect_t switch_box = {SWITCH_X, SWITCH_Y, SWITCH_W, SWITCH_H};
    const rect_t slider_box = {SLIDER_X, SLIDER_Y, SLIDER_W, SLIDER_H};
    const point_t caption_at = {AUTO_LABEL_X, AUTO_LABEL_Y};
    const point_t soil_at = {MARGIN_X, SOIL_Y};
    bento_ui_widget_args_t args;
    bento_ui_id_t unused;
    bento_status_t status;

    place(&args, switch_box);
    status = bento_ui_switch_create(screen->ui, &args,
                                    &screen->routed[ROUTED_AUTO]);
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "อัตโนมัติ", caption_at, COL_DIM,
                           FONT_TEXT, &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", soil_at, COL_TEXT,
                           FONT_TEXT, &screen->soil);
    }
    if (status == BENTO_OK) {
        place(&args, slider_box);
        args.min_val = 0;
        args.max_val = PERCENT;
        args.font_px = THRESHOLD_START;
        status = bento_ui_slider_create(
            screen->ui, &args, &screen->routed[ROUTED_THRESHOLD]);
    }
    return status;
}

static bento_status_t add_lines(router_screen_t *screen)
{
    const point_t last_at = {MARGIN_X, LAST_Y};
    const point_t note_at = {MARGIN_X, NOTE_Y};
    const point_t help_at = {MARGIN_X, HELP_Y};
    bento_status_t status = add_label(
        screen->ui, " ", last_at, COL_WARN, FONT_TITLE, &screen->last);

    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", note_at, COL_TEXT,
                           FONT_TEXT, &screen->note);
    }
    if (status == BENTO_OK) {
        status =
            add_label(screen->ui, "แตะ = ถาม  กดค้าง = เปิด  หยุด = ปิด",
                      help_at, COL_DIM, FONT_TEXT, &screen->help);
    }
    return status;
}

static bento_status_t build_screen(router_screen_t *screen)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    const point_t title_at = {MARGIN_X, TITLE_Y};
    bento_ui_id_t unused;
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = sleep_ms((uint32_t)SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "เหตุการณ์ -> ตาราง -> ตัวจัดการ",
                           title_at, COL_TEXT, FONT_TITLE, &unused);
    }
    if (status == BENTO_OK) {
        status = add_buttons(screen);
    }
    if (status == BENTO_OK) {
        status = add_mode_and_led(screen);
    }
    if (status == BENTO_OK) {
        status = add_auto_and_slider(screen);
    }
    if (status == BENTO_OK) {
        status = add_lines(screen);
    }
    if (status == BENTO_OK) {
        (void)poll_events(screen->ui, events); /* nothing to read yet */
    }
    return status;
}

/* The pump or the mode changed: the screen light, the switch, the
 * light matrix and the sound follow the real state. The switch is
 * moved too when the board changed the mode itself; value() sends no
 * toggled back, so no command repeats. */
static bento_status_t show_pump(const router_t *router)
{
    const pump_state_t *state = &router->state;
    bento_ui_t *ui = router->screen.ui;
    bento_status_t status;

    /* the widget setters ignore a refusal, as modui.c does */
    (void)bento_ui_set_value(ui, router->screen.led,
                             state->pump ? 1 : 0);
    (void)bento_ui_set_value(ui, router->screen.routed[ROUTED_AUTO],
                             state->automatic ? 1 : 0);
    status = draw_matrix(state->pump, state->mode);
    if ((status == BENTO_OK) && router->shown.known &&
        (state->pump != router->shown.pump)) {
        status = beep(ui, state->pump ? TUNE_START : TUNE_STOP);
    }
    return status;
}

/* One event through the table; a note on screen, and a tap sound
 * when the pump did not change (show_pump sounds that change). */
static bento_status_t handle_event(router_t *router,
                                   const bento_ui_event_t *event)
{
    const bool was_running = router->state.pump;
    const char *note =
        run_command(&router->state, route(router->screen.routed, event),
                    event->value);
    bento_status_t status = BENTO_OK;

    if (note != NULL) {
        set_text(router->screen.ui, router->screen.note, note);
        if (router->state.pump == was_running) {
            status = beep(router->screen.ui, TUNE_TAP);
        }
    }
    return status;
}

/* Polls until the queue is empty; the last event is written once,
 * since a dragged slider sends many a round. */
static bento_status_t drain_events(router_t *router)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    bento_ui_event_t last;
    bool any = false;
    uint32_t count = poll_events(router->screen.ui, events);
    uint32_t i;
    bento_status_t status = BENTO_OK;

    (void)memset(&last, 0, sizeof(last));
    while ((count != 0U) && (status == BENTO_OK)) {
        for (i = 0U; (i < count) && (status == BENTO_OK); i++) {
            status = handle_event(router, &events[i]);
            last = events[i];
            any = true;
        }
        count = poll_events(router->screen.ui, events);
    }
    if ((status == BENTO_OK) && any) {
        char text[TEXT_BYTES];

        (void)snprintf(text, sizeof(text), "%s = %" PRId32,
                       event_name(last.type), last.value);
        set_text(router->screen.ui, router->screen.last, text);
    }
    return status;
}

/* Every TICK_MS: the soil (VR1) and the threshold, rewritten only
 * when either changed. */
static bento_status_t tick(router_t *router, uint32_t now)
{
    char text[TEXT_BYTES];
    soil_line_t *shown = &router->soil_shown;
    bento_status_t status = BENTO_OK;

    if (bento_time_ticks_diff(now, router->tick_ms) >= TICK_MS) {
        router->tick_ms = now;
        status = read_percent(&router->soil);
        if ((status == BENTO_OK) &&
            (!shown->known || (shown->soil != router->soil) ||
             (shown->threshold != router->state.threshold))) {
            (void)snprintf(text, sizeof(text),
                           "ดิน %" PRId32 " %%  เกณฑ์ %" PRId32 " %%",
                           router->soil, router->state.threshold);
            set_text(router->screen.ui, router->screen.soil, text);
            shown->known = true;
            shown->soil = router->soil;
            shown->threshold = router->state.threshold;
        }
    }
    return status;
}

/* Acts and shows only when the state changed. */
static bento_status_t follow_state(router_t *router)
{
    pump_state_t *state = &router->state;
    picture_t *shown = &router->shown;
    bento_status_t status = BENTO_OK;

    if (state->automatic) {
        state->pump = router->soil < state->threshold;
    }
    if (!shown->known || (state->pump != shown->pump) ||
        (state->mode != shown->mode) ||
        (state->automatic != shown->automatic)) {
        status = show_pump(router);
        shown->known = true;
        shown->pump = state->pump;
        shown->mode = state->mode;
        shown->automatic = state->automatic;
    }
    return status;
}

static bento_status_t run_loop(router_t *router)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    router->tick_ms = start_ms;
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = drain_events(router);
        if (status == BENTO_OK) {
            status = tick(router, bento_time_ticks_ms());
        }
        if (status == BENTO_OK) {
            status = follow_state(router);
        }
        if (status == BENTO_OK) {
            status = sleep_ms((uint32_t)LOOP_MS);
        }
    }
    return status;
}

/* The light matrix goes dark and the closing line shows. */
static bento_status_t finish(const router_t *router)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    const bento_status_t status = draw_matrix(false, 0);

    (void)bento_ui_set_color(router->screen.ui, router->screen.help,
                             COL_WARN);
    set_text(router->screen.ui, router->screen.help,
             "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่");
    (void)poll_events(router->screen.ui, events);
    return status;
}

int main(void)
{
    router_t router;
    bento_status_t status;

    (void)memset(&router, 0, sizeof(router));
    router.screen.ui = bento_runtime_get_ui();
    router.state.threshold = THRESHOLD_START;
    router.soil = SOIL_START;
    /* a board without ui.volume answers BENTO_E_UNSUPPORTED and is
     * skipped as the .py skips it; elsewhere it waits for no answer */
    (void)bento_ui_sfx_volume_set(router.screen.ui, SPEAKER);
    /* the table is checked first; a failed case ends the program
     * before the screen opens */
    status =
        self_test() ? build_screen(&router.screen) : BENTO_E_CANCELLED;
    if (status == BENTO_OK) {
        status = run_loop(&router);
    }
    if (status == BENTO_OK) {
        status = finish(&router);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
