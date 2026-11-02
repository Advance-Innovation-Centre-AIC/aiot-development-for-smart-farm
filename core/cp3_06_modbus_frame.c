/*
 * cp3_06_modbus_frame.c -- หลักการ 3.6: กรอบ Modbus TCP ของจริง 12 ไบต์
 * (A real 12-byte Modbus TCP frame, built on the board)
 *
 * กด SW5 (ปุ่มล่าง) = FC03 ขออ่าน HR0-HR2 กด SW6 (ปุ่มบน) = FC06 สลับปั๊ม
 * HR0 จอโชว์ 12 ไบต์ของคำขอ สีฟ้า = MBAP สีเขียว = PDU บอร์ดไม่มี Modbus
 * และ socket จึงส่งกรอบเป็น hex ขึ้น MQTT ให้ gateway (หน้าเว็บ
 * core/app/modbus_plc.html) แล้วรอคำตอบที่มี n ตรงกันภายใน REPLY_MS
 * ไม่มีคำตอบ = "ไม่รู้" ไม่เดาว่าสำเร็จ ยังไม่ตั้ง WiFi = โหมดดูกรอบ
 * แก้ WIFI_SSID WIFI_PASS และ TEAM ก่อนรัน เล่น 5 นาทีแล้วจบเอง
 *
 * SW5 (the lower button) sends FC03, read HR0 to HR2; SW6 (the upper
 * one) sends FC06, toggle the pump in HR0. The screen shows the 12
 * bytes of the request, blue for the MBAP header and green for the PDU.
 * The board has no Modbus and no socket, so it sends the frame as hex
 * over MQTT to a gateway (the page core/app/modbus_plc.html) and waits
 * REPLY_MS for the answer with the same n. No answer means unknown,
 * never success. Without WiFi settings the program only shows frames.
 * Set WIFI_SSID, WIFI_PASS and TEAM before running. The program ends by
 * itself after five minutes.
 *
 * Twin of cp3_06_modbus_frame.py
 * (aiot-development-for-smart-farm/core/cp3_06_modbus_frame.py).
 * Style: CLEAN_C.md.
 * Differs from the original:
 *   - There is no struct module in C: the frame is written byte by
 *     byte, big-endian, as struct.pack(">HHHBBHH") lays it out. A
 *     value outside 0-65535 is packed as its low 16 bits, where
 *     struct.pack raises.
 *   - There is no json module in C. The request is written with
 *     snprintf, keys in the order the .py adds them (MicroPython's dict
 *     may print them in another order; a JSON reader does not care).
 *   - The answer is read by a small field finder: one JSON object of
 *     "key": value members, of which it keeps "n", "ok", "hex", "v",
 *     "exc" and "err" (the last equal key wins). "v" must be a list of
 *     integers, of which the first three are kept. A nested value is
 *     otherwise only checked for matching brackets. Anything else, or
 *     a message over PAYLOAD_KEEP bytes, counts as no answer, as the
 *     .py's except does.
 *   - Strings are shown as their raw bytes: escapes such as \u are not
 *     decoded. A real number such as 1.0 does not count as equal to 1
 *     (true and false count as 1 and 0, as in Python). A shown value
 *     is cut to VALUE_BYTES - 1 bytes.
 *   - After a confirmed answer with HR0 still unknown, 1 - "--" raises
 *     TypeError in the .py; here the next SW6 asks for off.
 *   - get_message() is one slot a newer message overwrites; the C
 *     client keeps a ring, so each round takes every waiting message
 *     and keeps the newest.
 *   - A broker connect that fails is closed at once; the .py leaves it
 *     to the next mqtt.connect() or the soft reset.
 *   - ui.volume has no hasattr test: a board without it answers
 *     BENTO_E_UNSUPPORTED, which is skipped as the .py skips the line.
 *   - wifi.* and mqtt.* print console lines of their own; C prints
 *     none of them.
 *   - RESP_MS is named REPLY_MS here (CLEAN_C.md C3 bars "resp").
 *   - The tune table keeps only the tunes this program plays.
 *   - In the BENTO Emulator, Edge AI/MQTT from C is not available yet:
 *     the program says so and runs offline, as the .py does.
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
/* Set these before running. Left as "<...>", the program only shows
 * frames. TEAM must match the team in modbus_plc.html. */
static const char WIFI_SSID[] = "<ชื่อ Hotspot ของกลุ่ม>";
static const char WIFI_PASS[] = "<รหัส Hotspot ของกลุ่ม>";
static const char TEAM[] = "teamXX";
enum {
    UNIT_ID = 1,     /* the PLC's unit id in the MBAP header */
    REPLY_MS = 3000, /* .py: RESP_MS; wait this long for the gateway */
    SAMPLE_MS = 20,  /* read the buttons and the inbox this often */
    RUN_MS = 300000, /* the program ends after five minutes */
    SPEAKER = 40,    /* speaker volume, 0-100 % */
    VOLUME = 25      /* tone velocity, 0-127 (about 20 %) */
};
/* the broker chosen in modbus_plc.html */
static const char BROKER[] = "broker.hivemq.com";
/* CLIENT_ID = bento-mb-TEAM-<clock>, built in main() */
/* T_REQ = bento-aiot/TEAM/modbus/req, built in main() */
/* T_RESP = bento-aiot/TEAM/modbus/resp, built in main() */
/* ---- end of settings ---- */

/* The network. */
enum {
    PORT = 1883,      /* MQTT, not encrypted */
    KEEPALIVE_S = 60, /* silent longer, the broker may drop us */
    WIFI_SLICE_MS = 50,
    MQTT_SLICE_MS = 20,
    CLIENT_TAIL_MASK = 0xFFFF, /* four hex digits from the clock */
    TEAM_PREFIX = 4,           /* "team" before the digits */
    TOPIC_BYTES = 48,          /* "bento-aiot/<TEAM>/modbus/resp" */
    CLIENT_ID_BYTES = 24,      /* 23 bytes and the NUL */
    PAYLOAD_BYTES = 128,       /* the request with any int32_t n */
    PAYLOAD_KEEP = 128,        /* the gateway's answers are shorter */
    TOPIC_KEEP = 64            /* the one topic this program hears */
};

/* The Modbus frame. */
enum {
    FRAME_BYTES = 12, /* MBAP 7 + PDU 5 */
    MBAP_BYTES = 7,
    HEX_BYTES = 25, /* two hex digits per byte and the NUL */
    WORD_MASK = 0xFFFF,
    BYTE_MASK = 0xFF,
    BYTE_BITS = 8,
    FOLLOWING = 6, /* bytes after the len field: unit 1 + PDU 5 */
    FC_READ = 3,
    FC_WRITE = 6,
    READ_COUNT = 3, /* SW5 reads HR0, HR1 and HR2 */
    AT_PROTOCOL = 2,
    AT_LENGTH = 4,
    AT_UNIT = 6,
    AT_CODE = 7,
    AT_ADDRESS = 8,
    AT_VALUE = 10,
    REGISTER_COUNT = 3,
    BUTTON_COUNT = 2, /* SW5 = buttons.pressed(1), SW6 = pressed(0) */
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
    TUNE_BAD
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
    WARNING_Y = 38,
    CARD_W = 768,
    FRAME_CARD_Y = 62,
    FRAME_CARD_H = 112,
    ANSWER_CARD_Y = 186,
    ANSWER_CARD_H = 152,
    CARD_TEXT_X = 12,
    CARD_TEXT_Y = 6,
    CARD_RADIUS = 12,
    BYTE_X = 28,
    BYTE_STEP = 62,
    BYTE_Y = 96,
    FIELD_NAME_Y = 136,
    RESULT_X = 28,
    RESULT_Y = 224,
    REPLY_HEX_Y = 270,
    NOTE_Y = 352,
    FONT_DEFAULT = 0,
    FONT_TINY = 14,
    FONT_SMALL = 16, /* digits and Latin only */
    FONT_THAI = 20,  /* Thai is 20 px or larger on the board */
    FONT_TITLE = 24,
    FONT_BYTE = 28,
    SETTLE_MS = 200,     /* let the cleared screen settle */
    BYTE_TEXT_BYTES = 4, /* "%02X" of one byte */
    REGISTER_BYTES = 12, /* "--" or any int32_t */
    TEXT_BYTES = 128,
    VALUE_BYTES = 64,
    /* "PLC ปฏิเสธ " and a whole value (87 with its NUL) */
    ANSWER_BYTES = 88
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

/* The fields of an answer this program reads. */
typedef enum {
    FIELD_N = 0,
    FIELD_OK,
    FIELD_HEX,
    FIELD_V,
    FIELD_EXC,
    FIELD_ERROR,
    FIELD_COUNT
} field_t;

typedef struct {
    int16_t x;
    int16_t y;
} point_t;

/* Where the Emulator's "no MQTT from C" line sits: on space that shows
 * nothing at that point of the run. */
enum {
    GAP_X = 28,
    GAP_Y = 300,
    GAP_W = -1 /* one line */
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

/* One message from modbus/resp and the fields found in it. */
typedef struct {
    uint8_t text[PAYLOAD_KEEP];
    uint32_t len;
    uint32_t members;
    json_span_t fields[FIELD_COUNT];
} reply_t;

/* HR0 to HR2 as the PLC last confirmed them; unknown shows "--". */
typedef struct {
    bool known[REGISTER_COUNT];
    int32_t value[REGISTER_COUNT];
} registers_t;

/* An answer, from the gateway or from the timeout, ready to show. */
typedef struct {
    bool ok;
    char text[ANSWER_BYTES];
    char hex[VALUE_BYTES];
} answer_t;

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t led;
    bento_ui_id_t link;
    bento_ui_id_t bytes[FRAME_BYTES];
    bento_ui_id_t result;
    bento_ui_id_t reply_hex;
    bento_ui_id_t note;
} modbus_screen_t;

typedef struct {
    modbus_screen_t screen;
    char client_id[CLIENT_ID_BYTES];
    char topic_request[TOPIC_BYTES];
    char topic_reply[TOPIC_BYTES];
    uint32_t broker;
    registers_t registers;
    int32_t want;    /* what SW6 writes to HR0 next */
    int32_t number;  /* the last tid sent */
    int32_t waiting; /* the n an answer must carry, 0 = none */
    uint32_t request_ms;
    bool down[BUTTON_COUNT];
    bool online;
    bool no_mqtt; /* the BENTO Emulator: no MQTT from C yet */
} modbus_t;

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
    static const uint8_t notes[][TUNE_NOTES_MAX] = {
        {76U, 0U, 0U}, {72U, 79U, 0U}, {72U, 79U, 84U}, {84U, 76U, 0U}};
    static const uint32_t note_count[] = {1U, 2U, 3U, 2U};
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
        "n", "ok", "hex", "v", "exc", "err"};
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
static bool read_member(json_reader_t *reader, reply_t *reply)
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
            reply->fields[field] = value;
        }
        reply->members++;
    }
    return ok;
}

static bool read_members(json_reader_t *reader, reply_t *reply)
{
    bool ok = true;
    bool closed = false;

    skip_space(reader);
    if (json_at(reader, (uint8_t)'}')) {
        reader->offset++;
        closed = true;
    }
    while (ok && !closed) {
        ok = read_member(reader, reply);
        skip_space(reader);
        closed = ok && json_at(reader, (uint8_t)'}');
        ok = ok && (closed || json_at(reader, (uint8_t)','));
        reader->offset++;
    }
    return ok;
}

/* The message as one JSON object; false when it is not one. */
static bool read_object(reply_t *reply)
{
    json_reader_t reader = {reply->text, reply->len, 0U};
    bool ok;
    uint32_t i;

    reply->members = 0U;
    for (i = 0U; i < (uint32_t)FIELD_COUNT; i++) {
        reply->fields[i].kind = JSON_MISSING;
        reply->fields[i].start = 0U;
        reply->fields[i].end = 0U;
    }
    skip_space(&reader);
    ok = json_at(&reader, (uint8_t)'{');
    if (ok) {
        reader.offset++;
        ok = read_members(&reader, reply);
    }
    skip_space(&reader);
    return ok && (reader.offset == reader.len);
}

/* An integer: digits with an optional '-', or true / false. */
static bool span_int(const uint8_t *text, const json_span_t *span,
                     int32_t *value)
{
    const uint32_t digits_max = 9U; /* fits an int32_t */
    uint32_t i = span->start;
    bool negative;
    bool ok = (span->kind == JSON_NUMBER);

    *value = (span->kind == JSON_TRUE) ? 1 : 0;
    negative = ok && (text[i] == (uint8_t)'-');
    i += negative ? 1U : 0U;
    ok = ok && (i < span->end) && ((span->end - i) <= digits_max);
    while (ok && (i < span->end)) {
        ok = is_digit(text[i]);
        *value =
            (*value * DECIMAL_BASE) + (int32_t)text[i] - (int32_t)'0';
        i++;
    }
    *value = negative ? -*value : *value;
    return ok || (span->kind == JSON_TRUE) ||
           (span->kind == JSON_FALSE);
}

static bool value_equals(const reply_t *reply, field_t field,
                         int32_t wanted)
{
    int32_t value = 0;

    return span_int(reply->text, &reply->fields[field], &value) &&
           (value == wanted);
}

/* The integers of a list after its '[': the first three go into
 * the registers. */
static bool read_items(json_reader_t *reader, registers_t *registers)
{
    uint32_t count = 0U;
    bool ok = true;
    bool closed = json_at(reader, (uint8_t)']');

    while (ok && !closed) {
        json_span_t item;
        int32_t value = 0;

        ok = read_value(reader, &item) && (item.kind == JSON_NUMBER) &&
             span_int(reader->text, &item, &value);
        if (ok && (count < (uint32_t)REGISTER_COUNT)) {
            registers->known[count] = true;
            registers->value[count] = value;
        }
        count++;
        skip_space(reader);
        closed = ok && json_at(reader, (uint8_t)']');
        ok = ok && (closed || json_at(reader, (uint8_t)','));
        reader->offset++;
    }
    return ok;
}

/* "v": a list of integers; a missing "v" is the empty list. */
static bool read_list(const reply_t *reply, registers_t *registers)
{
    const json_span_t *span = &reply->fields[FIELD_V];
    json_reader_t reader = {reply->text, span->end, span->start + 1U};
    bool ok = span->kind == JSON_MISSING;

    if ((span->kind == JSON_NESTED) &&
        (reply->text[span->start] == (uint8_t)'[')) {
        skip_space(&reader);
        ok = read_items(&reader, registers);
    }
    return ok;
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
static void value_text(const reply_t *reply, field_t field, char *out,
                       uint32_t cap)
{
    static const char *const words[] = {"None", "",      "",
                                        "True", "False", "None"};
    const json_span_t *span = &reply->fields[field];

    if (span->kind == JSON_STRING) {
        copy_cut(out, cap, &reply->text[span->start + 1U],
                 span->end - span->start - 2U);
    } else if ((span->kind == JSON_NUMBER) ||
               (span->kind == JSON_NESTED)) {
        copy_cut(out, cap, &reply->text[span->start],
                 span->end - span->start);
    } else {
        copy_cut(out, cap, (const uint8_t *)words[span->kind],
                 (uint32_t)strlen(words[span->kind]));
    }
}

/* ---- the network ---- */

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
static bool take_newest(uint32_t broker, reply_t *reply)
{
    char topic[TOPIC_KEEP]; /* the program never reads the topic */
    bento_mqtt_msg_t info;
    bool found = false;
    bool more = true;

    while (more) {
        bento_mqtt_msg_init(&info);
        more = bento_mqtt_poll(broker, &info, topic, sizeof(topic),
                               reply->text, PAYLOAD_KEEP) == BENTO_OK;
        if (more) {
            reply->len = info.payload_len;
            found = info.truncated == 0U;
        }
    }
    return found;
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

/* ---- the connection ---- */

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

/* mqtt.connect() with the client id made at the start. Only Stop is
 * an error. */
static bento_status_t open_broker(modbus_t *app, bool *linked)
{
    bento_mqtt_config_t config;
    bento_status_t status;
    bool started;

    bento_mqtt_config_init(&config);
    config.host = BROKER;
    config.host_len = (uint32_t)strlen(BROKER);
    config.port = (uint16_t)PORT;
    config.keepalive_s = (uint16_t)KEEPALIVE_S;
    config.client_id = app->client_id;
    config.client_id_len = (uint32_t)strlen(app->client_id);
    status = bento_mqtt_open_start(&config, &app->broker);
    started = status == BENTO_OK;
    if (started) {
        status = wait_broker(app->broker);
    }
    *linked = status == BENTO_OK;
    app->no_mqtt = status == BENTO_E_UNSUPPORTED;
    if (started && !*linked) {
        (void)bento_mqtt_close(
            app->broker); /* frees it, whatever state */
        app->broker = 0U;
    }
    return (status == BENTO_E_CANCELLED) ? status : BENTO_OK;
}

static bool subscribe_reply(const modbus_t *app)
{
    uint8_t granted = 0U;

    return bento_mqtt_subscribe(app->broker, app->topic_reply,
                                (uint32_t)strlen(app->topic_reply), 0U,
                                &granted) == BENTO_OK;
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

/* WiFi, an address, the broker and the answers' topic (subscribed
 * after the connect). *problem stays NULL when every step passed. */
static bento_status_t connect_farm(modbus_t *app, const char **problem)
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
        status = open_broker(app, &passed);
    }
    /* then the .py's own offline path: the frame-view mode */
    if ((status == BENTO_OK) && app->no_mqtt) {
        status = show_emulator_gap(app->screen.ui);
    }
    if ((status == BENTO_OK) && (*problem == NULL)) {
        passed = passed && subscribe_reply(app);
        *problem = passed ? NULL : "broker ไม่ตอบ";
    }
    return status;
}

/* ---- the frame ---- */

/* A 16-bit field, high byte first. */
static void put_word(uint8_t frame[FRAME_BYTES], uint32_t at,
                     int32_t value)
{
    const uint32_t word = (uint32_t)value & (uint32_t)WORD_MASK;

    frame[at] = (uint8_t)(word >> (uint32_t)BYTE_BITS);
    frame[at + 1U] = (uint8_t)(word & (uint32_t)BYTE_MASK);
}

/* tid, protocol 0, len 6, unit | fc, addr, qty or value. */
static void build_frame(uint8_t frame[FRAME_BYTES], int32_t number,
                        int32_t code, int32_t value)
{
    put_word(frame, 0U, number);
    put_word(frame, AT_PROTOCOL, 0);
    put_word(frame, AT_LENGTH, FOLLOWING);
    frame[AT_UNIT] = (uint8_t)UNIT_ID;
    frame[AT_CODE] = (uint8_t)code;
    put_word(frame, AT_ADDRESS, 0);
    put_word(frame, AT_VALUE, value);
}

/* 12 bytes as 24 hex digits, as the gateway prints them. */
static void frame_hex(const uint8_t frame[FRAME_BYTES],
                      char hex[HEX_BYTES])
{
    const size_t digits = 2U;
    size_t i;

    for (i = 0U; i < (size_t)FRAME_BYTES; i++) {
        (void)snprintf(&hex[i * digits],
                       (size_t)HEX_BYTES - (i * digits), "%02x",
                       (unsigned int)frame[i]);
    }
}

/* A request that cannot leave gets no answer: the timeout then says
 * unknown. */
static void send_request(const modbus_t *app,
                         const uint8_t frame[FRAME_BYTES], bool write,
                         int32_t argument)
{
    char hex[HEX_BYTES];
    char payload[PAYLOAD_BYTES];

    frame_hex(frame, hex);
    (void)snprintf(payload, sizeof(payload),
                   "{\"n\": %" PRId32 ", \"fc\": %u, \"addr\": 0, "
                   "\"hex\": \"%s\", \"%s\": %" PRId32 "}",
                   app->number, (unsigned int)frame[AT_CODE], hex,
                   write ? "value" : "qty", argument);
    /* a lost link makes publish fail, which the .py ignores too */
    (void)bento_mqtt_publish(app->broker, app->topic_request,
                             (uint32_t)strlen(app->topic_request),
                             (const uint8_t *)payload,
                             (uint32_t)strlen(payload), 0U, 0U);
}

/* ---- the screen ---- */

/* The title, MQTT, the warning line and the frame card. */
static bento_status_t add_top(bento_ui_t *ui)
{
    const rect_t box = {MARGIN_X, FRAME_CARD_Y, CARD_W, FRAME_CARD_H};
    const point_t title_at = {MARGIN_X, TITLE_Y};
    const point_t mqtt_at = {MQTT_X, LINK_Y};
    const point_t warning_at = {MARGIN_X, WARNING_Y};
    bento_ui_id_t unused;
    bento_status_t status = add_label(ui, "Modbus TCP 12 ไบต์", title_at,
                                      COL_TEXT, FONT_TITLE, &unused);

    if (status == BENTO_OK) {
        status = add_label(ui, "MQTT", mqtt_at, COL_DIM, FONT_SMALL,
                           &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(
            ui, "บอร์ดไม่มี Modbus/socket: MQTT -> gateway -> PLC",
            warning_at, COL_WARN, FONT_THAI, &unused);
    }
    if (status == BENTO_OK) {
        status = add_card(ui, box, "ฟ้า = MBAP  เขียว = PDU");
    }
    return status;
}

static bento_status_t add_link(modbus_screen_t *screen)
{
    const point_t link_at = {LINK_X, LINK_Y};
    bento_ui_widget_args_t args;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = LINK_LED_X;
    args.y = LINK_Y;
    args.w = LINK_LED_SIZE;
    args.h = LINK_LED_SIZE;
    args.color = COL_OK;
    status = bento_ui_led_create(screen->ui, &args, &screen->led);
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "ออฟไลน์", link_at, COL_DIM,
                           FONT_THAI, &screen->link);
    }
    return status;
}

/* Byte i sits at x = 28 + i * 62; each field's name under its first
 * byte. */
static bento_status_t add_bytes(modbus_screen_t *screen)
{
    static const int32_t first_byte[] = {0, 2, 4, 6, 7, 8, 10};
    static const char *const names[] = {
        "tid", "proto", "len", "unit", "fc", "addr", "qty/value"};
    bento_ui_id_t unused;
    bento_status_t status = BENTO_OK;
    int32_t i;

    for (i = 0; (i < FRAME_BYTES) && (status == BENTO_OK); i++) {
        const point_t at = {px(BYTE_X + (i * BYTE_STEP)), BYTE_Y};

        status = add_label(screen->ui, "--", at,
                           (i < MBAP_BYTES) ? COL_INFO : COL_OK,
                           FONT_BYTE, &screen->bytes[i]);
    }
    for (i = 0;
         (i < ((int32_t)sizeof(names) / (int32_t)sizeof(names[0]))) &&
         (status == BENTO_OK);
         i++) {
        const point_t at = {px(BYTE_X + (first_byte[i] * BYTE_STEP)),
                            FIELD_NAME_Y};

        status = add_label(screen->ui, names[i], at, COL_DIM, FONT_TINY,
                           &unused);
    }
    return status;
}

static bento_status_t add_answer(modbus_screen_t *screen)
{
    const rect_t box = {MARGIN_X, ANSWER_CARD_Y, CARD_W, ANSWER_CARD_H};
    const point_t result_at = {RESULT_X, RESULT_Y};
    const point_t hex_at = {RESULT_X, REPLY_HEX_Y};
    const point_t note_at = {MARGIN_X, NOTE_Y};
    bento_status_t status =
        add_card(screen->ui, box, "HR0 ปั๊ม  HR1 ดิน  HR2 ถัง");

    if (status == BENTO_OK) {
        /* no font size: the 20 px default */
        status = add_label(screen->ui, " ", result_at, COL_DIM,
                           FONT_DEFAULT, &screen->result);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", hex_at, COL_DIM, FONT_SMALL,
                           &screen->reply_hex);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "SW5 อ่าน  SW6 สลับปั๊ม", note_at,
                           COL_DIM, FONT_THAI, &screen->note);
    }
    return status;
}

static bento_status_t build_screen(modbus_screen_t *screen)
{
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = sleep_ms((uint32_t)SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_top(screen->ui);
    }
    if (status == BENTO_OK) {
        status = add_link(screen);
    }
    if (status == BENTO_OK) {
        status = add_bytes(screen);
    }
    if (status == BENTO_OK) {
        status = add_answer(screen);
    }
    if (status == BENTO_OK) {
        present(screen->ui);
    }
    return status;
}

/* The MQTT Led: lit = the broker is linked, frames can go; dim =
 * offline. */
static void show_link(const modbus_screen_t *screen, bool online)
{
    (void)bento_ui_set_value(screen->ui, screen->led, online ? 1 : 0);
    show(screen->ui, screen->link, online ? "เชื่อมต่อแล้ว" : "ออฟไลน์",
         online ? COL_OK : COL_DIM);
}

/* Two hex digits per byte, as the gateway prints them. */
static void show_frame(const modbus_screen_t *screen,
                       const uint8_t frame[FRAME_BYTES])
{
    char text[BYTE_TEXT_BYTES];
    uint32_t i;

    for (i = 0U; i < (uint32_t)FRAME_BYTES; i++) {
        (void)snprintf(text, sizeof(text), "%02X",
                       (unsigned int)frame[i]);
        set_text(screen->ui, screen->bytes[i], text);
    }
}

/* ---- the answer ---- */

/* The answer to the waiting request when this message is one: its n
 * matches, and a confirmed answer's "v" can be read. */
static bool reply_answers(const modbus_t *app, const reply_t *reply,
                          registers_t *registers)
{
    *registers = app->registers;
    return value_equals(reply, FIELD_N, app->waiting) &&
           (!value_equals(reply, FIELD_OK, 1) ||
            read_list(reply, registers));
}

/* Only a PLC confirmation is believed; an error leaves the pump
 * unknown and SW6 asks for off first. */
static void answer_reply(modbus_t *app, const reply_t *reply,
                         const registers_t *registers, answer_t *answer)
{
    char value[VALUE_BYTES];

    answer->ok = value_equals(reply, FIELD_OK, 1);
    if (answer->ok) {
        app->registers = *registers;
        (void)snprintf(answer->text, sizeof(answer->text), "ยืนยันแล้ว");
        app->want =
            app->registers.known[0] ? (1 - app->registers.value[0]) : 0;
    } else if (reply->fields[FIELD_EXC].kind != JSON_MISSING) {
        value_text(reply, FIELD_EXC, value, sizeof(value));
        (void)snprintf(answer->text, sizeof(answer->text),
                       "PLC ปฏิเสธ %s", value);
    } else {
        value_text(reply, FIELD_ERROR, answer->text,
                   sizeof(answer->text));
        app->registers.known[0] = false;
        app->want = 0;
    }
    if (reply->fields[FIELD_HEX].kind == JSON_MISSING) {
        (void)snprintf(answer->hex, sizeof(answer->hex), " ");
    } else {
        value_text(reply, FIELD_HEX, answer->hex, sizeof(answer->hex));
    }
}

/* Silent past REPLY_MS: unknown, never "it worked". */
static void answer_timeout(modbus_t *app, answer_t *answer)
{
    answer->ok = false;
    (void)snprintf(answer->text, sizeof(answer->text), "ไม่มีคำตอบ");
    (void)snprintf(answer->hex, sizeof(answer->hex), " ");
    app->registers.known[0] = false;
    app->want = 0;
}

static void register_text(const registers_t *registers, int32_t i,
                          char text[REGISTER_BYTES])
{
    if (registers->known[i]) {
        (void)snprintf(text, REGISTER_BYTES, "%" PRId32,
                       registers->value[i]);
    } else {
        (void)snprintf(text, REGISTER_BYTES, "--");
    }
}

static bento_status_t show_answer(const modbus_t *app,
                                  const answer_t *answer)
{
    char registers[REGISTER_COUNT][REGISTER_BYTES];
    char text[TEXT_BYTES];
    int32_t i;

    for (i = 0; i < REGISTER_COUNT; i++) {
        register_text(&app->registers, i, registers[i]);
    }
    (void)snprintf(text, sizeof(text), "%s:  %s  %s  %s", answer->text,
                   registers[0], registers[1], registers[2]);
    show(app->screen.ui, app->screen.result, text,
         answer->ok ? COL_OK : COL_BAD);
    /* the answer frame the PLC sent back, as the gateway's hex */
    set_text(app->screen.ui, app->screen.reply_hex, answer->hex);
    return beep(app->screen.ui, answer->ok ? TUNE_GOOD : TUNE_BAD);
}

/* The inbox is asked every round while waiting (it has one slot). */
static bento_status_t check_answer(modbus_t *app, uint32_t now)
{
    reply_t reply;
    registers_t registers;
    answer_t answer;
    bool answered = true;
    bento_status_t status = BENTO_OK;

    if (take_newest(app->broker, &reply) && read_object(&reply) &&
        reply_answers(app, &reply, &registers)) {
        answer_reply(app, &reply, &registers, &answer);
    } else if (bento_time_ticks_diff(now, app->request_ms) >=
               REPLY_MS) {
        answer_timeout(app, &answer);
    } else {
        answered = false;
    }
    if (answered) {
        app->waiting = 0;
        status = show_answer(app, &answer);
    }
    return status;
}

/* ---- the program ---- */

/* One press: build the frame, show it, and send it when online. */
static bento_status_t press_button(modbus_t *app, bool write,
                                   uint32_t now)
{
    const int32_t argument = write ? app->want : READ_COUNT;
    uint8_t frame[FRAME_BYTES];
    char text[TEXT_BYTES];
    bento_status_t status;

    app->number++;
    build_frame(frame, app->number, write ? FC_WRITE : FC_READ,
                argument);
    show_frame(&app->screen, frame);
    if (write) {
        (void)snprintf(text, sizeof(text),
                       "#%" PRId32 " FC06 เขียน HR0 = %" PRId32,
                       app->number, argument);
    } else {
        (void)snprintf(text, sizeof(text),
                       "#%" PRId32 " FC03 อ่าน HR0 %" PRId32 " ช่อง",
                       app->number, argument);
    }
    /* orange: no result yet; online, the answer replaces it */
    show(app->screen.ui, app->screen.result, text, COL_WARN);
    status = beep(app->screen.ui, TUNE_TAP);
    if ((status == BENTO_OK) && write && !app->online) {
        /* frame-view mode toggles by itself to show both frames; it
         * is not the pump's state */
        app->want = 1 - app->want;
    }
    if ((status == BENTO_OK) && app->online) {
        send_request(app, frame, write, argument);
        app->waiting = app->number;
        app->request_ms = now;
    }
    return status;
}

/* A fresh press, with no answer awaited, makes a request. The
 * buttons are debounced at every read, so they are read every
 * SAMPLE_MS. */
static bento_status_t read_buttons(modbus_t *app, uint32_t now)
{
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < (uint32_t)BUTTON_COUNT) && (status == BENTO_OK);
         i++) {
        bool down = false;

        status = bento_buttons_pressed(1U - i, &down);
        if ((status == BENTO_OK) && down && !app->down[i] &&
            (app->waiting == 0)) {
            status = press_button(app, i == 1U, now);
        }
        app->down[i] = down;
    }
    return status;
}

static bento_status_t run_round(modbus_t *app)
{
    uint32_t now;
    bento_status_t status;

    if (app->online && !link_is_up(app->broker)) {
        /* a request still waiting times out: its result is unknown */
        app->online = false;
        show_link(&app->screen, false);
    }
    now = bento_time_ticks_ms();
    status = read_buttons(app, now);
    if ((status == BENTO_OK) && (app->waiting != 0)) {
        status = check_answer(app, now);
    }
    if (status == BENTO_OK) {
        status = sleep_ms((uint32_t)SAMPLE_MS);
    }
    return status;
}

static bento_status_t run_loop(modbus_t *app)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; i < (uint32_t)REGISTER_COUNT; i++) {
        app->registers.known[i] = false;
    }
    /* online and the pump unknown: off first */
    app->want = app->online ? 0 : 1;
    app->request_ms = start_ms;
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = run_round(app);
    }
    return status;
}

/* Online or in frame-view mode: the status line, the Led, the loop,
 * then the broker is let go even after Stop. */
static bento_status_t run_session(modbus_t *app, const char *problem)
{
    bento_status_t status;

    app->online = problem == NULL;
    show(app->screen.ui, app->screen.result,
         app->online ? app->topic_request : problem,
         app->online ? COL_OK : COL_WARN);
    show_link(&app->screen, app->online);
    status = beep(app->screen.ui, TUNE_START);
    if (status == BENTO_OK) {
        status = run_loop(app);
        if (app->broker != 0U) {
            (void)bento_mqtt_close(
                app->broker); /* frees it, whatever */
            app->broker = 0U;
        }
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
    modbus_t app;
    const char *problem = "โหมดดูกรอบ";
    bento_status_t status;

    (void)memset(&app, 0, sizeof(app));
    app.screen.ui = bento_runtime_get_ui();
    /* a new tail on every run, so a quick rerun never clashes with
     * the old session */
    (void)snprintf(app.client_id, sizeof(app.client_id),
                   "bento-mb-%s-%04" PRIx32, TEAM,
                   bento_time_ticks_ms() & (uint32_t)CLIENT_TAIL_MASK);
    (void)snprintf(app.topic_request, sizeof(app.topic_request),
                   "bento-aiot/%s/modbus/req", TEAM);
    (void)snprintf(app.topic_reply, sizeof(app.topic_reply),
                   "bento-aiot/%s/modbus/resp", TEAM);
    /* a board without ui.volume answers BENTO_E_UNSUPPORTED and is
     * skipped as the .py skips it; elsewhere it waits for no answer */
    (void)bento_ui_sfx_volume_set(app.screen.ui, SPEAKER);
    status = build_screen(&app.screen);
    if ((status == BENTO_OK) && (WIFI_SSID[0] != '<')) {
        status = connect_farm(&app, &problem);
    }
    if (status == BENTO_OK) {
        status = run_session(&app, problem);
    }
    if (app.broker != 0U) { /* the client would outlive the program */
        (void)bento_mqtt_close(app.broker);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
