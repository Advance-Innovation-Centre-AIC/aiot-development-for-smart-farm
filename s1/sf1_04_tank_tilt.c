/*
 * sf1_04_tank_tilt.c -- แท็งก์น้ำ/รถไถเอียงเกินไหม
 * (Is the water tank or the tractor tilting too far?)
 *
 * IMU วัดมุมเอียง เกินมุมปลอดภัย = เตือนแดง เขย่าแรง = นับแรงกระแทก
 * วางนิ่ง 1 วินาทีตอนเริ่ม = ศูนย์ แล้วเอียง/เขย่า เกม: ถือเดินให้จุดอยู่ใน
 * เป้าสีฟ้าบนจอไฟ RGB ไฟ RGB_RED = เตือน SW5 = ตั้งศูนย์ SW6 = ล้างตัวนับ
 * เล่น 2 นาทีแล้วจบเอง
 *
 * The IMU measures the tilt: past the safe angle the screen turns
 * red, and a hard shake counts as a bump. Keep the board still for
 * the first moments (that pose becomes zero), then tilt or shake
 * it. Game: walk with the board and keep the dot inside the blue
 * target on the RGB matrix. RGB_RED is the alarm, SW5 sets zero
 * again, SW6 clears the counters. It ends after 2 minutes.
 *
 * Twin of sf1_04_tank_tilt.py (aiot-development-for-smart-farm/s1/sf1_04_tank_tilt.py). Style: CLEAN_C.md.
 * Differs from the .py:
 *   - Button and Record are structs with their own functions.
 *   - math.atan2 and math.sqrt are computed in double and rounded to
 *     the board's float, so an angle can differ in the last bit;
 *     round() is nearbyint (half to even), as MicroPython's.
 *   - A failed bmi270.motion() is "no reading", as the .py's except
 *     answers None; led_named() answers "no LED" for any gpio
 *     failure, as its except answers None.
 *   - A refused rgbmatrix.blit() or clear() is ignored, as the .py
 *     catches its OSError.
 *   - hasattr(ui, "volume") is a fact of the build: the volume is
 *     always set, and a build without it refuses, which is ignored.
 *   - ui.tone() is fire-and-forget there, so its status is ignored.
 *   - Stop ends the program quietly where MicroPython raises
 *     KeyboardInterrupt; as there, finish() then does not run.
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
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_sensors.h"
#include "bento_time.h"

/* ---- 1) ตั้งค่า (แก้ได้) ---- */
enum {
    SPEAKER = 40,  /* ความดังลำโพงรวม 0-100% (fw 2.4.2+) */
    VOLUME = 25,   /* ความดังเสียง 0-127 */
    SAFE_DEG = 20, /* เอียงเกินกี่องศา = อันตราย */
    RUN_MS = 120000,
    TICK_MS = 500,
    ZERO_SAMPLES = 5,
    GAUGE_MAX = 60, /* หน้าปัด 0-60 องศา */
    NEEDLE_LEN = 48,
    TARGET_DEG = 3 /* เกม: เอียงไม่เกินนี้ = อยู่ในเป้า */
};

/* แรงรวมเกินกี่ g = กระแทก */
static const bento_real_t BUMP_G = (bento_real_t)1.8;
static const bento_real_t G = (bento_real_t)9.81;
/* เอียงเท่านี้ จุดบนจอไฟ RGB ชนขอบ */
static const bento_real_t RANGE_DEG =
    (bento_real_t)SAFE_DEG * (bento_real_t)1.5;
static const bento_real_t DEGREES_PER_RADIAN =
    (bento_real_t)57.29577951308232;

/* ชื่อบนบอร์ด: SW5 = ปุ่มล่าง, SW6 = ปุ่มบน */
static const char *const BUTTON_NAMES[] = {"SW5", "SW6"};

enum { /* colours, 0xRRGGBB */
    COL_TEXT = 0xE8EAED,
    COL_DIM = 0x9AA3AF,
    COL_CARD = 0x171B22,
    COL_OK = 0x30A46C,
    COL_WARN = 0xF5A623,
    COL_BAD = 0xE5484D,
    COL_INFO = 0x4A9EFF
};

enum {
    LEVEL_SAFE,
    LEVEL_CAREFUL,
    LEVEL_DANGER,
    LEVELS
};

static const uint32_t LEVEL_COLORS[LEVELS] = {COL_OK, COL_WARN,
                                              COL_BAD};
static const int32_t MATRIX_COLORS[LEVELS] = {
    BENTO_RGBMATRIX_GREEN, BENTO_RGBMATRIX_YELLOW, BENTO_RGBMATRIX_RED};

enum { /* hardware and units */
    SW5_INDEX = 1,
    SW6_INDEX = 0,
    MS_PER_S = 1000,
    BUTTON_SAMPLE_MS = 20,
    NO_IMU_WAIT_MS = 500,
    BUMP_GAP_MS = 500, /* one bump, not one per reading */
    G_BAR_MAX = 300,   /* the bar is full at 3 g */
    G_TO_BAR = 100
};

enum { /* the RGB matrix: 16 x 8, two pixels per byte */
    MATRIX_BYTES_PER_ROW = 8,
    NIBBLE_BITS = 4,
    LOW_NIBBLE = 0x0F,
    HIGH_NIBBLE = 0xF0,
    TARGET_X = 7, /* the blue 2 x 2 target in the middle */
    TARGET_Y = 3,
    DOTS = 8
};

enum { /* sound */
    TONE_MS = 90,
    NOTE_GAP_MS = 100,
    TUNE_NOTES_MAX = 3
};

enum { /* screen */
    SETTLE_MS = 200,
    CARD_RADIUS = 12,
    CARD_TITLE_DX = 12,
    CARD_TITLE_DY = 6,
    FONT_CARD_TITLE = 20,
    CHART_POINTS = 400,
    GAUGE_TICKS = 13,
    NEEDLE_SHIFT = 16,
    NEEDLE_MASK = 0xFFFF,
    TEXT_CHARS = 128,
    NUMBER_CHARS = 36
};

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

/* A knob scaled to span, floor division as in the .py. */

/* Accelerations in m/s^2; `known` is false when the IMU did not
 * answer, as the .py's except answers None. */
typedef struct {
    bool known;
    float ax;
    float ay;
    float az;
} motion_t;

static motion_t read_motion(void)
{
    bento_sensors_imu6_t imu;
    motion_t motion = {false, 0.0F, 0.0F, 0.0F};

    bento_sensors_imu6_init(&imu);
    if (bento_sensors_bmi270_motion(&imu) == BENTO_OK) {
        motion.known = true;
        motion.ax = imu.ax;
        motion.ay = imu.ay;
        motion.az = imu.az;
    }
    return motion;
}

typedef struct {
    bool present;
    uint32_t index;
} board_led_t;

/* Any gpio failure means no LED, as the .py's except answers None;
 * a name the board lacks falls back to LED 0. */
static board_led_t led_named(const char *name)
{
    bento_gpio_board_info_t info;
    board_led_t led = {false, 0U};
    const char *led_name = NULL;
    bool found = false;
    uint32_t i;
    bento_status_t status;

    bento_gpio_board_info_init(&info);
    status = bento_gpio_board_info(&info);
    for (i = 0U; (status == BENTO_OK) && !found && (i < info.leds);
         i++) {
        status = bento_gpio_led_name(i, &led_name);
        found = (status == BENTO_OK) && (strcmp(led_name, name) == 0);
        if (found) {
            led.index = i;
        }
    }
    if ((status == BENTO_OK) && (led.index < info.leds)) {
        status = bento_gpio_led_off(led.index);
        led.present = status == BENTO_OK;
    }
    return led;
}

/* The red LED is the tilt alarm. */
static bento_status_t set_alarm(const board_led_t *alarm, bool danger)
{
    bento_status_t status = BENTO_OK;

    if (alarm->present) {
        status = danger ? bento_gpio_led_on(alarm->index)
                        : bento_gpio_led_off(alarm->index);
    }
    return status;
}

/* A base button that never misses a short press: the firmware
 * debounces for 50 ms, so it is sampled often while waiting.
 * SW5 = index 1 (lower, P17.7), SW6 = index 0 (upper, P17.5). */
typedef struct {
    uint32_t index;
    bool down;
    bool clicked;
} button_t;

static bento_status_t sample_button(button_t *button)
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

static bool pressed_now(button_t *button)
{
    const bool fired = button->clicked;

    button->clicked = false;
    return fired;
}

static bento_status_t wait_ms(int32_t ms, button_t *sw5, button_t *sw6)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    int32_t left_ms = ms;
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) && (left_ms > 0)) {
        status = sample_button(sw5);
        if (status == BENTO_OK) {
            status = sample_button(sw6);
        }
        left_ms =
            ms - bento_time_ticks_diff(bento_time_ticks_ms(), start_ms);
        if ((status == BENTO_OK) && (left_ms > 0)) {
            status = sleep_ms((left_ms < BUTTON_SAMPLE_MS)
                                  ? (uint32_t)left_ms
                                  : (uint32_t)BUTTON_SAMPLE_MS);
        }
    }
    return status;
}

static void put_pixel(uint8_t frame[], int32_t x, int32_t y,
                      int32_t color)
{
    const int32_t i = (y * MATRIX_BYTES_PER_ROW) + (x / 2);
    const uint32_t old = frame[i];

    if ((x % 2) == 1) {
        frame[i] = (uint8_t)((old & (uint32_t)LOW_NIBBLE) |
                             ((uint32_t)color << NIBBLE_BITS));
    } else {
        frame[i] =
            (uint8_t)((old & (uint32_t)HIGH_NIBBLE) | (uint32_t)color);
    }
}

/* A 2 x 2 square of pixels from (x, y). */
static void put_square(uint8_t frame[], int32_t x, int32_t y,
                       int32_t color)
{
    put_pixel(frame, x, y, color);
    put_pixel(frame, x + 1, y, color);
    put_pixel(frame, x, y + 1, color);
    put_pixel(frame, x + 1, y + 1, color);
}

/* The blue target, then the bubble over it. */
static void draw_bubble(int32_t bubble_x, int32_t bubble_y,
                        int32_t color)
{
    uint8_t frame[BENTO_RGBMATRIX_FRAME_LEN];

    (void)memset(frame, 0, sizeof(frame));
    put_square(frame, TARGET_X, TARGET_Y, BENTO_RGBMATRIX_BLUE);
    put_square(frame, bubble_x, bubble_y, color);
    /* the .py catches the OSError of a refused blit and goes on */
    (void)bento_rgbmatrix_blit(frame, BENTO_RGBMATRIX_FRAME_LEN);
}

/* What the matrix shows; it is drawn again only when this changes. */
typedef struct {
    bool known;
    int32_t x;
    int32_t y;
    int32_t color;
} bubble_t;

/* ---- 3) สมอง (ตัดสินใจ) ---- */
typedef struct {
    bento_real_t roll;
    bento_real_t pitch;
} angles_t;

/* Roll and pitch in degrees from the direction of gravity; abs(az)
 * lets the board lie face up or face down. */
static angles_t tilt_angles(const motion_t *motion)
{
    angles_t angles;
    const float side =
        (motion->ay * motion->ay) + (motion->az * motion->az);
    const bento_real_t roll_rad = (bento_real_t)atan2(
        (double)motion->ay, fabs((double)motion->az));
    const bento_real_t pitch_rad = (bento_real_t)atan2(
        -(double)motion->ax, (double)(float)sqrt((double)side));

    angles.roll = roll_rad * DEGREES_PER_RADIAN;
    angles.pitch = pitch_rad * DEGREES_PER_RADIAN;
    return angles;
}

static bento_real_t g_force(const motion_t *motion)
{
    float sum = motion->ax * motion->ax;

    sum = sum + (motion->ay * motion->ay);
    sum = sum + (motion->az * motion->az);
    return (bento_real_t)(float)sqrt((double)sum) / G;
}

/* 0 = safe, 1 = careful (past 70 % of SAFE_DEG), 2 = danger. */
static int32_t tilt_level(int32_t tilt)
{
    const bento_real_t careful_share = (bento_real_t)0.7;
    int32_t level = LEVEL_SAFE;

    if (tilt > SAFE_DEG) {
        level = LEVEL_DANGER;
    } else if ((bento_real_t)tilt >
               ((bento_real_t)SAFE_DEG * careful_share)) {
        level = LEVEL_CAREFUL;
    } else {
        /* within the safe angle */
    }
    return level;
}

static bento_real_t clamp_unit(bento_real_t value)
{
    bento_real_t result = value;

    if (result > (bento_real_t)1.0) {
        result = (bento_real_t)1.0;
    }
    if (result < (bento_real_t)-1.0) {
        result = (bento_real_t)-1.0;
    }
    return result;
}

/* The cell of the bubble's corner: pitch moves it across, roll up
 * and down, both a full side at RANGE_DEG. */
static bubble_t bubble_cell(const angles_t *angles, int32_t color)
{
    bubble_t bubble;
    const bento_real_t across =
        clamp_unit(angles->pitch / RANGE_DEG) * (bento_real_t)TARGET_X;
    const bento_real_t down =
        clamp_unit(angles->roll / RANGE_DEG) * (bento_real_t)TARGET_Y;

    bubble.known = true;
    bubble.x =
        (int32_t)nearbyint((double)((bento_real_t)TARGET_X + across));
    bubble.y =
        (int32_t)nearbyint((double)((bento_real_t)TARGET_Y + down));
    bubble.color = color;
    return bubble;
}

static void matrix_update(const angles_t *angles, int32_t level,
                          bubble_t *drawn)
{
    const bubble_t want = bubble_cell(angles, MATRIX_COLORS[level]);

    if (!drawn->known || (want.x != drawn->x) || (want.y != drawn->y) ||
        (want.color != drawn->color)) {
        draw_bubble(want.x, want.y, want.color);
    }
    *drawn = want;
}

/* The run's records; the last bump's time survives a reset. */
typedef struct {
    uint32_t last_bump_ms;
    int32_t bumps;
    int32_t max_tilt;
    bento_real_t g_max;
    int32_t on_target_ms;
} record_t;

static void reset_record(record_t *record)
{
    record->bumps = 0;
    record->max_tilt = 0;
    record->g_max = (bento_real_t)1.0;
    record->on_target_ms = 0;
}

/* Adds one reading; true when it was a new bump. */
static bool add_reading(record_t *record, int32_t tilt, bento_real_t g,
                        uint32_t now_ms, int32_t dt_ms)
{
    bool bumped = false;

    if (tilt > record->max_tilt) {
        record->max_tilt = tilt;
    }
    if (g > record->g_max) {
        record->g_max = g;
    }
    if (tilt <= TARGET_DEG) {
        record->on_target_ms += dt_ms;
    }
    if ((g > BUMP_G) &&
        (bento_time_ticks_diff(now_ms, record->last_bump_ms) >
         BUMP_GAP_MS)) {
        record->bumps++;
        record->last_bump_ms = now_ms;
        bumped = true;
    }
    return bumped;
}

/* ---- 4) หน้าจอ ---- */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
} box_t;

typedef struct {
    const char *text;
    int16_t x;
    int16_t y;
    uint32_t color;
    int32_t font_px;
} label_spec_t;

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t gauge;
    bento_ui_id_t angles;
    bento_ui_id_t verdict;
    bento_ui_id_t max_label;
    bento_ui_id_t bump_seg;
    bento_ui_id_t g_label;
    bento_ui_id_t g_bar;
    bento_ui_id_t chart;
    uint8_t safe_series;
    bento_ui_id_t help;
} tilt_screen_t;

static void box_args(bento_ui_widget_args_t *args, const box_t *box)
{
    bento_ui_widget_args_init(args);
    args->x = box->x;
    args->y = box->y;
    args->w = box->w;
    args->h = box->h;
}

static bento_status_t
add_label(bento_ui_t *ui, const label_spec_t *spec, bento_ui_id_t *out)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.text = spec->text;
    args.text_len = (uint32_t)strlen(spec->text);
    args.x = spec->x;
    args.y = spec->y;
    args.color = spec->color;
    args.font_px = spec->font_px;
    return bento_ui_label_create(ui, &args, out);
}

/* A card: a dark box with a grey edge and a blue title. */
static bento_status_t add_card(bento_ui_t *ui, const box_t *box,
                               const char *title)
{
    bento_ui_widget_args_t args;
    bento_ui_id_t widget;
    label_spec_t heading = {title, 0, 0, COL_INFO, FONT_CARD_TITLE};
    bento_status_t status;

    box_args(&args, box);
    args.color = COL_CARD;
    args.min_val = COL_DIM;
    args.max_val = CARD_RADIUS;
    args.font_px = 1;
    status = bento_ui_panel_create(ui, &args, &widget);
    heading.x = (int16_t)(box->x + CARD_TITLE_DX);
    heading.y = (int16_t)(box->y + CARD_TITLE_DY);
    if (status == BENTO_OK) {
        status = add_label(ui, &heading, &widget);
    }
    return status;
}

/* A widget setter's refusal was ignored in MicroPython too (modui.c). */
static void set_text(bento_ui_t *ui, bento_ui_id_t widget,
                     const char *text)
{
    (void)bento_ui_set_text(ui, widget, text, (uint32_t)strlen(text));
}

static void set_color(bento_ui_t *ui, bento_ui_id_t widget,
                      uint32_t color)
{
    (void)bento_ui_set_color(ui, widget, color);
}

static void set_value(bento_ui_t *ui, bento_ui_id_t widget,
                      int32_t value)
{
    (void)bento_ui_set_value(ui, widget, value);
}

/* The .py's ui.poll() only lets the screen catch up here; its events
 * are not read, and a failed poll answers none. */
static void refresh(bento_ui_t *ui)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    (void)bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                        NULL);
}

/* A Scale has no needle of its own: the needle's length is in the
 * upper 16 bits, the value it points at in the lower 16. */
static void set_needle(bento_ui_t *ui, bento_ui_id_t scale,
                       int32_t value)
{
    const uint32_t packed = ((uint32_t)NEEDLE_LEN << NEEDLE_SHIFT) |
                            ((uint32_t)value & (uint32_t)NEEDLE_MASK);

    (void)bento_ui_set_prop(ui, scale, BENTO_UI_PROP_SCALE_NEEDLE,
                            (int32_t)packed);
}

static bento_status_t add_gauge(tilt_screen_t *screen)
{
    static const box_t gauge = {24, 100, 160, 160};
    bento_ui_widget_args_t args;
    bento_status_t status;

    box_args(&args, &gauge);
    args.color = COL_TEXT;
    args.min_val = 0;
    args.max_val = GAUGE_MAX;
    status = bento_ui_scale_create(screen->ui, &args, &screen->gauge);
    if (status == BENTO_OK) {
        /* setters: a refusal is ignored, as in MicroPython */
        (void)bento_ui_set_prop(screen->ui, screen->gauge,
                                BENTO_UI_PROP_SCALE_MODE,
                                BENTO_UI_SCALE_ROUND_IN);
        (void)bento_ui_set_ticks(screen->ui, screen->gauge, GAUGE_TICKS,
                                 2);
        (void)bento_ui_set_prop(screen->ui, screen->gauge,
                                BENTO_UI_PROP_SCALE_NEEDLE_COLOR,
                                COL_WARN);
    }
    return status;
}

static bento_status_t build_tilt_card(tilt_screen_t *screen)
{
    static const box_t card = {12, 64, 380, 272};
    static const label_spec_t angles = {"หน้า-หลัง 0\nซ้าย-ขวา 0", 200,
                                        116, COL_OK, 20};
    static const label_spec_t verdict = {"...", 24, 262, COL_WARN, 28};
    static const label_spec_t most = {"", 24, 306, COL_DIM, 20};
    bento_status_t status =
        add_card(screen->ui, &card, "มุมเอียง (องศา)");

    if (status == BENTO_OK) {
        status = add_gauge(screen);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &angles, &screen->angles);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &verdict, &screen->verdict);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &most, &screen->max_label);
    }
    return status;
}

static bento_status_t add_bump_count(tilt_screen_t *screen)
{
    static const box_t seg = {414, 100, 90, 40};
    static const label_spec_t times = {"ครั้ง", 512, 110, COL_DIM, 20};
    bento_ui_widget_args_t args;
    bento_ui_id_t widget;
    bento_status_t status;

    box_args(&args, &seg);
    args.text = "0";
    args.text_len = 1U;
    args.color = COL_WARN;
    status = bento_ui_seg7_create(screen->ui, &args, &screen->bump_seg);
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &times, &widget);
    }
    return status;
}

static bento_status_t build_bump_card(tilt_screen_t *screen)
{
    static const box_t card = {402, 64, 378, 164};
    static const label_spec_t force = {"แรงรวม 1.00 g", 414, 148,
                                       COL_TEXT, 20};
    static const box_t bar = {414, 184, 354, 18};
    bento_ui_widget_args_t args;
    bento_status_t status =
        add_card(screen->ui, &card, "แรงกระแทก (เต็มแถบ = 3 g)");

    if (status == BENTO_OK) {
        status = add_bump_count(screen);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &force, &screen->g_label);
    }
    if (status == BENTO_OK) {
        box_args(&args, &bar);
        args.min_val = 0;
        args.max_val = G_BAR_MAX;
        status = bento_ui_bar_create(screen->ui, &args, &screen->g_bar);
    }
    return status;
}

static bento_status_t build_chart(tilt_screen_t *screen)
{
    static const label_spec_t legend = {
        "เส้นฟ้า = มุมเอียง   เส้นแดง = มุมปลอดภัย", 402, 232, COL_DIM, 20};
    static const box_t chart = {402, 262, 378, 74};
    bento_ui_widget_args_t args;
    bento_ui_id_t widget;
    bento_status_t status = add_label(screen->ui, &legend, &widget);

    if (status == BENTO_OK) {
        box_args(&args, &chart);
        args.color = COL_INFO;
        args.min_val = 0;
        args.max_val = GAUGE_MAX;
        status =
            bento_ui_chart_create(screen->ui, &args, &screen->chart);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_prop(screen->ui, screen->chart,
                                BENTO_UI_PROP_CHART_POINTS,
                                CHART_POINTS);
        status = bento_ui_chart_add_series(
            screen->ui, screen->chart, COL_BAD, &screen->safe_series);
    }
    return status;
}

static bento_status_t add_titles(tilt_screen_t *screen)
{
    static const label_spec_t title = {"แท็งก์น้ำ/รถไถ เอียงเกินไหม", 12, 4,
                                       COL_TEXT, 24};
    char text[TEXT_CHARS];
    const label_spec_t subtitle = {text, 12, 34, COL_DIM, 20};
    bento_ui_id_t widget;
    bento_status_t status = add_label(screen->ui, &title, &widget);

    (void)snprintf(text, sizeof(text),
                   "มุมปลอดภัยไม่เกิน %d องศา - ลองเอียงบอร์ดดู", SAFE_DEG);
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &subtitle, &widget);
    }
    return status;
}

static bento_status_t build_screen(tilt_screen_t *screen)
{
    static const label_spec_t help = {
        "วางบอร์ดนิ่ง ๆ ตอนเริ่ม = ศูนย์ (กำลังวัด...)", 12, 352, COL_WARN, 20};
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = sleep_ms(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_titles(screen);
    }
    if (status == BENTO_OK) {
        status = build_tilt_card(screen);
    }
    if (status == BENTO_OK) {
        status = build_bump_card(screen);
    }
    if (status == BENTO_OK) {
        status = build_chart(screen);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &help, &screen->help);
    }
    if (status == BENTO_OK) {
        refresh(screen->ui);
    }
    return status;
}

/* "%.<digits>f" as MicroPython writes it on this board. */
static void real_text(char *text, uint32_t cap, bento_real_t value,
                      int32_t digits)
{
    uint32_t len = 0U;

    if (bento_fmt_real_precision(text, cap, value, digits, &len) !=
        BENTO_OK) {
        text[0] = '\0';
    }
}

static void show_verdict(const tilt_screen_t *screen, int32_t tilt,
                         int32_t level)
{
    char text[TEXT_CHARS];

    if (level == LEVEL_DANGER) {
        (void)snprintf(text, sizeof(text),
                       "อันตราย! เอียง %" PRId32 " องศา", tilt);
        set_text(screen->ui, screen->verdict, text);
    } else {
        set_text(screen->ui, screen->verdict,
                 (level == LEVEL_CAREFUL) ? "ระวัง" : "ปลอดภัย");
    }
}

static void show_tilt(const tilt_screen_t *screen,
                      const angles_t *angles, int32_t tilt,
                      int32_t level, const record_t *record)
{
    const uint32_t color = LEVEL_COLORS[level];
    char text[TEXT_CHARS];

    set_needle(screen->ui, screen->gauge,
               (tilt < GAUGE_MAX) ? tilt : GAUGE_MAX);
    /* %d of a real number cuts toward zero, as a C conversion does */
    (void)snprintf(text, sizeof(text),
                   "หน้า-หลัง %" PRId32 "\nซ้าย-ขวา %" PRId32,
                   (int32_t)angles->roll, (int32_t)angles->pitch);
    set_text(screen->ui, screen->angles, text);
    set_color(screen->ui, screen->angles, color);
    set_color(screen->ui, screen->verdict, color);
    show_verdict(screen, tilt, level);
    (void)snprintf(text, sizeof(text),
                   "เอียงมากสุด %" PRId32 " องศา   อยู่ในเป้า %" PRId32
                   " วินาที",
                   record->max_tilt, record->on_target_ms / MS_PER_S);
    set_text(screen->ui, screen->max_label, text);
}

static void show_bumps(const tilt_screen_t *screen, bento_real_t g,
                       const record_t *record)
{
    const bento_real_t bar = g * (bento_real_t)G_TO_BAR;
    char now_g[NUMBER_CHARS];
    char most_g[NUMBER_CHARS];
    char text[TEXT_CHARS];

    (void)snprintf(text, sizeof(text), "%" PRId32, record->bumps);
    set_text(screen->ui, screen->bump_seg, text);
    real_text(now_g, sizeof(now_g), g, 2);
    real_text(most_g, sizeof(most_g), record->g_max, 2);
    (void)snprintf(text, sizeof(text), "แรงรวม %s g  สูงสุด %s g", now_g,
                   most_g);
    set_text(screen->ui, screen->g_label, text);
    set_value(screen->ui, screen->g_bar,
              (bar < (bento_real_t)G_BAR_MAX) ? (int32_t)bar
                                              : G_BAR_MAX);
    set_color(screen->ui, screen->g_bar,
              (g > BUMP_G) ? COL_BAD : COL_INFO);
}

/* Drawing a chart point is a setter: a refusal is ignored. */
static void show_chart(const tilt_screen_t *screen, int32_t tilt)
{
    (void)bento_ui_chart_set_next(screen->ui, screen->chart, 0U,
                                  (tilt < GAUGE_MAX) ? tilt
                                                     : GAUGE_MAX);
    (void)bento_ui_chart_set_next(screen->ui, screen->chart,
                                  screen->safe_series, SAFE_DEG);
}

/* ---- 5) โปรแกรมหลัก ---- */
typedef struct {
    board_led_t alarm;
    button_t sw5;
    button_t sw6;
    record_t record;
    angles_t zero;
    bubble_t drawn;
    bool was_danger;
    uint32_t last_ms;
} tank_t;

/* The pose at the start is zero: ZERO_SAMPLES readings averaged (on a
 * desk the board already leans about 39 degrees). */
static bento_status_t measure_zero(const tilt_screen_t *screen,
                                   angles_t *zero)
{
    bento_real_t sum_roll = (bento_real_t)0.0;
    bento_real_t sum_pitch = (bento_real_t)0.0;
    int32_t count = 0;
    int32_t i;
    bento_status_t status = BENTO_OK;

    for (i = 0; (i < ZERO_SAMPLES) && (status == BENTO_OK); i++) {
        const motion_t motion = read_motion();

        if (motion.known) {
            const angles_t angles = tilt_angles(&motion);

            sum_roll = sum_roll + angles.roll;
            sum_pitch = sum_pitch + angles.pitch;
            count++;
        }
        refresh(screen->ui);
        status = sleep_ms(TICK_MS);
    }
    zero->roll = (bento_real_t)0.0;
    zero->pitch = (bento_real_t)0.0;
    if (count != 0) {
        zero->roll = sum_roll / (bento_real_t)count;
        zero->pitch = sum_pitch / (bento_real_t)count;
    }
    return status;
}

/* SW5: this pose is zero; SW6: clear the counters. */
static bento_status_t handle_buttons(const tilt_screen_t *screen,
                                     tank_t *tank, const angles_t *raw)
{
    bento_status_t status = BENTO_OK;

    if (pressed_now(&tank->sw5)) {
        tank->zero = *raw;
        status = beep(screen->ui, TUNE_TAP);
    }
    if ((status == BENTO_OK) && pressed_now(&tank->sw6)) {
        reset_record(&tank->record);
        status = beep(screen->ui, TUNE_STOP);
    }
    return status;
}

static int32_t largest_angle(const angles_t *angles)
{
    const bento_real_t roll = (bento_real_t)fabs((double)angles->roll);
    const bento_real_t pitch =
        (bento_real_t)fabs((double)angles->pitch);

    return (int32_t)((roll > pitch) ? roll : pitch);
}

/* Records the reading and sounds a bump or a fresh danger once. */
static bento_status_t judge(const tilt_screen_t *screen, tank_t *tank,
                            int32_t tilt, bento_real_t g)
{
    const uint32_t now_ms = bento_time_ticks_ms();
    const int32_t dt_ms = bento_time_ticks_diff(now_ms, tank->last_ms);
    bento_status_t status = BENTO_OK;

    if (add_reading(&tank->record, tilt, g, now_ms, dt_ms)) {
        status = beep(screen->ui, TUNE_HIT);
    }
    tank->last_ms = now_ms;
    if ((status == BENTO_OK) && (tilt_level(tilt) == LEVEL_DANGER) &&
        !tank->was_danger) {
        status = beep(screen->ui, TUNE_BAD);
    }
    tank->was_danger = tilt_level(tilt) == LEVEL_DANGER;
    return status;
}

static bento_status_t use_motion(const tilt_screen_t *screen,
                                 tank_t *tank, const motion_t *motion)
{
    const angles_t raw = tilt_angles(motion);
    angles_t angles;
    int32_t tilt = 0;
    const bento_real_t g = g_force(motion);
    bento_status_t status = handle_buttons(screen, tank, &raw);

    if (status == BENTO_OK) {
        angles.roll = raw.roll - tank->zero.roll;
        angles.pitch = raw.pitch - tank->zero.pitch;
        tilt = largest_angle(&angles);
        status = judge(screen, tank, tilt, g);
    }
    if (status == BENTO_OK) {
        status = set_alarm(&tank->alarm, tank->was_danger);
    }
    if (status == BENTO_OK) {
        matrix_update(&angles, tilt_level(tilt), &tank->drawn);
        show_tilt(screen, &angles, tilt, tilt_level(tilt),
                  &tank->record);
        show_bumps(screen, g, &tank->record);
        show_chart(screen, tilt);
        refresh(screen->ui);
        status = wait_ms(TICK_MS, &tank->sw5, &tank->sw6);
    }
    return status;
}

static bento_status_t run_tick(const tilt_screen_t *screen,
                               tank_t *tank)
{
    const motion_t motion = read_motion();
    bento_status_t status = BENTO_OK;

    if (!motion.known) {
        set_color(screen->ui, screen->verdict, COL_BAD);
        set_text(screen->ui, screen->verdict, "อ่าน IMU ไม่ได้");
        status = sleep_ms(NO_IMU_WAIT_MS);
    } else {
        status = use_motion(screen, tank, &motion);
    }
    return status;
}

static bento_status_t finish(const tilt_screen_t *screen,
                             const tank_t *tank)
{
    const bento_status_t status = set_alarm(&tank->alarm, false);

    if (status == BENTO_OK) {
        /* the .py catches the OSError of a refused clear */
        (void)bento_rgbmatrix_clear();
        set_color(screen->ui, screen->help, COL_WARN);
        set_text(screen->ui, screen->help,
                 "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่");
        refresh(screen->ui);
        (void)printf("เอียงมากสุด %" PRId32 " องศา  แรงกระแทก %" PRId32
                     " ครั้ง\n",
                     tank->record.max_tilt, tank->record.bumps);
    }
    return status;
}

static bento_status_t run_tank(const tilt_screen_t *screen,
                               tank_t *tank)
{
    char text[TEXT_CHARS];
    uint32_t start_ms;
    bento_status_t status = measure_zero(screen, &tank->zero);

    if (status == BENTO_OK) {
        set_color(screen->ui, screen->help, COL_DIM);
        (void)snprintf(text, sizeof(text),
                       "%s (ล่าง) = ตั้งศูนย์ใหม่   %s (บน) = ล้างตัวนับ",
                       BUTTON_NAMES[0], BUTTON_NAMES[1]);
        set_text(screen->ui, screen->help, text);
    }
    tank->last_ms = bento_time_ticks_ms();
    start_ms = bento_time_ticks_ms();
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = run_tick(screen, tank);
    }
    if (status == BENTO_OK) {
        status = finish(screen, tank);
    }
    return status;
}

int main(void)
{
    tilt_screen_t screen;
    tank_t tank;
    bento_status_t status;

    (void)memset(&screen, 0, sizeof(screen));
    (void)memset(&tank, 0, sizeof(tank));
    screen.ui = bento_runtime_get_ui();
    /* a build without a speaker volume refuses it, as hasattr() skips
     * it */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    status = build_screen(&screen);
    if (status == BENTO_OK) {
        tank.alarm = led_named("RGB_RED");
        tank.sw5.index = SW5_INDEX;
        tank.sw6.index = SW6_INDEX;
        reset_record(&tank.record);
        status = run_tank(&screen, &tank);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
