/*
 * sf3_01_practise.c -- Code Quest ระดับ 3: เติมช่อง A B C ใน confirm()
 *
 * โปรแกรมยามเฝ้าคอกเดียวกับ sf3_01_pen_guard แต่ confirm() มีช่องว่าง
 * สามช่อง (A B C) ให้เติม ก่อนเปิดจอโปรแกรมตรวจ confirm() 7 กรณี
 * ผ่าน = Console ขึ้น "ผ่าน!" แล้วเฝ้าคอกต่อ ไม่ผ่าน = บอกกรณีที่ผิด
 * แล้วจบ ติดขัด? ดูใบงาน เฉลย: โบนัส เฉลยคาบหน้า
 *
 * The same pen guard as sf3_01_pen_guard, but confirm() has three
 * blanks (A, B, C) to fill in. Before the screen opens the program
 * checks confirm() on 7 cases: pass = the Console shows "ผ่าน!" and
 * the guard runs; fail = it names the wrong case and ends. Stuck?
 * See the worksheet. The answer comes next session.
 *
 * Twin of sf3_01_practise.py
 * (aiot-development-for-smart-farm/s3/practise/sf3_01_practise.py).
 * Style: CLEAN_C.md.
 * Differs from the original:
 *   - Each blank ____ is BLANK_A, BLANK_B or BLANK_C (-1, never a
 *     right answer): C does not compile an empty blank, so the
 *     self-test says ยังไม่ถูก until the blank is filled, where
 *     MicroPython stops with a NameError.
 *   - THRESH_DB is THRESH_DB_TENTHS (4.0 dB = 40): an enum holds no
 *     real number, and the radar takes tenths of a dB anyway.
 *   - hasattr(ui, "volume"): the volume is always set, and a build
 *     without sound refuses it, which is ignored.
 *   - try/except OSError around the radar and the RGB matrix: the
 *     call's status is checked and the same fallback is taken.
 *   - The Guard class and the widget dict are structs kept in
 *     main() and passed by pointer; a self-test case is printed as
 *     the tuples print().
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_buttons.h"
#include "bento_fmt.h"
#include "bento_pots.h"
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_sensors.h"
#include "bento_time.h"

/* Settings: change them and run again. */
enum {
    MIN_CM = 50, /* VR3 sets the alert zone 50-250 cm */
    SPAN_CM = 200,
    CONFIRM_N = 3,         /* rounds in a row before we believe */
    THRESH_DB_TENTHS = 40, /* 4.0 dB above the still scene */
    MAX_CM = 300,          /* far end of the bar, ruler, chart */
    VOLUME = 25,           /* tone loudness 0-127 */
    SPEAKER = 40,          /* speaker 0-100 % (fw 2.4.2+) */
    RUN_MS = 180000,       /* one round: 3 minutes */
    SAMPLE_MS = 250,       /* read the radar this often */
    TICK_MS = 500,         /* redraw this often */
    MATRIX_MS = 3000       /* send the RGB matrix picture again */
};

/* The blanks to fill in: -1 is never a right answer. */
enum {
    BLANK_A = -1,
    BLANK_B = -1,
    BLANK_C = -1
};

/* Hardware and timing. */
enum {
    SW5_INDEX = 1, /* buttons.pressed(1) is SW5, the lower one */
    ZONE_POT = 2,  /* VR3 */
    POT_FULL = 4095,
    CM_PER_M = 100,
    TENTHS_PER_DB = 10,
    BEEP_MS = 120,
    SIREN_TONE_MS = 140,
    SIREN_STEP_MS = 150,
    SCENE_SETTLE_MS = 500,
    SCREEN_SETTLE_MS = 200,
    COUNTDOWN_S = 3,
    SECOND_MS = 1000,
    BUTTON_POLL_MS = 20,
    HISTORY_LEN = 5, /* median of the last 5 distances */
    CHART_POINTS_MAX = 400,
    TEXT_BYTES = 128
};

/* Layout on the 800 x 400 canvas. */
enum {
    TITLE_X = 12,
    TITLE_Y = 6,
    TITLE_PX = 24,
    BIG_PX = 28,
    THAI_PX = 20, /* Thai is 20 px or larger on the board */
    TEXT_PX = 16, /* Latin only */
    SMALL_PX = 14,
    CARD_RADIUS = 12,
    CARD_BORDER = 1,
    CARD_TITLE_DX = 12,
    CARD_TITLE_DY = 6,
    GATE_X = 12,
    GATE_Y = 44,
    GATE_W = 470,
    GATE_H = 196,
    STATE_X = 24,
    STATE_Y = 76,
    DISTANCE_Y = 112,
    DISTANCE_W = 150,
    DISTANCE_H = 56,
    CM_X = 182,
    CM_Y = 140,
    LED_X = 300,
    LED_Y = 116,
    LED_SIZE = 28,
    MOTION_X = 338,
    MOTION_Y = 118,
    BAR_Y = 176,
    BAR_W = 446,
    BAR_H = 20,
    RULER_Y = 198,
    RULER_H = 34,
    RULER_TICKS = 13, /* a tick every 25 cm */
    COUNTER_X = 492,
    COUNTER_W = 288,
    COUNT_X = 504,
    COUNT_Y = 78,
    COUNT_W = 150,
    COUNT_H = 60,
    SWITCH_Y = 146,
    SWITCH_W = 64,
    SWITCH_H = 32,
    ARM_X = 578,
    ARM_Y = 150,
    ZONE_Y = 196,
    LEGEND_X = 12,
    LEGEND_Y = 242,
    CHART_X = 12,
    CHART_Y = 268,
    CHART_W = 400,
    CHART_H = 72,
    LOG_X = 422,
    LOG_Y = 250,
    LOG_W = 358,
    LOG_H = 88,
    LAST_X = 434,
    LAST_Y = 284,
    ENERGY_Y = 312,
    STATUS_X = 12,
    STATUS_Y = 352
};

/* Colours, 0xRRGGBB. */
enum {
    COLOR_TEXT = 0xE8EAED,
    COLOR_DIM = 0x9AA3AF,
    COLOR_CARD = 0x171B22,
    COLOR_OK = 0x30A46C,
    COLOR_WARN = 0xF5A623,
    COLOR_BAD = 0xE5484D,
    COLOR_INFO = 0x4A9EFF,
    COLOR_SAFE_BG = 0x123322, /* the gate panel: safe */
    COLOR_ALERT_BG = 0x4A1216 /* the gate panel: intruder */
};

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

/* One radar reading: the distance (when there is a target), motion
 * and the energy. */
typedef struct {
    bool has_target;
    int32_t cm;
    bool moving;
    float energy;
} radar_reading_t;

/* What the guard believes and how many rounds the new view held. */
typedef struct {
    bool inside;
    int32_t streak;
} belief_t;

/* The picture on the RGB matrix. */
typedef struct {
    bool armed;
    bool inside;
    int32_t count;
} matrix_look_t;

/* The widgets the program changes after it has drawn them. */
typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t gate;
    bento_ui_id_t state;
    bento_ui_id_t distance;
    bento_ui_id_t led;
    bento_ui_id_t bar;
    bento_ui_id_t count;
    bento_ui_id_t toggle;
    bento_ui_id_t arm;
    bento_ui_id_t zone;
    bento_ui_id_t chart;
    uint8_t zone_series;
    bento_ui_id_t last;
    bento_ui_id_t energy;
    bento_ui_id_t status;
} guard_screen_t;

/* The guard's memory: it touches no hardware and no screen. */
typedef struct {
    bool armed;
    bool inside;
    int32_t streak;
    int32_t count;
    int32_t history[HISTORY_LEN];
    uint32_t history_len;
    bool radar_ok;
    bool has_shown;
    matrix_look_t shown; /* the picture sent last ... */
    uint32_t sent_ms;    /* ... and when */
} guard_t;

/* The widget arguments are 16-bit. */
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

/* A dark card with its title; the card's id comes back. */
static bento_status_t add_card(bento_ui_t *ui, const rect_t *place,
                               const char *title, bento_ui_id_t *out_id)
{
    bento_ui_widget_args_t args;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = place->x;
    args.y = place->y;
    args.w = place->w;
    args.h = place->h;
    args.color = COLOR_CARD;
    args.min_val = COLOR_DIM; /* a panel's min= is its border */
    args.max_val = CARD_RADIUS;
    args.font_px = CARD_BORDER;
    status = bento_ui_panel_create(ui, &args, out_id);
    if (status == BENTO_OK) {
        status =
            add_text(ui, title, place->x + CARD_TITLE_DX,
                     place->y + CARD_TITLE_DY, COLOR_INFO, THAI_PX);
    }
    return status;
}

/* A widget made from a place, a colour and a value. */
static void widget_args(const rect_t *place, uint32_t color,
                        int32_t value, bento_ui_widget_args_t *args)
{
    bento_ui_widget_args_init(args);
    args->x = place->x;
    args->y = place->y;
    args->w = place->w;
    args->h = place->h;
    args->color = color;
    args->font_px = value;
}

static bento_status_t add_seg7(bento_ui_t *ui, const char *text,
                               const rect_t *place, uint32_t color,
                               bento_ui_id_t *out_id)
{
    bento_ui_widget_args_t args;

    widget_args(place, color, 0, &args);
    args.text = text;
    args.text_len = (uint32_t)strlen(text);
    return bento_ui_seg7_create(ui, &args, out_id);
}

/* A line chart with no round dots: LVGL draws none when there are
 * at least as many points as pixels (400 at most). */
static bento_status_t add_line_chart(bento_ui_t *ui,
                                     const rect_t *place, int32_t high,
                                     uint32_t color,
                                     bento_ui_id_t *out_id)
{
    bento_ui_widget_args_t args;
    rect_t sized = *place;
    bento_status_t status;

    if (sized.w > CHART_POINTS_MAX) {
        sized.w = CHART_POINTS_MAX;
    }
    widget_args(&sized, color, 0, &args);
    args.min_val = 0;
    args.max_val = high;
    status = bento_ui_chart_create(ui, &args, out_id);
    if (status == BENTO_OK) {
        /* .prop() never raises in MicroPython */
        (void)bento_ui_set_prop(ui, *out_id,
                                (uint8_t)BENTO_UI_PROP_CHART_POINTS,
                                CHART_POINTS_MAX);
    }
    return status;
}

/* Soft MIDI notes (ui.sfx cannot be made quieter), 120 ms apart.
 * ui.tone() is sent and forgotten, so its status is not read. */
static bento_status_t beep(bento_ui_t *ui, const uint8_t *notes,
                           uint32_t count)
{
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < count) && (status == BENTO_OK); i++) {
        (void)bento_ui_sfx_tone(ui, notes[i],
                                (uint8_t)BENTO_UI_WAVE_SINE,
                                (uint8_t)VOLUME, (uint16_t)BEEP_MS);
        status = pause_ms(BEEP_MS);
    }
    return status;
}

/* The alert zone from VR3, in cm. */
static bento_status_t zone_cm(int32_t *alert_cm)
{
    uint16_t raw = 0U;
    const bento_status_t status = bento_pots_read(ZONE_POT, &raw);

    *alert_cm = (int32_t)MIN_CM +
                (((int32_t)raw * (int32_t)SPAN_CM) / (int32_t)POT_FULL);
    return status;
}

/* One radar reading; false when the radar does not answer (that
 * round is skipped). */
static bool read_radar(radar_reading_t *reading)
{
    bento_sensors_radar_status_t radar;
    bento_sensors_radar_range_t range;
    bool answered;

    bento_sensors_radar_status_init(&radar);
    bento_sensors_radar_range_init(&range);
    answered = (bento_sensors_radar_status(&radar) == BENTO_OK) &&
               (bento_sensors_radar_range(&range) == BENTO_OK);
    reading->has_target = answered && (range.target != 0U);
    reading->cm = 0;
    if (reading->has_target) {
        const bento_real_t cm =
            (bento_real_t)range.distance_m * (bento_real_t)CM_PER_M;

        reading->cm = (int32_t)cm;
    }
    reading->moving = radar.presence != 0U;
    reading->energy = radar.energy;
    return answered;
}

/* radar_config(0) learns the still scene again (nobody may move in
 * front of the board), then the threshold says how much stronger
 * than that scene a target must be. *learned false: the radar did
 * not answer. */
static bento_status_t calibrate_radar(bool *learned)
{
    const float threshold_db =
        (float)THRESH_DB_TENTHS / (float)TENTHS_PER_DB;
    bento_status_t status = BENTO_OK;

    *learned = bento_sensors_radar_config(0.0F) == BENTO_OK;
    if (*learned) {
        status = pause_ms(SCENE_SETTLE_MS);
    }
    if ((status == BENTO_OK) && *learned) {
        *learned = bento_sensors_radar_config(threshold_db) == BENTO_OK;
    }
    return status;
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

/* Waits duration_ms, reading the button every 20 ms so a short tap
 * is not missed. */
static bento_status_t wait_ms(uint32_t duration_ms,
                              board_button_t *button)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;
    bool waiting = true;

    while (waiting) {
        int32_t left_ms;

        status = sample_button(button);
        left_ms =
            (int32_t)duration_ms -
            bento_time_ticks_diff(bento_time_ticks_ms(), start_ms);
        waiting = (status == BENTO_OK) && (left_ms > 0);
        if (waiting) {
            status = pause_ms((left_ms < BUTTON_POLL_MS)
                                  ? (uint32_t)left_ms
                                  : (uint32_t)BUTTON_POLL_MS);
            waiting = status == BENTO_OK;
        }
    }
    return status;
}

/* Disarmed = dark, intruder = solid red, otherwise the count. No
 * scrolling text: it writes the I2C bus every 60 ms, the bus the
 * touch screen shares, while fill is one command. A refused picture
 * is drawn again at the next MATRIX_MS resend. */
static void show_matrix(bool armed, bool inside, int32_t count)
{
    if (!armed) {
        (void)bento_rgbmatrix_clear();
    } else if (inside) {
        (void)bento_rgbmatrix_fill(BENTO_RGBMATRIX_RED);
    } else {
        (void)bento_rgbmatrix_score(count, BENTO_RGBMATRIX_GREEN);
    }
}

/* MIDI note numbers, not hertz. */
static bento_status_t siren(bento_ui_t *ui)
{
    static const uint8_t notes[] = {81U, 74U, 81U, 74U};
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < (uint32_t)sizeof(notes)) && (status == BENTO_OK);
         i++) {
        (void)bento_ui_sfx_tone(
            ui, notes[i], (uint8_t)BENTO_UI_WAVE_SQUARE,
            (uint8_t)VOLUME, (uint16_t)SIREN_TONE_MS);
        status = pause_ms(SIREN_STEP_MS);
    }
    return status;
}

/* The median of the last 5 distances: a radar jumps between frames
 * on echoes that take more than one path. */
static int32_t smooth(guard_t *guard, int32_t cm)
{
    int32_t sorted[HISTORY_LEN];
    uint32_t i;
    uint32_t j;

    if (guard->history_len == (uint32_t)HISTORY_LEN) {
        for (i = 1U; i < (uint32_t)HISTORY_LEN; i++) {
            guard->history[i - 1U] = guard->history[i];
        }
        guard->history_len--;
    }
    guard->history[guard->history_len] = cm;
    guard->history_len++;
    for (i = 0U; i < guard->history_len; i++) {
        const int32_t value = guard->history[i];

        j = i;
        while ((j > 0U) && (sorted[j - 1U] > value)) {
            sorted[j] = sorted[j - 1U];
            j--;
        }
        sorted[j] = value;
    }
    return sorted[guard->history_len / 2U];
}

static bool in_zone(bool armed, const radar_reading_t *reading,
                    int32_t cm, int32_t alert_cm)
{
    return armed && reading->has_target && (cm < alert_cm);
}

static belief_t belief(bool inside, int32_t streak)
{
    belief_t result;

    result.inside = inside;
    result.streak = streak;
    return result;
}

/* A new state must hold CONFIRM_N rounds in a row before it is
 * believed. Gives the believed state and how many rounds in a row
 * the new one has been seen. */
static belief_t confirm(bool inside, int32_t streak, bool near)
{
    belief_t result;

    if (near == inside) {
        /* ช่อง A: เห็นเหมือนสถานะเดิม ตัวนับควรกลับไปเป็นเท่าไร? */
        result = belief(inside, BLANK_A);
    } else if ((streak + 1) >= BLANK_B) {
        /* ช่อง B: ต้องเห็นติดกันกี่รอบถึงจะเชื่อ? (ดูส่วน 1 ตั้งค่า) */
        result = belief(near, 0);
    } else {
        /* ช่อง C: ยังไม่ครบ ตัวนับควรเป็นเท่าไร? */
        result = belief(inside, BLANK_C);
    }
    return result;
}

/* "True" or "False", as print() writes a bool. */
static const char *bool_text(bool value)
{
    return value ? "True" : "False";
}

/* One case of the self-test: the arguments and the answer wanted. */
typedef struct {
    bool inside;
    int32_t streak;
    bool near;
    belief_t want;
} confirm_case_t;

/* Checks confirm() on 7 cases (CONFIRM_N = 1 can be checked too). */
static bool self_test(void)
{
    static const confirm_case_t cases[] = {
        {false, 0, false, {false, 0}},
        {false, 0, true, {CONFIRM_N <= 1, (CONFIRM_N > 1) ? 1 : 0}},
        {false, CONFIRM_N - 2, true, {false, CONFIRM_N - 1}},
        {false, CONFIRM_N - 1, true, {true, 0}},
        {true, 1, true, {true, 0}},
        {true, CONFIRM_N - 1, false, {false, 0}},
        {false, 1, false, {false, 0}}};
    const uint32_t count = (uint32_t)(sizeof(cases) / sizeof(cases[0]));
    bool passed = true;
    uint32_t i;

    for (i = 0U; (i < count) && passed; i++) {
        const confirm_case_t *check = &cases[i];
        const belief_t got =
            confirm(check->inside, check->streak, check->near);

        passed = (got.inside == check->want.inside) &&
                 (got.streak == check->want.streak);
        if (!passed) {
            (void)printf(
                "ยังไม่ถูก: confirm (%s, %" PRId32
                ", %s) ควรได้ (%s, %" PRId32 ") แต่ได้ (%s, %" PRId32
                ")\n",
                bool_text(check->inside), check->streak,
                bool_text(check->near), bool_text(check->want.inside),
                check->want.streak, bool_text(got.inside), got.streak);
        }
    }
    if (passed) {
        (void)printf("ผ่าน! confirm() ถูกทั้ง 7 กรณี\n");
    }
    return passed;
}

/* The bottom status line. */
static void say(const guard_screen_t *screen, const char *text,
                uint32_t color)
{
    (void)bento_ui_set_color(screen->ui, screen->status, color);
    set_text(screen->ui, screen->status, text);
}

static void show_arm(const guard_screen_t *screen, bool armed)
{
    static const char *const button_names[] = {"SW5", "SW6"};
    char text[TEXT_BYTES];

    (void)snprintf(text, sizeof(text), "ระบบเฝ้า: %s (%s)",
                   armed ? "เปิด" : "ปิด", button_names[0]);
    set_text(screen->ui, screen->arm, text);
    (void)bento_ui_set_color(screen->ui, screen->arm,
                             armed ? COLOR_OK : COLOR_DIM);
}

/* The gate card: its state, the distance, the LED, the bar. */
static bento_status_t build_gate(guard_screen_t *screen)
{
    const rect_t card = {GATE_X, GATE_Y, GATE_W, GATE_H};
    const rect_t state = {STATE_X, STATE_Y, 0, 0};
    const rect_t distance = {STATE_X, DISTANCE_Y, DISTANCE_W,
                             DISTANCE_H};
    const rect_t led = {LED_X, LED_Y, LED_SIZE, LED_SIZE};
    bento_ui_widget_args_t args;
    bento_status_t status =
        add_card(screen->ui, &card, "ประตูคอก (เรดาร์)", &screen->gate);

    if (status == BENTO_OK) {
        status = add_label(screen->ui, "ปลอดภัย", &state, COLOR_OK,
                           BIG_PX, &screen->state);
    }
    if (status == BENTO_OK) {
        status = add_seg7(screen->ui, "---", &distance, COLOR_INFO,
                          &screen->distance);
    }
    if (status == BENTO_OK) {
        status =
            add_text(screen->ui, "cm", CM_X, CM_Y, COLOR_DIM, TEXT_PX);
    }
    if (status == BENTO_OK) {
        widget_args(&led, COLOR_WARN, 0, &args);
        status = bento_ui_led_create(screen->ui, &args, &screen->led);
    }
    if (status == BENTO_OK) {
        status = add_text(screen->ui, "การเคลื่อนไหว", MOTION_X, MOTION_Y,
                          COLOR_DIM, THAI_PX);
    }
    return status;
}

/* The distance bar and its ruler under the gate. */
static bento_status_t build_ruler(guard_screen_t *screen)
{
    const rect_t bar = {STATE_X, BAR_Y, BAR_W, BAR_H};
    const rect_t ruler = {STATE_X, RULER_Y, BAR_W, RULER_H};
    bento_ui_widget_args_t args;
    bento_ui_id_t scale;
    bento_status_t status;

    widget_args(&bar, 0U, 0, &args);
    args.min_val = 0;
    args.max_val = MAX_CM;
    status = bento_ui_bar_create(screen->ui, &args, &screen->bar);
    if (status == BENTO_OK) {
        widget_args(&ruler, COLOR_DIM, 0, &args);
        args.min_val = 0;
        args.max_val = MAX_CM;
        status = bento_ui_scale_create(screen->ui, &args, &scale);
    }
    if (status == BENTO_OK) {
        /* a tick every 25 cm, a number every 50 cm; a setter */
        (void)bento_ui_set_ticks(screen->ui, scale, RULER_TICKS, 2);
    }
    return status;
}

/* The intruder card: the count, the switch, the zone. */
static bento_status_t build_counter(guard_screen_t *screen)
{
    const rect_t card = {COUNTER_X, GATE_Y, COUNTER_W, GATE_H};
    const rect_t count = {COUNT_X, COUNT_Y, COUNT_W, COUNT_H};
    const rect_t toggle = {COUNT_X, SWITCH_Y, SWITCH_W, SWITCH_H};
    const rect_t arm = {ARM_X, ARM_Y, 0, 0};
    const rect_t zone = {COUNT_X, ZONE_Y, 0, 0};
    bento_ui_widget_args_t args;
    bento_ui_id_t unused;
    bento_status_t status =
        add_card(screen->ui, &card, "ผู้บุกรุก (ครั้ง)", &unused);

    if (status == BENTO_OK) {
        status = add_seg7(screen->ui, "0", &count, COLOR_BAD,
                          &screen->count);
    }
    if (status == BENTO_OK) {
        widget_args(&toggle, 0U, 1, &args);
        status =
            bento_ui_switch_create(screen->ui, &args, &screen->toggle);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", &arm, COLOR_OK, THAI_PX,
                           &screen->arm);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "เขตเตือน - cm (VR3)", &zone,
                           COLOR_INFO, THAI_PX, &screen->zone);
    }
    if (status == BENTO_OK) {
        show_arm(screen, true);
    }
    return status;
}

/* The chart of distance against the zone, and the radar log. */
static bento_status_t build_log(guard_screen_t *screen)
{
    const rect_t chart = {CHART_X, CHART_Y, CHART_W, CHART_H};
    const rect_t card = {LOG_X, LOG_Y, LOG_W, LOG_H};
    const rect_t last = {LAST_X, LAST_Y, 0, 0};
    const rect_t energy = {LAST_X, ENERGY_Y, 0, 0};
    bento_ui_id_t unused;
    bento_status_t status =
        add_text(screen->ui, "ฟ้า = ระยะเป้า   แดง = เขตเตือน", LEGEND_X,
                 LEGEND_Y, COLOR_DIM, THAI_PX);

    if (status == BENTO_OK) {
        status = add_line_chart(screen->ui, &chart, MAX_CM, COLOR_INFO,
                                &screen->chart);
    }
    if (status == BENTO_OK) {
        status = bento_ui_chart_add_series(
            screen->ui, screen->chart, COLOR_BAD, &screen->zone_series);
    }
    if (status == BENTO_OK) {
        status = add_card(screen->ui, &card, "บันทึกเรดาร์", &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "ยังไม่มี", &last, COLOR_TEXT,
                           THAI_PX, &screen->last);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "energy: -", &energy, COLOR_DIM,
                           SMALL_PX, &screen->energy);
    }
    return status;
}

static bento_status_t build_screen(guard_screen_t *screen)
{
    const rect_t status_line = {STATUS_X, STATUS_Y, 0, 0};
    bento_status_t status =
        bento_ui_screen(screen->ui, BENTO_RUNTIME_SCREEN_WIDTH_PX,
                        BENTO_RUNTIME_SCREEN_HEIGHT_PX);

    if (status == BENTO_OK) {
        status = pause_ms(SCREEN_SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_text(screen->ui, "ยามเฝ้าคอก (เรดาร์)", TITLE_X,
                          TITLE_Y, COLOR_TEXT, TITLE_PX);
    }
    if (status == BENTO_OK) {
        status = build_gate(screen);
    }
    if (status == BENTO_OK) {
        status = build_ruler(screen);
    }
    if (status == BENTO_OK) {
        status = build_counter(screen);
    }
    if (status == BENTO_OK) {
        status = build_log(screen);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", &status_line, COLOR_WARN,
                           THAI_PX, &screen->status);
    }
    if (status == BENTO_OK) {
        drain_events(screen->ui);
    }
    return status;
}

/* The distance on the bar and the chart: no target = full. */
static int32_t bar_cm(const radar_reading_t *reading, int32_t cm)
{
    return (!reading->has_target || (cm > MAX_CM)) ? MAX_CM : cm;
}

static void show_gate(const guard_screen_t *screen,
                      const guard_t *guard, bool near,
                      const radar_reading_t *reading, int32_t cm)
{
    char text[TEXT_BYTES];
    uint32_t state_color = guard->armed ? COLOR_OK : COLOR_DIM;
    const char *state_text = guard->armed ? "ปลอดภัย" : "ปิดระบบเฝ้า";

    if (guard->inside) {
        state_color = COLOR_BAD;
        state_text = "มีผู้บุกรุก!";
    }
    (void)bento_ui_set_color(screen->ui, screen->gate,
                             guard->inside ? COLOR_ALERT_BG
                                           : COLOR_SAFE_BG);
    (void)bento_ui_set_color(screen->ui, screen->state, state_color);
    set_text(screen->ui, screen->state, state_text);
    (void)snprintf(text, sizeof(text), "%" PRId32, cm);
    set_text(screen->ui, screen->distance,
             reading->has_target ? text : "---");
    (void)bento_ui_set_value(screen->ui, screen->bar,
                             bar_cm(reading, cm));
    (void)bento_ui_set_color(screen->ui, screen->bar,
                             near ? COLOR_BAD : COLOR_OK);
    /* the amber light = something moves */
    (void)bento_ui_set_value(screen->ui, screen->led,
                             reading->moving ? 1 : 0);
}

static void show_zone(const guard_screen_t *screen, int32_t alert_cm,
                      const radar_reading_t *reading, int32_t cm)
{
    char text[TEXT_BYTES];
    char energy[BENTO_FMT_BUF_SIZE];
    uint32_t length = 0U;

    (void)snprintf(text, sizeof(text), "เขตเตือน %" PRId32 " cm (VR3)",
                   alert_cm);
    set_text(screen->ui, screen->zone, text);
    if (bento_fmt_real_precision(energy, BENTO_FMT_BUF_SIZE,
                                 (bento_real_t)reading->energy, 0,
                                 &length) != BENTO_OK) {
        energy[0] = '\0';
    }
    (void)snprintf(text, sizeof(text), "energy: %s", energy);
    set_text(screen->ui, screen->energy, text);
    /* a chart point is sent and forgotten, as set_next() is */
    (void)bento_ui_chart_set_next(screen->ui, screen->chart, 0U,
                                  bar_cm(reading, cm));
    (void)bento_ui_chart_set_next(screen->ui, screen->chart,
                                  screen->zone_series, alert_cm);
}

/* A count-down while everybody steps back, then the still scene. */
static bento_status_t start_guard(const guard_screen_t *screen)
{
    static const uint8_t notes[] = {72U, 79U};
    char text[TEXT_BYTES];
    int32_t second;
    bool learned = false;
    bento_status_t status = BENTO_OK;

    for (second = COUNTDOWN_S; (second > 0) && (status == BENTO_OK);
         second--) {
        (void)snprintf(text, sizeof(text),
                       "ถอยห่างบอร์ด! จำฉากนิ่งใน %" PRId32 " วินาที",
                       second);
        say(screen, text, COLOR_WARN);
        status = pause_ms(SECOND_MS);
    }
    if (status == BENTO_OK) {
        status = calibrate_radar(&learned);
    }
    if (status == BENTO_OK) {
        if (learned) {
            say(screen, "เฝ้าคอกแล้ว เดินเข้าหาบอร์ดได้เลย", COLOR_OK);
        } else {
            say(screen, "จำฉากไม่สำเร็จ ใช้ฉากเดิม", COLOR_BAD);
        }
        status = beep(screen->ui, notes, (uint32_t)sizeof(notes));
    }
    return status;
}

/* The switch on the screen, or SW5. ui.poll() is called only here;
 * it answers no events when it fails. */
static bento_status_t on_arm(const guard_screen_t *screen,
                             guard_t *guard, board_button_t *sw5)
{
    static const uint8_t notes[] = {76U};
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;
    uint32_t i;
    bool armed = guard->armed;
    bento_status_t status = BENTO_OK;

    if (bento_ui_poll(screen->ui, events, BENTO_UI_POLL_EVENTS_MAX,
                      &count, NULL) != BENTO_OK) {
        count = 0U;
    }
    for (i = 0U; i < count; i++) {
        if ((events[i].id == screen->toggle) &&
            (events[i].type == (uint8_t)BENTO_UI_EVENT_TOGGLED)) {
            armed = events[i].value == 1;
        }
    }
    if (button_clicked(sw5)) {
        armed = !armed;
        (void)bento_ui_set_value(screen->ui, screen->toggle,
                                 armed ? 1 : 0);
    }
    if (armed != guard->armed) {
        guard->armed = armed;
        guard->inside = false;
        guard->streak = 0;
        show_arm(screen, armed);
        status = beep(screen->ui, notes, (uint32_t)sizeof(notes));
    }
    return status;
}

/* Written only when the radar comes or goes. */
static void on_radar(const guard_screen_t *screen, guard_t *guard,
                     bool answered)
{
    if (answered != guard->radar_ok) {
        guard->radar_ok = answered;
        if (answered) {
            say(screen, "เรดาร์กลับมาแล้ว", COLOR_OK);
        } else {
            say(screen, "เรดาร์ไม่ตอบ กำลังลองใหม่", COLOR_BAD);
        }
    }
}

/* A new intruder: the count, the log, the console and the siren. */
static bento_status_t count_intruder(const guard_screen_t *screen,
                                     guard_t *guard, int32_t cm)
{
    char text[TEXT_BYTES];

    guard->count++;
    (void)snprintf(text, sizeof(text), "%" PRId32, guard->count);
    set_text(screen->ui, screen->count, text);
    (void)snprintf(text, sizeof(text),
                   "คนที่ %" PRId32 "  ระยะ %" PRId32 " cm", guard->count,
                   cm);
    set_text(screen->ui, screen->last, text);
    (void)printf("ผู้บุกรุก%s\n", text);
    return siren(screen->ui);
}

/* Sound and count only when the state changes. *near: the target is
 * inside the zone this round. */
static bento_status_t decide(const guard_screen_t *screen,
                             guard_t *guard,
                             const radar_reading_t *reading, int32_t cm,
                             int32_t alert_cm, bool *near)
{
    static const uint8_t left_notes[] = {79U, 84U};
    const bool was_inside = guard->inside;
    belief_t now;
    bento_status_t status = BENTO_OK;

    *near = in_zone(guard->armed, reading, cm, alert_cm);
    now = confirm(guard->inside, guard->streak, *near);
    guard->inside = now.inside;
    guard->streak = now.streak;
    if (guard->inside && !was_inside) {
        status = count_intruder(screen, guard, cm);
    } else if (was_inside && !guard->inside) {
        /* gone again */
        status =
            beep(screen->ui, left_notes, (uint32_t)sizeof(left_notes));
    } else {
        /* no change, no sound */
    }
    return status;
}

/* Drawn when the picture changes, and sent again every MATRIX_MS. */
static void matrix_tick(guard_t *guard)
{
    const uint32_t now_ms = bento_time_ticks_ms();
    const bool same = guard->has_shown &&
                      (guard->shown.armed == guard->armed) &&
                      (guard->shown.inside == guard->inside) &&
                      (guard->shown.count == guard->count);

    if (!same ||
        (bento_time_ticks_diff(now_ms, guard->sent_ms) >= MATRIX_MS)) {
        show_matrix(guard->armed, guard->inside, guard->count);
        guard->shown.armed = guard->armed;
        guard->shown.inside = guard->inside;
        guard->shown.count = guard->count;
        guard->has_shown = true;
        guard->sent_ms = now_ms;
    }
}

/* One reading that answered: smooth, decide, and show it on every
 * TICK_MS round. */
static bento_status_t on_reading(const guard_screen_t *screen,
                                 guard_t *guard,
                                 const radar_reading_t *reading,
                                 bool show_now)
{
    int32_t cm = 0;
    int32_t alert_cm = 0;
    bool near = false;
    bento_status_t status;

    if (reading->has_target) {
        cm = smooth(guard, reading->cm);
    }
    status = zone_cm(&alert_cm);
    if (status == BENTO_OK) {
        status = decide(screen, guard, reading, cm, alert_cm, &near);
    }
    if ((status == BENTO_OK) && show_now) {
        show_gate(screen, guard, near, reading, cm);
        show_zone(screen, alert_cm, reading, cm);
    }
    return status;
}

/* One round: the button, a reading, the decision and the act. */
static bento_status_t guard_round(const guard_screen_t *screen,
                                  guard_t *guard, board_button_t *sw5,
                                  bool show_now)
{
    radar_reading_t reading;
    bento_status_t status = on_arm(screen, guard, sw5);

    if (status == BENTO_OK) {
        const bool answered = read_radar(&reading);

        on_radar(screen, guard, answered);
        if (answered) {
            status = on_reading(screen, guard, &reading, show_now);
        }
    }
    return status;
}

/* Every SAMPLE_MS until RUN_MS; the screen every TICK_MS. */
static bento_status_t run_guard(const guard_screen_t *screen,
                                guard_t *guard)
{
    board_button_t sw5 = {SW5_INDEX, false, false};
    const int32_t every =
        TICK_MS / SAMPLE_MS; /* TICK_MS >= SAMPLE_MS */
    const uint32_t start_ms = bento_time_ticks_ms();
    int32_t tick = 0;
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = guard_round(screen, guard, &sw5, (tick % every) == 0);
        if (status == BENTO_OK) {
            matrix_tick(guard);
            tick++;
            status = wait_ms(SAMPLE_MS, &sw5); /* wait, and listen */
        }
    }
    return status;
}

/* Stopped halfway or not: the RGB matrix goes dark and the round is
 * summed up. */
static void finish(const guard_screen_t *screen, int32_t count)
{
    show_matrix(false, false, count);
    say(screen, "จบรอบแล้ว - กด Program to Device เพื่อเล่นใหม่",
        COLOR_WARN);
    drain_events(screen->ui);
    (void)printf("เฝ้าคอกครบเวลา พบผู้บุกรุก %" PRId32 " ครั้ง\n", count);
}

int main(void)
{
    guard_screen_t screen;
    guard_t guard;
    bento_status_t status = BENTO_OK;

    (void)memset(&screen, 0, sizeof(screen));
    (void)memset(&guard, 0, sizeof(guard));
    screen.ui = bento_runtime_get_ui();
    guard.armed = true;
    guard.radar_ok = true;
    /* a build without sound refuses it, as hasattr() skips it */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    if (self_test()) {
        status = build_screen(&screen);
        if (status == BENTO_OK) {
            status = start_guard(&screen);
        }
        if (status == BENTO_OK) {
            status = run_guard(&screen, &guard);
            finish(&screen, guard.count);
        }
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
