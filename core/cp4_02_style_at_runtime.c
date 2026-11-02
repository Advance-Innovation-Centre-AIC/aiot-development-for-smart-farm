/*
 * cp4_02_style_at_runtime.c -- หลักการ 4.2: สไตล์เปลี่ยนตามค่า ตอนโปรแกรมรัน
 * (Style changes with the value, while the program runs)
 *
 * การ์ดใบเดิม แต่งใหม่ระหว่างรันด้วยปุ่มปรับเท่าที่เฟิร์มแวร์มีจริง: สีตามโซน
 * สีเข็มหน้าปัด ขีดหน้าปัด แถบเตือนที่โผล่และหาย ปุ่มที่ห้ามกดตอนถังต่ำ และ
 * ไฟเตือนที่เต้นด้วยการเปลี่ยนขนาด หมุน VR1 (ความชื้นดิน) ลงช้า ๆ ดูสีเปลี่ยน
 * เขียว ส้ม แดง หมุน VR2 (น้ำในถัง) ต่ำกว่า 20 % ปุ่ม "เปิดปั๊ม" เป็นสีเทา
 * และกดไม่ติด ทุกคำสั่งแต่งจอส่งเฉพาะตอนสถานะเปลี่ยน เล่น 3 นาทีแล้วจบเอง
 *
 * The same card, restyled while the program runs with the knobs the
 * firmware really has: colour by zone, the gauge needle colour, the
 * gauge ticks, a warning banner that shows and hides, a button that
 * is disabled while the tank is low, and an alarm light that pulses
 * by changing its size. Turn VR1 (soil moisture) down slowly to see
 * green, orange, red; turn VR2 (water in the tank) below 20 % and the
 * "pump" button turns grey and stops answering. Every restyle is sent
 * only when a state changes. The program ends after three minutes.
 *
 * Twin of cp4_02_style_at_runtime.py
 * (aiot-development-for-smart-farm/core/cp4_02_style_at_runtime.py).
 * Style: CLEAN_C.md.
 * Differs from the original:
 *   - ui.volume has no hasattr test: a board without it answers
 *     BENTO_E_UNSUPPORTED, which is skipped as the .py skips the line.
 *   - disable()/enable() statuses are ignored: where the Emulator has
 *     no such call the .py catches AttributeError and goes on.
 *   - Events are compared by their BENTO_UI_EVENT_* type and widget
 *     id, never by the name "clicked".
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

/* ---- แก้ก่อนรัน: settings (MISRA deviation CT02) ---- */
enum {
    DRY_BELOW = 25,    /* below this the soil is dry: red and banner */
    WARN_BELOW = 40,   /* below this it is getting dry: orange */
    TANK_MIN = 20,     /* below this % the pump must not start */
    TICKS_TOTAL = 11,  /* ticks on the gauge */
    TICKS_MAJOR = 2,   /* a number every so many ticks */
    NEEDLE_LEN = 50,   /* gauge needle length, px */
    CARD_RADIUS = 18,  /* the card's corner radius (Panel max=) */
    CARD_BORDER_W = 3, /* the card's border width (Panel value=) */
    LED_SMALL = 36,    /* the alarm light at rest */
    LED_BIG = 50,      /* the alarm light on the beat */
    LOOP_MS = 30,      /* read the screen's events this often */
    TICK_MS = 500,     /* read the knobs and restyle this often */
    RUN_MS = 180000,   /* the program ends after three minutes */
    SPEAKER = 40,      /* speaker volume, 0-100 % */
    VOLUME = 25        /* tone velocity, 0-127 (about 20 %) */
};
/* .py: BTN_NAMES = ("SW5", "SW6"): this file reads no board button */
/* ---- end of settings ---- */

/* Knobs, zones and the needle. */
enum {
    PERCENT = 100,
    ADC_FULL = 4095, /* pots.read() at the right end */
    VR1 = 0,
    VR2 = 1,
    ZONE_OK = 0,
    ZONE_DRY = 2,
    ZONE_COUNT = 3,
    UNKNOWN = -1,     /* nothing shown yet */
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
    TUNE_TAP = 0,
    TUNE_START,
    TUNE_BAD
} tune_t;

/* Screen layout. */
enum {
    MARGIN_X = 12,
    TITLE_Y = 6,
    HINT_Y = 38,
    CARD_X = 12,
    CARD_Y = 64,
    CARD_W = 470,
    CARD_H = 274,
    CARD_TITLE_X = 28,
    CARD_TITLE_Y = 74,
    GAUGE_X = 28,
    GAUGE_Y = 104,
    GAUGE_SIZE = 160,
    VALUE_X = 210,
    NUMBER_Y = 104,
    BAR_Y = 156,
    BAR_W = 256,
    BAR_H = 22,
    ZONE_Y = 192,
    LED_Y = 236,
    LIMITS_Y = 308,
    SIDE_X = 494,
    SIDE_W = 286,
    BANNER_Y = 64,
    BANNER_H = 56,
    BANNER_RADIUS = 8,
    BANNER_TEXT_X = 510,
    BANNER_TEXT_Y = 76,
    TANK_Y = 140,
    TANK_BAR_Y = 172,
    TANK_BAR_H = 18,
    BUTTON_Y = 236,
    BUTTON_H = 56,
    NOTE_Y = 302,
    HELP_Y = 352,
    FONT_SMALL = 16,
    FONT_TEXT = 20,
    FONT_TITLE = 24,
    FONT_NUMBER = 28,
    SETTLE_MS = 200, /* let the cleared screen settle */
    TEXT_BYTES = 48  /* "น้ำในถัง %d %%" with any int32_t */
};

/* Colours, 0xRRGGBB. */
enum {
    COL_TEXT = 0xE8EAED,
    COL_DIM = 0x9AA3AF,
    COL_CARD = 0x171B22,
    COL_ALARM_BG = 0x3A1416,
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

/* A Panel's look: fill colour, edge colour, corner radius and
 * border width (color=, min=, max=, value=). */
typedef struct {
    uint32_t fill;
    uint32_t edge;
    int32_t radius;
    int32_t border;
} panel_look_t;

/* The widgets the program changes after building the screen. */
typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t card;
    bento_ui_id_t gauge;
    bento_ui_id_t number;
    bento_ui_id_t bar;
    bento_ui_id_t zone;
    bento_ui_id_t led;
    bento_ui_id_t banner;
    bento_ui_id_t banner_text;
    bento_ui_id_t tank;
    bento_ui_id_t tank_bar;
    bento_ui_id_t button;
    bento_ui_id_t note;
    bento_ui_id_t help;
} style_screen_t;

typedef struct {
    style_screen_t screen;
    int32_t shown;
    int32_t zone;
    int32_t tank_shown;
    int32_t tank;
    int32_t runs;
    bool lock_known;
    bool locked;
    bool big;
    uint32_t show_ms;
} style_t;

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

/* A screen refresh whose events are not read here. */
static void present(bento_ui_t *ui)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    /* ui.poll() answers no events when it fails */
    (void)bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                        NULL);
}

static void set_text(bento_ui_t *ui, bento_ui_id_t widget,
                     const char *text)
{
    /* a setter's refusal was ignored before too (modui.c) */
    (void)bento_ui_set_text(ui, widget, text, (uint32_t)strlen(text));
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
        {76U, 0U}, {72U, 79U}, {84U, 76U}};
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

static bento_status_t read_percent(uint32_t pot, int32_t *percent)
{
    uint16_t raw = 0U;
    const bento_status_t status = bento_pots_read(pot, &raw);

    *percent = ((int32_t)raw * PERCENT) / ADC_FULL;
    return status;
}

/* 0 = normal, 1 = getting dry, 2 = dry. */
static int32_t zone_of(int32_t percent)
{
    int32_t zone = ZONE_OK;

    if (percent < DRY_BELOW) {
        zone = ZONE_DRY;
    } else if (percent < WARN_BELOW) {
        zone = 1;
    } else {
        /* moist enough */
    }
    return zone;
}

/* The safety rule lives in the program, not in the button's
 * colour: grey only tells the person. */
static bool pump_allowed(int32_t tank)
{
    return tank >= TANK_MIN;
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

static bento_status_t add_panel(bento_ui_t *ui, rect_t box,
                                const panel_look_t *look,
                                bento_ui_id_t *out)
{
    bento_ui_widget_args_t args;

    place(&args, box);
    args.color = look->fill;
    args.min_val = (int32_t)look->edge;
    args.max_val = look->radius;
    args.font_px = look->border;
    return bento_ui_panel_create(ui, &args, out);
}

/* A Bar from 0 to 100 that starts empty. */
static bento_status_t add_bar(bento_ui_t *ui, rect_t box,
                              bento_ui_id_t *out)
{
    bento_ui_widget_args_t args;

    place(&args, box);
    args.min_val = 0;
    args.max_val = PERCENT;
    return bento_ui_bar_create(ui, &args, out);
}

/* The Scale draws ticks and numbers but no needle; the program
 * sets the needle: its length in the top 16 bits, the value below. */
static void set_needle(const style_screen_t *screen, int32_t value)
{
    static const uint32_t needle_value_bits = 0xFFFFU;
    const uint32_t packed =
        ((uint32_t)NEEDLE_LEN << (uint32_t)NEEDLE_SHIFT) |
        ((uint32_t)value & needle_value_bits);

    (void)bento_ui_set_prop(screen->ui, screen->gauge,
                            (uint8_t)BENTO_UI_PROP_SCALE_NEEDLE,
                            (int32_t)packed);
}

static bento_status_t add_gauge(style_screen_t *screen)
{
    const rect_t box = {GAUGE_X, GAUGE_Y, GAUGE_SIZE, GAUGE_SIZE};
    bento_ui_widget_args_t args;
    bento_status_t status;

    place(&args, box);
    args.color = COL_TEXT;
    args.min_val = 0;
    args.max_val = PERCENT;
    status = bento_ui_scale_create(screen->ui, &args, &screen->gauge);
    if (status == BENTO_OK) {
        /* a gauge's setters ignore a refusal, as modui.c does */
        (void)bento_ui_set_prop(screen->ui, screen->gauge,
                                (uint8_t)BENTO_UI_PROP_SCALE_MODE,
                                BENTO_UI_SCALE_ROUND_IN);
        (void)bento_ui_set_ticks(screen->ui, screen->gauge, TICKS_TOTAL,
                                 TICKS_MAJOR);
    }
    return status;
}

static bento_status_t add_led(style_screen_t *screen)
{
    const rect_t box = {VALUE_X, LED_Y, LED_SMALL, LED_SMALL};
    bento_ui_widget_args_t args;

    place(&args, box);
    args.color = COL_BAD;
    args.font_px = 1;
    return bento_ui_led_create(screen->ui, &args, &screen->led);
}

/* The soil card: Panel border set at creation, the gauge, the number,
 * the bar, the zone and the alarm light. */
static bento_status_t add_soil_card(style_screen_t *screen)
{
    const rect_t box = {CARD_X, CARD_Y, CARD_W, CARD_H};
    const panel_look_t look = {COL_CARD, COL_INFO, CARD_RADIUS,
                               CARD_BORDER_W};
    const point_t title_at = {CARD_TITLE_X, CARD_TITLE_Y};
    const point_t number_at = {VALUE_X, NUMBER_Y};
    const rect_t bar_box = {VALUE_X, BAR_Y, BAR_W, BAR_H};
    const point_t zone_at = {VALUE_X, ZONE_Y};
    bento_ui_id_t unused;
    bento_status_t status =
        add_panel(screen->ui, box, &look, &screen->card);

    if (status == BENTO_OK) {
        status = add_label(screen->ui, "ความชื้นดิน", title_at, COL_INFO,
                           FONT_TEXT, &unused);
    }
    if (status == BENTO_OK) {
        status = add_gauge(screen);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "-- %", number_at, COL_TEXT,
                           FONT_NUMBER, &screen->number);
    }
    if (status == BENTO_OK) {
        status = add_bar(screen->ui, bar_box, &screen->bar);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", zone_at, COL_TEXT,
                           FONT_TEXT, &screen->zone);
    }
    if (status == BENTO_OK) {
        status = add_led(screen);
    }
    return status;
}

/* The banner is made now and hidden; show() brings it up when the
 * soil is dry. */
static bento_status_t add_banner(style_screen_t *screen)
{
    const rect_t box = {SIDE_X, BANNER_Y, SIDE_W, BANNER_H};
    const panel_look_t look = {COL_BAD, COL_BAD, BANNER_RADIUS, 1};
    const point_t text_at = {BANNER_TEXT_X, BANNER_TEXT_Y};
    bento_status_t status =
        add_panel(screen->ui, box, &look, &screen->banner);

    if (status == BENTO_OK) {
        status = add_label(screen->ui, "ดินแห้ง! รดน้ำ", text_at, COL_TEXT,
                           FONT_TITLE, &screen->banner_text);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_visible(screen->ui, screen->banner, 0U);
        (void)bento_ui_set_visible(screen->ui, screen->banner_text, 0U);
    }
    return status;
}

static bento_status_t add_pump_button(style_screen_t *screen)
{
    const rect_t box = {SIDE_X, BUTTON_Y, SIDE_W, BUTTON_H};
    const char *text = "เปิดปั๊ม";
    bento_ui_widget_args_t args;

    place(&args, box);
    args.text = text;
    args.text_len = (uint32_t)strlen(text);
    args.color = COL_INFO;
    args.font_px = FONT_TEXT; /* Thai: 20 px, not the default */
    return bento_ui_button_create(screen->ui, &args, &screen->button);
}

/* The tank side: banner, tank level, the pump button and its note. */
static bento_status_t add_tank_side(style_screen_t *screen)
{
    const point_t tank_at = {SIDE_X, TANK_Y};
    const rect_t tank_box = {SIDE_X, TANK_BAR_Y, SIDE_W, TANK_BAR_H};
    const point_t note_at = {SIDE_X, NOTE_Y};
    bento_status_t status = add_banner(screen);

    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", tank_at, COL_INFO,
                           FONT_TEXT, &screen->tank);
    }
    if (status == BENTO_OK) {
        status = add_bar(screen->ui, tank_box, &screen->tank_bar);
    }
    if (status == BENTO_OK) {
        status = add_pump_button(screen);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", note_at, COL_DIM, FONT_TEXT,
                           &screen->note);
    }
    return status;
}

static bento_status_t build_screen(style_screen_t *screen)
{
    const point_t title_at = {MARGIN_X, TITLE_Y};
    const point_t hint_at = {MARGIN_X, HINT_Y};
    const point_t limits_at = {CARD_TITLE_X, LIMITS_Y};
    const point_t help_at = {MARGIN_X, HELP_Y};
    bento_ui_id_t unused;
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = sleep_ms((uint32_t)SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "สไตล์เปลี่ยนตามค่า", title_at,
                           COL_TEXT, FONT_TITLE, &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(
            screen->ui,
            "color() size() show() hide() disable() ticks() prop()",
            hint_at, COL_DIM, FONT_SMALL, &unused);
    }
    if (status == BENTO_OK) {
        status = add_soil_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "ปรับไม่ได้: ฟอนต์ ความโปร่งใส ไล่สี",
                           limits_at, COL_DIM, FONT_TEXT, &unused);
    }
    if (status == BENTO_OK) {
        status = add_tank_side(screen);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "VR1 = ดิน  VR2 = ถัง  แตะปุ่มได้",
                           help_at, COL_DIM, FONT_TEXT, &screen->help);
    }
    if (status == BENTO_OK) {
        present(screen->ui);
    }
    return status;
}

/* The card follows the zone; sent only when the zone changes. */
static void restyle(const style_screen_t *screen, int32_t zone)
{
    static const char *const zone_names[ZONE_COUNT] = {"ปกติ", "เริ่มแห้ง",
                                                       "แห้ง!"};
    static const uint32_t zone_col[ZONE_COUNT] = {COL_OK, COL_WARN,
                                                  COL_BAD};
    const uint32_t color = zone_col[zone];
    const uint8_t visible = (zone == ZONE_DRY) ? 1U : 0U;
    bento_ui_t *ui = screen->ui;

    /* the widget setters ignore a refusal, as modui.c does */
    (void)bento_ui_set_color(ui, screen->number, color);
    (void)bento_ui_set_color(ui, screen->bar, color);
    (void)bento_ui_set_prop(ui, screen->gauge,
                            (uint8_t)BENTO_UI_PROP_SCALE_NEEDLE_COLOR,
                            (int32_t)color);
    (void)bento_ui_set_color(ui, screen->zone, color);
    set_text(ui, screen->zone, zone_names[zone]);
    /* a Panel's color() is its fill */
    (void)bento_ui_set_color(
        ui, screen->card, (zone == ZONE_DRY) ? COL_ALARM_BG : COL_CARD);
    (void)bento_ui_set_visible(ui, screen->banner, visible);
    (void)bento_ui_set_visible(ui, screen->banner_text, visible);
}

/* A tap on the pump button; the program checks the tank again,
 * because the Emulator lets a disabled button be tapped. */
static bento_status_t on_pump_tap(style_t *style)
{
    char text[TEXT_BYTES];
    bento_status_t status;

    if (pump_allowed(style->tank)) {
        style->runs++;
        status = beep(style->screen.ui, TUNE_TAP);
        if (status == BENTO_OK) {
            (void)snprintf(text, sizeof(text), "สั่ง %" PRId32 " ครั้ง",
                           style->runs);
            set_text(style->screen.ui, style->screen.note, text);
        }
    } else {
        status = beep(style->screen.ui, TUNE_BAD);
    }
    return status;
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

static bento_status_t handle_events(style_t *style,
                                    const bento_ui_event_t *events,
                                    uint32_t count)
{
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < count) && (status == BENTO_OK); i++) {
        if (event_is(&events[i], BENTO_UI_EVENT_CLICKED,
                     style->screen.button)) {
            status = on_pump_tap(style);
        }
    }
    return status;
}

/* Reads the screen's events until none is left. */
static bento_status_t drain_events(style_t *style)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = poll_events(style->screen.ui, events);
    bento_status_t status = BENTO_OK;

    while ((count != 0U) && (status == BENTO_OK)) {
        status = handle_events(style, events, count);
        count = poll_events(style->screen.ui, events);
    }
    return status;
}

static void show_soil(style_t *style, int32_t soil)
{
    char text[TEXT_BYTES];

    if (soil != style->shown) {
        (void)snprintf(text, sizeof(text), "%" PRId32 " %%", soil);
        set_text(style->screen.ui, style->screen.number, text);
        (void)bento_ui_set_value(style->screen.ui, style->screen.bar,
                                 soil);
        set_needle(&style->screen, soil);
        style->shown = soil;
    }
}

/* Worse sounds the alert, better sounds the ok tune. */
static bento_status_t show_zone(style_t *style, int32_t zone)
{
    bento_status_t status = BENTO_OK;

    if (zone != style->zone) {
        if ((style->zone != UNKNOWN) && (zone > style->zone)) {
            status = beep(style->screen.ui, TUNE_BAD);
        } else if (style->zone != UNKNOWN) {
            status = beep(style->screen.ui, TUNE_START);
        } else {
            /* the first zone is shown in silence */
        }
        restyle(&style->screen, zone);
        style->zone = zone;
    }
    return status;
}

static void show_tank(style_t *style)
{
    char text[TEXT_BYTES];

    if (style->tank != style->tank_shown) {
        (void)bento_ui_set_value(style->screen.ui,
                                 style->screen.tank_bar, style->tank);
        (void)snprintf(text, sizeof(text), "น้ำในถัง %" PRId32 " %%",
                       style->tank);
        set_text(style->screen.ui, style->screen.tank, text);
        style->tank_shown = style->tank;
    }
}

/* A low tank greys the button and silences it; enough water
 * enables it again. */
static void show_lock(style_t *style)
{
    const bool lock = !pump_allowed(style->tank);
    bento_ui_t *ui = style->screen.ui;

    if (!style->lock_known || (lock != style->locked)) {
        /* the Emulator has no disable()/enable(); the .py ignores
         * that, and so does the twin */
        (void)bento_ui_set_enabled(ui, style->screen.button,
                                   lock ? 0U : 1U);
        (void)bento_ui_set_color(ui, style->screen.note,
                                 lock ? COL_BAD : COL_DIM);
        set_text(ui, style->screen.note,
                 lock ? "ถังต่ำ: ห้ามกด" : "ถังพอ: กดได้");
        style->locked = lock;
        style->lock_known = true;
    }
}

/* The alarm light pulses while the soil is dry: big and small in
 * turn, resized only when the size changes. */
static void pulse_led(style_t *style, int32_t zone)
{
    const bool beat = (zone == ZONE_DRY) ? !style->big : false;

    if (beat != style->big) {
        const int16_t size = px(beat ? LED_BIG : LED_SMALL);

        (void)bento_ui_set_size(style->screen.ui, style->screen.led,
                                size, size);
        style->big = beat;
    }
}

/* Every TICK_MS: read both knobs and restyle what changed. */
static bento_status_t tick(style_t *style, uint32_t now)
{
    int32_t soil = 0;
    bento_status_t status = BENTO_OK;

    if (bento_time_ticks_diff(now, style->show_ms) >= TICK_MS) {
        style->show_ms = now;
        status = read_percent(VR1, &soil);
        if (status == BENTO_OK) {
            status = read_percent(VR2, &style->tank);
        }
        if (status == BENTO_OK) {
            show_soil(style, soil);
            status = show_zone(style, zone_of(soil));
        }
        if (status == BENTO_OK) {
            show_tank(style);
            show_lock(style);
            pulse_led(style, style->zone);
        }
    }
    return status;
}

static bento_status_t run_loop(style_t *style)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    style->show_ms = start_ms;
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = drain_events(style);
        if (status == BENTO_OK) {
            status = tick(style, bento_time_ticks_ms());
        }
        if (status == BENTO_OK) {
            status = sleep_ms((uint32_t)LOOP_MS);
        }
    }
    return status;
}

int main(void)
{
    style_t style;
    bento_status_t status;

    (void)memset(&style, 0, sizeof(style));
    style.screen.ui = bento_runtime_get_ui();
    style.shown = UNKNOWN;
    style.zone = UNKNOWN;
    style.tank_shown = UNKNOWN;
    /* a board without ui.volume answers BENTO_E_UNSUPPORTED and is
     * skipped as the .py skips it; elsewhere it waits for no answer */
    (void)bento_ui_sfx_volume_set(style.screen.ui, SPEAKER);
    status = build_screen(&style.screen);
    if (status == BENTO_OK) {
        status = run_loop(&style);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_color(style.screen.ui, style.screen.help,
                                 COL_WARN);
        set_text(style.screen.ui, style.screen.help,
                 "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่");
        present(style.screen.ui);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
