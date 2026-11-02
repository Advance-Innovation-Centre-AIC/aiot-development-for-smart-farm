/*
 * sf3_06_ai_to_mqtt.c -- ผล AI บนบอร์ดขึ้น MQTT: ให้แอปของกลุ่มรู้ทันที
 * ว่า AI เห็นอะไร
 *
 * รันโมเดล AI บนบอร์ด (หาด้วยชื่อตามลำดับ MODEL_KEYS: AnomalousVibration
 * จาก Edge AI Store ก่อน ไม่มีหรือเลือกไม่ได้จึงใช้ Motion ในตัว) แล้วส่ง
 * "ผลที่ AI สรุปแล้ว" ขึ้น bento-aiot/<TEAM>/ai เมื่อป้ายเปลี่ยน และส่งซ้ำ
 * ทุก 2 วินาทีเป็นสัญญาณชีพ ห่างกันอย่างน้อย 200 ms ป้ายอันตรายติดไฟแดง
 * และดังเสียง สายหลุด = ทำงานต่อแบบออฟไลน์แล้วต่อใหม่เอง รอ 5 -> 10 ->
 * 20 -> 30 วินาที ลำดับข้อความ n นับต่อระหว่างหลุด แก้ WIFI_SSID
 * WIFI_PASS TEAM ก่อนรัน ยังไม่แก้ = ออฟไลน์ (พิมพ์ผลลง Console แทน)
 *
 * Runs an Edge AI model on the board (found by name in MODEL_KEYS
 * order: AnomalousVibration from the Edge AI Store first, the
 * built-in Motion when it is missing or cannot be selected) and
 * sends the model's verdict to bento-aiot/<TEAM>/ai when the label
 * changes, and again every 2 seconds as a heartbeat, at least
 * 200 ms apart. A danger label lights the red LED and beeps. When
 * the public broker drops, the board keeps working offline and
 * reconnects by itself after 5, 10, 20, then 30 seconds; n keeps
 * counting so the app sees the gap. Edit WIFI_SSID, WIFI_PASS and
 * TEAM before running; until then it runs offline and prints each
 * message to the Console.
 *
 * Twin of sf3_06_ai_to_mqtt.py
 * (aiot-development-for-smart-farm/s3/sf3_06_ai_to_mqtt.py).
 * Style: CLEAN_C.md.
 * Differs from the original:
 *   - json.dumps writes the keys in the slot order of the board's
 *     MicroPython dict (py/map.c, 1-byte qstr hash on the PSoC Edge,
 *     2-byte on the reTerminal); the text is built with snprintf in
 *     that order, strings escaped as mp_str_print_json does.
 *   - edge_ai.models() is read row by row for each name in
 *     MODEL_KEYS; a row the board refuses is passed over.
 *     edge_ai.result() with no verdict yet is the original's None.
 *   - wifi.connect(), mqtt.connect(), edge_ai.select() and stop()
 *     wait inside the binding; here they are started and polled, so
 *     Stop ends a wait. Stop still runs the finally (the model is
 *     stopped, the broker closed).
 *   - hasattr(ui, "volume"): the volume is always set, and a build
 *     without sound refuses it, which is ignored.
 *   - try/except OSError: each call's status is checked and the same
 *     fallback is taken; mqtt.disconnect() closes the broker handle,
 *     after which the link counts as not connected.
 *   - The widget dict and the loop's variables are structs kept in
 *     main() and passed by pointer; the JSON line is a file-scope
 *     buffer (CLEAN_C.md C9).
 *   - In the BENTO Emulator, Edge AI from C is not available yet: the
 *     program says so and ends as the original does with no model.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_edgeai.h"
#include "bento_fmt.h"
#include "bento_gpio.h"
#include "bento_mqtt.h"
#include "bento_runtime.h"
#include "bento_time.h"
#include "bento_wifi.h"

/* Edit these before running. */
static const char WIFI_SSID[] =
    "<ชื่อ Hotspot ของกลุ่ม>"; /* short, no spaces */
/* WIFI_PASS is wifi_pass in join_wifi() and BROKER is broker in
 * open_broker(): each is read there only. */
static const char TEAM[] = "teamXX"; /* e.g. team05, as in the app */

/* Tried in order: the Store model first, then the built-in one. */
static const char *const MODEL_KEYS[] = {"AnomalousVibration",
                                         "Motion"};

/* DANGER, the labels that count as danger, is danger_labels in
 * is_danger(). */

/* Settings: change them and run again. */
enum {
    HEARTBEAT_MS = 2000, /* sent again this often, label or not */
    GAP_MS = 200,        /* never more often (a shared broker) */
    TICK_MS = 100,       /* ask the AI this often */
    RUN_MS = 300000,
    KEEPALIVE_S = 30,     /* shorter = a dropped line is seen sooner */
    RETRY_MS = 5000,      /* first wait before reconnecting ... */
    RETRY_MAX_MS = 30000, /* ... doubled up to this */
    VOLUME = 25,          /* 0-127 (about 20 %) */
    SPEAKER = 40          /* speaker 0-100 % (2.4.2+) */
};

/* Network, the AI core and timing. */
enum {
    MQTT_PORT = 1883,
    CONNECT_TRIES = 3,
    NET_POLL_MS = 50,
    OP_POLL_MS = 20,
    TEAM_CHARS = 6,
    TEAM_PREFIX = 4,
    ID_SUFFIX_MASK = 0xFFFF,
    CLIENT_ID_BYTES = 32,
    TOPIC_BYTES = 64,
    TEXT_BYTES = 128,
    JSON_LINE_BYTES = 1024,
    PERCENT = 100,
    SECOND_MS = 1000,
    BEEP_MS = 120,
    SCREEN_SETTLE_MS = 200,
    UNICODE_ESCAPE_BYTES = 7 /* \u00XX and its NUL */
};

/* MIDI notes of the danger beep. */
enum {
    NOTE_E5 = 76,
    NOTE_C6 = 84
};

/* Layout on the 800 x 400 canvas. */
enum {
    TITLE_X = 12,
    TITLE_Y = 6,
    TITLE_PX = 24,
    MODEL_Y = 44,
    MODEL_PX = 20,
    BIG_PX = 28,
    THAI_PX = 20, /* Thai is 20 px or larger on the board */
    TEXT_PX = 16, /* Latin only */
    SMALL_PX = 14,
    ARC_X = 12,
    ARC_Y = 84,
    ARC_SIZE = 180,
    CONF_X = 70,
    CONF_Y = 162,
    SEES_X = 220,
    SEES_Y = 90,
    LABEL_Y = 116,
    LED_Y = 170,
    LED_SIZE = 36,
    LED_NOTE_X = 268,
    LED_NOTE_Y = 178,
    MQTT_X = 560,
    MQTT_LED_Y = 116,
    MQTT_TEXT_X = 606,
    MQTT_TEXT_Y = 124,
    SENT_Y = 230,
    JSON_X = 12,
    JSON_Y = 300,
    GAP_X = 12, /* the Emulator line, between the ring and the JSON */
    GAP_Y = 268,
    NOTE_X = 12,
    NOTE_Y = 352
};

/* Colours, 0xRRGGBB. */
enum {
    COLOR_TEXT = 0xE8EAED,
    COLOR_DIM = 0x9AA3AF,
    COLOR_OK = 0x30A46C,
    COLOR_WARN = 0xF5A623,
    COLOR_BAD = 0xE5484D,
    COLOR_INFO = 0x4A9EFF
};

/* go_online(): online, not yet (try again later), or settings that
 * a retry cannot fix (the original's None). */
typedef enum {
    LINK_ONLINE = 0,
    LINK_NOT_YET,
    LINK_UNSET
} link_result_t;

typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
} rect_t;

/* The widgets the program changes after it has drawn them. */
typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t model;
    bento_ui_id_t arc;
    bento_ui_id_t confidence;
    bento_ui_id_t label;
    bento_ui_id_t led;
    bento_ui_id_t mqtt;
    bento_ui_id_t mqtt_text;
    bento_ui_id_t sent;
    bento_ui_id_t json;
    bento_ui_id_t note;
} link_screen_t;

/* The broker link and the reconnect back-off. */
typedef struct {
    bool has_broker;
    uint32_t broker;
    bool online;
    bool can_retry;
    int32_t drops;
    uint32_t retry_ms;
    int32_t wait_ms;
} link_t;

/* The newest answer and what was sent; has_* false is None. */
typedef struct {
    char name[BENTO_EDGEAI_NAME_LEN];
    bool has_sequence;
    uint32_t sequence;
    bool has_label;
    char label[BENTO_EDGEAI_LABEL_LEN];
    int32_t confidence;
    bool has_sent_label;
    char sent_label[BENTO_EDGEAI_LABEL_LEN];
    int32_t sent_count;
    int32_t sent_ok;
    uint32_t sent_ms;
} watch_t;

/* The JSON line being built: file scope, zeroed by the loader on
 * every load and set again at the start of main() (CLEAN_C.md C9,
 * deviation BENTO-MISRA-E03/E04). */
typedef struct {
    char text[JSON_LINE_BYTES];
    uint32_t used;
} json_line_t;

extern json_line_t g_json_line;
json_line_t g_json_line;

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

static bento_status_t add_led(bento_ui_t *ui, int32_t x, int32_t y,
                              uint32_t color, bento_ui_id_t *out_id)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.x = px(x);
    args.y = px(y);
    args.w = LED_SIZE;
    args.h = LED_SIZE;
    args.color = color;
    args.font_px = 0;
    return bento_ui_led_create(ui, &args, out_id);
}

/* Two soft MIDI notes (ui.sfx cannot be made quieter), 120 ms
 * apart. ui.tone() is sent and forgotten, so its status is not
 * read. */
static bento_status_t beep(bento_ui_t *ui, uint8_t first,
                           uint8_t second)
{
    bento_status_t status;

    (void)bento_ui_sfx_tone(ui, first, (uint8_t)BENTO_UI_WAVE_SINE,
                            (uint8_t)VOLUME, (uint16_t)BEEP_MS);
    status = pause_ms(BEEP_MS);
    if (status == BENTO_OK) {
        (void)bento_ui_sfx_tone(ui, second, (uint8_t)BENTO_UI_WAVE_SINE,
                                (uint8_t)VOLUME, (uint16_t)BEEP_MS);
        status = pause_ms(BEEP_MS);
    }
    return status;
}

/* int(pct / 100 * 100), in the board's float as MicroPython does
 * it: it may be one less than pct. */
static int32_t as_percent(uint8_t pct)
{
    const float score = (float)pct / (float)PERCENT;
    const bento_real_t percent =
        (bento_real_t)score * (bento_real_t)PERCENT;

    return (int32_t)percent;
}

/* Waits for a started select or stop; Stop cancels it. */
static bento_status_t finish_op(void)
{
    bento_status_t status = BENTO_E_IN_PROGRESS;

    while (status == BENTO_E_IN_PROGRESS) {
        status = pause_ms(OP_POLL_MS);
        if (status == BENTO_OK) {
            status = bento_edgeai_op_poll(NULL);
        } else {
            (void)bento_edgeai_op_cancel(); /* Stop wins */
        }
    }
    return status;
}

/* edge_ai.stop(): stopped already is fine, and an OSError is
 * passed. */
static bento_status_t stop_ai(void)
{
    bento_status_t status = bento_edgeai_stop_start();

    if (status == BENTO_OK) {
        status = finish_op();
    }
    if (status != BENTO_E_CANCELLED) {
        status = BENTO_OK;
    }
    return status;
}

/* key.lower() in name.lower(), for ASCII letters (lower() leaves
 * every other character as it is). */
static char lower(char letter)
{
    static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char lower_case[] = "abcdefghijklmnopqrstuvwxyz";
    char result = letter;
    uint32_t i;

    for (i = 0U; i < (uint32_t)(sizeof(upper) - 1U); i++) {
        if (upper[i] == letter) {
            result = lower_case[i];
        }
    }
    return result;
}

static bool starts_with_folded(const char *text, const char *key)
{
    size_t i = 0U;

    while ((key[i] != '\0') && (lower(text[i]) == lower(key[i]))) {
        i++;
    }
    return key[i] == '\0';
}

static bool contains_folded(const char *name, const char *key)
{
    bool found = false;
    size_t at;

    for (at = 0U; (name[at] != '\0') && !found; at++) {
        found = starts_with_folded(&name[at], key);
    }
    return found || (key[0] == '\0');
}

/* edge_ai.select(i): *selected false is the original's OSError,
 * after which the next model is tried. */
static bento_status_t select_model(uint32_t index, bool *selected)
{
    bento_status_t status = bento_edgeai_select_start(index);

    if (status == BENTO_OK) {
        status = finish_op(); /* select() loads, then runs */
    }
    *selected = status == BENTO_OK;
    return (status == BENTO_E_CANCELLED) ? status : BENTO_OK;
}

/* Row i: selected when its name holds the key and it starts. */
static bento_status_t try_row(const char *key, uint32_t row,
                              watch_t *watch, bool *selected)
{
    bento_edgeai_model_info_t info;
    bento_status_t status = BENTO_OK;

    bento_edgeai_model_info_init(&info);
    if ((bento_edgeai_model_info(row, &info) == BENTO_OK) &&
        contains_folded(info.name, key)) {
        status = select_model(row, selected);
    }
    if (*selected) {
        (void)snprintf(watch->name, sizeof(watch->name), "%s",
                       info.name);
    }
    return status;
}

/* Tries every model whose name holds the key, in board order. */
static bento_status_t try_key(const char *key, int32_t rows,
                              watch_t *watch, bool *selected)
{
    int32_t i;
    bento_status_t status = BENTO_OK;

    for (i = 0; (i < rows) && !*selected && (status == BENTO_OK); i++) {
        status = try_row(key, (uint32_t)i, watch, selected);
    }
    return status;
}

/* The BENTO Emulator has no Edge AI from C yet: one line on the
 * screen and on the Console. */
static bento_status_t show_emulator_gap(bento_ui_t *ui)
{
    static const char gap_text[] =
        "Edge AI จาก C ยังไม่มีใน BENTO "
        "Emulator ให้รันบน Dev Kit หรือ Eva Kit";
    const rect_t place = {GAP_X, GAP_Y, 0, 0};
    bento_ui_id_t unused;

    (void)printf(
        "%s / Edge AI from C is not in the BENTO Emulator yet: "
        "run it on a Dev Kit or Eva Kit.\n",
        gap_text);
    return add_label(ui, gap_text, &place, COLOR_WARN, THAI_PX,
                     &unused);
}

/* The first model in MODEL_KEYS order that can be selected. The
 * AI core not answering is the original's empty list. */
static bento_status_t pick_model(const link_screen_t *screen,
                                 watch_t *watch, bool *selected)
{
    const uint32_t keys =
        (uint32_t)(sizeof(MODEL_KEYS) / sizeof(MODEL_KEYS[0]));
    int32_t rows = 0;
    uint32_t key;
    const bento_status_t count_status = bento_edgeai_count(&rows);
    bento_status_t status = BENTO_OK;

    *selected = false;
    if (count_status == BENTO_E_UNSUPPORTED) {
        status = show_emulator_gap(screen->ui);
    }
    if (count_status != BENTO_OK) {
        rows = 0;
    }
    for (key = 0U; (key < keys) && !*selected && (status == BENTO_OK);
         key++) {
        status = try_key(MODEL_KEYS[key], rows, watch, selected);
    }
    return status;
}

/* Labels that count as danger (red LED and a beep); as sf3_04,
 * without the radar's Push. */
static bool is_danger(const char *label)
{
    static const char *const danger_labels[] = {
        "anomaly",  "shaking",  "sirens",         "cough",
        "baby_cry", "chainsaw", "crackling_fire", "alarm"};
    const uint32_t count =
        (uint32_t)(sizeof(danger_labels) / sizeof(danger_labels[0]));
    bool danger = false;
    uint32_t i;

    for (i = 0U; (i < count) && !danger; i++) {
        danger = strcmp(label, danger_labels[i]) == 0;
    }
    return danger;
}

/* Send when far enough from the last one and (the label differs
 * from the one sent last, or the heartbeat is due). A label that
 * changes inside the 200 ms gap waits and goes out when it ends. */
static bool should_send(bool changed, int32_t since_ms)
{
    return (since_ms >= GAP_MS) &&
           (changed || (since_ms >= HEARTBEAT_MS));
}

/* team01-team99: "team", two digits, not team00 (another team's
 * name would send over that team's data). */
static bool team_is_set(void)
{
    const char *digits = &TEAM[TEAM_PREFIX];
    bool good = (strlen(TEAM) == (size_t)TEAM_CHARS) &&
                (strncmp(TEAM, "team", (size_t)TEAM_PREFIX) == 0);

    good = good && (digits[0] >= '0') && (digits[0] <= '9') &&
           (digits[1] >= '0') && (digits[1] <= '9');
    return good && (strcmp(TEAM, "team00") != 0);
}

/* The Hotspot name is still the example (or empty): never sent to
 * wifi.connect(). */
static bool ssid_is_example(void)
{
    return (WIFI_SSID[0] == '<') || (WIFI_SSID[0] == '\0');
}

static void make_topic(char *topic, size_t cap)
{
    (void)snprintf(topic, cap, "bento-aiot/%s/ai", TEAM);
}

/* The bottom note. */
static void note(const link_screen_t *screen, const char *text,
                 uint32_t color)
{
    (void)bento_ui_set_color(screen->ui, screen->note, color);
    set_text(screen->ui, screen->note, text);
}

static bool wifi_connected(void)
{
    bool connected = false;

    if (bento_wifi_is_connected(&connected) != BENTO_OK) {
        connected = false;
    }
    return connected;
}

/* Waits for a started WiFi connect; Stop cancels it. */
static bento_status_t wait_wifi(void)
{
    bento_status_t status = BENTO_E_IN_PROGRESS;

    while (status == BENTO_E_IN_PROGRESS) {
        status = pause_ms(NET_POLL_MS);
        if (status == BENTO_OK) {
            status = bento_wifi_connect_poll();
        } else {
            (void)bento_wifi_connect_cancel(); /* always BENTO_OK */
        }
    }
    return status;
}

static bento_status_t join_wifi(void)
{
    /* 8+ characters; share no file that holds it */
    static const char wifi_pass[] = "<รหัส Hotspot ของกลุ่ม>";
    bento_wifi_connect_params_t params;

    bento_wifi_connect_params_init(&params);
    params.ssid = WIFI_SSID;
    params.ssid_len = (uint32_t)strlen(WIFI_SSID);
    params.pass = wifi_pass;
    params.pass_len = (uint32_t)strlen(wifi_pass);
    params.flags = BENTO_WIFI_F_SAVE_CREDS; /* as the binding does */
    return (bento_wifi_connect_start(&params) == BENTO_OK)
               ? wait_wifi()
               : BENTO_E_FAILED;
}

/* WiFi up with an address? Not connected yet: connect, but only
 * once the Hotspot name is set (else wait for a saved network). */
static bento_status_t wifi_up(bool *up)
{
    uint32_t address = 0U;
    bento_status_t step = BENTO_OK;

    *up = wifi_connected();
    if (!*up && !ssid_is_example()) {
        step = join_wifi();
        *up = step == BENTO_OK;
    }
    if (*up) {
        *up =
            (bento_wifi_ip(BENTO_WIFI_IF_ANY, &address) == BENTO_OK) &&
            (address != 0U);
    }
    return (step == BENTO_E_CANCELLED) ? step : BENTO_OK;
}

/* mqtt.disconnect(); a failure is passed, as the original does. */
static void disconnect(link_t *link)
{
    if (link->has_broker) {
        (void)bento_mqtt_close(link->broker); /* the handle is gone */
    }
    link->has_broker = false;
}

/* One mqtt.connect() with a fresh client id from the board's clock,
 * so an old session with the same id is not in the way. */
static bento_status_t open_broker(link_t *link)
{
    static const char broker[] = "broker.hivemq.com";
    char client_id[CLIENT_ID_BYTES];
    bento_mqtt_config_t config;
    bento_status_t status;
    bool waiting;

    disconnect(link); /* the module keeps one client: a new one
                       * replaces the old */
    (void)snprintf(client_id, sizeof(client_id),
                   "bento-ai-%s-%04" PRIx32, TEAM,
                   bento_time_ticks_ms() & (uint32_t)ID_SUFFIX_MASK);
    bento_mqtt_config_init(&config);
    config.host = broker;
    config.host_len = (uint32_t)strlen(broker);
    config.port = (uint16_t)MQTT_PORT;
    config.keepalive_s = (uint16_t)KEEPALIVE_S;
    config.client_id = client_id;
    config.client_id_len = (uint32_t)strlen(client_id);
    status = bento_mqtt_open_start(&config, &link->broker);
    waiting = status == BENTO_OK;
    while (waiting) {
        status = pause_ms(NET_POLL_MS);
        if (status == BENTO_OK) {
            status = bento_mqtt_open_poll(link->broker);
        }
        waiting = status == BENTO_E_IN_PROGRESS;
    }
    link->has_broker = status == BENTO_OK;
    if (!link->has_broker) {
        (void)bento_mqtt_close(link->broker); /* nothing to undo */
    }
    return status;
}

/* A public broker sometimes does not answer: up to `tries`
 * attempts, each with a new client id. */
static bento_status_t connect_broker(const link_screen_t *screen,
                                     link_t *link, int32_t tries)
{
    char text[TEXT_BYTES];
    int32_t attempt;
    bento_status_t status = BENTO_E_FAILED;

    for (attempt = 1; (attempt <= tries) && (status != BENTO_OK) &&
                      (status != BENTO_E_CANCELLED);
         attempt++) {
        if (attempt > 1) {
            (void)snprintf(text, sizeof(text),
                           "ลองต่อ broker ใหม่ %" PRId32 "/%" PRId32,
                           attempt, tries);
            note(screen, text, COLOR_WARN);
            drain_events(screen->ui);
        }
        status = open_broker(link);
    }
    return status;
}

/* The settings a retry cannot fix: TEAM still teamXX, or no Hotspot
 * name and no WiFi. */
static bool settings_missing(const link_screen_t *screen)
{
    bool missing = true;

    if (!team_is_set()) {
        note(screen, "แก้ TEAM ก่อน (team01-team99): ออฟไลน์", COLOR_WARN);
    } else if (ssid_is_example() && !wifi_connected()) {
        note(screen, "ตั้งชื่อ Hotspot ก่อน: ออฟไลน์", COLOR_WARN);
    } else {
        missing = false;
    }
    return missing;
}

/* WiFi, then the broker; the note says which step failed. */
static bento_status_t go_online(const link_screen_t *screen,
                                link_t *link, link_result_t *result)
{
    char text[TEXT_BYTES];
    char topic[TOPIC_BYTES];
    const char *step = "WiFi";
    bool up = false;
    bento_status_t status = BENTO_OK;

    *result = LINK_UNSET;
    if (!settings_missing(screen)) {
        *result = LINK_NOT_YET;
        note(screen, "ต่อ WiFi...", COLOR_WARN);
        drain_events(screen->ui); /* the note shows before the wait */
        status = wifi_up(&up);
        if ((status == BENTO_OK) && up) {
            step = "broker";
            status = connect_broker(screen, link, CONNECT_TRIES);
            *result = (status == BENTO_OK) ? LINK_ONLINE : LINK_NOT_YET;
        }
        if (*result == LINK_ONLINE) {
            make_topic(topic, sizeof(topic));
            (void)snprintf(text, sizeof(text), "ออนไลน์ %s", topic);
            note(screen, text, COLOR_OK);
        } else {
            (void)snprintf(text, sizeof(text),
                           "ต่อ %s ไม่ได้: จะลองใหม่เอง (ทำงานต่อ)", step);
            note(screen, text, COLOR_WARN);
        }
    }
    return (status == BENTO_E_CANCELLED) ? status : BENTO_OK;
}

/* After a drop: one try per round, so the screen never hangs long. */
static bento_status_t reconnect(const link_screen_t *screen,
                                link_t *link, bool *online)
{
    bool up = false;
    bento_status_t status;

    note(screen, "กำลังต่อใหม่...", COLOR_WARN);
    drain_events(screen->ui);
    status = wifi_up(&up);
    *online = false;
    if ((status == BENTO_OK) && up) {
        status = connect_broker(screen, link, 1);
        *online = status == BENTO_OK;
    }
    return (status == BENTO_E_CANCELLED) ? status : BENTO_OK;
}

static bool broker_connected(const link_t *link)
{
    uint32_t state = BENTO_MQTT_STATE_CLOSED;

    return link->has_broker &&
           (bento_mqtt_state(link->broker, &state) == BENTO_OK) &&
           (state == BENTO_MQTT_STATE_CONNECTED);
}

/* Appends text to the JSON line; a full line is cut short. */
static void json_raw(json_line_t *line, const char *text)
{
    const uint32_t room = (uint32_t)sizeof(line->text) - line->used;
    const int written =
        snprintf(&line->text[line->used], room, "%s", text);

    if ((written > 0) && ((uint32_t)written < room)) {
        line->used += (uint32_t)written;
    }
}

/* A str as json.dumps writes it: in quotes, " and \ escaped,
 * control bytes as \n, \r, \t or \u00XX (mp_str_print_json). */
static void json_string(json_line_t *line, const char *text)
{
    char escaped[UNICODE_ESCAPE_BYTES];
    size_t i;

    json_raw(line, "\"");
    for (i = 0U; text[i] != '\0'; i++) {
        const uint8_t byte = (uint8_t)text[i];

        if ((text[i] == '"') || (text[i] == '\\')) {
            (void)snprintf(escaped, sizeof(escaped), "\\%c", text[i]);
        } else if (byte >= (uint8_t)' ') {
            (void)snprintf(escaped, sizeof(escaped), "%c", text[i]);
        } else if (text[i] == '\n') {
            (void)snprintf(escaped, sizeof(escaped), "\\n");
        } else if (text[i] == '\r') {
            (void)snprintf(escaped, sizeof(escaped), "\\r");
        } else if (text[i] == '\t') {
            (void)snprintf(escaped, sizeof(escaped), "\\t");
        } else {
            (void)snprintf(escaped, sizeof(escaped), "\\u%04x",
                           (unsigned int)byte);
        }
        json_raw(line, escaped);
    }
    json_raw(line, "\"");
}

static void json_int(json_line_t *line, int32_t value)
{
    char text[TEXT_BYTES];

    (void)snprintf(text, sizeof(text), "%" PRId32, value);
    json_raw(line, text);
}

/* Is this the reTerminal? The dict order of json.dumps depends on
 * it; asked by the name the board gives, so one Emulator build
 * serves both board models. */
static bool on_reterminal(void)
{
    bento_gpio_board_info_t info;
#if defined(BENTO_TARGET_RETERMINAL)
    bool answer = true;
#else
    bool answer = false;
#endif

    bento_gpio_board_info_init(&info);
    if ((bento_gpio_board_info(&info) == BENTO_OK) &&
        (info.name != NULL)) {
        answer = (strcmp(info.name, "Seeed reTerminal") == 0);
    }
    return answer;
}

/* json.dumps({"id", "n", "model", "label", "conf"}) in the slot
 * order of the board's dict: id conf label n model on the PSoC
 * Edge, model id conf label n on the reTerminal. */
static void write_json(const watch_t *watch)
{
    json_line_t *line = &g_json_line;
    const bool reterminal = on_reterminal();

    line->used = 0U;
    line->text[0] = '\0';
    json_raw(line, "{");
    if (reterminal) {
        json_raw(line, "\"model\": ");
        json_string(line, watch->name);
        json_raw(line, ", ");
    }
    json_raw(line, "\"id\": ");
    json_string(line, TEAM);
    json_raw(line, ", \"conf\": ");
    json_int(line, watch->confidence);
    json_raw(line, ", \"label\": ");
    json_string(line, watch->label);
    json_raw(line, ", \"n\": ");
    json_int(line, watch->sent_count);
    if (!reterminal) {
        json_raw(line, ", \"model\": ");
        json_string(line, watch->name);
    }
    json_raw(line, "}");
}

/* Prints the line; true when it reached the broker (QoS 0).
 * Offline or a broken line only prints it. */
static bool send(const link_t *link, const watch_t *watch)
{
    char topic[TOPIC_BYTES];
    bool sent = false;

    write_json(watch);
    (void)printf("%s\n", g_json_line.text);
    if (link->online && link->has_broker) {
        make_topic(topic, sizeof(topic));
        sent = bento_mqtt_publish(link->broker, topic,
                                  (uint32_t)strlen(topic),
                                  (const uint8_t *)g_json_line.text,
                                  g_json_line.used, 0U, 0U) == BENTO_OK;
    }
    return sent;
}

/* The answer, its confidence, and the danger light. */
static bento_status_t build_answer(link_screen_t *screen)
{
    const rect_t confidence = {CONF_X, CONF_Y, 0, 0};
    const rect_t label = {SEES_X, LABEL_Y, 0, 0};
    bento_ui_widget_args_t args;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = ARC_X;
    args.y = ARC_Y;
    args.w = ARC_SIZE;
    args.h = ARC_SIZE;
    args.min_val = 0;
    args.max_val = PERCENT;
    args.font_px = 0;
    status = bento_ui_arc_create(screen->ui, &args, &screen->arc);
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "0 %", &confidence, COLOR_TEXT,
                           TITLE_PX, &screen->confidence);
    }
    if (status == BENTO_OK) {
        status = add_text(screen->ui, "AI เห็นว่า", SEES_X, SEES_Y,
                          COLOR_DIM, THAI_PX);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "-", &label, COLOR_TEXT, BIG_PX,
                           &screen->label);
    }
    if (status == BENTO_OK) {
        status =
            add_led(screen->ui, SEES_X, LED_Y, COLOR_BAD, &screen->led);
    }
    if (status == BENTO_OK) {
        status = add_text(screen->ui, "ไฟแดง = ป้ายอันตราย", LED_NOTE_X,
                          LED_NOTE_Y, COLOR_DIM, THAI_PX);
    }
    return status;
}

/* The MQTT light, what was sent, the JSON and the note. */
static bento_status_t build_link(link_screen_t *screen)
{
    const rect_t mqtt_text = {MQTT_TEXT_X, MQTT_TEXT_Y, 0, 0};
    const rect_t sent = {SEES_X, SENT_Y, 0, 0};
    const rect_t json = {JSON_X, JSON_Y, 0, 0};
    const rect_t note_place = {NOTE_X, NOTE_Y, 0, 0};
    bento_status_t status = add_text(screen->ui, "MQTT", MQTT_X, SEES_Y,
                                     COLOR_DIM, TEXT_PX);

    if (status == BENTO_OK) {
        status = add_led(screen->ui, MQTT_X, MQTT_LED_Y, COLOR_OK,
                         &screen->mqtt);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "ยังไม่ได้ต่อ", &mqtt_text,
                           COLOR_DIM, THAI_PX, &screen->mqtt_text);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "ยังไม่ได้ส่ง", &sent, COLOR_DIM,
                           THAI_PX, &screen->sent);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", &json, COLOR_TEXT, SMALL_PX,
                           &screen->json);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", &note_place, COLOR_WARN,
                           THAI_PX, &screen->note);
    }
    return status;
}

static bento_status_t build_screen(link_screen_t *screen)
{
    const rect_t model = {TITLE_X, MODEL_Y, 0, 0};
    bento_status_t status =
        bento_ui_screen(screen->ui, BENTO_RUNTIME_SCREEN_WIDTH_PX,
                        BENTO_RUNTIME_SCREEN_HEIGHT_PX);

    if (status == BENTO_OK) {
        status = pause_ms(SCREEN_SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_text(screen->ui, "ผล AI ขึ้น MQTT", TITLE_X, TITLE_Y,
                          COLOR_TEXT, TITLE_PX);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "หาโมเดล...", &model, COLOR_INFO,
                           MODEL_PX, &screen->model);
    }
    if (status == BENTO_OK) {
        status = build_answer(screen);
    }
    if (status == BENTO_OK) {
        status = build_link(screen);
    }
    if (status == BENTO_OK) {
        drain_events(screen->ui);
    }
    return status;
}

/* The MQTT light: green = on the broker, the app gets each line;
 * off = offline, the lines go to the Console. */
static void show_link(const link_screen_t *screen, bool online)
{
    (void)bento_ui_set_value(screen->ui, screen->mqtt, online ? 1 : 0);
    (void)bento_ui_set_color(screen->ui, screen->mqtt_text,
                             online ? COLOR_OK : COLOR_BAD);
    set_text(screen->ui, screen->mqtt_text,
             online ? "เชื่อมต่อแล้ว" : "ออฟไลน์");
}

static void show_result(const link_screen_t *screen,
                        const watch_t *watch)
{
    char text[TEXT_BYTES];
    const bool bad = is_danger(watch->label);

    set_text(screen->ui, screen->label, watch->label);
    (void)snprintf(text, sizeof(text), "%" PRId32 " %%",
                   watch->confidence);
    set_text(screen->ui, screen->confidence, text);
    (void)bento_ui_set_value(screen->ui, screen->arc,
                             watch->confidence);
    (void)bento_ui_set_color(screen->ui, screen->arc,
                             bad ? COLOR_BAD : COLOR_OK);
    (void)bento_ui_set_value(screen->ui, screen->led, bad ? 1 : 0);
}

/* The top label of a result, or "-" when there is none. */
static void read_label(const bento_edgeai_result_t *result, char *label,
                       size_t cap)
{
    uint32_t length = 0U;

    if ((bento_edgeai_label(result->index, result->top, label,
                            (uint32_t)cap, &length) != BENTO_OK) ||
        (label[0] == '\0')) {
        (void)snprintf(label, cap, "%s", "-");
    }
}

/* A new answer from the AI: shown, and a beep the moment the label
 * turns into a danger label. A failed read skips the round. */
static bento_status_t take_result(const link_screen_t *screen,
                                  watch_t *watch)
{
    bento_edgeai_result_t result;
    char old[BENTO_EDGEAI_LABEL_LEN];
    bool had_label;
    bento_status_t status = BENTO_OK;

    bento_edgeai_result_init(&result);
    if ((bento_edgeai_result(&result) == BENTO_OK) &&
        result.has_verdict &&
        (!watch->has_sequence || (result.seq != watch->sequence))) {
        had_label = watch->has_label;
        (void)snprintf(old, sizeof(old), "%s", watch->label);
        watch->has_sequence = true;
        watch->sequence = result.seq;
        read_label(&result, watch->label, sizeof(watch->label));
        watch->has_label = true;
        watch->confidence = as_percent(result.score_pct[result.top]);
        show_result(screen, watch);
        if (is_danger(watch->label) &&
            (!had_label || (strcmp(watch->label, old) != 0))) {
            status = beep(screen->ui, (uint8_t)NOTE_C6,
                          (uint8_t)NOTE_E5); /* once, as it turns */
        }
    }
    return status;
}

/* Sends when it means something or the heartbeat is due. A line
 * that cannot be sent while online means the link is broken: the
 * old line is closed, and the drop check below counts it. */
static void maybe_send(const link_screen_t *screen, link_t *link,
                       watch_t *watch)
{
    char text[TEXT_BYTES];
    const bool changed = !watch->has_sent_label ||
                         (strcmp(watch->label, watch->sent_label) != 0);
    bool sent;

    if (watch->has_label &&
        should_send(changed,
                    bento_time_ticks_diff(bento_time_ticks_ms(),
                                          watch->sent_ms))) {
        watch->sent_count++;
        sent = send(link, watch);
        /* the time it really went: a beep before it takes ~240 ms */
        watch->sent_ms = bento_time_ticks_ms();
        (void)snprintf(watch->sent_label, sizeof(watch->sent_label),
                       "%s", watch->label);
        watch->has_sent_label = true;
        watch->sent_ok += sent ? 1 : 0;
        (void)snprintf(text, sizeof(text),
                       "ส่งออกได้ %" PRId32 " จาก %" PRId32 " ข้อความ",
                       watch->sent_ok, watch->sent_count);
        set_text(screen->ui, screen->sent, text);
        (void)snprintf(text, sizeof(text),
                       "%s %" PRId32 "%% n=%" PRId32, watch->label,
                       watch->confidence, watch->sent_count);
        set_text(screen->ui, screen->json, text);
        if (link->online && !sent) {
            disconnect(link);
        }
    }
}

/* Dropped: keep working offline and wait to reconnect. */
static void check_drop(const link_screen_t *screen, link_t *link)
{
    char text[TEXT_BYTES];

    if (link->online && !broker_connected(link)) {
        link->online = false;
        link->drops++;
        show_link(screen, false);
        link->retry_ms = bento_time_ticks_ms();
        link->wait_ms = RETRY_MS;
        (void)snprintf(text, sizeof(text),
                       "เน็ตหลุด (ครั้งที่ %" PRId32 ") จะต่อใหม่ใน %" PRId32
                       " วินาที",
                       link->drops, link->wait_ms / SECOND_MS);
        note(screen, text, COLOR_WARN);
    }
}

/* Time to try again: back on line, or wait twice as long (at most
 * RETRY_MAX_MS). */
static bento_status_t retry_link(const link_screen_t *screen,
                                 link_t *link, const watch_t *watch)
{
    char text[TEXT_BYTES];
    bento_status_t status = BENTO_OK;

    if (!link->online && link->can_retry &&
        (bento_time_ticks_diff(bento_time_ticks_ms(), link->retry_ms) >=
         link->wait_ms)) {
        status = reconnect(screen, link, &link->online);
        show_link(screen, link->online);
        if (link->online) {
            link->wait_ms = RETRY_MS;
            (void)snprintf(text, sizeof(text),
                           "ต่อใหม่สำเร็จ ส่งต่อที่ n=%" PRId32
                           " (หลุดไป %" PRId32 " ครั้ง)",
                           watch->sent_count + 1, link->drops);
            note(screen, text, COLOR_OK);
        } else {
            link->retry_ms = bento_time_ticks_ms();
            link->wait_ms = ((link->wait_ms * 2) < RETRY_MAX_MS)
                                ? (link->wait_ms * 2)
                                : RETRY_MAX_MS;
            (void)snprintf(text, sizeof(text),
                           "ยังต่อไม่ได้ จะลองใหม่ใน %" PRId32 " วินาที",
                           link->wait_ms / SECOND_MS);
            note(screen, text, COLOR_WARN);
        }
    }
    return status;
}

/* Every TICK_MS until RUN_MS: a new answer, a send, the link. */
static bento_status_t watch_and_send(const link_screen_t *screen,
                                     link_t *link, watch_t *watch)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    watch->sent_ms = start_ms;
    link->retry_ms = start_ms;
    link->wait_ms = RETRY_MS;
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = take_result(screen, watch);
        if (status == BENTO_OK) {
            maybe_send(screen, link, watch);
            check_drop(screen, link);
            status = retry_link(screen, link, watch);
        }
        if (status == BENTO_OK) {
            drain_events(screen->ui);
            status = pause_ms(TICK_MS);
        }
    }
    return status;
}

/* The model, then the link, then the loop. */
static bento_status_t run(const link_screen_t *screen, link_t *link,
                          watch_t *watch)
{
    char text[TEXT_BYTES];
    link_result_t result = LINK_UNSET;
    bool selected = false;
    bento_status_t status = pick_model(screen, watch, &selected);

    if ((status == BENTO_OK) && !selected) {
        (void)snprintf(text, sizeof(text),
                       "ไม่พบหรือโหลดโมเดลไม่ได้: %s / %s", MODEL_KEYS[0],
                       MODEL_KEYS[1]);
        note(screen, text, COLOR_BAD);
    }
    if ((status == BENTO_OK) && selected) {
        (void)snprintf(text, sizeof(text), "โมเดล: %s", watch->name);
        set_text(screen->ui, screen->model, text);
        status = go_online(screen, link, &result);
        link->can_retry = result != LINK_UNSET; /* settings complete */
        link->online = result == LINK_ONLINE;
        show_link(screen, link->online);
    }
    if ((status == BENTO_OK) && selected) {
        status = watch_and_send(screen, link, watch);
    }
    return status;
}

int main(void)
{
    link_screen_t screen;
    link_t link;
    watch_t watch;
    bento_status_t status;
    bento_status_t stop_status;

    (void)memset(&g_json_line, 0, sizeof(g_json_line));
    (void)memset(&screen, 0, sizeof(screen));
    (void)memset(&link, 0, sizeof(link));
    (void)memset(&watch, 0, sizeof(watch));
    screen.ui = bento_runtime_get_ui();
    /* a build without sound refuses it, as hasattr() skips it */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    status = build_screen(&screen);
    if (status == BENTO_OK) {
        status = run(&screen, &link, &watch);
        if (status == BENTO_E_CANCELLED) { /* the finally still runs */
            (void)bento_runtime_acknowledge_stop();
        }
        stop_status = stop_ai(); /* the model stops, always */
        disconnect(&link);
        status = (status == BENTO_E_CANCELLED) ? stop_status : status;
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
