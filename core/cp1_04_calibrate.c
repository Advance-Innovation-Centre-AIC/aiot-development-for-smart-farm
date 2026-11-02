/*
 * cp1_04_calibrate.c -- หลักการ 1.4: สอบเทียบก่อนเชื่อ (ชดเชย ตั้งศูนย์ สองจุด)
 *
 * ค่าจากเซนเซอร์ = ค่าจริง + ความเพี้ยน ชดเชย (offset) = บวก/ลบค่าคงที่
 * เช่น TEMP_OFFSET ของ SHT40 ตั้งศูนย์ (tare) = จำท่าตอนนี้ไว้เป็น 0 แล้วลบออก
 * ทุกครั้ง สองจุด (two-point) = จำค่าดิบตอน "แห้ง" = 0 % และตอน "เปียก" = 100 %
 * ถ้าสองจุดใกล้กันเกิน MIN_SPAN ห้ามหาร กด SW5 (ปุ่มล่าง) = ท่านี้คือศูนย์
 * หมุน VR1 แล้วกด SW6 (ปุ่มบน) = จุดแห้ง หมุนอีกตำแหน่ง กด SW6 = จุดเปียก
 * กด SW6 อีกครั้ง = เริ่มใหม่ โปรแกรมเล่น 3 นาทีแล้วจบเอง ใช้กับ TESAIoT
 * Dev Kit และ BENTO Emulator
 *
 * Calibrate before you trust a sensor: an offset added to the SHT40
 * temperature, a tare that takes the board's present tilt as zero, and
 * a two-point calibration of VR1 as a soil probe (dry = 0 %, wet =
 * 100 %), which refuses two points closer than MIN_SPAN. SW5 (lower
 * button) sets the zero; SW6 (upper button) takes the dry point, then
 * the wet point, then starts again. It runs for three minutes.
 *
 * Twin of cp1_04_calibrate.py (aiot-development-for-smart-farm/core/cp1_04_calibrate.py). Style: CLEAN_C.md.
 * What C does differently:
 *   - the (roll, pitch) tuples are a struct; None (no reading, no point
 *     yet) is a bool beside the value;
 *   - try/except around a sensor is a status check with the same
 *     fallback: no value, or three tries 50 ms apart for the SHT40;
 *   - real numbers are bento_real_t, the board MicroPython's float,
 *     and their text is MicroPython's own (bento_fmt_real_precision);
 *     "%+.1f" puts the plus sign in front by hand;
 *   - the float % of tare() is fmod with the sign of the divisor, as
 *     MicroPython's float % is; round() rounds a half to even, as
 *     MicroPython's round() of a float does;
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
#include "bento_dsp.h"
#include "bento_fmt.h"
#include "bento_pots.h"
#include "bento_runtime.h"
#include "bento_sensors.h"
#include "bento_time.h"

/* Settings a student may change. */
enum {
    MIN_SPAN = 100, /* dry and wet must be more than this apart */
    CLIMATE_MS =
        2000,        /* read the SHT40 every 2 s (it changes slowly) */
    SAMPLE_MS = 20,  /* read the buttons this often (debounce) */
    TICK_MS = 500,   /* refresh the screen this often */
    RUN_MS = 180000, /* play for three minutes, then end */
    SPEAKER = 40,    /* speaker level 0-100 % (firmware 2.4.2+) */
    VOLUME = 25      /* tone loudness 0-127 (about 20 %) */
};

/* TEMP_OFFSET, the SHT40 offset a student sets, lives in show_temp(),
 * its one reader, as temp_offset. */

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
    HELP_Y = 352,
    CARD_TOP_Y = 44,
    CARD_TOP_H = 150,
    CARD_HALF_W = 378,
    CARD_RIGHT_X = 402,
    CARD_PROBE_Y = 202,
    CARD_PROBE_W = 768,
    CARD_PROBE_H = 136,
    CARD_RADIUS = 12,
    CARD_BORDER = 1,
    CARD_TITLE_DX = 12,
    CARD_TITLE_DY = 6,
    LEFT_X = 24,
    RIGHT_X = 414,
    FORMULA_Y = 78,
    TEMP_Y = 110,
    TILT_RAW_Y = 78,
    TILT_FIXED_Y = 116,
    ZERO_Y = 160,
    PROBE_RAW_Y = 232,
    POINTS_X = 200,
    POINTS_Y = 236,
    PERCENT_X = 560,
    PERCENT_Y = 226,
    BAR_Y = 270,
    BAR_W = 744,
    BAR_H = 18,
    STEP_Y = 300
};

/* Font sizes, in pixels. */
enum {
    FONT_TITLE = 24,
    FONT_TEXT = 20, /* Thai text is 20 px or larger on the board */
    FONT_READING = 20,
    FONT_PERCENT = 28
};

/* Sound, timing and sizes that are not settings. */
enum {
    SETTLE_MS = 200,   /* let the cleared screen settle */
    TONE_MS = 90,      /* each note of a tune */
    NOTE_GAP_MS = 100, /* one note to the next */
    TUNE_NOTES = 3,    /* the longest tune */
    SHT40_TRIES = 3,   /* the bus is busy now and then */
    RETRY_MS = 50,     /* pause between two SHT40 tries */
    TEXT_BYTES = 128,  /* a SET_TEXT carries at most 126 bytes */
    ONE_DIGIT = 1,     /* "%.1f" */
    PERCENT_MAX = 100,
    HALF_TURN = 180,
    FULL_TURN = 360,
    SW5 = 0, /* button 0 of this program is SW5, the lower one */
    SW6 = 1  /* button 1 is SW6, the upper one */
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

/* A tilt in degrees, from the direction of gravity. */
typedef struct {
    bento_real_t roll;
    bento_real_t pitch;
} tilt_t;

/* The widgets that change after the screen is drawn. */
typedef struct {
    bento_ui_id_t temp;
    bento_ui_id_t tilt_raw;
    bento_ui_id_t tilt_fixed;
    bento_ui_id_t zero;
    bento_ui_id_t probe_raw;
    bento_ui_id_t points;
    bento_ui_id_t percent;
    bento_ui_id_t bar;
    bento_ui_id_t step;
    bento_ui_id_t help;
} widgets_t;

typedef struct {
    bento_ui_t *ui;
    widgets_t widgets;
    bool was_down[2]; /* was SW5 / SW6 down last round */
    tilt_t zero;      /* the pose that counts as zero */
    bool dry_known;
    uint16_t dry; /* VR1's raw count at the dry point */
    bool wet_known;
    uint16_t wet;
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

/* The raw SHT40 temperature (no offset), three tries; *known is false
 * when none of them worked. */
static bento_status_t read_temp_raw(bento_real_t *celsius, bool *known)
{
    bento_status_t status = BENTO_OK;
    uint32_t tries = 0U;

    *known = false;
    while ((status == BENTO_OK) && !*known &&
           (tries < (uint32_t)SHT40_TRIES)) {
        float reading = 0.0F;
        float humidity = 0.0F;

        if (bento_sensors_sht40_read(&reading, &humidity) == BENTO_OK) {
            *celsius = reading;
            *known = true;
        } else {
            status = pause_ms((uint32_t)RETRY_MS);
        }
        tries++;
    }
    return status;
}

/* (roll, pitch) from the IMU; false when it cannot be read. */
static bool read_tilt(tilt_t *tilt)
{
    bento_sensors_imu6_t motion;
    float roll = 0.0F;
    float pitch = 0.0F;
    bool known = false;

    bento_sensors_imu6_init(&motion);
    if ((bento_sensors_bmi270_motion(&motion) == BENTO_OK) &&
        (bento_dsp_tilt(motion.ax, motion.ay, motion.az, &roll,
                        &pitch) == BENTO_OK)) {
        tilt->roll = roll;
        tilt->pitch = pitch;
        known = true;
    }
    return known;
}

/* VR1 stands in for a soil probe: a raw count 0-4095. */
static bento_status_t read_probe(uint16_t *raw)
{
    return bento_pots_read(0U, raw);
}

/* True once, when button i has just gone down (0 = SW5 = pressed(1),
 * 1 = SW6 = pressed(0)). Called every round: the debounce moves only
 * when the button is read. */
static bento_status_t just_pressed(app_t *app, uint32_t i, bool *edge)
{
    bool down = false;
    bento_status_t status =
        bento_buttons_pressed((uint32_t)(1U - i), &down);

    *edge = (status == BENTO_OK) && down && !app->was_down[i];
    app->was_down[i] = down;
    return status;
}

/* Tones go through ui.tone because its loudness can be set. */
static bento_status_t beep(bento_ui_t *ui, int32_t tune)
{
    static const uint8_t notes[TUNE_KINDS][TUNE_NOTES] = {
        {76U, 0U, 0U},   {72U, 79U, 0U}, {79U, 72U, 0U},
        {72U, 79U, 84U}, {84U, 76U, 0U}, {84U, 76U, 69U},
        {88U, 0U, 0U}};
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

/* Take the zero off, then wrap into -180..180 degrees (179 - (-179)
 * is -2, not 358). */
static bento_real_t tare(bento_real_t angle, bento_real_t zero)
{
    const bento_real_t turn = (bento_real_t)FULL_TURN;
    const bento_real_t shifted =
        (angle - zero) + (bento_real_t)HALF_TURN;
    /* fmod is exact, so its double result fits the board's float */
    bento_real_t wrapped =
        (bento_real_t)fmod((double)shifted, (double)turn);

    if (wrapped < (bento_real_t)0.0) {
        wrapped += turn; /* the sign of the divisor, as Python's % */
    }
    return wrapped - (bento_real_t)HALF_TURN;
}

static bool far_enough(uint16_t first, uint16_t second)
{
    const int32_t span = (int32_t)second - (int32_t)first;

    return ((span < 0) ? -span : span) >= MIN_SPAN;
}

/* Two-point calibration: dry -> 0 %, wet -> 100 %, for a probe that
 * rises or falls when wet. False while a point is missing or the two
 * are closer than MIN_SPAN: then nothing is divided. */
static bool two_point(const app_t *app, uint16_t raw,
                      bento_real_t *percent)
{
    bool known = false;

    if (app->dry_known && app->wet_known &&
        far_enough(app->dry, app->wet)) {
        const int32_t scaled =
            ((int32_t)raw - (int32_t)app->dry) * PERCENT_MAX;
        const int32_t span = (int32_t)app->wet - (int32_t)app->dry;
        const bento_real_t value =
            (bento_real_t)scaled / (bento_real_t)span;

        *percent = value;
        if (value > (bento_real_t)PERCENT_MAX) {
            *percent = (bento_real_t)PERCENT_MAX;
        }
        if (value < (bento_real_t)0.0) {
            *percent = (bento_real_t)0.0;
        }
        known = true;
    }
    return known;
}

/* MicroPython's round() of a float: to the nearest whole number, a
 * half to the even one. */
static int32_t round_half_even(bento_real_t value)
{
    const bento_real_t half = (bento_real_t)0.5;
    int32_t whole = (int32_t)value;
    bento_real_t rest;

    if ((bento_real_t)whole > value) {
        whole--;
    }
    rest = value - (bento_real_t)whole;
    if ((rest > half) || ((rest == half) && ((whole % 2) != 0))) {
        whole++;
    }
    return whole;
}

/* MicroPython's own '%.1f' text; a BENTO_FMT_BUF_SIZE buffer always
 * holds it. */
static void format_real(char *text, bento_real_t value)
{
    uint32_t length = 0U;

    if (bento_fmt_real_precision(text, BENTO_FMT_BUF_SIZE, value,
                                 ONE_DIGIT, &length) != BENTO_OK) {
        text[0] = '\0';
    }
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

/* The percent bar; its colour must be set after it is made. */
static bento_status_t add_bar(bento_ui_t *ui, bento_ui_id_t *out_id)
{
    bento_ui_widget_args_t args;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = LEFT_X;
    args.y = BAR_Y;
    args.w = BAR_W;
    args.h = BAR_H;
    args.min_val = 0;
    args.max_val = PERCENT_MAX;
    args.font_px = 0;
    status = bento_ui_bar_create(ui, &args, out_id);
    if (status == BENTO_OK) {
        /* a setter's refusal is ignored, as in MicroPython */
        (void)bento_ui_set_color(ui, *out_id, COL_OK);
    }
    return status;
}

/* Card 1: the offset of the SHT40. */
static bento_status_t build_offset_card(bento_ui_t *ui,
                                        widgets_t *widgets)
{
    const rect_t box = {MARGIN_X, CARD_TOP_Y, CARD_HALF_W, CARD_TOP_H};
    const label_spec_t formula = {LEFT_X, FORMULA_Y, FONT_TEXT,
                                  COL_DIM};
    const label_spec_t temp = {LEFT_X, TEMP_Y, FONT_TITLE, COL_OK};
    bento_ui_id_t unused;
    bento_status_t status = add_card(ui, &box, "1) ชดเชย: SHT40 (C)");

    if (status == BENTO_OK) {
        status = add_label(ui, "ดิบ + TEMP_OFFSET = ค่าที่ใช้", &formula,
                           &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(ui, "--", &temp, &widgets->temp);
    }
    return status;
}

/* Card 2: the tare of the tilt. */
static bento_status_t build_tare_card(bento_ui_t *ui,
                                      widgets_t *widgets)
{
    const rect_t box = {CARD_RIGHT_X, CARD_TOP_Y, CARD_HALF_W,
                        CARD_TOP_H};
    const label_spec_t raw = {RIGHT_X, TILT_RAW_Y, FONT_READING,
                              COL_WARN};
    const label_spec_t fixed = {RIGHT_X, TILT_FIXED_Y, FONT_READING,
                                COL_OK};
    const label_spec_t zero = {RIGHT_X, ZERO_Y, FONT_TEXT, COL_DIM};
    bento_status_t status =
        add_card(ui, &box, "2) ตั้งศูนย์: มุมเอียง (องศา)");

    if (status == BENTO_OK) {
        status = add_label(ui, "ดิบ --", &raw, &widgets->tilt_raw);
    }
    if (status == BENTO_OK) {
        status = add_label(ui, "--", &fixed, &widgets->tilt_fixed);
    }
    if (status == BENTO_OK) {
        status =
            add_label(ui, "ศูนย์ = 0, 0 (กด SW5)", &zero, &widgets->zero);
    }
    return status;
}

/* Card 3: the two-point calibration of VR1. */
static bento_status_t build_probe_card(bento_ui_t *ui,
                                       widgets_t *widgets)
{
    const rect_t box = {MARGIN_X, CARD_PROBE_Y, CARD_PROBE_W,
                        CARD_PROBE_H};
    const label_spec_t raw = {LEFT_X, PROBE_RAW_Y, FONT_READING,
                              COL_WARN};
    const label_spec_t points = {POINTS_X, POINTS_Y, FONT_TEXT,
                                 COL_TEXT};
    const label_spec_t percent = {PERCENT_X, PERCENT_Y, FONT_PERCENT,
                                  COL_OK};
    const label_spec_t step = {LEFT_X, STEP_Y, FONT_TEXT, COL_WARN};
    bento_status_t status =
        add_card(ui, &box, "3) สองจุด: VR1 = หัววัดดิน");

    if (status == BENTO_OK) {
        status = add_label(ui, "raw --", &raw, &widgets->probe_raw);
    }
    if (status == BENTO_OK) {
        status = add_label(ui, " ", &points, &widgets->points);
    }
    if (status == BENTO_OK) {
        status = add_label(ui, "-- %", &percent, &widgets->percent);
    }
    if (status == BENTO_OK) {
        status = add_bar(ui, &widgets->bar);
    }
    if (status == BENTO_OK) {
        status = add_label(ui, " ", &step, &widgets->step);
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
        status = add_label(ui, "สอบเทียบก่อนเชื่อ", &title, &unused);
    }
    if (status == BENTO_OK) {
        status = build_offset_card(ui, &app->widgets);
    }
    if (status == BENTO_OK) {
        status = build_tare_card(ui, &app->widgets);
    }
    if (status == BENTO_OK) {
        status = build_probe_card(ui, &app->widgets);
    }
    if (status == BENTO_OK) {
        status = add_label(ui,
                           "SW5 (ล่าง) = ตั้งศูนย์   SW6 (บน) = แห้ง / "
                           "เปียก / เริ่มใหม่",
                           &help, &app->widgets.help);
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

/* "R <roll>  P <pitch>", one decimal each, after a prefix. */
static void format_tilt(char *text, uint32_t size, const char *prefix,
                        const tilt_t *tilt)
{
    char roll[BENTO_FMT_BUF_SIZE];
    char pitch[BENTO_FMT_BUF_SIZE];

    format_real(roll, tilt->roll);
    format_real(pitch, tilt->pitch);
    (void)snprintf(text, size, "%sR %s  P %s", prefix, roll, pitch);
}

/* The dry and wet points, and the next step (0 = wait for dry, 1 =
 * wait for wet, 2 = both taken). */
static void show_points(const app_t *app)
{
    static const char *const steps[3] = {"หมุน VR1 ไปจุดแห้ง กด SW6",
                                         "หมุน VR1 ไปจุดเปียก กด SW6",
                                         "ครบสองจุด  SW6 = เริ่มใหม่"};
    char dry[BENTO_FMT_BUF_SIZE];
    char wet[BENTO_FMT_BUF_SIZE];
    char text[TEXT_BYTES];
    uint32_t step = 2U;

    (void)snprintf(dry, sizeof(dry), "--");
    (void)snprintf(wet, sizeof(wet), "--");
    if (app->dry_known) {
        (void)snprintf(dry, sizeof(dry), "%u", (unsigned int)app->dry);
    }
    if (app->wet_known) {
        (void)snprintf(wet, sizeof(wet), "%u", (unsigned int)app->wet);
    }
    (void)snprintf(text, sizeof(text), "แห้ง %s   เปียก %s", dry, wet);
    set_text(app->ui, app->widgets.points, text);
    if (!app->dry_known) {
        step = 0U;
    } else if (!app->wet_known) {
        step = 1U;
    } else {
        /* both points taken */
    }
    set_text(app->ui, app->widgets.step, steps[step]);
}

/* SW5: the present pose becomes the zero. */
static bento_status_t take_zero(app_t *app)
{
    char text[TEXT_BYTES];
    tilt_t tilt = {(bento_real_t)0.0, (bento_real_t)0.0};
    bento_status_t status = BENTO_OK;

    if (read_tilt(&tilt)) {
        app->zero = tilt;
        format_tilt(text, sizeof(text), "ศูนย์ = ", &app->zero);
        set_text(app->ui, app->widgets.zero, text);
        status = beep(app->ui, TUNE_START);
    }
    return status;
}

/* SW6 with a point accepted: dry, then wet, then start again. */
static bento_status_t take_point(app_t *app, uint16_t raw)
{
    bento_status_t status;

    if (!app->dry_known) {
        app->dry = raw;
        app->dry_known = true;
        status = beep(app->ui, TUNE_TAP);
    } else if (!app->wet_known) {
        app->wet = raw;
        app->wet_known = true;
        status = beep(app->ui, TUNE_GOOD);
    } else {
        app->dry_known = false;
        app->wet_known = false;
        status = beep(app->ui, TUNE_TAP);
    }
    if (status == BENTO_OK) {
        show_points(app);
    }
    return status;
}

/* SW6: a wet point too close to the dry one is refused, so nothing is
 * ever divided by zero. */
static bento_status_t press_point(app_t *app)
{
    uint16_t raw = 0U;
    bento_status_t status = read_probe(&raw);

    if (status == BENTO_OK) {
        if (app->dry_known && !app->wet_known &&
            !far_enough(app->dry, raw)) {
            set_text(app->ui, app->widgets.step, "ใกล้จุดแห้งเกิน");
            status = beep(app->ui, TUNE_BAD);
        } else {
            status = take_point(app, raw);
        }
    }
    return status;
}

static bento_status_t check_buttons(app_t *app)
{
    bool edge = false;
    bento_status_t status = just_pressed(app, (uint32_t)SW5, &edge);

    if ((status == BENTO_OK) && edge) {
        status = take_zero(app);
    }
    if (status == BENTO_OK) {
        status = just_pressed(app, (uint32_t)SW6, &edge);
    }
    if ((status == BENTO_OK) && edge) {
        status = press_point(app);
    }
    return status;
}

/* 1) The offset, every CLIMATE_MS. */
static bento_status_t show_temp(const app_t *app)
{
    /* The board warms itself: compare with a room thermometer and put
     * the difference here, such as -9.5. */
    static const bento_real_t temp_offset = (bento_real_t)0.0;
    char raw_text[BENTO_FMT_BUF_SIZE];
    char offset_text[BENTO_FMT_BUF_SIZE];
    char fixed_text[BENTO_FMT_BUF_SIZE];
    char text[TEXT_BYTES];
    bento_real_t celsius = (bento_real_t)0.0;
    bool known = false;
    bento_status_t status = read_temp_raw(&celsius, &known);

    if ((status == BENTO_OK) && known) {
        format_real(raw_text, celsius);
        format_real(offset_text, temp_offset);
        format_real(fixed_text, celsius + temp_offset);
        (void)snprintf(text, sizeof(text), "%s %s%s = %s", raw_text,
                       (offset_text[0] == '-') ? "" : "+", offset_text,
                       fixed_text);
        set_text(app->ui, app->widgets.temp, text);
    }
    return status;
}

/* 2) The tare: the raw tilt and the tilt with the zero taken off. */
static void show_tilt(const app_t *app)
{
    char text[TEXT_BYTES];
    tilt_t tilt = {(bento_real_t)0.0, (bento_real_t)0.0};

    if (read_tilt(&tilt)) {
        tilt_t fixed;

        format_tilt(text, sizeof(text), "ดิบ ", &tilt);
        set_text(app->ui, app->widgets.tilt_raw, text);
        fixed.roll = tare(tilt.roll, app->zero.roll);
        fixed.pitch = tare(tilt.pitch, app->zero.pitch);
        format_tilt(text, sizeof(text), "", &fixed);
        set_text(app->ui, app->widgets.tilt_fixed, text);
    }
}

/* 3) The two points: raw, percent and the bar. */
static bento_status_t show_probe(const app_t *app)
{
    char text[TEXT_BYTES];
    bento_real_t percent = (bento_real_t)0.0;
    uint16_t raw = 0U;
    bento_status_t status = read_probe(&raw);

    if (status == BENTO_OK) {
        const bool known = two_point(app, raw, &percent);
        /* the widget takes whole numbers only */
        const int32_t whole = known ? round_half_even(percent) : 0;

        (void)snprintf(text, sizeof(text), "raw %u", (unsigned int)raw);
        set_text(app->ui, app->widgets.probe_raw, text);
        if (known) {
            (void)snprintf(text, sizeof(text), "%ld %%", (long)whole);
        } else {
            (void)snprintf(text, sizeof(text), "-- %%");
        }
        set_text(app->ui, app->widgets.percent, text);
        (void)bento_ui_set_value(app->ui, app->widgets.bar, whole);
        poll_screen(app->ui);
    }
    return status;
}

static bento_status_t run_loop(app_t *app)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    uint32_t shown_ms = start_ms;
    uint32_t air_ms = bento_time_ticks_add(start_ms, -CLIMATE_MS);
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        const uint32_t now_ms = bento_time_ticks_ms();

        status = check_buttons(app);
        if ((status == BENTO_OK) &&
            (bento_time_ticks_diff(now_ms, air_ms) >= CLIMATE_MS)) {
            air_ms = now_ms;
            status = show_temp(app);
        }
        if ((status == BENTO_OK) &&
            (bento_time_ticks_diff(now_ms, shown_ms) >= TICK_MS)) {
            shown_ms = now_ms;
            show_tilt(app);
            status = show_probe(app);
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
        show_points(&app);
        status = run_loop(&app);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_color(app.ui, app.widgets.help, COL_WARN);
        set_text(app.ui, app.widgets.help,
                 "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่");
        poll_screen(app.ui);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
