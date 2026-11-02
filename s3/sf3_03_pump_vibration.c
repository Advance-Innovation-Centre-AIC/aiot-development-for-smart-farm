/*
 * sf3_03_pump_vibration.c -- หมอฟังปั๊มน้ำ: เครื่องจักรสั่นผิดปกติ
 *
 * เรียนรู้ค่าสั่นปกติจาก IMU ใน 5 วินาทีแรก แล้วให้คะแนนการสั่น 0-100
 * สั่นกว่าปกติ K_WARN เท่า = เฝ้าระวัง K_BAD เท่า = ผิดปกติ ระดับใหม่
 * ต้องค้าง CONFIRM_N ตัวอย่างถึงจะเชื่อ วางบอร์ดนิ่งช่วงเรียนรู้ แล้ว
 * เคาะโต๊ะหรือเขย่าแรง ๆ SW5 (ปุ่มล่าง) = เรียนรู้ใหม่ จอไฟ RGB แสดง
 * จำนวนครั้งที่ผิดปกติ (anomaly detection ไม่ใช้ AI)
 *
 * Learns the normal vibration from the IMU in the first 5 seconds,
 * then scores the vibration 0-100: K_WARN times the normal = watch,
 * K_BAD times = abnormal. A new level must hold CONFIRM_N samples
 * before it is believed. Keep the board still while it learns, then
 * knock the table or shake it hard. SW5 (the lower button) learns
 * again. The RGB matrix shows how many times it went abnormal
 * (anomaly detection without AI).
 *
 * Twin of sf3_03_pump_vibration.py
 * (aiot-development-for-smart-farm/s3/sf3_03_pump_vibration.py).
 * Style: CLEAN_C.md.
 * Differs from the original:
 *   - MIN_BASE is a const inside learn(), the one function that reads
 *     it; K_WARN and K_BAD stay at the top (several functions read
 *     them).
 *   - hasattr(ui, "volume"): the volume is always set, and a build
 *     without sound refuses it, which is ignored.
 *   - try/except OSError around the IMU and the RGB matrix: the
 *     call's status is checked and the same fallback is taken.
 *   - math.sqrt is sqrt() in double, rounded to the board's float
 *     (the same result as a correctly rounded float sqrt).
 *   - The Watch class and the widget dict are structs kept in main()
 *     and passed by pointer; base None is has_base false.
 */
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_buttons.h"
#include "bento_fmt.h"
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_sensors.h"
#include "bento_time.h"

/* Settings: change them and run again. */
enum {
    VOLUME = 25,      /* warning tone 0-127 */
    SPEAKER = 40,     /* speaker 0-100 % (fw 2.4.2+) */
    LEARN_MS = 5000,  /* learning time */
    SAMPLE_MS = 50,   /* read the IMU this often */
    WIN = 20,         /* vibration from the last 20 samples */
    CONFIRM_N = 10,   /* samples a new level must hold */
    RUN_MS = 120000,  /* playing time */
    TICK_MS = 500,    /* redraw this often */
    MATRIX_MS = 3000, /* send the RGB matrix again */
    CHART_MAX = 300   /* chart ceiling (vibration x100) */
};

/* Times normal = watch / abnormal. */
static const bento_real_t K_WARN = (bento_real_t)3.0;
static const bento_real_t K_BAD = (bento_real_t)6.0;

/* The three levels. */
enum {
    LEVEL_OK = 0,
    LEVEL_WATCH = 1,
    LEVEL_BAD = 2,
    LEVEL_COUNT = 3
};

/* Hardware, timing and sounds. */
enum {
    SW5_INDEX = 1, /* buttons.pressed(1) is SW5, the lower one */
    BUTTON_POLL_MS = 20,
    BEEP_MS = 120,
    SCREEN_SETTLE_MS = 200,
    SECOND_MS = 1000,
    PERCENT = 100,
    VIBRATION_DIGITS = 3, /* m/s^2 shown as %.3f */
    MATRIX_STEPS = 8,
    CHART_POINTS_MAX = 400,
    TEXT_BYTES = 128,
    NOTE_C5 = 72,
    NOTE_E5 = 76,
    NOTE_G5 = 79,
    NOTE_C6 = 84
};

/* Layout on the 800 x 400 canvas. */
enum {
    TITLE_X = 12,
    TITLE_Y = 6,
    TITLE_PX = 24,
    BIG_PX = 28,
    MID_PX = 20,
    TEXT_PX = 16,
    SMALL_PX = 14,
    STATUS_Y = 38,
    CARD_Y = 64,
    CARD_H = 190,
    CARD_RADIUS = 12,
    CARD_BORDER = 1,
    CARD_TITLE_DX = 12,
    CARD_TITLE_DY = 6,
    SCORE_CARD_X = 12,
    SCORE_CARD_W = 380,
    ARC_X = 24,
    ARC_Y = 92,
    ARC_SIZE = 140,
    SCORE_X = 180,
    SCORE_Y = 94,
    VERDICT_Y = 134,
    LEVEL_LED_Y = 196,
    LEVEL_LED_SIZE = 20,
    LEVEL_STEP = 66,
    LEVEL_NAME_DX = 26,
    LEVEL_NAME_Y = 198,
    VIB_CARD_X = 402,
    VIB_CARD_W = 378,
    VIB_X = 414,
    VIB_Y = 92,
    BASE_Y = 120,
    LEARN_Y = 146,
    LEARN_W = 354,
    LEARN_H = 12,
    BAD_TITLE_Y = 166,
    SEG7_Y = 186,
    SEG7_W = 110,
    SEG7_H = 58,
    TIME_X = 540,
    TIME_Y = 204,
    CHART_X = 12,
    CHART_Y = 262,
    CHART_W = 400,
    CHART_H = 76,
    LEGEND_X = 424,
    LEGEND_Y = 266,
    LEGEND_STEP = 24,
    HELP_X = 12,
    HELP_Y = 352
};

/* Colours, 0xRRGGBB. */
enum {
    COLOR_TEXT = 0xE8EAED,
    COLOR_DIM = 0x9AA3AF,
    COLOR_CARD = 0x171B22,
    COLOR_OK = 0x30A46C,
    COLOR_WARN = 0xF5A623,
    COLOR_BAD = 0xE5484D,
    COLOR_INFO = 0x4A9EFF
};

/* The colour of each level. */
static const uint32_t COLORS[LEVEL_COUNT] = {COLOR_OK, COLOR_WARN,
                                             COLOR_BAD};

typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
} rect_t;

/* One board button: a press is remembered until it is asked for. */
typedef struct {
    uint32_t index;
    bool down;
    bool clicked;
} board_button_t;

/* The widgets the program changes after it has drawn them. */
typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t status;
    bento_ui_id_t arc;
    bento_ui_id_t score;
    bento_ui_id_t verdict;
    bento_ui_id_t leds[LEVEL_COUNT];
    bento_ui_id_t vibration;
    bento_ui_id_t base;
    bento_ui_id_t learn;
    bento_ui_id_t bad_count;
    bento_ui_id_t bad_time;
    bento_ui_id_t chart;
    uint8_t warn_series;
    uint8_t bad_series;
    bento_ui_id_t help;
} pump_screen_t;

/* The memory: it touches no hardware and no screen. */
typedef struct {
    bento_real_t samples[WIN];
    uint32_t sample_count;
    bento_real_t vibration;
    int32_t dt_ms;
    uint32_t last_ms;
    int32_t bad_count;
    int32_t bad_ms;
    bool has_base;
    bento_real_t base;
    bento_real_t total;
    int32_t learned;
    uint32_t learn_start_ms;
    int32_t level;
    int32_t candidate;
    int32_t streak;
} watch_t;

/* The RGB matrix picture: the count, the level and the bar step. */
typedef struct {
    bool has_shown;
    int32_t bad_count;
    int32_t level;
    int32_t step;
    uint32_t sent_ms;
} matrix_key_t;

static int16_t px(int32_t value)
{
    return (int16_t)value;
}

/* Sleeps, and turns Stop into BENTO_E_CANCELLED. */
static bento_status_t pause_ms(uint32_t duration_ms)
{
    bento_status_t status = bento_time_sleep_ms(duration_ms);

    if ((status == BENTO_OK) && !bento_runtime_is_running()) {
        status = BENTO_E_CANCELLED;
    }
    return status;
}

/* ui.poll() here only keeps the queue empty; it answers no events
 * when it fails. */
static void drain_events(bento_ui_t *ui)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    (void)bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                        NULL);
}

/* A setter's refusal is ignored, as MicroPython's widget setters
 * ignore it (modui.c). */
static void set_text(bento_ui_t *ui, bento_ui_id_t widget,
                     const char *text)
{
    (void)bento_ui_set_text(ui, widget, text, (uint32_t)strlen(text));
}

/* A real number as '%.<digits>f' writes it on this board. */
static void real_text(bento_real_t value, int32_t digits, char *text)
{
    uint32_t length = 0U;

    if (bento_fmt_real_precision(text, BENTO_FMT_BUF_SIZE, value,
                                 digits, &length) != BENTO_OK) {
        text[0] = '\0';
    }
}

static bento_status_t add_label(bento_ui_t *ui, const char *text,
                                const rect_t *place, uint32_t color,
                                int32_t size_px, bento_ui_id_t *out_id)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.text = text;
    args.text_len = (uint32_t)strlen(text);
    args.x = place->x;
    args.y = place->y;
    args.color = color;
    args.font_px = size_px;
    return bento_ui_label_create(ui, &args, out_id);
}

/* A label whose id nobody needs again. */
static bento_status_t add_text(bento_ui_t *ui, const char *text,
                               int32_t x, int32_t y, uint32_t color,
                               int32_t size_px)
{
    const rect_t place = {px(x), px(y), 0, 0};
    bento_ui_id_t unused;

    return add_label(ui, text, &place, color, size_px, &unused);
}

/* A dark card with its title. */
static bento_status_t add_card(bento_ui_t *ui, int32_t x, int32_t w,
                               const char *title)
{
    bento_ui_widget_args_t args;
    bento_ui_id_t unused;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = px(x);
    args.y = CARD_Y;
    args.w = px(w);
    args.h = CARD_H;
    args.color = COLOR_CARD;
    args.min_val = COLOR_DIM; /* a panel's min= is its border */
    args.max_val = CARD_RADIUS;
    args.font_px = CARD_BORDER;
    status = bento_ui_panel_create(ui, &args, &unused);
    if (status == BENTO_OK) {
        status = add_text(ui, title, x + CARD_TITLE_DX,
                          CARD_Y + CARD_TITLE_DY, COLOR_INFO, TEXT_PX);
    }
    return status;
}

/* A widget from a place, a colour, a value and a range. */
static void widget_args(const rect_t *place, uint32_t color,
                        int32_t high, bento_ui_widget_args_t *args)
{
    bento_ui_widget_args_init(args);
    args->x = place->x;
    args->y = place->y;
    args->w = place->w;
    args->h = place->h;
    args->color = color;
    args->font_px = 0;
    args->min_val = 0;
    args->max_val = high;
}

/* Soft MIDI notes (ui.sfx cannot be made quieter), 120 ms apart.
 * ui.tone() is sent and forgotten, so its status is not read. A
 * second note of 0 means one note only. */
static bento_status_t beep(bento_ui_t *ui, uint8_t first,
                           uint8_t second)
{
    bento_status_t status;

    (void)bento_ui_sfx_tone(ui, first, (uint8_t)BENTO_UI_WAVE_SINE,
                            (uint8_t)VOLUME, (uint16_t)BEEP_MS);
    status = pause_ms(BEEP_MS);
    if ((status == BENTO_OK) && (second != 0U)) {
        (void)bento_ui_sfx_tone(ui, second, (uint8_t)BENTO_UI_WAVE_SINE,
                                (uint8_t)VOLUME, (uint16_t)BEEP_MS);
        status = pause_ms(BEEP_MS);
    }
    return status;
}

/* The size of the acceleration over three axes (m/s^2): about 9.8
 * at rest, gravity. The size does not change with the tilt, so a
 * tilted board measures vibration as well as a flat one. false: the
 * bus was busy; the sample is skipped, never guessed. */
static bool magnitude(bento_real_t *size)
{
    bento_sensors_imu6_t motion;
    bool read;

    bento_sensors_imu6_init(&motion);
    read = bento_sensors_bmi270_motion(&motion) == BENTO_OK;
    *size = (bento_real_t)0.0;
    if (read) {
        const bento_real_t ax = (bento_real_t)motion.ax;
        const bento_real_t ay = (bento_real_t)motion.ay;
        const bento_real_t az = (bento_real_t)motion.az;
        bento_real_t sum = ax * ax;

        sum += ay * ay;
        sum += az * az;
        *size = (bento_real_t)sqrt((double)sum);
    }
    return read;
}

/* Reads a board button and remembers a new press (a rising edge). */
static bento_status_t sample_button(board_button_t *button)
{
    bool pressed = false;
    const bento_status_t status =
        bento_buttons_pressed(button->index, &pressed);

    if ((status == BENTO_OK) && pressed && !button->down) {
        button->clicked = true;
    }
    button->down = pressed;
    return status;
}

/* True once per press: asking clears it. */
static bool button_clicked(board_button_t *button)
{
    const bool fired = button->clicked;

    button->clicked = false;
    return fired;
}

/* Waits duration_ms reading the button every 20 ms, so a short press
 * is not missed. */
static bento_status_t wait_ms(uint32_t duration_ms,
                              board_button_t *button)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            (int32_t)duration_ms)) {
        status = sample_button(button);
        if (status == BENTO_OK) {
            status = pause_ms(BUTTON_POLL_MS);
        }
    }
    return status;
}

/* The count and the bar; a refused picture is drawn again at the
 * next change or MATRIX_MS resend. */
static void draw_matrix(const matrix_key_t *key)
{
    static const int32_t matrix_colors[LEVEL_COUNT] = {
        BENTO_RGBMATRIX_GREEN, BENTO_RGBMATRIX_YELLOW,
        BENTO_RGBMATRIX_RED};
    const int32_t color = matrix_colors[key->level];

    (void)bento_rgbmatrix_score(key->bad_count, color);
    (void)bento_rgbmatrix_bar(key->step, MATRIX_STEPS, color);
}

/* The standard deviation = the vibration. */
static bento_real_t spread(const watch_t *watch)
{
    const bento_real_t count = (bento_real_t)watch->sample_count;
    bento_real_t mean = (bento_real_t)0.0;
    bento_real_t squares = (bento_real_t)0.0;
    bento_real_t variance;
    uint32_t i;

    for (i = 0U; i < watch->sample_count; i++) {
        mean += watch->samples[i];
    }
    mean /= count;
    for (i = 0U; i < watch->sample_count; i++) {
        const bento_real_t away = watch->samples[i] - mean;

        squares += away * away;
    }
    variance = squares / count;
    return (bento_real_t)sqrt((double)variance);
}

/* 0 = normal, 1 = watch (above K_WARN times normal), 2 = abnormal
 * (above K_BAD times). */
static int32_t classify(bento_real_t vibration, bento_real_t base)
{
    int32_t level = LEVEL_OK;

    if (vibration > (base * K_BAD)) {
        level = LEVEL_BAD;
    } else if (vibration > (base * K_WARN)) {
        level = LEVEL_WATCH;
    } else {
        /* normal */
    }
    return level;
}

/* Forgets the normal and starts learning again from now. */
static void relearn(watch_t *watch, uint32_t now_ms)
{
    watch->has_base = false;
    watch->total = (bento_real_t)0.0;
    watch->learned = 0;
    watch->learn_start_ms = now_ms;
    watch->level = LEVEL_OK;
    watch->candidate = LEVEL_OK;
    watch->streak = 0;
}

static void watch_init(watch_t *watch, uint32_t now_ms)
{
    (void)memset(watch, 0, sizeof(*watch));
    watch->last_ms = now_ms;
    relearn(watch, now_ms);
}

/* True once WIN samples are in and a new vibration is ready. */
static bool push(watch_t *watch, bento_real_t size, uint32_t now_ms)
{
    uint32_t i;

    watch->dt_ms = bento_time_ticks_diff(now_ms, watch->last_ms);
    watch->last_ms = now_ms;
    if (watch->sample_count == (uint32_t)WIN) {
        for (i = 1U; i < (uint32_t)WIN; i++) {
            watch->samples[i - 1U] = watch->samples[i];
        }
        watch->sample_count--;
    }
    watch->samples[watch->sample_count] = size;
    watch->sample_count++;
    if (watch->sample_count == (uint32_t)WIN) {
        watch->vibration = spread(watch);
    }
    return watch->sample_count == (uint32_t)WIN;
}

/* After LEARN_MS the normal is known (true once). */
static bool learn(watch_t *watch, uint32_t now_ms)
{
    const bento_real_t min_base = (bento_real_t)0.05; /* m/s^2 */
    bool done;

    watch->total += watch->vibration;
    watch->learned++;
    done = bento_time_ticks_diff(now_ms, watch->learn_start_ms) >=
           LEARN_MS;
    if (done) {
        const bento_real_t mean =
            watch->total / (bento_real_t)watch->learned;

        watch->base = (mean > min_base) ? mean : min_base;
        watch->has_base = true;
    }
    return done;
}

/* A new level must hold CONFIRM_N samples in a row before it is
 * believed (one knock does not break the machine). Gives the level
 * before. */
static int32_t judge(watch_t *watch)
{
    const int32_t fresh = classify(watch->vibration, watch->base);
    const int32_t old = watch->level;

    watch->streak =
        (fresh == watch->candidate) ? (watch->streak + 1) : 1;
    watch->candidate = fresh;
    if ((fresh != old) && (watch->streak >= CONFIRM_N)) {
        watch->level = fresh;
        if (fresh == LEVEL_BAD) {
            watch->bad_count++;
        }
    }
    if (watch->level == LEVEL_BAD) {
        watch->bad_ms += watch->dt_ms; /* real time, not SAMPLE_MS */
    }
    return old;
}

/* The score card: the ring, the score, the verdict, three LEDs. */
static bento_status_t build_score_card(pump_screen_t *screen)
{
    static const char *const level_names[LEVEL_COUNT] = {"ปกติ", "ระวัง",
                                                         "ผิดปกติ"};
    const rect_t arc = {ARC_X, ARC_Y, ARC_SIZE, ARC_SIZE};
    const rect_t score = {SCORE_X, SCORE_Y, 0, 0};
    const rect_t verdict = {SCORE_X, VERDICT_Y, 0, 0};
    bento_ui_widget_args_t args;
    int32_t i;
    bento_status_t status = add_card(
        screen->ui, SCORE_CARD_X, SCORE_CARD_W, "คะแนนการสั่น (0-100)");

    if (status == BENTO_OK) {
        widget_args(&arc, 0U, PERCENT, &args);
        status = bento_ui_arc_create(screen->ui, &args, &screen->arc);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_color(screen->ui, screen->arc, COLOR_OK);
        status = add_label(screen->ui, "0", &score, COLOR_TEXT, BIG_PX,
                           &screen->score);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "...", &verdict, COLOR_DIM,
                           BIG_PX, &screen->verdict);
    }
    for (i = 0; (i < LEVEL_COUNT) && (status == BENTO_OK); i++) {
        const int32_t x = SCORE_X + (i * LEVEL_STEP);
        const rect_t led = {px(x), LEVEL_LED_Y, LEVEL_LED_SIZE,
                            LEVEL_LED_SIZE};

        widget_args(&led, COLORS[i], 0, &args);
        status =
            bento_ui_led_create(screen->ui, &args, &screen->leds[i]);
        if (status == BENTO_OK) {
            status =
                add_text(screen->ui, level_names[i], x + LEVEL_NAME_DX,
                         LEVEL_NAME_Y, COLOR_DIM, SMALL_PX);
        }
    }
    return status;
}

/* The vibration card: now, the learned normal, the learning bar and
 * the abnormal count and time. */
static bento_status_t build_vib_card(pump_screen_t *screen)
{
    const rect_t vibration = {VIB_X, VIB_Y, 0, 0};
    const rect_t base = {VIB_X, BASE_Y, 0, 0};
    const rect_t learn_bar = {VIB_X, LEARN_Y, LEARN_W, LEARN_H};
    const rect_t count = {VIB_X, SEG7_Y, SEG7_W, SEG7_H};
    const rect_t bad_time = {TIME_X, TIME_Y, 0, 0};
    bento_ui_widget_args_t args;
    bento_status_t status =
        add_card(screen->ui, VIB_CARD_X, VIB_CARD_W, "ค่าสั่น (m/s^2)");

    if (status == BENTO_OK) {
        status = add_label(screen->ui, "ตอนนี้ -", &vibration, COLOR_TEXT,
                           MID_PX, &screen->vibration);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "ปกติที่เรียนมา -", &base,
                           COLOR_INFO, TEXT_PX, &screen->base);
    }
    if (status == BENTO_OK) {
        widget_args(&learn_bar, 0U, PERCENT, &args);
        status = bento_ui_bar_create(screen->ui, &args, &screen->learn);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_color(screen->ui, screen->learn, COLOR_WARN);
        status = add_text(screen->ui, "ผิดปกติ (ครั้ง)", VIB_X, BAD_TITLE_Y,
                          COLOR_DIM, SMALL_PX);
    }
    if (status == BENTO_OK) {
        widget_args(&count, COLOR_BAD, 0, &args);
        args.text = "0";
        args.text_len = (uint32_t)strlen(args.text);
        status =
            bento_ui_seg7_create(screen->ui, &args, &screen->bad_count);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "รวม 0.0 วิ", &bad_time,
                           COLOR_DIM, MID_PX, &screen->bad_time);
    }
    return status;
}

/* A line chart with no round dots: LVGL draws none when there are
 * at least as many points as pixels (400 at most). */
static bento_status_t build_chart(pump_screen_t *screen)
{
    const rect_t chart = {CHART_X, CHART_Y, CHART_W, CHART_H};
    bento_ui_widget_args_t args;
    bento_status_t status;

    widget_args(&chart, COLOR_INFO, CHART_MAX, &args);
    status = bento_ui_chart_create(screen->ui, &args, &screen->chart);
    if (status == BENTO_OK) {
        /* .prop() never raises in MicroPython */
        (void)bento_ui_set_prop(screen->ui, screen->chart,
                                (uint8_t)BENTO_UI_PROP_CHART_POINTS,
                                CHART_POINTS_MAX);
        status =
            bento_ui_chart_add_series(screen->ui, screen->chart,
                                      COLOR_WARN, &screen->warn_series);
    }
    if (status == BENTO_OK) {
        status = bento_ui_chart_add_series(
            screen->ui, screen->chart, COLOR_BAD, &screen->bad_series);
    }
    if (status == BENTO_OK) {
        status = add_text(screen->ui, "ฟ้า = ค่าสั่น x100", LEGEND_X,
                          LEGEND_Y, COLOR_INFO, SMALL_PX);
    }
    if (status == BENTO_OK) {
        status = add_text(screen->ui, "ส้ม = เส้นเฝ้าระวัง", LEGEND_X,
                          LEGEND_Y + LEGEND_STEP, COLOR_WARN, SMALL_PX);
    }
    if (status == BENTO_OK) {
        status =
            add_text(screen->ui, "แดง = เส้นผิดปกติ", LEGEND_X,
                     LEGEND_Y + (LEGEND_STEP * 2), COLOR_BAD, SMALL_PX);
    }
    return status;
}

static bento_status_t build_screen(pump_screen_t *screen)
{
    const rect_t status_line = {TITLE_X, STATUS_Y, 0, 0};
    const rect_t help = {HELP_X, HELP_Y, 0, 0};
    bento_status_t status =
        bento_ui_screen(screen->ui, BENTO_RUNTIME_SCREEN_WIDTH_PX,
                        BENTO_RUNTIME_SCREEN_HEIGHT_PX);

    if (status == BENTO_OK) {
        status = pause_ms(SCREEN_SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_text(screen->ui, "หมอฟังปั๊มน้ำ", TITLE_X, TITLE_Y,
                          COLOR_TEXT, TITLE_PX);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", &status_line, COLOR_WARN,
                           TEXT_PX, &screen->status);
    }
    if (status == BENTO_OK) {
        status = build_score_card(screen);
    }
    if (status == BENTO_OK) {
        status = build_vib_card(screen);
    }
    if (status == BENTO_OK) {
        status = build_chart(screen);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "SW5 (ปุ่มล่าง) = เรียนรู้ค่าปกติใหม่",
                           &help, COLOR_DIM, TEXT_PX, &screen->help);
    }
    if (status == BENTO_OK) {
        drain_events(screen->ui);
    }
    return status;
}

static void show_note(const pump_screen_t *screen, const char *text,
                      uint32_t color)
{
    (void)bento_ui_set_color(screen->ui, screen->status, color);
    set_text(screen->ui, screen->status, text);
}

static void show_learning(const pump_screen_t *screen,
                          const watch_t *watch, uint32_t now_ms)
{
    int32_t left_ms =
        LEARN_MS - bento_time_ticks_diff(now_ms, watch->learn_start_ms);
    int32_t left_percent;
    char text[TEXT_BYTES];

    if (left_ms < 0) {
        left_ms = 0;
    }
    (void)snprintf(text, sizeof(text),
                   "วางบอร์ดนิ่ง ๆ เรียนรู้ค่าปกติ อีก %" PRId32 " วิ",
                   (left_ms + (SECOND_MS - 1)) / SECOND_MS);
    show_note(screen, text, COLOR_WARN);
    left_percent = (left_ms * (int32_t)PERCENT) / (int32_t)LEARN_MS;
    (void)bento_ui_set_value(screen->ui, screen->learn,
                             PERCENT - left_percent);
}

static void show_learned(const pump_screen_t *screen,
                         const watch_t *watch)
{
    char number[BENTO_FMT_BUF_SIZE];
    char text[TEXT_BYTES];

    real_text(watch->base, VIBRATION_DIGITS, number);
    (void)snprintf(text, sizeof(text), "ปกติที่เรียนมา %s", number);
    set_text(screen->ui, screen->base, text);
    (void)bento_ui_set_value(screen->ui, screen->learn, PERCENT);
    (void)bento_ui_set_color(screen->ui, screen->learn, COLOR_OK);
}

/* A chart value x100, no higher than the chart. */
static int32_t chart_value(bento_real_t value)
{
    const int32_t scaled = (int32_t)(value * (bento_real_t)PERCENT);

    return (scaled < CHART_MAX) ? scaled : CHART_MAX;
}

/* The vibration now, the abnormal count and time, and the chart. */
static void show_numbers(const pump_screen_t *screen,
                         const watch_t *watch)
{
    char number[BENTO_FMT_BUF_SIZE];
    char text[TEXT_BYTES];
    const bento_real_t warn_line = watch->base * K_WARN;
    const bento_real_t bad_line = watch->base * K_BAD;

    real_text(watch->vibration, VIBRATION_DIGITS, number);
    (void)snprintf(text, sizeof(text), "ตอนนี้ %s", number);
    set_text(screen->ui, screen->vibration, text);
    (void)snprintf(text, sizeof(text), "%" PRId32, watch->bad_count);
    set_text(screen->ui, screen->bad_count, text);
    real_text((bento_real_t)watch->bad_ms / (bento_real_t)SECOND_MS, 1,
              number);
    (void)snprintf(text, sizeof(text), "รวม %s วิ", number);
    set_text(screen->ui, screen->bad_time, text);
    /* a chart point is sent and forgotten, as set_next() is */
    (void)bento_ui_chart_set_next(screen->ui, screen->chart, 0U,
                                  chart_value(watch->vibration));
    (void)bento_ui_chart_set_next(screen->ui, screen->chart,
                                  screen->warn_series,
                                  chart_value(warn_line));
    (void)bento_ui_chart_set_next(screen->ui, screen->chart,
                                  screen->bad_series,
                                  chart_value(bad_line));
}

static void show_watch(const pump_screen_t *screen,
                       const watch_t *watch, int32_t score)
{
    static const char *const names[LEVEL_COUNT] = {"ปกติ", "เฝ้าระวัง",
                                                   "ผิดปกติ!"};
    const uint32_t color = COLORS[watch->level];
    char text[TEXT_BYTES];
    int32_t i;

    (void)bento_ui_set_value(screen->ui, screen->arc, score);
    (void)bento_ui_set_color(screen->ui, screen->arc, color);
    (void)snprintf(text, sizeof(text), "%" PRId32, score);
    set_text(screen->ui, screen->score, text);
    (void)bento_ui_set_color(screen->ui, screen->verdict, color);
    set_text(screen->ui, screen->verdict, names[watch->level]);
    for (i = 0; i < LEVEL_COUNT; i++) {
        /* 0 = dimmed */
        (void)bento_ui_set_value(screen->ui, screen->leds[i],
                                 (i == watch->level) ? 1 : 0);
    }
    show_numbers(screen, watch);
}

/* Sound only when the level changes. */
static bento_status_t think(const pump_screen_t *screen, watch_t *watch,
                            uint32_t now_ms)
{
    char number[BENTO_FMT_BUF_SIZE];
    bento_status_t status = BENTO_OK;
    int32_t old;

    if (!watch->has_base) {
        if (learn(watch, now_ms)) {
            show_learned(screen, watch);
            status = beep(screen->ui, (uint8_t)NOTE_E5, 0U);
        }
    } else {
        old = judge(watch);
        if ((watch->level != old) && (watch->level == LEVEL_BAD)) {
            status =
                beep(screen->ui, (uint8_t)NOTE_C6, (uint8_t)NOTE_E5);
            real_text(watch->vibration, VIBRATION_DIGITS, number);
            (void)printf("ผิดปกติครั้งที่ %" PRId32 " ค่าสั่น %s\n",
                         watch->bad_count, number);
        } else if ((watch->level != old) && (old == LEVEL_BAD)) {
            /* abnormal no more */
            status =
                beep(screen->ui, (uint8_t)NOTE_G5, (uint8_t)NOTE_C6);
        } else {
            /* the same level, or between normal and watch */
        }
    }
    return status;
}

/* Every TICK_MS: the screen, then the RGB matrix when its picture
 * changes or MATRIX_MS has passed. */
static void refresh(const pump_screen_t *screen, const watch_t *watch,
                    uint32_t now_ms, int32_t fails, matrix_key_t *shown)
{
    int32_t score = 0;
    matrix_key_t key;

    if (fails >= WIN) {
        show_note(screen, "อ่าน IMU ไม่ได้", COLOR_BAD);
    } else if (!watch->has_base) {
        show_learning(screen, watch, now_ms);
    } else {
        /* 100 = on the abnormal line */
        const bento_real_t hundred =
            watch->vibration * (bento_real_t)PERCENT;

        const bento_real_t bad_line = watch->base * K_BAD;
        const bento_real_t ratio = hundred / bad_line;

        score = (int32_t)ratio;
        if (score > PERCENT) {
            score = PERCENT;
        }
        show_watch(screen, watch, score);
        show_note(screen, "เฝ้าเครื่องแล้ว ลองเคาะหรือเขย่าบอร์ด", COLOR_OK);
    }
    drain_events(screen->ui);
    key.bad_count = watch->bad_count;
    key.level = watch->level;
    key.step = (score * MATRIX_STEPS) / PERCENT;
    if (!shown->has_shown || (key.bad_count != shown->bad_count) ||
        (key.level != shown->level) || (key.step != shown->step) ||
        (bento_time_ticks_diff(now_ms, shown->sent_ms) >= MATRIX_MS)) {
        draw_matrix(&key);
        key.has_shown = true;
        key.sent_ms = now_ms;
        *shown = key;
    }
}

/* Stop or not: the RGB matrix is cleared and the round summed up. */
static void finish(const pump_screen_t *screen, const watch_t *watch)
{
    char number[BENTO_FMT_BUF_SIZE];

    (void)bento_rgbmatrix_clear(); /* the original passes a refusal */
    (void)bento_ui_set_color(screen->ui, screen->help, COLOR_WARN);
    set_text(screen->ui, screen->help,
             "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่");
    drain_events(screen->ui);
    real_text((bento_real_t)watch->bad_ms / (bento_real_t)SECOND_MS, 1,
              number);
    (void)printf("เฝ้าปั๊มครบเวลา ผิดปกติ %" PRId32 " ครั้ง รวม %s วินาที\n",
                 watch->bad_count, number);
}

/* One SAMPLE_MS round: read, decide, the button, and the screen
 * every TICK_MS. */
static bento_status_t pump_round(const pump_screen_t *screen,
                                 watch_t *watch, board_button_t *sw5,
                                 uint32_t now_ms, int32_t *fails)
{
    bento_real_t size = (bento_real_t)0.0;
    const bool read = magnitude(&size);
    bento_status_t status = BENTO_OK;

    *fails = read ? 0 : (*fails + 1);
    if (read && push(watch, size, now_ms)) {
        status = think(screen, watch, now_ms);
    }
    if ((status == BENTO_OK) && button_clicked(sw5)) {
        relearn(watch, now_ms);
        (void)bento_ui_set_color(screen->ui, screen->learn, COLOR_WARN);
        status = beep(screen->ui, (uint8_t)NOTE_C5, (uint8_t)NOTE_G5);
    }
    return status;
}

/* Until RUN_MS; whatever ends it, finish() runs. */
static bento_status_t run_pump(const pump_screen_t *screen,
                               watch_t *watch)
{
    board_button_t sw5 = {SW5_INDEX, false, false};
    const uint32_t start_ms = bento_time_ticks_ms();
    uint32_t drawn_ms = start_ms;
    matrix_key_t shown;
    int32_t fails = 0;
    bento_status_t status = BENTO_OK;

    (void)memset(&shown, 0, sizeof(shown));
    shown.sent_ms = start_ms;
    watch_init(watch, start_ms);
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        const uint32_t now_ms = bento_time_ticks_ms();

        status = pump_round(screen, watch, &sw5, now_ms, &fails);
        if ((status == BENTO_OK) &&
            (bento_time_ticks_diff(now_ms, drawn_ms) >= TICK_MS)) {
            drawn_ms = now_ms;
            refresh(screen, watch, now_ms, fails, &shown);
        }
        if (status == BENTO_OK) {
            status = wait_ms(SAMPLE_MS, &sw5);
        }
    }
    finish(screen, watch);
    return status;
}

int main(void)
{
    pump_screen_t screen;
    watch_t watch;
    bento_status_t status;

    (void)memset(&screen, 0, sizeof(screen));
    (void)memset(&watch, 0, sizeof(watch));
    screen.ui = bento_runtime_get_ui();
    /* a build without sound refuses it, as hasattr() skips it */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    status = build_screen(&screen);
    if (status == BENTO_OK) {
        status = run_pump(&screen, &watch);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
