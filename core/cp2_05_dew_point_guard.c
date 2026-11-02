/*
 * cp2_05_dew_point_guard.c -- หลักการ 2.5: ฟีเจอร์จากฟิสิกส์ จุดน้ำค้าง
 * (dew point) เตือนหยดน้ำเกาะ
 *
 * จุดน้ำค้าง Td = อุณหภูมิที่อากาศก้อนนี้ต้องเย็นลงถึง ไอน้ำจึงกลั่นเป็นหยดน้ำ
 * ถ้าอุณหภูมิอากาศ T ห่างจาก Td ไม่เกิน RISK_C ผิวที่เย็นกว่าอากาศนิดเดียว
 * (ใบพืช หลังคา) ก็เปียก Td คิดจากคู่ T ดิบกับ RH ดิบของ SHT40 ส่วน T
 * ของอากาศ = T ดิบ + TEMP_OFFSET แล้วคิด RH ของอากาศกลับจาก Td หายใจรด
 * SHT40 แล้วดูช่องห่าง T - Td แคบลงจนขึ้น "เสี่ยงหยดน้ำเกาะ" จอไฟ RGB
 * เป็นสีฟ้าทั้งจอเมื่อเสี่ยง โปรแกรมเล่น 3 นาทีแล้วจบเอง ใช้กับ TESAIoT
 * Dev Kit และ BENTO Emulator
 *
 * A feature from physics: the dew point Td, the temperature this air
 * must cool to before its water vapour condenses. When the air
 * temperature T is within RISK_C of Td, a slightly cooler surface (a
 * leaf, a roof) gets wet. Td comes from the raw SHT40 pair; T is the
 * raw temperature plus TEMP_OFFSET, and the air's RH is worked back
 * from Td. Breathe on the SHT40 and watch the T - Td gap close until
 * the screen warns of condensation; the RGB matrix turns blue while
 * the risk lasts. It runs for three minutes.
 *
 * Twin of cp2_05_dew_point_guard.py (aiot-development-for-smart-farm/core/cp2_05_dew_point_guard.py). Style: CLEAN_C.md.
 * What C does differently:
 *   - try/except around the SHT40 is a status check with the same
 *     three tries and 50 ms pauses; None is a bool beside the values;
 *   - the RGB matrix's OSError is ignored, as the .py ignores it; a
 *     board without the matrix refuses each call the same way, where
 *     MicroPython's import rgbmatrix would raise;
 *   - dsp.dew_point, dsp.heat_index and dsp.comfort_zone return a
 *     status, which ends the program where MicroPython would raise;
 *   - the settings sit in one block at the top (deviation CT02); the
 *     real ones are float, the type of the sensor and dsp values they
 *     meet; the Magnus A and B are MAGNUS_A and MAGNUS_B (no
 *     one-letter names);
 *   - "%.1f" numbers are formatted by bento_fmt_real_precision, the
 *     board's own float formatting; round() rounds half to even, as
 *     Python's does;
 *   - hasattr(ui, "volume") is not asked: the volume call is always
 *     made, and a firmware without it refuses it, which is ignored;
 *   - ui.tone() and chart points are fire-and-forget in MicroPython,
 *     so their status is ignored here too;
 *   - Stop ends the program at the next sleep and main returns 0,
 *     without the closing line (MicroPython's KeyboardInterrupt).
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_dsp.h"
#include "bento_fmt.h"
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_sensors.h"
#include "bento_time.h"

/* ---- แก้ก่อนรัน: settings (MISRA deviation CT02) ---- */
/* T - Td at or below this = condensation risk, degrees C. */
static const float RISK_C = 2.0F;
/* The board warms its own sensor: compare with a room thermometer and
 * put the difference here, such as -3.0. */
static const float TEMP_OFFSET = 0.0F;
/* The Magnus constants, the set dsp.dew_point uses in the firmware. */
static const float MAGNUS_A = 17.27F; /* .py: A */
static const float MAGNUS_B = 237.7F; /* .py: B */
enum {
    TICK_MS = 1000,  /* read the SHT40 and refresh the screen */
    RUN_MS = 180000, /* play for three minutes, then end */
    SPEAKER = 40,    /* speaker level 0-100 % (firmware 2.4.2+) */
    VOLUME = 25      /* tone loudness 0-127 (about 20 %) */
};
/* ---- end of settings ---- */

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
    CARD_RADIUS = 12,
    CARD_BORDER = 1,
    CARD_TITLE_DX = 12,
    CARD_TITLE_DY = 6,
    TOP_CARD_Y = 64,
    TOP_CARD_H = 120,
    T_CARD_X = 12,
    T_CARD_W = 250,
    TD_CARD_X = 272,
    TD_CARD_W = 250,
    GAP_CARD_X = 532,
    GAP_CARD_W = 248,
    SEG_DX = 12,
    SEG_Y = 98,
    SEG_W = 150,
    GAP_SEG_W = 130,
    SEG_H = 56,
    LED_X = 700,
    LED_Y = 106,
    LED_SIZE = 40,
    HUMIDITY_CARD_X = 12,
    HUMIDITY_CARD_Y = 194,
    HUMIDITY_CARD_W = 380,
    HUMIDITY_CARD_H = 150,
    LINE_X = 24,
    RH_Y = 228,
    RH_AIR_Y = 256,
    FEEL_Y = 284,
    STATE_Y = 312,
    CHART_X = 422,
    LEGEND_Y = 194,
    CHART_Y = 224,
    CHART_W = 358,
    CHART_H = 114,
    CHART_LO = 0,
    CHART_HI = 50,
    CHART_MAX_W = 400,
    CHART_POINTS = 400
};

/* Font sizes, in pixels: Thai text is 20 px or more on the board. */
enum {
    FONT_TITLE = 24,
    FONT_TEXT = 20
};

/* Sound, timing and sizes that are not settings. */
enum {
    SETTLE_MS = 200,      /* let the cleared screen settle */
    RETRY_MS = 50,        /* the SHT40 bus may be busy for a moment */
    SHT40_TRIES = 3,      /* tries before a reading counts as missing */
    TONE_MS = 90,         /* each note of a tune */
    NOTE_GAP_MS = 100,    /* one note to the next */
    TUNE_NOTES = 3,       /* the longest tune */
    TEXT_BYTES = 128,     /* a SET_TEXT carries at most 126 bytes */
    ZONE_NAME_BYTES = 16, /* "comfortable" and its NUL */
    ONE_DIGIT = 1         /* "%.1f" */
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

/* A number as the board's MicroPython prints it. */
typedef struct {
    char text[BENTO_FMT_BUF_SIZE];
} real_text_t;

/* One SHT40 reading, as the sensor gave it. */
typedef struct {
    float t_raw;
    float rh_raw;
} reading_t;

/* The air: its temperature, its dew point and its humidity. */
typedef struct {
    float t;
    float td;
    float rh;
} air_t;

/* The widgets that change after the screen is drawn. */
typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t t_seg;
    bento_ui_id_t td_seg;
    bento_ui_id_t gap_seg;
    bento_ui_id_t led;
    bento_ui_id_t rh;
    bento_ui_id_t rh_air;
    bento_ui_id_t feel;
    bento_ui_id_t state;
    bento_ui_id_t chart;
    uint8_t td_series;
    bento_ui_id_t help;
} screen_t;

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

/* Python's round(): the nearest whole number, a half to the even
 * one. */
static int32_t py_round(float value)
{
    const float half = 0.5F;
    const float below = floorf(value);
    const float rest = value - below;
    int32_t whole = (int32_t)below;

    if ((rest > half) || ((rest == half) && ((whole % 2) != 0))) {
        whole++;
    }
    return whole;
}

/* "%.<digits>f" % value; an empty text if it cannot be made. */
static const char *real_text(real_text_t *out, float value,
                             int32_t digits)
{
    uint32_t length = 0U;

    if (bento_fmt_real_precision(out->text, (uint32_t)sizeof(out->text),
                                 value, digits, &length) != BENTO_OK) {
        out->text[0] = '\0';
    }
    return out->text;
}

/* T raw and RH raw; *found is false after three failed tries. */
static bento_status_t read_sht40(reading_t *reading, bool *found)
{
    bento_status_t status = BENTO_OK;
    int32_t i;

    *found = false;
    for (i = 0; (i < SHT40_TRIES) && !*found && (status == BENTO_OK);
         i++) {
        if ((bento_sensors_sht40_temperature(&reading->t_raw) ==
             BENTO_OK) &&
            (bento_sensors_sht40_humidity(&reading->rh_raw) ==
             BENTO_OK)) {
            *found = true;
        } else {
            status = pause_ms((uint32_t)RETRY_MS);
        }
    }
    return status;
}

/* At risk the whole matrix is blue, else off (fill takes only colours
 * 1-7, so off is clear). */
static void show_matrix(bool risk)
{
    /* the matrix may be slow to answer: this picture is skipped */
    if (risk) {
        (void)bento_rgbmatrix_fill(BENTO_RGBMATRIX_BLUE);
    } else {
        (void)bento_rgbmatrix_clear();
    }
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

/* The RH (%) of air at temperature t whose dew point is td: the
 * ratio of saturation pressures es(td) / es(t). The 6.112 of es()
 * cancels out. */
static float rh_at(float td, float t)
{
    const float full = 100.0F;
    const float dew_term = (MAGNUS_A * td) / (MAGNUS_B + td);
    const float air_term = (MAGNUS_A * t) / (MAGNUS_B + t);
    const float rh = full * expf(dew_term - air_term);

    return (rh < full) ? rh : full;
}

/* Td from the raw pair, T from the offset, RH worked back from Td. */
static bento_status_t work_out_air(const reading_t *reading, air_t *air)
{
    bento_status_t status =
        bento_dsp_dew_point(reading->t_raw, reading->rh_raw, &air->td);

    air->t = reading->t_raw + TEMP_OFFSET;
    if (status == BENTO_OK) {
        air->rh = rh_at(air->td, air->t);
    }
    return status;
}

static bool at_risk(const air_t *air)
{
    return (air->t - air->td) <= RISK_C;
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

/* A card with a Seg7 that starts at "0". */
static bento_status_t add_seg_card(bento_ui_t *ui, const rect_t *box,
                                   const char *title, int32_t seg_w,
                                   uint32_t color,
                                   bento_ui_id_t *out_id)
{
    bento_ui_widget_args_t args;
    bento_status_t status = add_card(ui, box, title);

    bento_ui_widget_args_init(&args);
    args.text = "0";
    args.text_len = 1U;
    args.x = px(box->x + SEG_DX);
    args.y = SEG_Y;
    args.w = px(seg_w);
    args.h = SEG_H;
    args.color = color;
    if (status == BENTO_OK) {
        status = bento_ui_seg7_create(ui, &args, out_id);
    }
    return status;
}

static bento_status_t add_led(screen_t *screen)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.x = LED_X;
    args.y = LED_Y;
    args.w = LED_SIZE;
    args.h = LED_SIZE;
    args.color = COL_BAD;
    args.font_px = 0;
    return bento_ui_led_create(screen->ui, &args, &screen->led);
}

/* A smooth line chart with no dots: LVGL draws no dots once the
 * points reach the chart's width, so it is at most 400 wide and has
 * 400 points (the firmware takes 10-400). */
static bento_status_t add_chart(screen_t *screen)
{
    bento_ui_widget_args_t args;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = CHART_X;
    args.y = CHART_Y;
    args.w = (CHART_W < CHART_MAX_W) ? CHART_W : CHART_MAX_W;
    args.h = CHART_H;
    args.color = COL_WARN;
    args.min_val = CHART_LO;
    args.max_val = CHART_HI;
    status = bento_ui_chart_create(screen->ui, &args, &screen->chart);
    if (status == BENTO_OK) {
        /* a setter: MicroPython ignores its refusal too */
        (void)bento_ui_set_prop(screen->ui, screen->chart,
                                (uint8_t)BENTO_UI_PROP_CHART_POINTS,
                                CHART_POINTS);
        status = bento_ui_chart_add_series(
            screen->ui, screen->chart, COL_INFO, &screen->td_series);
    }
    return status;
}

/* The three cards along the top, each with its big number. */
static bento_status_t add_top_cards(screen_t *screen)
{
    const rect_t t_box = {T_CARD_X, TOP_CARD_Y, T_CARD_W, TOP_CARD_H};
    const rect_t td_box = {TD_CARD_X, TOP_CARD_Y, TD_CARD_W,
                           TOP_CARD_H};
    const rect_t gap_box = {GAP_CARD_X, TOP_CARD_Y, GAP_CARD_W,
                            TOP_CARD_H};
    bento_status_t status =
        add_seg_card(screen->ui, &t_box, "อากาศ T (C)", SEG_W, COL_WARN,
                     &screen->t_seg);

    if (status == BENTO_OK) {
        status = add_seg_card(screen->ui, &td_box, "จุดน้ำค้าง Td (C)",
                              SEG_W, COL_INFO, &screen->td_seg);
    }
    if (status == BENTO_OK) {
        status = add_seg_card(screen->ui, &gap_box, "ห่าง T - Td",
                              GAP_SEG_W, COL_OK, &screen->gap_seg);
    }
    if (status == BENTO_OK) {
        status = add_led(screen);
    }
    return status;
}

/* The humidity card and its four lines. */
static bento_status_t add_humidity_card(screen_t *screen)
{
    const rect_t box = {HUMIDITY_CARD_X, HUMIDITY_CARD_Y,
                        HUMIDITY_CARD_W, HUMIDITY_CARD_H};
    const label_spec_t rh = {LINE_X, RH_Y, FONT_TEXT, COL_TEXT};
    const label_spec_t rh_air = {LINE_X, RH_AIR_Y, FONT_TEXT, COL_TEXT};
    const label_spec_t feel = {LINE_X, FEEL_Y, FONT_TEXT, COL_DIM};
    const label_spec_t state = {LINE_X, STATE_Y, FONT_TEXT, COL_OK};
    bento_status_t status =
        add_card(screen->ui, &box, "ความชื้น และฟีเจอร์อื่น");

    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", &rh, &screen->rh);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", &rh_air, &screen->rh_air);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", &feel, &screen->feel);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", &state, &screen->state);
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

/* The title and the rule under it. */
static bento_status_t add_heading(bento_ui_t *ui)
{
    const label_spec_t title = {MARGIN_X, TITLE_Y, FONT_TITLE,
                                COL_TEXT};
    const label_spec_t subtitle = {MARGIN_X, SUBTITLE_Y, FONT_TEXT,
                                   COL_DIM};
    real_text_t risk;
    char text[TEXT_BYTES];
    bento_ui_id_t unused;
    bento_status_t status =
        add_label(ui, "จุดน้ำค้าง: เตือนหยดน้ำเกาะ", &title, &unused);

    (void)snprintf(text, sizeof(text), "เสี่ยงเมื่อ T - Td <= %s C",
                   real_text(&risk, RISK_C, ONE_DIGIT));
    if (status == BENTO_OK) {
        status = add_label(ui, text, &subtitle, &unused);
    }
    return status;
}

static bento_status_t build_screen(screen_t *screen)
{
    const label_spec_t legend = {CHART_X, LEGEND_Y, FONT_TEXT, COL_DIM};
    const label_spec_t help = {MARGIN_X, HELP_Y, FONT_TEXT, COL_DIM};
    bento_ui_id_t unused;
    bento_status_t status =
        bento_ui_screen(screen->ui, BENTO_RUNTIME_SCREEN_WIDTH_PX,
                        BENTO_RUNTIME_SCREEN_HEIGHT_PX);

    if (status == BENTO_OK) {
        status = pause_ms((uint32_t)SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_heading(screen->ui);
    }
    if (status == BENTO_OK) {
        status = add_top_cards(screen);
    }
    if (status == BENTO_OK) {
        status = add_humidity_card(screen);
    }
    if (status == BENTO_OK) {
        status =
            add_label(screen->ui, "ส้ม = T   ฟ้า = Td", &legend, &unused);
    }
    if (status == BENTO_OK) {
        status = add_chart(screen);
    }
    if (status == BENTO_OK) {
        status =
            add_label(screen->ui, "หายใจรด SHT40 แล้วดูช่องห่าง T - Td",
                      &help, &screen->help);
    }
    if (status == BENTO_OK) {
        poll_screen(screen->ui);
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

/* The three big numbers, the LED and the chart. */
static void show_numbers(const screen_t *screen, const air_t *air,
                         bool risk)
{
    real_text_t number;

    set_text(screen->ui, screen->t_seg,
             real_text(&number, air->t, ONE_DIGIT));
    set_text(screen->ui, screen->td_seg,
             real_text(&number, air->td, ONE_DIGIT));
    set_text(screen->ui, screen->gap_seg,
             real_text(&number, air->t - air->td, ONE_DIGIT));
    (void)bento_ui_set_color(screen->ui, screen->gap_seg,
                             risk ? COL_BAD : COL_OK);
    (void)bento_ui_set_value(screen->ui, screen->led, risk ? 1 : 0);
    /* chart points are fire-and-forget in MicroPython too */
    (void)bento_ui_chart_set_next(screen->ui, screen->chart, 0U,
                                  py_round(air->t));
    (void)bento_ui_chart_set_next(screen->ui, screen->chart,
                                  screen->td_series, py_round(air->td));
}

/* The two humidity lines: the sensor's RH and the air's. */
static void show_humidity(const screen_t *screen,
                          const reading_t *reading, const air_t *air)
{
    real_text_t rh;
    real_text_t t_raw;
    char text[TEXT_BYTES];

    (void)snprintf(text, sizeof(text), "RH ดิบ %s %% (ที่ %s C)",
                   real_text(&rh, reading->rh_raw, 0),
                   real_text(&t_raw, reading->t_raw, ONE_DIGIT));
    set_text(screen->ui, screen->rh, text);
    (void)snprintf(text, sizeof(text), "RH อากาศ %s %% (คำนวณจาก Td)",
                   real_text(&rh, air->rh, 0));
    set_text(screen->ui, screen->rh_air, text);
}

/* How the air feels to a person, and the state line. */
static bento_status_t show_feel(const screen_t *screen,
                                const air_t *air, bool risk)
{
    float feel_c = 0.0F;
    uint32_t zone = 0U;
    uint32_t length = 0U;
    char zone_name[ZONE_NAME_BYTES];
    real_text_t feel;
    char text[TEXT_BYTES];
    bento_status_t status =
        bento_dsp_heat_index(air->t, air->rh, &feel_c);

    if (status == BENTO_OK) {
        status = bento_dsp_comfort_zone(air->t, air->rh, &zone);
    }
    if (status == BENTO_OK) {
        status = bento_dsp_comfort_zone_name(
            zone, zone_name, (uint32_t)sizeof(zone_name), &length);
    }
    if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text), "รู้สึกเหมือน %s C  คน: %s",
                       real_text(&feel, feel_c, ONE_DIGIT), zone_name);
        set_text(screen->ui, screen->feel, text);
        set_text(screen->ui, screen->state,
                 risk ? "เสี่ยงหยดน้ำเกาะ - เปิดพัดลม" : "ปลอดภัย");
        (void)bento_ui_set_color(screen->ui, screen->state,
                                 risk ? COL_BAD : COL_OK);
    }
    return status;
}

/* The decision and its action: the matrix and a tune, only when the
 * state changes (no tune for the first decision). */
static bento_status_t decide(const screen_t *screen, const air_t *air,
                             bool *risk_known, bool *risk)
{
    const bool was_known = *risk_known;
    const bool was = *risk;
    bento_status_t status = BENTO_OK;

    *risk = at_risk(air);
    *risk_known = true;
    if (!was_known || (*risk != was)) {
        show_matrix(*risk);
    }
    if (was_known && (*risk != was)) {
        status = beep(screen->ui, *risk ? TUNE_BAD : TUNE_GOOD);
    }
    return status;
}

/* One reading: the features, the decision, the action, the screen. */
static bento_status_t handle_reading(const screen_t *screen,
                                     const reading_t *reading,
                                     bool *risk_known, bool *risk)
{
    air_t air;
    bento_status_t status = work_out_air(reading, &air);

    if (status == BENTO_OK) {
        status = decide(screen, &air, risk_known, risk);
    }
    if (status == BENTO_OK) {
        show_numbers(screen, &air, *risk);
        show_humidity(screen, reading, &air);
        status = show_feel(screen, &air, *risk);
    }
    return status;
}

static bento_status_t run_loop(const screen_t *screen)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bool risk_known = false;
    bool risk = false;
    int32_t fails = 0;
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        reading_t reading = {0.0F, 0.0F};
        bool found = false;

        status = read_sht40(&reading, &found);
        if ((status == BENTO_OK) && !found) {
            char text[TEXT_BYTES];

            fails++;
            (void)snprintf(text, sizeof(text), "อ่าน SHT40 ไม่ได้ %ld ครั้ง",
                           (long)fails);
            set_text(screen->ui, screen->help, text);
        } else if (status == BENTO_OK) {
            status =
                handle_reading(screen, &reading, &risk_known, &risk);
        } else {
            /* Stop during a retry */
        }
        if (status == BENTO_OK) {
            poll_screen(screen->ui);
            status = pause_ms((uint32_t)TICK_MS);
        }
    }
    return status;
}

int main(void)
{
    screen_t screen;
    bento_status_t status;

    (void)memset(&screen, 0, sizeof(screen));
    screen.ui = bento_runtime_get_ui();
    /* firmware 2.4.1 has no volume and refuses it; the .py skips it */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    status = build_screen(&screen);
    if (status == BENTO_OK) {
        show_matrix(false);
        status = run_loop(&screen);
    }
    if (status == BENTO_OK) {
        show_matrix(false);
        status = beep(screen.ui, TUNE_GOOD);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_color(screen.ui, screen.help, COL_WARN);
        set_text(screen.ui, screen.help,
                 "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่");
        poll_screen(screen.ui);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
