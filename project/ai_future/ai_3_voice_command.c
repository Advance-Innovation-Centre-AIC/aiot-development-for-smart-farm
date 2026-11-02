/*
 * ai_3_voice_command.c -- สั่งปั๊ม (จำลอง) ด้วยเสียง "go" / "stop"
 * แล้วกดปุ่มยืนยันก่อนทำจริง
 *
 * พูด "go" = ขอเปิดปั๊ม "stop" = ขอปิดปั๊ม บอร์ดไม่ทำทันที แต่ถามก่อน
 * กด SW6 (ปุ่มบน) ภายใน CONFIRM_MS = ยืนยัน SW5 (ปุ่มล่าง) = ยกเลิก
 * ไม่กด = ไม่ทำ ปั๊มในไฟล์นี้เป็นปั๊มจำลอง (ไฟบนจอ + จอไฟ RGB)
 * ไม่สั่งรีเลย์หรือ PLC จริง โมเดลชื่อบนบอร์ด "Voice Commands" (Edge
 * AI Store การ์ด VoiceCommands) ใช้แค่ go กับ stop คำอื่นแสดงบนจอ
 * เฉย ๆ คำนั้นต้องชนะอย่างน้อย K ใน N ผลล่าสุด ผล AI ส่งขึ้น
 * bento-aiot/<TEAM>/ai และ event voice_cmd ขึ้น bento-aiot/<TEAM>/event
 * แก้ WIFI_SSID WIFI_PASS และ TEAM ก่อนรัน (ยังไม่แก้ = ทำงานออฟไลน์
 * พิมพ์ผลลง Console แทน) ผล AI ใน BENTO Emulator เป็นผลจำลอง
 * ต้องทดสอบบนบอร์ดจริง
 *
 * Say "go" to ask for the pump on and "stop" to ask for it off. The
 * board does not act at once: it asks first. Press SW6 (the top
 * button) within CONFIRM_MS to confirm, SW5 (the bottom one) to
 * cancel; no press, nothing happens. The pump here is simulated (an
 * LED on the screen and the RGB matrix): no real relay or PLC. The
 * model is named "Voice Commands" on the board (the VoiceCommands
 * card in the Edge AI Store); only go and stop act, other words are
 * just shown. A word must win at least K of the last N results. AI
 * results go to bento-aiot/<TEAM>/ai and the voice_cmd event to
 * bento-aiot/<TEAM>/event. Set WIFI_SSID, WIFI_PASS and TEAM first;
 * unset, it runs offline and prints every message on the console. In
 * the BENTO Emulator the AI result is simulated: test on a real
 * board.
 *
 * Twin of ai_3_voice_command.py
 * (aiot-development-for-smart-farm/project/ai_future/ai_3_voice_command.py).
 * Style: CLEAN_C.md.
 * Differs from the original:
 *   - A board without the two buttons (bento_buttons_count() is 0)
 *     takes the .py's path for a failed "import buttons": no press
 *     is ever seen, so the pump never moves.
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

#include "bento_buttons.h"
#include "bento_edgeai.h"
#include "bento_fmt.h"
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

/* The name the board reports, not the card's name in the Store. */
static const char *const MODEL_KEYS[1] = {"Voice Commands"};

/* The two words that act, and what each one asks for. */
typedef enum {
    WORD_GO = 0,
    WORD_STOP,
    WORD_COUNT,
    WORD_NONE
} word_t;

static const char *const WORDS[] = {"go", "stop"};
static const char *const ASK[] = {"เปิดปั๊ม", "ปิดปั๊ม"};

/* The settings. */
enum {
    CONF_MIN = 60, /* below this many % a word is not counted */
    K = 2,         /* a word must win K of the last N results */
    N = 4,
    REFRACT_MS = 1000, /* after a word, no new word this long */
    CONFIRM_MS = 5000, /* the press may come this late */
    MUTE_MS = 800,     /* the board's own tone reaches the microphone */
    SW6 = 0,           /* button 0 is SW6 (top), 1 is SW5 (bottom) */
    SW5 = 1,
    HEARTBEAT_MS = 2000,
    GAP_MS = 200,
    RETRY_MS = 10000, /* a lost link is tried again this often */
    TICK_MS = 50,
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
    NOTE_LOW = 79, /* a confirmed command rises 79 -> 84 */
    NOTE_HIGH = 84,
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
    SLOT_WORD,
    SLOT_CONF,
    SLOT_ASK,
    SLOT_PUMP,
    SLOT_PUMP_TEXT,
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

/* How a question ended: done, cancelled, or no press in time. */
typedef enum {
    WHY_DONE = 0,
    WHY_CANCEL,
    WHY_UNCONFIRMED,
    WHY_OPEN /* still waiting */
} why_t;

/* The model that runs, with its labels. */
typedef struct {
    uint32_t index;
    bool found;
    uint32_t class_count;
    char name[BENTO_EDGEAI_NAME_LEN];
    char labels[BENTO_EDGEAI_MAX_CLASSES][BENTO_EDGEAI_LABEL_LEN];
} model_t;

/* True once per press, as sf3_08. */
typedef struct {
    uint32_t index;
    bool down;
    bool clicked;
} button_t;

/* The latest result, the last N confident words, and the question
 * waiting for a press. */
typedef struct {
    bool has_sequence;
    uint32_t sequence;
    char label[BENTO_EDGEAI_LABEL_LEN];
    int32_t conf;
    word_t history[N];
    uint32_t history_count;
    word_t want;
    bool pump;
    uint32_t word_ms;
    uint32_t ask_ms;
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
    uint32_t sent_ms;
    uint32_t retry_ms;
} link_t;

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t ids[SLOT_COUNT];
    model_t model;
    bool has_buttons;
    button_t sw6;
    button_t sw5;
    verdict_t ai;
    link_t link;
    char payload[PAYLOAD_BYTES];
} voice_t;

/* Too big for the 8 KB program stack: file scope, zeroed by the
 * loader and set again at the start of main() (CLEAN_C.md C9,
 * deviation BENTO-MISRA-E03/E04). */
extern voice_t g_voice;
voice_t g_voice;

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

static void show_text(const voice_t *voice, slot_t slot,
                      const char *text)
{
    /* MicroPython's widget setters ignore a refusal (modui.c) */
    (void)bento_ui_set_text(voice->ui, voice->ids[slot], text,
                            (uint32_t)strlen(text));
}

static void show_color(const voice_t *voice, slot_t slot,
                       uint32_t color)
{
    (void)bento_ui_set_color(voice->ui, voice->ids[slot], color);
}

/* Two notes of TONE_MS each, as the .py's beep(). */
static bento_status_t beep(const voice_t *voice, uint8_t first,
                           uint8_t second)
{
    const uint8_t notes[] = {first, second};
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < 2U) && (status == BENTO_OK); i++) {
        /* a tone's result is not read in MicroPython either */
        (void)bento_ui_sfx_tone(voice->ui, notes[i], BENTO_UI_WAVE_SINE,
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

static void show_sent(const voice_t *voice)
{
    const link_t *link = &voice->link;
    char text[TEXT_BYTES];

    (void)snprintf(
        text, sizeof(text), "ส่งได้ %" PRId32 " จาก %" PRId32 " ข้อความ%s",
        link->delivered, link->sent, link->online ? "" : " (ออฟไลน์)");
    show_text(voice, SLOT_SENT, text);
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

static bento_status_t add_part(voice_t *voice, const part_t *part)
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
        status = bento_ui_label_create(voice->ui, &args, &made);
    } else {
        args.w = part->size;
        args.h = part->size;
        args.font_px = 0;
        if (part->kind == KIND_ARC) {
            args.max_val = PERCENT;
            status = bento_ui_arc_create(voice->ui, &args, &made);
        } else {
            status = bento_ui_led_create(voice->ui, &args, &made);
        }
    }
    if (part->slot != SLOT_NONE) {
        voice->ids[part->slot] = made;
    }
    return status;
}

static bento_status_t build_screen(voice_t *voice)
{
    static const part_t parts[] = {
        {KIND_LABEL, SLOT_NONE, "สั่งปั๊มด้วยเสียง (Edge AI)", 12, 6, 24,
         COLOR_TEXT},
        {KIND_LABEL, SLOT_MODEL, "หาโมเดล...", 12, 44, 20, COLOR_INFO},
        {KIND_LABEL, SLOT_NONE, "พูด go = เปิดปั๊ม   stop = ปิดปั๊ม", 12, 76,
         20, COLOR_DIM},
        {KIND_ARC, SLOT_ARC, "", 12, 110, 150, 0U},
        {KIND_LABEL, SLOT_WORD, "-", 180, 120, 28, COLOR_TEXT},
        {KIND_LABEL, SLOT_CONF, "มั่นใจ - %", 180, 164, 20, COLOR_DIM},
        {KIND_LABEL, SLOT_ASK, " ", 180, 200, 20, COLOR_WARN},
        {KIND_LED, SLOT_PUMP, "", 12, 290, 40, COLOR_OK},
        {KIND_LABEL, SLOT_PUMP_TEXT, "ปั๊ม (จำลอง): ปิด", 64, 298, 20,
         COLOR_TEXT},
        {KIND_LED, SLOT_MQTT, "", 12, 350, 24, COLOR_OK},
        {KIND_LABEL, SLOT_SENT, "ยังไม่ได้ส่ง", 44, 352, 20, COLOR_DIM},
        {KIND_LABEL, SLOT_NOTE, " ", 12, 400, 20, COLOR_WARN}};
    const uint32_t count = (uint32_t)(sizeof(parts) / sizeof(parts[0]));
    bento_status_t status = bento_ui_screen_default(voice->ui);
    uint32_t i;

    if (status == BENTO_OK) {
        status = wait_ms(SETTLE_MS);
    }
    for (i = 0U; (i < count) && (status == BENTO_OK); i++) {
        status = add_part(voice, &parts[i]);
    }
    if (status == BENTO_OK) {
        poll_display(voice->ui);
    }
    return status;
}

/* The simulated pump: the LED, its text and the matrix. */
static void show_pump(const voice_t *voice)
{
    const bool pump = voice->ai.pump;

    (void)bento_ui_set_value(voice->ui, voice->ids[SLOT_PUMP],
                             pump ? 1 : 0);
    show_text(voice, SLOT_PUMP_TEXT,
              pump ? "ปั๊ม (จำลอง): เปิด" : "ปั๊ม (จำลอง): ปิด");
    matrix_scroll(pump ? "PUMP ON" : "PUMP OFF",
                  pump ? BENTO_RGBMATRIX_GREEN : BENTO_RGBMATRIX_RED);
}

/* A new edge since the last sample; a board without the buttons
 * never sees one. */
static bento_status_t sample(const voice_t *voice, button_t *button)
{
    bool now_down = false;
    bento_status_t status = BENTO_OK;

    if (voice->has_buttons) {
        status = bento_buttons_pressed(button->index, &now_down);
    }
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

/* ---- the brain ---- */

static word_t word_of(const char *label)
{
    word_t word = WORD_NONE;
    uint32_t i;

    for (i = 0U; i < (uint32_t)WORD_COUNT; i++) {
        if ((word == WORD_NONE) && (strcmp(label, WORDS[i]) == 0)) {
            word = (word_t)i;
        }
    }
    return word;
}

/* The word in WORDS that won at least K of the last N results. */
static word_t heard(const verdict_t *ai)
{
    word_t found = WORD_NONE;
    uint32_t word;

    for (word = 0U;
         (word < (uint32_t)WORD_COUNT) && (found == WORD_NONE);
         word++) {
        uint32_t hits = 0U;
        uint32_t i;

        for (i = 0U; i < ai->history_count; i++) {
            hits += (ai->history[i] == (word_t)word) ? 1U : 0U;
        }
        if (hits >= (uint32_t)K) {
            found = (word_t)word;
        }
    }
    return found;
}

static void remember(verdict_t *ai, word_t word)
{
    if (ai->history_count == (uint32_t)N) {
        (void)memmove(&ai->history[0], &ai->history[1],
                      sizeof(ai->history) - sizeof(ai->history[0]));
        ai->history_count--;
    }
    ai->history[ai->history_count] = word;
    ai->history_count++;
}

/* A word heard: ask first, do nothing yet. */
static void ask(voice_t *voice, word_t word, uint32_t now_ms)
{
    static const char *const prompts[] = {"GO?", "STOP?"};
    verdict_t *ai = &voice->ai;
    char text[TEXT_BYTES];

    ai->want = word;
    ai->ask_ms = now_ms;
    ai->word_ms = now_ms;
    ai->history_count = 0U;
    (void)snprintf(text, sizeof(text),
                   "ได้ยิน %s: กด SW6 เพื่อ%s (SW5 ยกเลิก)", WORDS[word],
                   ASK[word]);
    show_text(voice, SLOT_ASK, text);
    matrix_scroll(prompts[word], BENTO_RGBMATRIX_BLUE);
}

/* A new result, unless the board's own tone may still be in the
 * microphone. */
static void take_result(voice_t *voice,
                        const bento_edgeai_result_t *result,
                        uint32_t now_ms)
{
    verdict_t *ai = &voice->ai;
    const char *label = label_of(&voice->model, result->top);
    char text[TEXT_BYTES];
    word_t word;

    ai->has_sequence = true;
    ai->sequence = result->seq;
    (void)snprintf(ai->label, sizeof(ai->label), "%s",
                   (label[0] != '\0') ? label : "-");
    ai->conf = percent_of(result->score_pct[result->top]);
    remember(ai,
             (ai->conf >= CONF_MIN) ? word_of(ai->label) : WORD_NONE);
    show_text(voice, SLOT_WORD, ai->label);
    (void)snprintf(text, sizeof(text), "มั่นใจ %" PRId32 " %%", ai->conf);
    show_text(voice, SLOT_CONF, text);
    (void)bento_ui_set_value(voice->ui, voice->ids[SLOT_ARC], ai->conf);
    word = heard(ai);
    if ((word != WORD_NONE) && (ai->want == WORD_NONE) &&
        (bento_time_ticks_diff(now_ms, ai->word_ms) >= REFRACT_MS)) {
        ask(voice, word, now_ms);
    }
}

/* edge_ai.result() raising OSError is the .py's None. */
static void listen(voice_t *voice, uint32_t now_ms)
{
    const verdict_t *ai = &voice->ai;
    bento_edgeai_result_t result;

    bento_edgeai_result_init(&result);
    if ((bento_edgeai_result(&result) == BENTO_OK) &&
        result.has_verdict &&
        (!ai->has_sequence || (result.seq != ai->sequence)) &&
        (bento_time_ticks_diff(now_ms, ai->quiet_ms) >= 0)) {
        take_result(voice, &result, now_ms);
    }
}

/* The question ends: the screen, the pump and the voice_cmd event
 * (contract item 3.11). */
static void close_question(voice_t *voice, why_t why)
{
    static const char *const why_texts[] = {"", "cancel",
                                            "unconfirmed"};
    verdict_t *ai = &voice->ai;
    char text[TEXT_BYTES];

    ai->quiet_ms = bento_time_ticks_add(bento_time_ticks_ms(), MUTE_MS);
    if (why == WHY_DONE) {
        (void)snprintf(text, sizeof(text), "ทำแล้ว: %s", ASK[ai->want]);
    } else {
        (void)snprintf(text, sizeof(text), "ไม่ได้ทำ: %s",
                       why_texts[why]);
        show_pump(voice);
    }
    show_text(voice, SLOT_ASK, text);
    (void)snprintf(voice->payload, sizeof(voice->payload),
                   "{\"id\": \"%s\", \"event\": \"voice_cmd\", \"m\": "
                   "\"voice\", \"cmd\": \"%s\", \"ok\": %d, "
                   "\"why\": \"%s\"}",
                   TEAM, WORDS[ai->want], (why == WHY_DONE) ? 1 : 0,
                   why_texts[why]);
    (void)send(&voice->link, voice->link.topic_event,
               voice->payload); /* an event is not counted */
    ai->want = WORD_NONE;
}

/* Waits for a person: SW6 does it, SW5 cancels, no press in
 * CONFIRM_MS does nothing. A press with no question is dropped. */
static bento_status_t confirm(voice_t *voice, uint32_t now_ms)
{
    verdict_t *ai = &voice->ai;
    why_t why = WHY_OPEN;
    bento_status_t status = BENTO_OK;

    if (ai->want == WORD_NONE) {
        (void)pressed_now(&voice->sw6);
        (void)pressed_now(&voice->sw5);
    } else if (pressed_now(&voice->sw6)) {
        ai->pump = ai->want == WORD_GO;
        why = WHY_DONE;
        show_pump(voice);
        status = beep(voice, NOTE_LOW, NOTE_HIGH);
    } else if (pressed_now(&voice->sw5)) {
        why = WHY_CANCEL;
    } else if (bento_time_ticks_diff(now_ms, ai->ask_ms) >=
               CONFIRM_MS) {
        why = WHY_UNCONFIRMED;
    } else {
        /* still waiting for a press */
    }
    if ((status == BENTO_OK) && (why != WHY_OPEN)) {
        close_question(voice, why);
    }
    return status;
}

/* The AI result to .../ai (contract item 3.9). */
static void report(voice_t *voice, uint32_t now_ms)
{
    link_t *link = &voice->link;
    const verdict_t *ai = &voice->ai;
    const bool changed =
        !link->has_key || (strcmp(link->key_label, ai->label) != 0);
    bool delivered;

    if (should_send(changed,
                    bento_time_ticks_diff(bento_time_ticks_ms(),
                                          link->sent_ms))) {
        link->sent++;
        (void)snprintf(voice->payload, sizeof(voice->payload),
                       "{\"id\": \"%s\", \"n\": %" PRId32
                       ", \"model\": \"%s\", \"label\": \"%s\", "
                       "\"conf\": %" PRId32 "}",
                       TEAM, link->sent, voice->model.name, ai->label,
                       ai->conf);
        delivered = send(link, link->topic_ai, voice->payload);
        link->delivered += delivered ? 1 : 0;
        link->sent_ms = bento_time_ticks_ms();
        link->has_key = true;
        (void)snprintf(link->key_label, sizeof(link->key_label), "%s",
                       ai->label);
        show_sent(voice);
        if (link->online && !delivered) {
            link->online = false;
            link->retry_ms = now_ms;
        }
    }
}

/* A lost broker goes offline; offline tries again every RETRY_MS. */
static bento_status_t keep_link(voice_t *voice, uint32_t now_ms)
{
    link_t *link = &voice->link;
    bento_status_t status = BENTO_OK;

    if (link->online && !broker_alive(link)) {
        link->online = false;
        link->retry_ms = now_ms;
    }
    if (link->can_net && !link->online &&
        (bento_time_ticks_diff(now_ms, link->retry_ms) >= RETRY_MS)) {
        status = go_online(link);
        link->retry_ms = bento_time_ticks_ms();
    }
    (void)bento_ui_set_value(voice->ui, voice->ids[SLOT_MQTT],
                             link->online ? 1 : 0);
    return status;
}

/* One pass every TICK_MS: the buttons, listening, the question, the
 * report and the link. */
static bento_status_t run_pass(voice_t *voice)
{
    const uint32_t now_ms = bento_time_ticks_ms();
    bento_status_t status = sample(voice, &voice->sw6);

    if (status == BENTO_OK) {
        status = sample(voice, &voice->sw5);
    }
    if (status == BENTO_OK) {
        listen(voice, now_ms);
        status = confirm(voice, now_ms);
    }
    if (status == BENTO_OK) {
        report(voice, now_ms);
        status = keep_link(voice, now_ms);
    }
    if (status == BENTO_OK) {
        poll_display(voice->ui);
        status = wait_ms(TICK_MS);
    }
    return status;
}

static bento_status_t run_loop(voice_t *voice)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    voice->sw6.index = SW6;
    voice->sw5.index = SW5;
    voice->ai.want = WORD_NONE;
    voice->link.sent_ms = start_ms;
    voice->link.retry_ms = start_ms;
    voice->ai.word_ms = start_ms;
    voice->ai.ask_ms = start_ms;
    voice->ai.quiet_ms = start_ms;
    (void)snprintf(voice->ai.label, sizeof(voice->ai.label), "-");
    show_pump(voice);
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = run_pass(voice);
    }
    return status;
}

/* Stopped halfway or not: the AI core is released, the matrix
 * clears and the link closes. */
static bento_status_t finish(voice_t *voice, bento_status_t status)
{
    if (bento_edgeai_stop_start() == BENTO_OK) {
        (void)finish_op(); /* the .py ignores an OSError from stop() */
    }
    (void)bento_edgeai_deinit();   /* never fails: nothing is sent */
    (void)bento_rgbmatrix_clear(); /* an OSError is ignored there */
    close_broker(&voice->link);
    if (status == BENTO_OK) {
        show_text(voice, SLOT_NOTE,
                  "ครบเวลา - กด Program to Device เพื่อเล่นใหม่");
        poll_display(voice->ui);
    }
    return status;
}

/* Edge AI from C is not in the BENTO Emulator yet: one line on the
 * screen, clear of everything the .py still shows, and the Console. */
static bento_status_t show_emulator_gap(voice_t *voice)
{
    static const char gap[] = "Edge AI จาก C ยังไม่มีใน BENTO Emulator "
                              "ให้รันบน Dev Kit หรือ Eva Kit";
    static const part_t line = {KIND_LABEL, SLOT_NONE, gap,       180,
                                240,        20,        COLOR_WARN};
    const bento_status_t status = add_part(voice, &line);

    if (status == BENTO_OK) {
        poll_display(voice->ui);
        (void)printf("%s / Edge AI from C is not in the BENTO Emulator "
                     "yet: run it on a Dev Kit or Eva Kit.\n",
                     gap);
    }
    return status;
}

static void report_no_model(const voice_t *voice)
{
    show_text(voice, SLOT_MODEL, "ไม่พบโมเดล Voice Commands");
    show_color(voice, SLOT_NOTE, COLOR_BAD);
    show_text(voice, SLOT_NOTE,
              "ต้อง Deploy โมเดลจาก Edge AI Store ก่อน");
    poll_display(voice->ui);
    (void)bento_edgeai_deinit(); /* never fails: nothing is sent */
    (void)printf("ไม่พบโมเดล ('%s',)\n", MODEL_KEYS[0]);
}

/* The model found: the buttons, the network, then the run. */
static bento_status_t run_with_model(voice_t *voice)
{
    char text[TEXT_BYTES];
    link_t *link = &voice->link;
    bento_status_t status = BENTO_OK;

    (void)snprintf(text, sizeof(text), "โมเดล: %s", voice->model.name);
    show_text(voice, SLOT_MODEL, text);
    voice->has_buttons = bento_buttons_count() > 0U;
    if (!voice->has_buttons) {
        show_text(voice, SLOT_NOTE, "ไม่มีปุ่ม SW6: ปั๊มจะไม่ขยับ");
    }
    (void)snprintf(link->topic_ai, sizeof(link->topic_ai),
                   "bento-aiot/%s/ai", TEAM);
    (void)snprintf(link->topic_event, sizeof(link->topic_event),
                   "bento-aiot/%s/event", TEAM);
    link->can_net = team_ok() && !(ssid_unset() && !wifi_connected());
    if (link->can_net) {
        status = go_online(link);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_value(voice->ui, voice->ids[SLOT_MQTT],
                                 link->online ? 1 : 0);
        status = finish(voice, run_loop(voice));
    }
    return status;
}

int main(void)
{
    voice_t *voice = &g_voice;
    bool no_link = false;
    bento_status_t status;

    (void)memset(voice, 0, sizeof(*voice));
    voice->ui = bento_runtime_get_ui();
    status = bento_ui_sfx_volume_set(voice->ui, SPEAKER);
    if (status == BENTO_E_UNSUPPORTED) {
        status = BENTO_OK; /* no volume control: the .py's hasattr() */
    }
    if (status == BENTO_OK) {
        status = build_screen(voice);
    }
    if (status == BENTO_OK) {
        status = find_model(&voice->model, &no_link);
    }
    if ((status == BENTO_OK) && no_link) {
        status = show_emulator_gap(voice);
    }
    if ((status == BENTO_OK) && !voice->model.found) {
        report_no_model(voice);
    } else if (status == BENTO_OK) {
        status = run_with_model(voice);
    } else {
        /* Stop or a failure ends the run */
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
