/*
 * sf2_04_crop_alert.c -- พืชไม่สบาย มือถือรู้ทันที
 * (A plant in trouble: the phone knows at once)
 *
 * วัดอุณหภูมิและความชื้นทุกวินาที บวก "แดดจำลอง" จาก VR3 (0-10 C)
 * แล้วตัดสินตามเกณฑ์ของพืชใน CROP: สบาย เครียด หรือแย่ ไฟสถานะ 3 ดวง
 * กราฟอุณหภูมิกับเส้นเกณฑ์ร้อน ทุกครั้งที่ระดับเปลี่ยนจะส่งแจ้งเตือน
 * JSON เข้าหัวข้อ event แล้วเล่นเสียงและขึ้นจอไฟ RGB ระดับแย่ขึ้นกล่อง
 * เตือนและร้องซ้ำทุก 5 วินาทีจนมีคนรับทราบ ด้วย {"cmd":"ack"} จากแอป
 * หรือกด SW5 คำสั่ง {"cmd":"set","t_hi":28} เปลี่ยนเกณฑ์ร้อน แก้
 * WIFI_SSID WIFI_PASS TEAM CROP และ TEMP_OFFSET ก่อนรัน
 *
 * Reads the temperature and humidity every second, adds a simulated
 * sun from VR3 (0-10 C) and judges them against the crop's limits:
 * fine, stressed or bad, on three lamps and a chart with the heat
 * limit. Each change of level sends a JSON alert on the event topic,
 * then a tune and the RGB matrix; "bad" opens a message box and
 * beeps every 5 s until someone acknowledges it, with {"cmd":"ack"}
 * from the app or SW5. {"cmd":"set","t_hi":28} moves the heat limit.
 * Set WIFI_SSID, WIFI_PASS, TEAM, CROP and TEMP_OFFSET first.
 *
 * Twin of sf2_04_crop_alert.py
 * (aiot-development-for-smart-farm/s2/sf2_04_crop_alert.py).
 * Style: CLEAN_C.md. Differs from the original:
 *   - json.loads is a small reader here. It reads JSON as
 *     MicroPython's json does (a ',' or ':' is skipped like a space)
 *     and finds the top-level keys the program asks for; nesting
 *     deeper than JSON_DEPTH_MAX counts as not JSON. The payload's
 *     bytes are not checked as UTF-8.
 *   - json.dumps is snprintf: the keys come in the order written,
 *     where MicroPython's dict may order them differently (the web
 *     app reads them by name). round(x, 1) is nearbyint(x * 10) / 10
 *     at the board's float width, printed as str() prints it.
 *   - wifi.connect() and mqtt.connect() print lines of their own; the
 *     C calls print nothing. An SSID or password the radio cannot
 *     take counts as a failed join, as it does there.
 *   - A broker try that fails closes its client before the next try:
 *     MicroPython's next mqtt.connect() does that itself. A subscribe
 *     on a link that has just dropped counts as refused.
 *   - get_message() keeps one message, the newest; the C client
 *     queues up to four, so each round takes them all and keeps the
 *     newest.
 *   - A failed sht40 read gives the same "no reading" as the
 *     original's except: the next round tries again.
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
static const char TEAM[] = "teamXX";
/* ชื่อไทยตรงกับตาราง CROPS ใน find_crop() */
static const char CROP[] = "มะเขือเทศ";
/* บอร์ดอ่านสูงกว่าห้อง: ใส่ค่าชดเชย เช่น -7.0 */
static const bento_real_t TEMP_OFFSET = (bento_real_t)0.0;

static const char BROKER[] = "broker.hivemq.com";
static const char ROOT[] = "bento-aiot";
/* CLIENT_ID = "bento-farm-" + TEAM, + เลขจากนาฬิกาบอร์ดทุกครั้งที่ต่อ:
 * ไม่ชน id เก่า, built in main() */
/* TOPIC_EVENT = ROOT/TEAM/event, built in main() */
/* TOPIC_CMD = ROOT/TEAM/cmd, built in main() */
/* ---- end of settings ---- */

static const char *const BUTTON_NAMES[] = {"SW6", "SW5"};

/* The crops: Thai name, English code and their limits. */
enum {
    LIMIT_T_LOW,
    LIMIT_T_HIGH,
    LIMIT_H_LOW,
    LIMIT_H_HIGH,
    LIMIT_COUNT
};

typedef struct {
    const char *thai;
    const char *code;
    int32_t limits[LIMIT_COUNT]; /* C low, C high, %RH low, %RH high */
} crop_t;

enum {
    T_HI_MAX = 45,
    READ_MS = 1000,
    BEEP_MS = 5000,
    POLL_MS = 20,
    RUN_MS = 600000,
    CHART_MAX_C = 50
};

enum {
    BOX_X = 212, /* the alarm's message box */
    BOX_Y = 90,
    BOX_W = 368,
    BOX_H = 150
};

enum {
    SPEAKER = 40,
    VOLUME = 25
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

/* The three levels, their lamps and colours. */
enum {
    LEVEL_NONE = -1, /* not judged yet */
    LEVEL_FINE = 0,
    LEVEL_STRESSED = 1,
    LEVEL_BAD = 2,
    LEVEL_COUNT = 3
};

static const uint32_t LEVEL_COLORS[LEVEL_COUNT] = {COLOR_OK, COLOR_WARN,
                                                   COLOR_BAD};

/* Times, scales and the link. */
enum {
    SETTLE_MS = 200, /* let the cleared screen settle */
    TONE_MS = 90,
    NOTE_MS = 100,
    KNOB_FULL = 4095,
    KNOB_SUN = 2, /* VR3 */
    SUN_MAX_C = 10,
    FAR_C = 3, /* this far past a limit is "bad" */
    FAR_RH = 10,
    TENTHS = 10, /* round(x, 1) */
    BROKER_TRIES = 3,
    PORT = 1883, /* not encrypted: anyone can read the topics */
    KEEPALIVE_S = 60,
    CLIENT_CLOCK_MASK = 0xFFFF,
    WIFI_SLICE_MS = 50,
    MQTT_SLICE_MS = 20,
    SCROLL_MS = 80,
    CHART_POINTS = 400,
    BUTTON_CONFIRM = 1, /* SW5, the lower button */
    TEAM_CHARS = 6,     /* "team" and two digits */
    TEAM_PREFIX = 4
};

/* Text buffers, each sized for the longest text built in it. */
enum {
    TEXT_BYTES = 128,
    NAME_BYTES = 64,
    CLIENT_PREFIX_BYTES = 24, /* MQTT's 23-byte client id and a NUL */
    WHY_BYTES = 48,
    KEY_BYTES = 16,
    PAYLOAD_BYTES = 256 /* every number at full length */
};

/* Cards on the screen: a panel and its title. */
enum {
    CARD_RADIUS = 12,
    CARD_BORDER_PX = 1,
    CARD_INSET_X = 12,
    CARD_INSET_Y = 6,
    CARD_TITLE_PX = 20
};

/* ---- sound and Stop ---- */

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

/* false when either sht40 value cannot be read. */
static bool read_climate(bento_real_t *t, bento_real_t *h)
{
    float t_raw = 0.0F;
    float humidity = 0.0F;
    const bool read =
        (bento_sensors_sht40_temperature(&t_raw) == BENTO_OK) &&
        (bento_sensors_sht40_humidity(&humidity) == BENTO_OK);

    *t = (bento_real_t)t_raw + TEMP_OFFSET;
    *h = (bento_real_t)humidity;
    return read;
}

/* Stops the marquee too, else it keeps running on its own. */
static bento_status_t matrix_off(void)
{
    bento_status_t status = bento_rgbmatrix_scroll(
        "", 0U, BENTO_RGBMATRIX_WHITE, SCROLL_MS);

    if (status == BENTO_OK) {
        status = bento_rgbmatrix_clear();
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

/* ---- json.loads, as far as the program reads it ---- */

typedef enum {
    JSON_MISSING,
    JSON_STRING,
    JSON_INT,
    JSON_FLOAT,
    JSON_TRUE,
    JSON_FALSE,
    JSON_NULL,
    JSON_LIST,
    JSON_OBJECT,
    JSON_CLOSE
} json_kind_t;

/* The keys the program reads from a message. */
typedef enum {
    FIELD_CMD,
    FIELD_T_HI,
    FIELD_COUNT
} field_t;

enum {
    JSON_DEPTH_MAX = 16,
    FIELD_NONE = -1,
    UNICODE_ESCAPE_BYTES = 6,
    HEX_DIGITS = 4,
    HEX_LETTER_GAP = 39, /* from '9' + 1 to 'a' */
    HEX_BITS = 4,
    HEX_MASK = 0xF,
    UTF8_SHIFT = 6,
    UTF8_BYTES_MAX = 3,
    UTF8_TWO_BYTES = 0x80,
    UTF8_THREE_BYTES = 0x800,
    UTF8_TOP_BITS = 0xC0,
    UTF8_FOLLOW = 0x80,
    UTF8_LOW_BITS = 0x3F,
    UTF8_LEAD_TWO = 0xC0,
    UTF8_LEAD_THREE = 0xE0,
    ASCII_FIRST = 0x20, /* ' ' */
    ASCII_LAST = 0x7E,  /* '~' */
    DECIMAL = 10
};

typedef struct {
    json_kind_t kind;
    uint32_t start; /* its first byte */
    uint32_t end;   /* one past its last byte */
} json_value_t;

typedef struct {
    const uint8_t *text;
    uint32_t len;
    json_kind_t top; /* JSON_MISSING: the text is not JSON */
    bool has_keys;   /* the top object is not empty */
    json_value_t fields[FIELD_COUNT];
} command_t;

typedef struct {
    command_t *command;
    uint32_t offset;
    uint32_t depth;
    bool key_next;      /* in the top object, a key comes next */
    int32_t key_field;  /* the field the last key named */
    int32_t open_field; /* the field whose list or object is open */
} json_reader_t;

static bool is_digit(uint8_t byte)
{
    return (byte >= (uint8_t)'0') && (byte <= (uint8_t)'9');
}

static bool is_space(uint8_t byte)
{
    return (byte == (uint8_t)' ') || (byte == (uint8_t)'\t') ||
           (byte == (uint8_t)'\n') || (byte == (uint8_t)'\r');
}

static bool is_separator(uint8_t byte)
{
    return is_space(byte) || (byte == (uint8_t)',') ||
           (byte == (uint8_t)':');
}

static bool is_exponent(uint8_t byte)
{
    return (byte == (uint8_t)'e') || (byte == (uint8_t)'E');
}

static bool is_number_byte(uint8_t byte)
{
    return is_digit(byte) || is_exponent(byte) ||
           (byte == (uint8_t)'.') || (byte == (uint8_t)'+') ||
           (byte == (uint8_t)'-');
}

static bool is_container(json_kind_t kind)
{
    return (kind == JSON_LIST) || (kind == JSON_OBJECT);
}

/* null, true, false: every letter must be there. */
static bool read_word(json_reader_t *reader, const char *word)
{
    const command_t *command = reader->command;
    const uint32_t length = (uint32_t)strlen(word);
    bool same = (command->len - reader->offset) >= length;

    if (same) {
        same = strncmp((const char *)&command->text[reader->offset],
                       word, (size_t)length) == 0;
    }
    if (same) {
        reader->offset += length;
    }
    return same;
}

/* A string, both quotes included; an escape takes 2 or 6 bytes. */
static bool read_string(json_reader_t *reader)
{
    const command_t *command = reader->command;
    uint32_t offset = reader->offset + 1U;
    bool closed = false;

    while (!closed && (offset < command->len)) {
        const uint8_t byte = command->text[offset];

        if (byte == (uint8_t)'"') {
            closed = true;
        } else if ((byte == (uint8_t)'\\') &&
                   ((offset + 1U) < command->len) &&
                   (command->text[offset + 1U] == (uint8_t)'u')) {
            offset += (uint32_t)UNICODE_ESCAPE_BYTES;
        } else {
            offset += (byte == (uint8_t)'\\') ? 2U : 1U;
        }
    }
    reader->offset = closed ? (offset + 1U) : command->len;
    return closed && (offset < command->len);
}

/* A number: '-', digits, and for a real one a '.' or an exponent. */
static bool number_text_ok(const uint8_t *text, uint32_t len, bool real)
{
    uint32_t i = (text[0] == (uint8_t)'-') ? 1U : 0U;
    bool ok = (i < len) && is_digit(text[i]);

    for (; ok && !real && (i < len); i++) {
        ok = is_digit(text[i]);
    }
    if (ok && real) {
        ok = is_digit(text[len - 1U]);
    }
    return ok;
}

static bool read_number(json_reader_t *reader, json_value_t *token)
{
    const command_t *command = reader->command;
    uint32_t offset = reader->offset + 1U;
    bool real = false;

    while ((offset < command->len) &&
           is_number_byte(command->text[offset])) {
        real = real || is_exponent(command->text[offset]) ||
               (command->text[offset] == (uint8_t)'.');
        offset++;
    }
    token->kind = real ? JSON_FLOAT : JSON_INT;
    reader->offset = offset;
    return number_text_ok(&command->text[token->start],
                          offset - token->start, real);
}

static bool read_token(json_reader_t *reader, json_value_t *token)
{
    const uint8_t byte = reader->command->text[reader->offset];
    bool ok = true;

    token->start = reader->offset;
    if ((byte == (uint8_t)'{') || (byte == (uint8_t)'[')) {
        token->kind = (byte == (uint8_t)'{') ? JSON_OBJECT : JSON_LIST;
        reader->offset++;
    } else if ((byte == (uint8_t)'}') || (byte == (uint8_t)']')) {
        token->kind = JSON_CLOSE;
        reader->offset++;
    } else if (byte == (uint8_t)'"') {
        token->kind = JSON_STRING;
        ok = read_string(reader);
    } else if (byte == (uint8_t)'n') {
        token->kind = JSON_NULL;
        ok = read_word(reader, "null");
    } else if (byte == (uint8_t)'t') {
        token->kind = JSON_TRUE;
        ok = read_word(reader, "true");
    } else if (byte == (uint8_t)'f') {
        token->kind = JSON_FALSE;
        ok = read_word(reader, "false");
    } else if ((byte == (uint8_t)'-') || is_digit(byte)) {
        ok = read_number(reader, token);
    } else {
        ok = false;
    }
    token->end = reader->offset;
    return ok;
}

/* The number after \u; modjson.c does not check the hex digits. */
static uint32_t unicode_number(const uint8_t *digits)
{
    const uint8_t lower_case = 0x20U;
    uint32_t number = 0U;
    uint32_t i;

    for (i = 0U; i < (uint32_t)HEX_DIGITS; i++) {
        uint32_t digit =
            (uint32_t)(digits[i] | lower_case) - (uint32_t)'0';

        if (digit >= (uint32_t)DECIMAL) {
            digit -= (uint32_t)HEX_LETTER_GAP;
        }
        number = (number << (uint32_t)HEX_BITS) |
                 (digit & (uint32_t)HEX_MASK);
    }
    return number;
}

/* The UTF-8 bytes of one \u escape: one, two or three. */
static uint32_t unicode_bytes(uint32_t number, uint8_t *bytes)
{
    const uint32_t low_bits = (uint32_t)UTF8_LOW_BITS;
    const uint32_t follow = (uint32_t)UTF8_FOLLOW;
    const uint32_t shift = (uint32_t)UTF8_SHIFT;
    uint32_t count = 1U;

    if (number < (uint32_t)UTF8_TWO_BYTES) {
        bytes[0] = (uint8_t)number;
    } else if (number < (uint32_t)UTF8_THREE_BYTES) {
        bytes[0] =
            (uint8_t)((uint32_t)UTF8_LEAD_TWO | (number >> shift));
        bytes[1] = (uint8_t)(follow | (number & low_bits));
        count = 2U;
    } else {
        bytes[0] = (uint8_t)((uint32_t)UTF8_LEAD_THREE |
                             (number >> (shift + shift)));
        bytes[1] = (uint8_t)(follow | ((number >> shift) & low_bits));
        bytes[2] = (uint8_t)(follow | (number & low_bits));
        count = (uint32_t)UTF8_BYTES_MAX;
    }
    return count;
}

/* \b \f \n \r \t stand for control bytes; any other letter for
 * itself (\" \\ \/). */
static uint8_t escaped_byte(uint8_t letter)
{
    static const uint8_t letters[] = {'b', 'f', 'n', 'r', 't'};
    static const uint8_t bytes[] = {0x08U, 0x0CU, 0x0AU, 0x0DU, 0x09U};
    uint8_t byte = letter;
    uint32_t i;

    for (i = 0U; i < (uint32_t)sizeof(letters); i++) {
        if (letter == letters[i]) {
            byte = bytes[i];
        }
    }
    return byte;
}

/* One character of a string at *offset, as bytes; moves past it. */
static uint32_t next_piece(const uint8_t *text, uint32_t *offset,
                           uint8_t *piece)
{
    const uint32_t at = *offset;
    uint32_t count = 1U;

    piece[0] = text[at];
    if (text[at] != (uint8_t)'\\') {
        *offset = at + 1U;
    } else if (text[at + 1U] == (uint8_t)'u') {
        count = unicode_bytes(unicode_number(&text[at + 2U]), piece);
        *offset = at + (uint32_t)UNICODE_ESCAPE_BYTES;
    } else {
        piece[0] = escaped_byte(text[at + 1U]);
        *offset = at + 2U;
    }
    return count;
}

/* The text inside a string token, cut to cap - 1 bytes and ended
 * with a NUL. Answers its whole length. */
static uint32_t decode_string(const uint8_t *text,
                              const json_value_t *value, char *out,
                              uint32_t cap)
{
    const uint32_t end = value->end - 1U; /* the closing quote */
    uint32_t offset = value->start + 1U;
    uint32_t length = 0U;

    while (offset < end) {
        uint8_t piece[UTF8_BYTES_MAX] = {0U, 0U, 0U};
        const uint32_t count = next_piece(text, &offset, piece);
        uint32_t i;

        for (i = 0U; (i < count) && ((length + 1U) < cap); i++) {
            out[length] = (char)piece[i];
            length++;
        }
        length += count - i; /* the bytes that did not fit */
    }
    out[(length < cap) ? length : (cap - 1U)] = '\0';
    return length;
}

/* Which field a key names, else FIELD_NONE. */
static int32_t field_named(const command_t *command,
                           const json_value_t *key)
{
    static const char *const field_names[FIELD_COUNT] = {"cmd", "t_hi"};
    char name[KEY_BYTES];
    const uint32_t length =
        decode_string(command->text, key, name, sizeof(name));
    int32_t found = FIELD_NONE;
    int32_t i;

    for (i = 0; i < (int32_t)FIELD_COUNT; i++) {
        if ((length == (uint32_t)strlen(field_names[i])) &&
            (strcmp(name, field_names[i]) == 0)) {
            found = i;
        }
    }
    return found;
}

/* A token inside the top object: a key, or the value of the key
 * before it (the last equal key wins). Only a string is a key. */
static bool take_member(json_reader_t *reader,
                        const json_value_t *token)
{
    command_t *command = reader->command;
    bool ok = true;

    if (reader->key_next) {
        ok = token->kind == JSON_STRING;
        reader->key_field =
            ok ? field_named(command, token) : FIELD_NONE;
        command->has_keys = true;
    } else if (reader->key_field != FIELD_NONE) {
        command->fields[reader->key_field] = *token;
        if (is_container(token->kind)) {
            reader->open_field = reader->key_field;
        }
    } else {
        /* a key the program does not read */
    }
    reader->key_next = !reader->key_next;
    return ok;
}

/* A ] or }: the innermost list or object ends here. */
static bool take_close(json_reader_t *reader, const json_value_t *token,
                       bool *done)
{
    command_t *command = reader->command;
    const bool ok = (reader->depth > 0U) &&
                    ((reader->depth > 1U) ||
                     (command->top != JSON_OBJECT) || reader->key_next);

    if (ok) {
        reader->depth--;
        if ((reader->depth == 1U) &&
            (reader->open_field != FIELD_NONE)) {
            command->fields[reader->open_field].end = token->end;
            reader->open_field = FIELD_NONE;
        }
        *done = reader->depth == 0U;
    }
    return ok;
}

static bool take_token(json_reader_t *reader, const json_value_t *token,
                       bool *done)
{
    command_t *command = reader->command;
    bool ok = true;

    if (token->kind == JSON_CLOSE) {
        ok = take_close(reader, token, done);
    } else if (command->top == JSON_MISSING) {
        command->top = token->kind;
        *done = !is_container(token->kind);
    } else if ((reader->depth == 1U) && (command->top == JSON_OBJECT)) {
        ok = take_member(reader, token);
    } else {
        /* inside a list, or nested deeper: only skipped over */
    }
    if (ok && is_container(token->kind)) {
        ok = reader->depth < (uint32_t)JSON_DEPTH_MAX;
        reader->depth++;
        reader->key_next = reader->key_next || (reader->depth == 1U);
    }
    return ok;
}

/* json.loads(raw.decode()): the kind of the top value and, for an
 * object, the fields the program reads. */
static void read_command(const uint8_t *text, uint32_t len,
                         command_t *command)
{
    json_reader_t reader = {command, 0U,         0U,
                            false,   FIELD_NONE, FIELD_NONE};
    json_value_t token = {JSON_MISSING, 0U, 0U};
    bool done = false;
    bool ok = true;
    int32_t i;

    command->text = text;
    command->len = len;
    command->top = JSON_MISSING;
    command->has_keys = false;
    for (i = 0; i < (int32_t)FIELD_COUNT; i++) {
        command->fields[i] = token;
    }
    while (ok && !done) {
        while ((reader.offset < len) &&
               is_separator(text[reader.offset])) {
            reader.offset++;
        }
        ok = (reader.offset < len) && read_token(&reader, &token) &&
             take_token(&reader, &token, &done);
    }
    while ((reader.offset < len) && is_space(text[reader.offset])) {
        reader.offset++;
    }
    if (!ok || (reader.offset != len)) {
        command->top = JSON_MISSING;
    }
}

/* Is the field the string word? */
static bool value_is(const command_t *command, field_t field,
                     const char *word)
{
    char text[KEY_BYTES];
    const json_value_t *value = &command->fields[field];
    bool same = false;

    if (value->kind == JSON_STRING) {
        const uint32_t length =
            decode_string(command->text, value, text, sizeof(text));

        same = (length == (uint32_t)strlen(word)) &&
               (strcmp(text, word) == 0);
    }
    return same;
}

/* The digits of a whole number, clamped to the int32_t range:
 * Python's int has no limit, and every use here is capped far below
 * that. */
static int32_t clamped_int(const uint8_t *text, uint32_t len)
{
    const bool negative = text[0] == (uint8_t)'-';
    const int64_t limit =
        negative ? -(int64_t)INT32_MIN : (int64_t)INT32_MAX;
    int64_t magnitude = 0;
    uint32_t i;

    for (i = negative ? 1U : 0U; i < len; i++) {
        magnitude =
            (magnitude * DECIMAL) + ((int64_t)text[i] - (int64_t)'0');
        magnitude = (magnitude > limit) ? limit : magnitude;
    }
    return (int32_t)(negative ? -magnitude : magnitude);
}

/* A whole number (not true or false) into *number. */
static bool int_value(const command_t *command, field_t field,
                      int32_t *number)
{
    const json_value_t *value = &command->fields[field];
    const bool whole = value->kind == JSON_INT;

    if (whole) {
        *number = clamped_int(&command->text[value->start],
                              value->end - value->start);
    }
    return whole;
}

/* ---- 3) สมอง ---- */

static bool team_is_set(void)
{
    return (strlen(TEAM) == (size_t)TEAM_CHARS) &&
           (strncmp(TEAM, "team", (size_t)TEAM_PREFIX) == 0) &&
           is_digit((uint8_t)TEAM[TEAM_PREFIX]) &&
           is_digit((uint8_t)TEAM[TEAM_PREFIX + 1]) &&
           (strcmp(TEAM, "team00") != 0);
}

/* What judge() found: the level, and why in Thai and in English. */
typedef struct {
    int32_t level;
    char thai[WHY_BYTES];
    char english[WHY_BYTES];
} verdict_t;

/* The words of each problem, joined with " + " and "+". */
static void name_problems(const bool *problem, verdict_t *verdict)
{
    static const char *const thai[LIMIT_COUNT] = {"หนาวไป", "ร้อนไป",
                                                  "แห้งไป", "ชื้นไป"};
    static const char *const english[LIMIT_COUNT] = {"cold", "hot",
                                                     "dry", "wet"};
    size_t thai_len = 0U;
    size_t english_len = 0U;
    uint32_t i;

    verdict->thai[0] = '\0';
    verdict->english[0] = '\0';
    for (i = 0U; i < (uint32_t)LIMIT_COUNT; i++) {
        if (problem[i]) {
            (void)snprintf(&verdict->thai[thai_len],
                           sizeof(verdict->thai) - thai_len, "%s%s",
                           (thai_len != 0U) ? " + " : "", thai[i]);
            (void)snprintf(&verdict->english[english_len],
                           sizeof(verdict->english) - english_len,
                           "%s%s", (english_len != 0U) ? "+" : "",
                           english[i]);
            thai_len = strlen(verdict->thai);
            english_len = strlen(verdict->english);
        }
    }
}

/* Level 0 fine, 1 stressed, 2 far off (more than 3 C or 10 %RH). */
static void judge(bento_real_t t, bento_real_t h, const int32_t *limits,
                  verdict_t *verdict)
{
    const int32_t t_lo = limits[LIMIT_T_LOW];
    const int32_t t_hi = limits[LIMIT_T_HIGH];
    const int32_t h_lo = limits[LIMIT_H_LOW];
    const int32_t h_hi = limits[LIMIT_H_HIGH];
    const bool problem[LIMIT_COUNT] = {
        t<(bento_real_t)t_lo, t>(bento_real_t) t_hi,
        h<(bento_real_t)h_lo, h>(bento_real_t) h_hi};
    bool far;

    name_problems(problem, verdict);
    far = (t < (bento_real_t)(t_lo - FAR_C)) ||
          (t > (bento_real_t)(t_hi + FAR_C)) ||
          (h < (bento_real_t)(h_lo - FAR_RH)) ||
          (h > (bento_real_t)(h_hi + FAR_RH));
    if (verdict->thai[0] == '\0') {
        verdict->level = LEVEL_FINE;
        (void)snprintf(verdict->thai, sizeof(verdict->thai), "สบายดี");
        (void)snprintf(verdict->english, sizeof(verdict->english),
                       "ok");
    } else {
        verdict->level = far ? LEVEL_BAD : LEVEL_STRESSED;
    }
}

/* ---- 4) เครือข่าย ---- */

typedef struct {
    char client_id[CLIENT_PREFIX_BYTES]; /* no clock number yet */
    char topic_event[NAME_BYTES];
    char topic_cmd[NAME_BYTES];
} names_t;

/* The newest message: too big for the 8 KB program stack, so at file
 * scope, zeroed by the loader and set again in main() (CLEAN_C.md C9,
 * deviation BENTO-MISRA-E03/E04). */
typedef struct {
    char topic[BENTO_MQTT_TOPIC_MAX];
    uint8_t payload[BENTO_MQTT_PAYLOAD_MAX];
    uint32_t payload_len;
} message_t;

extern message_t g_message;
message_t g_message;

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

/* mqtt.get_message(): the newest message waiting, if any. */
static bool take_newest(uint32_t broker, message_t *message)
{
    bento_mqtt_msg_t info;
    bool found = false;
    bool more = true;

    while (more) {
        bento_mqtt_msg_init(&info);
        more = bento_mqtt_poll(broker, &info, message->topic,
                               sizeof(message->topic), message->payload,
                               sizeof(message->payload)) == BENTO_OK;
        if (more) {
            message->payload_len = info.payload_len;
            found = true;
        }
    }
    return found;
}

/* ---- 5) หน้าจอ ---- */

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t crop;
    bento_ui_id_t reading;
    bento_ui_id_t mood;
    bento_ui_id_t lamps[LEVEL_COUNT];
    bento_ui_id_t answer;
    bento_ui_id_t chart;
    uint8_t hot_series;
    bento_ui_id_t status;
    bento_ui_id_t link_led;
    bento_ui_id_t link_text;
    bento_ui_id_t box;
    bool has_box;
} alert_screen_t;

static void show_status(const alert_screen_t *screen, const char *text,
                        uint32_t color)
{
    show_colored(screen->ui, screen->status, color, text);
}

static void show_link(const alert_screen_t *screen, bool linked)
{
    show_value(screen->ui, screen->link_led, linked ? 1 : 0);
    show_text(screen->ui, screen->link_text,
              linked ? "MQTT: เชื่อมต่อแล้ว" : "MQTT: ออฟไลน์");
}

/* The three lamps, each with its name beside it. */
static bento_status_t build_lamps(alert_screen_t *screen)
{
    static const char *const names[LEVEL_COUNT] = {"สบาย", "เครียด",
                                                   "แย่"};
    const int16_t step_px = 130;
    bento_status_t status = BENTO_OK;
    bento_ui_id_t unused;
    int32_t i;

    for (i = 0; (i < LEVEL_COUNT) && (status == BENTO_OK); i++) {
        const int16_t x = (int16_t)(24 + (i * step_px));
        const box_t lamp = {x, 198, 20, 20, LEVEL_COLORS[i], 0, 0, 0};
        const spot_t name = {(int16_t)(x + 28), 198, COLOR_DIM, 20};

        status = add_box(screen->ui, UI_WIDGET_LED, &lamp, NULL,
                         &screen->lamps[i]);
        if (status == BENTO_OK) {
            status = add_label(screen->ui, &name, names[i], &unused);
        }
    }
    return status;
}

static bento_status_t build_now_card(alert_screen_t *screen)
{
    static const box_t card = {12, 44, 500, 200, 0U, 0, 0, 0};
    static const spot_t crop = {24, 80, COLOR_INFO, 20};
    static const spot_t reading = {24, 110, COLOR_TEXT, 24};
    static const spot_t mood = {24, 150, COLOR_WARN, 20};
    bento_ui_id_t unused;
    bento_status_t status =
        add_card(screen->ui, &card, "ตอนนี้", &unused);

    if (status == BENTO_OK) {
        status = add_label(screen->ui, &crop, "-", &screen->crop);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &reading, "-- C  -- %",
                           &screen->reading);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &mood, "...", &screen->mood);
    }
    if (status == BENTO_OK) {
        status = build_lamps(screen);
    }
    return status;
}

/* LVGL draws no round dots once the points reach the width. */
static bento_status_t build_chart(alert_screen_t *screen)
{
    static const box_t answer_card = {522, 44, 258, 200, 0U, 0, 0, 0};
    static const spot_t answer = {534, 84, COLOR_DIM, 20};
    static const box_t chart = {12,         252, 400,         86,
                                COLOR_WARN, 0,   CHART_MAX_C, 0};
    static const spot_t legend = {424, 256, COLOR_DIM, 20};
    char text[TEXT_BYTES];
    bento_ui_id_t unused;
    bento_status_t status =
        add_card(screen->ui, &answer_card, "แจ้งสถานะ", &unused);

    if (status == BENTO_OK) {
        status = add_label(screen->ui, &answer, "-", &screen->answer);
    }
    if (status == BENTO_OK) {
        status = add_box(screen->ui, UI_WIDGET_CHART, &chart, NULL,
                         &screen->chart);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_prop(screen->ui, screen->chart,
                                (uint8_t)BENTO_UI_PROP_CHART_POINTS,
                                CHART_POINTS); /* a setter */
        status = bento_ui_chart_add_series(
            screen->ui, screen->chart, COLOR_BAD, &screen->hot_series);
    }
    if (status == BENTO_OK) {
        (void)snprintf(
            text, sizeof(text),
            "ส้ม = อุณหภูมิ  แดง = เกณฑ์ร้อน\nVR3 = แดด  %s = รับทราบ",
            BUTTON_NAMES[1]);
        status = add_label(screen->ui, &legend, text, &unused);
    }
    return status;
}

static bento_status_t build_screen(alert_screen_t *screen)
{
    static const spot_t heading = {12, 6, COLOR_TEXT, 24};
    static const spot_t status_line = {12, 352, COLOR_DIM, 20};
    static const box_t link_led = {586, 12, 18, 18, COLOR_OK, 0, 0, 0};
    static const spot_t link_text = {612, 10, COLOR_DIM, 20};
    bento_ui_id_t unused;
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = pause_ms(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &heading, "พืชไม่สบาย มือถือรู้ทันที",
                           &unused);
    }
    if (status == BENTO_OK) {
        status = build_now_card(screen);
    }
    if (status == BENTO_OK) {
        status = build_chart(screen);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &status_line, "กำลังเริ่ม",
                           &screen->status);
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

/* A box that is already gone needs nothing; delete() ignores a
 * refusal (modui.c). */
static void close_box(alert_screen_t *screen)
{
    if (screen->has_box) {
        (void)bento_ui_delete(screen->ui, screen->box);
        screen->has_box = false;
    }
}

/* ---- 6) โปรแกรมหลัก ---- */

typedef struct {
    int32_t level; /* LEVEL_NONE: judge again and alert at once */
    bool acked;
    int32_t alerts;
    bool has_reading; /* false: read again next round */
    const crop_t *crop;
    int32_t limits[LIMIT_COUNT];
} watch_t;

typedef struct {
    alert_screen_t screen;
    names_t names;
    uint32_t broker;
    watch_t watch;
    bool was_down;
    uint32_t read_ms;
    uint32_t beep_ms;
} farm_t;

static void show_answer(const farm_t *farm, const char *text,
                        uint32_t color)
{
    char line[TEXT_BYTES];

    (void)snprintf(line, sizeof(line), "แจ้งไปแล้ว %" PRId32 " ครั้ง\n%s",
                   farm->watch.alerts, text);
    show_colored(farm->screen.ui, farm->screen.answer, color, line);
}

static void show_crop(const farm_t *farm)
{
    const int32_t *limits = farm->watch.limits;
    char text[TEXT_BYTES];

    (void)snprintf(text, sizeof(text),
                   "%s ชอบ %" PRId32 "-%" PRId32 " C, %" PRId32
                   "-%" PRId32 " %%",
                   farm->watch.crop->thai, limits[LIMIT_T_LOW],
                   limits[LIMIT_T_HIGH], limits[LIMIT_H_LOW],
                   limits[LIMIT_H_HIGH]);
    show_text(farm->screen.ui, farm->screen.crop, text);
}

/* '%.1f' % value; BENTO_FMT_BUF_SIZE holds any number. */
static void one_decimal(bento_real_t value, char *text)
{
    uint32_t length = 0U;

    (void)bento_fmt_real_precision(text, BENTO_FMT_BUF_SIZE, value, 1,
                                   &length);
}

static void show_reading(const farm_t *farm, bento_real_t t,
                         bento_real_t h, int32_t sun,
                         const verdict_t *verdict)
{
    static const char *const moods[LEVEL_COUNT] = {
        "สบายดี :)", "เริ่มเครียด: ", "แย่แล้ว!: "};
    const alert_screen_t *screen = &farm->screen;
    const bento_real_t chart_max = (bento_real_t)CHART_MAX_C;
    const bento_real_t shown = (t > chart_max) ? chart_max : t;
    char t_text[BENTO_FMT_BUF_SIZE];
    char h_text[BENTO_FMT_BUF_SIZE];
    char text[TEXT_BYTES];
    int32_t i;

    one_decimal(t, t_text);
    one_decimal(h, h_text);
    (void)snprintf(text, sizeof(text),
                   "%s C  %s %%  (แดด +%" PRId32 ")", t_text, h_text,
                   sun);
    show_text(screen->ui, screen->reading, text);
    (void)snprintf(text, sizeof(text), "%s%s", moods[verdict->level],
                   (verdict->level == LEVEL_FINE) ? "" : verdict->thai);
    show_colored(screen->ui, screen->mood, LEVEL_COLORS[verdict->level],
                 text);
    for (i = 0; i < LEVEL_COUNT; i++) {
        show_value(screen->ui, screen->lamps[i],
                   (i == verdict->level) ? 1 : 0);
    }
    /* set_next() is fire-and-forget, as in modui.c */
    (void)bento_ui_chart_set_next(
        screen->ui, screen->chart, 0U,
        (shown > (bento_real_t)0.0) ? (int32_t)shown : 0);
    (void)bento_ui_chart_set_next(screen->ui, screen->chart,
                                  screen->hot_series,
                                  farm->watch.limits[LIMIT_T_HIGH]);
}

/* The lesson's polite end: the marquee and the box go first. */
static bento_status_t stop(farm_t *farm, const char *text,
                           uint32_t color)
{
    bento_status_t status;

    show_link(&farm->screen, false);
    status = matrix_off();
    if (status == BENTO_OK) {
        close_box(&farm->screen);
        show_status(&farm->screen, text, color);
        refresh(farm->screen.ui);
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
    static const char note[] = "MQTT จาก C\nยังไม่มีใน BENTO "
                               "Emulator\nให้รันบน Dev Kit\nหรือ Eva Kit";
    static const box_t place = {534,        140, 236, 104,
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
            show_status(&farm->screen, text, COLOR_WARN);
            refresh(farm->screen.ui);
        }
        status = try_broker(&farm->names, &farm->broker, linked);
    }
    return status;
}

/* WiFi, an address, the broker, then the subscription; *problem
 * names the step that failed, "" when all passed. */
static bento_status_t connect_farm(farm_t *farm, const char **problem)
{
    uint8_t granted_qos = 0U;
    bool passed = false;
    bento_status_t status;

    show_status(&farm->screen, "ต่อ WiFi...", COLOR_WARN);
    refresh(farm->screen.ui); /* the note must be up before the wait */
    status = join_wifi(&passed);
    *problem = "WiFi ไม่ติด: ตรวจชื่อ/รหัส";
    if ((status == BENTO_OK) && passed) {
        status = connect_broker(farm, &passed);
        status = emulator_gap(farm->screen.ui, status);
        *problem = "broker ไม่ตอบ: รอ 1 นาทีแล้วรันใหม่";
    }
    if ((status == BENTO_OK) && passed) {
        passed = bento_mqtt_subscribe(
                     farm->broker, farm->names.topic_cmd,
                     (uint32_t)strlen(farm->names.topic_cmd), 0U,
                     &granted_qos) == BENTO_OK;
    }
    if (passed) {
        *problem = "";
    }
    return status;
}

/* round(x, 1) at the board's float width, as str() prints it. */
static void tenth_text(bento_real_t value, char *text)
{
    const bento_real_t scale = (bento_real_t)TENTHS;
    const bento_real_t scaled = value * scale;
    uint32_t length = 0U;

    (void)bento_fmt_real(
        text, BENTO_FMT_BUF_SIZE,
        (bento_real_t)nearbyint((double)scaled) / scale, &length);
}

/* The alert (MQTT contract 3.4). A lost link makes publish() raise
 * OSError: false here. */
static bool send_alert(const farm_t *farm, const verdict_t *verdict,
                       bento_real_t t, bento_real_t h, int32_t sun)
{
    char t_text[BENTO_FMT_BUF_SIZE];
    char h_text[BENTO_FMT_BUF_SIZE];
    char payload[PAYLOAD_BYTES];

    tenth_text(t, t_text);
    tenth_text(h, h_text);
    (void)snprintf(
        payload, sizeof(payload),
        "{\"id\": \"%s\", \"crop\": \"%s\", \"level\": %" PRId32
        ", \"alert\": \"%s\", \"temp_c\": %s, \"rh\": %s"
        ", \"sun_c\": %" PRId32 "}",
        TEAM, farm->watch.crop->code, verdict->level, verdict->english,
        t_text, h_text, sun);
    return bento_mqtt_publish(farm->broker, farm->names.topic_event,
                              (uint32_t)strlen(farm->names.topic_event),
                              (const uint8_t *)payload,
                              (uint32_t)strlen(payload), 0U,
                              0U) != BENTO_E_SESSION;
}

/* "bad" opens the box; a MsgBox holds at most 126 bytes. */
static bento_status_t open_box(farm_t *farm, const verdict_t *verdict)
{
    const box_t place = {BOX_X, BOX_Y, BOX_W, BOX_H, 0U, 0, 0, 0};
    char text[TEXT_BYTES];
    bento_status_t status;

    (void)snprintf(text, sizeof(text), "แย่แล้ว!\n%s\n%s / ack = รับทราบ",
                   verdict->thai, BUTTON_NAMES[1]);
    status = add_box(farm->screen.ui, UI_WIDGET_MSGBOX, &place, text,
                     &farm->screen.box);
    farm->screen.has_box = status == BENTO_OK;
    return status;
}

/* The level changed: the alert goes out first, then the tune, the
 * RGB matrix and the box. */
static bento_status_t on_level(farm_t *farm, const verdict_t *verdict,
                               bento_real_t t, bento_real_t h,
                               int32_t sun)
{
    static const char *const words[LEVEL_COUNT] = {"OK", "WARN",
                                                   "ALERT"};
    static const int32_t colors[LEVEL_COUNT] = {BENTO_RGBMATRIX_GREEN,
                                                BENTO_RGBMATRIX_YELLOW,
                                                BENTO_RGBMATRIX_RED};
    static const tune_t tunes[LEVEL_COUNT] = {TUNE_GOOD, TUNE_TAP,
                                              TUNE_EMPTY};
    const int32_t level = verdict->level;
    bento_status_t status = BENTO_OK;

    farm->watch.level = level;
    farm->watch.acked = level < LEVEL_BAD;
    farm->watch.alerts++;
    if (!send_alert(farm, verdict, t, h, sun)) {
        status = stop(farm, "สายหลุดตอนส่ง", COLOR_BAD);
    }
    if (status == BENTO_OK) {
        status = bento_rgbmatrix_scroll(words[level],
                                        (uint32_t)strlen(words[level]),
                                        colors[level], SCROLL_MS);
    }
    if (status == BENTO_OK) {
        status = beep(farm->screen.ui, tunes[level]);
    }
    if (status == BENTO_OK) {
        show_answer(farm, (level == LEVEL_BAD) ? "รอคนรับทราบ" : "ส่งแล้ว",
                    (level == LEVEL_BAD) ? COLOR_BAD : COLOR_DIM);
        close_box(&farm->screen);
    }
    if ((status == BENTO_OK) && (level == LEVEL_BAD)) {
        status = open_box(farm, verdict);
    }
    return status;
}

/* Read, judge, show, and alert when the level changed. *alerted
 * restarts the count to the next repeat beep. */
static bento_status_t read_and_judge(farm_t *farm, bool *alerted)
{
    uint16_t raw = 0U;
    bento_real_t t = (bento_real_t)0.0;
    bento_real_t h = (bento_real_t)0.0;
    verdict_t verdict;
    int32_t sun = 0;
    bento_status_t status = bento_pots_read(KNOB_SUN, &raw);

    *alerted = false;
    sun = ((int32_t)raw * SUN_MAX_C) / KNOB_FULL; /* VR3 = 0-10 C sun */
    farm->watch.has_reading =
        (status == BENTO_OK) && read_climate(&t, &h);
    if (farm->watch.has_reading) {
        t += (bento_real_t)sun;
        judge(t, h, farm->watch.limits, &verdict);
        show_reading(farm, t, h, sun, &verdict);
        *alerted = verdict.level != farm->watch.level;
    }
    if (*alerted) { /* sent, sounded and drawn only on a change */
        status = on_level(farm, &verdict, t, h, sun);
    }
    return status;
}

/* {"cmd":"set","t_hi":..}: a new heat limit, a whole number in a
 * sensible range (Python's bool counts as a whole number). */
static bento_status_t set_heat_limit(farm_t *farm,
                                     const command_t *command)
{
    const json_kind_t kind = command->fields[FIELD_T_HI].kind;
    int32_t limit = (kind == JSON_TRUE) ? 1 : 0;
    const bool whole = int_value(command, FIELD_T_HI, &limit) ||
                       (kind == JSON_TRUE) || (kind == JSON_FALSE);
    bento_status_t status = BENTO_OK;

    if (whole && (farm->watch.limits[LIMIT_T_LOW] < limit) &&
        (limit <= T_HI_MAX)) {
        farm->watch.limits[LIMIT_T_HIGH] = limit;
        farm->watch.level = LEVEL_NONE; /* judge again, alert at once */
        farm->watch.has_reading = false;
        show_crop(farm);
        status = beep(farm->screen.ui, TUNE_TAP);
    }
    return status;
}

/* A command from the app (MQTT contract 4): *acknowledged when it is an
 * acknowledgement. Anything else, or not a JSON object, is ignored. */
static bento_status_t on_command(farm_t *farm, bool *acknowledged)
{
    command_t command;
    bento_status_t status = BENTO_OK;

    read_command(g_message.payload, g_message.payload_len, &command);
    *acknowledged = (command.top == JSON_OBJECT) &&
                    value_is(&command, FIELD_CMD, "ack");
    if ((command.top == JSON_OBJECT) &&
        value_is(&command, FIELD_CMD, "set")) {
        status = set_heat_limit(farm, &command);
    }
    return status;
}

/* SW5 is the farmer's own acknowledgement, no network needed; a press
 * counts once, when it goes down. */
static bento_status_t read_confirm_button(farm_t *farm,
                                          bool *acknowledged)
{
    bool down = false;
    bento_status_t status =
        bento_buttons_pressed(BUTTON_CONFIRM, &down);

    if ((status == BENTO_OK) && down && !farm->was_down) {
        *acknowledged = true;
        if (farm->watch.acked) { /* nothing to acknowledge: say so */
            show_answer(farm, "ยังไม่มีเหตุเตือน", COLOR_DIM);
            status = beep(farm->screen.ui, TUNE_TAP);
        }
    }
    farm->was_down = down;
    return status;
}

/* Someone acknowledged: stop beeping, close the box, ACK on the RGB
 * matrix. */
static bento_status_t acknowledge(farm_t *farm)
{
    bento_status_t status;

    farm->watch.acked = true;
    close_box(&farm->screen);
    show_answer(farm, "รับทราบแล้ว", COLOR_OK);
    status = bento_rgbmatrix_scroll("ACK", (uint32_t)strlen("ACK"),
                                    BENTO_RGBMATRIX_CYAN, SCROLL_MS);
    if (status == BENTO_OK) {
        status = beep(farm->screen.ui, TUNE_TAP);
    }
    return status;
}

/* The sensors are read every second, the broker every 20 ms. */
static bento_status_t sense_and_beep(farm_t *farm, uint32_t now_ms)
{
    bool alerted = false;
    bento_status_t status = BENTO_OK;

    if (!farm->watch.has_reading ||
        (bento_time_ticks_diff(now_ms, farm->read_ms) >= READ_MS)) {
        farm->read_ms = now_ms;
        status = read_and_judge(farm, &alerted);
        if (alerted) {
            farm->beep_ms = now_ms;
        }
    }
    if ((status == BENTO_OK) && (farm->watch.level == LEVEL_BAD) &&
        !farm->watch.acked &&
        (bento_time_ticks_diff(now_ms, farm->beep_ms) >= BEEP_MS)) {
        farm->beep_ms = now_ms;
        status = beep(farm->screen.ui, TUNE_BAD);
    }
    return status;
}

static bento_status_t watch_once(farm_t *farm)
{
    const uint32_t now_ms = bento_time_ticks_ms();
    bool acknowledged = false;
    bento_status_t status = BENTO_OK;

    if (take_newest(farm->broker, &g_message)) {
        status = on_command(farm, &acknowledged);
    }
    if (status == BENTO_OK) {
        status = read_confirm_button(farm, &acknowledged);
    }
    if ((status == BENTO_OK) && acknowledged && !farm->watch.acked) {
        status = acknowledge(farm);
    }
    if (status == BENTO_OK) {
        refresh(farm->screen.ui);
        status = sense_and_beep(farm, now_ms);
    }
    if (status == BENTO_OK) {
        status = pause_ms(POLL_MS);
    }
    return status;
}

/* The crop named in CROP, or NULL when the table has no such name.
 * (Thai name, English code, C low, C high, %RH low, %RH high) */
static const crop_t *find_crop(void)
{
    static const crop_t crops[] = {
        {"มะเขือเทศ", "tomato", {20, 30, 60, 80}},
        {"ผักสลัด", "lettuce", {15, 25, 50, 70}},
        {"เห็ดนางฟ้า", "mushroom", {22, 28, 80, 95}},
        {"กล้วยไม้", "orchid", {22, 32, 60, 80}}};
    const crop_t *found = NULL;
    uint32_t i;

    for (i = 0U; (i < (uint32_t)(sizeof(crops) / sizeof(crops[0]))) &&
                 (found == NULL);
         i++) {
        if (strcmp(crops[i].thai, CROP) == 0) {
            found = &crops[i];
        }
    }
    return found;
}

static bento_status_t start_farm(farm_t *farm)
{
    const char *problem = "";
    bento_status_t status;

    /* no volume control on this firmware: skipped, as hasattr() does */
    (void)bento_ui_sfx_volume_set(farm->screen.ui, SPEAKER);
    status = build_screen(&farm->screen);
    farm->watch.crop = find_crop();
    if ((status == BENTO_OK) && (farm->watch.crop == NULL)) {
        status = stop(farm, "แก้ CROP ให้ตรงกับ CROPS", COLOR_BAD);
    }
    if (status == BENTO_OK) {
        (void)memcpy(farm->watch.limits, farm->watch.crop->limits,
                     sizeof(farm->watch.limits));
        show_crop(farm);
        status = team_is_set() ? BENTO_OK
                               : stop(farm, "แก้ TEAM ก่อน", COLOR_BAD);
    }
    if (status == BENTO_OK) {
        status = connect_farm(farm, &problem);
    }
    if ((status == BENTO_OK) && (problem[0] != '\0')) {
        status = stop(farm, problem, COLOR_BAD);
    }
    if (status == BENTO_OK) {
        show_link(&farm->screen, true);
        show_status(&farm->screen, "เฝ้าอยู่", COLOR_OK);
    }
    return status;
}

static bento_status_t run_watch(farm_t *farm)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    farm->read_ms = start_ms;
    farm->beep_ms = start_ms;
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = watch_once(farm);
    }
    if (status == BENTO_OK) {
        status = stop(farm, "จบรอบ", COLOR_DIM);
    }
    return status;
}

int main(void)
{
    farm_t farm;
    bento_status_t status;

    (void)memset(&farm, 0, sizeof(farm));
    (void)memset(&g_message, 0, sizeof(g_message));
    farm.screen.ui = bento_runtime_get_ui();
    farm.watch.level = LEVEL_NONE;
    farm.watch.acked = true;
    (void)snprintf(farm.names.client_id, sizeof(farm.names.client_id),
                   "bento-farm-%s", TEAM);
    (void)snprintf(farm.names.topic_event,
                   sizeof(farm.names.topic_event), "%s/%s/event", ROOT,
                   TEAM);
    (void)snprintf(farm.names.topic_cmd, sizeof(farm.names.topic_cmd),
                   "%s/%s/cmd", ROOT, TEAM);
    status = start_farm(&farm);
    if (status == BENTO_OK) {
        status = run_watch(&farm);
    }
    if (farm.broker != 0U) { /* closed every time, even after Stop */
        (void)bento_mqtt_close(farm.broker); /* a refusal was ignored */
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
