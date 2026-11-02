/*
 * sf2_07_field_station.c -- บอร์ดนี้เล่นเป็น "แปลงผัก": โหนดเซนเซอร์ + PLC
 * คุมปั๊ม
 * (This board plays the field: sensor nodes and a PLC for the pump)
 *
 * ส่งดิน (VR1) เข้า field/soil และถัง (VR4) เข้า field/tank ทุก 5 วินาที
 * ฟัง plc/cmd แล้วเปิดหรือปิดปั๊มตามกฎ PLC (เปิดไม่เกิน 30 วินาที ถังต่ำ
 * กว่า 10 % ไม่เปิด) แล้วส่ง plc/state ตอนเปลี่ยนและทุก 5 วินาที ไฟ
 * RGB_BLUE คือรีเลย์ปั๊ม SW5 (ปุ่มล่าง) หยุดฉุกเฉิน กลุ่มเพื่อนรัน
 * sf2_06_smart_gateway (TEAM เดียวกัน) แล้วหมุน VR1 ให้ดินแห้ง ดูว่า
 * Gateway สั่งปั๊มไหม แก้ WIFI_SSID WIFI_PASS และ TEAM ก่อนรัน
 * สัญญา MQTT: app/MQTT_CONTRACT_th.md
 *
 * Sends the soil (VR1) to field/soil and the tank (VR4) to field/tank
 * every 5 s, listens on plc/cmd and runs the pump by the PLC's own
 * rules (at most 30 s, never below 10 % in the tank), and reports
 * plc/state on every change and every 5 s. The RGB_BLUE light is the
 * pump relay; SW5 (the lower button) is an emergency stop. Another
 * team runs sf2_06_smart_gateway with the same TEAM; turn VR1 dry and
 * see whether the gateway orders water. Set WIFI_SSID, WIFI_PASS and
 * TEAM first.
 *
 * Twin of sf2_07_field_station.py
 * (aiot-development-for-smart-farm/s2/sf2_07_field_station.py).
 * Style: CLEAN_C.md. Differs from the original:
 *   - json.loads is a small reader here. It reads JSON as
 *     MicroPython's json does (a ',' or ':' is skipped like a space)
 *     and finds the top-level keys the program asks for; nesting
 *     deeper than JSON_DEPTH_MAX counts as not JSON. The payload's
 *     bytes are not checked as UTF-8.
 *   - json.dumps is snprintf: the keys come in the order written,
 *     where MicroPython's dict may order them differently (the web
 *     app reads them by name). A real number is printed as str()
 *     prints it; None is null.
 *   - wifi.connect() and mqtt.connect() print lines of their own; the
 *     C calls print nothing. An SSID or password the radio cannot
 *     take counts as a failed join, as it does there.
 *   - A broker try that fails closes its client before the next try:
 *     MicroPython's next mqtt.connect() does that itself.
 *     A subscribe on a link that has just dropped counts as refused.
 *   - get_message() keeps one message, the newest; the C client
 *     queues up to four, so each round takes them all and keeps the
 *     newest.
 *   - In the BENTO Emulator, Edge AI/MQTT from C is not available yet:
 *     the program says so and ends, through the original's own
 *     "broker does not answer" stop.
 *   - The lesson's Stop exception travels as BENTO_E_CANCELLED, which
 *     main() turns into 0 as it does the IDE's Stop button.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_buttons.h"
#include "bento_gpio.h"
#include "bento_mqtt.h"
#include "bento_pots.h"
#include "bento_runtime.h"
#include "bento_time.h"
#include "bento_wifi.h"

/* ---- แก้ก่อนรัน: settings (MISRA deviation CT02) ---- */
/* ตั้งเอง: อังกฤษ/ตัวเลขสั้น ๆ ไม่มีเว้นวรรค */
static const char WIFI_SSID[] = "<ชื่อ Hotspot ของกลุ่ม>";
/* อย่างน้อย 8 ตัว · อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร */
static const char WIFI_PASS[] = "<รหัส Hotspot ของกลุ่ม>";
/* เลขกลุ่มที่เป็น Gateway (ต้องตรงกันทั้งสองบอร์ด) */
static const char TEAM[] = "teamXX";

static const char BROKER[] = "broker.hivemq.com";
/* T_BASE = ROOT/TEAM/ */
static const char ROOT[] = "bento-aiot";
/* + TEAM + เลขจากนาฬิกาบอร์ดทุกครั้งที่ต่อ: ไม่ชน id เก่า */
static const char CLIENT_ID[] = "bento-field-";

enum {
    PLC_MAX_S = 30, /* กฎความปลอดภัยของ PLC */
    PLC_TANK_MIN = 10,
    PLC_DEFAULT_S = 10,
    SEND_MS = 5000,
    POLL_MS = 100,
    RUN_MS = 1800000
};

/* .py: BTN_NAMES */
static const char *const BUTTON_NAMES[] = {"SW5", "SW6"};

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
    SAMPLE_MS = 20,  /* the firmware debounces for 50 ms */
    TONE_MS = 90,
    NOTE_MS = 100,
    MS_PER_S = 1000,
    KNOB_FULL = 4095,
    KNOB_SOIL = 0, /* VR1 */
    KNOB_TANK = 3, /* VR4 */
    PERCENT = 100,
    BROKER_TRIES = 3,
    PORT = 1883, /* not encrypted: anyone can read the topics */
    KEEPALIVE_S = 60,
    CLIENT_CLOCK_MASK = 0xFFFF,
    WIFI_SLICE_MS = 50,
    MQTT_SLICE_MS = 20,
    TEAM_CHARS = 6, /* "team" and two digits */
    TEAM_PREFIX = 4,
    STATE_LAG_PARTS = 4, /* the state goes out 1/4 of a period late */
    BUTTON_STOP = 1      /* SW5 */
};

/* Text buffers, each sized for the longest text built in it. */
enum {
    TEXT_BYTES = 128,
    NAME_BYTES = 64,
    KEY_BYTES = 16,
    PAYLOAD_BYTES = 128
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

typedef struct {
    bool found;
    uint32_t index;
} board_led_t;

/* The LED is found by its name, not its number; a board without that
 * name lends LED 0, and one whose LED cannot be switched has none. */
static board_led_t led_named(const char *name)
{
    const uint32_t count = bento_gpio_num_leds();
    board_led_t led = {false, 0U};
    bool named = false;
    uint32_t i;

    for (i = 0U; (i < count) && !named; i++) {
        const char *led_name = NULL;

        named = (bento_gpio_led_name(i, &led_name) == BENTO_OK) &&
                (led_name != NULL) && (strcmp(led_name, name) == 0);
        if (named) {
            led.index = i;
        }
    }
    led.found =
        (count != 0U) && (bento_gpio_led_off(led.index) == BENTO_OK);
    return led;
}

static bento_status_t set_led(const board_led_t *led, bool on)
{
    bento_status_t status = BENTO_OK;

    if (led->found) {
        status = on ? bento_gpio_led_on(led->index)
                    : bento_gpio_led_off(led->index);
    }
    return status;
}

static bento_status_t knob_percent(uint32_t knob, int32_t *percent)
{
    uint16_t raw = 0U;
    const bento_status_t status = bento_pots_read(knob, &raw);

    *percent = ((int32_t)raw * PERCENT) / KNOB_FULL;
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
    FIELD_PUMP,
    FIELD_SEC,
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
    static const char *const field_names[FIELD_COUNT] = {"pump", "sec"};
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

/* A number is zero when no digit before its exponent is. */
static bool number_is_zero(const uint8_t *text, uint32_t len)
{
    bool zero = true;
    uint32_t i;

    for (i = 0U; (i < len) && !is_exponent(text[i]); i++) {
        zero =
            zero && !(is_digit(text[i]) && (text[i] != (uint8_t)'0'));
    }
    return zero;
}

/* Python's truth of a field; a missing one is missing_truth. */
static bool value_truth(const command_t *command, field_t field,
                        bool missing_truth)
{
    const json_value_t *value = &command->fields[field];
    const uint8_t *text = &command->text[value->start];
    const uint32_t length = value->end - value->start;
    bool truth = missing_truth;

    if ((value->kind == JSON_INT) || (value->kind == JSON_FLOAT)) {
        truth = !number_is_zero(text, length);
    } else if (value->kind == JSON_STRING) {
        truth = length > 2U; /* more than the two quotes */
    } else if (is_container(value->kind)) {
        uint32_t i = 1U;

        while ((i < length) && is_separator(text[i])) {
            i++;
        }
        truth = (i < length) && (text[i] != (uint8_t)']') &&
                (text[i] != (uint8_t)'}');
    } else if (value->kind != JSON_MISSING) {
        truth = value->kind == JSON_TRUE;
    } else {
        /* missing: the default */
    }
    return truth;
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

/* ---- 3) สมอง (ตัดสินใจ) ---- */

static bool team_is_set(void)
{
    return (strlen(TEAM) == (size_t)TEAM_CHARS) &&
           (strncmp(TEAM, "team", (size_t)TEAM_PREFIX) == 0) &&
           is_digit((uint8_t)TEAM[TEAM_PREFIX]) &&
           is_digit((uint8_t)TEAM[TEAM_PREFIX + 1]) &&
           (strcmp(TEAM, "team00") != 0);
}

/* A command from plc/cmd (MQTT contract 4.2): answers the reason, and
 * *has_run with *run_s the seconds to run (0 = off). true and false
 * are not whole numbers in the contract, though Python counts bool
 * as int. */
static const char *plc_decide(const uint8_t *raw, uint32_t len,
                              int32_t tank, bool *has_run,
                              int32_t *run_s)
{
    command_t command;
    const char *why = "on";
    int32_t seconds = PLC_DEFAULT_S;

    read_command(raw, len, &command);
    *has_run = false;
    *run_s = 0;
    if ((command.top != JSON_OBJECT) ||
        (command.fields[FIELD_PUMP].kind == JSON_MISSING)) {
        why = "bad_cmd";
    } else if (!value_truth(&command, FIELD_PUMP, false)) {
        *has_run = true;
        why = "off";
    } else if (tank < PLC_TANK_MIN) {
        why = "blocked_tank";
    } else {
        if ((command.fields[FIELD_SEC].kind != JSON_MISSING) &&
            (!int_value(&command, FIELD_SEC, &seconds) ||
             (seconds <= 0))) {
            seconds = PLC_DEFAULT_S;
        }
        *has_run = true;
        *run_s = (seconds < PLC_MAX_S) ? seconds : PLC_MAX_S;
    }
    return why;
}

/* left_s rounded up, so it never shows 0 while the pump runs. */
static int32_t seconds_left(int32_t left_ms)
{
    return (left_ms > 0) ? ((left_ms + MS_PER_S - 1) / MS_PER_S) : 0;
}

/* ---- 4) เครือข่าย ---- */

typedef struct {
    char client_id[NAME_BYTES]; /* without the clock number */
    char base[NAME_BYTES];      /* bento-aiot/<TEAM>/ */
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

/* mqtt.is_connected(): an unknown state counts as dropped. */
static bool link_is_up(uint32_t broker)
{
    uint32_t state = BENTO_MQTT_STATE_CLOSED;

    if (bento_mqtt_state(broker, &state) != BENTO_OK) {
        state = BENTO_MQTT_STATE_CLOSED;
    }
    return state == BENTO_MQTT_STATE_CONNECTED;
}

/* A lost link makes publish() raise OSError; that is false here. A
 * refused message is not, as the original ignores publish()'s
 * answer. */
static bool send(uint32_t broker, const names_t *names,
                 const char *topic, const char *payload)
{
    char full_topic[NAME_BYTES];

    (void)snprintf(full_topic, sizeof(full_topic), "%s%s", names->base,
                   topic);
    return bento_mqtt_publish(
               broker, full_topic, (uint32_t)strlen(full_topic),
               (const uint8_t *)payload, (uint32_t)strlen(payload), 0U,
               0U) != BENTO_E_SESSION;
}

/* ---- 5) หน้าจอ ---- */

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t status;
    bento_ui_id_t soil_bar;
    bento_ui_id_t soil;
    bento_ui_id_t tank_bar;
    bento_ui_id_t tank;
    bento_ui_id_t sent;
    bento_ui_id_t relay;
    bento_ui_id_t pump;
    bento_ui_id_t why;
    bento_ui_id_t arc;
    bento_ui_id_t got;
    bento_ui_id_t note;
    bento_ui_id_t link_led;
    bento_ui_id_t link_text;
} field_screen_t;

static bento_status_t build_node_card(field_screen_t *screen)
{
    static const box_t card = {12, 44, 380, 130, 0U, 0, 0, 0};
    static const spot_t soil_name = {24, 84, COLOR_TEXT, 20};
    static const box_t soil_bar = {110, 88, 200, 20, 0U, 0, 0, 0};
    static const spot_t soil = {320, 86, COLOR_TEXT, 16};
    static const spot_t tank_name = {24, 122, COLOR_TEXT, 20};
    static const box_t tank_bar = {110, 126, 200, 20, 0U, 0, 0, 0};
    static const spot_t tank = {320, 124, COLOR_TEXT, 16};
    static const spot_t sent = {250, 50, COLOR_DIM, 20};
    bento_ui_id_t unused;
    bento_status_t status =
        add_card(screen->ui, &card, "โหนดเซนเซอร์ (ส่งทุก 5 วิ)", &unused);

    if (status == BENTO_OK) {
        status = add_label(screen->ui, &soil_name, "ดิน VR1", &unused);
    }
    if (status == BENTO_OK) {
        status = add_box(screen->ui, UI_WIDGET_BAR, &soil_bar, NULL,
                         &screen->soil_bar);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &soil, "-- %", &screen->soil);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &tank_name, "ถัง VR4", &unused);
    }
    if (status == BENTO_OK) {
        status = add_box(screen->ui, UI_WIDGET_BAR, &tank_bar, NULL,
                         &screen->tank_bar);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &tank, "-- %", &screen->tank);
    }
    if (status == BENTO_OK) {
        status =
            add_label(screen->ui, &sent, "ส่งแล้ว 0 ใบ", &screen->sent);
    }
    return status;
}

static bento_status_t build_plc_card(field_screen_t *screen)
{
    static const box_t card = {12, 182, 380, 152, 0U, 0, 0, 0};
    static const box_t relay = {28, 222, 44, 44, COLOR_INFO, 0, 0, 0};
    static const spot_t pump = {84, 230, COLOR_DIM, 20};
    static const spot_t why = {28, 272, COLOR_DIM, 16};
    static const box_t arc = {270, 208, 110, 110, 0U, 0, PLC_MAX_S, 0};
    bento_ui_id_t unused;
    bento_status_t status =
        add_card(screen->ui, &card, "PLC (รีเลย์ = ไฟสีฟ้า)", &unused);

    if (status == BENTO_OK) {
        status = add_box(screen->ui, UI_WIDGET_LED, &relay, NULL,
                         &screen->relay);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &pump, "ปั๊มหยุด", &screen->pump);
    }
    if (status == BENTO_OK) {
        status =
            add_label(screen->ui, &why, "why = start", &screen->why);
    }
    if (status == BENTO_OK) {
        status = add_box(screen->ui, UI_WIDGET_ARC, &arc, NULL,
                         &screen->arc);
    }
    return status;
}

static bento_status_t build_rules_card(field_screen_t *screen)
{
    static const spot_t button = {28, 302, COLOR_WARN, 20};
    static const box_t card = {402, 44, 378, 290, 0U, 0, 0, 0};
    static const spot_t rules = {414, 86, COLOR_TEXT, 20};
    static const spot_t got = {414, 126, COLOR_DIM, 20};
    char text[TEXT_BYTES];
    bento_ui_id_t unused;
    bento_status_t status;

    (void)bento_ui_set_color(screen->ui, screen->arc, COLOR_OK);
    (void)snprintf(text, sizeof(text), "%s = หยุดฉุกเฉิน",
                   BUTTON_NAMES[0]);
    status = add_label(screen->ui, &button, text, &unused);
    if (status == BENTO_OK) {
        status = add_card(screen->ui, &card,
                          "กฎของ PLC (ตรวจก่อนทุกคำสั่ง)", &unused);
    }
    if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text),
                       "เปิดสูงสุด %d วิ / ถัง < %d%% ไม่เปิด", PLC_MAX_S,
                       PLC_TANK_MIN);
        status = add_label(screen->ui, &rules, text, &unused);
    }
    if (status == BENTO_OK) {
        status =
            add_label(screen->ui, &got, "รับคำสั่งแล้ว 0", &screen->got);
    }
    return status;
}

static bento_status_t build_screen(field_screen_t *screen)
{
    static const spot_t heading = {12, 6, COLOR_TEXT, 24};
    static const spot_t status_line = {240, 12, COLOR_DIM, 20};
    static const spot_t note = {12, 352, COLOR_DIM, 20};
    static const box_t link_led = {586, 12, 18, 18, COLOR_OK, 0, 0, 0};
    static const spot_t link_text = {612, 10, COLOR_DIM, 20};
    bento_ui_id_t unused;
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = pause_ms(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &heading, "แปลงผัก: โหนด + PLC",
                           &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, &status_line, "กำลังเริ่ม",
                           &screen->status);
    }
    if (status == BENTO_OK) {
        status = build_node_card(screen);
    }
    if (status == BENTO_OK) {
        status = build_plc_card(screen);
    }
    if (status == BENTO_OK) {
        status = build_rules_card(screen);
    }
    if (status == BENTO_OK) {
        status =
            add_label(screen->ui, &note, "ยังไม่มีคำสั่ง", &screen->note);
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

/* The status line reaches the screen at once. */
static void show_status(const field_screen_t *screen, const char *text,
                        uint32_t color)
{
    show_colored(screen->ui, screen->status, color, text);
    refresh(screen->ui);
}

static void show_link(const field_screen_t *screen, bool linked)
{
    show_value(screen->ui, screen->link_led, linked ? 1 : 0);
    show_text(screen->ui, screen->link_text,
              linked ? "MQTT: เชื่อมต่อแล้ว" : "MQTT: ออฟไลน์");
}

static void show_knobs(const field_screen_t *screen, int32_t soil,
                       int32_t tank)
{
    char text[TEXT_BYTES];

    show_value(screen->ui, screen->soil_bar, soil);
    (void)snprintf(text, sizeof(text), "%" PRId32 " %%", soil);
    show_text(screen->ui, screen->soil, text);
    show_value(screen->ui, screen->tank_bar, tank);
    (void)bento_ui_set_color(screen->ui, screen->tank_bar,
                             (tank >= PLC_TANK_MIN) ? COLOR_INFO
                                                    : COLOR_BAD);
    (void)snprintf(text, sizeof(text), "%" PRId32 " %%", tank);
    show_text(screen->ui, screen->tank, text);
}

static void show_relay(const field_screen_t *screen, int32_t left_s,
                       const char *why)
{
    char text[TEXT_BYTES];

    show_value(screen->ui, screen->relay, (left_s != 0) ? 1 : 0);
    (void)bento_ui_set_color(screen->ui, screen->pump,
                             (left_s != 0) ? COLOR_OK : COLOR_DIM);
    if (left_s != 0) {
        (void)snprintf(text, sizeof(text), "ปั๊มเดิน อีก %" PRId32 " วิ",
                       left_s);
        show_text(screen->ui, screen->pump, text);
    } else {
        show_text(screen->ui, screen->pump, "ปั๊มหยุด");
    }
    show_value(screen->ui, screen->arc, left_s);
    (void)snprintf(text, sizeof(text), "why = %s", why);
    show_text(screen->ui, screen->why, text);
}

/* ---- 6) โปรแกรมหลัก ---- */

typedef struct {
    int32_t run_ms; /* 0: the pump is off */
    uint32_t on_ms;
    const char *why;
    int32_t n;
} plc_t;

/* The two nodes and the PLC's heartbeat, a share of a period apart:
 * the gateway's inbox has one slot. */
enum {
    DUE_SOIL,
    DUE_TANK,
    DUE_STATE,
    DUE_COUNT
};

typedef struct {
    field_screen_t screen;
    names_t names;
    uint32_t broker;
    board_led_t relay;
    button_t stop_button;
    plc_t plc;
    int32_t got;
    int32_t counts[2]; /* soil, tank */
    int32_t due_ms[DUE_COUNT];
    int32_t shown;
    bool was_on;
    uint32_t start_ms;
    int32_t soil;
    int32_t tank;
} farm_t;

static int32_t plc_left(const plc_t *plc, uint32_t now_ms)
{
    return (plc->run_ms != 0)
               ? (plc->run_ms -
                  bento_time_ticks_diff(now_ms, plc->on_ms))
               : 0;
}

/* plc/state (MQTT contract 3.6), with why, or the PLC's own why when
 * NULL. */
static bool report_plc(farm_t *farm, uint32_t now_ms, const char *why)
{
    char payload[PAYLOAD_BYTES];
    const int32_t left_s = seconds_left(plc_left(&farm->plc, now_ms));

    farm->plc.n++;
    (void)snprintf(payload, sizeof(payload),
                   "{\"pump\": %d, \"left_s\": %" PRId32
                   ", \"why\": \"%s\", \"n\": %" PRId32 "}",
                   (left_s != 0) ? 1 : 0, left_s,
                   (why != NULL) ? why : farm->plc.why, farm->plc.n);
    return send(farm->broker, &farm->names, "plc/state", payload);
}

static bento_status_t stop(farm_t *farm, const char *text,
                           uint32_t color)
{
    bento_status_t status;

    show_link(&farm->screen, false);
    status = set_led(&farm->relay, false);
    if (status == BENTO_OK) {
        show_status(&farm->screen, text, color);
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
    static const box_t place = {414,        170, 360, 56,
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
static bento_status_t connect_station(farm_t *farm,
                                      const char **problem)
{
    uint8_t granted_qos = 0U;
    bool passed = false;
    bento_status_t status;

    show_status(&farm->screen, "กำลังต่อ WiFi จอจะนิ่งสักครู่", COLOR_WARN);
    status = join_wifi(&passed);
    *problem = "ต่อ WiFi ไม่ได้ ตรวจชื่อวงกับรหัส";
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

static bento_status_t start_station(farm_t *farm)
{
    const char *problem = "";
    char text[TEXT_BYTES];
    bento_status_t status;

    /* no volume control on this firmware: skipped, as hasattr() does */
    (void)bento_ui_sfx_volume_set(farm->screen.ui, SPEAKER);
    status = build_screen(&farm->screen);
    if (status == BENTO_OK) {
        farm->relay = led_named("RGB_BLUE");
        status = team_is_set()
                     ? BENTO_OK
                     : stop(farm, "แก้ TEAM เป็นเลขกลุ่มที่เป็น Gateway",
                            COLOR_BAD);
    }
    if (status == BENTO_OK) {
        status = connect_station(farm, &problem);
    }
    if ((status == BENTO_OK) && (problem[0] != '\0')) {
        status = stop(farm, problem, COLOR_BAD);
    }
    if (status == BENTO_OK) {
        show_link(&farm->screen, true);
        (void)snprintf(text, sizeof(text), "แปลงของ %s ออนไลน์", TEAM);
        show_status(&farm->screen, text, COLOR_OK);
    }
    return status;
}

/* SW5 stops the pump without the network. Each plc/state report's
 * answer is not used, as in the original: the link check at the end
 * of the round ends the run. */
static bento_status_t emergency_stop(farm_t *farm, uint32_t now_ms)
{
    bento_status_t status = BENTO_OK;

    if (pressed_now(&farm->stop_button) && (farm->plc.run_ms != 0)) {
        farm->plc.run_ms = 0;
        farm->plc.why = "stop";
        status = beep(farm->screen.ui, TUNE_STOP);
        if (status == BENTO_OK) {
            (void)report_plc(farm, now_ms, NULL);
        }
    }
    return status;
}

/* A command from the gateway: check, act, sound, then report at
 * once. */
static bento_status_t on_command(farm_t *farm, uint32_t now_ms)
{
    char text[TEXT_BYTES];
    bool has_run = false;
    int32_t run_s = 0;
    const char *why =
        plc_decide(g_message.payload, g_message.payload_len, farm->tank,
                   &has_run, &run_s);
    tune_t tune = TUNE_BAD;
    bento_status_t status;

    farm->plc.why = why;
    if (has_run) {
        farm->plc.run_ms = run_s * MS_PER_S;
        farm->plc.on_ms = now_ms;
    }
    if (strcmp(why, "on") == 0) {
        tune = TUNE_START;
    } else if (strcmp(why, "off") == 0) {
        tune = TUNE_STOP;
    } else {
        /* refused: the bad tune */
    }
    status = beep(farm->screen.ui, tune);
    if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text), "รับคำสั่งแล้ว %" PRId32,
                       farm->got);
        show_text(farm->screen.ui, farm->screen.got, text);
        (void)snprintf(text, sizeof(text), "คำสั่งล่าสุด -> %s", why);
        show_text(farm->screen.ui, farm->screen.note, text);
        if (!report_plc(farm, now_ms, NULL)) {
            status = stop(farm, "สายหลุด", COLOR_BAD);
        }
    }
    return status;
}

/* Time up, or the tank ran dry while running: the PLC stops itself. */
static bento_status_t pump_timeout(farm_t *farm, uint32_t now_ms,
                                   int32_t *left_ms)
{
    bento_status_t status = BENTO_OK;

    *left_ms = plc_left(&farm->plc, now_ms);
    if ((farm->plc.run_ms != 0) &&
        ((*left_ms <= 0) || (farm->tank < PLC_TANK_MIN))) {
        farm->plc.run_ms = 0;
        farm->plc.why = (*left_ms <= 0) ? "timeout" : "blocked_tank";
        *left_ms = 0;
        status = beep(farm->screen.ui, TUNE_STOP);
        if (status == BENTO_OK) {
            (void)report_plc(farm, now_ms, NULL);
        }
    }
    return status;
}

static bento_status_t show_pump(farm_t *farm, int32_t left_ms)
{
    const bool on = farm->plc.run_ms > 0;
    const int32_t left_s = (farm->plc.run_ms != 0)
                               ? ((left_ms + MS_PER_S - 1) / MS_PER_S)
                               : 0;
    const bento_status_t status = set_led(&farm->relay, on);

    if ((status == BENTO_OK) &&
        ((left_s != farm->shown) || (farm->was_on != on))) {
        farm->shown = left_s;
        farm->was_on = on;
        show_relay(&farm->screen, left_s, farm->plc.why);
    }
    return status;
}

/* The sensor nodes, one at a time, every 5 s, and the PLC's
 * heartbeat. */
static bento_status_t send_nodes(farm_t *farm, uint32_t now_ms)
{
    static const char *const keys[2] = {"soil", "tank"};
    const int32_t elapsed_ms =
        bento_time_ticks_diff(now_ms, farm->start_ms);
    const int32_t values[2] = {farm->soil, farm->tank};
    char payload[PAYLOAD_BYTES];
    char topic[KEY_BYTES];
    bento_status_t status = BENTO_OK;
    int32_t i;

    for (i = 0; (i < 2) && (status == BENTO_OK); i++) {
        if (elapsed_ms >= farm->due_ms[i]) {
            farm->due_ms[i] += SEND_MS;
            farm->counts[i]++;
            (void)snprintf(topic, sizeof(topic), "field/%s", keys[i]);
            (void)snprintf(payload, sizeof(payload),
                           "{\"node\": \"%s-1\", \"value\": %" PRId32
                           ", \"unit\": \"%%\", \"n\": %" PRId32 "}",
                           keys[i], values[i], farm->counts[i]);
            status = send(farm->broker, &farm->names, topic, payload)
                         ? BENTO_OK
                         : stop(farm, "สายหลุด", COLOR_BAD);
        }
    }
    return status;
}

static void count_sent(farm_t *farm, uint32_t now_ms)
{
    char text[TEXT_BYTES];

    (void)snprintf(text, sizeof(text), "ส่งแล้ว %" PRId32 " ใบ",
                   farm->counts[0] + farm->counts[1]);
    show_text(farm->screen.ui, farm->screen.sent, text);
    if (bento_time_ticks_diff(now_ms, farm->start_ms) >=
        farm->due_ms[DUE_STATE]) {
        farm->due_ms[DUE_STATE] += SEND_MS;
        (void)report_plc(farm, now_ms, "tick");
    }
}

static bento_status_t take_command(farm_t *farm, uint32_t now_ms)
{
    bento_status_t status = BENTO_OK;

    if (take_newest(farm->broker, &g_message)) {
        farm->got++;
        status = on_command(farm, now_ms);
    }
    return status;
}

static bento_status_t field_once(farm_t *farm)
{
    const uint32_t now_ms = bento_time_ticks_ms();
    int32_t left_ms = 0;
    bento_status_t status = knob_percent(KNOB_SOIL, &farm->soil);

    if (status == BENTO_OK) {
        status = knob_percent(KNOB_TANK, &farm->tank);
    }
    if (status == BENTO_OK) {
        status = emergency_stop(farm, now_ms);
    }
    if (status == BENTO_OK) {
        status = take_command(farm, now_ms);
    }
    if (status == BENTO_OK) {
        status = pump_timeout(farm, now_ms, &left_ms);
    }
    if (status == BENTO_OK) {
        status = show_pump(farm, left_ms);
    }
    if (status == BENTO_OK) {
        show_knobs(&farm->screen, farm->soil, farm->tank);
        status = send_nodes(farm, now_ms);
    }
    if (status == BENTO_OK) {
        count_sent(farm, now_ms);
        status = link_is_up(farm->broker)
                     ? BENTO_OK
                     : stop(farm, "สายหลุด", COLOR_BAD);
    }
    if (status == BENTO_OK) {
        refresh(farm->screen.ui);
        status = wait_ms(POLL_MS, &farm->stop_button, 1U);
    }
    return status;
}

static bento_status_t run_field(farm_t *farm)
{
    bento_status_t status = BENTO_OK;

    farm->start_ms = bento_time_ticks_ms();
    farm->due_ms[DUE_SOIL] = 0;
    farm->due_ms[DUE_STATE] =
        (int32_t)SEND_MS / (int32_t)STATE_LAG_PARTS;
    farm->due_ms[DUE_TANK] = SEND_MS / 2;
    (void)report_plc(farm, farm->start_ms,
                     NULL); /* see emergency_stop */
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(),
                                  farm->start_ms) < RUN_MS)) {
        status = field_once(farm);
    }
    if (status == BENTO_OK) {
        status = stop(farm, "จบรอบแปลง", COLOR_DIM);
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
    farm.stop_button.index = BUTTON_STOP;
    farm.plc.why = "start";
    farm.shown = -1;
    (void)snprintf(farm.names.client_id, sizeof(farm.names.client_id),
                   "%s%s", CLIENT_ID, TEAM);
    (void)snprintf(farm.names.base, sizeof(farm.names.base), "%s/%s/",
                   ROOT, TEAM);
    (void)snprintf(farm.names.topic_cmd, sizeof(farm.names.topic_cmd),
                   "%splc/cmd", farm.names.base);
    status = start_station(&farm);
    if (status == BENTO_OK) {
        status = run_field(&farm);
    }
    if (farm.broker != 0U) { /* closed every time, even after Stop */
        (void)bento_mqtt_close(farm.broker); /* a refusal was ignored */
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
