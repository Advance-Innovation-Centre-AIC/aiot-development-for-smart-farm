/*
 * cp4_01_one_value_many_faces.c -- หลักการ 4.1: ค่าเดียว หลายหน้าตา
 * (One value, many faces: pick the widget by the kind of data)
 *
 * ค่าเดียว (VR1 = ความชื้นดิน 0-100 %) แสดงได้ 7 แบบ: ตัวเลขใช้ Label กับ
 * Seg7 เปอร์เซ็นต์ใช้ Bar กับ Arc หน้าปัดใช้ Scale กับเข็ม สถานะใช้ Led
 * ประวัติใช้ Chart widget สร้างครั้งเดียว แล้วส่งค่าใหม่ไปเฉพาะตอนค่าเปลี่ยน
 * เกิน DEAD_BAND หมุน VR1 ช้า ๆ ดูทุกหน้าตาขยับพร้อมกัน ปล่อยนิ่ง ตัวนับ
 * "ส่งไปจอ" หยุด แต่กราฟยังเดิน ต่ำกว่า 25 % ไฟสถานะสว่างสุดและมีเสียงเตือน
 * เล่น 3 นาทีแล้วจบเอง
 *
 * One value (VR1 as soil moisture, 0-100 %) is shown seven ways:
 * a number as a Label and a Seg7, a percentage as a Bar and an Arc, a
 * dial as a Scale with a needle, a state as a Led, a history as a
 * Chart. The widgets are made once; a new value goes to the screen only
 * when it moved by DEAD_BAND or more. Turn VR1 slowly and watch every
 * face move together. Leave it still: the "sent to screen" count stops,
 * the chart keeps going. Below 25 % the status light is brightest and
 * one alarm sounds. The program ends after three minutes.
 *
 * Twin of cp4_01_one_value_many_faces.py
 * (aiot-development-for-smart-farm/core/cp4_01_one_value_many_faces.py).
 * Style: CLEAN_C.md.
 * Differs from the original:
 *   - ui.volume has no hasattr test: a board without it answers
 *     BENTO_E_UNSUPPORTED, which is skipped as the .py skips the line.
 *   - "Not shown yet" (the .py's None) is a flag for the value and -1
 *     for the zone.
 *   - The tune table keeps only the tunes this program plays.
 *   - Stop ends the program at once, without the closing line, as
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
    DEAD_BAND = 1,  /* send to the screen when the value moved this % */
    DRY_BELOW = 25, /* below this: dry soil, brightest status light */
    WARN_BELOW = 40, /* below this: getting dry */
    NEEDLE_LEN = 40, /* needle length (px), shorter than the numbers */
    TICK_MS = 500,   /* read and refresh this often */
    RUN_MS = 180000, /* the program ends after three minutes */
    SPEAKER = 40,    /* speaker volume, 0-100 % */
    VOLUME = 25      /* tone velocity, 0-127 (about 20 %) */
};

/* The knob, the zones and the needle. */
enum {
    PERCENT = 100,
    ADC_FULL = 4095, /* pots.read() at the right end */
    VR1 = 0,
    ZONE_COUNT = 3,
    ZONE_NONE = -1,   /* no zone shown yet */
    NEEDLE_SHIFT = 16 /* the needle length sits in the top 16 bits */
};

/* Sound. */
enum {
    NOTE_MS = 90,
    NOTE_GAP_MS = 100,
    TUNE_NOTES_MAX = 2
};

/* The tunes this program plays. */
typedef enum {
    TUNE_START = 0,
    TUNE_BAD
} tune_t;

/* Screen layout. */
enum {
    MARGIN_X = 12,
    TITLE_Y = 6,
    HINT_Y = 38,
    CARD_Y = 64,
    CARD_SIZE = 186,
    CARD_RADIUS = 12,
    CARD_TEXT_X = 12,
    CARD_TITLE_Y = 70,
    CARD_CAPTION_Y = 226,
    NUMBER_CARD_X = 12,
    PERCENT_CARD_X = 206,
    GAUGE_CARD_X = 400,
    LED_CARD_X = 594,
    NUMBER_X = 24,
    NUMBER_Y = 100,
    DIGITS_Y = 156,
    DIGITS_W = 160,
    DIGITS_H = 40,
    BAR_X = 218,
    BAR_Y = 100,
    BAR_W = 162,
    BAR_H = 20,
    ARC_X = 254,
    ARC_Y = 130,
    ARC_SIZE = 90,
    GAUGE_X = 430,
    GAUGE_Y = 94,
    GAUGE_SIZE = 126,
    TICKS_TOTAL = 11, /* 11 ticks, a number every 2: 0 20 ... 100 */
    TICKS_MAJOR = 2,
    LED_X = 656,
    LED_Y = 104,
    LED_SIZE = 60,
    ZONE_X = 606,
    ZONE_Y = 184,
    CHART_X = 12,
    CHART_Y = 262,
    CHART_W = 400, /* at most 400 wide: 400 points draw a smooth line */
    CHART_H = 76,
    CHART_POINTS = 400,
    SIDE_X = 424,
    CHART_NOTE_Y = 266,
    SENT_Y = 296,
    HELP_Y = 352,
    FONT_TINY = 14,
    FONT_SMALL = 16,
    FONT_ZONE = 20,
    FONT_TITLE = 24,
    FONT_NUMBER = 28,
    SETTLE_MS = 200, /* let the cleared screen settle */
    TEXT_BYTES = 80  /* "อ่าน %d ครั้ง ส่งไปจอ %d ครั้ง", any int32_t */
};

/* Colours, 0xRRGGBB. */
enum {
    COL_TEXT = 0xE8EAED,
    COL_DIM = 0x9AA3AF,
    COL_CARD = 0x171B22,
    COL_OK = 0x30A46C,
    COL_WARN = 0xF5A623,
    COL_BAD = 0xE5484D,
    COL_INFO = 0x4A9EFF
};

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

/* The widgets the program changes after building the screen. */
typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t number;
    bento_ui_id_t digits;
    bento_ui_id_t bar;
    bento_ui_id_t arc;
    bento_ui_id_t gauge;
    bento_ui_id_t led;
    bento_ui_id_t zone;
    bento_ui_id_t chart;
    bento_ui_id_t sent;
    bento_ui_id_t help;
} faces_screen_t;

typedef struct {
    faces_screen_t screen;
    bool has_shown;
    int32_t shown;
    int32_t zone;
    int32_t reads;
    int32_t sent;
} faces_t;

/* The one place a layout number narrows to the API's int16_t. */
static int16_t px(int32_t value)
{
    return (int16_t)value;
}

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

/* One screen refresh; answers how many events came (none read). */
static uint32_t present(bento_ui_t *ui)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    /* ui.poll() answers no events when it fails */
    if (bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                      NULL) != BENTO_OK) {
        count = 0U;
    }
    return count;
}

/* Empties the event queue: this program reads no events, but a full
 * queue drops the new ones. */
static void drain_events(bento_ui_t *ui)
{
    while (present(ui) != 0U) {
    }
}

static void set_text(bento_ui_t *ui, bento_ui_id_t widget,
                     const char *text)
{
    /* a setter's refusal was ignored before too (modui.c) */
    (void)bento_ui_set_text(ui, widget, text, (uint32_t)strlen(text));
}

/* Plays a tune note by note, 100 ms apart. */
static bento_status_t beep(bento_ui_t *ui, tune_t tune)
{
    static const uint8_t notes[][TUNE_NOTES_MAX] = {{72U, 79U},
                                                    {84U, 76U}};
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < (uint32_t)TUNE_NOTES_MAX) && (status == BENTO_OK);
         i++) {
        /* ui.tone() sends and waits for no answer (modui.c) */
        (void)bento_ui_sfx_tone(ui, notes[tune][i],
                                (uint8_t)BENTO_UI_WAVE_SINE,
                                (uint8_t)VOLUME, (uint16_t)NOTE_MS);
        status = sleep_ms((uint32_t)NOTE_GAP_MS);
    }
    return status;
}

/* VR1 stands in for a soil moisture sensor: 0 bone dry, 100 soaked. */
static bento_status_t soil_percent(int32_t *percent)
{
    uint16_t raw = 0U;
    const bento_status_t status = bento_pots_read(VR1, &raw);

    *percent = ((int32_t)raw * PERCENT) / ADC_FULL;
    return status;
}

/* Send a new value? Not shown yet, or DEAD_BAND or more away. */
static bool changed(int32_t value, const faces_t *faces)
{
    const int32_t gap = (value > faces->shown) ? (value - faces->shown)
                                               : (faces->shown - value);

    return !faces->has_shown || (gap >= DEAD_BAND);
}

/* 0 moist enough, 1 getting dry, 2 dry. */
static int32_t zone_of(int32_t value)
{
    int32_t zone = 0;

    if (value < DRY_BELOW) {
        zone = 2;
    } else if (value < WARN_BELOW) {
        zone = 1;
    } else {
        /* moist enough */
    }
    return zone;
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

/* One card per face: the title names the widget, the line at the
 * bottom the kind of data it suits. */
static bento_status_t add_card(bento_ui_t *ui, int32_t x,
                               const char *title, const char *caption)
{
    const rect_t box = {px(x), CARD_Y, CARD_SIZE, CARD_SIZE};
    const point_t title_at = {px(x + CARD_TEXT_X), CARD_TITLE_Y};
    const point_t caption_at = {px(x + CARD_TEXT_X), CARD_CAPTION_Y};
    bento_ui_widget_args_t args;
    bento_ui_id_t unused;
    bento_status_t status;

    place(&args, box);
    args.color = COL_CARD;
    args.min_val = COL_DIM;
    args.max_val = CARD_RADIUS;
    args.font_px = 1;
    status = bento_ui_panel_create(ui, &args, &unused);
    if (status == BENTO_OK) {
        status = add_label(ui, title, title_at, COL_INFO, FONT_SMALL,
                           &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(ui, caption, caption_at, COL_DIM, FONT_TINY,
                           &unused);
    }
    return status;
}

static bento_status_t add_number_card(faces_screen_t *screen)
{
    const point_t number_at = {NUMBER_X, NUMBER_Y};
    const rect_t box = {NUMBER_X, DIGITS_Y, DIGITS_W, DIGITS_H};
    bento_ui_widget_args_t args;
    bento_status_t status = add_card(screen->ui, NUMBER_CARD_X,
                                     "Label + Seg7", "ตัวเลข: อ่านค่าแม่น");

    if (status == BENTO_OK) {
        status = add_label(screen->ui, "-- %", number_at, COL_TEXT,
                           FONT_NUMBER, &screen->number);
    }
    if (status == BENTO_OK) {
        place(&args, box);
        args.text = "0";
        args.text_len = 1U;
        args.color = COL_INFO;
        status =
            bento_ui_seg7_create(screen->ui, &args, &screen->digits);
    }
    return status;
}

/* A Bar or an Arc from 0 to 100 that starts at 0; its colour can only
 * be set after it is made. */
static bento_status_t add_percent(bento_ui_t *ui, rect_t box, bool arc,
                                  bento_ui_id_t *out)
{
    bento_ui_widget_args_t args;
    bento_status_t status;

    place(&args, box);
    args.min_val = 0;
    args.max_val = PERCENT;
    status = arc ? bento_ui_arc_create(ui, &args, out)
                 : bento_ui_bar_create(ui, &args, out);
    if (status == BENTO_OK) {
        (void)bento_ui_set_color(ui, *out, COL_INFO);
    }
    return status;
}

static bento_status_t add_percent_card(faces_screen_t *screen)
{
    const rect_t bar_box = {BAR_X, BAR_Y, BAR_W, BAR_H};
    const rect_t arc_box = {ARC_X, ARC_Y, ARC_SIZE, ARC_SIZE};
    bento_status_t status = add_card(screen->ui, PERCENT_CARD_X,
                                     "Bar + Arc", "เปอร์เซ็นต์: เต็มแค่ไหน");

    if (status == BENTO_OK) {
        status = add_percent(screen->ui, bar_box, false, &screen->bar);
    }
    if (status == BENTO_OK) {
        status = add_percent(screen->ui, arc_box, true, &screen->arc);
    }
    return status;
}

/* A round dial with the numbers inside and a needle the program
 * moves. */
static bento_status_t add_gauge_card(faces_screen_t *screen)
{
    const rect_t box = {GAUGE_X, GAUGE_Y, GAUGE_SIZE, GAUGE_SIZE};
    bento_ui_widget_args_t args;
    bento_status_t status = add_card(screen->ui, GAUGE_CARD_X,
                                     "Scale + เข็ม", "หน้าปัด: อยู่ช่วงไหน");

    if (status == BENTO_OK) {
        place(&args, box);
        args.color = COL_TEXT;
        args.min_val = 0;
        args.max_val = PERCENT;
        status =
            bento_ui_scale_create(screen->ui, &args, &screen->gauge);
    }
    if (status == BENTO_OK) {
        /* prop() and ticks() send and wait for no answer (modui.c) */
        (void)bento_ui_set_prop(screen->ui, screen->gauge,
                                (uint8_t)BENTO_UI_PROP_SCALE_MODE,
                                BENTO_UI_SCALE_ROUND_IN);
        (void)bento_ui_set_ticks(screen->ui, screen->gauge, TICKS_TOTAL,
                                 TICKS_MAJOR);
        (void)bento_ui_set_prop(
            screen->ui, screen->gauge,
            (uint8_t)BENTO_UI_PROP_SCALE_NEEDLE_COLOR, COL_WARN);
    }
    return status;
}

static bento_status_t add_led_card(faces_screen_t *screen)
{
    const rect_t box = {LED_X, LED_Y, LED_SIZE, LED_SIZE};
    const point_t zone_at = {ZONE_X, ZONE_Y};
    bento_ui_widget_args_t args;
    bento_status_t status =
        add_card(screen->ui, LED_CARD_X, "Led", "สถานะ: ดีหรือไม่ดี");

    if (status == BENTO_OK) {
        place(&args, box);
        args.color = COL_BAD;
        args.font_px = 1;
        status = bento_ui_led_create(screen->ui, &args, &screen->led);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", zone_at, COL_TEXT,
                           FONT_ZONE, &screen->zone);
    }
    return status;
}

/* A smooth line without round dots: LVGL draws no dots when the
 * points are as many as the chart is wide. */
static bento_status_t add_history(faces_screen_t *screen)
{
    const rect_t box = {CHART_X, CHART_Y, CHART_W, CHART_H};
    const point_t note_at = {SIDE_X, CHART_NOTE_Y};
    const point_t sent_at = {SIDE_X, SENT_Y};
    bento_ui_widget_args_t args;
    bento_ui_id_t unused;
    bento_status_t status;

    place(&args, box);
    args.color = COL_INFO;
    args.min_val = 0;
    args.max_val = PERCENT;
    status = bento_ui_chart_create(screen->ui, &args, &screen->chart);
    if (status == BENTO_OK) {
        /* prop() sends and waits for no answer (modui.c) */
        (void)bento_ui_set_prop(screen->ui, screen->chart,
                                (uint8_t)BENTO_UI_PROP_CHART_POINTS,
                                CHART_POINTS);
        status = add_label(screen->ui, "Chart = ประวัติ: ขึ้นหรือลง",
                           note_at, COL_DIM, FONT_TINY, &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", sent_at, COL_TEXT,
                           FONT_SMALL, &screen->sent);
    }
    return status;
}

static bento_status_t add_headings(bento_ui_t *ui)
{
    const point_t title_at = {MARGIN_X, TITLE_Y};
    const point_t hint_at = {MARGIN_X, HINT_Y};
    bento_ui_id_t unused;
    bento_status_t status = add_label(ui, "ค่าเดียว หลายหน้าตา", title_at,
                                      COL_TEXT, FONT_TITLE, &unused);

    if (status == BENTO_OK) {
        status = add_label(
            ui, "VR1 = ความชื้นดิน 0-100 %  เลือก widget ตามชนิดข้อมูล",
            hint_at, COL_DIM, FONT_SMALL, &unused);
    }
    return status;
}

/* Everything on the screen, made once. */
static bento_status_t build_screen(faces_screen_t *screen)
{
    const point_t help_at = {MARGIN_X, HELP_Y};
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = sleep_ms((uint32_t)SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_headings(screen->ui);
    }
    if (status == BENTO_OK) {
        status = add_number_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_percent_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_gauge_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_led_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_history(screen);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui,
                           "หมุน VR1 ช้า ๆ  ปล่อยนิ่ง = ไม่ต้องส่งค่าไปจอ",
                           help_at, COL_DIM, FONT_SMALL, &screen->help);
    }
    if (status == BENTO_OK) {
        (void)present(screen->ui);
    }
    return status;
}

/* The Scale has no needle of its own: its length goes in the top 16
 * bits, the value it points at below. */
static void set_needle(const faces_screen_t *screen, int32_t value)
{
    static const uint32_t needle_value_bits = 0xFFFFU;
    const uint32_t packed =
        ((uint32_t)NEEDLE_LEN << (uint32_t)NEEDLE_SHIFT) |
        ((uint32_t)value & needle_value_bits);

    (void)bento_ui_set_prop(screen->ui, screen->gauge,
                            (uint8_t)BENTO_UI_PROP_SCALE_NEEDLE,
                            (int32_t)packed);
}

/* The faces of one value, sent only when it changed. */
static void show_value(const faces_screen_t *screen, int32_t value)
{
    char text[TEXT_BYTES];

    (void)snprintf(text, sizeof(text), "%" PRId32 " %%", value);
    set_text(screen->ui, screen->number, text);
    (void)snprintf(text, sizeof(text), "%" PRId32, value);
    set_text(screen->ui, screen->digits, text);
    /* the widget setters ignore a refusal, as modui.c does */
    (void)bento_ui_set_value(screen->ui, screen->bar, value);
    (void)bento_ui_set_value(screen->ui, screen->arc, value);
    set_needle(screen, value);
}

/* The status light: brighter means look here. */
static void show_zone(const faces_screen_t *screen, int32_t zone)
{
    static const uint32_t zone_cols[ZONE_COUNT] = {COL_OK, COL_WARN,
                                                   COL_BAD};
    /* Led brightness per zone; the firmware keeps about 80 at least, so
     * the light never goes dark. */
    static const int32_t bright[ZONE_COUNT] = {80, 170, 255};
    static const char *const zone_names[ZONE_COUNT] = {"ชื้นพอ", "เริ่มแห้ง",
                                                       "แห้ง!"};

    (void)bento_ui_set_prop(screen->ui, screen->led,
                            (uint8_t)BENTO_UI_PROP_LED_BRIGHTNESS,
                            bright[zone]);
    set_text(screen->ui, screen->zone, zone_names[zone]);
    (void)bento_ui_set_color(screen->ui, screen->zone, zone_cols[zone]);
}

/* A new zone: worse sounds the alarm, better the ok tune, the first
 * one is quiet. */
static bento_status_t change_zone(faces_t *faces, int32_t zone)
{
    bento_status_t status = BENTO_OK;

    if ((faces->zone != ZONE_NONE) && (zone > faces->zone)) {
        status = beep(faces->screen.ui, TUNE_BAD);
    } else if (faces->zone != ZONE_NONE) {
        status = beep(faces->screen.ui, TUNE_START);
    } else {
        /* the first zone: no sound */
    }
    if (status == BENTO_OK) {
        show_zone(&faces->screen, zone);
        faces->zone = zone;
    }
    return status;
}

/* One round: read, decide, show only what changed. */
static bento_status_t run_round(faces_t *faces)
{
    char text[TEXT_BYTES];
    int32_t value = 0;
    bento_status_t status = soil_percent(&value);

    if (status == BENTO_OK) {
        faces->reads++;
        if (changed(value, faces)) {
            show_value(&faces->screen, value);
            faces->shown = value;
            faces->has_shown = true;
            faces->sent++;
        }
        if (zone_of(value) != faces->zone) {
            status = change_zone(faces, zone_of(value));
        }
    }
    if (status == BENTO_OK) {
        /* the chart is time, so it gets a point every round */
        (void)bento_ui_chart_set_next(faces->screen.ui,
                                      faces->screen.chart, 0U, value);
        (void)snprintf(text, sizeof(text),
                       "อ่าน %" PRId32 " ครั้ง ส่งไปจอ %" PRId32 " ครั้ง",
                       faces->reads, faces->sent);
        set_text(faces->screen.ui, faces->screen.sent, text);
        drain_events(faces->screen.ui);
        status = sleep_ms((uint32_t)TICK_MS);
    }
    return status;
}

static bento_status_t run_loop(faces_t *faces)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = run_round(faces);
    }
    return status;
}

int main(void)
{
    faces_t faces;
    bento_status_t status;

    (void)memset(&faces, 0, sizeof(faces));
    faces.screen.ui = bento_runtime_get_ui();
    faces.zone = ZONE_NONE;
    /* a board without ui.volume answers BENTO_E_UNSUPPORTED and is
     * skipped as the .py skips it; elsewhere it waits for no answer */
    (void)bento_ui_sfx_volume_set(faces.screen.ui, SPEAKER);
    status = build_screen(&faces.screen);
    if (status == BENTO_OK) {
        status = run_loop(&faces);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_color(faces.screen.ui, faces.screen.help,
                                 COL_WARN);
        set_text(faces.screen.ui, faces.screen.help,
                 "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่");
        (void)present(faces.screen.ui);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
