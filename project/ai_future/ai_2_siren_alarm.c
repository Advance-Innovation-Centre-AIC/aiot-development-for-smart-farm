/*
 * ai_2_siren_alarm.c -- ฟังเสียงไซเรนหน้าฟาร์มด้วย AI บนบอร์ด
 * แล้วแจ้งแอปของกลุ่มทาง MQTT
 *
 * รถฉุกเฉินมาหน้าฟาร์ม (ไซเรน) = เตือนคนในฟาร์มให้ไปเปิดประตู
 * โมเดลฟังไมโครโฟน ป้าย sirens มั่นใจถึง CONF_MIN % อย่างน้อย 3 ใน 5
 * ผลล่าสุด = เตือน (ไฟแดง ตัววิ่ง และเสียง) หลังบอร์ดส่งเสียงเอง
 * ไม่เชื่อผล MUTE_MS ผลไม่ขยับนานเกิน STALL_MS จอบอกให้รีเซ็ตบอร์ด
 * ลองเปิดคลิปเสียงไซเรนจากมือถือห่างบอร์ดราว 1 เมตร แล้วลองเสียงพูด
 * เพลง ตบมือ ดูว่าเตือนมั่วไหม โมเดล SirenDetection (Deploy จาก Edge
 * AI Store) ถ้าไม่มีใช้ Siren Detection ในตัว ผลส่งขึ้น
 * bento-aiot/<TEAM>/ai และ event ai_alarm_start / ai_alarm_end ขึ้น
 * bento-aiot/<TEAM>/event แก้ WIFI_SSID WIFI_PASS และ TEAM ก่อนรัน
 * (ยังไม่แก้ = ทำงานออฟไลน์ พิมพ์ผลลง Console แทน) ผล AI ใน BENTO
 * Emulator เป็นผลจำลอง ต้องทดสอบบนบอร์ดจริง
 *
 * An emergency vehicle at the farm gate (a siren) = tell the people
 * on the farm to open the gate. The model listens to the microphone:
 * the label sirens with at least CONF_MIN % in 3 of the last 5
 * results is an alarm (red LED, the matrix and a tone). After the
 * board's own tone, results are not trusted for MUTE_MS; results
 * that stop moving for STALL_MS ask for a board reset. Play a siren
 * clip from a phone about 1 metre away, then try speech, music and
 * claps to see whether it raises false alarms. The model is
 * SirenDetection from the Edge AI Store, else the built-in Siren
 * Detection. Results go to bento-aiot/<TEAM>/ai, and the events
 * ai_alarm_start / ai_alarm_end to bento-aiot/<TEAM>/event. Set
 * WIFI_SSID, WIFI_PASS and TEAM first; unset, it runs offline and
 * prints every message on the console. In the BENTO Emulator the AI
 * result is simulated: test on a real board.
 *
 * Twin of ai_2_siren_alarm.py
 * (aiot-development-for-smart-farm/project/ai_future/ai_2_siren_alarm.py).
 * Style: CLEAN_C.md.
 * Differs from the original:
 *   - json.dumps becomes snprintf into a buffer sized for every
 *     field; the keys are written in the .py's order (MicroPython's
 *     dict may print them in another order). Model names and labels
 *     are written as the board gives them: they hold no quote.
 *   - edge_ai.models() reads the whole list or raises; here the rows
 *     are read one by one and a row that cannot be read is skipped.
 *     The model's labels are read once, when it starts; a result
 *     names its winning class by index.
 *   - edge_ai.select(), wifi.connect() and mqtt.connect() wait inside
 *     the binding; here they are started and polled, so Stop ends a
 *     wait at once.
 *   - A reconnect closes the lost MQTT client before it opens a new
 *     one, and the program also closes the module's Edge AI link
 *     (bento_edgeai_deinit) after the final stop.
 *   - A board without the model link (reTerminal) answers
 *     BENTO_E_UNSUPPORTED to bento_edgeai_init(): the same "no model"
 *     path as an empty edge_ai.models().
 *   - In the BENTO Emulator, Edge AI/MQTT from C is not available
 *     yet: the program says so and ends. The line goes up beside the
 *     .py's own "no model" note, which then follows as before.
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
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_time.h"
#include "bento_wifi.h"

/* Edit before running. Unset, the program runs offline and prints
 * every message on the console. TEAM is the number the teacher hands
 * out, such as team05; it must match the app. */
static const char WIFI_SSID[] = "<ชื่อ Hotspot ของกลุ่ม>";
/* WIFI_PASS is wifi_pass in wifi_up(), BROKER is broker_host in
 * open_broker(). */
static const char TEAM[] = "teamXX";

/* The names on the board: the Store's model first, then the
 * built-in one; the label that counts as a siren is in is_danger(). */
static const char *const MODEL_KEYS[] = {"SirenDetection",
                                         "Siren Detection"};

/* The settings. */
enum {
    CONF_MIN = 60, /* below this many % the AI is not counted */
    CONFIRM_N = 3,
    WINDOW_N = 5,
    MUTE_MS = 800,   /* the board's own tone reaches the microphone */
    STALL_MS = 8000, /* no new result this long: reset the board */
    HEARTBEAT_MS = 2000,
    GAP_MS = 200,
    RETRY_MS = 10000, /* a lost link is tried again this often */
    TICK_MS = 100,
    RUN_MS = 300000,
    VOLUME = 25,
    SPEAKER = 40
};

/* The course palette, 0xRRGGBB. */
enum {
    COLOR_TEXT = 0xE8EAED,
    COLOR_DIM = 0x9AA3AF,
    COLOR_OK = 0x30A46C,
    COLOR_WARN = 0xF5A623,
    COLOR_BAD = 0xE5484D,
    COLOR_INFO = 0x4A9EFF
};

/* The link, the tones and the sizes of what is built. */
enum {
    PORT = 1883,
    KEEPALIVE_S = 30,
    CLIENT_SUFFIX_MASK = 0xFFFF, /* four hex digits of the clock */
    WIFI_SLICE_MS = 50,
    MQTT_SLICE_MS = 20,
    OP_POLL_MS = 20,
    SETTLE_MS = 200, /* let the cleared screen settle */
    TONE_MS = 120,
    MATRIX_MS = 80,
    NOTE_HIGH = 84, /* the alarm falls 84 -> 76, the all-clear rises */
    NOTE_ALARM_LOW = 76,
    NOTE_CLEAR_LOW = 79,
    PERCENT = 100,
    NAME_BYTES = 48,     /* the client id or a topic */
    TEXT_BYTES = 128,    /* a label's text: a SET_TEXT holds 126 */
    PAYLOAD_BYTES = 256, /* the JSON line with two 47-byte names */
    TEAM_CHARS = 6,      /* "team" and two digits */
    TEAM_PREFIX = 4
};

/* The widgets the program changes, in the order they are made. */
typedef enum {
    SLOT_MODEL = 0,
    SLOT_ARC,
    SLOT_CONF,
    SLOT_LABEL,
    SLOT_LED,
    SLOT_STATE,
    SLOT_HISTORY,
    SLOT_MQTT,
    SLOT_SENT,
    SLOT_NOTE,
    SLOT_COUNT,
    SLOT_NONE
} slot_t;

typedef enum {
    KIND_LABEL = 0,
    KIND_ARC,
    KIND_LED
} kind_t;

/* The model that runs, with its labels. */
typedef struct {
    uint32_t index;
    bool found;
    uint32_t class_count;
    char name[BENTO_EDGEAI_NAME_LEN];
    char labels[BENTO_EDGEAI_MAX_CLASSES][BENTO_EDGEAI_LABEL_LEN];
} model_t;

/* The AI: the latest result, the window of siren flags and the
 * alarm they give. */
typedef struct {
    bool has_sequence;
    uint32_t sequence;
    char label[BENTO_EDGEAI_LABEL_LEN];
    int32_t conf;
    uint8_t history[WINDOW_N];
    uint32_t history_count;
    bool alarm;
    bool stalled;
    uint32_t last_new_ms;
    uint32_t quiet_ms; /* results before this are the board's tone */
} verdict_t;

/* The network and what has been sent. */
typedef struct {
    bool can_net;
    bool online;
    bool has_broker;
    uint32_t broker;
    char topic_ai[NAME_BYTES];
    char topic_event[NAME_BYTES];
    int32_t sent;
    int32_t delivered;
    bool has_key;
    char key_label[BENTO_EDGEAI_LABEL_LEN];
    bool key_alarm;
    uint32_t sent_ms;
    uint32_t retry_ms;
} link_t;

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t ids[SLOT_COUNT];
    model_t model;
    verdict_t ai;
    link_t link;
    bool has_led;
    uint32_t led;
    char payload[PAYLOAD_BYTES];
} siren_t;

/* Too big for the 8 KB program stack: file scope, zeroed by the
 * loader and set again at the start of main() (CLEAN_C.md C9,
 * deviation BENTO-MISRA-E03/E04). */
extern siren_t g_siren;
siren_t g_siren;

/* ---- hardware ---- */

/* Sleeps, and turns Stop into BENTO_E_CANCELLED. */
static bento_status_t wait_ms(uint32_t duration_ms)
{
    bento_status_t status = bento_time_sleep_ms(duration_ms);

    if ((status == BENTO_OK) && !bento_runtime_is_running()) {
        status = BENTO_E_CANCELLED;
    }
    return status;
}

/* ui.poll() only keeps the display moving here: a failed poll
 * answers no events in MicroPython, so it is not a failure. */
static void poll_display(bento_ui_t *ui)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    (void)bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                        NULL);
}

static void show_text(const siren_t *siren, slot_t slot,
                      const char *text)
{
    /* MicroPython's widget setters ignore a refusal (modui.c) */
    (void)bento_ui_set_text(siren->ui, siren->ids[slot], text,
                            (uint32_t)strlen(text));
}

static void show_color(const siren_t *siren, slot_t slot,
                       uint32_t color)
{
    (void)bento_ui_set_color(siren->ui, siren->ids[slot], color);
}

/* Two notes of TONE_MS each, as the .py's beep(). */
static bento_status_t beep(const siren_t *siren, uint8_t first,
                           uint8_t second)
{
    const uint8_t notes[] = {first, second};
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < 2U) && (status == BENTO_OK); i++) {
        /* a tone's result is not read in MicroPython either */
        (void)bento_ui_sfx_tone(siren->ui, notes[i], BENTO_UI_WAVE_SINE,
                                (uint8_t)VOLUME, (uint16_t)TONE_MS);
        status = wait_ms(TONE_MS);
    }
    return status;
}

static void matrix_scroll(const char *text, int32_t color)
{
    /* the .py ignores an OSError from the matrix */
    (void)bento_rgbmatrix_scroll(text, (uint32_t)strlen(text), color,
                                 MATRIX_MS);
}

/* The LED named RGB_RED, else LED 0; none when the board cannot say
 * or has no LED, as the .py's None. */
static void find_led(siren_t *siren)
{
    bento_gpio_board_info_t info;
    const char *name = NULL;
    uint32_t i;

    bento_gpio_board_info_init(&info);
    siren->has_led =
        (bento_gpio_board_info(&info) == BENTO_OK) && (info.leds > 0U);
    siren->led = 0U;
    for (i = 0U; siren->has_led && (i < info.leds); i++) {
        if ((bento_gpio_led_name(i, &name) == BENTO_OK) &&
            (name != NULL) && (strcmp(name, "RGB_RED") == 0)) {
            siren->led = i;
        }
    }
}

static bento_status_t set_led(const siren_t *siren, bool on)
{
    bento_status_t status = BENTO_OK;

    if (siren->has_led) {
        status = on ? bento_gpio_led_on(siren->led)
                    : bento_gpio_led_off(siren->led);
    }
    return status;
}

/* ---- Edge AI ---- */

/* Waits for a started select or stop; Stop cancels it. */
static bento_status_t finish_op(void)
{
    bento_status_t status = BENTO_E_IN_PROGRESS;

    while (status == BENTO_E_IN_PROGRESS) {
        status = wait_ms(OP_POLL_MS);
        if (status == BENTO_OK) {
            status = bento_edgeai_op_poll(NULL);
        } else {
            (void)bento_edgeai_op_cancel(); /* Stop wins */
        }
    }
    return status;
}

static char lower_ascii(char letter)
{
    static const char upper_letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char lower_letters[] = "abcdefghijklmnopqrstuvwxyz";
    char lower = letter;
    size_t i;

    for (i = 0U; upper_letters[i] != '\0'; i++) {
        if (letter == upper_letters[i]) {
            lower = lower_letters[i];
        }
    }
    return lower;
}

/* key.lower() in name.lower() */
static bool name_has(const char *name, const char *key)
{
    const size_t key_length = strlen(key);
    bool match = false;
    size_t start;

    for (start = 0U; (name[start] != '\0') && !match; start++) {
        size_t i = 0U;

        while ((i < key_length) && (name[start + i] != '\0') &&
               (lower_ascii(name[start + i]) == lower_ascii(key[i]))) {
            i++;
        }
        match = i == key_length;
    }
    return match;
}

static void read_labels(model_t *model, uint32_t class_count)
{
    uint32_t length = 0U;
    uint32_t i;

    model->class_count = (class_count > BENTO_EDGEAI_MAX_CLASSES)
                             ? BENTO_EDGEAI_MAX_CLASSES
                             : class_count;
    for (i = 0U; i < model->class_count; i++) {
        if (bento_edgeai_label(model->index, i, model->labels[i],
                               BENTO_EDGEAI_LABEL_LEN,
                               &length) != BENTO_OK) {
            model->labels[i][0] = '\0';
        }
    }
}

/* Starts row index; a select the engine refuses leaves the model
 * unfound so the next row is tried. Only Stop is an error. */
static bento_status_t start_model(model_t *model, uint32_t index,
                                  const bento_edgeai_model_info_t *info)
{
    bento_status_t status = bento_edgeai_select_start(index);

    if (status == BENTO_OK) {
        status = finish_op();
    }
    model->found = status == BENTO_OK;
    if (model->found) {
        model->index = index;
        (void)memcpy(model->name, info->name, sizeof(info->name));
        read_labels(model, info->class_count);
    }
    return (status == BENTO_E_CANCELLED) ? status : BENTO_OK;
}

/* Tries every model whose name holds key; the first one that starts
 * is kept. */
static bento_status_t try_key(model_t *model, const char *key,
                              int32_t count)
{
    bento_edgeai_model_info_t info;
    bento_status_t status = BENTO_OK;
    int32_t i;

    for (i = 0; (i < count) && (status == BENTO_OK) && !model->found;
         i++) {
        bento_edgeai_model_info_init(&info);
        if ((bento_edgeai_model_info((uint32_t)i, &info) == BENTO_OK) &&
            name_has(info.name, key)) {
            status = start_model(model, (uint32_t)i, &info);
        }
    }
    return status;
}

/* By name, in MODEL_KEYS order: the order on the board can change
 * after a reboot. No link, or no list, means no model. */
static bento_status_t find_model(model_t *model, bool *no_link)
{
    const uint32_t keys =
        (uint32_t)(sizeof(MODEL_KEYS) / sizeof(MODEL_KEYS[0]));
    const bento_status_t link = bento_edgeai_init();
    bool has_link = false;
    int32_t count = 0;
    bento_status_t status = BENTO_OK;
    uint32_t i;

    /* a reTerminal answers init the same way but says "no link" to
     * the query; only the BENTO Emulator's C runtime refuses both */
    *no_link = false;
    if (link == BENTO_E_UNSUPPORTED) {
        *no_link =
            bento_edgeai_supported(&has_link) == BENTO_E_UNSUPPORTED;
    }
    if ((link != BENTO_OK) ||
        (bento_edgeai_count(&count) != BENTO_OK)) {
        count = 0;
    }
    for (i = 0U; (i < keys) && (status == BENTO_OK) && !model->found;
         i++) {
        status = try_key(model, MODEL_KEYS[i], count);
    }
    return status;
}

/* int(pct / 100 * 100) in the board's float, as the .py works it. */
static int32_t percent_of(uint8_t pct)
{
    const float hundred = 100.0F;
    const float score = (float)pct / hundred;
    const bento_real_t scaled =
        (bento_real_t)score * (bento_real_t)PERCENT;

    return (int32_t)scaled;
}

static const char *label_of(const model_t *model, uint32_t index)
{
    return (index < model->class_count) ? model->labels[index] : "";
}

/* ---- the brain ---- */

/* As sf3_04: trouble from CONFIRM_N in the last WINDOW_N results; it
 * clears only when the whole window is safe. */
static void alert_rule(verdict_t *ai, bool danger)
{
    uint32_t hits = 0U;
    uint32_t i;

    if (ai->history_count == (uint32_t)WINDOW_N) {
        (void)memmove(&ai->history[0], &ai->history[1],
                      sizeof(ai->history) - sizeof(ai->history[0]));
        ai->history_count--;
    }
    ai->history[ai->history_count] = danger ? 1U : 0U;
    ai->history_count++;
    for (i = 0U; i < ai->history_count; i++) {
        hits += ai->history[i];
    }
    ai->alarm =
        (hits >= (uint32_t)CONFIRM_N) || (ai->alarm && (hits > 0U));
}

static uint32_t history_hits(const verdict_t *ai)
{
    uint32_t hits = 0U;
    uint32_t i;

    for (i = 0U; i < ai->history_count; i++) {
        hits += ai->history[i];
    }
    return hits;
}

static bool is_danger(const char *label)
{
    static const char *const danger_labels[1] = {"sirens"};
    const uint32_t count =
        (uint32_t)(sizeof(danger_labels) / sizeof(danger_labels[0]));
    bool danger = false;
    uint32_t i;

    for (i = 0U; i < count; i++) {
        danger = danger || (strcmp(label, danger_labels[i]) == 0);
    }
    return danger;
}

static bool should_send(bool changed, int32_t since_ms)
{
    return (since_ms >= GAP_MS) &&
           (changed || (since_ms >= HEARTBEAT_MS));
}

/* ---- the network (sf3_06, short) ---- */

/* team01 style: "team", two digits, not team00. */
static bool team_ok(void)
{
    const char *digits = &TEAM[TEAM_PREFIX];
    bool good = (strlen(TEAM) == (size_t)TEAM_CHARS) &&
                (strncmp(TEAM, "team", (size_t)TEAM_PREFIX) == 0);

    good = good && (digits[0] >= '0') && (digits[0] <= '9') &&
           (digits[1] >= '0') && (digits[1] <= '9');
    return good && (strcmp(TEAM, "team00") != 0);
}

static bool ssid_unset(void)
{
    return (WIFI_SSID[0] == '<') || (WIFI_SSID[0] == '\0');
}

static bool wifi_connected(void)
{
    bool connected = false;

    if (bento_wifi_is_connected(&connected) != BENTO_OK) {
        connected = false;
    }
    return connected;
}

/* Asks the started join every WIFI_SLICE_MS until it ends. */
static bento_status_t wait_join(void)
{
    bento_status_t status = BENTO_E_IN_PROGRESS;

    while (status == BENTO_E_IN_PROGRESS) {
        status = bento_wifi_connect_poll();
        if (status == BENTO_E_IN_PROGRESS) {
            status = wait_ms(WIFI_SLICE_MS);
            status =
                (status == BENTO_OK) ? BENTO_E_IN_PROGRESS : status;
        }
    }
    if (status == BENTO_E_CANCELLED) {
        (void)bento_wifi_connect_cancel(); /* answers BENTO_OK always */
    }
    return status;
}

/* Joined with an address; only Stop is an error. */
static bento_status_t wifi_up(bool *up)
{
    static const char wifi_pass[] = "<รหัส Hotspot ของกลุ่ม>";
    bento_wifi_connect_params_t params;
    uint32_t address = 0U;
    bento_status_t status = BENTO_OK;

    *up = wifi_connected();
    if (!*up && !ssid_unset()) {
        bento_wifi_connect_params_init(&params);
        params.ssid = WIFI_SSID;
        params.ssid_len = (uint32_t)strlen(WIFI_SSID);
        params.pass = wifi_pass;
        params.pass_len = (uint32_t)strlen(wifi_pass);
        params.flags = BENTO_WIFI_F_SAVE_CREDS; /* as the binding */
        status = bento_wifi_connect_start(&params);
        if (status == BENTO_OK) {
            status = wait_join();
        }
        *up = status == BENTO_OK;
    }
    if (*up) {
        *up =
            (bento_wifi_ip(BENTO_WIFI_IF_ANY, &address) == BENTO_OK) &&
            (address != 0U);
    }
    return (status == BENTO_E_CANCELLED) ? status : BENTO_OK;
}

/* Asks the started connect every MQTT_SLICE_MS until it ends. */
static bento_status_t wait_broker(uint32_t broker)
{
    bento_status_t status = BENTO_E_IN_PROGRESS;

    while (status == BENTO_E_IN_PROGRESS) {
        status = wait_ms(MQTT_SLICE_MS);
        if (status == BENTO_OK) {
            status = bento_mqtt_open_poll(broker);
        }
    }
    return status;
}

/* A fresh client id each time, so the broker never kicks a twin. */
static bento_status_t open_broker(link_t *link)
{
    static const char broker_host[] = "broker.hivemq.com";
    bento_mqtt_config_t config;
    char client_id[NAME_BYTES];
    bento_status_t status;

    (void)snprintf(
        client_id, sizeof(client_id), "bento-ai-%s-%04" PRIx32, TEAM,
        bento_time_ticks_ms() & (uint32_t)CLIENT_SUFFIX_MASK);
    bento_mqtt_config_init(&config);
    config.host = broker_host;
    config.host_len = (uint32_t)strlen(broker_host);
    config.port = (uint16_t)PORT;
    config.keepalive_s = (uint16_t)KEEPALIVE_S;
    config.client_id = client_id;
    config.client_id_len = (uint32_t)strlen(client_id);
    status = bento_mqtt_open_start(&config, &link->broker);
    link->has_broker = status == BENTO_OK;
    if (link->has_broker) {
        status = wait_broker(link->broker);
    }
    link->online = status == BENTO_OK;
    return (status == BENTO_E_CANCELLED) ? status : BENTO_OK;
}

static void close_broker(link_t *link)
{
    if (link->has_broker) {
        /* the .py ignores an OSError from disconnect() */
        (void)bento_mqtt_close(link->broker);
        link->has_broker = false;
    }
}

/* Wi-Fi, then the broker; offline when either fails. */
static bento_status_t go_online(link_t *link)
{
    bool up = false;
    bento_status_t status = wifi_up(&up);

    close_broker(link);
    link->online = false;
    if ((status == BENTO_OK) && up) {
        status = open_broker(link);
    }
    return status;
}

static bool broker_alive(const link_t *link)
{
    uint32_t state = BENTO_MQTT_STATE_CLOSED;

    return (bento_mqtt_state(link->broker, &state) == BENTO_OK) &&
           (state == BENTO_MQTT_STATE_CONNECTED);
}

/* Prints the line, then publishes it to topic when online. */
static bool send(const link_t *link, const char *topic,
                 const char *line)
{
    bool delivered = false;

    (void)printf("%s\n", line);
    if (link->online) {
        delivered = bento_mqtt_publish(
                        link->broker, topic, (uint32_t)strlen(topic),
                        (const uint8_t *)line, (uint32_t)strlen(line),
                        0U, 0U) == BENTO_OK;
    }
    return delivered;
}

static void show_sent(const siren_t *siren)
{
    const link_t *link = &siren->link;
    char text[TEXT_BYTES];

    (void)snprintf(
        text, sizeof(text), "ส่งได้ %" PRId32 " จาก %" PRId32 " ข้อความ%s",
        link->delivered, link->sent, link->online ? "" : " (ออฟไลน์)");
    show_text(siren, SLOT_SENT, text);
}

/* The message of contract item 3.10, with the alarm key. */
static void report(siren_t *siren, uint32_t now_ms)
{
    link_t *link = &siren->link;
    const verdict_t *ai = &siren->ai;
    const bool changed = !link->has_key ||
                         (link->key_alarm != ai->alarm) ||
                         (strcmp(link->key_label, ai->label) != 0);
    bool delivered;

    if (should_send(changed,
                    bento_time_ticks_diff(bento_time_ticks_ms(),
                                          link->sent_ms))) {
        link->sent++;
        (void)snprintf(siren->payload, sizeof(siren->payload),
                       "{\"id\": \"%s\", \"n\": %" PRId32
                       ", \"model\": \"%s\", \"label\": \"%s\", "
                       "\"conf\": %" PRId32 ", \"alarm\": %d}",
                       TEAM, link->sent, siren->model.name, ai->label,
                       ai->conf, ai->alarm ? 1 : 0);
        delivered = send(link, link->topic_ai, siren->payload);
        link->delivered += delivered ? 1 : 0;
        link->sent_ms = bento_time_ticks_ms();
        link->has_key = true;
        (void)snprintf(link->key_label, sizeof(link->key_label), "%s",
                       ai->label);
        link->key_alarm = ai->alarm;
        show_sent(siren);
        if (link->online && !delivered) {
            link->online = false;
            link->retry_ms = now_ms;
        }
    }
}

/* A lost broker goes offline; offline tries again every RETRY_MS. */
static bento_status_t keep_link(siren_t *siren, uint32_t now_ms)
{
    link_t *link = &siren->link;
    bento_status_t status = BENTO_OK;

    if (link->online && !broker_alive(link)) {
        link->online = false;
        link->retry_ms = now_ms;
    }
    if (link->can_net && !link->online &&
        (bento_time_ticks_diff(now_ms, link->retry_ms) >= RETRY_MS)) {
        status = go_online(link);
        link->retry_ms = bento_time_ticks_ms();
        (void)bento_ui_set_value(siren->ui, siren->ids[SLOT_MQTT],
                                 link->online ? 1 : 0);
    }
    return status;
}

/* ---- the screen ---- */

/* One widget of the screen's table. */
typedef struct {
    kind_t kind;
    slot_t slot;
    const char *text;
    int16_t x;
    int16_t y;
    int16_t size; /* a label's font, else the width and height */
    uint32_t color;
} part_t;

static bento_status_t add_part(siren_t *siren, const part_t *part)
{
    bento_ui_widget_args_t args;
    bento_ui_id_t made = 0U;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = part->x;
    args.y = part->y;
    args.color = part->color;
    if (part->kind == KIND_LABEL) {
        args.text = part->text;
        args.text_len = (uint32_t)strlen(part->text);
        args.font_px = part->size;
        status = bento_ui_label_create(siren->ui, &args, &made);
    } else {
        args.w = part->size;
        args.h = part->size;
        args.font_px = 0;
        if (part->kind == KIND_ARC) {
            args.max_val = PERCENT;
            status = bento_ui_arc_create(siren->ui, &args, &made);
        } else {
            status = bento_ui_led_create(siren->ui, &args, &made);
        }
    }
    if (part->slot != SLOT_NONE) {
        siren->ids[part->slot] = made;
    }
    return status;
}

static bento_status_t build_screen(siren_t *siren)
{
    static const part_t parts[] = {
        {KIND_LABEL, SLOT_NONE, "ฟังเสียงไซเรนหน้าฟาร์ม (Edge AI)", 12, 6,
         24, COLOR_TEXT},
        {KIND_LABEL, SLOT_MODEL, "หาโมเดล...", 12, 44, 20, COLOR_INFO},
        {KIND_ARC, SLOT_ARC, "", 12, 84, 180, 0U},
        {KIND_LABEL, SLOT_CONF, "0 %", 70, 162, 24, COLOR_TEXT},
        {KIND_LABEL, SLOT_NONE, "AI ได้ยิน", 220, 90, 20, COLOR_DIM},
        {KIND_LABEL, SLOT_LABEL, "-", 220, 116, 28, COLOR_TEXT},
        {KIND_LED, SLOT_LED, "", 220, 170, 36, COLOR_BAD},
        {KIND_LABEL, SLOT_STATE, "ปกติ", 268, 178, 20, COLOR_OK},
        {KIND_LABEL, SLOT_HISTORY, " ", 220, 230, 20, COLOR_DIM},
        {KIND_LED, SLOT_MQTT, "", 12, 300, 24, COLOR_OK},
        {KIND_LABEL, SLOT_SENT, "ยังไม่ได้ส่ง", 44, 302, 20, COLOR_DIM},
        {KIND_LABEL, SLOT_NOTE, " ", 12, 352, 20, COLOR_WARN}};
    const uint32_t count = (uint32_t)(sizeof(parts) / sizeof(parts[0]));
    bento_status_t status = bento_ui_screen_default(siren->ui);
    uint32_t i;

    if (status == BENTO_OK) {
        status = wait_ms(SETTLE_MS);
    }
    for (i = 0U; (i < count) && (status == BENTO_OK); i++) {
        status = add_part(siren, &parts[i]);
    }
    if (status == BENTO_OK) {
        poll_display(siren->ui);
    }
    return status;
}

static void show(const siren_t *siren)
{
    const verdict_t *ai = &siren->ai;
    char text[TEXT_BYTES];

    show_text(siren, SLOT_LABEL, ai->label);
    (void)snprintf(text, sizeof(text), "%" PRId32 " %%", ai->conf);
    show_text(siren, SLOT_CONF, text);
    (void)bento_ui_set_value(siren->ui, siren->ids[SLOT_ARC], ai->conf);
    show_color(siren, SLOT_ARC,
               is_danger(ai->label) ? COLOR_BAD : COLOR_OK);
    (void)bento_ui_set_value(siren->ui, siren->ids[SLOT_LED],
                             ai->alarm ? 1 : 0);
    show_text(siren, SLOT_STATE,
              ai->alarm ? "ไซเรน! ไปเปิดประตูฟาร์ม" : "ปกติ");
    show_color(siren, SLOT_STATE, ai->alarm ? COLOR_BAD : COLOR_OK);
    (void)snprintf(text, sizeof(text),
                   "ไซเรน %" PRIu32 " ใน %" PRIu32 " ผลล่าสุด",
                   history_hits(ai), ai->history_count);
    show_text(siren, SLOT_HISTORY, text);
    (void)bento_ui_set_value(siren->ui, siren->ids[SLOT_MQTT],
                             siren->link.online ? 1 : 0);
    show_sent(siren);
}

/* The alarm starts or ends: the LED, the matrix, the tones, a quiet
 * time for the microphone, and the event. */
static bento_status_t follow_alarm(siren_t *siren)
{
    const verdict_t *ai = &siren->ai;
    bento_status_t status = set_led(siren, ai->alarm);

    if (status == BENTO_OK) {
        if (ai->alarm) {
            matrix_scroll("SIREN OPEN GATE", BENTO_RGBMATRIX_RED);
        } else {
            /* an empty text stops the marquee (as sf2_03) */
            matrix_scroll("", BENTO_RGBMATRIX_SCROLL_COLOR_DEFAULT);
            (void)bento_rgbmatrix_clear(); /* an OSError is ignored */
        }
        status = ai->alarm ? beep(siren, NOTE_HIGH, NOTE_ALARM_LOW)
                           : beep(siren, NOTE_CLEAR_LOW, NOTE_HIGH);
    }
    if (status == BENTO_OK) {
        siren->ai.quiet_ms =
            bento_time_ticks_add(bento_time_ticks_ms(), MUTE_MS);
        (void)snprintf(siren->payload, sizeof(siren->payload),
                       "{\"id\": \"%s\", \"event\": \"%s\", "
                       "\"model\": \"%s\", \"label\": \"%s\", "
                       "\"conf\": %" PRId32 "}",
                       TEAM,
                       ai->alarm ? "ai_alarm_start" : "ai_alarm_end",
                       siren->model.name, ai->label, ai->conf);
        (void)send(&siren->link, siren->link.topic_event,
                   siren->payload); /* an event is not counted */
    }
    return status;
}

/* A new result. One that arrives while the board's own tone may
 * still be in the microphone is not trusted. */
static bento_status_t take_result(siren_t *siren,
                                  const bento_edgeai_result_t *result,
                                  uint32_t now_ms)
{
    verdict_t *ai = &siren->ai;
    const char *label = label_of(&siren->model, result->top);
    bento_status_t status = BENTO_OK;

    ai->has_sequence = true;
    ai->sequence = result->seq;
    ai->last_new_ms = now_ms;
    if (ai->stalled) {
        ai->stalled = false;
        show_text(siren, SLOT_NOTE, " ");
    }
    if (bento_time_ticks_diff(now_ms, ai->quiet_ms) >= 0) {
        const bool was = ai->alarm;

        (void)snprintf(ai->label, sizeof(ai->label), "%s",
                       (label[0] != '\0') ? label : "-");
        ai->conf = percent_of(result->score_pct[result->top]);
        alert_rule(ai, is_danger(ai->label) && (ai->conf >= CONF_MIN));
        if (ai->alarm != was) {
            status = follow_alarm(siren);
        }
        if (status == BENTO_OK) {
            show(siren);
        }
    }
    return status;
}

/* A new result is taken; none for STALL_MS says so on the screen.
 * edge_ai.result() raising OSError is the .py's None. */
static bento_status_t read_ai(siren_t *siren, uint32_t now_ms)
{
    verdict_t *ai = &siren->ai;
    bento_edgeai_result_t result;
    bento_status_t status = BENTO_OK;

    bento_edgeai_result_init(&result);
    if ((bento_edgeai_result(&result) == BENTO_OK) &&
        result.has_verdict &&
        (!ai->has_sequence || (result.seq != ai->sequence))) {
        status = take_result(siren, &result, now_ms);
    } else if (!ai->stalled &&
               (bento_time_ticks_diff(now_ms, ai->last_new_ms) >
                STALL_MS)) {
        ai->stalled = true;
        show_text(siren, SLOT_NOTE, "ผลไม่ขยับ ลองรีเซ็ตบอร์ด");
    } else {
        /* the same result as before */
    }
    return status;
}

/* One pass every TICK_MS: the AI, the report, the link. */
static bento_status_t run_pass(siren_t *siren)
{
    const uint32_t now_ms = bento_time_ticks_ms();
    bento_status_t status = read_ai(siren, now_ms);

    if (status == BENTO_OK) {
        report(siren, now_ms);
        status = keep_link(siren, now_ms);
    }
    if (status == BENTO_OK) {
        poll_display(siren->ui);
        status = wait_ms(TICK_MS);
    }
    return status;
}

static bento_status_t run_loop(siren_t *siren)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    siren->link.sent_ms = start_ms;
    siren->link.retry_ms = start_ms;
    siren->ai.last_new_ms = start_ms;
    siren->ai.quiet_ms = start_ms;
    (void)snprintf(siren->ai.label, sizeof(siren->ai.label), "-");
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = run_pass(siren);
    }
    return status;
}

/* Stopped halfway or not: the AI core is released, the LED goes
 * off, the matrix clears and the link closes. */
static bento_status_t finish(siren_t *siren, bento_status_t run_status)
{
    bento_status_t status;

    if (bento_edgeai_stop_start() == BENTO_OK) {
        (void)finish_op(); /* the .py ignores an OSError from stop() */
    }
    (void)bento_edgeai_deinit(); /* never fails: nothing is sent */
    status = set_led(siren, false);
    (void)bento_rgbmatrix_clear(); /* an OSError is ignored there */
    close_broker(&siren->link);
    status = (run_status != BENTO_OK) ? run_status : status;
    if (status == BENTO_OK) {
        show_text(siren, SLOT_NOTE,
                  "ครบเวลา - กด Program to Device เพื่อเล่นใหม่");
        poll_display(siren->ui);
    }
    return status;
}

/* Edge AI from C is not in the BENTO Emulator yet: one line on the
 * screen, clear of everything the .py still shows, and the Console. */
static bento_status_t show_emulator_gap(siren_t *siren)
{
    static const char gap[] = "Edge AI จาก C ยังไม่มีใน BENTO Emulator "
                              "ให้รันบน Dev Kit หรือ Eva Kit";
    static const part_t line = {KIND_LABEL, SLOT_NONE, gap,       180,
                                268,        20,        COLOR_WARN};
    const bento_status_t status = add_part(siren, &line);

    if (status == BENTO_OK) {
        poll_display(siren->ui);
        (void)printf("%s / Edge AI from C is not in the BENTO Emulator "
                     "yet: run it on a Dev Kit or Eva Kit.\n",
                     gap);
    }
    return status;
}

static void report_no_model(const siren_t *siren)
{
    show_text(siren, SLOT_MODEL, "ไม่พบโมเดล SirenDetection");
    show_color(siren, SLOT_NOTE, COLOR_BAD);
    show_text(siren, SLOT_NOTE,
              "ต้อง Deploy โมเดลจาก Edge AI Store ก่อน");
    poll_display(siren->ui);
    (void)bento_edgeai_deinit(); /* never fails: nothing is sent */
    (void)printf("ไม่พบโมเดล ('%s', '%s')\n", MODEL_KEYS[0],
                 MODEL_KEYS[1]);
}

/* The model found: the network, the LED, then the run. */
static bento_status_t run_with_model(siren_t *siren)
{
    char text[TEXT_BYTES];
    link_t *link = &siren->link;
    bento_status_t status = BENTO_OK;

    (void)snprintf(text, sizeof(text), "โมเดล: %s", siren->model.name);
    show_text(siren, SLOT_MODEL, text);
    (void)snprintf(link->topic_ai, sizeof(link->topic_ai),
                   "bento-aiot/%s/ai", TEAM);
    (void)snprintf(link->topic_event, sizeof(link->topic_event),
                   "bento-aiot/%s/event", TEAM);
    link->can_net = team_ok() && !(ssid_unset() && !wifi_connected());
    if (!link->can_net) {
        show_text(siren, SLOT_NOTE, "แก้ TEAM และ Hotspot ก่อน: ออฟไลน์");
    } else {
        status = go_online(link);
    }
    if (status == BENTO_OK) {
        find_led(siren);
        status = finish(siren, run_loop(siren));
    }
    return status;
}

int main(void)
{
    siren_t *siren = &g_siren;
    bool no_link = false;
    bento_status_t status;

    (void)memset(siren, 0, sizeof(*siren));
    siren->ui = bento_runtime_get_ui();
    status = bento_ui_sfx_volume_set(siren->ui, SPEAKER);
    if (status == BENTO_E_UNSUPPORTED) {
        status = BENTO_OK; /* no volume control: the .py's hasattr() */
    }
    if (status == BENTO_OK) {
        status = build_screen(siren);
    }
    if (status == BENTO_OK) {
        status = find_model(&siren->model, &no_link);
    }
    if ((status == BENTO_OK) && no_link) {
        status = show_emulator_gap(siren);
    }
    if ((status == BENTO_OK) && !siren->model.found) {
        report_no_model(siren);
    } else if (status == BENTO_OK) {
        status = run_with_model(siren);
    } else {
        /* Stop or a failure ends the run */
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
