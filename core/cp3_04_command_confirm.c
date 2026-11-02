/*
 * cp3_04_command_confirm.c -- หลักการ 3.4: ทุกคำสั่งต้องมีคำยืนยัน
 * และมีทางถอยที่ปลอดภัย (Every command needs a confirmation and a safe
 * way back)
 *
 * กด SW5 = ขอเปิดปั๊ม PUMP_SEC วินาที กด SW6 = ขอปิด บอร์ดส่งคำสั่งไป
 * plc/cmd แล้วรอคำยืนยันจาก plc/state ภายใน TIMEOUT_MS เงียบ = ส่งซ้ำ
 * RETRIES ครั้ง ยังเงียบ = สั่งปิดแล้วถือว่า "ไม่รู้" สถานะปั๊มบนจอมาจาก
 * plc/state เท่านั้น เปิด PLC Simulator ใน s2/app/farm_web.html ก่อน
 * แก้ WIFI_SSID WIFI_PASS และ TEAM ก่อนรัน เล่น 5 นาทีแล้วจบเอง
 *
 * SW5 asks the PLC to run the pump for PUMP_SEC seconds, SW6 asks it to
 * stop. The board sends the command to plc/cmd and waits TIMEOUT_MS for
 * the PLC to confirm it on plc/state. Silence means one more send
 * (RETRIES), and silence after that means: order the pump off and call
 * its state unknown. The pump on screen comes only from plc/state.
 * Start the PLC Simulator in s2/app/farm_web.html first, and set
 * WIFI_SSID, WIFI_PASS and TEAM before running. The program ends by
 * itself after five minutes.
 *
 * Twin of cp3_04_command_confirm.py
 * (aiot-development-for-smart-farm/core/cp3_04_command_confirm.py).
 * Style: CLEAN_C.md.
 * Differs from the original:
 *   - There is no json module in C. Outgoing commands are written with
 *     snprintf, keys in the order the .py writes them (MicroPython's
 *     dict may print them in another order; a JSON reader does not
 *     care).
 *   - Incoming plc/state is read by a small field finder: one JSON
 *     object of "key": value members, of which it keeps "pump",
 *     "left_s" and "why" (the last equal key wins). A nested object or
 *     list is only checked for matching brackets. Anything else, an
 *     empty object, or a message over PAYLOAD_KEEP bytes counts as no
 *     message, as the .py's except does.
 *   - Strings are compared and shown as their raw bytes: escapes such
 *     as \u are not decoded. A real number such as 1.0 does not count
 *     as equal to 1 (true and false count as 1 and 0, as in Python).
 *     A shown value is cut to VALUE_BYTES - 1 bytes.
 *   - get_message() is one slot a newer message overwrites; the C
 *     client keeps a ring, so each round takes every waiting message
 *     and keeps the newest.
 *   - A broker connect that fails is closed before the next try; the
 *     .py's next mqtt.connect() cleans it up the same way.
 *   - When the link cannot be made the client is closed silently at
 *     the end; the .py leaves that to the soft reset.
 *   - ui.volume has no hasattr test: a board without it answers
 *     BENTO_E_UNSUPPORTED, which is skipped as the .py skips the line.
 *   - wifi.* and mqtt.* print console lines of their own; C prints
 *     none of them.
 *   - The tune table keeps only the tunes this program plays.
 *   - In the BENTO Emulator, Edge AI/MQTT from C is not available yet:
 *     the program says so and ends.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_buttons.h"
#include "bento_mqtt.h"
#include "bento_runtime.h"
#include "bento_time.h"
#include "bento_wifi.h"

/* ---- แก้ก่อนรัน: settings (MISRA deviation CT02) ---- */
/* Set these before running. TEAM must match the team opened in
 * farm_web.html. */
static const char WIFI_SSID[] = "<ชื่อ Hotspot ของกลุ่ม>";
static const char WIFI_PASS[] = "<รหัส Hotspot ของกลุ่ม>";
static const char TEAM[] = "teamXX";
enum {
    PUMP_SEC = 10, /* SW5 asks for this many seconds (PLC cuts at 30) */
    TIMEOUT_MS = 3000, /* wait this long for a confirmation, per send */
    RETRIES = 1,       /* sends again this often before giving up */
    SAMPLE_MS = 20,    /* read the buttons and the inbox this often */
    RUN_MS = 300000,   /* the program ends after five minutes */
    SPEAKER = 40,      /* speaker volume, 0-100 % */
    VOLUME = 25        /* tone velocity, 0-127 */
};
/* the same broker farm_web.html connects to (HiveMQ) */
static const char BROKER[] = "broker.hivemq.com";
/* CLIENT_ID = bento-cmd-TEAM (+ the clock), built in open_broker() */
/* T_CMD = bento-aiot/TEAM/plc/cmd, built in main() */
/* T_STATE = bento-aiot/TEAM/plc/state, built in main() */
/* ---- end of settings ---- */

/* The network. */
enum {
    PORT = 1883,      /* MQTT, not encrypted */
    KEEPALIVE_S = 60, /* silent longer, the broker may drop us */
    BROKER_TRIES =
        3, /* some public brokers miss an answer now and then */
    WIFI_SLICE_MS = 50,
    MQTT_SLICE_MS = 20,
    CLIENT_TAIL_MASK = 0xFFFF, /* four hex digits from the clock */
    TEAM_PREFIX = 4,           /* "team" before the digits */
    TOPIC_BYTES = 48,          /* "bento-aiot/<TEAM>/plc/state" */
    CLIENT_ID_BYTES = 24,      /* 23 bytes and the NUL */
    PAYLOAD_BYTES = 32,        /* {"pump": 1, "sec": <int32_t>} */
    PAYLOAD_KEEP = 128,        /* a PLC state is far shorter */
    TOPIC_KEEP = 64            /* the one topic this program hears */
};

/* Commands and buttons. */
enum {
    WANT_NONE = -1, /* no command waits for its confirmation */
    PUMP_OFF = 0,
    PUMP_ON = 1,
    BUTTON_COUNT = 2, /* SW5 = buttons.pressed(1), SW6 = pressed(0) */
    STEP_COUNT = 4,
    DECIMAL_BASE = 10
};

/* Sound. */
enum {
    NOTE_MS = 90,
    NOTE_GAP_MS = 100,
    TUNE_NOTES_MAX = 3
};

typedef enum {
    TUNE_TAP = 0,
    TUNE_START,
    TUNE_GOOD,
    TUNE_BAD,
    TUNE_EMPTY
} tune_t;

/* Screen layout. */
enum {
    MARGIN_X = 12,
    TITLE_Y = 6,
    LINK_Y = 12,
    MQTT_X = 380,
    LINK_LED_X = 430,
    LINK_LED_SIZE = 20,
    LINK_X = 458,
    CARD_Y = 64,
    CARD_H = 274,
    STEPS_W = 440,
    PLC_X = 462,
    PLC_W = 318,
    CARD_TEXT_X = 12,
    CARD_TEXT_Y = 6,
    CARD_RADIUS = 12,
    STEP_X = 28,
    STEP_Y = 98,
    STEP_GAP = 44,
    RESULT_Y = 282,
    PUMP_X = 478,
    PUMP_Y = 104,
    NOTE_Y = 352,
    FONT_DEFAULT = 0,
    FONT_SMALL = 16, /* digits and Latin only */
    FONT_THAI = 20,  /* Thai is 20 px or larger on the board */
    FONT_TITLE = 24,
    SETTLE_MS = 200, /* let the cleared screen settle */
    /* the pump line with two VALUE_BYTES values (131 with its NUL);
     * a label shows at most 126 of it */
    TEXT_BYTES = 132,
    VALUE_BYTES = 48
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

/* What a JSON value is; JSON_MISSING: the key was not there. */
typedef enum {
    JSON_MISSING = 0,
    JSON_STRING,
    JSON_NUMBER,
    JSON_TRUE,
    JSON_FALSE,
    JSON_NULL,
    JSON_NESTED
} json_kind_t;

/* The fields of plc/state this program reads. */
typedef enum {
    FIELD_PUMP = 0,
    FIELD_LEFT_S,
    FIELD_WHY,
    FIELD_COUNT
} field_t;

typedef struct {
    int16_t x;
    int16_t y;
} point_t;

/* Where the Emulator's "no MQTT from C" line sits: on space that shows
 * nothing at that point of the run. */
enum {
    GAP_X = 476,
    GAP_Y = 140,
    GAP_W = 294 /* the line wraps inside its card */
};

typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
} rect_t;

/* A value's bytes in the message: start up to, not including, end. */
typedef struct {
    json_kind_t kind;
    uint32_t start;
    uint32_t end;
} json_span_t;

typedef struct {
    const uint8_t *text;
    uint32_t len;
    uint32_t offset;
} json_reader_t;

/* One message from plc/state and the fields found in it. */
typedef struct {
    uint8_t text[PAYLOAD_KEEP];
    uint32_t len;
    uint32_t members;
    json_span_t fields[FIELD_COUNT];
} plc_state_t;

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t led;
    bento_ui_id_t link;
    bento_ui_id_t steps[STEP_COUNT];
    bento_ui_id_t result;
    bento_ui_id_t pump;
    bento_ui_id_t note;
} confirm_screen_t;

typedef struct {
    confirm_screen_t screen;
    char topic_cmd[TOPIC_BYTES];
    char topic_state[TOPIC_BYTES];
    uint32_t broker;
    int32_t want;
    int32_t tries;
    uint32_t sent_ms;
    bool down[BUTTON_COUNT];
    bool online;
    bool no_mqtt; /* the BENTO Emulator: no MQTT from C yet */
} confirm_t;

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

/* One label: colour and text, then up on the screen. */
static void show(bento_ui_t *ui, bento_ui_id_t widget, const char *text,
                 uint32_t color)
{
    (void)bento_ui_set_color(ui, widget, color);
    set_text(ui, widget, text);
    present(ui);
}

/* Plays a tune note by note, 100 ms apart. */
static bento_status_t beep(bento_ui_t *ui, tune_t tune)
{
    static const uint8_t notes[][TUNE_NOTES_MAX] = {{76U, 0U, 0U},
                                                    {72U, 79U, 0U},
                                                    {72U, 79U, 84U},
                                                    {84U, 76U, 0U},
                                                    {84U, 76U, 69U}};
    static const uint32_t note_count[] = {1U, 2U, 3U, 2U, 3U};
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

/* ---- the JSON field finder ---- */

static bool is_space(uint8_t byte)
{
    return (byte == (uint8_t)' ') || (byte == (uint8_t)'\t') ||
           (byte == (uint8_t)'\n') || (byte == (uint8_t)'\r');
}

static bool is_digit(uint8_t byte)
{
    return (byte >= (uint8_t)'0') && (byte <= (uint8_t)'9');
}

static bool is_number_byte(uint8_t byte)
{
    return is_digit(byte) || (byte == (uint8_t)'.') ||
           (byte == (uint8_t)'e') || (byte == (uint8_t)'E') ||
           (byte == (uint8_t)'+') || (byte == (uint8_t)'-');
}

static bool json_at(const json_reader_t *reader, uint8_t byte)
{
    return (reader->offset < reader->len) &&
           (reader->text[reader->offset] == byte);
}

static void skip_space(json_reader_t *reader)
{
    while ((reader->offset < reader->len) &&
           is_space(reader->text[reader->offset])) {
        reader->offset++;
    }
}

/* A string, both quotes included; a backslash takes the next byte
 * with it. */
static bool skip_string(json_reader_t *reader)
{
    bool closed = false;

    reader->offset++;
    while (!closed && (reader->offset < reader->len)) {
        const uint8_t byte = reader->text[reader->offset];

        if (byte == (uint8_t)'\\') {
            reader->offset += 2U;
        } else {
            closed = byte == (uint8_t)'"';
            reader->offset++;
        }
    }
    return closed;
}

/* +1 for a bracket that opens, -1 for one that closes, else 0. */
static int32_t depth_change(uint8_t byte)
{
    int32_t change = 0;

    if ((byte == (uint8_t)'{') || (byte == (uint8_t)'[')) {
        change = 1;
    } else if ((byte == (uint8_t)'}') || (byte == (uint8_t)']')) {
        change = -1;
    } else {
        /* any other byte inside */
    }
    return change;
}

/* An object or a list: only its brackets are matched. */
static bool skip_nested(json_reader_t *reader)
{
    int32_t depth = 0;
    bool ok = true;

    do {
        const uint8_t byte = reader->text[reader->offset];

        if (byte == (uint8_t)'"') {
            ok = skip_string(reader);
        } else {
            depth += depth_change(byte);
            reader->offset++;
        }
    } while (ok && (depth != 0) && (reader->offset < reader->len));
    return ok && (depth == 0);
}

/* The same bytes as the word: a loop, since MISRA keeps memcmp off
 * text. */
static bool bytes_are(const uint8_t *text, const char *word,
                      uint32_t length)
{
    bool same = true;
    uint32_t i;

    for (i = 0U; same && (i < length); i++) {
        same = text[i] == (uint8_t)word[i];
    }
    return same;
}

static bool skip_word(json_reader_t *reader, const char *word)
{
    const uint32_t length = (uint32_t)strlen(word);
    bool same = (reader->len - reader->offset) >= length;

    if (same) {
        same = bytes_are(&reader->text[reader->offset], word, length);
    }
    if (same) {
        reader->offset += length;
    }
    return same;
}

/* A number: the run of number bytes, with at least one digit. */
static bool skip_number(json_reader_t *reader)
{
    bool has_digit = false;

    while ((reader->offset < reader->len) &&
           is_number_byte(reader->text[reader->offset])) {
        has_digit = has_digit || is_digit(reader->text[reader->offset]);
        reader->offset++;
    }
    return has_digit;
}

static bool skip_scalar(json_reader_t *reader, json_kind_t *kind)
{
    const uint8_t byte = reader->text[reader->offset];
    bool ok;

    if (byte == (uint8_t)'t') {
        *kind = JSON_TRUE;
        ok = skip_word(reader, "true");
    } else if (byte == (uint8_t)'f') {
        *kind = JSON_FALSE;
        ok = skip_word(reader, "false");
    } else if (byte == (uint8_t)'n') {
        *kind = JSON_NULL;
        ok = skip_word(reader, "null");
    } else {
        *kind = JSON_NUMBER;
        ok = ((byte == (uint8_t)'-') || is_digit(byte)) &&
             skip_number(reader);
    }
    return ok;
}

static bool read_value(json_reader_t *reader, json_span_t *span)
{
    bool ok;

    skip_space(reader);
    span->start = reader->offset;
    ok = reader->offset < reader->len;
    if (ok && json_at(reader, (uint8_t)'"')) {
        span->kind = JSON_STRING;
        ok = skip_string(reader);
    } else if (ok && (json_at(reader, (uint8_t)'{') ||
                      json_at(reader, (uint8_t)'['))) {
        span->kind = JSON_NESTED;
        ok = skip_nested(reader);
    } else if (ok) {
        ok = skip_scalar(reader, &span->kind);
    } else {
        /* the text ended where a value should be */
    }
    span->end = reader->offset;
    return ok;
}

/* Which field a key names, FIELD_COUNT for none. */
static int32_t field_named(const uint8_t *text, const json_span_t *key)
{
    static const char *const field_names[FIELD_COUNT] = {
        "pump", "left_s", "why"};
    const uint32_t length = key->end - key->start - 2U;
    int32_t found = (int32_t)FIELD_COUNT;
    int32_t i;

    for (i = 0; i < (int32_t)FIELD_COUNT; i++) {
        if ((length == (uint32_t)strlen(field_names[i])) &&
            bytes_are(&text[key->start + 1U], field_names[i], length)) {
            found = i;
        }
    }
    return found;
}

/* "key": value, keeping the value of a field the program reads. */
static bool read_member(json_reader_t *reader, plc_state_t *state)
{
    json_span_t key;
    json_span_t value;
    bool ok = read_value(reader, &key) && (key.kind == JSON_STRING);

    skip_space(reader);
    ok = ok && json_at(reader, (uint8_t)':');
    if (ok) {
        reader->offset++;
        ok = read_value(reader, &value);
    }
    if (ok) {
        const int32_t field = field_named(reader->text, &key);

        if (field < (int32_t)FIELD_COUNT) {
            state->fields[field] = value;
        }
        state->members++;
    }
    return ok;
}

static bool read_members(json_reader_t *reader, plc_state_t *state)
{
    bool ok = true;
    bool closed = false;

    skip_space(reader);
    if (json_at(reader, (uint8_t)'}')) {
        reader->offset++;
        closed = true;
    }
    while (ok && !closed) {
        ok = read_member(reader, state);
        skip_space(reader);
        closed = ok && json_at(reader, (uint8_t)'}');
        ok = ok && (closed || json_at(reader, (uint8_t)','));
        reader->offset++;
    }
    return ok;
}

/* The message as one JSON object; false when it is not one. */
static bool read_object(plc_state_t *state)
{
    json_reader_t reader = {state->text, state->len, 0U};
    bool ok;
    uint32_t i;

    state->members = 0U;
    for (i = 0U; i < (uint32_t)FIELD_COUNT; i++) {
        state->fields[i].kind = JSON_MISSING;
        state->fields[i].start = 0U;
        state->fields[i].end = 0U;
    }
    skip_space(&reader);
    ok = json_at(&reader, (uint8_t)'{');
    if (ok) {
        reader.offset++;
        ok = read_members(&reader, state);
    }
    skip_space(&reader);
    return ok && (reader.offset == reader.len);
}

/* An integer value: digits with an optional '-', or true / false. */
static bool value_int(const plc_state_t *state, field_t field,
                      int32_t *value)
{
    const json_span_t *span = &state->fields[field];
    const uint32_t digits_max = 9U; /* fits an int32_t */
    uint32_t i = span->start;
    bool negative;
    bool ok = (span->kind == JSON_NUMBER);

    *value = (span->kind == JSON_TRUE) ? 1 : 0;
    negative = ok && (state->text[i] == (uint8_t)'-');
    i += negative ? 1U : 0U;
    ok = ok && (i < span->end) && ((span->end - i) <= digits_max);
    while (ok && (i < span->end)) {
        ok = is_digit(state->text[i]);
        *value = (*value * DECIMAL_BASE) + (int32_t)state->text[i] -
                 (int32_t)'0';
        i++;
    }
    *value = negative ? -*value : *value;
    return ok || (span->kind == JSON_TRUE) ||
           (span->kind == JSON_FALSE);
}

static bool value_equals(const plc_state_t *state, field_t field,
                         int32_t wanted)
{
    int32_t value = 0;

    return value_int(state, field, &value) && (value == wanted);
}

/* A string value equal to the word, compared byte for byte. */
static bool value_is(const plc_state_t *state, field_t field,
                     const char *word)
{
    const json_span_t *span = &state->fields[field];
    const uint32_t length = (uint32_t)strlen(word);

    return (span->kind == JSON_STRING) &&
           ((span->end - span->start - 2U) == length) &&
           bytes_are(&state->text[span->start + 1U], word, length);
}

/* Copies bytes, cut to cap - 1 at a character boundary. */
static void copy_cut(char *out, uint32_t cap, const uint8_t *bytes,
                     uint32_t len)
{
    const uint8_t top_bits = 0xC0U;
    const uint8_t continuation = 0x80U;
    uint32_t length = (len < cap) ? len : (cap - 1U);

    while ((length < len) && (length > 0U) &&
           ((bytes[length] & top_bits) == continuation)) {
        length--;
    }
    (void)memcpy(out, bytes, length);
    out[length] = '\0';
}

/* str() of a value: a string without its quotes, a word as Python
 * writes it, anything else as its JSON text. */
static void value_text(const plc_state_t *state, field_t field,
                       char *out, uint32_t cap)
{
    static const char *const words[] = {"None", "",      "",
                                        "True", "False", "None"};
    const json_span_t *span = &state->fields[field];

    if (span->kind == JSON_STRING) {
        copy_cut(out, cap, &state->text[span->start + 1U],
                 span->end - span->start - 2U);
    } else if ((span->kind == JSON_NUMBER) ||
               (span->kind == JSON_NESTED)) {
        copy_cut(out, cap, &state->text[span->start],
                 span->end - span->start);
    } else {
        copy_cut(out, cap, (const uint8_t *)words[span->kind],
                 (uint32_t)strlen(words[span->kind]));
    }
}

/* ---- the network ---- */

static bool team_is_set(void)
{
    const char *digits = &TEAM[TEAM_PREFIX];
    bool good = digits[0] != '\0';
    uint32_t i;

    for (i = 0U; digits[i] != '\0'; i++) {
        good = good && is_digit((uint8_t)digits[i]);
    }
    return good;
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

/* wifi.connect() and an address other than 0.0.0.0. A join that
 * was refused or timed out is the .py's False; other failures end
 * the run, as its OSError does. */
static bento_status_t connect_wifi(bool *joined)
{
    bento_wifi_connect_params_t params;
    uint32_t address = 0U;
    bento_status_t status;

    bento_wifi_connect_params_init(&params);
    params.ssid = WIFI_SSID;
    params.ssid_len = (uint32_t)strlen(WIFI_SSID);
    params.pass = WIFI_PASS;
    params.pass_len = (uint32_t)strlen(WIFI_PASS);
    params.flags = BENTO_WIFI_F_SAVE_CREDS;
    status = bento_wifi_connect_start(&params);
    if (status == BENTO_OK) {
        status = wait_connect();
    }
    *joined =
        (status == BENTO_OK) &&
        (bento_wifi_ip(BENTO_WIFI_IF_ANY, &address) == BENTO_OK) &&
        (address != 0U);
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

/* One mqtt.connect() with a fresh client id from the board clock, so
 * an old session never clashes. Only Stop is an error. */
static bento_status_t open_broker(uint32_t *broker, bool *linked,
                                  bool *no_mqtt)
{
    char client_id[CLIENT_ID_BYTES];
    bento_mqtt_config_t config;
    bento_status_t status;
    bool started;

    (void)snprintf(client_id, sizeof(client_id),
                   "bento-cmd-%s-%04" PRIx32, TEAM,
                   bento_time_ticks_ms() & (uint32_t)CLIENT_TAIL_MASK);
    bento_mqtt_config_init(&config);
    config.host = BROKER;
    config.host_len = (uint32_t)strlen(BROKER);
    config.port = (uint16_t)PORT;
    config.keepalive_s = (uint16_t)KEEPALIVE_S;
    config.client_id = client_id;
    config.client_id_len = (uint32_t)strlen(client_id);
    status = bento_mqtt_open_start(&config, broker);
    started = status == BENTO_OK;
    if (started) {
        status = wait_broker(*broker);
    }
    *linked = status == BENTO_OK;
    *no_mqtt = status == BENTO_E_UNSUPPORTED;
    if (started && !*linked) {
        (void)bento_mqtt_close(*broker); /* frees it, whatever state */
        *broker = 0U;
    }
    return (status == BENTO_E_CANCELLED) ? status : BENTO_OK;
}

static bento_status_t connect_broker(confirm_t *app, bool *linked)
{
    char text[TEXT_BYTES];
    int32_t attempt;
    bento_status_t status = BENTO_OK;

    *linked = false;
    for (attempt = 1; (attempt <= BROKER_TRIES) && !*linked &&
                      !app->no_mqtt && (status == BENTO_OK);
         attempt++) {
        if (attempt > 1) {
            (void)snprintf(text, sizeof(text),
                           "ลองต่อ broker ใหม่ %" PRId32 "/3", attempt);
            show(app->screen.ui, app->screen.result, text, COL_WARN);
        }
        status = open_broker(&app->broker, linked, &app->no_mqtt);
    }
    return status;
}

static bool subscribe_state(const confirm_t *app)
{
    uint8_t granted = 0U;

    return bento_mqtt_subscribe(app->broker, app->topic_state,
                                (uint32_t)strlen(app->topic_state), 0U,
                                &granted) == BENTO_OK;
}

/* The BENTO Emulator's C runtime has no MQTT yet (every bento_mqtt_*
 * call answers BENTO_E_UNSUPPORTED): say so on screen and in the
 * console, in bento-ide's words. */
static bento_status_t show_emulator_gap(bento_ui_t *ui)
{
    static const char gap[] = "MQTT จาก C ยังไม่มีใน BENTO Emulator "
                              "ให้รันบน Dev Kit หรือ Eva Kit";
    /* the Emulator does not wrap a label: explicit breaks, three
     * lines of 110/233/241 px; the printf keeps the one-line text */
    static const char gap_lines[] =
        "MQTT จาก C\nยังไม่มีใน BENTO Emulator\n"
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

/* WiFi, an address, the broker and the one topic it listens to.
 * *problem stays NULL when every step passed. */
static bento_status_t connect_farm(confirm_t *app, const char **problem)
{
    bool passed = team_is_set();
    bento_status_t status = BENTO_OK;

    *problem = passed ? NULL : "แก้ TEAM ก่อน";
    if (passed) {
        /* the label must be up before the join blocks the screen */
        show(app->screen.ui, app->screen.result, "ต่อ WiFi...",
             COL_WARN);
        status = connect_wifi(&passed);
        *problem = passed ? NULL : "ต่อ WiFi ไม่ได้";
    }
    if ((status == BENTO_OK) && passed) {
        status = connect_broker(app, &passed);
    }
    /* then the .py's own way out: the broker did not answer */
    if ((status == BENTO_OK) && app->no_mqtt) {
        status = show_emulator_gap(app->screen.ui);
    }
    if ((status == BENTO_OK) && (*problem == NULL)) {
        passed = passed && subscribe_state(app);
        *problem = passed ? NULL : "broker ไม่ตอบ";
    }
    return status;
}

static bool link_is_up(uint32_t broker)
{
    uint32_t state = BENTO_MQTT_STATE_CLOSED;

    if (bento_mqtt_state(broker, &state) != BENTO_OK) {
        state = BENTO_MQTT_STATE_CLOSED;
    }
    return state == BENTO_MQTT_STATE_CONNECTED;
}

/* get_message() is one slot a newer message overwrites: take every
 * message waiting and keep the newest. */
static bool take_newest(uint32_t broker, plc_state_t *state)
{
    char topic[TOPIC_KEEP]; /* the program never reads the topic */
    bento_mqtt_msg_t info;
    bool found = false;
    bool more = true;

    while (more) {
        bento_mqtt_msg_init(&info);
        more = bento_mqtt_poll(broker, &info, topic, sizeof(topic),
                               state->text, PAYLOAD_KEEP) == BENTO_OK;
        if (more) {
            state->len = info.payload_len;
            found = info.truncated == 0U;
        }
    }
    return found;
}

/* A sent command that cannot leave is left to time out. */
static void send_command(const confirm_t *app, int32_t on)
{
    char payload[PAYLOAD_BYTES];

    if (on != 0) {
        (void)snprintf(payload, sizeof(payload),
                       "{\"pump\": 1, \"sec\": %" PRId32 "}",
                       (int32_t)PUMP_SEC);
    } else {
        (void)snprintf(payload, sizeof(payload), "{\"pump\": 0}");
    }
    /* a lost link makes publish fail: the timeout takes the way back */
    (void)bento_mqtt_publish(
        app->broker, app->topic_cmd, (uint32_t)strlen(app->topic_cmd),
        (const uint8_t *)payload, (uint32_t)strlen(payload), 0U, 0U);
}

/* ---- the screen ---- */

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

/* The title line: MQTT, its Led and the link word. */
static bento_status_t add_header(confirm_screen_t *screen)
{
    const point_t title_at = {MARGIN_X, TITLE_Y};
    const point_t mqtt_at = {MQTT_X, LINK_Y};
    const point_t link_at = {LINK_X, LINK_Y};
    bento_ui_widget_args_t args;
    bento_ui_id_t unused;
    bento_status_t status =
        add_label(screen->ui, "สั่ง -> ยืนยัน -> ปลอดภัย", title_at,
                  COL_TEXT, FONT_TITLE, &unused);

    if (status == BENTO_OK) {
        status = add_label(screen->ui, "MQTT", mqtt_at, COL_DIM,
                           FONT_SMALL, &unused);
    }
    bento_ui_widget_args_init(&args);
    args.x = LINK_LED_X;
    args.y = LINK_Y;
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

static bento_status_t add_steps(confirm_screen_t *screen)
{
    static const char *const steps[STEP_COUNT] = {
        "1 ส่ง plc/cmd", "2 รอ plc/state", "3 เงียบ: ส่งซ้ำ",
        "4 ยอมแพ้: สั่งปิด"};
    const rect_t box = {MARGIN_X, CARD_Y, STEPS_W, CARD_H};
    const point_t result_at = {STEP_X, RESULT_Y};
    bento_status_t status = add_card(screen->ui, box, "ขั้น");
    int32_t i;

    for (i = 0; (i < STEP_COUNT) && (status == BENTO_OK); i++) {
        const point_t at = {STEP_X, px(STEP_Y + (i * STEP_GAP))};

        status = add_label(screen->ui, steps[i], at, COL_DIM,
                           FONT_DEFAULT, &screen->steps[i]);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", result_at, COL_TEXT,
                           FONT_DEFAULT, &screen->result);
    }
    return status;
}

static bento_status_t add_plc(confirm_screen_t *screen)
{
    const rect_t box = {PLC_X, CARD_Y, PLC_W, CARD_H};
    const point_t pump_at = {PUMP_X, PUMP_Y};
    const point_t note_at = {MARGIN_X, NOTE_Y};
    char text[TEXT_BYTES];
    bento_status_t status = add_card(screen->ui, box, "PLC บอกว่า");

    if (status == BENTO_OK) {
        status = add_label(screen->ui, "--", pump_at, COL_DIM,
                           FONT_TITLE, &screen->pump);
    }
    (void)snprintf(text, sizeof(text),
                   "SW5 ขอเปิด %" PRId32 " วิ  SW6 ขอปิด",
                   (int32_t)PUMP_SEC);
    if (status == BENTO_OK) {
        status = add_label(screen->ui, text, note_at, COL_DIM,
                           FONT_THAI, &screen->note);
    }
    return status;
}

static bento_status_t build_screen(confirm_screen_t *screen)
{
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = sleep_ms((uint32_t)SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_header(screen);
    }
    if (status == BENTO_OK) {
        status = add_steps(screen);
    }
    if (status == BENTO_OK) {
        status = add_plc(screen);
    }
    if (status == BENTO_OK) {
        present(screen->ui);
    }
    return status;
}

/* The MQTT Led: lit = the broker is linked, dim = offline. */
static void show_link(const confirm_screen_t *screen, bool online)
{
    (void)bento_ui_set_value(screen->ui, screen->led, online ? 1 : 0);
    show(screen->ui, screen->link, online ? "เชื่อมต่อแล้ว" : "ออฟไลน์",
         online ? COL_OK : COL_DIM);
}

/* Step colours: grey = not yet, orange = now, green = passed, red =
 * failed. */
static void show_steps(const confirm_screen_t *screen,
                       const uint32_t colors[STEP_COUNT],
                       const char *text, uint32_t color)
{
    int32_t i;

    for (i = 0; i < STEP_COUNT; i++) {
        (void)bento_ui_set_color(screen->ui, screen->steps[i],
                                 colors[i]);
    }
    show(screen->ui, screen->result, text, color);
}

/* ---- the program ---- */

/* WANT_NONE: not an answer to the waiting command; PUMP_ON: the PLC
 * confirmed it; PUMP_OFF: the PLC answered that it will not. */
static int32_t judge(int32_t want, const plc_state_t *state)
{
    const bool switched = value_is(state, FIELD_WHY, "on") ||
                          value_is(state, FIELD_WHY, "off");
    const bool answered = switched ||
                          value_is(state, FIELD_WHY, "blocked_tank") ||
                          value_is(state, FIELD_WHY, "bad_cmd");
    int32_t verdict = WANT_NONE;

    if ((want != WANT_NONE) && answered) {
        verdict = (switched && value_equals(state, FIELD_PUMP, want))
                      ? PUMP_ON
                      : PUMP_OFF;
    }
    return verdict;
}

static void show_pump(const confirm_t *app, const plc_state_t *state)
{
    char why[VALUE_BYTES];
    char left[VALUE_BYTES];
    char text[TEXT_BYTES];
    const bool on = value_equals(state, FIELD_PUMP, PUMP_ON);

    value_text(state, FIELD_WHY, why, sizeof(why));
    value_text(state, FIELD_LEFT_S, left, sizeof(left));
    if (on) {
        (void)snprintf(text, sizeof(text), "เปิด อีก %s วิ  why=%s", left,
                       why);
    } else {
        (void)snprintf(text, sizeof(text), "ปิด  why=%s", why);
    }
    show(app->screen.ui, app->screen.pump, text,
         on ? COL_OK : COL_TEXT);
}

/* Every message from the PLC is the latest truth. */
static bento_status_t take_state(confirm_t *app,
                                 const plc_state_t *state)
{
    static const uint32_t refused[STEP_COUNT] = {COL_OK, COL_BAD,
                                                 COL_DIM, COL_DIM};
    char why[VALUE_BYTES];
    char text[TEXT_BYTES];
    const int32_t verdict = judge(app->want, state);
    bento_status_t status = BENTO_OK;

    show_pump(app, state);
    if (verdict == PUMP_ON) {
        const uint32_t confirmed[STEP_COUNT] = {
            COL_OK, COL_OK, (app->tries > 1) ? COL_OK : COL_DIM,
            COL_DIM};

        show_steps(&app->screen, confirmed, "ยืนยันแล้ว", COL_OK);
        app->want = WANT_NONE;
        status = beep(app->screen.ui, TUNE_GOOD);
    } else if (verdict == PUMP_OFF) {
        value_text(state, FIELD_WHY, why, sizeof(why));
        (void)snprintf(text, sizeof(text), "PLC ไม่ทำ: %s", why);
        show_steps(&app->screen, refused, text, COL_BAD);
        app->want = WANT_NONE;
        status = beep(app->screen.ui, TUNE_BAD);
    } else {
        /* not an answer: keep waiting */
    }
    return status;
}

/* Step 1: send, then step 2: wait. */
static bento_status_t start_command(confirm_t *app, int32_t want,
                                    uint32_t now)
{
    static const uint32_t sending[STEP_COUNT] = {COL_OK, COL_WARN,
                                                 COL_DIM, COL_DIM};

    app->want = want;
    app->tries = 1;
    app->sent_ms = now;
    send_command(app, want);
    show_steps(&app->screen, sending, "...", COL_WARN);
    return beep(app->screen.ui, TUNE_TAP);
}

/* A fresh press, with no command waiting, sends one. The buttons are
 * debounced at every read, so they are read every SAMPLE_MS. */
static bento_status_t read_buttons(confirm_t *app, uint32_t now)
{
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < (uint32_t)BUTTON_COUNT) && (status == BENTO_OK);
         i++) {
        bool down = false;

        status = bento_buttons_pressed(1U - i, &down);
        if ((status == BENTO_OK) && down && !app->down[i] &&
            (app->want == WANT_NONE)) {
            status = start_command(app, 1 - (int32_t)i, now);
        }
        app->down[i] = down;
    }
    return status;
}

/* Step 3: silence means one more send, never "it probably worked".
 * Step 4: give up and fall back to safe. */
static bento_status_t check_timeout(confirm_t *app, uint32_t now)
{
    static const uint32_t again[STEP_COUNT] = {COL_OK, COL_BAD,
                                               COL_WARN, COL_DIM};
    static const uint32_t given_up[STEP_COUNT] = {COL_OK, COL_BAD,
                                                  COL_BAD, COL_BAD};
    bento_status_t status = BENTO_OK;

    if ((app->want != WANT_NONE) &&
        (bento_time_ticks_diff(now, app->sent_ms) >= TIMEOUT_MS)) {
        if (app->tries <= RETRIES) {
            app->tries++;
            app->sent_ms = now;
            send_command(app, app->want);
            show_steps(&app->screen, again, "ส่งซ้ำ", COL_WARN);
            status = beep(app->screen.ui, TUNE_TAP);
        } else {
            send_command(app, PUMP_OFF);
            show(app->screen.ui, app->screen.pump, "ไม่รู้ = ถือว่าปิด",
                 COL_BAD);
            show_steps(&app->screen, given_up, "ยอมแพ้: สั่งปิด", COL_BAD);
            app->want = WANT_NONE;
            status = beep(app->screen.ui, TUNE_EMPTY);
        }
    }
    return status;
}

static bento_status_t run_round(confirm_t *app)
{
    plc_state_t state;
    uint32_t now;
    bento_status_t status;

    if (app->online && !link_is_up(app->broker)) {
        app->online = false; /* the Led dims, the loop goes on */
        show_link(&app->screen, false);
    }
    now = bento_time_ticks_ms();
    status = read_buttons(app, now);
    if ((status == BENTO_OK) && take_newest(app->broker, &state) &&
        read_object(&state) && (state.members != 0U)) {
        status = take_state(app, &state);
    }
    if (status == BENTO_OK) {
        status = check_timeout(app, now);
    }
    if (status == BENTO_OK) {
        status = sleep_ms((uint32_t)SAMPLE_MS);
    }
    return status;
}

static bento_status_t run_loop(confirm_t *app)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    app->sent_ms = start_ms;
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = run_round(app);
    }
    return status;
}

/* Linked: ready, the loop, then the broker is let go even after
 * Stop. */
static bento_status_t run_linked(confirm_t *app)
{
    bento_status_t status;

    show_link(&app->screen, true);
    show(app->screen.ui, app->screen.result, "พร้อม", COL_OK);
    status = beep(app->screen.ui, TUNE_START);
    if (status == BENTO_OK) {
        status = run_loop(app);
        (void)bento_mqtt_close(
            app->broker); /* frees it, whatever state */
        app->broker = 0U;
        show_link(&app->screen, false);
    }
    if (status == BENTO_OK) {
        show(app->screen.ui, app->screen.note,
             "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่",
             COL_WARN);
        status = beep(app->screen.ui, TUNE_GOOD);
    }
    return status;
}

int main(void)
{
    confirm_t app;
    const char *problem = NULL;
    bento_status_t status;

    (void)memset(&app, 0, sizeof(app));
    app.screen.ui = bento_runtime_get_ui();
    app.want = WANT_NONE;
    app.online = true;
    (void)snprintf(app.topic_cmd, sizeof(app.topic_cmd),
                   "bento-aiot/%s/plc/cmd", TEAM);
    (void)snprintf(app.topic_state, sizeof(app.topic_state),
                   "bento-aiot/%s/plc/state", TEAM);
    /* a board without ui.volume answers BENTO_E_UNSUPPORTED and is
     * skipped as the .py skips it; elsewhere it waits for no answer */
    (void)bento_ui_sfx_volume_set(app.screen.ui, SPEAKER);
    status = build_screen(&app.screen);
    if (status == BENTO_OK) {
        status = connect_farm(&app, &problem);
    }
    if ((status == BENTO_OK) && (problem != NULL)) {
        show(app.screen.ui, app.screen.result, problem, COL_BAD);
        status = beep(app.screen.ui, TUNE_BAD);
    } else if (status == BENTO_OK) {
        status = run_linked(&app);
    } else {
        /* Stop or a failure while connecting ends the run */
    }
    if (app.broker != 0U) { /* the client would outlive the program */
        (void)bento_mqtt_close(app.broker);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
