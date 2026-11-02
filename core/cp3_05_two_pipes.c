/*
 * cp3_05_two_pipes.c -- หลักการ 3.5: ค่าเดียวกัน สองท่อ
 * (One value, two pipes: plain MQTT 1883 against MQTTS/TLS)
 *
 * ท่อ 1 = mqtt พอร์ต 1883 ไม่เข้ารหัส ใบที่ส่งวิ่งเป็นตัวอักษรตรง ๆ
 * ใครอยู่บนทางก็อ่านได้ จอโชว์ใบนั้นทีละตัวอักษร ท่อ 2 = tesaiot (TLS)
 * เข้ารหัสและตรวจบัตร broker ไฟล์นี้อ่านค่าตั้งของท่อ 2 จากแฟลช
 * (tls_mode, broker, พอร์ตที่ต่อจริง) อย่างเดียว ไม่ต่อ ไม่ส่ง
 * ไม่ตั้ง WiFi = เห็นใบที่ "จะ" ส่ง ตั้ง WiFi + TEAM = ท่อ 1 ส่งจริงทุก
 * SEND_MS ไปที่ bento-aiot/<TEAM>/core/pipes หมุน VR1 = ค่าที่ส่ง
 *
 * Pipe 1 is mqtt on port 1883, not encrypted: the message travels as
 * plain text that anyone on the way can read, and the screen shows it
 * letter by letter. Pipe 2 is tesaiot over TLS, encrypted and checking
 * the broker's certificate. This program only reads pipe 2's settings
 * from flash (tls_mode, broker and the port really used); it neither
 * connects nor sends there. Without WiFi settings it shows the message
 * it would send; with WiFi and TEAM set, pipe 1 sends every SEND_MS to
 * bento-aiot/<TEAM>/core/pipes. Turn VR1 to change the value sent.
 *
 * Twin of cp3_05_two_pipes.py
 * (aiot-development-for-smart-farm/core/cp3_05_two_pipes.py).
 * Style: CLEAN_C.md.
 * Differs from the original:
 *   - ui.volume has no hasattr test: a board without it answers
 *     BENTO_E_UNSUPPORTED, which is skipped as the .py skips the line.
 *   - tesaiot.config() is read field by field (tls_mode, broker); a
 *     field that cannot be read gives the "--" rows, as the .py's
 *     except does.
 *   - json.dumps is snprintf with the keys in the order the .py writes
 *     them; MicroPython's dict may print them in another order, which
 *     a JSON reader does not see.
 *   - mqtt.publish raises OSError only when the link is down; here the
 *     link is asked just before, and a refused publish is its False.
 *   - A broker try that fails is closed before the next one, so the
 *     three tries never hold more than one C client. mqtt.disconnect
 *     runs only for a client that was opened; one still open after a
 *     Stop during the connect steps is closed too.
 *   - wifi.* and mqtt.* print console lines of their own; here the
 *     program prints the ones C can know (connecting, connected,
 *     disconnected), not the radio's result codes.
 *   - The tune table keeps only the tunes this program plays.
 *   - In the BENTO Emulator, Edge AI/MQTT from C is not available yet:
 *     the program says so and runs offline, as the .py does.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_mqtt.h"
#include "bento_pots.h"
#include "bento_runtime.h"
#include "bento_tesaiot.h"
#include "bento_time.h"
#include "bento_wifi.h"

/* Left as they are, nothing is sent: the screen shows the message it
 * would send and pipe 2's settings only. TEAM is the group number the
 * teacher hands out, e.g. team05. */
static const char WIFI_SSID[] = "<ชื่อ Hotspot ของกลุ่ม>";
/* WIFI_PASS lives in connect_wifi() */
static const char TEAM[] = "teamXX";

/* The spare broker is test.mosquitto.org, if the teacher says so. */
/* BROKER lives in try_broker() */

/* Settings a student may change. */
enum {
    SEND_MS = 5000,  /* send a value this often */
    RUN_MS = 180000, /* the program ends after three minutes */
    SPEAKER = 40,    /* speaker volume, 0-100 % */
    VOLUME = 25,     /* tone velocity, 0-127 (about 20 %) */
    LOOP_MS = 100    /* look at the clock this often */
};

/* The network. */
enum {
    PORT = 1883, /* MQTT, not encrypted */
    KEEPALIVE_S = 60,
    BROKER_TRIES =
        3, /* some public brokers miss a connect now and then */
    WIFI_SLICE_MS = 50,  /* ask the join this often */
    MQTT_SLICE_MS = 20,  /* ask the broker connect this often */
    CLOCK_MASK = 0xFFFF, /* the clock bits that end the client id */
    TEAM_PREFIX = 4,     /* "team" before the digits */
    CLIENT_BYTES = 32,   /* "bento-pipe-teamXX-ffff" and its NUL */
    TOPIC_BYTES = 48,    /* "bento-aiot/teamXX/core/pipes" */
    BODY_BYTES = 96,     /* the JSON message with any int32_t */
    TEXT_BYTES = 96,     /* a line on screen with any int32_t */
    CONFIG_BYTES = 128,  /* one tesaiot setting: 127 bytes and NUL */
    PORT_BYTES = 8,      /* "8883" */
    MODE_PREFIX = 4      /* "mTLS" */
};

/* Measuring. */
enum {
    PERCENT = 100,
    ADC_FULL = 4095, /* pots.read() at the right end */
    VR1 = 0
};

/* Sound. */
enum {
    NOTE_MS = 90,
    NOTE_GAP_MS = 100,
    TUNE_NOTES_MAX = 3
};

typedef enum {
    TUNE_START = 0,
    TUNE_GOOD,
    TUNE_BAD
} tune_t;

/* Screen layout. */
enum {
    MARGIN_X = 12,
    TITLE_Y = 6,
    TOP_Y = 12,
    MQTT_X = 380,
    LINK_LED_X = 430,
    LINK_LED_SIZE = 20,
    LINK_X = 458,
    CARD_Y = 44,
    CARD_H = 294,
    PIPE1_W = 380,
    PIPE2_X = 402,
    PIPE2_W = 378,
    CARD_TEXT_X = 12,
    CARD_TEXT_Y = 6,
    CARD_RADIUS = 12,
    PIPE1_TEXT_X = 28,
    PIPE2_TEXT_X = 418,
    WARNING_Y = 76,
    WIRE_TITLE_Y = 116,
    WIRE_Y = 148,
    PIPE_LINE_Y = 290,
    ROW_NAME_Y = 116,
    ROW_VALUE_Y = 134,
    ROW_STEP = 50,
    ROW_COUNT = 3,
    NOTE_Y = 352,
    FONT_THAI = 20, /* Thai keeps its strokes from 20 px */
    FONT_TINY = 14,
    FONT_SMALL = 16,
    FONT_TITLE = 24,
    SETTLE_MS = 200 /* let the cleared screen settle */
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

/* Where the Emulator's "no MQTT from C" line sits: on space that shows
 * nothing at that point of the run. */
enum {
    GAP_X = 28,
    GAP_Y = 200,
    GAP_W = 352 /* the line wraps inside its card */
};

typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
} rect_t;

/* Pipe 2's three rows, as text. */
typedef struct {
    char mode[CONFIG_BYTES];
    char broker[CONFIG_BYTES];
    char port[PORT_BYTES];
} pipe2_rows_t;

/* The widgets the program changes after building the screen. */
typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t led;
    bento_ui_id_t link;
    bento_ui_id_t wire;     /* the message as it travels in pipe 1 */
    bento_ui_id_t pipe_one; /* pipe 1's state line */
    bento_ui_id_t note;
} pipes_screen_t;

/* The broker client and the names built from TEAM. */
typedef struct {
    uint32_t broker; /* 0 = no client */
    char client_base[CLIENT_BYTES];
    char client_id[CLIENT_BYTES];
    char topic[TOPIC_BYTES];
    bool no_mqtt; /* the BENTO Emulator: no MQTT from C yet */
} link_t;

/* What the sending loop keeps. */
typedef struct {
    const char *problem; /* NULL = pipe 1 is sending */
    int32_t made;        /* messages made */
    int32_t sent;        /* messages pipe 1 took */
    uint32_t start_ms;
    uint32_t send_ms;
} pipe_t;

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

/* A screen refresh whose events nobody reads. */
static void present(bento_ui_t *ui)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    /* ui.poll() answers no events when it fails, and this program
     * reads none */
    (void)bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                        NULL);
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
    static const uint8_t notes[][TUNE_NOTES_MAX] = {
        {72U, 79U, 0U}, {72U, 79U, 84U}, {84U, 76U, 0U}};
    static const uint32_t note_count[] = {2U, 3U, 2U};
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

/* VR1 as 0-100 %: the value sent. */
static bento_status_t read_soil(int32_t *percent)
{
    uint16_t raw = 0U;
    const bento_status_t status = bento_pots_read(VR1, &raw);

    *percent = ((int32_t)raw * PERCENT) / ADC_FULL;
    return status;
}

static bento_status_t read_setting(const char *key, char *out,
                                   uint32_t capacity)
{
    uint32_t length = 0U;

    return bento_tesaiot_config_get_field(key, (uint32_t)strlen(key),
                                          0U, out, capacity, &length);
}

/* tesaiot's settings from flash, no network, as three rows to show:
 * mode, broker and the port really used. The port follows the mode
 * as the firmware picks it: both mTLS modes 8883, any other 8884. No
 * device id or key is read. A setting that cannot be read gives
 * dashes. */
static void read_config(pipe2_rows_t *rows)
{
    const bool known = (read_setting("tls_mode", rows->mode,
                                     sizeof(rows->mode)) == BENTO_OK) &&
                       (read_setting("broker", rows->broker,
                                     sizeof(rows->broker)) == BENTO_OK);

    if (known) {
        (void)snprintf(
            rows->port, sizeof(rows->port), "%s",
            (strncmp(rows->mode, "mTLS", (size_t)MODE_PREFIX) == 0)
                ? "8883"
                : "8884");
    } else {
        (void)snprintf(rows->mode, sizeof(rows->mode), "--");
        (void)snprintf(rows->broker, sizeof(rows->broker), "--");
        (void)snprintf(rows->port, sizeof(rows->port), "--");
    }
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

/* A card: a dark box with a grey edge and a blue title. */
static bento_status_t add_card(bento_ui_t *ui, rect_t box,
                               const char *title)
{
    bento_ui_widget_args_t args;
    bento_ui_id_t unused;
    const point_t at = {px(box.x + CARD_TEXT_X),
                        px(box.y + CARD_TEXT_Y)};
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = box.x;
    args.y = box.y;
    args.w = box.w;
    args.h = box.h;
    args.color = COL_CARD;
    args.min_val = COL_DIM;
    args.max_val = CARD_RADIUS;
    args.font_px = 1;
    status = bento_ui_panel_create(ui, &args, &unused);
    if (status == BENTO_OK) {
        status = add_label(ui, title, at, COL_INFO, FONT_THAI, &unused);
    }
    return status;
}

static bento_status_t add_header(pipes_screen_t *screen)
{
    bento_ui_widget_args_t args;
    bento_ui_id_t unused;
    const point_t title_at = {MARGIN_X, TITLE_Y};
    const point_t mqtt_at = {MQTT_X, TOP_Y};
    const point_t link_at = {LINK_X, TOP_Y};
    bento_status_t status =
        add_label(screen->ui, "ค่าเดียวกัน สองท่อ", title_at, COL_TEXT,
                  FONT_TITLE, &unused);

    if (status == BENTO_OK) {
        status = add_label(screen->ui, "MQTT", mqtt_at, COL_DIM,
                           FONT_SMALL, &unused);
    }
    bento_ui_widget_args_init(&args);
    args.x = LINK_LED_X;
    args.y = TOP_Y;
    args.w = LINK_LED_SIZE;
    args.h = LINK_LED_SIZE;
    args.color = COL_OK;
    if (status == BENTO_OK) {
        status = bento_ui_led_create(screen->ui, &args, &screen->led);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "ออฟไลน์", link_at, COL_DIM,
                           FONT_THAI, &screen->link);
    }
    return status;
}

/* Pipe 1: plain mqtt, readable letter by letter. */
static bento_status_t add_pipe_one(pipes_screen_t *screen)
{
    const rect_t box = {MARGIN_X, CARD_Y, PIPE1_W, CARD_H};
    const point_t warning_at = {PIPE1_TEXT_X, WARNING_Y};
    const point_t title_at = {PIPE1_TEXT_X, WIRE_TITLE_Y};
    const point_t wire_at = {PIPE1_TEXT_X, WIRE_Y};
    const point_t line_at = {PIPE1_TEXT_X, PIPE_LINE_Y};
    bento_ui_id_t unused;
    bento_status_t status =
        add_card(screen->ui, box, "ท่อ 1: mqtt พอร์ต 1883");

    if (status == BENTO_OK) {
        status = add_label(screen->ui, "ไม่เข้ารหัส: ใครก็อ่านได้",
                           warning_at, COL_BAD, FONT_THAI, &unused);
    }
    if (status == BENTO_OK) {
        status =
            add_label(screen->ui, "ใบที่วิ่งในสาย (ตัวอักษรตรง ๆ):", title_at,
                      COL_DIM, FONT_THAI, &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", wire_at, COL_TEXT,
                           FONT_SMALL, &screen->wire);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", line_at, COL_DIM, FONT_THAI,
                           &screen->pipe_one);
    }
    return status;
}

/* Pipe 2's rows: a name, then its value read from flash. */
static bento_status_t add_pipe_two_rows(bento_ui_t *ui,
                                        const pipe2_rows_t *rows)
{
    static const char *const names[ROW_COUNT] = {"tls_mode", "broker",
                                                 "port"};
    const char *values[ROW_COUNT];
    bento_ui_id_t unused;
    bento_status_t status = BENTO_OK;
    int32_t i;

    values[0] = rows->mode;
    values[1] = rows->broker;
    values[2] = rows->port;
    for (i = 0; (i < ROW_COUNT) && (status == BENTO_OK); i++) {
        const point_t name_at = {PIPE2_TEXT_X,
                                 px(ROW_NAME_Y + (i * ROW_STEP))};
        const point_t value_at = {PIPE2_TEXT_X,
                                  px(ROW_VALUE_Y + (i * ROW_STEP))};

        status = add_label(ui, names[i], name_at, COL_DIM, FONT_TINY,
                           &unused);
        if (status == BENTO_OK) {
            /* an mTLS mode name is 47 letters long */
            status = add_label(ui, values[i], value_at, COL_TEXT,
                               FONT_TINY, &unused);
        }
    }
    return status;
}

/* Pipe 2: tesaiot over TLS, its settings only. */
static bento_status_t add_pipe_two(bento_ui_t *ui,
                                   const pipe2_rows_t *rows)
{
    const rect_t box = {PIPE2_X, CARD_Y, PIPE2_W, CARD_H};
    const point_t good_at = {PIPE2_TEXT_X, WARNING_Y};
    const point_t line_at = {PIPE2_TEXT_X, PIPE_LINE_Y};
    bento_ui_id_t unused;
    bento_status_t status = add_card(ui, box, "ท่อ 2: tesaiot (TLS)");

    if (status == BENTO_OK) {
        status = add_label(ui, "เข้ารหัส + ตรวจบัตร broker", good_at,
                           COL_OK, FONT_THAI, &unused);
    }
    if (status == BENTO_OK) {
        status = add_pipe_two_rows(ui, rows);
    }
    if (status == BENTO_OK) {
        status = add_label(ui, "ไฟล์นี้อ่านค่าตั้งอย่างเดียว ไม่ต่อ", line_at,
                           COL_DIM, FONT_THAI, &unused);
    }
    return status;
}

static bento_status_t build_screen(pipes_screen_t *screen,
                                   const pipe2_rows_t *rows)
{
    const point_t note_at = {MARGIN_X, NOTE_Y};
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = sleep_ms((uint32_t)SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_header(screen);
    }
    if (status == BENTO_OK) {
        status = add_pipe_one(screen);
    }
    if (status == BENTO_OK) {
        status = add_pipe_two(screen->ui, rows);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "หมุน VR1 = ค่าที่ส่ง", note_at,
                           COL_DIM, FONT_THAI, &screen->note);
    }
    if (status == BENTO_OK) {
        present(screen->ui);
    }
    return status;
}

/* One label's colour and text, then onto the screen; only when
 * something happened. */
static void show(bento_ui_t *ui, bento_ui_id_t label, const char *text,
                 uint32_t color)
{
    /* the widget setters ignore a refusal, as modui.c does */
    (void)bento_ui_set_color(ui, label, color);
    set_text(ui, label, text);
    present(ui);
}

/* Pipe 1's MQTT light: on = linked to the broker, dim = offline. */
static void show_link(const pipes_screen_t *screen, bool online)
{
    (void)bento_ui_set_value(screen->ui, screen->led, online ? 1 : 0);
    show(screen->ui, screen->link, online ? "เชื่อมต่อแล้ว" : "ออฟไลน์",
         online ? COL_OK : COL_DIM);
}

/* "team" followed by digits only; teamXX is not set yet. */
static bool team_is_set(void)
{
    const size_t length = strlen(TEAM);
    bool digits = length > (size_t)TEAM_PREFIX;
    size_t i;

    for (i = (size_t)TEAM_PREFIX; digits && (i < length); i++) {
        digits = (TEAM[i] >= '0') && (TEAM[i] <= '9');
    }
    return digits;
}

/* Asks the started join every WIFI_SLICE_MS until it ends. */
static bento_status_t wait_connect(void)
{
    bento_status_t status = BENTO_OK;
    bool waiting = true;

    while (waiting) {
        status = bento_wifi_connect_poll();
        waiting = status == BENTO_E_IN_PROGRESS;
        if (waiting) {
            status = sleep_ms((uint32_t)WIFI_SLICE_MS);
            waiting = status == BENTO_OK;
        }
    }
    if (status == BENTO_E_CANCELLED) {
        (void)bento_wifi_connect_cancel(); /* answers BENTO_OK always */
    }
    return status;
}

/* wifi.ip() is "0.0.0.0" while there is no address. */
static bool has_address(void)
{
    char text[BENTO_WIFI_IPV4_TEXT_MAX];
    uint32_t address = 0U;
    uint32_t length = 0U;
    bool known =
        (bento_wifi_ip(BENTO_WIFI_IF_ANY, &address) == BENTO_OK) &&
        (bento_wifi_ipv4_format(address, text, sizeof(text), &length) ==
         BENTO_OK);

    if (known) {
        (void)printf("[WiFi] Connected! IP=%s\n", text);
    }
    return known && (strcmp(text, "0.0.0.0") != 0);
}

/* wifi.connect() and an address: *joined false is the original's
 * False; any other failure ends the run, as its OSError does. The
 * binding saves the network after a join, so the twin asks for that
 * too. */
static bento_status_t connect_wifi(bool *joined)
{
    static const char wifi_pass[] = "<รหัส Hotspot ของกลุ่ม>";
    bento_wifi_connect_params_t params;
    bento_status_t status;

    bento_wifi_connect_params_init(&params);
    params.ssid = WIFI_SSID;
    params.ssid_len = (uint32_t)strlen(WIFI_SSID);
    params.pass = wifi_pass;
    params.pass_len = (uint32_t)strlen(wifi_pass);
    params.flags = BENTO_WIFI_F_SAVE_CREDS;
    (void)printf("[WiFi] Connecting...\n");
    status = bento_wifi_connect_start(&params);
    if (status == BENTO_OK) {
        status = wait_connect();
    }
    *joined = (status == BENTO_OK) && has_address();
    if ((status == BENTO_E_TIMEOUT) || (status == BENTO_E_FAILED)) {
        status = BENTO_OK;
    }
    return status;
}

/* Asks the started connect every MQTT_SLICE_MS until it ends. */
static bento_status_t wait_broker(uint32_t broker)
{
    bento_status_t status = BENTO_OK;
    bool waiting = true;

    while (waiting) {
        status = sleep_ms((uint32_t)MQTT_SLICE_MS);
        if (status == BENTO_OK) {
            status = bento_mqtt_open_poll(broker);
        }
        waiting = status == BENTO_E_IN_PROGRESS;
    }
    return status;
}

/* One try with a fresh client id (a number from the board's clock),
 * so it never clashes with an old one. Only Stop is an error. */
static bento_status_t try_broker(link_t *link, bool *linked)
{
    static const char broker[] = "broker.hivemq.com";
    bento_mqtt_config_t config;
    bento_status_t status;

    (void)snprintf(link->client_id, sizeof(link->client_id),
                   "%s-%04" PRIx32, link->client_base,
                   bento_time_ticks_ms() & (uint32_t)CLOCK_MASK);
    bento_mqtt_config_init(&config);
    config.host = broker;
    config.host_len = (uint32_t)strlen(broker);
    config.port = (uint16_t)PORT;
    config.keepalive_s = (uint16_t)KEEPALIVE_S;
    config.client_id = link->client_id;
    config.client_id_len = (uint32_t)strlen(link->client_id);
    (void)printf("[MQTT] Connecting to %s:%d ...\n", broker, PORT);
    status = bento_mqtt_open_start(&config, &link->broker);
    if (status == BENTO_OK) {
        status = wait_broker(link->broker);
    }
    *linked = status == BENTO_OK;
    link->no_mqtt = status == BENTO_E_UNSUPPORTED;
    if (*linked) {
        (void)printf("[MQTT] Connected to %s:%d as '%s'\n", broker,
                     PORT, link->client_id);
    } else if ((status != BENTO_E_CANCELLED) && (link->broker != 0U)) {
        (void)bento_mqtt_close(link->broker); /* frees it, any state */
        link->broker = 0U;
    } else {
        /* nothing started, or Stop: main closes what is open */
    }
    return (status == BENTO_E_CANCELLED) ? status : BENTO_OK;
}

/* mqtt.is_connected(): an unknown state counts as dropped. */
static bool link_is_up(uint32_t broker)
{
    uint32_t state = BENTO_MQTT_STATE_CLOSED;

    if (bento_mqtt_state(broker, &state) != BENTO_OK) {
        state = BENTO_MQTT_STATE_CLOSED;
    }
    return state == BENTO_MQTT_STATE_CONNECTED;
}

static bento_status_t connect_broker(const pipes_screen_t *screen,
                                     link_t *link, bool *linked)
{
    char text[TEXT_BYTES];
    int32_t attempt;
    bento_status_t status = BENTO_OK;

    *linked = false;
    for (attempt = 1; (attempt <= BROKER_TRIES) && !*linked &&
                      !link->no_mqtt && (status == BENTO_OK);
         attempt++) {
        if (attempt > 1) {
            (void)snprintf(text, sizeof(text),
                           "ลองต่อ broker ใหม่ %" PRId32 "/3", attempt);
            show(screen->ui, screen->pipe_one, text, COL_WARN);
        }
        status = try_broker(link, linked);
    }
    return status;
}

/* The BENTO Emulator's C runtime has no MQTT yet (every bento_mqtt_*
 * call answers BENTO_E_UNSUPPORTED): say so on screen and in the
 * console, in bento-ide's words. */
static bento_status_t show_emulator_gap(bento_ui_t *ui)
{
    static const char gap[] = "MQTT จาก C ยังไม่มีใน BENTO Emulator "
                              "ให้รันบน Dev Kit หรือ Eva Kit";
    /* the Emulator does not wrap a label: explicit breaks, two lines
     * of 348/241 px; the printf keeps the one-line text */
    static const char gap_lines[] =
        "MQTT จาก C ยังไม่มีใน BENTO Emulator\n"
        "ให้รันบน Dev Kit หรือ Eva Kit";
    bento_ui_widget_args_t args;
    bento_ui_id_t label;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.text = gap_lines;
    args.text_len = (uint32_t)strlen(gap_lines);
    args.x = GAP_X;
    args.y = GAP_Y;
    args.w = GAP_W;
    args.color = COL_WARN;
    args.font_px = FONT_THAI;
    status = bento_ui_label_create(ui, &args, &label);
    (void)printf("%s / MQTT from C is not in the BENTO Emulator yet: "
                 "run it on a Dev Kit or Eva Kit.\n",
                 gap);
    if (status == BENTO_OK) {
        present(ui);
    }
    return status;
}

/* Pipe 1: WiFi, an address, then the broker on port 1883. *problem
 * says where it broke, or stays NULL. */
static bento_status_t connect_plain(const pipes_screen_t *screen,
                                    link_t *link, const char **problem)
{
    bool passed = team_is_set();
    bento_status_t status = BENTO_OK;

    if (!passed) { /* teamXX: no link, nothing goes out */
        *problem = "แก้ TEAM ก่อน";
    } else {
        /* the label is up before the line that blocks */
        show(screen->ui, screen->pipe_one, "ต่อ WiFi...", COL_WARN);
        status = connect_wifi(&passed);
        *problem = passed ? NULL : "ต่อ WiFi ไม่ได้";
    }
    if ((status == BENTO_OK) && passed) {
        status = connect_broker(screen, link, &passed);
        *problem = passed ? NULL : "broker ไม่ตอบ";
    }
    /* then the .py's own offline path: the broker did not answer */
    if ((status == BENTO_OK) && link->no_mqtt) {
        status = show_emulator_gap(screen->ui);
    }
    return status;
}

/* The link dropped: say so on screen and stop sending. */
static void drop_link(const pipes_screen_t *screen, pipe_t *pipe)
{
    pipe->problem = "สายหลุด";
    show_link(screen, false);
    show(screen->ui, screen->pipe_one, pipe->problem, COL_BAD);
}

/* The same message pipe 2 would encrypt before it leaves the
 * board. */
static bento_status_t send_value(const pipes_screen_t *screen,
                                 const link_t *link, pipe_t *pipe)
{
    char body[BODY_BYTES];
    char text[TEXT_BYTES];
    int32_t soil = 0;
    bento_status_t status = read_soil(&soil);

    pipe->made++;
    (void)snprintf(body, sizeof(body),
                   "{\"id\": \"%s\", \"n\": %" PRId32
                   ", \"soil\": %" PRId32 "}",
                   TEAM, pipe->made, soil);
    if (status == BENTO_OK) {
        set_text(screen->ui, screen->wire, body);
    }
    if ((status == BENTO_OK) && (pipe->problem == NULL)) {
        if (!link_is_up(link->broker)) { /* publish raises: link down */
            drop_link(screen, pipe);
            status = beep(screen->ui, TUNE_BAD);
        } else if (bento_mqtt_publish(link->broker, link->topic,
                                      (uint32_t)strlen(link->topic),
                                      (const uint8_t *)body,
                                      (uint32_t)strlen(body), 0U,
                                      0U) == BENTO_OK) {
            pipe->sent++;
            (void)snprintf(text, sizeof(text),
                           "ส่งแล้ว %" PRId32 " ใบ (อ่านได้ทุกตัว)",
                           pipe->sent);
            set_text(screen->ui, screen->pipe_one, text);
        } else {
            /* the broker took it not: the next one tries again */
        }
    }
    if (status == BENTO_OK) {
        present(screen->ui);
    }
    return status;
}

/* The first message goes at once, then one every SEND_MS. */
static bento_status_t run_round(const pipes_screen_t *screen,
                                const link_t *link, pipe_t *pipe)
{
    uint32_t now;
    bento_status_t status = BENTO_OK;

    if ((pipe->problem == NULL) && !link_is_up(link->broker)) {
        drop_link(screen, pipe);
    }
    now = bento_time_ticks_ms();
    if ((pipe->made == 0) ||
        (bento_time_ticks_diff(now, pipe->send_ms) >= SEND_MS)) {
        pipe->send_ms = now;
        status = send_value(screen, link, pipe);
    }
    if (status == BENTO_OK) {
        status = sleep_ms((uint32_t)LOOP_MS);
    }
    return status;
}

static bento_status_t run_pipes(const pipes_screen_t *screen,
                                const link_t *link, pipe_t *pipe)
{
    bento_status_t status = BENTO_OK;

    pipe->start_ms = bento_time_ticks_ms();
    pipe->send_ms = pipe->start_ms;
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(),
                                  pipe->start_ms) < RUN_MS)) {
        status = run_round(screen, link, pipe);
    }
    return status;
}

/* Pipe 1 when WiFi is set, the result on its line, then the sending
 * loop. A stop on the way still unlinks the broker and dims the
 * light. */
static bento_status_t run_program(const pipes_screen_t *screen,
                                  link_t *link)
{
    pipe_t pipe;
    bento_status_t status = BENTO_OK;

    (void)memset(&pipe, 0, sizeof(pipe));
    pipe.problem = "ยังไม่ตั้ง WiFi: ไม่ส่ง";
    if (WIFI_SSID[0] != '<') {
        status = connect_plain(screen, link, &pipe.problem);
    }
    if (status == BENTO_OK) {
        const bool linked = pipe.problem == NULL;

        show(screen->ui, screen->pipe_one,
             linked ? link->topic : pipe.problem,
             linked ? COL_OK : COL_WARN);
        if (linked) {
            show_link(screen, true);
        }
        status = beep(screen->ui, linked ? TUNE_START : TUNE_BAD);
    }
    if (status == BENTO_OK) {
        status = run_pipes(screen, link, &pipe);
        if (link->broker != 0U) {
            (void)bento_mqtt_close(
                link->broker); /* frees it, any state */
            link->broker = 0U;
            (void)printf("[MQTT] Disconnected\n");
        }
        show_link(screen, false);
    }
    return status;
}

int main(void)
{
    pipes_screen_t screen;
    pipe2_rows_t rows;
    link_t link;
    bento_status_t status;

    (void)memset(&screen, 0, sizeof(screen));
    (void)memset(&link, 0, sizeof(link));
    screen.ui = bento_runtime_get_ui();
    (void)snprintf(link.client_base, sizeof(link.client_base),
                   "bento-pipe-%s", TEAM);
    (void)snprintf(link.topic, sizeof(link.topic),
                   "bento-aiot/%s/core/pipes", TEAM);
    /* a board without ui.volume answers BENTO_E_UNSUPPORTED and is
     * skipped as the .py skips it; elsewhere it waits for no answer */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    read_config(&rows);
    status = build_screen(&screen, &rows);
    if (status == BENTO_OK) {
        status = run_program(&screen, &link);
    }
    if (link.broker != 0U) { /* a Stop while connecting */
        (void)bento_mqtt_close(link.broker); /* frees it, any state */
    }
    if (status == BENTO_OK) {
        show(screen.ui, screen.note,
             "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่",
             COL_WARN);
        status = beep(screen.ui, TUNE_GOOD);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
