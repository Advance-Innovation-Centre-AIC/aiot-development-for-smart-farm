/*
 * cp4_05_hmi_page_pattern.c -- หลักการ 4.5: หน้าจอแผงควบคุม (HMI)
 * (An HMI page: a status bar always in sight, a dangerous command is
 * confirmed)
 *
 * แผงควบคุมที่ดีมี 4 กฎ: แถบสถานะอยู่นอกแท็บจึงเห็นทุกหน้า แยกหน้าตามงาน
 * (ภาพรวม สั่งงาน บันทึก) คำสั่งอันตราย (เปิดปั๊ม) ต้องยืนยันก่อน ส่วน "หยุด"
 * ทำทันที และทุกเหตุการณ์ลงบันทึกพร้อมเวลาและมีเสียงหนึ่งครั้ง แตะแท็บ
 * "สั่งงาน" แล้วแตะ "เปิดปั๊ม" กล่องยืนยันโผล่ "ยืนยัน" = ปั๊มเดิน "ยกเลิก" =
 * ไม่เดิน หมุน VR2 (น้ำในถัง) ต่ำกว่า 20 % ไฟเตือนติด ปั๊มหยุดเอง และเปิด
 * ไม่ได้จนน้ำกลับมาถึง 25 % แท็บ "บันทึก" เก็บ 6 แถวแล้ววนทับแถวเก่าสุด
 * เล่น 3 นาทีแล้วจบเอง
 *
 * A good control panel keeps four rules: the status bar sits outside
 * the tabs, so every page shows it; pages are split by job (overview,
 * control, log); a dangerous command (start the pump) is confirmed
 * first, while "stop" acts at once; every event is logged with its
 * time and one sound. Tap the "control" tab, then "start pump": the
 * confirm box appears, "confirm" runs the pump, "cancel" does not.
 * Turn VR2 (water in the tank) below 20 %: the alarm light comes on,
 * the pump stops by itself and cannot start until the water is back
 * at 25 %. The "log" tab keeps 6 rows, then overwrites the oldest.
 * The program ends after three minutes.
 *
 * Twin of cp4_05_hmi_page_pattern.py
 * (aiot-development-for-smart-farm/core/cp4_05_hmi_page_pattern.py).
 * Style: CLEAN_C.md.
 * Differs from the original:
 *   - ui.volume has no hasattr test: a board without it answers
 *     BENTO_E_UNSUPPORTED, which is skipped as the .py skips the line.
 *   - The router is a static const table of (widget, BENTO_UI_EVENT_*,
 *     command); events are compared by enum and id, never by name.
 *   - decide() answers an entry of a static const table (text and
 *     tune) instead of a tuple; "what the screen shows" is a struct
 *     of numbers where -1 means not shown yet, instead of a dict.
 *   - The table's next row, which ui.Table counts by itself, is kept
 *     by the program (bento_ui_add_row takes the row number).
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
#include "bento_runtime.h"
#include "bento_time.h"

/* Settings a student may change. */
enum {
    TANK_MIN = 20,   /* below this %: alarm, pump stops, no start */
    TANK_HYST = 5,   /* the alarm clears only at TANK_MIN + 5 */
    LOG_ROWS = 6,    /* log rows, then the oldest is overwritten */
    TAB_BAR_H = 44,  /* tab bar height, tall enough for a finger */
    LOOP_MS = 30,    /* read the screen's events this often */
    TICK_MS = 500,   /* read the knob this often */
    RUN_MS = 180000, /* the program ends after three minutes */
    SPEAKER = 40,    /* speaker volume, 0-100 % */
    VOLUME = 25      /* tone velocity, 0-127 (about 20 %) */
};

/* Knob and log. */
enum {
    PERCENT = 100,
    ADC_FULL = 4095, /* pots.read() at the right end */
    VR2 = 1,
    MS_PER_S = 1000,
    TAB_OVERVIEW = 0,
    SEEN_NONE = -1, /* not on screen yet */
    TIME_COLUMN_W = 470
};

/* Sound. */
enum {
    NOTE_MS = 90,
    NOTE_GAP_MS = 100,
    TUNE_NOTES_MAX = 3
};

/* The tunes this program plays. */
typedef enum {
    TUNE_TAP = 0,
    TUNE_START,
    TUNE_STOP,
    TUNE_GOOD,
    TUNE_BAD,
    TUNE_EMPTY
} tune_t;

/* Screen layout. */
enum {
    MARGIN_X = 12,
    BAR_LED_Y = 8,
    BAR_TEXT_Y = 10,
    BAR_LED_SIZE = 28,
    PUMP_TEXT_X = 50,
    TANK_TEXT_X = 240,
    ALARM_X = 440,
    ALARM_TEXT_X = 478,
    TABS_Y = 52,
    TABS_W = 768,
    TABS_H = 286,
    LEVEL_W = 70,
    LEVEL_H = 200,
    BIG_X = 100,
    BIG_Y = 80,
    COMMAND_W = 220,
    COMMAND_H = 80,
    STOP_Y = 100,
    LOG_W = 600,
    LOG_H = 230,
    LOG_COLUMNS = 2,
    BOX_X = 300,
    BOX_Y = 100,
    BOX_W = 440,
    BOX_H = 210,
    ANSWER_Y = 226,
    ANSWER_W = 190,
    ANSWER_H = 64,
    YES_X = 320,
    NO_X = 530,
    HELP_Y = 352,
    FONT_TEXT = 20,
    FONT_BIG = 28,
    SETTLE_MS = 200, /* let the cleared screen settle */
    TEXT_BYTES = 48, /* "น้ำในถัง (VR2) %d %%" with any int32_t */
    CELLS_BYTES = 96 /* a time and an event, NUL between them */
};

/* Colours, 0xRRGGBB. */
enum {
    COL_TEXT = 0xE8EAED,
    COL_DIM = 0x9AA3AF,
    COL_OK = 0x30A46C,
    COL_BAD = 0xE5484D,
    COL_INFO = 0x4A9EFF
};

/* The commands the decision knows. */
typedef enum {
    COMMAND_NONE = 0,
    COMMAND_TANK,
    COMMAND_TAB,
    COMMAND_STOP,
    COMMAND_OPEN,
    COMMAND_YES,
    COMMAND_NO
} command_t;

/* What one decision writes in the log. */
typedef enum {
    ENTRY_NONE = 0,
    ENTRY_WATER_OK,
    ENTRY_TANK_LOW,
    ENTRY_STOPPED,
    ENTRY_NO_START,
    ENTRY_ASKED,
    ENTRY_STARTED,
    ENTRY_CANCELLED
} entry_t;

typedef struct {
    const char *text;
    tune_t tune;
} log_entry_t;

/* The widgets the router knows, by their place in screen.routed. */
typedef enum {
    ROUTED_OPEN = 0,
    ROUTED_YES,
    ROUTED_NO,
    ROUTED_STOP,
    ROUTED_TABS,
    ROUTED_COUNT
} routed_t;

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
    bento_ui_id_t pump_led;
    bento_ui_id_t pump;
    bento_ui_id_t tank;
    bento_ui_id_t alarm;
    bento_ui_id_t level;
    bento_ui_id_t big;
    bento_ui_id_t log;
    bento_ui_id_t box;
    bento_ui_id_t help;
    uint8_t next_row; /* the row the next add_row writes */
} hmi_screen_t;

/* All the state; the screen is only a picture of it. ask: the
 * confirm box is open; low: the tank alarm; tab: the page shown. */
typedef struct {
    bool pump;
    int32_t tank;
    bool low;
    bool ask;
    int32_t tab;
    int32_t logged;
    uint32_t start_ms;
} hmi_state_t;

/* What the screen shows now, SEEN_NONE before the first update. */
typedef struct {
    int32_t pump;
    int32_t tank;
    int32_t low;
    int32_t level;
    int32_t ask;
} seen_t;

typedef struct {
    hmi_screen_t screen;
    hmi_state_t state;
    seen_t seen;
} hmi_t;

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

/* Plays a tune note by note, 100 ms apart. */
static bento_status_t beep(bento_ui_t *ui, tune_t tune)
{
    static const uint8_t notes[][TUNE_NOTES_MAX] = {
        {76U, 0U, 0U},   {72U, 79U, 0U}, {79U, 72U, 0U},
        {72U, 79U, 84U}, {84U, 76U, 0U}, {84U, 76U, 69U}};
    static const uint32_t note_count[] = {1U, 2U, 2U, 3U, 2U, 3U};
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

static bento_status_t read_tank(int32_t *percent)
{
    uint16_t raw = 0U;
    const bento_status_t status = bento_pots_read(VR2, &raw);

    *percent = ((int32_t)raw * PERCENT) / ADC_FULL;
    return status;
}

/* A new tank reading; the alarm clears only TANK_HYST above
 * TANK_MIN, so a knob at the edge does not flutter. */
static entry_t decide_tank(hmi_state_t *state, int32_t tank)
{
    const int32_t limit = TANK_MIN + (state->low ? TANK_HYST : 0);
    const bool low = tank < limit;
    entry_t entry = ENTRY_NONE;

    state->tank = tank;
    if (low != state->low) {
        state->low = low;
        entry = ENTRY_WATER_OK;
        if (low) {
            /* the pump stops itself and the open box closes */
            state->ask = false;
            state->pump = false;
            entry = ENTRY_TANK_LOW;
        }
    }
    return entry;
}

/* "yes" and "no" count only while the box is open; a late answer
 * is ignored. */
static entry_t decide_answer(hmi_state_t *state, command_t command)
{
    entry_t entry = ENTRY_NONE;

    if (((command == COMMAND_YES) || (command == COMMAND_NO)) &&
        state->ask) {
        state->ask = false;
        entry = ENTRY_CANCELLED;
        if (command == COMMAND_YES) {
            state->pump = true;
            entry = ENTRY_STARTED;
        }
    }
    return entry;
}

/* The one gate every change of the state passes: the screen's
 * commands and the tank read every TICK_MS. */
static entry_t decide(hmi_state_t *state, command_t command,
                      int32_t value)
{
    entry_t entry = ENTRY_NONE;

    if (command == COMMAND_TANK) {
        entry = decide_tank(state, value);
    } else if (command == COMMAND_TAB) {
        state->tab = value; /* the page number, from 0 */
    } else if (command == COMMAND_STOP) {
        /* stop beats everything and needs no question */
        state->ask = false;
        state->pump = false;
        entry = ENTRY_STOPPED;
    } else if (command == COMMAND_OPEN) {
        /* the safety rule is in the decision: no question at all */
        entry = ENTRY_NO_START;
        if (!state->low) {
            state->ask = true;
            entry = ENTRY_ASKED;
        }
    } else {
        entry = decide_answer(state, command);
    }
    return entry;
}

static void place(bento_ui_widget_args_t *args, rect_t box,
                  bento_ui_id_t parent)
{
    bento_ui_widget_args_init(args);
    args->x = box.x;
    args->y = box.y;
    args->w = box.w;
    args->h = box.h;
    args->parent = parent;
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

    place(&args, box, 0U);
    args.text = text;
    args.text_len = (uint32_t)strlen(text);
    args.color = color;
    args.font_px = FONT_TEXT; /* Thai: 20 px, not the default */
    return bento_ui_button_create(ui, &args, out);
}

static bento_status_t add_led(bento_ui_t *ui, int16_t x, uint32_t color,
                              bento_ui_id_t *out)
{
    const rect_t box = {x, BAR_LED_Y, BAR_LED_SIZE, BAR_LED_SIZE};
    bento_ui_widget_args_t args;

    place(&args, box, 0U);
    args.color = color;
    return bento_ui_led_create(ui, &args, out);
}

/* The status bar sits on the screen, not in a tab, so every page
 * shows it. */
static bento_status_t add_status_bar(hmi_screen_t *screen)
{
    const point_t pump_at = {PUMP_TEXT_X, BAR_TEXT_Y};
    const point_t tank_at = {TANK_TEXT_X, BAR_TEXT_Y};
    const point_t alarm_at = {ALARM_TEXT_X, BAR_TEXT_Y};
    bento_ui_id_t unused;
    bento_status_t status =
        add_led(screen->ui, MARGIN_X, COL_OK, &screen->pump_led);

    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", pump_at, COL_TEXT,
                           FONT_TEXT, &screen->pump);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", tank_at, COL_TEXT,
                           FONT_TEXT, &screen->tank);
    }
    if (status == BENTO_OK) {
        status = add_led(screen->ui, ALARM_X, COL_BAD, &screen->alarm);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "เตือน", alarm_at, COL_DIM,
                           FONT_TEXT, &unused);
    }
    return status;
}

static bento_status_t add_page(const hmi_screen_t *screen,
                               const char *name, bento_ui_id_t *page)
{
    return bento_ui_add_tab(screen->ui, screen->routed[ROUTED_TABS],
                            name, (uint32_t)strlen(name), page);
}

/* Overview, to look at only: a Bar taller than wide fills from the
 * bottom like a tank (an Arc could be dragged by a finger). */
static bento_status_t add_overview(hmi_screen_t *screen)
{
    const rect_t level_box = {0, 0, LEVEL_W, LEVEL_H};
    bento_ui_widget_args_t args;
    bento_ui_id_t page;
    bento_status_t status = add_page(screen, "ภาพรวม", &page);

    if (status == BENTO_OK) {
        place(&args, level_box, page);
        args.min_val = 0;
        args.max_val = PERCENT;
        status = bento_ui_bar_create(screen->ui, &args, &screen->level);
    }
    if (status == BENTO_OK) {
        bento_ui_widget_args_init(&args);
        args.text = " ";
        args.text_len = 1U;
        args.x = BIG_X;
        args.y = BIG_Y;
        args.color = COL_TEXT;
        args.font_px = FONT_BIG;
        args.parent = page;
        status = bento_ui_label_create(screen->ui, &args, &screen->big);
    }
    return status;
}

static bento_status_t add_page_button(bento_ui_t *ui, const char *text,
                                      rect_t box, uint32_t color,
                                      bento_ui_id_t page,
                                      bento_ui_id_t *out)
{
    bento_ui_widget_args_t args;

    place(&args, box, page);
    args.text = text;
    args.text_len = (uint32_t)strlen(text);
    args.color = color;
    args.font_px = FONT_TEXT; /* Thai: 20 px, not the default */
    return bento_ui_button_create(ui, &args, out);
}

static bento_status_t add_controls(hmi_screen_t *screen)
{
    const rect_t open_box = {0, 0, COMMAND_W, COMMAND_H};
    const rect_t stop_box = {0, STOP_Y, COMMAND_W, COMMAND_H};
    bento_ui_id_t page;
    bento_status_t status = add_page(screen, "สั่งงาน", &page);

    if (status == BENTO_OK) {
        status =
            add_page_button(screen->ui, "เปิดปั๊ม", open_box, COL_INFO,
                            page, &screen->routed[ROUTED_OPEN]);
    }
    if (status == BENTO_OK) {
        status = add_page_button(screen->ui, "หยุด", stop_box, COL_BAD,
                                 page, &screen->routed[ROUTED_STOP]);
    }
    return status;
}

/* One table row: a time and an event, the two cells joined by a
 * NUL as bento_ui_add_row takes them. */
static void add_log_row(hmi_screen_t *screen, const char *first,
                        const char *second)
{
    char cells[CELLS_BYTES];
    const size_t first_len = strlen(first) + 1U;
    size_t length = 0U;

    if (first_len < sizeof(cells)) {
        (void)memcpy(cells, first, first_len);
        (void)snprintf(&cells[first_len], sizeof(cells) - first_len,
                       "%s", second);
        length = first_len + strlen(&cells[first_len]);
    }
    /* add_row sends and waits for no answer (modui.c) */
    (void)bento_ui_add_row(screen->ui, screen->log, screen->next_row,
                           cells, (uint32_t)length);
    screen->next_row++;
}

/* The log page: row 0 is the heading; column 0 keeps the default
 * 130 px width. */
static bento_status_t add_log_page(hmi_screen_t *screen)
{
    const rect_t log_box = {0, 0, LOG_W, LOG_H};
    bento_ui_widget_args_t args;
    bento_ui_id_t page;
    bento_status_t status = add_page(screen, "บันทึก", &page);

    if (status == BENTO_OK) {
        place(&args, log_box, page);
        args.min_val = LOG_COLUMNS;
        args.max_val = LOG_ROWS + 1;
        status = bento_ui_table_create(screen->ui, &args, &screen->log);
    }
    if (status == BENTO_OK) {
        /* a table's setters ignore a refusal, as modui.c does */
        (void)bento_ui_set_col_width(screen->ui, screen->log, 1,
                                     TIME_COLUMN_W);
        add_log_row(screen, "เวลา", "เหตุการณ์");
    }
    return status;
}

static bento_status_t add_tabs(hmi_screen_t *screen)
{
    const rect_t tabs_box = {MARGIN_X, TABS_Y, TABS_W, TABS_H};
    bento_ui_widget_args_t args;
    bento_status_t status;

    /* the tabs end at y = 338: the bottom right is the Console
     * button */
    place(&args, tabs_box, 0U);
    args.font_px = TAB_BAR_H;
    status = bento_ui_tabview_create(screen->ui, &args,
                                     &screen->routed[ROUTED_TABS]);
    if (status == BENTO_OK) {
        status = add_overview(screen);
    }
    if (status == BENTO_OK) {
        status = add_controls(screen);
    }
    if (status == BENTO_OK) {
        status = add_log_page(screen);
    }
    return status;
}

/* The confirm box floats over the tabs and the answer buttons over
 * the box; all three hidden until the box asks. The text says what
 * will happen, not just "are you sure". */
static bento_status_t add_confirm(hmi_screen_t *screen)
{
    const char *question = "เปิดปั๊ม?\nปั๊มจะเดินทันที";
    const rect_t box = {BOX_X, BOX_Y, BOX_W, BOX_H};
    const rect_t yes_box = {YES_X, ANSWER_Y, ANSWER_W, ANSWER_H};
    const rect_t no_box = {NO_X, ANSWER_Y, ANSWER_W, ANSWER_H};
    bento_ui_widget_args_t args;
    bento_status_t status;

    place(&args, box, 0U);
    args.text = question;
    args.text_len = (uint32_t)strlen(question);
    status = bento_ui_msgbox_create(screen->ui, &args, &screen->box);
    if (status == BENTO_OK) {
        status = add_button(screen->ui, "ยืนยัน", yes_box, COL_OK,
                            &screen->routed[ROUTED_YES]);
    }
    if (status == BENTO_OK) {
        status = add_button(screen->ui, "ยกเลิก", no_box, COL_DIM,
                            &screen->routed[ROUTED_NO]);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_visible(screen->ui, screen->box, 0U);
        (void)bento_ui_set_visible(screen->ui,
                                   screen->routed[ROUTED_YES], 0U);
        (void)bento_ui_set_visible(screen->ui,
                                   screen->routed[ROUTED_NO], 0U);
    }
    return status;
}

/* Builds everything once (18 of the firmware's 64 widgets). */
static bento_status_t build(hmi_screen_t *screen)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    const point_t help_at = {MARGIN_X, HELP_Y};
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = sleep_ms((uint32_t)SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_status_bar(screen);
    }
    if (status == BENTO_OK) {
        status = add_tabs(screen);
    }
    if (status == BENTO_OK) {
        status = add_confirm(screen);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", help_at, COL_DIM, FONT_TEXT,
                           &screen->help);
    }
    if (status == BENTO_OK) {
        (void)poll_events(screen->ui, events); /* nothing to read yet */
    }
    return status;
}

/* Is this value on screen already? If not, remember it and say so,
 * so the caller sends it. */
static bool changed(int32_t *seen, int32_t value)
{
    const bool different = *seen != value;

    *seen = value;
    return different;
}

static void show_text(bento_ui_t *ui, bento_ui_id_t widget,
                      const char *format, int32_t value)
{
    char text[TEXT_BYTES];

    (void)snprintf(text, sizeof(text), format, value);
    set_text(ui, widget, text);
}

/* The status bar, seen from every page. */
static void update_status_bar(const hmi_screen_t *screen,
                              const hmi_state_t *state, seen_t *seen)
{
    /* the widget setters ignore a refusal, as modui.c does */
    if (changed(&seen->pump, state->pump ? 1 : 0)) {
        (void)bento_ui_set_value(screen->ui, screen->pump_led,
                                 state->pump ? 1 : 0);
        set_text(screen->ui, screen->pump,
                 state->pump ? "ปั๊มเดิน" : "ปั๊มหยุด");
    }
    if (changed(&seen->tank, state->tank)) {
        show_text(screen->ui, screen->tank, "ถัง %" PRId32 " %%",
                  state->tank);
    }
    if (changed(&seen->low, state->low ? 1 : 0)) {
        (void)bento_ui_set_value(screen->ui, screen->alarm,
                                 state->low ? 1 : 0);
    }
}

/* The confirm box and its two answers show and hide together. */
static void show_confirm(const hmi_screen_t *screen, bool ask)
{
    const bento_ui_id_t parts[] = {screen->box,
                                   screen->routed[ROUTED_YES],
                                   screen->routed[ROUTED_NO]};
    uint32_t i;

    for (i = 0U; i < (sizeof(parts) / sizeof(parts[0])); i++) {
        (void)bento_ui_set_visible(screen->ui, parts[i], ask ? 1U : 0U);
    }
}

/* Sends the state to the screen, only the parts that changed. */
static void update(hmi_t *hmi)
{
    const hmi_state_t *state = &hmi->state;
    const hmi_screen_t *screen = &hmi->screen;
    seen_t *seen = &hmi->seen;

    update_status_bar(screen, state, seen);
    /* the overview is refreshed only while it is the page shown */
    if ((state->tab == TAB_OVERVIEW) &&
        changed(&seen->level, state->tank)) {
        (void)bento_ui_set_value(screen->ui, screen->level,
                                 state->tank);
        show_text(screen->ui, screen->big,
                  "น้ำในถัง (VR2) %" PRId32 " %%", state->tank);
    }
    if (changed(&seen->ask, state->ask ? 1 : 0)) {
        show_confirm(screen, state->ask);
    }
}

/* One event = one log row and one sound; once the rows are full,
 * the oldest row is overwritten cell by cell. */
static bento_status_t write_log(hmi_t *hmi, entry_t entry)
{
    /* One row per entry_t; ENTRY_NONE logs nothing. */
    static const log_entry_t entries[] = {{"", TUNE_TAP},
                                          {"น้ำพอ", TUNE_GOOD},
                                          {"ถังต่ำ ปั๊มหยุด", TUNE_EMPTY},
                                          {"หยุดปั๊ม", TUNE_STOP},
                                          {"ถังต่ำ ห้ามเปิด", TUNE_BAD},
                                          {"ถามยืนยัน", TUNE_TAP},
                                          {"เปิดปั๊ม", TUNE_START},
                                          {"ยกเลิก", TUNE_TAP}};
    char when[TEXT_BYTES];
    hmi_state_t *state = &hmi->state;
    bento_status_t status = BENTO_OK;

    if (entry != ENTRY_NONE) {
        const char *text = entries[entry].text;
        const int32_t elapsed_s =
            bento_time_ticks_diff(bento_time_ticks_ms(),
                                  state->start_ms) /
            MS_PER_S;

        (void)snprintf(when, sizeof(when), "%" PRId32 " วิ", elapsed_s);
        if (state->logged < LOG_ROWS) {
            add_log_row(&hmi->screen, when, text);
        } else {
            const int32_t oldest =
                1 + (state->logged % (int32_t)LOG_ROWS);
            const uint8_t row = (uint8_t)oldest;

            /* cell() sends and waits for no answer (modui.c) */
            (void)bento_ui_set_cell(hmi->screen.ui, hmi->screen.log,
                                    row, 0U, when,
                                    (uint32_t)strlen(when));
            (void)bento_ui_set_cell(hmi->screen.ui, hmi->screen.log,
                                    row, 1U, text,
                                    (uint32_t)strlen(text));
        }
        state->logged++;
        status = beep(hmi->screen.ui, entries[entry].tune);
    }
    return status;
}

/* Looks the pair (widget, type) up in the table; not in it = no
 * command, and the decision then does nothing. */
static command_t route(const hmi_screen_t *screen,
                       const bento_ui_event_t *event)
{
    /* (widget, event type) -> the command the decision knows. */
    static const route_t routes[] = {
        {ROUTED_OPEN, BENTO_UI_EVENT_CLICKED, COMMAND_OPEN},
        {ROUTED_YES, BENTO_UI_EVENT_CLICKED, COMMAND_YES},
        {ROUTED_NO, BENTO_UI_EVENT_CLICKED, COMMAND_NO},
        {ROUTED_STOP, BENTO_UI_EVENT_CLICKED, COMMAND_STOP},
        {ROUTED_TABS, BENTO_UI_EVENT_VALUE_CHANGED, COMMAND_TAB}};
    command_t command = COMMAND_NONE;
    uint32_t i;

    for (i = 0U; i < (sizeof(routes) / sizeof(routes[0])); i++) {
        if (event_is(event, routes[i].type,
                     screen->routed[routes[i].widget])) {
            command = routes[i].command;
        }
    }
    return command;
}

/* Event -> table -> command -> decision -> log, until the queue is
 * empty. */
static bento_status_t drain_events(hmi_t *hmi)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = poll_events(hmi->screen.ui, events);
    uint32_t i;
    bento_status_t status = BENTO_OK;

    while ((count != 0U) && (status == BENTO_OK)) {
        for (i = 0U; (i < count) && (status == BENTO_OK); i++) {
            const command_t command = route(&hmi->screen, &events[i]);

            status = write_log(
                hmi, decide(&hmi->state, command, events[i].value));
        }
        count = poll_events(hmi->screen.ui, events);
    }
    return status;
}

/* VR2 goes through the same gate as the screen's commands. */
static bento_status_t read_tick(hmi_t *hmi, uint32_t *tick_ms)
{
    int32_t tank = 0;
    bento_status_t status = BENTO_OK;

    if (bento_time_ticks_diff(bento_time_ticks_ms(), *tick_ms) >=
        TICK_MS) {
        *tick_ms = bento_time_ticks_ms();
        status = read_tank(&tank);
        if (status == BENTO_OK) {
            status =
                write_log(hmi, decide(&hmi->state, COMMAND_TANK, tank));
        }
    }
    return status;
}

static bento_status_t run_loop(hmi_t *hmi)
{
    uint32_t tick_ms = hmi->state.start_ms;
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(),
                                  hmi->state.start_ms) < RUN_MS)) {
        status = drain_events(hmi);
        if (status == BENTO_OK) {
            status = read_tick(hmi, &tick_ms);
        }
        if (status == BENTO_OK) {
            update(hmi);
            status = sleep_ms((uint32_t)LOOP_MS);
        }
    }
    return status;
}

/* The round ends: the pump stops, the box closes, the closing line
 * shows. */
static void finish(hmi_t *hmi)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];

    hmi->state.ask = false;
    hmi->state.pump = false;
    update(hmi);
    (void)bento_ui_set_color(hmi->screen.ui, hmi->screen.help, COL_BAD);
    set_text(hmi->screen.ui, hmi->screen.help,
             "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่");
    (void)poll_events(hmi->screen.ui, events);
}

static void forget_screen(seen_t *seen)
{
    seen->pump = SEEN_NONE;
    seen->tank = SEEN_NONE;
    seen->low = SEEN_NONE;
    seen->level = SEEN_NONE;
    seen->ask = SEEN_NONE;
}

int main(void)
{
    hmi_t hmi;
    bento_status_t status;

    (void)memset(&hmi, 0, sizeof(hmi));
    hmi.screen.ui = bento_runtime_get_ui();
    forget_screen(&hmi.seen);
    /* a board without ui.volume answers BENTO_E_UNSUPPORTED and is
     * skipped as the .py skips it; elsewhere it waits for no answer */
    (void)bento_ui_sfx_volume_set(hmi.screen.ui, SPEAKER);
    status = build(&hmi.screen);
    if (status == BENTO_OK) {
        status = read_tank(&hmi.state.tank);
    }
    if (status == BENTO_OK) {
        hmi.state.low = hmi.state.tank < TANK_MIN;
        hmi.state.start_ms = bento_time_ticks_ms();
        status = run_loop(&hmi);
    }
    if (status == BENTO_OK) {
        finish(&hmi);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
