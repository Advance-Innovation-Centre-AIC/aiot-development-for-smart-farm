/*
 * sf2_02_greenhouse_report.c -- โรงเรือนส่งรายงานทุก 5 วินาที
 * (The greenhouse reports every 5 seconds)
 *
 * วัดอากาศทุก 1 วินาที (อุณหภูมิ ความชื้น ความกด และแกน z ของบอร์ด)
 * ลูกบิด VR1 VR3 VR4 จำลองดิน แสง และถัง แล้วส่งรายงาน JSON ผ่าน MQTT
 * ทุก 5 วินาที โดยถามว่าถึงเวลาหรือยัง ไม่ใช่หลับรอ SW5 ส่งเดี๋ยวนี้
 * SW6 ส่ง "เหตุการณ์" เข้าอีกหัวข้อ เปิด farm_web.html?team=<เลขกลุ่ม>
 * บนมือถือแล้วดูค่าเปลี่ยน แก้ WIFI_SSID WIFI_PASS และ TEAM ก่อนรัน
 * พอร์ต 1883 ไม่เข้ารหัส ใครก็อ่านหัวข้อเราได้ ห้ามส่งของลับ
 *
 * Measures the air every second (temperature, humidity, pressure and
 * the board's z axis), takes VR1, VR3 and VR4 as a simulated soil,
 * light and tank, and sends a JSON report over MQTT every 5 seconds
 * by asking the clock whether it is time, not by sleeping. SW5 sends
 * one now; SW6 sends an "event" to a second topic. Open
 * farm_web.html?team=<team> on a phone and watch. Set WIFI_SSID,
 * WIFI_PASS and TEAM first. Port 1883 is not encrypted: send no
 * secrets.
 *
 * Twin of sf2_02_greenhouse_report.py
 * (aiot-development-for-smart-farm/s2/sf2_02_greenhouse_report.py).
 * Style: CLEAN_C.md. Differs from the original:
 *   - json.dumps is snprintf: the keys come in the order written,
 *     where MicroPython's dict may order them differently (the web
 *     app reads them by name). A real number is printed as str()
 *     prints it; None is null.
 *   - wifi.connect() and mqtt.connect() print lines of their own; the
 *     C calls print nothing. An SSID or password the radio cannot
 *     take counts as a failed join, as it does there.
 *   - A broker try that fails closes its client before the next try:
 *     MicroPython's next mqtt.connect() does that itself.
 *   - A failed sensor read gives the same None as the original's
 *     except.
 *   - In the BENTO Emulator, Edge AI/MQTT from C is not available yet:
 *     the program says so and ends, through the original's own
 *     "broker does not answer" stop.
 *   - The lesson's Stop exception travels as BENTO_E_CANCELLED, which
 *     main() turns into 0 as it does the IDE's Stop button.
 */
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_buttons.h"
#include "bento_fmt.h"
#include "bento_mqtt.h"
#include "bento_pots.h"
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_sensors.h"
#include "bento_time.h"
#include "bento_wifi.h"

/* ---- แก้ก่อนรัน: settings (MISRA deviation CT02) ---- */
/* ตั้งเอง: อังกฤษ/ตัวเลขสั้น ๆ ไม่มีเว้นวรรค */
static const char WIFI_SSID[] = "<ชื่อ Hotspot ของกลุ่ม>";
/* อย่างน้อย 8 ตัว · อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร */
static const char WIFI_PASS[] = "<รหัส Hotspot ของกลุ่ม>";
/* เลขกลุ่มที่ผู้สอนแจก เช่น team05 ห้ามซ้ำกลุ่มอื่น */
static const char TEAM[] = "teamXX";
/* ชดเชยความอุ่นจากชิป เช่น -7.0 */
static const bento_real_t TEMP_OFFSET = (bento_real_t)0.0;

/* สำรอง: "test.mosquitto.org" ถ้าผู้สอนประกาศ */
static const char BROKER[] = "broker.hivemq.com";
/* + TEAM + เลขจากนาฬิกาบอร์ดทุกครั้งที่ต่อ: ไม่ชน id เก่า */
static const char CLIENT_ID[] = "bento-farm-";
/* TOPIC = ROOT/TEAM/telemetry, TOPIC_EVENT = ROOT/TEAM/event */
static const char ROOT[] = "bento-aiot";

enum {
    READ_MS = 1000,
    SEND_MS = 5000,
    POLL_MS = 100,
    RUN_MS = 1800000 /* ส่งนาน 30 นาที */
};

static const bool SOUND = true; /* false = ดังเฉพาะตอนกด SW5 */

enum {
    SPEAKER = 40, /* ลำโพงรวม 0-100% (firmware 2.4.2 ขึ้นไป) */
    VOLUME = 25   /* ความดังเสียง 0-127 */
};
/* ---- end of settings ---- */

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

/* Times, scales and the link. */
enum {
    SETTLE_MS = 200, /* let the cleared screen settle */
    SAMPLE_MS = 20,  /* a short tap lasts longer than this */
    TONE_MS = 90,
    NOTE_MS = 100,
    KNOB_FULL = 4095,
    KNOB_COUNT = 3, /* VR1 soil, VR3 light, VR4 tank */
    PERCENT = 100,
    TENTHS = 10,
    HUNDREDTHS = 100,
    BROKER_TRIES = 3,
    PORT = 1883, /* not encrypted: anyone can read the topics */
    KEEPALIVE_S = 60,
    CLIENT_CLOCK_MASK = 0xFFFF,
    WIFI_SLICE_MS = 50,
    MQTT_SLICE_MS = 20,
    TEAM_CHARS = 6, /* "team" and two digits */
    TEAM_PREFIX = 4,
    CHART_POINTS = 400,
    BUTTON_SEND = 1, /* SW5 */
    BUTTON_CALL = 0  /* SW6 */
};

/* Text buffers, each sized for the longest text built in it. */
enum {
    TEXT_BYTES = 128,
    NAME_BYTES = 64,
    PAYLOAD_BYTES = 256
};

/* ---- sound, Stop and waiting ---- */

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

/* ---- 2) ฮาร์ดแวร์ ---- */

/* A reading that may be missing: Python's None. */
typedef struct {
    bool known;
    bento_real_t value;
} maybe_t;

/* What one round of reading gives; anything unreadable is unknown. */
typedef struct {
    maybe_t t;
    maybe_t h;
    maybe_t p;
    maybe_t az;
    int32_t knobs[KNOB_COUNT];
} readings_t;

/* The temperature and the humidity are read together: either fails,
 * both are unknown. */
static void read_climate(readings_t *now)
{
    float t_raw = 0.0F;
    float humidity = 0.0F;
    float hpa = 0.0F;

    now->t.known =
        (bento_sensors_sht40_temperature(&t_raw) == BENTO_OK) &&
        (bento_sensors_sht40_humidity(&humidity) == BENTO_OK);
    now->h.known = now->t.known;
    now->t.value = (bento_real_t)t_raw + TEMP_OFFSET;
    now->h.value = (bento_real_t)humidity;
    now->p.known = bento_sensors_dps368_pressure(&hpa) == BENTO_OK;
    now->p.value = (bento_real_t)hpa;
}

/* The z axis (m/s2): about 9.8 lying flat, less when tilted. */
static maybe_t read_az(void)
{
    bento_sensors_imu6_t motion;
    maybe_t az = {false, (bento_real_t)0.0};

    bento_sensors_imu6_init(&motion);
    if (bento_sensors_bmi270_motion(&motion) == BENTO_OK) {
        az.known = true;
        az.value = (bento_real_t)motion.az;
    }
    return az;
}

static bento_status_t knob_percent(uint32_t knob, int32_t *percent)
{
    uint16_t raw = 0U;
    const bento_status_t status = bento_pots_read(knob, &raw);

    *percent = ((int32_t)raw * PERCENT) / KNOB_FULL;
    return status;
}

typedef struct {
    uint32_t index;
    bool down;
    bool clicked;
} button_t;

/* A press counts once, when the button goes down. */
static bento_status_t sample_button(button_t *button)
{
    bool now_down = false;
    const bento_status_t status =
        bento_buttons_pressed(button->index, &now_down);

    if (status == BENTO_OK) {
        if (now_down && !button->down) {
            button->clicked = true;
        }
        button->down = now_down;
    }
    return status;
}

static bool pressed_now(button_t *button)
{
    const bool fired = button->clicked;

    button->clicked = false;
    return fired;
}

/* Waits ms, reading the buttons every SAMPLE_MS so a tap is seen. */
static bento_status_t wait_ms(uint32_t ms, button_t *buttons,
                              uint32_t count)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;
    uint32_t i;

    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            (int32_t)ms)) {
        for (i = 0U; (i < count) && (status == BENTO_OK); i++) {
            status = sample_button(&buttons[i]);
        }
        if (status == BENTO_OK) {
            status = pause_ms(SAMPLE_MS);
        }
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

static bool is_digit(uint8_t byte)
{
    return (byte >= (uint8_t)'0') && (byte <= (uint8_t)'9');
}

static bool team_is_set(void)
{
    return (strlen(TEAM) == (size_t)TEAM_CHARS) &&
           (strncmp(TEAM, "team", (size_t)TEAM_PREFIX) == 0) &&
           is_digit((uint8_t)TEAM[TEAM_PREFIX]) &&
           is_digit((uint8_t)TEAM[TEAM_PREFIX + 1]) &&
           (strcmp(TEAM, "team00") != 0);
}

/* round(x, digits) as JSON: str() of the rounded number at the board's
 * float width, or null. BENTO_FMT_BUF_SIZE holds any number. */
static void rounded_json(maybe_t reading, int32_t scale_10s, char *text)
{
    const bento_real_t scale = (bento_real_t)scale_10s;
    const bento_real_t scaled = reading.value * scale;
    uint32_t length = 0U;

    if (reading.known) {
        (void)bento_fmt_real(
            text, BENTO_FMT_BUF_SIZE,
            (bento_real_t)nearbyint((double)scaled) / scale, &length);
    } else {
        (void)snprintf(text, BENTO_FMT_BUF_SIZE, "null");
    }
}

/* One report with every key of MQTT contract 3.1: id and n always,
 * null for what could not be read. sim says which keys come from
 * knobs; by says whether the clock or SW5 sent it. */
static void build_payload(const readings_t *now, int32_t n, bool manual,
                          char *payload, size_t capacity)
{
    char t[BENTO_FMT_BUF_SIZE];
    char h[BENTO_FMT_BUF_SIZE];
    char p[BENTO_FMT_BUF_SIZE];
    char az[BENTO_FMT_BUF_SIZE];

    rounded_json(now->t, TENTHS, t);
    rounded_json(now->h, TENTHS, h);
    rounded_json(now->p, TENTHS, p);
    rounded_json(now->az, HUNDREDTHS, az);
    (void)snprintf(
        payload, capacity,
        "{\"id\": \"%s\", \"n\": %" PRId32
        ", \"temp_c\": %s, \"rh\": %s, \"hpa\": %s, \"az\": %s"
        ", \"soil\": %" PRId32 ", \"light\": %" PRId32
        ", \"tank\": %" PRId32
        ", \"sim\": \"soil light tank\", \"by\": \"%s\"}",
        TEAM, n, t, h, p, az, now->knobs[0], now->knobs[1],
        now->knobs[2], manual ? "sw5" : "timer");
}

/* ---- 4) เครือข่าย ---- */

typedef struct {
    char client_id[NAME_BYTES]; /* without the clock number */
    char topic[NAME_BYTES];
    char topic_event[NAME_BYTES];
} names_t;

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
    bento_wifi_connect_params_t params;
    bool waiting;
    bento_status_t status;

    bento_wifi_connect_params_init(&params);
    params.ssid = WIFI_SSID;
    params.ssid_len = (uint32_t)strlen(WIFI_SSID);
    params.pass = WIFI_PASS;
    params.pass_len = (uint32_t)strlen(WIFI_PASS);
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

/* One mqtt.connect(); a refused or silent broker is *linked false. */
static bento_status_t try_broker(const names_t *names, uint32_t *broker,
                                 bool *linked)
{
    char client_id[NAME_BYTES];
    bento_mqtt_config_t config;
    bool waiting;
    bento_status_t status;

    (void)snprintf(client_id, sizeof(client_id), "%s-%04" PRIx32,
                   names->client_id,
                   bento_time_ticks_ms() & (uint32_t)CLIENT_CLOCK_MASK);
    bento_mqtt_config_init(&config);
    config.host = BROKER;
    config.host_len = (uint32_t)strlen(BROKER);
    config.port = (uint16_t)PORT;
    config.keepalive_s = (uint16_t)KEEPALIVE_S;
    config.client_id = client_id;
    config.client_id_len = (uint32_t)strlen(client_id);
    status = bento_mqtt_open_start(&config, broker);
    waiting = status == BENTO_OK;
    while (waiting) {
        status = pause_ms(MQTT_SLICE_MS);
        if (status == BENTO_OK) {
            status = bento_mqtt_open_poll(*broker);
        }
        waiting = status == BENTO_E_IN_PROGRESS;
    }
    *linked = status == BENTO_OK;
    if (!*linked && (*broker != 0U)) {
        (void)bento_mqtt_close(*broker); /* frees it, whatever state */
        *broker = 0U;
    }
    return ((status == BENTO_E_CANCELLED) ||
            (status == BENTO_E_UNSUPPORTED))
               ? status
               : BENTO_OK;
}

typedef enum {
    SENT_OK,
    SENT_REFUSED, /* the broker would not take this one */
    SENT_LOST     /* the link is gone: publish() raises OSError */
} sent_t;

static sent_t publish_json(uint32_t broker, const char *topic,
                           const char *payload)
{
    const bento_status_t status = bento_mqtt_publish(
        broker, topic, (uint32_t)strlen(topic),
        (const uint8_t *)payload, (uint32_t)strlen(payload), 0U, 0U);
    sent_t sent = SENT_REFUSED;

    if (status == BENTO_OK) {
        sent = SENT_OK;
    } else if (status == BENTO_E_SESSION) {
        sent = SENT_LOST;
    } else {
        /* refused */
    }
    return sent;
}

/* ---- 5) หน้าจอ ---- */

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t led;
    bento_ui_id_t sent;
    bento_ui_id_t segs[KNOB_COUNT];
    bento_ui_id_t bars[KNOB_COUNT];
    bento_ui_id_t chart;
    uint8_t sent_series;
    bento_ui_id_t tilt;
    bento_ui_id_t next;
    bento_ui_id_t note;
    bento_ui_id_t link_led;
    bento_ui_id_t link_text;
} report_screen_t;

static void show_note(const report_screen_t *screen, const char *text,
                      uint32_t color)
{
    show_colored(screen->ui, screen->note, color, text);
}

static void show_link(const report_screen_t *screen, bool linked)
{
    show_value(screen->ui, screen->link_led, linked ? 1 : 0);
    show_text(screen->ui, screen->link_text,
              linked ? "MQTT: เชื่อมต่อแล้ว" : "MQTT: ออฟไลน์");
}

/* Three cards with a big number each, one per climate value. */
static bento_status_t build_numbers(report_screen_t *screen)
{
    static const char *const names[KNOB_COUNT] = {"อุณหภูมิ C", "ความชื้น %",
                                                  "ความกด hPa"};
    static const uint32_t colors[KNOB_COUNT] = {COLOR_OK, COLOR_INFO,
                                                COLOR_WARN};
    const int16_t step_px = 184;
    bento_status_t status = BENTO_OK;
    bento_ui_id_t unused;
    int32_t i;

    for (i = 0; (i < KNOB_COUNT) && (status == BENTO_OK); i++) {
        const int16_t x = (int16_t)(i * step_px);
        const box_t card = {
            (int16_t)(12 + x), 44, 176, 100, 0U, 0, 0, 0};
        const box_t seg = {(int16_t)(22 + x), 84, 150, 52,
                           colors[i],         0,  0,   0};

        status = add_card(screen->ui, &card, names[i], &unused);
        if (status == BENTO_OK) {
            status = add_box(screen->ui, UI_WIDGET_SEG7, &seg, "--",
                             &screen->segs[i]);
        }
    }
    return status;
}

/* The three simulated knobs as bars. */
static bento_status_t build_knobs(report_screen_t *screen)
{
    static const box_t card = {564, 44, 216, 100, 0U, 0, 0, 0};
    const int16_t step_px = 20;
    bento_ui_id_t unused;
    bento_status_t status =
        add_card(screen->ui, &card, "ดิน / แสง / ถัง (จำลอง)", &unused);
    int32_t i;

    for (i = 0; (i < KNOB_COUNT) && (status == BENTO_OK); i++) {
        const box_t bar = {
            576, (int16_t)(84 + (i * step_px)), 190, 14, 0U, 0, 0, 0};

        status = add_box(screen->ui, UI_WIDGET_BAR, &bar, NULL,
                         &screen->bars[i]);
    }
    return status;
}

/* 15-40 C, because 0-100 would flatten the line. LVGL draws no round
 * dots once the points reach the width. */
static bento_status_t build_chart(report_screen_t *screen)
{
    static const box_t card = {12, 152, 420, 186, 0U, 0, 0, 0};
    static const box_t chart = {22,         190, 400, 140,
                                COLOR_INFO, 15,  40,  0};
    bento_ui_id_t unused;
    bento_status_t status = add_card(
        screen->ui, &card, "กราฟ C: ฟ้า = บอร์ด  เขียว = ส่งแล้ว", &unused);

    if (status == BENTO_OK) {
        status = add_box(screen->ui, UI_WIDGET_CHART, &chart, NULL,
                         &screen->chart);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_prop(screen->ui, screen->chart,
                                (uint8_t)BENTO_UI_PROP_CHART_POINTS,
                                CHART_POINTS); /* a setter */
        status = bento_ui_chart_add_series(
            screen->ui, screen->chart, COLOR_OK, &screen->sent_series);
    }
    return status;
}

static bento_status_t build_tilt(report_screen_t *screen)
{
    static const box_t card = {442, 152, 338, 186, 0U, 0, 0, 0};
    static const spot_t tilt = {456, 192, COLOR_TEXT, 28};
    static const spot_t caption = {456, 258, COLOR_DIM, 20};
    static const box_t next = {456, 294, 300, 18, 0U, 0, SEND_MS, 0};
    bento_ui_id_t unused;
    bento_status_t status =
        add_card(screen->ui, &card, "az เทียบตอนเริ่ม", &unused);

    if (status == BENTO_OK) {
        status = add_label(screen->ui, &tilt, "--", &screen->tilt);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &caption, "ส่งใบถัดไปใน", &unused);
    }
    if (status == BENTO_OK) {
        status = add_box(screen->ui, UI_WIDGET_BAR, &next, NULL,
                         &screen->next);
    }
    return status;
}

static bento_status_t build_top(report_screen_t *screen)
{
    static const spot_t heading = {12, 6, COLOR_TEXT, 24};
    static const box_t led = {250, 12, 20, 20, COLOR_OK, 0, 0, 0};
    static const spot_t sent = {290, 12, COLOR_TEXT, 20};
    bento_ui_id_t unused;
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = pause_ms(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status =
            add_label(screen->ui, &heading, "โรงเรือนรายงานตัว", &unused);
    }
    if (status == BENTO_OK) {
        status = add_box(screen->ui, UI_WIDGET_LED, &led, NULL,
                         &screen->led);
    }
    if (status == BENTO_OK) {
        status =
            add_label(screen->ui, &sent, "ส่งแล้ว 0 ใบ", &screen->sent);
    }
    return status;
}

static bento_status_t build_screen(report_screen_t *screen)
{
    static const spot_t note = {12, 352, COLOR_DIM, 20};
    static const box_t link_led = {586, 12, 18, 18, COLOR_OK, 0, 0, 0};
    static const spot_t link_text = {612, 10, COLOR_DIM, 20};
    bento_status_t status = build_top(screen);

    if (status == BENTO_OK) {
        status = build_numbers(screen);
    }
    if (status == BENTO_OK) {
        status = build_knobs(screen);
    }
    if (status == BENTO_OK) {
        status = build_chart(screen);
    }
    if (status == BENTO_OK) {
        status = build_tilt(screen);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &note, "กำลังเริ่ม", &screen->note);
    }
    if (status == BENTO_OK) {
        status = add_box(screen->ui, UI_WIDGET_LED, &link_led, NULL,
                         &screen->link_led);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &link_text, "MQTT: ออฟไลน์",
                           &screen->link_text);
    }
    if (status == BENTO_OK) {
        refresh(screen->ui);
    }
    return status;
}

/* The screen shows az against the start; the report sends the real
 * value. A Seg7 takes a text, not a number. */
static void show_now(const report_screen_t *screen,
                     const readings_t *now, maybe_t az0)
{
    maybe_t climate[KNOB_COUNT];
    char text[BENTO_FMT_BUF_SIZE + 1U];
    uint32_t length = 0U;
    int32_t i;

    climate[0] = now->t;
    climate[1] = now->h;
    climate[2] = now->p;
    for (i = 0; i < KNOB_COUNT; i++) {
        (void)snprintf(text, sizeof(text), "--");
        if (climate[i].known) {
            (void)bento_fmt_real_precision(
                text, BENTO_FMT_BUF_SIZE, climate[i].value, 1, &length);
        }
        show_text(screen->ui, screen->segs[i], text);
    }
    for (i = 0; i < KNOB_COUNT; i++) {
        show_value(screen->ui, screen->bars[i], now->knobs[i]);
    }
    (void)snprintf(text, sizeof(text), "--");
    if (now->az.known && az0.known) {
        (void)bento_fmt_real_precision(&text[1], BENTO_FMT_BUF_SIZE,
                                       now->az.value - az0.value, 2,
                                       &length);
        text[0] = '+'; /* '%+.2f': a sign always */
        if (text[1] == '-') {
            (void)memmove(text, &text[1], (size_t)length + 1U);
        }
    }
    show_text(screen->ui, screen->tilt, text);
}

/* ---- 6) โปรแกรมหลัก ---- */

typedef struct {
    report_screen_t screen;
    names_t names;
    uint32_t broker;
    button_t buttons[2]; /* SW5 = send now, SW6 = call */
    readings_t now;
    maybe_t az0;    /* the pose at the start is zero tilt */
    maybe_t last_t; /* the temperature of the last report sent */
    int32_t sent;
    bool first;
    uint32_t read_ms;
    uint32_t send_ms;
} farm_t;

static bento_status_t stop(farm_t *farm, const char *text)
{
    bento_status_t status;

    show_link(&farm->screen, false);
    status = bento_rgbmatrix_clear();
    if (status == BENTO_OK) {
        show_value(farm->screen.ui, farm->screen.led, 0);
        show_note(&farm->screen, text, COLOR_BAD);
        refresh(farm->screen.ui);
        (void)printf("หยุดที่: %s\n", text);
        status = BENTO_E_CANCELLED;
    }
    return status;
}

/* The BENTO Emulator has no MQTT from C yet: every bento_mqtt_* call
 * answers BENTO_E_UNSUPPORTED there. Say so on the screen and the
 * console, then go on as if the broker did not answer, as the
 * original does. Any other status passes through. */
static bento_status_t emulator_gap(bento_ui_t *ui,
                                   bento_status_t status)
{
    static const char gap[] =
        "MQTT จาก C ยังไม่มีใน BENTO Emulator ให้รันบน Dev Kit หรือ Eva Kit";
    /* the Emulator does not wrap a label: the breaks are written in */
    static const char note[] =
        "MQTT จาก C ยังไม่มีใน BENTO Emulator\nให้รันบน Dev Kit หรือ Eva Kit";
    /* bottom right, ending before the firmware's Console button */
    static const box_t place = {316,        340, 370, 52,
                                COLOR_WARN, 0,   0,   20};
    bento_ui_id_t label;
    bento_status_t result = status;

    if (status == BENTO_E_UNSUPPORTED) {
        result = add_box(ui, UI_WIDGET_LABEL, &place, note, &label);
        refresh(ui);
        (void)printf(
            "%s / MQTT from C is not in the BENTO Emulator yet: "
            "run it on a Dev Kit or Eva Kit.\n",
            gap);
    }
    return result;
}

/* Three tries, each with a new client id: a public broker sometimes
 * does not answer for a while. */
static bento_status_t connect_broker(farm_t *farm, bool *linked)
{
    char text[TEXT_BYTES];
    int32_t n;
    bento_status_t status = BENTO_OK;

    *linked = false;
    for (n = 1; (n <= BROKER_TRIES) && !*linked && (status == BENTO_OK);
         n++) {
        if (n > 1) {
            (void)snprintf(text, sizeof(text),
                           "ลองต่อ broker ใหม่ %" PRId32 "/3", n);
            show_note(&farm->screen, text, COLOR_WARN);
            refresh(farm->screen.ui);
        }
        status = try_broker(&farm->names, &farm->broker, linked);
    }
    return status;
}

/* WiFi, an address, then the broker; *problem names the step that
 * failed, "" when all passed. */
static bento_status_t connect_farm(farm_t *farm, const char **problem)
{
    bool passed = false;
    bento_status_t status;

    show_note(&farm->screen, "ต่อ WiFi... จอนิ่งได้", COLOR_WARN);
    refresh(farm->screen.ui); /* the note must be up before the wait */
    status = join_wifi(&passed);
    *problem = "ต่อ WiFi ไม่ได้";
    if ((status == BENTO_OK) && passed) {
        status = connect_broker(farm, &passed);
        status = emulator_gap(farm->screen.ui, status);
        *problem = "broker ไม่ตอบ: รอ 1 นาทีแล้วรันใหม่";
    }
    if (passed) {
        *problem = "";
    }
    return status;
}

static bento_status_t start_farm(farm_t *farm)
{
    const char *problem = "";
    char text[TEXT_BYTES];
    bento_status_t status;

    /* no volume control on this firmware: skipped, as hasattr() does */
    (void)bento_ui_sfx_volume_set(farm->screen.ui, SPEAKER);
    status = build_screen(&farm->screen);
    if ((status == BENTO_OK) && !team_is_set()) {
        status = stop(farm, "แก้ TEAM เป็นเลขกลุ่มก่อน");
    }
    if (status == BENTO_OK) {
        farm->az0 = read_az();
        status = connect_farm(farm, &problem);
    }
    if ((status == BENTO_OK) && (problem[0] != '\0')) {
        status = stop(farm, problem);
    }
    if (status == BENTO_OK) {
        show_link(&farm->screen, true);
        show_value(farm->screen.ui, farm->screen.led, 1);
        (void)snprintf(text, sizeof(text), "ส่งเข้า %s",
                       farm->names.topic);
        show_note(&farm->screen, text, COLOR_OK);
    }
    return status;
}

/* SW6 is an event, not a report, so it goes to its own topic. */
static bento_status_t call_if_pressed(farm_t *farm)
{
    char payload[TEXT_BYTES];
    bento_status_t status = BENTO_OK;

    if (pressed_now(&farm->buttons[BUTTON_CALL])) {
        (void)snprintf(payload, sizeof(payload),
                       "{\"id\": \"%s\", \"event\": \"sw6\", \"msg\": "
                       "\"call\"}",
                       TEAM);
        if (publish_json(farm->broker, farm->names.topic_event,
                         payload) == SENT_LOST) {
            status = stop(farm, "สายหลุด ส่งไม่ออก");
        }
        if (status == BENTO_OK) {
            status = beep(farm->screen.ui, TUNE_TAP);
        }
    }
    return status;
}

/* One round of reading: the sensors, the tilt and the knobs. */
static bento_status_t read_round(readings_t *now)
{
    static const uint32_t knobs[KNOB_COUNT] = {0U, 2U,
                                               3U}; /* VR1 3 4 */
    bento_status_t status = BENTO_OK;
    int32_t i;

    read_climate(now);
    now->az = read_az();
    for (i = 0; (i < KNOB_COUNT) && (status == BENTO_OK); i++) {
        status = knob_percent(knobs[i], &now->knobs[i]);
    }
    return status;
}

/* set_next() is fire-and-forget, as in modui.c. */
static void plot_round(const farm_t *farm)
{
    if (farm->now.t.known) {
        (void)bento_ui_chart_set_next(farm->screen.ui,
                                      farm->screen.chart, 0U,
                                      (int32_t)farm->now.t.value);
    }
    if (farm->last_t.known) {
        (void)bento_ui_chart_set_next(
            farm->screen.ui, farm->screen.chart,
            farm->screen.sent_series, (int32_t)farm->last_t.value);
    }
}

/* Three clocks: the buttons every 0.1 s, the sensors every 1 s, the
 * report every 5 s. */
static bento_status_t read_if_due(farm_t *farm, uint32_t now_ms)
{
    bento_status_t status = BENTO_OK;

    if (farm->first ||
        (bento_time_ticks_diff(now_ms, farm->read_ms) >= READ_MS)) {
        farm->read_ms = now_ms;
        status = read_round(&farm->now);
        if (status == BENTO_OK) {
            show_now(&farm->screen, &farm->now, farm->az0);
            plot_round(farm);
        }
    }
    return status;
}

static bento_status_t on_sent(farm_t *farm, const char *payload,
                              bool manual)
{
    char text[TEXT_BYTES];
    bento_status_t status;

    (void)snprintf(text, sizeof(text), "ส่งแล้ว %" PRId32 " ใบ",
                   farm->sent);
    show_text(farm->screen.ui, farm->screen.sent, text);
    status = bento_rgbmatrix_score(farm->sent, BENTO_RGBMATRIX_GREEN);
    if ((status == BENTO_OK) && (SOUND || manual)) {
        status = beep(farm->screen.ui, TUNE_GOOD);
    }
    if (status == BENTO_OK) {
        (void)printf("ส่ง: %s\n", payload);
    }
    return status;
}

static bento_status_t send_if_due(farm_t *farm, uint32_t now_ms,
                                  bool manual)
{
    char payload[PAYLOAD_BYTES];
    char text[TEXT_BYTES];
    bento_status_t status = BENTO_OK;
    sent_t sent;

    if (farm->first || manual ||
        (bento_time_ticks_diff(now_ms, farm->send_ms) >= SEND_MS)) {
        farm->first = false;
        farm->send_ms = now_ms;
        build_payload(&farm->now, farm->sent + 1, manual, payload,
                      sizeof(payload));
        sent = publish_json(farm->broker, farm->names.topic, payload);
        if (sent == SENT_LOST) {
            status = stop(farm, "สายหลุด ส่งไม่ออก");
        } else if (sent == SENT_OK) {
            farm->sent++;
            farm->last_t = farm->now.t;
            status = on_sent(farm, payload, manual);
        } else {
            (void)snprintf(text, sizeof(text),
                           "ใบที่ %" PRId32 " ถูกปฏิเสธ", farm->sent + 1);
            show_note(&farm->screen, text, COLOR_WARN);
        }
    }
    return status;
}

static bento_status_t report_once(farm_t *farm)
{
    const uint32_t now_ms = bento_time_ticks_ms();
    const bool manual = pressed_now(&farm->buttons[BUTTON_SEND]);
    bento_status_t status = call_if_pressed(farm);

    if (status == BENTO_OK) {
        status = read_if_due(farm, now_ms);
    }
    if (status == BENTO_OK) {
        status = send_if_due(farm, now_ms, manual);
    }
    if (status == BENTO_OK) {
        show_value(farm->screen.ui, farm->screen.next,
                   bento_time_ticks_diff(bento_time_ticks_ms(),
                                         farm->send_ms));
        refresh(farm->screen.ui);
        status = wait_ms(POLL_MS, farm->buttons, 2U);
    }
    return status;
}

/* The normal end: the link closed, the lights off. */
static bento_status_t finish(farm_t *farm)
{
    char text[TEXT_BYTES];
    bento_status_t status;

    (void)bento_mqtt_close(farm->broker); /* a refusal was ignored */
    farm->broker = 0U;
    status = bento_rgbmatrix_clear();
    if (status == BENTO_OK) {
        show_value(farm->screen.ui, farm->screen.led, 0);
        (void)snprintf(text, sizeof(text), "จบ ส่งไป %" PRId32 " ใบ",
                       farm->sent);
        show_note(&farm->screen, text, COLOR_DIM);
    }
    return status;
}

static bento_status_t report(farm_t *farm)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    farm->first = true;
    farm->read_ms = start_ms;
    farm->send_ms = start_ms;
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = report_once(farm);
    }
    if (status == BENTO_OK) {
        status = finish(farm);
    }
    return status;
}

int main(void)
{
    farm_t farm;
    bento_status_t status;

    (void)memset(&farm, 0, sizeof(farm));
    farm.screen.ui = bento_runtime_get_ui();
    farm.buttons[BUTTON_SEND].index = 1U; /* SW5 */
    farm.buttons[BUTTON_CALL].index = 0U; /* SW6 */
    (void)snprintf(farm.names.client_id, sizeof(farm.names.client_id),
                   "%s%s", CLIENT_ID, TEAM);
    (void)snprintf(farm.names.topic, sizeof(farm.names.topic),
                   "%s/%s/telemetry", ROOT, TEAM);
    (void)snprintf(farm.names.topic_event,
                   sizeof(farm.names.topic_event), "%s/%s/event", ROOT,
                   TEAM);
    status = start_farm(&farm);
    if (status == BENTO_OK) {
        status = report(&farm);
    }
    if (farm.broker != 0U) { /* closed every time, even after Stop */
        (void)bento_mqtt_close(farm.broker); /* a refusal was ignored */
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
