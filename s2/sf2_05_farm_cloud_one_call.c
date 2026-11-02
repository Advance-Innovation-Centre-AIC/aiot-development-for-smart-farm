/*
 * sf2_05_farm_cloud_one_call.c -- ฟาร์มจำที่อยู่ของแพลตฟอร์มเอง
 * แล้วส่งข้อมูลด้วยคำสั่งเดียว
 * (The farm remembers the platform's address and sends with one call)
 *
 * อ่าน "คลังค่าตั้ง" tesaiot.config() บนแฟลช (ไม่ต้องมีเน็ต) แล้วโชว์ 6 คีย์
 * ในตาราง ถ้าใส่ DEVICE_ID และชื่อ broker ของแพลตฟอร์ม (PLATFORM_BROKER)
 * แล้ว จะเขียนสองคีย์นั้นลงแฟลช ต่อ WiFi ต่อแพลตฟอร์มแบบเข้ารหัส (TLS)
 * รอสายนานสุด 15 วินาที แล้วส่งอากาศ 6 ใบ ห่างกันใบละ 5 วินาทีด้วย
 * tesaiot.publish() ลองรันแบบ PLATFORM_BROKER ว่างก่อน แล้วจดค่าในตาราง
 * ลงใบงาน แก้ WIFI_SSID WIFI_PASS DEVICE_ID และ PLATFORM_BROKER ก่อนรัน
 * กับดัก: แพลตฟอร์มใช้ TLS พอร์ต 8883/8884 เสมอ จึงต่อ
 * broker.hivemq.com:1883 ไม่ได้
 *
 * Reads the settings store tesaiot.config() from flash (no network
 * needed) and shows six keys in a table. With DEVICE_ID and the
 * platform's broker (PLATFORM_BROKER) set, it writes those two keys
 * to flash, joins Wi-Fi, connects to the platform over TLS, waits up
 * to 15 s for the link and sends the air six times, 5 s apart, with
 * tesaiot.publish(). Run it first with PLATFORM_BROKER empty and note
 * the table on the worksheet. Set WIFI_SSID, WIFI_PASS, DEVICE_ID and
 * PLATFORM_BROKER first. The trap: the platform always uses TLS on
 * 8883/8884, so broker.hivemq.com:1883 cannot work.
 *
 * Twin of sf2_05_farm_cloud_one_call.py
 * (aiot-development-for-smart-farm/s2/sf2_05_farm_cloud_one_call.py).
 * Style: CLEAN_C.md. Differs from the original:
 *   - tesaiot.config() builds a dict of every setting; here the number
 *     of keys is counted and only the six shown are read. str()[:24]
 *     cuts 24 UTF-8 characters.
 *   - tesaiot.config_set() and tesaiot.connect() answer True or
 *     False, which the original ignores; here BENTO_OK and
 *     BENTO_E_FAILED are ignored the same way. Any other status (no
 *     TESAIoT service in this build) ends the program.
 *   - tesaiot.publish() is True when the message was queued; the C
 *     call answers BENTO_OK then.
 *   - json.dumps is snprintf with the keys in the order written;
 *     round(x, 1) is nearbyint(x * 10) / 10 at the board's float
 *     width, printed as str() prints it, null for None.
 *   - wifi.connect() prints lines of its own; the C call prints
 *     nothing. An SSID or password the radio cannot take counts as a
 *     failed join, as it does there. A failed sensor read gives the
 *     same None as the original's except.
 *   - In the BENTO Emulator, Edge AI/MQTT from C is not available yet
 *     (here the TESAIoT link): the program says so and ends (its
 *     status line carries the message).
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
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_sensors.h"
#include "bento_tesaiot.h"
#include "bento_time.h"
#include "bento_wifi.h"

/* ---- 1) ตั้งค่า (แก้ได้) ---- */
/* WIFI_SSID, WIFI_PASS: ตั้งใน join_wifi() (set them in join_wifi()) */
static const char DEVICE_ID[] = "<device_id ที่ผู้สอนแจก>";
/* ห้ามใส่ broker.hivemq.com: จะถูกเขียนลงแฟลชแล้วรอจนหมดเวลาเปล่า ๆ */
static const char PLATFORM_BROKER[] = "";
/* TEMP_OFFSET (ชดเชยความอุ่นจากชิป เช่น -7.0) และ HUM_FIX: ตั้งใน
 * read_climate() */

enum {
    WAIT_MS = 15000, /* รอต่อติดนานสุด 15 วิ แล้วเลิกรอ */
    POLL_MS = 250,
    SENDS = 6, /* ส่ง 6 ใบ ห่างกันใบละ 5 วิ */
    GAP_MS = 5000
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
    CARD_TITLE_PX = 20
};

/* Times, scales and the table. */
enum {
    SETTLE_MS = 200, /* let the cleared screen settle */
    TONE_MS = 90,
    NOTE_MS = 100,
    MS_PER_S = 1000,
    PERCENT = 100,
    TENTHS = 10,
    SCROLL_MS = 80,
    WIFI_SLICE_MS = 50,
    SHOW_KEYS = 6, /* the keys in the table */
    TABLE_ROWS = 3,
    TABLE_COLS = 4,
    VALUE_CHARS = 24
};

/* Text buffers, each sized for the longest text built in it. */
enum {
    TEXT_BYTES = 128,
    VALUE_BYTES = 128, /* BENTO_TESAIOT_CONFIG_VALUE_MAX and its NUL */
    PAYLOAD_BYTES = 128
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

/* The marquee (CLOUD) always stops first; then n, or 0 to clear. */
static bento_status_t matrix_show(int32_t n)
{
    bento_status_t status = bento_rgbmatrix_scroll(
        "", 0U, BENTO_RGBMATRIX_WHITE, SCROLL_MS);

    if ((status == BENTO_OK) && (n != 0)) {
        status = bento_rgbmatrix_score(n, BENTO_RGBMATRIX_CYAN);
    } else if (status == BENTO_OK) {
        status = bento_rgbmatrix_clear();
    } else {
        /* the marquee did not stop: its status ends the run */
    }
    return status;
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

/* ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ไม่แตะเน็ต ---- */

/* round(v, 1) as str() prints it, or none_text for None.
 * BENTO_FMT_BUF_SIZE holds any number. */
static void rounded_text(maybe_t reading, const char *none_text,
                         char *text)
{
    const bento_real_t scale = (bento_real_t)TENTHS;
    const bento_real_t scaled = reading.value * scale;
    uint32_t length = 0U;

    if (reading.known) {
        (void)bento_fmt_real(
            text, BENTO_FMT_BUF_SIZE,
            (bento_real_t)nearbyint((double)scaled) / scale, &length);
    } else {
        (void)snprintf(text, BENTO_FMT_BUF_SIZE, "%s", none_text);
    }
}

/* text[:keep], counting UTF-8 characters. */
static void cut_characters(char *text, uint32_t keep)
{
    const uint32_t top_bits = 0xC0U;
    const uint32_t follow = 0x80U;
    uint32_t characters = 0U;
    uint32_t i = 0U;
    bool going = true;

    while (going && (text[i] != '\0')) {
        const bool starts =
            (((uint32_t)(uint8_t)text[i]) & top_bits) != follow;

        going = !starts || (characters < keep);
        if (going) {
            characters += starts ? 1U : 0U;
            i++;
        }
    }
    text[i] = '\0';
}

/* One key of the store, cut to fit its cell; a key the store does not
 * have says so plainly. */
static bento_status_t config_text(const char *key, char *text,
                                  uint32_t capacity)
{
    uint32_t length = 0U;
    bento_status_t status = bento_tesaiot_config_get_field(
        key, (uint32_t)strlen(key), 0U, text, capacity, &length);

    if (status == BENTO_E_NOT_FOUND) {
        (void)snprintf(text, capacity, "ไม่มีคีย์นี้");
        status = BENTO_OK;
    }
    if (status == BENTO_OK) {
        cut_characters(text, VALUE_CHARS);
    }
    return status;
}

/* An empty PLATFORM_BROKER means only reading the store, not a
 * mistake, so it is orange. *ready: nothing stands in the way. */
static void setup_problem(uint32_t keys, char *text, uint32_t capacity,
                          uint32_t *color, bool *ready)
{
    *ready = false;
    if (PLATFORM_BROKER[0] == '\0') {
        (void)snprintf(text, capacity,
                       "อ่านได้ %" PRIu32 " คีย์ | ไม่ได้เขียนอะไรลงแฟลช",
                       keys);
        *color = COLOR_WARN;
    } else if (DEVICE_ID[0] == '<') {
        (void)snprintf(text, capacity, "ใส่ DEVICE_ID ที่ผู้สอนแจกก่อน");
        *color = COLOR_BAD;
    } else {
        *ready = true;
    }
}

/* ---- 4) เครือข่าย ---- */

/* config_set() and connect() answer True or False, which the original
 * ignores; any other status ends the run. */
static bento_status_t ignore_false(bento_status_t status)
{
    return (status == BENTO_E_FAILED) ? BENTO_OK : status;
}

/* Two keys written over, then the store read again to prove they
 * reached the flash (show_config). */
static bento_status_t save_platform(void)
{
    bento_status_t status = ignore_false(bento_tesaiot_config_set(
        "device_id", (uint32_t)strlen("device_id"), DEVICE_ID,
        (uint32_t)strlen(DEVICE_ID)));

    if (status == BENTO_OK) {
        status = ignore_false(bento_tesaiot_config_set(
            "broker", (uint32_t)strlen("broker"), PLATFORM_BROKER,
            (uint32_t)strlen(PLATFORM_BROKER)));
    }
    return status;
}

/* The address is "0.0.0.0" until DHCP answers. */
static bool has_ip(void)
{
    uint32_t address = 0U;

    return (bento_wifi_ip(BENTO_WIFI_IF_ANY, &address) == BENTO_OK) &&
           (address != 0U);
}

/* *joined is the original's True or False; any other failure ends the
 * run, as an OSError there does. */
static bento_status_t join_wifi(bool *joined)
{
    static const char wifi_pass[] =
        "<รหัส Hotspot ของกลุ่ม>"; /* อย่างน้อย 8 ตัว */
    static const char wifi_ssid[] =
        "<ชื่อ Hotspot ของกลุ่ม>"; /* อังกฤษ/ตัวเลขสั้น ๆ */
    bento_wifi_connect_params_t params;
    bool waiting;
    bento_status_t status;

    bento_wifi_connect_params_init(&params);
    params.ssid = wifi_ssid;
    params.ssid_len = (uint32_t)strlen(wifi_ssid);
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
    *joined = (status == BENTO_OK) && has_ip();
    if ((status == BENTO_E_TIMEOUT) || (status == BENTO_E_FAILED) ||
        (status == BENTO_E_INVALID_ARGUMENT)) {
        status = BENTO_OK;
    }
    return status;
}

/* tesaiot.is_connected(): a link that cannot be asked is down. */
static bool platform_is_up(void)
{
    bool connected = false;

    return (bento_tesaiot_is_connected(&connected) == BENTO_OK) &&
           connected;
}

/* ---- 5) หน้าจอ ---- */

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t status;
    bento_ui_id_t table;
    bento_ui_id_t led;
    bento_ui_id_t bar;
    bento_ui_id_t wait_ms;
    bento_ui_id_t sent;
    bento_ui_id_t note;
} cloud_screen_t;

/* Six rows would overflow the card: three rows of two key-value
 * pairs. */
static bento_status_t build_config_card(cloud_screen_t *screen)
{
    static const box_t card = {12, 44, 768, 218, 0U, 0, 0, 0};
    static const box_t table = {24, 84,         744,        172,
                                0U, TABLE_COLS, TABLE_ROWS, 0};
    static const int32_t widths[TABLE_COLS] = {140, 330, 140, 134};
    bento_ui_id_t unused;
    bento_status_t status =
        add_card(screen->ui, &card,
                 "tesaiot.config() บนแฟลช ถอดไฟแล้วยังอยู่", &unused);
    int32_t col;

    if (status == BENTO_OK) {
        status = add_box(screen->ui, UI_WIDGET_TABLE, &table, NULL,
                         &screen->table);
    }
    for (col = 0; (col < TABLE_COLS) && (status == BENTO_OK); col++) {
        (void)bento_ui_set_col_width(screen->ui, screen->table, col,
                                     widths[col]); /* a setter */
    }
    return status;
}

/* A Bar takes its colour only after it is made. */
static bento_status_t build_link_card(cloud_screen_t *screen)
{
    static const box_t card = {12, 266, 390, 82, 0U, 0, 0, 0};
    static const box_t led = {24, 305, 34, 34, COLOR_OK, 0, 0, 0};
    static const box_t bar = {120, 316, 140, 14, 0U, 0, WAIT_MS, 0};
    static const box_t seg = {272, 303, 120, 38, COLOR_INFO, 0, 0, 0};
    bento_ui_id_t unused;
    bento_status_t status =
        add_card(screen->ui, &card, "สาย TLS: รอมาแล้วกี่ ms", &unused);

    if (status == BENTO_OK) { /* 0 = dim = not linked yet */
        status = add_box(screen->ui, UI_WIDGET_LED, &led, NULL,
                         &screen->led);
    }
    if (status == BENTO_OK) {
        status = add_box(screen->ui, UI_WIDGET_BAR, &bar, NULL,
                         &screen->bar);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_color(screen->ui, screen->bar, COLOR_WARN);
        status = add_box(screen->ui, UI_WIDGET_SEG7, &seg, "0",
                         &screen->wait_ms);
    }
    return status;
}

static bento_status_t build_sent_card(cloud_screen_t *screen)
{
    static const box_t card = {412, 266, 368, 82, 0U, 0, 0, 0};
    static const box_t seg = {424, 303, 70, 38, COLOR_OK, 0, 0, 0};
    static const spot_t note = {506, 308, COLOR_DIM, 20};
    bento_ui_id_t unused;
    bento_status_t status = add_card(
        screen->ui, &card, "tesaiot.publish() ส่งแล้ว (ใบ)", &unused);

    if (status == BENTO_OK) {
        status = add_box(screen->ui, UI_WIDGET_SEG7, &seg, "0",
                         &screen->sent);
    }
    if (status == BENTO_OK) {
        status =
            add_label(screen->ui, &note, "ยังไม่ได้ส่ง", &screen->note);
    }
    return status;
}

static bento_status_t build_screen(cloud_screen_t *screen)
{
    static const spot_t heading = {12, 6, COLOR_TEXT, 24};
    static const spot_t status_line = {12, 352, COLOR_DIM, 20};
    bento_ui_id_t unused;
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = pause_ms(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &heading, "ฟาร์มจำที่อยู่คลาวด์เอง",
                           &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &status_line, "กำลังอ่านคลังค่าตั้ง",
                           &screen->status);
    }
    if (status == BENTO_OK) {
        status = build_config_card(screen);
    }
    if (status == BENTO_OK) {
        status = build_link_card(screen);
    }
    if (status == BENTO_OK) {
        status = build_sent_card(screen);
    }
    if (status == BENTO_OK) {
        refresh(screen->ui);
    }
    return status;
}

/* ui.poll() here: the line must be up before a call that blocks. */
static void show_status(const cloud_screen_t *screen, const char *text,
                        uint32_t color)
{
    show_colored(screen->ui, screen->status, color, text);
    refresh(screen->ui);
}

/* Columns 0-1 hold the first three keys, 2-3 the last three. Each
 * cell is fire-and-forget, as in modui.c. */
static bento_status_t show_config(const cloud_screen_t *screen)
{
    static const char *const keys[SHOW_KEYS] = {
        "device_id", "broker", "tls_mode", "port", "qos", "keepalive"};
    char value[VALUE_BYTES];
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < (uint32_t)SHOW_KEYS) && (status == BENTO_OK);
         i++) {
        const uint8_t row = (uint8_t)(i % (uint32_t)TABLE_ROWS);
        const uint8_t col = (uint8_t)((i / (uint32_t)TABLE_ROWS) * 2U);

        (void)bento_ui_set_cell(screen->ui, screen->table, row, col,
                                keys[i], (uint32_t)strlen(keys[i]));
        status = config_text(keys[i], value, sizeof(value));
        if (status == BENTO_OK) {
            (void)bento_ui_set_cell(screen->ui, screen->table, row,
                                    (uint8_t)(col + 1U), value,
                                    (uint32_t)strlen(value));
        }
    }
    if (status == BENTO_OK) {
        refresh(screen->ui);
    }
    return status;
}

/* A full bar = WAIT_MS = give up. */
static void show_wait(const cloud_screen_t *screen, int32_t waited_ms)
{
    char text[TEXT_BYTES];

    (void)snprintf(text, sizeof(text), "%" PRId32, waited_ms);
    show_text(screen->ui, screen->wait_ms, text);
    show_value(screen->ui, screen->bar,
               (waited_ms < WAIT_MS) ? waited_ms : WAIT_MS);
    refresh(screen->ui);
}

/* Green = linked, red = waited until the time ran out. */
static void mark_link(const cloud_screen_t *screen, uint32_t color)
{
    (void)bento_ui_set_color(screen->ui, screen->wait_ms, color);
    (void)bento_ui_set_color(screen->ui, screen->bar, color);
}

static bento_status_t show_online(const cloud_screen_t *screen)
{
    bento_status_t status;

    mark_link(screen, COLOR_OK);
    show_value(screen->ui, screen->led, 1);
    show_status(screen, "ต่อแพลตฟอร์มติดแล้ว", COLOR_OK);
    status = bento_rgbmatrix_scroll("CLOUD", (uint32_t)strlen("CLOUD"),
                                    BENTO_RGBMATRIX_CYAN, SCROLL_MS);
    if (status == BENTO_OK) {
        status = beep(screen->ui, TUNE_START);
    }
    return status;
}

static bento_status_t show_sent(const cloud_screen_t *screen, int32_t i,
                                int32_t sent, const char *t_text,
                                const char *h_text)
{
    char text[TEXT_BYTES];
    bento_status_t status;

    (void)snprintf(text, sizeof(text), "%" PRId32, sent);
    show_text(screen->ui, screen->sent, text);
    status = matrix_show(sent);
    if (status == BENTO_OK) {
        status = beep(screen->ui, TUNE_GOOD);
    }
    if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text),
                       "ใบที่ %" PRId32 " temp_c=%s rh=%s", i, t_text,
                       h_text);
        show_colored(screen->ui, screen->note, COLOR_TEXT, text);
    }
    return status;
}

/* ---- 6) โปรแกรมหลัก ---- */

static bento_status_t stop(const cloud_screen_t *screen,
                           const char *text, uint32_t color)
{
    bento_status_t status = matrix_show(0);

    if (status == BENTO_OK) {
        show_value(screen->ui, screen->led, 0);
        show_status(screen, text, color);
        (void)printf("หยุดที่: %s\n", text);
        status = BENTO_E_CANCELLED;
    }
    return status;
}

/* connect() answers at once: "taken", not "linked". Ask every
 * POLL_MS until linked (*linked) or WAIT_MS has passed. */
static bento_status_t wait_platform(const cloud_screen_t *screen,
                                    bool *linked)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = ignore_false(bento_tesaiot_connect_start());
    bool waiting = status == BENTO_OK;

    *linked = false;
    while (waiting) {
        const int32_t waited_ms =
            bento_time_ticks_diff(bento_time_ticks_ms(), start_ms);

        *linked = platform_is_up();
        waiting = !*linked;
        if (waiting) {
            show_wait(screen, waited_ms);
            waiting = waited_ms < WAIT_MS;
        }
        if (waiting) {
            status = pause_ms(POLL_MS);
            waiting = status == BENTO_OK;
        }
    }
    return status;
}

/* One message: the topic comes from device_id, the firmware builds
 * it. *ok is false when it was refused. */
static bento_status_t send_one(const cloud_screen_t *screen, int32_t i,
                               int32_t *sent)
{
    char t_text[BENTO_FMT_BUF_SIZE];
    char h_text[BENTO_FMT_BUF_SIZE];
    char t_json[BENTO_FMT_BUF_SIZE];
    char h_json[BENTO_FMT_BUF_SIZE];
    char payload[PAYLOAD_BYTES];
    char text[TEXT_BYTES];
    maybe_t t;
    maybe_t h;
    bento_status_t status = BENTO_OK;
    bool ok;

    read_climate(&t, &h);
    rounded_text(t, "None", t_text);
    rounded_text(h, "None", h_text);
    rounded_text(t, "null", t_json);
    rounded_text(h, "null", h_json);
    (void)snprintf(payload, sizeof(payload),
                   "{\"n\": %" PRId32 ", \"temp_c\": %s, \"rh\": %s}",
                   i, t_json, h_json);
    ok = bento_tesaiot_publish(NULL, 0U, (const uint8_t *)payload,
                               (uint32_t)strlen(payload), 0U,
                               NULL) == BENTO_OK;
    if (ok) {
        (*sent)++;
        status = show_sent(screen, i, *sent, t_text, h_text);
    } else {
        (void)snprintf(text, sizeof(text), "ใบที่ %" PRId32 " ถูกปฏิเสธ",
                       i);
        show_colored(screen->ui, screen->note, COLOR_WARN, text);
    }
    refresh(screen->ui);
    return status;
}

/* Answers how many went out; a refused one does not count. */
static bento_status_t send_all(const cloud_screen_t *screen,
                               int32_t *sent)
{
    char text[TEXT_BYTES];
    bento_status_t status = BENTO_OK;
    int32_t i;

    *sent = 0;
    for (i = 1; (i <= SENDS) && (status == BENTO_OK); i++) {
        if (!platform_is_up()) {
            (void)snprintf(text, sizeof(text), "สายหลุดที่ใบที่ %" PRId32,
                           i);
            status = stop(screen, text, COLOR_BAD);
        }
        if (status == BENTO_OK) {
            status = send_one(screen, i, sent);
        }
        if (status == BENTO_OK) {
            status = pause_ms(GAP_MS);
        }
    }
    return status;
}

/* The BENTO Emulator has no TESAIoT link from C yet: every
 * bento_tesaiot_* call answers BENTO_E_UNSUPPORTED there, starting
 * with the settings store. Say so on the status line and the console,
 * then end normally. Any other status passes through. */
static bento_status_t emulator_gap(const cloud_screen_t *screen,
                                   bento_status_t status)
{
    static const char gap[] = "TESAIoT จาก C ยังไม่มีใน BENTO Emulator "
                              "ให้รันบน Dev Kit หรือ Eva Kit";
    bento_status_t result = status;

    if (status == BENTO_E_UNSUPPORTED) {
        show_status(screen, gap, COLOR_WARN);
        (void)printf(
            "%s / TESAIoT from C is not in the BENTO Emulator yet: "
            "run it on a Dev Kit or Eva Kit.\n",
            gap);
        result = BENTO_E_CANCELLED; /* main() answers 0 */
    }
    return result;
}

/* 1) The store is read: no network needed. */
static bento_status_t read_store(const cloud_screen_t *screen)
{
    char text[TEXT_BYTES];
    uint32_t keys = 0U;
    uint32_t color = COLOR_WARN;
    bool ready = false;
    bento_status_t status = emulator_gap(screen, show_config(screen));

    if (status == BENTO_OK) {
        status = bento_tesaiot_config_key_count(&keys);
    }
    if (status == BENTO_OK) {
        (void)printf("คลังค่าตั้งมี %" PRIu32 " คีย์\n", keys);
        setup_problem(keys, text, sizeof(text), &color, &ready);
        if (!ready) {
            status = stop(screen, text, color);
        }
    }
    return status;
}

/* 2) Two keys written over, read back from the flash; 3) WiFi first,
 * the platform after; 4) waited for, with a time to give up. */
static bento_status_t link_platform(const cloud_screen_t *screen)
{
    char text[TEXT_BYTES];
    bool passed = false;
    bento_status_t status = save_platform();

    if (status == BENTO_OK) {
        status = show_config(screen);
    }
    if (status == BENTO_OK) {
        show_status(screen, "กำลังต่อ WiFi จอจะนิ่งสักครู่", COLOR_WARN);
        status = join_wifi(&passed);
    }
    if ((status == BENTO_OK) && !passed) {
        status =
            stop(screen, "ต่อ WiFi ไม่ได้ ค่าที่ตั้งไว้ยังอยู่บนแฟลช", COLOR_BAD);
    }
    if (status == BENTO_OK) {
        show_status(screen, "สั่งต่อแล้ว กำลังรอสาย TLS", COLOR_WARN);
        status = wait_platform(screen, &passed);
    }
    if ((status == BENTO_OK) && !passed) {
        mark_link(screen, COLOR_BAD);
        (void)snprintf(text, sizeof(text),
                       "รอ %" PRId32 " วิ ยังไม่ติด ตรวจ broker/ใบรับรอง",
                       (int32_t)WAIT_MS / (int32_t)MS_PER_S);
        status = stop(screen, text, COLOR_BAD);
    }
    return status;
}

int main(void)
{
    cloud_screen_t screen;
    char text[TEXT_BYTES];
    int32_t sent = 0;
    bento_status_t status;

    (void)memset(&screen, 0, sizeof(screen));
    screen.ui = bento_runtime_get_ui();
    /* no volume control on this firmware: skipped, as hasattr() does */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    status = build_screen(&screen);
    if (status == BENTO_OK) {
        status = read_store(&screen);
    }
    if (status == BENTO_OK) {
        status = link_platform(&screen);
    }
    if (status == BENTO_OK) {
        status = show_online(&screen);
    }
    if (status == BENTO_OK) {
        status = send_all(&screen, &sent); /* 5) one call each */
    }
    if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text),
                       "จบ ส่งได้ %" PRId32 " ใบ จาก %d", sent, SENDS);
        show_status(&screen, text, COLOR_OK);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
