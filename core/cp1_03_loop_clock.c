/*
 * cp1_03_loop_clock.c -- หลักการ 1.3: ลูปเดียว สามจังหวะ โดยไม่มีใครรอใคร
 *
 * งานแต่ละอย่างมีจังหวะของตัวเอง IMU เร็ว (20 Hz) อากาศช้า (1 Hz) จอพอดีตา
 * (2 Hz) ลูปเดียววนถามนาฬิกา ticks_ms() ว่าถึงนัดของงานไหนแล้ว นัดถัดไป =
 * นัดเดิม + คาบ (ticks_add) คาบเฉลี่ยจึงไม่เลื่อน โหมดง่าย (กด SW5) ทำงาน
 * แล้ว sleep_ms(50) ต่อกัน คาบจริงจึงช้ากว่าเป้าและช้าสะสม ดูตาราง เป้า (ms)
 * เทียบกับคาบเฉลี่ย ต่ำสุด-สูงสุด และ "ช้า" กด SW5 (ปุ่มล่าง) สลับโหมด
 * ตัวนับเริ่มใหม่ทุกครั้งที่สลับ โปรแกรมเล่น 3 นาทีแล้วจบเอง ใช้กับ TESAIoT
 * Dev Kit และ BENTO Emulator (เวลาใน Emulator ไม่ใช่เวลาของบอร์ด)
 *
 * One loop, three rhythms, and nobody waits for anybody: the IMU at
 * 20 Hz, the climate at 1 Hz and the screen at 2 Hz. The loop asks the
 * clock which task is due and runs only that one; the next appointment
 * is the old one plus the period, so the average period does not
 * drift. SW5 (lower button) switches to the naive mode, work then
 * sleep_ms(50), where every task runs late and the lateness adds up.
 * The table shows the target, the measured average, min-max and how
 * often a task started late. It runs for three minutes.
 *
 * Twin of cp1_03_loop_clock.py (aiot-development-for-smart-farm/core/cp1_03_loop_clock.py). Style: CLEAN_C.md.
 * What C does differently:
 *   - the Button and Task classes are structs with small functions;
 *     None (not started yet, not read) is a bool beside the value;
 *   - try/except around a sensor is a status check with the same
 *     fallback: no value, or three tries 50 ms apart for the SHT40;
 *   - the magnitude's x ** 0.5 is a correctly rounded square root,
 *     which may differ from the board's powf in the last bit;
 *   - real numbers are bento_real_t, the board MicroPython's float,
 *     and their text is MicroPython's own (bento_fmt_real_precision);
 *   - the cells dict is a small table of the texts each cell shows;
 *     floor division of the non-negative sums is C's division;
 *   - the TUNES dict is a table indexed by an enum;
 *   - hasattr(ui, "volume") is not asked: the volume call is always
 *     made, and a firmware without it refuses it, which is ignored;
 *   - ui.tone() is fire-and-forget in MicroPython, so its status is
 *     ignored here too;
 *   - Stop ends the program at the next sleep and main returns 0,
 *     without the closing line (MicroPython's KeyboardInterrupt).
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_buttons.h"
#include "bento_fmt.h"
#include "bento_runtime.h"
#include "bento_sensors.h"
#include "bento_time.h"

/* Settings a student may change. */
enum {
    IMU_MS = 50, /* read the IMU every 50 ms = 20 times a second */
    CLIMATE_MS = 1000, /* read the SHT40 every 1000 ms */
    SCREEN_MS = 500,   /* refresh the table every 500 ms */
    LATE_MS = 10, /* started later than this past the target = late */
    RUN_MS = 180000, /* play for three minutes, then end */
    SPEAKER = 40,    /* speaker level 0-100 % (firmware 2.4.2+) */
    VOLUME = 25      /* tone loudness 0-127 (about 20 %) */
};

/* The board warms itself: compare with a room thermometer and put the
 * difference here, such as -9.5. */
static const bento_real_t TEMP_OFFSET = (bento_real_t)0.0;

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
    MODE_Y = 38,
    VALUES_X = 500,
    TABLE_Y = 64,
    TABLE_W = 768,
    TABLE_H = 272,
    HELP_Y = 352
};

/* Font sizes, in pixels. */
enum {
    FONT_TITLE = 24,
    FONT_TEXT = 20, /* Thai text is 20 px or larger on the board */
    FONT_LATIN = 16 /* digits and Latin only */
};

/* The table and the tasks. */
enum {
    TASK_IMU = 0,
    TASK_CLIMATE = 1,
    TASK_SCREEN = 2,
    TASK_COUNT = 3,
    TABLE_COLS = 6,
    TABLE_ROWS = 4,
    COLUMN_AVERAGE = 2, /* the first column the loop rewrites */
    COLUMN_RANGE = 3,
    COLUMN_LATE = 4,
    COLUMN_WORK = 5,
    LIVE_COLS = 4, /* columns 2-5 change while the program runs */
    LOW_START = 99999
};

/* Sound, timing and sizes that are not settings. */
enum {
    SETTLE_MS = 200,   /* let the cleared screen settle */
    TONE_MS = 90,      /* each note of a tune */
    NOTE_GAP_MS = 100, /* one note to the next */
    TUNE_NOTES = 3,    /* the longest tune */
    SHT40_TRIES = 3,   /* the bus is busy now and then */
    RETRY_MS = 50,     /* pause between two SHT40 tries */
    WAIT_STEP_MS = 20, /* read the button this often while waiting */
    TEXT_BYTES = 128,  /* a SET_TEXT carries at most 126 bytes */
    CELL_BYTES = 24,   /* "%d-%d" of two int32_t, with its NUL */
    ACCEL_DIGITS = 2,
    TEMP_DIGITS = 1,
    SW5 = 1 /* SW5, the lower button, is pressed(1) */
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
static const char *const BUTTON_NAMES[2] = {"SW5", "SW6"};

typedef struct {
    int32_t x;
    int32_t y;
    int32_t font_px;
    uint32_t color;
} label_spec_t;

/* A base-board button that misses no short tap: the firmware's
 * debounce moves only when the button is read, so it is read often
 * and a press is remembered until asked for. */
typedef struct {
    uint32_t index;
    bool down;    /* held down now */
    bool clicked; /* went down since the last question */
} button_t;

/* One task's appointment and the statistics of its measured period. */
typedef struct {
    int32_t period_ms;
    uint32_t due_ms;
    bool started; /* has it run once, so last_ms means something */
    uint32_t last_ms;
    int32_t runs;
    int32_t total_ms;
    int32_t late;
    int32_t low_ms;
    int32_t high_ms;
    int32_t work_ms;
} task_t;

/* The widgets that change, and the text each live cell shows. */
typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t mode;
    bento_ui_id_t values;
    bento_ui_id_t table;
    bento_ui_id_t help;
    char cells[TASK_COUNT][LIVE_COLS][CELL_BYTES];
} screen_t;

/* The latest reading of the IMU and of the temperature. */
typedef struct {
    bool accel_known;
    bento_real_t accel;
    bool temp_known;
    bento_real_t temp;
} readings_t;

typedef struct {
    task_t tasks[TASK_COUNT];
    readings_t readings;
    button_t sw5;
    bool naive;
    int32_t rounds;
} schedule_t;

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

/* The size of the acceleration (m/s2) from the BMI270, about 9.8 when
 * still; no value when it cannot be read. */
static void read_accel(readings_t *readings)
{
    bento_sensors_imu6_t motion;

    bento_sensors_imu6_init(&motion);
    readings->accel_known = false;
    if (bento_sensors_bmi270_motion(&motion) == BENTO_OK) {
        const bento_real_t ax = motion.ax;
        const bento_real_t ay = motion.ay;
        const bento_real_t az = motion.az;
        const bento_real_t sum = (ax * ax) + (ay * ay) + (az * az);

        /* sqrt is correctly rounded, so rounding its double result
         * to the board's float gives sqrtf's value */
        readings->accel = (bento_real_t)sqrt((double)sum);
        readings->accel_known = true;
    }
}

/* The SHT40 temperature with the offset, three tries; a failed try
 * waits 50 ms, which makes the IMU start late (the "late" column). */
static bento_status_t read_temp(readings_t *readings)
{
    bento_status_t status = BENTO_OK;
    uint32_t tries = 0U;

    readings->temp_known = false;
    while ((status == BENTO_OK) && !readings->temp_known &&
           (tries < (uint32_t)SHT40_TRIES)) {
        float celsius = 0.0F;
        float humidity = 0.0F;

        if (bento_sensors_sht40_read(&celsius, &humidity) == BENTO_OK) {
            readings->temp = celsius + TEMP_OFFSET;
            readings->temp_known = true;
        } else {
            status = pause_ms((uint32_t)RETRY_MS);
        }
        tries++;
    }
    return status;
}

static bento_status_t sample(button_t *button)
{
    bool now_down = false;
    bento_status_t status =
        bento_buttons_pressed(button->index, &now_down);

    if ((status == BENTO_OK) && now_down && !button->down) {
        button->clicked = true;
    }
    button->down = now_down;
    return status;
}

/* True once per press: holding it down does not count again. */
static bool pressed_now(button_t *button)
{
    const bool fired = button->clicked;

    button->clicked = false;
    return fired;
}

/* Waits total_ms but reads the button every 20 ms meanwhile, so a short
 * tap is not missed. */
static bento_status_t wait_ms(int32_t total_ms, button_t *button)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = sample(button);
    int32_t left_ms = total_ms - bento_time_ticks_diff(
                                     bento_time_ticks_ms(), start_ms);

    while ((status == BENTO_OK) && (left_ms > 0)) {
        status = pause_ms((uint32_t)((left_ms < WAIT_STEP_MS)
                                         ? left_ms
                                         : WAIT_STEP_MS));
        if (status == BENTO_OK) {
            status = sample(button);
        }
        left_ms = total_ms - bento_time_ticks_diff(
                                 bento_time_ticks_ms(), start_ms);
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

static void reset_task(task_t *task, uint32_t now_ms)
{
    task->due_ms = bento_time_ticks_add(now_ms, task->period_ms);
    task->started = false;
    task->runs = 0;
    task->total_ms = 0;
    task->late = 0;
    task->high_ms = 0;
    task->work_ms = 0;
    task->low_ms = LOW_START;
}

/* Due yet? If so, the next appointment is the old one plus the period;
 * more than one period late skips the missed ones instead of racing. */
static bool is_due(task_t *task, uint32_t now_ms)
{
    bool due = false;

    if (bento_time_ticks_diff(now_ms, task->due_ms) >= 0) {
        task->due_ms =
            bento_time_ticks_add(task->due_ms, task->period_ms);
        if (bento_time_ticks_diff(now_ms, task->due_ms) >= 0) {
            task->due_ms =
                bento_time_ticks_add(now_ms, task->period_ms);
        }
        due = true;
    }
    return due;
}

/* Notes the real start time, then updates the period's statistics. */
static void record(task_t *task, uint32_t start_ms)
{
    if (task->started) {
        const int32_t period_ms =
            bento_time_ticks_diff(start_ms, task->last_ms);

        task->runs++;
        task->total_ms += period_ms;
        if (period_ms < task->low_ms) {
            task->low_ms = period_ms;
        }
        if (period_ms > task->high_ms) {
            task->high_ms = period_ms;
        }
        if (period_ms > (task->period_ms + LATE_MS)) {
            task->late++;
        }
    }
    task->last_ms = start_ms;
    task->started = true;
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

/* MicroPython's widget setters ignore a refusal (modui.c), so the
 * twin does too. */
static void set_text(bento_ui_t *ui, bento_ui_id_t widget,
                     const char *text)
{
    (void)bento_ui_set_text(ui, widget, text, (uint32_t)strlen(text));
}

static void set_cell(const screen_t *screen, uint32_t row, uint32_t col,
                     const char *text)
{
    /* a setter's refusal is ignored, as in MicroPython */
    (void)bento_ui_set_cell(screen->ui, screen->table, (uint8_t)row,
                            (uint8_t)col, text, (uint32_t)strlen(text));
}

/* The fixed cells: the column titles, the task names and targets. */
static void fill_table(const screen_t *screen)
{
    static const int32_t widths[TABLE_COLS] = {150, 100, 120,
                                               150, 90,  158};
    static const char *const titles[TABLE_COLS] = {
        "งาน", "เป้า ms", "เฉลี่ย", "ต่ำ-สูง", "ช้า", "ใช้ ms"};
    static const char *const rows[TASK_COUNT] = {"IMU 20Hz",
                                                 "อากาศ 1Hz", "จอ 2Hz"};
    static const int32_t targets[TASK_COUNT] = {IMU_MS, CLIMATE_MS,
                                                SCREEN_MS};
    char text[CELL_BYTES];
    uint32_t i;

    for (i = 0U; i < (uint32_t)TABLE_COLS; i++) {
        /* a setter's refusal is ignored, as in MicroPython */
        (void)bento_ui_set_col_width(screen->ui, screen->table,
                                     (int32_t)i, widths[i]);
    }
    for (i = 0U; i < (uint32_t)TABLE_COLS; i++) {
        set_cell(screen, 0U, i, titles[i]);
    }
    for (i = 0U; i < (uint32_t)TASK_COUNT; i++) {
        set_cell(screen, i + 1U, 0U, rows[i]);
        (void)snprintf(text, sizeof(text), "%ld", (long)targets[i]);
        set_cell(screen, i + 1U, 1U, text);
    }
}

static bento_status_t add_table(screen_t *screen)
{
    bento_ui_widget_args_t args;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = MARGIN_X;
    args.y = TABLE_Y;
    args.w = TABLE_W;
    args.h = TABLE_H;
    args.min_val = TABLE_COLS;
    args.max_val = TABLE_ROWS;
    status = bento_ui_table_create(screen->ui, &args, &screen->table);
    if (status == BENTO_OK) {
        fill_table(screen);
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

static bento_status_t add_help(screen_t *screen)
{
    const label_spec_t help = {MARGIN_X, HELP_Y, FONT_TEXT, COL_DIM};
    char text[TEXT_BYTES];

    (void)snprintf(text, sizeof(text),
                   "ช้า = ช้ากว่าเป้าเกิน %ld ms   %s (ล่าง) = สลับโหมด",
                   (long)LATE_MS, BUTTON_NAMES[0]);
    return add_label(screen->ui, text, &help, &screen->help);
}

static bento_status_t build_screen(screen_t *screen)
{
    const label_spec_t title = {MARGIN_X, TITLE_Y, FONT_TITLE,
                                COL_TEXT};
    const label_spec_t mode = {MARGIN_X, MODE_Y, FONT_TEXT, COL_OK};
    const label_spec_t values = {VALUES_X, MODE_Y, FONT_LATIN,
                                 COL_INFO};
    bento_ui_id_t unused;
    bento_status_t status =
        bento_ui_screen(screen->ui, BENTO_RUNTIME_SCREEN_WIDTH_PX,
                        BENTO_RUNTIME_SCREEN_HEIGHT_PX);

    if (status == BENTO_OK) {
        status = pause_ms((uint32_t)SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status =
            add_label(screen->ui, "ลูปเดียว สามจังหวะ", &title, &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", &mode, &screen->mode);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", &values, &screen->values);
    }
    if (status == BENTO_OK) {
        status = add_table(screen);
    }
    if (status == BENTO_OK) {
        status = add_help(screen);
    }
    if (status == BENTO_OK) {
        poll_screen(screen->ui);
    }
    return status;
}

static void show_mode(const screen_t *screen, bool naive)
{
    char text[TEXT_BYTES];

    if (naive) {
        (void)snprintf(text, sizeof(text),
                       "โหมดง่าย: sleep_ms(%ld) ต่อกัน", (long)IMU_MS);
    } else {
        (void)snprintf(text, sizeof(text), "โหมดนัด: ticks_add");
    }
    set_text(screen->ui, screen->mode, text);
    (void)bento_ui_set_color(screen->ui, screen->mode,
                             naive ? COL_WARN : COL_OK);
}

/* A live cell is written only when its text changes, so fewer
 * messages cross to the display core. */
static void put(screen_t *screen, uint32_t row, uint32_t col,
                const char *text)
{
    char *shown =
        screen->cells[row - 1U][col - (uint32_t)COLUMN_AVERAGE];

    if (strcmp(shown, text) != 0) {
        (void)snprintf(shown, CELL_BYTES, "%s", text);
        set_cell(screen, row, col, text);
    }
}

/* One row: average period, min-max, late starts and the last work. */
static void show_task(screen_t *screen, uint32_t row,
                      const task_t *task)
{
    char text[CELL_BYTES];

    if (task->runs != 0) {
        (void)snprintf(text, sizeof(text), "%ld",
                       (long)(task->total_ms / task->runs));
        put(screen, row, (uint32_t)COLUMN_AVERAGE, text);
        (void)snprintf(text, sizeof(text), "%ld-%ld",
                       (long)task->low_ms, (long)task->high_ms);
        put(screen, row, (uint32_t)COLUMN_RANGE, text);
    } else {
        put(screen, row, (uint32_t)COLUMN_AVERAGE, "--");
        put(screen, row, (uint32_t)COLUMN_RANGE, "--");
    }
    (void)snprintf(text, sizeof(text), "%ld", (long)task->late);
    put(screen, row, (uint32_t)COLUMN_LATE, text);
    (void)snprintf(text, sizeof(text), "%ld", (long)task->work_ms);
    put(screen, row, (uint32_t)COLUMN_WORK, text);
}

/* MicroPython's own '%.<digits>f' text; a BENTO_FMT_BUF_SIZE buffer
 * always holds it. */
static void format_real(char *text, bento_real_t value, int32_t digits)
{
    uint32_t length = 0U;

    if (bento_fmt_real_precision(text, BENTO_FMT_BUF_SIZE, value,
                                 digits, &length) != BENTO_OK) {
        text[0] = '\0';
    }
}

static void show_values(const screen_t *screen,
                        const readings_t *readings)
{
    char accel[BENTO_FMT_BUF_SIZE] = "--";
    char temp[BENTO_FMT_BUF_SIZE] = "--";
    char text[TEXT_BYTES];

    if (readings->accel_known) {
        format_real(accel, readings->accel, ACCEL_DIGITS);
        (void)snprintf(text, sizeof(text), "a %s m/s2", accel);
    } else {
        (void)snprintf(text, sizeof(text), "a --");
    }
    if (readings->temp_known) {
        format_real(temp, readings->temp, TEMP_DIGITS);
        (void)snprintf(&text[strlen(text)], sizeof(text) - strlen(text),
                       "   T %s C", temp);
    } else {
        (void)snprintf(&text[strlen(text)], sizeof(text) - strlen(text),
                       "   T --");
    }
    set_text(screen->ui, screen->values, text);
}

static void show_all(screen_t *screen, const schedule_t *schedule)
{
    uint32_t i;

    for (i = 0U; i < (uint32_t)TASK_COUNT; i++) {
        show_task(screen, i + 1U, &schedule->tasks[i]);
    }
    show_values(screen, &schedule->readings);
}

/* Runs task i once, timing how long its work takes. */
static bento_status_t run_task(screen_t *screen, schedule_t *schedule,
                               uint32_t i)
{
    task_t *task = &schedule->tasks[i];
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    record(task, start_ms);
    if (i == (uint32_t)TASK_IMU) {
        read_accel(&schedule->readings);
    } else if (i == (uint32_t)TASK_CLIMATE) {
        status = read_temp(&schedule->readings);
    } else {
        show_all(screen, schedule);
        poll_screen(screen->ui);
    }
    task->work_ms =
        bento_time_ticks_diff(bento_time_ticks_ms(), start_ms);
    return status;
}

/* SW5 pressed once: switch the mode and start every count again. */
static bento_status_t
check_switch(screen_t *screen, schedule_t *schedule, uint32_t now_ms)
{
    bento_status_t status = BENTO_OK;
    uint32_t i;

    if (pressed_now(&schedule->sw5)) {
        schedule->naive = !schedule->naive;
        for (i = 0U; i < (uint32_t)TASK_COUNT; i++) {
            reset_task(&schedule->tasks[i], now_ms);
        }
        show_mode(screen, schedule->naive);
        status = beep(screen->ui, TUNE_TAP);
    }
    return status;
}

/* Which tasks run this round: the naive mode counts rounds instead of
 * looking at the clock. */
static void find_due(schedule_t *schedule, uint32_t now_ms, bool *due)
{
    uint32_t i;

    if (schedule->naive) {
        schedule->rounds++;
        due[TASK_IMU] = true;
        due[TASK_CLIMATE] =
            ((schedule->rounds % (CLIMATE_MS / IMU_MS)) == 0);
        due[TASK_SCREEN] =
            ((schedule->rounds % (SCREEN_MS / IMU_MS)) == 0);
    } else {
        for (i = 0U; i < (uint32_t)TASK_COUNT; i++) {
            due[i] = is_due(&schedule->tasks[i], now_ms);
        }
    }
}

/* Milliseconds from now to the nearest appointment. */
static int32_t nearest_due_ms(const schedule_t *schedule,
                              uint32_t now_ms)
{
    int32_t nearest_ms =
        bento_time_ticks_diff(schedule->tasks[0].due_ms, now_ms);
    uint32_t i;

    for (i = 1U; i < (uint32_t)TASK_COUNT; i++) {
        const int32_t left_ms =
            bento_time_ticks_diff(schedule->tasks[i].due_ms, now_ms);

        if (left_ms < nearest_ms) {
            nearest_ms = left_ms;
        }
    }
    return nearest_ms;
}

/* The naive mode sleeps a whole period whatever the work took; the
 * clock mode sleeps until the nearest appointment. */
static bento_status_t wait_round(schedule_t *schedule)
{
    const int32_t wait_for_ms =
        schedule->naive
            ? IMU_MS
            : nearest_due_ms(schedule, bento_time_ticks_ms());

    return wait_ms(wait_for_ms, &schedule->sw5);
}

static bento_status_t run_round(screen_t *screen, schedule_t *schedule)
{
    const uint32_t now_ms = bento_time_ticks_ms();
    bool due[TASK_COUNT] = {false, false, false};
    bento_status_t status = check_switch(screen, schedule, now_ms);
    uint32_t i;

    if (status == BENTO_OK) {
        find_due(schedule, now_ms, due);
    }
    for (i = 0U; (i < (uint32_t)TASK_COUNT) && (status == BENTO_OK);
         i++) {
        if (due[i]) {
            status = run_task(screen, schedule, i);
        }
    }
    if (status == BENTO_OK) {
        status = wait_round(schedule);
    }
    return status;
}

static bento_status_t run_loop(screen_t *screen, schedule_t *schedule)
{
    static const int32_t periods[TASK_COUNT] = {IMU_MS, CLIMATE_MS,
                                                SCREEN_MS};
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; i < (uint32_t)TASK_COUNT; i++) {
        schedule->tasks[i].period_ms = periods[i];
        reset_task(&schedule->tasks[i], start_ms);
    }
    schedule->sw5.index = (uint32_t)SW5;
    show_mode(screen, schedule->naive);
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = run_round(screen, schedule);
    }
    return status;
}

int main(void)
{
    screen_t screen;
    schedule_t schedule;
    bento_status_t status;

    (void)memset(&screen, 0, sizeof(screen));
    (void)memset(&schedule, 0, sizeof(schedule));
    screen.ui = bento_runtime_get_ui();
    /* firmware 2.4.1 has no volume and refuses it; the .py skips it */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    status = build_screen(&screen);
    if (status == BENTO_OK) {
        status = run_loop(&screen, &schedule);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_color(screen.ui, screen.help, COL_WARN);
        set_text(screen.ui, screen.help,
                 "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่");
        poll_screen(screen.ui);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
