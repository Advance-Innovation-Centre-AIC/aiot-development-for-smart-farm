/*
 * sf2_01_farm_goes_online.c -- พาโรงเรือนขึ้นอินเทอร์เน็ตครั้งแรก
 * (The greenhouse goes online for the first time)
 *
 * ต่อ WiFi โชว์เวลาที่ใช้ต่อ (Seg7) กับ IP แล้วเฝ้าดูลิงก์หนึ่งนาที:
 * ออนไลน์กี่ % ของเวลา (Arc) ไฟ (Led) หลุดกี่ครั้ง กราฟลิงก์และสัญญาณ
 * (Chart) และอากาศในโรงเรือน ระหว่างเฝ้าดู ลองปิด Hotspot สัก 10 วินาที
 * แล้วเปิดใหม่ แก้ WIFI_SSID กับ WIFI_PASS ก่อนรัน กับดัก: การต่อ WiFi
 * บล็อกได้ราว 85 วินาที จอจะนิ่ง อย่ากดรีเซ็ต
 *
 * Joins Wi-Fi, shows how long the join took (Seg7) and the address,
 * then watches the link for one minute: the share of time online
 * (Arc), a lamp (Led), the number of drops, a chart of the link and
 * the signal, and the greenhouse air. While it watches, switch the
 * hotspot off for about 10 s and on again. Set WIFI_SSID and
 * WIFI_PASS first. The trap: the join can block for about 85 s; the
 * screen freezes, do not press reset.
 *
 * Twin of sf2_01_farm_goes_online.py
 * (aiot-development-for-smart-farm/s2/sf2_01_farm_goes_online.py).
 * Style: CLEAN_C.md. Differs from the original:
 *   - wifi.connect() prints lines of its own; the C call prints
 *     nothing. An SSID or password the radio cannot take counts as a
 *     failed join, as it does there. Stop ends the wait for the join.
 *   - wifi.is_connected() is false when the radio cannot be asked;
 *     a failed sensor read gives the same None as the original's
 *     except.
 *   - The lesson's Stop exception travels as BENTO_E_CANCELLED, which
 *     main() turns into 0 as it does the IDE's Stop button.
 */
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_fmt.h"
#include "bento_gpio.h"
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_sensors.h"
#include "bento_time.h"
#include "bento_wifi.h"

/* ---- 1) ตั้งค่า (แก้ได้) ---- */
/* ใช้ Hotspot มือถือ (WiFi ที่ต้อง login ผ่านเว็บ บอร์ดใช้ไม่ได้) */
static const char WIFI_SSID[] =
    "<ชื่อ Hotspot ของกลุ่ม>"; /* อังกฤษ/ตัวเลขสั้น ๆ */
/* WIFI_PASS: ตั้งใน join_wifi() (set it in join_wifi()) */
/* FARM_NAME: ชื่อบนจอ ตั้งใน build_screen() */
/* TEMP_OFFSET (ชดเชยความอุ่นจากชิป เช่น -7.0) และ HUM_FIX: ตั้งใน
 * read_climate() */

enum {
    RUN_MS = 60000, /* เฝ้าดูลิงก์กี่ ms */
    TICK_MS = 1000, /* ถามลิงก์ทุกกี่ ms */
    IP_TRIES = 15,
    IP_WAIT_MS = 200
};

enum {
    SPEAKER = 40, /* the speaker, 0-100 % */
    VOLUME = 25   /* each tone, 0-127 */
};

/* The course palette, 0xRRGGBB. */
enum {
    COLOR_TEXT = 0xE8EAED,
    COLOR_DIM = 0x9AA3AF,
    COLOR_CARD = 0x171B22,
    COLOR_OK = 0x30A46C,
    COLOR_WARN = 0xF5A623,
    COLOR_BAD = 0xE5484D,
    COLOR_INFO = 0x4A9EFF
};

/* Cards on the screen: a panel and its title. */
enum {
    CARD_RADIUS = 12,
    CARD_BORDER_PX = 1,
    CARD_INSET_X = 12,
    CARD_INSET_Y = 6,
    CARD_TITLE_PX = 16
};

/* Times, scales and the link. */
enum {
    SETTLE_MS = 200, /* let the cleared screen settle */
    TONE_MS = 90,
    NOTE_MS = 100,
    MS_PER_S = 1000,
    PERCENT = 100,
    PCT_GOOD = 90,
    PCT_FAIR = 60,
    RSSI_FLOOR = 100, /* -100 dBm is 0 % */
    CHART_POINTS = 400,
    LINK_UP = 90, /* the link's line on the chart */
    LINK_DOWN = 10,
    SCROLL_MS = 80,
    WIFI_SLICE_MS = 50
};

/* Text buffers, each sized for the longest text built in it. */
enum {
    TEXT_BYTES = 128
};

/* ---- 2) ฮาร์ดแวร์ ---- */

typedef enum {
    TUNE_TAP,
    TUNE_START,
    TUNE_STOP,
    TUNE_GOOD,
    TUNE_BAD,
    TUNE_EMPTY,
    TUNE_HIT,
    TUNE_COUNT
} tune_t;

enum {
    TUNE_NOTES_MAX = 3
};

static bento_status_t check_running(bento_status_t status)
{
    bento_status_t result = status;

    if ((result == BENTO_OK) && !bento_runtime_is_running()) {
        result = BENTO_E_CANCELLED;
    }
    return result;
}

static bento_status_t pause_ms(uint32_t ms)
{
    return check_running(bento_time_sleep_ms(ms));
}

/* A 0 ends a tune early. */
static bento_status_t beep(bento_ui_t *ui, tune_t tune)
{
    static const uint8_t tunes[TUNE_COUNT][TUNE_NOTES_MAX] = {
        {76U, 0U, 0U},   {72U, 79U, 0U}, {79U, 72U, 0U},
        {72U, 79U, 84U}, {84U, 76U, 0U}, {84U, 76U, 69U},
        {88U, 0U, 0U}};
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < (uint32_t)TUNE_NOTES_MAX) &&
                 (status == BENTO_OK) && (tunes[tune][i] != 0U);
         i++) {
        /* fire-and-forget: ui.tone() never reports a refusal */
        (void)bento_ui_sfx_tone(ui, tunes[tune][i],
                                (uint8_t)BENTO_UI_WAVE_SINE,
                                (uint8_t)VOLUME, (uint16_t)TONE_MS);
        status = pause_ms(NOTE_MS);
    }
    return status;
}

/* A reading that may be missing: Python's None. */
typedef struct {
    bool known;
    bento_real_t value;
} maybe_t;

/* math.exp at the board's float width. */
static bento_real_t real_exp(bento_real_t x)
{
#if BENTO_FMT_REAL_DOUBLE
    return exp(x);
#else
    return expf(x);
#endif
}

/* ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ไม่แตะเน็ต ---- */

/* The Magnus formula (hPa). */
static bento_real_t sat_pressure(bento_real_t t)
{
    const bento_real_t base_hpa = (bento_real_t)6.112;
    const bento_real_t slope = (bento_real_t)17.62;
    const bento_real_t offset_c = (bento_real_t)243.12;

    return base_hpa * real_exp((slope * t) / (offset_c + t));
}

/* The chip's humidity seen at the room's temperature. */
static bento_real_t room_humidity(bento_real_t h_raw,
                                  bento_real_t t_raw,
                                  bento_real_t t_room)
{
    const bento_real_t full = (bento_real_t)PERCENT;
    const bento_real_t h =
        (h_raw * sat_pressure(t_raw)) / sat_pressure(t_room);

    return (h < full) ? h : full;
}

/* The signal as 0-100 %; an rssi of 0 was not measured: unknown. */
static maybe_t signal_level(int32_t rssi)
{
    maybe_t level = {false, (bento_real_t)0.0};
    int32_t percent = rssi + RSSI_FLOOR;

    if (rssi < 0) {
        percent = (percent < 0) ? 0 : percent;
        level.known = true;
        level.value =
            (bento_real_t)((percent > PERCENT) ? PERCENT : percent);
    }
    return level;
}

static uint32_t pct_color(int32_t pct)
{
    uint32_t color = COLOR_BAD;

    if (pct >= PCT_GOOD) {
        color = COLOR_OK;
    } else if (pct >= PCT_FAIR) {
        color = COLOR_WARN;
    } else {
        /* below fair */
    }
    return color;
}

/* The temperature with its offset, then the humidity; a value that
 * cannot be read is unknown, and so is the humidity after a failed
 * temperature. The humidity follows the offset only when HUM_FIX. */
static void read_climate(maybe_t *t, maybe_t *h)
{
    static const bool hum_fix = true; /* แปลงความชื้นเป็นของห้อง */
    static const bento_real_t temp_offset = (bento_real_t)0.0;
    float t_raw = 0.0F;
    float humidity = 0.0F;

    t->known = bento_sensors_sht40_temperature(&t_raw) == BENTO_OK;
    h->known = t->known &&
               (bento_sensors_sht40_humidity(&humidity) == BENTO_OK);
    t->value = (bento_real_t)t_raw + temp_offset;
    h->value = (bento_real_t)humidity;
    if (h->known && t->known && (temp_offset != (bento_real_t)0.0) &&
        hum_fix) {
        h->value =
            room_humidity(h->value, (bento_real_t)t_raw, t->value);
    }
}

typedef struct {
    bool found;
    uint32_t index;
} board_led_t;

/* The LED is found by its name, not its number; a board without that
 * name lends LED 0, and one whose LED cannot be switched has none. */
static board_led_t led_named(const char *name)
{
    const uint32_t count = bento_gpio_num_leds();
    board_led_t led = {false, 0U};
    bool named = false;
    uint32_t i;

    for (i = 0U; (i < count) && !named; i++) {
        const char *led_name = NULL;

        named = (bento_gpio_led_name(i, &led_name) == BENTO_OK) &&
                (led_name != NULL) && (strcmp(led_name, name) == 0);
        if (named) {
            led.index = i;
        }
    }
    led.found =
        (count != 0U) && (bento_gpio_led_off(led.index) == BENTO_OK);
    return led;
}

static bento_status_t set_led(const board_led_t *led, bool on)
{
    bento_status_t status = BENTO_OK;

    if (led->found) {
        status = on ? bento_gpio_led_on(led->index)
                    : bento_gpio_led_off(led->index);
    }
    return status;
}

/* One call starts the marquee; the firmware runs it ("" stops it). */
static bento_status_t marquee(const char *text, int32_t color)
{
    return bento_rgbmatrix_scroll(text, (uint32_t)strlen(text), color,
                                  SCROLL_MS);
}

/* ---- widgets ---- */

/* Where a Label goes and how it looks. */
typedef struct {
    int16_t x;
    int16_t y;
    uint32_t color;
    int32_t font_px; /* 0: the default size */
} spot_t;

/* Any other widget: its box, colour and min=, max=, value=. */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    uint32_t color;
    int32_t low;
    int32_t high;
    int32_t value;
} box_t;

static bento_status_t add_label(bento_ui_t *ui, const spot_t *spot,
                                const char *text, bento_ui_id_t *id)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.x = spot->x;
    args.y = spot->y;
    args.color = spot->color;
    args.font_px = spot->font_px;
    args.text = text;
    args.text_len = (uint32_t)strlen(text);
    return bento_ui_label_create(ui, &args, id);
}

/* type is a UI_WIDGET_*; text may be NULL. */
static bento_status_t add_box(bento_ui_t *ui, int32_t type,
                              const box_t *box, const char *text,
                              bento_ui_id_t *id)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.x = box->x;
    args.y = box->y;
    args.w = box->w;
    args.h = box->h;
    args.color = box->color;
    args.min_val = box->low;
    args.max_val = box->high;
    args.font_px = box->value;
    if (text != NULL) {
        args.text = text;
        args.text_len = (uint32_t)strlen(text);
    }
    return bento_ui_create(ui, (uint8_t)type, &args, id);
}

/* A rounded panel with its border in COLOR_DIM, and its title. */
static bento_status_t add_card(bento_ui_t *ui, const box_t *place,
                               const char *title,
                               bento_ui_id_t *title_id)
{
    const box_t panel = {place->x,    place->y,      place->w,
                         place->h,    COLOR_CARD,    COLOR_DIM,
                         CARD_RADIUS, CARD_BORDER_PX};
    const spot_t heading = {(int16_t)(place->x + CARD_INSET_X),
                            (int16_t)(place->y + CARD_INSET_Y),
                            COLOR_INFO, CARD_TITLE_PX};
    bento_ui_id_t panel_id;
    bento_status_t status =
        add_box(ui, UI_WIDGET_PANEL, &panel, NULL, &panel_id);

    if (status == BENTO_OK) {
        status = add_label(ui, &heading, title, title_id);
    }
    return status;
}

/* MicroPython's widget setters ignore a refusal (modui.c), so these
 * do too. */
static void show_text(bento_ui_t *ui, bento_ui_id_t id,
                      const char *text)
{
    (void)bento_ui_set_text(ui, id, text, (uint32_t)strlen(text));
}

static void show_colored(bento_ui_t *ui, bento_ui_id_t id,
                         uint32_t color, const char *text)
{
    (void)bento_ui_set_color(ui, id, color);
    show_text(ui, id, text);
}

static void show_value(bento_ui_t *ui, bento_ui_id_t id, int32_t value)
{
    (void)bento_ui_set_value(ui, id, value);
}

/* ui.poll() here only lets the screen catch up; no widget listens. */
static void refresh(bento_ui_t *ui)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    (void)bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                        NULL);
}

/* ---- 4) เครือข่าย ---- */

/* The scan's answers: too big for the 8 KB program stack, so at file
 * scope, zeroed by the loader and set again in main() (CLEAN_C.md C9,
 * deviation BENTO-MISRA-E03/E04). */
extern bento_wifi_ap_t g_networks[BENTO_WIFI_SCAN_MAX];
bento_wifi_ap_t g_networks[BENTO_WIFI_SCAN_MAX];

/* wifi.connect(): *joined is its True or False; any other failure
 * ends the run, as an OSError there does. */
static bento_status_t join_wifi(bool *joined)
{
    static const char wifi_pass[] =
        "<รหัส Hotspot ของกลุ่ม>"; /* อย่างน้อย 8 ตัว */
    bento_wifi_connect_params_t params;
    bool waiting;
    bento_status_t status;

    bento_wifi_connect_params_init(&params);
    params.ssid = WIFI_SSID;
    params.ssid_len = (uint32_t)strlen(WIFI_SSID);
    params.pass = wifi_pass;
    params.pass_len = (uint32_t)strlen(wifi_pass);
    params.flags = BENTO_WIFI_F_SAVE_CREDS; /* as the binding does */
    status = bento_wifi_connect_start(&params);
    waiting = status == BENTO_OK;
    while (waiting) {
        status = bento_wifi_connect_poll();
        waiting = status == BENTO_E_IN_PROGRESS;
        if (waiting) {
            status = pause_ms(WIFI_SLICE_MS);
            waiting = status == BENTO_OK;
        }
    }
    if (status == BENTO_E_CANCELLED) {
        (void)bento_wifi_connect_cancel(); /* answers BENTO_OK always */
    }
    *joined = status == BENTO_OK;
    if ((status == BENTO_E_TIMEOUT) || (status == BENTO_E_FAILED) ||
        (status == BENTO_E_INVALID_ARGUMENT)) {
        status = BENTO_OK;
    }
    return status;
}

/* "Not heard at all" and "heard but the password is wrong" are fixed
 * in different ways; a scan tells them apart. A failed scan only
 * means not heard; Stop is the one error. */
static bento_status_t heard_on_air(bool *heard)
{
    uint32_t count = 0U;
    uint32_t i;
    bento_status_t status = bento_wifi_scan_start();
    bool waiting = status == BENTO_OK;

    *heard = false;
    while (waiting) {
        status = bento_wifi_scan_poll(g_networks, BENTO_WIFI_SCAN_MAX,
                                      &count);
        waiting = status == BENTO_E_IN_PROGRESS;
        if (waiting) {
            status = pause_ms(WIFI_SLICE_MS);
            waiting = status == BENTO_OK;
        }
    }
    for (i = 0U; (status == BENTO_OK) && (i < count); i++) {
        *heard = *heard || (strcmp(g_networks[i].ssid, WIFI_SSID) == 0);
    }
    if (status == BENTO_E_CANCELLED) {
        (void)bento_wifi_scan_cancel(); /* answers BENTO_OK always */
    }
    return (status == BENTO_E_CANCELLED) ? status : BENTO_OK;
}

/* wifi.ip(): "0.0.0.0" when there is no address. */
static void read_ip(char *text, uint32_t capacity)
{
    uint32_t address = 0U;
    uint32_t length = 0U;

    if ((bento_wifi_ip(BENTO_WIFI_IF_ANY, &address) != BENTO_OK) ||
        (bento_wifi_ipv4_format(address, text, capacity, &length) !=
         BENTO_OK)) {
        (void)snprintf(text, capacity, "0.0.0.0");
    }
}

static bool link_is_up(void)
{
    bool connected = false;

    return (bento_wifi_is_connected(&connected) == BENTO_OK) &&
           connected;
}

/* firmware 2.4.1 answers rssi = 0, so the blue line shows only in the
 * Emulator. */
static maybe_t read_signal(void)
{
    bento_wifi_status_t link;
    maybe_t level = {false, (bento_real_t)0.0};

    bento_wifi_status_init(&link);
    if (bento_wifi_status(&link) == BENTO_OK) {
        level = signal_level(link.rssi_dbm);
    }
    return level;
}

/* ---- 5) หน้าจอ ---- */

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t seg;
    bento_ui_id_t ip;
    bento_ui_id_t state;
    bento_ui_id_t arc;
    bento_ui_id_t pct;
    bento_ui_id_t led;
    bento_ui_id_t drops;
    bento_ui_id_t chart;
    uint8_t signal_series;
    bento_ui_id_t t;
    bento_ui_id_t h;
    bento_ui_id_t note;
} online_screen_t;

static bento_status_t build_wifi_card(online_screen_t *screen)
{
    static const box_t card = {12, 44, 380, 150, 0U, 0, 0, 0};
    static const box_t seg = {24, 76, 150, 56, COLOR_WARN, 0, 0, 0};
    static const spot_t ip = {190, 92, COLOR_TEXT, 20};
    static const spot_t state = {24, 148, COLOR_WARN, 20};
    bento_ui_id_t unused;
    bento_status_t status =
        add_card(screen->ui, &card, "ต่อ WiFi (ms) และ IP", &unused);

    if (status == BENTO_OK) {
        status = add_box(screen->ui, UI_WIDGET_SEG7, &seg, "----",
                         &screen->seg);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &ip, "-", &screen->ip);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &state, "กำลังจะเริ่มต่อ",
                           &screen->state);
    }
    return status;
}

static bento_status_t build_online_card(online_screen_t *screen)
{
    static const box_t card = {402, 44, 378, 150, 0U, 0, 0, 0};
    static const box_t arc = {414, 72, 116, 116, 0U, 0, 0, 0};
    static const spot_t pct = {546, 78, COLOR_TEXT, 28};
    static const box_t led = {546, 124, 24, 24, COLOR_OK, 0, 0, 0};
    static const spot_t drops = {580, 126, COLOR_DIM, 16};
    bento_ui_id_t unused;
    bento_status_t status =
        add_card(screen->ui, &card, "ออนไลน์กี่ % ของเวลา", &unused);

    if (status == BENTO_OK) {
        status = add_box(screen->ui, UI_WIDGET_ARC, &arc, NULL,
                         &screen->arc);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &pct, "--", &screen->pct);
    }
    if (status == BENTO_OK) {
        status = add_box(screen->ui, UI_WIDGET_LED, &led, NULL,
                         &screen->led);
    }
    if (status == BENTO_OK) {
        status =
            add_label(screen->ui, &drops, "หลุด 0 ครั้ง", &screen->drops);
    }
    return status;
}

/* LVGL draws no round dots once the points reach the width. */
static bento_status_t build_chart_card(online_screen_t *screen)
{
    static const box_t card = {12, 202, 400, 136, 0U, 0, 0, 0};
    static const box_t chart = {24, 230, 376, 100, COLOR_OK, 0, 100, 0};
    bento_ui_id_t unused;
    bento_status_t status = add_card(
        screen->ui, &card, "เขียว=ลิงก์ ฟ้า=สัญญาณ (ถ้าวัดได้)", &unused);

    if (status == BENTO_OK) {
        status = add_box(screen->ui, UI_WIDGET_CHART, &chart, NULL,
                         &screen->chart);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_prop(screen->ui, screen->chart,
                                (uint8_t)BENTO_UI_PROP_CHART_POINTS,
                                CHART_POINTS); /* a setter */
        status = bento_ui_chart_add_series(screen->ui, screen->chart,
                                           COLOR_INFO,
                                           &screen->signal_series);
    }
    return status;
}

static bento_status_t build_air_card(online_screen_t *screen)
{
    static const box_t card = {422, 202, 358, 136, 0U, 0, 0, 0};
    static const spot_t t = {434, 236, COLOR_TEXT, 24};
    static const spot_t h = {434, 282, COLOR_TEXT, 24};
    static const spot_t note = {12, 352, COLOR_DIM, 16};
    bento_ui_id_t unused;
    bento_status_t status =
        add_card(screen->ui, &card, "อากาศในโรงเรือน", &unused);

    if (status == BENTO_OK) {
        status = add_label(screen->ui, &t, "อุณหภูมิ -- C", &screen->t);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &h, "ความชื้น -- %", &screen->h);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &note, "กำลังเริ่ม", &screen->note);
    }
    return status;
}

static bento_status_t build_screen(online_screen_t *screen)
{
    static const char farm_name[] = "โรงเรือนกลุ่ม XX"; /* ชื่อบนจอ */
    static const spot_t heading = {12, 6, COLOR_TEXT, 24};
    static const spot_t farm = {320, 12, COLOR_INFO, 16};
    bento_ui_id_t unused;
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = pause_ms(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status =
            add_label(screen->ui, &heading, "ฟาร์มต่ออินเทอร์เน็ต", &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &farm, farm_name, &unused);
    }
    if (status == BENTO_OK) {
        status = build_wifi_card(screen);
    }
    if (status == BENTO_OK) {
        status = build_online_card(screen);
    }
    if (status == BENTO_OK) {
        status = build_chart_card(screen);
    }
    if (status == BENTO_OK) {
        status = build_air_card(screen);
    }
    if (status == BENTO_OK) {
        refresh(screen->ui);
    }
    return status;
}

/* No ui.poll() here: the caller chooses when. */
static void show_note(const online_screen_t *screen, const char *text,
                      uint32_t color)
{
    show_colored(screen->ui, screen->note, color, text);
}

static void show_state(const online_screen_t *screen, const char *text,
                       uint32_t color)
{
    show_colored(screen->ui, screen->state, color, text);
}

static void show_link(const online_screen_t *screen, bool online,
                      int32_t pct, int32_t drops)
{
    char text[TEXT_BYTES];

    show_value(screen->ui, screen->arc, pct);
    (void)bento_ui_set_color(screen->ui, screen->arc, pct_color(pct));
    (void)snprintf(text, sizeof(text), "%" PRId32 " %%", pct);
    show_text(screen->ui, screen->pct, text);
    show_value(screen->ui, screen->led, online ? 1 : 0); /* 0 = dim */
    (void)snprintf(text, sizeof(text), "หลุด %" PRId32 " ครั้ง", drops);
    show_text(screen->ui, screen->drops, text);
    show_state(screen, online ? "ออนไลน์อยู่" : "หลุด! ฟาร์มขาดการติดต่อ",
               online ? COLOR_OK : COLOR_BAD);
}

/* '%.1f' % value; BENTO_FMT_BUF_SIZE holds any number. */
static void show_air(const online_screen_t *screen, maybe_t t,
                     maybe_t h)
{
    char number[BENTO_FMT_BUF_SIZE];
    char text[TEXT_BYTES];
    uint32_t length = 0U;

    if (t.known) {
        (void)bento_fmt_real_precision(number, sizeof(number), t.value,
                                       1, &length);
        (void)snprintf(text, sizeof(text), "อุณหภูมิ %s C", number);
        show_text(screen->ui, screen->t, text);
    }
    if (h.known) {
        (void)bento_fmt_real_precision(number, sizeof(number), h.value,
                                       1, &length);
        (void)snprintf(text, sizeof(text), "ความชื้น %s %%", number);
        show_text(screen->ui, screen->h, text);
    }
}

/* Several points a second, so one minute fills the chart exactly.
 * set_next() is fire-and-forget, as in modui.c. */
static void plot(const online_screen_t *screen, bool online,
                 maybe_t signal)
{
    const int32_t per_tick =
        ((int32_t)CHART_POINTS * (int32_t)TICK_MS) / (int32_t)RUN_MS;
    const int32_t points = (per_tick > 1) ? per_tick : 1;
    int32_t i;

    for (i = 0; i < points; i++) {
        (void)bento_ui_chart_set_next(screen->ui, screen->chart, 0U,
                                      online ? LINK_UP : LINK_DOWN);
        if (signal.known) {
            (void)bento_ui_chart_set_next(screen->ui, screen->chart,
                                          screen->signal_series,
                                          (int32_t)signal.value);
        }
    }
}

/* ---- 6) โปรแกรมหลัก ---- */

typedef struct {
    online_screen_t screen;
    board_led_t led;
    int32_t took_ms;
    int32_t up;
    int32_t total;
    int32_t drops;
    bool was_online;
} farm_t;

/* The marquee must be stopped too, or it keeps running. */
static bento_status_t stop(farm_t *farm, const char *text,
                           uint32_t color)
{
    bento_status_t status = set_led(&farm->led, false);

    if (status == BENTO_OK) {
        status = marquee("", BENTO_RGBMATRIX_WHITE);
    }
    if (status == BENTO_OK) {
        status = bento_rgbmatrix_clear();
    }
    if (status == BENTO_OK) {
        show_note(&farm->screen, text, color);
        refresh(farm->screen.ui);
    }
    return status;
}

/* The note goes up before the call that blocks; *joined and the
 * milliseconds it took come back. */
static bento_status_t connect_wifi(farm_t *farm, bool *joined)
{
    char text[TEXT_BYTES];
    uint32_t start_ms;
    bento_status_t status;

    show_note(&farm->screen, "ครั้งแรกอาจรอนาน จอนิ่งได้ อย่ากดรีเซ็ต",
              COLOR_WARN);
    refresh(farm->screen.ui); /* every label up before the wait */
    status = marquee("WIFI", BENTO_RGBMATRIX_YELLOW);
    if (status == BENTO_OK) {
        (void)printf("กำลังต่อ %s - บรรทัดถัดไปจะบล็อก\n", WIFI_SSID);
        start_ms = bento_time_ticks_ms();
        status = join_wifi(joined);
        farm->took_ms =
            bento_time_ticks_diff(bento_time_ticks_ms(), start_ms);
    }
    if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text), "%" PRId32, farm->took_ms);
        show_text(farm->screen.ui, farm->screen.seg, text); /* a text */
        (void)bento_ui_set_color(farm->screen.ui, farm->screen.seg,
                                 *joined ? COLOR_OK : COLOR_BAD);
    }
    return status;
}

/* The join failed: say what to fix, and leave the RGB matrix red. */
static bento_status_t failed(farm_t *farm)
{
    char text[TEXT_BYTES];
    bool heard = false;
    bento_status_t status = heard_on_air(&heard);

    if (status == BENTO_OK) {
        show_state(&farm->screen, "ต่อไม่สำเร็จ", COLOR_BAD);
        (void)snprintf(text, sizeof(text), "ไม่ได้ยินวง %s ตรวจชื่อวง",
                       WIFI_SSID);
        status = stop(farm, heard ? "ได้ยินวงแต่ต่อไม่ผ่าน ตรวจรหัสผ่าน" : text,
                      COLOR_BAD);
    }
    if (status == BENTO_OK) {
        status = bento_rgbmatrix_fill(BENTO_RGBMATRIX_RED);
    }
    if (status == BENTO_OK) {
        status = beep(farm->screen.ui, TUNE_BAD);
    }
    if (status == BENTO_OK) {
        (void)printf("ต่อไม่สำเร็จใน %" PRId32 " ms | ได้ยินวงนี้: %s\n",
                     farm->took_ms, heard ? "True" : "False");
        status = BENTO_E_CANCELLED;
    }
    return status;
}

/* "0.0.0.0" is a text that is not empty, so the address must be
 * compared; DHCP gets IP_TRIES more chances. */
static bento_status_t wait_for_ip(const farm_t *farm, char *ip,
                                  uint32_t capacity)
{
    int32_t attempt;
    bento_status_t status = BENTO_OK;

    read_ip(ip, capacity);
    for (attempt = 0;
         (attempt < IP_TRIES) && (strcmp(ip, "0.0.0.0") == 0) &&
         (status == BENTO_OK);
         attempt++) {
        refresh(farm->screen.ui);
        status = pause_ms(IP_WAIT_MS);
        if (status == BENTO_OK) {
            read_ip(ip, capacity);
        }
    }
    return status;
}

static bento_status_t show_online(farm_t *farm)
{
    char ip[BENTO_WIFI_IPV4_TEXT_MAX];
    bento_status_t status = wait_for_ip(farm, ip, sizeof(ip));
    bool got_ip;

    if (status == BENTO_OK) {
        got_ip = strcmp(ip, "0.0.0.0") != 0;
        show_state(&farm->screen,
                   got_ip ? "ออนไลน์แล้ว" : "ลิงก์ขึ้นแต่ยังไม่มี IP",
                   got_ip ? COLOR_OK : COLOR_WARN);
        show_text(farm->screen.ui, farm->screen.ip, ip);
        show_note(&farm->screen,
                  "จด IP กับเวลาลงใบงาน แล้วลองปิด Hotspot ดู", COLOR_DIM);
        refresh(farm->screen.ui);
        status = marquee("ONLINE", BENTO_RGBMATRIX_GREEN);
    }
    if (status == BENTO_OK) {
        status = beep(farm->screen.ui, TUNE_START);
    }
    if (status == BENTO_OK) {
        (void)printf("ต่อสำเร็จใน %" PRId32 " ms | ip = %s\n",
                     farm->took_ms, ip);
    }
    return status;
}

/* The RGB matrix and the tune follow only a change of state, not
 * every second. */
static bento_status_t link_changed(bool online, int32_t second,
                                   bento_ui_t *ui)
{
    bento_status_t status;

    if (!online) {
        (void)printf("หลุดตอนวินาทีที่ %" PRId32 "\n", second);
    }
    status = online ? marquee("ONLINE", BENTO_RGBMATRIX_GREEN)
                    : marquee("OFFLINE", BENTO_RGBMATRIX_RED);
    if (status == BENTO_OK) {
        status = beep(ui, online ? TUNE_GOOD : TUNE_BAD);
    }
    return status;
}

/* connect() said the link was up then; is_connected() says whether
 * it is up now. */
static bento_status_t watch_once(farm_t *farm, uint32_t start_ms)
{
    const bool online = link_is_up();
    maybe_t t;
    maybe_t h;
    bento_status_t status = BENTO_OK;

    farm->total++;
    farm->up += online ? 1 : 0;
    if (online != farm->was_online) {
        farm->drops += online ? 0 : 1;
        status = link_changed(
            online,
            bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) /
                MS_PER_S,
            farm->screen.ui);
    }
    farm->was_online = online;
    if (status == BENTO_OK) {
        status = set_led(&farm->led, online);
    }
    if (status == BENTO_OK) {
        show_link(&farm->screen, online,
                  (farm->up * PERCENT) / farm->total, farm->drops);
        read_climate(&t, &h);
        show_air(&farm->screen, t, h);
        plot(&farm->screen, online, read_signal());
        refresh(farm->screen.ui);
        status = pause_ms(TICK_MS);
    }
    return status;
}

static bento_status_t watch_link(farm_t *farm)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    farm->was_online = true;
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = watch_once(farm, start_ms);
    }
    return status;
}

static bento_status_t go_online(farm_t *farm)
{
    bool joined = false;
    bento_status_t status;

    /* no volume control on this firmware: skipped, as hasattr() does */
    (void)bento_ui_sfx_volume_set(farm->screen.ui, SPEAKER);
    status = build_screen(&farm->screen);
    if (status == BENTO_OK) {
        farm->led = led_named("RGB_GREEN");
        status = connect_wifi(farm, &joined);
    }
    if ((status == BENTO_OK) && !joined) {
        status = failed(farm);
    }
    if (status == BENTO_OK) {
        status = show_online(farm);
    }
    return status;
}

int main(void)
{
    farm_t farm;
    const int32_t one = 1;
    bento_status_t status;

    (void)memset(&farm, 0, sizeof(farm));
    (void)memset(g_networks, 0, sizeof(g_networks));
    farm.screen.ui = bento_runtime_get_ui();
    status = go_online(&farm);
    if (status == BENTO_OK) {
        status = watch_link(&farm);
    }
    if (status == BENTO_OK) {
        status =
            stop(&farm, "จบรอบ - กด Program to Device อีกครั้งเพื่อเล่นใหม่",
                 COLOR_WARN);
    }
    if (status == BENTO_OK) {
        (void)printf("ออนไลน์ %" PRId32 " %% จาก %" PRId32
                     " วินาที | หลุด %" PRId32 " ครั้ง\n",
                     (farm.up * PERCENT) /
                         ((farm.total > one) ? farm.total : one),
                     farm.total, farm.drops);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
