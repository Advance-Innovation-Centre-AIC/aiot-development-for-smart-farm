/*
 * cp3_03_report_by_exception.c -- หลักการ 3.3: ส่งเมื่อเปลี่ยน + ส่งว่ายังอยู่
 * (Report by exception + heartbeat)
 *
 * วัด VR1 ทุก SAMPLE_MS แต่ส่งขึ้น MQTT เฉพาะตอนค่าต่างจาก "ใบที่ส่งไป
 * ล่าสุด" มากกว่า DEADBAND ค่าแรกส่งเสมอ และส่งใบ "ยังอยู่" (heartbeat)
 * ทุก HEARTBEAT_S วินาที ปลายทางจึงแยก "ค่านิ่ง" กับ "บอร์ดตาย" ได้
 * กราฟฟ้า = ค่าที่วัด เขียว = ค่าในใบที่ส่งล่าสุด จอนับว่าส่งกี่ใบ
 * จากที่วัดกี่ครั้ง และประหยัดกี่ % แก้ WIFI_SSID WIFI_PASS และ TEAM
 * ก่อนรัน ดูใบด้วย MQTT Explorer (bento-aiot/<TEAM>/core/#) พอร์ต 1883
 * ไม่เข้ารหัส ห้ามส่งของลับ
 *
 * VR1 is measured every SAMPLE_MS, but a message goes to MQTT only when
 * the value differs from the last message sent by more than DEADBAND.
 * The first value always goes, and a "still here" heartbeat goes every
 * HEARTBEAT_S seconds, so the receiver can tell a steady value from a
 * dead board. The chart shows the measured value in blue and the value
 * of the last message in green; the screen counts messages sent, values
 * measured and the percentage saved. Set WIFI_SSID, WIFI_PASS and TEAM
 * first. Port 1883 is not encrypted: send no secrets.
 *
 * Twin of cp3_03_report_by_exception.py
 * (aiot-development-for-smart-farm/core/cp3_03_report_by_exception.py).
 * Style: CLEAN_C.md.
 * Differs from the original:
 *   - ui.volume has no hasattr test: a board without it answers
 *     BENTO_E_UNSUPPORTED, which is skipped as the .py skips the line.
 *   - json.dumps is snprintf with the keys in the order the .py writes
 *     them; MicroPython's dict may print them in another order, which
 *     a JSON reader does not see.
 *   - mqtt.publish raises OSError only when the link is down; here the
 *     link is asked just before, and a refused publish is its False.
 *   - A broker try that fails is closed before the next one, so the
 *     three tries never hold more than one C client. Any client still
 *     open is closed when the program ends, also after the team or
 *     WiFi check stops it, where the .py had none open.
 *   - wifi.* and mqtt.* print console lines of their own; here the
 *     program prints the ones C can know (connecting, connected,
 *     disconnected), not the radio's result codes.
 *   - The Stop class is a flag: the reason is shown, the tune plays,
 *     and the program ends quietly with 0.
 *   - The tune table keeps only the tunes this program plays.
 *   - In the BENTO Emulator, Edge AI/MQTT from C is not available yet:
 *     the program says so and ends.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_mqtt.h"
#include "bento_pots.h"
#include "bento_runtime.h"
#include "bento_time.h"
#include "bento_wifi.h"

/* Set these three before running. TEAM is the group number the
 * teacher hands out, e.g. team05, and no two groups share it. */
/* WIFI_SSID lives in connect_wifi() */
/* WIFI_PASS lives in connect_wifi() */
static const char TEAM[] = "teamXX";

/* The spare broker is test.mosquitto.org, if the teacher says so. */
/* BROKER lives in try_broker() */
static const char UNIT[] = "%";

/* Settings a student may change. */
enum {
    DEADBAND = 3,     /* send when the value moved more than this */
    HEARTBEAT_S = 30, /* nothing changed: "still here" this often */
    SAMPLE_MS = 1000, /* measure this often */
    RUN_MS = 300000,  /* the program ends after five minutes */
    SPEAKER = 40,     /* speaker volume, 0-100 % */
    VOLUME = 25,      /* tone velocity, 0-127 */
    CHART_LO = 0,     /* the chart's range */
    CHART_HI = 100
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
    TEAM_CHARS = 6,      /* "team" and two digits */
    TEAM_PREFIX = 4,
    CLIENT_BYTES = 32, /* "bento-core-teamXX-ffff" and its NUL */
    TOPIC_BYTES = 48,  /* "bento-aiot/teamXX/core/report" */
    BODY_BYTES = 128,  /* the JSON message with any int32_t */
    TEXT_BYTES = 128   /* a line on screen with any int32_t */
};

/* Measuring. */
enum {
    MS_PER_S = 1000,
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
    CHART_CARD_W = 420,
    COUNT_CARD_X = 442,
    COUNT_CARD_W = 338,
    CARD_TEXT_X = 12,
    CARD_TEXT_Y = 6,
    CARD_RADIUS = 12,
    CHART_X = 22,
    CHART_Y = 76,
    CHART_W = 400,
    CHART_H = 252,
    CHART_POINTS = 400, /* as many points as pixels: a smooth line */
    COUNT_X = 456,
    SENT_Y = 80,
    SAVED_Y = 150,
    LAST_Y = 250,
    TOPIC_Y = 300,
    NOTE_Y = 352,
    FONT_TINY = 14,
    FONT_SMALL = 16,
    FONT_ROW = 20,
    FONT_TITLE = 24,
    FONT_BIG = 28,
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
    GAP_X = 456,
    GAP_Y = 80,
    GAP_W = 314 /* the line wraps inside its card */
};

typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
} rect_t;

/* The widgets the program changes after building the screen. */
typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t led;
    bento_ui_id_t link;
    bento_ui_id_t chart;
    uint8_t sent_series;
    bento_ui_id_t sent;
    bento_ui_id_t saved;
    bento_ui_id_t last;
    bento_ui_id_t note;
} report_screen_t;

/* The broker client and the names built from TEAM. */
typedef struct {
    uint32_t broker; /* 0 = no client */
    char client_base[CLIENT_BYTES];
    char client_id[CLIENT_BYTES];
    char topic[TOPIC_BYTES];
    bool no_mqtt; /* the BENTO Emulator: no MQTT from C yet */
} link_t;

/* What the reporting loop counts. */
typedef struct {
    int32_t sent;     /* messages that went out */
    int32_t measured; /* values measured: all of them would go */
    int32_t last;     /* the value in the last message sent */
    bool has_last;
    bool stopped;    /* stop() ended the run */
    uint32_t now_ms; /* when this round began */
    uint32_t start_ms;
    uint32_t sent_ms;
} counter_t;

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

/* VR1 as 0-100 %: the board has no pots.percent(). */
static bento_status_t read_value(int32_t *percent)
{
    uint16_t raw = 0U;
    const bento_status_t status = bento_pots_read(VR1, &raw);

    *percent = ((int32_t)raw * PERCENT) / ADC_FULL;
    return status;
}

/* Send this one? "change", "heartbeat" or NULL. Touches no network,
 * so it can be tested without WiFi. */
static const char *why_send(int32_t value, const counter_t *counter,
                            int32_t quiet_ms)
{
    const char *why = NULL;
    int32_t moved = 0;

    if (counter->has_last) {
        moved = value - counter->last;
        moved = (moved < 0) ? -moved : moved;
    }
    if (!counter->has_last || (moved > DEADBAND)) {
        why = "change";
    } else if (quiet_ms >= ((int32_t)HEARTBEAT_S * (int32_t)MS_PER_S)) {
        why = "heartbeat";
    } else {
        /* steady and not yet due: stay quiet */
    }
    return why;
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
        status = add_label(ui, title, at, COL_INFO, FONT_ROW, &unused);
    }
    return status;
}

/* A line chart with as many points as it is wide (at most 400), so
 * LVGL draws a smooth line without dots. */
static bento_status_t add_line_chart(report_screen_t *screen)
{
    bento_ui_widget_args_t args;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = CHART_X;
    args.y = CHART_Y;
    args.w = CHART_W;
    args.h = CHART_H;
    args.color = COL_INFO;
    args.min_val = CHART_LO;
    args.max_val = CHART_HI;
    status = bento_ui_chart_create(screen->ui, &args, &screen->chart);
    if (status == BENTO_OK) {
        /* w.prop() is a setter: a refusal was ignored before too */
        (void)bento_ui_set_prop(screen->ui, screen->chart,
                                (uint8_t)BENTO_UI_PROP_CHART_POINTS,
                                CHART_POINTS);
        status = bento_ui_chart_add_series(
            screen->ui, screen->chart, COL_OK, &screen->sent_series);
    }
    return status;
}

static bento_status_t add_header(report_screen_t *screen)
{
    bento_ui_widget_args_t args;
    bento_ui_id_t unused;
    const point_t title_at = {MARGIN_X, TITLE_Y};
    const point_t mqtt_at = {MQTT_X, TOP_Y};
    const point_t link_at = {LINK_X, TOP_Y};
    bento_status_t status =
        add_label(screen->ui, "ส่งเมื่อเปลี่ยน + ส่งว่ายังอยู่", title_at,
                  COL_TEXT, FONT_TITLE, &unused);

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
                           FONT_ROW, &screen->link);
    }
    return status;
}

static bento_status_t add_count_card(report_screen_t *screen,
                                     const char *topic)
{
    const rect_t box = {COUNT_CARD_X, CARD_Y, COUNT_CARD_W, CARD_H};
    const point_t sent_at = {COUNT_X, SENT_Y};
    const point_t saved_at = {COUNT_X, SAVED_Y};
    const point_t last_at = {COUNT_X, LAST_Y};
    const point_t topic_at = {COUNT_X, TOPIC_Y};
    bento_ui_id_t unused;
    bento_status_t status = add_card(screen->ui, box, "นับใบ");

    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", sent_at, COL_TEXT, FONT_ROW,
                           &screen->sent);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", saved_at, COL_OK, FONT_BIG,
                           &screen->saved);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", last_at, COL_TEXT,
                           FONT_SMALL, &screen->last);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, topic, topic_at, COL_DIM,
                           FONT_TINY, &unused);
    }
    return status;
}

static bento_status_t build_screen(report_screen_t *screen,
                                   const char *topic)
{
    const rect_t chart_box = {MARGIN_X, CARD_Y, CHART_CARD_W, CARD_H};
    const point_t note_at = {MARGIN_X, NOTE_Y};
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = sleep_ms((uint32_t)SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_header(screen);
    }
    if (status == BENTO_OK) {
        status =
            add_card(screen->ui, chart_box, "ฟ้า = วัด  เขียว = ใบที่ส่ง");
    }
    if (status == BENTO_OK) {
        status = add_line_chart(screen);
    }
    if (status == BENTO_OK) {
        status = add_count_card(screen, topic);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", note_at, COL_DIM, FONT_ROW,
                           &screen->note);
    }
    if (status == BENTO_OK) {
        present(screen->ui);
    }
    return status;
}

/* The MQTT light: on = linked to the broker, dim = offline. */
static void show_link(const report_screen_t *screen, bool online)
{
    /* the widget setters ignore a refusal, as modui.c does */
    (void)bento_ui_set_value(screen->ui, screen->led, online ? 1 : 0);
    (void)bento_ui_set_color(screen->ui, screen->link,
                             online ? COL_OK : COL_DIM);
    set_text(screen->ui, screen->link, online ? "เชื่อมต่อแล้ว" : "ออฟไลน์");
}

static void note(const report_screen_t *screen, const char *text,
                 uint32_t color)
{
    (void)bento_ui_set_color(screen->ui, screen->note, color);
    set_text(screen->ui, screen->note, text);
    present(screen->ui);
}

/* The normal way to end with a reason: SystemExit would restart the
 * board and may hang it. */
static bento_status_t stop_here(const report_screen_t *screen,
                                const char *reason)
{
    show_link(screen, false);
    note(screen, reason, COL_BAD);
    return beep(screen->ui, TUNE_BAD);
}

/* "team", two digits, and not team00. */
static bool team_is_set(void)
{
    const char *digits = &TEAM[TEAM_PREFIX];
    bool good = (strlen(TEAM) == (size_t)TEAM_CHARS) &&
                (strncmp(TEAM, "team", (size_t)TEAM_PREFIX) == 0);

    good = good && (digits[0] >= '0') && (digits[0] <= '9') &&
           (digits[1] >= '0') && (digits[1] <= '9');
    return good && (strcmp(TEAM, "team00") != 0);
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
    static const char wifi_ssid[] = "<ชื่อ Hotspot ของกลุ่ม>";
    bento_wifi_connect_params_t params;
    bento_status_t status;

    bento_wifi_connect_params_init(&params);
    params.ssid = wifi_ssid;
    params.ssid_len = (uint32_t)strlen(wifi_ssid);
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

static bento_status_t connect_broker(const report_screen_t *screen,
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
            note(screen, text, COL_WARN);
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
    bento_ui_widget_args_t args;
    bento_ui_id_t label;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.text = gap;
    args.text_len = (uint32_t)strlen(gap);
    args.x = GAP_X;
    args.y = GAP_Y;
    args.w = GAP_W;
    args.color = COL_WARN;
    args.font_px = FONT_ROW;
    status = bento_ui_label_create(ui, &args, &label);
    (void)printf("%s / MQTT from C is not in the BENTO Emulator yet: "
                 "run it on a Dev Kit or Eva Kit.\n",
                 gap);
    if (status == BENTO_OK) {
        present(ui);
    }
    return status;
}

/* WiFi, then an address, then the broker. *problem says where it
 * broke, or stays NULL. */
static bento_status_t connect_farm(const report_screen_t *screen,
                                   link_t *link, const char **problem)
{
    bool passed = false;
    bento_status_t status;

    /* the label is up before the line that blocks */
    note(screen, "ต่อ WiFi...", COL_WARN);
    status = connect_wifi(&passed);
    if ((status == BENTO_OK) && !passed) {
        *problem = "ต่อ WiFi ไม่ได้";
    }
    if ((status == BENTO_OK) && passed) {
        status = connect_broker(screen, link, &passed);
        *problem = passed ? NULL : "broker ไม่ตอบ";
    }
    /* then the .py's own way out: the broker did not answer */
    if ((status == BENTO_OK) && link->no_mqtt) {
        status = show_emulator_gap(screen->ui);
    }
    return status;
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

/* One message: {"id", "n", "up_s", "value", "unit", "why"}. */
static bento_status_t send_report(const report_screen_t *screen,
                                  const link_t *link,
                                  counter_t *counter, int32_t value,
                                  const char *why)
{
    char body[BODY_BYTES];
    char text[TEXT_BYTES];
    const uint32_t now = counter->now_ms;
    const int32_t up_s =
        bento_time_ticks_diff(now, counter->start_ms) / MS_PER_S;
    bento_status_t status = BENTO_OK;

    (void)snprintf(body, sizeof(body),
                   "{\"id\": \"%s\", \"n\": %" PRId32
                   ", \"up_s\": %" PRId32 ", \"value\": %" PRId32
                   ", \"unit\": \"%s\", \"why\": \"%s\"}",
                   TEAM, counter->sent + 1, up_s, value, UNIT, why);
    if (!link_is_up(link->broker)) { /* publish raises: link down */
        counter->stopped = true;
        status = stop_here(screen, "สายหลุด");
    } else if (bento_mqtt_publish(
                   link->broker, link->topic,
                   (uint32_t)strlen(link->topic), (const uint8_t *)body,
                   (uint32_t)strlen(body), 0U, 0U) == BENTO_OK) {
        counter->sent++;
        counter->last = value;
        counter->has_last = true;
        counter->sent_ms = now;
        (void)snprintf(text, sizeof(text), "#%" PRId32 " %s = %" PRId32,
                       counter->sent, why, value);
        set_text(screen->ui, screen->last, text);
        (void)printf("ส่ง: %s\n", body);
    } else { /* the broker took it not: the next round tries again */
        (void)snprintf(text, sizeof(text), "broker ไม่รับใบ %" PRId32,
                       counter->sent + 1);
        note(screen, text, COL_WARN);
    }
    return status;
}

/* The chart takes whole numbers only; the green line is the value of
 * the last message. */
static void show_counts(const report_screen_t *screen,
                        const counter_t *counter, int32_t value)
{
    char text[TEXT_BYTES];

    (void)bento_ui_chart_set_next(screen->ui, screen->chart, 0U, value);
    if (counter->has_last) {
        (void)bento_ui_chart_set_next(screen->ui, screen->chart,
                                      screen->sent_series,
                                      counter->last);
    }
    (void)snprintf(text, sizeof(text),
                   "ส่ง %" PRId32 " ใบ / วัด %" PRId32 " ครั้ง",
                   counter->sent, counter->measured);
    set_text(screen->ui, screen->sent, text);
    (void)snprintf(text, sizeof(text), "ประหยัด %" PRId32 " %%",
                   PERCENT -
                       ((counter->sent * PERCENT) / counter->measured));
    set_text(screen->ui, screen->saved, text);
    present(screen->ui);
}

/* Measure, decide, send, show. */
static bento_status_t run_round(const report_screen_t *screen,
                                const link_t *link, counter_t *counter)
{
    int32_t value = 0;
    const char *why = NULL;
    bento_status_t status = BENTO_OK;

    if (!link_is_up(link->broker)) { /* dropped between rounds */
        counter->stopped = true;
        status = stop_here(screen, "สายหลุด");
    }
    if (!counter->stopped) {
        counter->now_ms = bento_time_ticks_ms();
        status = read_value(&value);
        counter->measured++;
        why = why_send(
            value, counter,
            bento_time_ticks_diff(counter->now_ms, counter->sent_ms));
    }
    if ((status == BENTO_OK) && (why != NULL)) {
        status = send_report(screen, link, counter, value, why);
    }
    if ((status == BENTO_OK) && !counter->stopped) {
        show_counts(screen, counter, value);
        status = sleep_ms((uint32_t)SAMPLE_MS);
    }
    return status;
}

static bento_status_t run_reports(const report_screen_t *screen,
                                  const link_t *link,
                                  counter_t *counter)
{
    char text[TEXT_BYTES];
    bento_status_t status;

    show_link(screen, true);
    (void)snprintf(text, sizeof(text), "ต่างเกิน %d %s หรือครบ %d วิ = ส่ง",
                   DEADBAND, UNIT, HEARTBEAT_S);
    note(screen, text, COL_OK);
    status = beep(screen->ui, TUNE_START);
    counter->start_ms = bento_time_ticks_ms();
    counter->sent_ms = counter->start_ms;
    while ((status == BENTO_OK) && !counter->stopped &&
           (bento_time_ticks_diff(bento_time_ticks_ms(),
                                  counter->start_ms) < RUN_MS)) {
        status = run_round(screen, link, counter);
    }
    if ((status == BENTO_OK) && !counter->stopped) {
        show_link(screen, false);
    }
    return status;
}

/* The checks, the link, then the reports. *done: the run reached its
 * end, so the closing line follows. */
static bento_status_t run_program(const report_screen_t *screen,
                                  link_t *link, bool *done)
{
    counter_t counter;
    const char *problem = NULL;
    bento_status_t status = BENTO_OK;

    (void)memset(&counter, 0, sizeof(counter));
    *done = false;
    if (!team_is_set()) {
        problem = "แก้ TEAM เป็นเลขกลุ่มก่อน";
    } else {
        status = connect_farm(screen, link, &problem);
    }
    if ((status == BENTO_OK) && (problem != NULL)) {
        status = stop_here(screen, problem);
    } else if (status == BENTO_OK) {
        status = run_reports(screen, link, &counter);
        *done = !counter.stopped;
    } else {
        /* Stop or a failure beyond False ends the run */
    }
    return status;
}

int main(void)
{
    report_screen_t screen;
    link_t link;
    bool done = false;
    bento_status_t status;

    (void)memset(&screen, 0, sizeof(screen));
    (void)memset(&link, 0, sizeof(link));
    screen.ui = bento_runtime_get_ui();
    (void)snprintf(link.client_base, sizeof(link.client_base),
                   "bento-core-%s", TEAM);
    (void)snprintf(link.topic, sizeof(link.topic),
                   "bento-aiot/%s/core/report", TEAM);
    /* a board without ui.volume answers BENTO_E_UNSUPPORTED and is
     * skipped as the .py skips it; elsewhere it waits for no answer */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    status = build_screen(&screen, link.topic);
    if (status == BENTO_OK) {
        status = run_program(&screen, &link, &done);
    }
    if (link.broker != 0U) { /* a stop on the way still unlinks */
        (void)bento_mqtt_close(link.broker); /* frees it, any state */
        (void)printf("[MQTT] Disconnected\n");
    }
    if ((status == BENTO_OK) && done) {
        note(&screen, "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่",
             COL_WARN);
        status = beep(screen.ui, TUNE_GOOD);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
