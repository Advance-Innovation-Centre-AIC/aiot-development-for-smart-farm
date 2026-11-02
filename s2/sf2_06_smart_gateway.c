/*
 * sf2_06_smart_gateway.c -- Smart IoT Gateway: ฟังแปลง ตัดสินใจ สั่ง PLC
 *
 * ฟังสามหัวข้อของกลุ่ม: field/+ (โหนดในแปลงส่งค่าดินและถังน้ำ) plc/state
 * (PLC รายงานปั๊ม) และ cmd (แอป) ดินต่ำกว่า 30% และน้ำในถังพอ = ส่ง plc/cmd
 * ให้ PLC รดน้ำ 10 วินาที เว้น 1 นาทีก่อนสั่งซ้ำ แอปสั่งปั๊ม ตั้งออโต้ เรียกคน
 * ที่ฟาร์ม (beep) และส่งข้อความขึ้นจอไฟ RGB (say) ได้เหมือน sf2_03
 * ไฟ PLC บนจอกับ RGB_BLUE ตาม plc/state ที่ PLC รายงาน ไม่ใช่ที่เราสั่ง
 * PLC เงียบเกิน 15 วินาที = "PLC หลุด!" SW5 สั่งรดน้ำ SW6 สลับออโต้
 * ลองเล่นกับ PLC Simulator ใน app/farm_web.html หรืออีกกลุ่มรัน
 * sf2_07_field_station.py (TEAM เดียวกัน) แก้ wifi_ssid, wifi_pass
 * (ใน join_wifi) และ TEAM ก่อนรัน
 *
 * The smart IoT gateway listens to three topics of the team: field/+
 * (field nodes send the soil and the tank), plc/state (the PLC
 * reports its pump) and cmd (the app). Soil below 30 % with enough
 * water in the tank sends plc/cmd: water for 10 s, then wait a
 * minute before the next order. The app can run the pump, switch the
 * auto mode, call someone at the farm (beep) and scroll a text on the
 * RGB matrix (say) as in sf2_03. The PLC lamp and RGB_BLUE follow
 * what the PLC reports, not what was ordered; a PLC silent for 15 s
 * is "PLC หลุด!". SW5 waters, SW6 switches the auto mode. Play with
 * the PLC Simulator in app/farm_web.html, or another team running
 * sf2_07_field_station.py with the same TEAM. Edit wifi_ssid,
 * wifi_pass (in join_wifi) and TEAM before running.
 *
 * Twin of sf2_06_smart_gateway.py (aiot-development-for-smart-farm/s2/sf2_06_smart_gateway.py). Style: CLEAN_C.md.
 * Where C differs:
 *   - json.loads: a small reader of one JSON object finds the keys
 *     the program reads at its top level ("cmd", "on", "sec",
 *     "text", "value", "pump", "why", "left_s"). A nested list or
 *     object is skipped by its brackets, not checked inside; a key
 *     spelt with escapes is not recognised; MicroPython's reader is
 *     looser (commas and colons count as spaces there), so some
 *     broken text it takes counts as no JSON here.
 *   - A JSON number is read at the board's float width (bento_real_t)
 *     for "value", "pump" and "left_s". A "left_s" that is not a
 *     number shows as 0 (the original's "%d" would raise); a "pump"
 *     of true, false or 1.0 is kept as 1 or 0 (the original keeps the
 *     value and would send it back as written).
 *   - str() of a value: a text, an integer, true, false and null
 *     read as MicroPython writes them; a real number, a list or an
 *     object shows as its JSON text. [:n] keeps whole characters
 *     (the reTerminal's way; the PSoC Edge's MicroPython cuts bytes).
 *   - json.dumps: built with snprintf, the keys in the order the .py
 *     writes them (MicroPython writes its dict's slot order); a text
 *     is escaped as MicroPython's json.dumps escapes it.
 *   - farm.field keeps every field/<name>; only soil and tank are
 *     ever read, so only they are kept here.
 *   - get_message() is one slot a newer message overwrites; the C
 *     client keeps a ring of BENTO_MQTT_RING_SLOTS, and each round
 *     takes them all and keeps the newest.
 *   - try/except OSError: a status. A broker client that cannot
 *     start, or a connect that fails, is the next try; a publish
 *     on a lost link is the original's False.
 *   - led_named(): a board without "RGB_BLUE" uses LED 0; an LED
 *     that cannot be switched off is the original's None.
 *   - ui.volume, ui.tone, Chart.prop() and Chart.set_next() are fire
 *     and forget in modui.c, so their status is ignored here too; a
 *     board without volume (E_UNSUPPORTED) is the original's
 *     hasattr() False.
 *   - rgbmatrix is on the Dev Kit only (elsewhere the .py stops at
 *     its import); here the matrix calls are skipped where
 *     bento_rgbmatrix_available() is false.
 *   - raise Stop is a stopped flag that ends the status chain; the
 *     finally's mqtt.disconnect() is the close at the end of main.
 *   - wifi.* and mqtt.* print console lines of their own in
 *     MicroPython; the C library prints none.
 *   - Stop ends the program quietly at the next sleep, where
 *     MicroPython raises KeyboardInterrupt.
 */
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_buttons.h"
#include "bento_fmt.h"
#include "bento_gpio.h"
#include "bento_mqtt.h"
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_time.h"
#include "bento_wifi.h"

/* ---- 1) ตั้งค่า (แก้ได้) ---- */
/* WIFI_SSID (wifi_ssid) และ WIFI_PASS (wifi_pass) อยู่ใน
 * join_wifi() ด้านล่าง แก้ที่นั่น */
static const char TEAM[] = "teamXX"; /* เลขกลุ่มที่ผู้สอนแจก */
/* BROKER (broker_name) and CLIENT_PREFIX (client_prefix) live in
 * open_broker(), TOPIC_ROOT (topic_root) in
 * main(), the SW6 name in add_history(): each is read there only. */
static const char WATER_BUTTON_NAME[] = "SW5"; /* BTN_NAMES[0] */

/* The rules and the times. */
enum {
    SOIL_MIN = 30,
    PUMP_SEC = 10,   /* how long to water */
    PUMP_MAX_S = 30, /* the cap sent to the PLC */
    TANK_MIN = 10, /* tank below this: no order (the PLC checks too) */
    COOLDOWN_MS = 60000,
    STALE_MS = 15000,
    SEND_MS = 5000,
    POLL_MS = 100,
    DRAW_MS = 500,
    RUN_MS = 1800000,
    SPEAKER = 40, /* speaker volume, percent */
    VOLUME = 25   /* a tone's velocity */
};

/* The course palette, 0xRRGGBB. */
enum {
    COL_TEXT = 0xE8EAED,
    COL_DIM = 0x9AA3AF,
    COL_CARD = 0x171B22,
    COL_OK = 0x30A46C,
    COL_WARN = 0xF5A623,
    COL_BAD = 0xE5484D,
    COL_INFO = 0x4A9EFF
};

/* The board, the network and the texts. */
enum {
    WATER_BUTTON = 1, /* SW5 is buttons index 1 */
    AUTO_BUTTON = 0,  /* SW6 is buttons index 0 */
    PERCENT = 100,
    SAMPLE_MS = 20,  /* how often the buttons are read while waiting */
    SETTLE_MS = 200, /* let the cleared screen settle */
    TONE_MS = 90,
    TONE_GAP_MS = 100,
    TUNE_NOTES_MAX = 3,
    BROKER_PORT = 1883,
    KEEPALIVE_S = 60,
    BROKER_TRIES = 3,
    CONNECT_POLL_MS = 20,
    CLIENT_SUFFIX_MASK = 0xFFFF,
    SCROLL_MS = 80,
    SAY_MAX = 20, /* letters the matrix gets */
    WHY_MAX = 16, /* letters of the PLC's reason */
    TEAM_LEN = 6,
    TEAM_DIGITS_AT = 4,
    CHART_POINTS = 400,
    CHART_WIDTH_MAX = 400,
    PUMP_LINE_ON = 90, /* the pump line on the chart */
    PUMP_LINE_OFF = 5,
    FIELD_PREFIX_LEN = 6, /* "field/" */
    REAL_DIGITS_MAX = 18, /* digits a double carries exactly */
    EXPONENT_MAX = 400,   /* beyond any double */
    DECIMAL = 10,
    TOPIC_COUNT = 3
};

/* Buffer sizes, in bytes. */
enum {
    TEXT_BYTES = 128,     /* a label text: 126 at most */
    NAME_BYTES = 64,      /* a topic */
    CLIENT_ID_BYTES = 32, /* the broker takes 23 */
    PAYLOAD_KEEP = 255,   /* what the MicroPython binding keeps */
    VALUE_BYTES = 256, /* a decoded value: never longer than its text */
    WORD_BYTES = 8,    /* the longest command word, and its NUL */
    SAY_BYTES = 24,
    WHY_BYTES = 72,     /* 16 characters of up to 4 bytes */
    QUOTED_BYTES = 112, /* the reason as JSON: 16 x 6 bytes + quotes */
    REPORT_BYTES = 256
};

/* SW5 or SW6, sampled while the program waits so a short press
 * counts. */
typedef struct {
    uint32_t index;
    bool down;
    bool clicked;
} button_t;

/* A number that may be missing: the original's None. */
typedef struct {
    bool known;
    int32_t value;
} maybe_t;

/* The last value of one field node and when it came. */
typedef struct {
    bool heard;
    int32_t value;
    uint32_t at_ms;
} field_t;

/* The widgets the program changes after drawing them. */
typedef struct {
    bento_ui_id_t arc;
    bento_ui_id_t soil;
    bento_ui_id_t tank_bar;
    bento_ui_id_t tank;
    bento_ui_id_t lamp;
    bento_ui_id_t plc;
    bento_ui_id_t auto_switch;
    bento_ui_id_t chart;
    uint8_t pump_series;
    bento_ui_id_t note;
    bento_ui_id_t link_lamp;
    bento_ui_id_t link_text;
} screen_t;

/* Everything one run of the program keeps (the original's Farm and
 * its widgets). */
typedef struct {
    bento_ui_t *ui;
    screen_t screen;
    button_t water_button;
    button_t auto_button;
    char base[NAME_BYTES]; /* bento-aiot/<team>/ */
    uint32_t broker;
    bool has_broker;
    uint32_t led;
    bool has_led;
    bool has_matrix;
    bool stopped; /* the original's raise Stop */
    field_t soil_node;
    field_t tank_node;
    maybe_t pump; /* what the PLC reports: None, 0 or 1 */
    bool plc_heard;
    uint32_t plc_ms;
    int32_t left;
    bool auto_mode;
    bool lost;
    int32_t sent; /* reports sent, the "n" key */
    uint32_t command_ms;
    uint32_t send_ms;
    uint32_t draw_ms;
} farm_t;

/* An order for the PLC: {"pump": 0} or {"pump": 1, "sec": n}. */
typedef struct {
    bool wanted;
    int32_t pump;
    int32_t sec;
} order_t;

/* Where a widget goes and what it starts with. */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    uint32_t color;
    int32_t min;
    int32_t max;
    int32_t value; /* a label's font size, else the first value */
} spec_t;

typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
} rect_t;

typedef bento_ui_status_t (*factory_t)(
    bento_ui_t *ui, const bento_ui_widget_args_t *args,
    bento_ui_id_t *out);

typedef enum {
    TUNE_TAP = 0,
    TUNE_START,
    TUNE_STOP,
    TUNE_GOOD,
    TUNE_BAD,
    TUNE_EMPTY,
    TUNE_HIT,
    TUNE_COUNT
} tune_t;

typedef struct {
    uint8_t notes[TUNE_NOTES_MAX];
    uint32_t count;
} tune_notes_t;

/* What a JSON value is. */
typedef enum {
    JSON_MISSING = 0,
    JSON_STRING,
    JSON_NUMBER,
    JSON_TRUE,
    JSON_FALSE,
    JSON_NULL,
    JSON_LIST,
    JSON_OBJECT
} json_kind_t;

/* A value: its kind and where its text is in the payload. */
typedef struct {
    json_kind_t kind;
    uint32_t start;
    uint32_t end; /* one past its last byte */
} json_value_t;

/* The keys the program reads from a message. */
enum {
    FIELD_CMD = 0,
    FIELD_ON,
    FIELD_SEC,
    FIELD_TEXT,
    FIELD_VALUE,
    FIELD_PUMP,
    FIELD_WHY,
    FIELD_LEFT,
    FIELD_COUNT
};

typedef struct {
    const uint8_t *text;
    uint32_t len;
    uint32_t at;
} json_reader_t;

/* One message as far as the program asks. */
typedef struct {
    const uint8_t *payload;
    bool is_object;
    uint32_t members; /* an empty object is false in Python */
    json_value_t fields[FIELD_COUNT];
} command_t;

typedef enum {
    ACT_OTHER = 0, /* missing, empty or unknown */
    ACT_PUMP,
    ACT_AUTO,
    ACT_BEEP,
    ACT_SAY,
    ACT_ACKNOWLEDGE
} act_t;

/* ---- 2) ฮาร์ดแวร์ ---- */
/* ---- 2) ฮาร์ดแวร์ ---- */
static int16_t px(int32_t value)
{
    return (int16_t)value;
}

static bento_status_t sleep_checked(uint32_t duration_ms)
{
    bento_status_t status = bento_time_sleep_ms(duration_ms);

    if ((status == BENTO_OK) && !bento_runtime_is_running()) {
        status = BENTO_E_CANCELLED;
    }
    return status;
}

static bento_status_t beep(bento_ui_t *ui, tune_t tune)
{
    /* MIDI notes of each sound: the team edits them here */
    static const tune_notes_t tunes[TUNE_COUNT] = {
        [TUNE_TAP] = {{76U, 0U, 0U}, 1U},
        [TUNE_START] = {{72U, 79U, 0U}, 2U},
        [TUNE_STOP] = {{79U, 72U, 0U}, 2U},
        [TUNE_GOOD] = {{72U, 79U, 84U}, 3U},
        [TUNE_BAD] = {{84U, 76U, 0U}, 2U},
        [TUNE_EMPTY] = {{84U, 76U, 69U}, 3U},
        [TUNE_HIT] = {{88U, 0U, 0U}, 1U}};
    const tune_notes_t *sound = &tunes[tune];
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (status == BENTO_OK) && (i < sound->count); i++) {
        /* fire and forget in modui.c: no status comes back there */
        (void)bento_ui_sfx_tone(ui, sound->notes[i],
                                (uint8_t)BENTO_UI_WAVE_SINE,
                                (uint8_t)VOLUME, (uint16_t)TONE_MS);
        status = sleep_checked(TONE_GAP_MS);
    }
    return status;
}

/* The LED with this name, else LED 0; false when it cannot be
 * switched off (the original's None). */
static bool find_led(const char *name, uint32_t *led)
{
    const uint32_t count = bento_gpio_num_leds();
    bool found = false;
    uint32_t i;

    *led = 0U;
    for (i = 0U; !found && (i < count); i++) {
        const char *led_name = NULL;

        if ((bento_gpio_led_name(i, &led_name) == BENTO_OK) &&
            (led_name != NULL) && (strcmp(led_name, name) == 0)) {
            *led = i;
            found = true;
        }
    }
    return (count != 0U) && (bento_gpio_led_off(*led) == BENTO_OK);
}

static bento_status_t set_led(const farm_t *farm, bool on)
{
    bento_status_t status = BENTO_OK;

    if (farm->has_led) {
        status = on ? bento_gpio_led_on(farm->led)
                    : bento_gpio_led_off(farm->led);
    }
    return status;
}

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

static bool take_click(button_t *button)
{
    const bool fired = button->clicked;

    button->clicked = false;
    return fired;
}

/* A wait that keeps reading SW5 and SW6, so a short press between
 * two rounds still counts. */
static bento_status_t wait_ms(button_t *water_button,
                              button_t *auto_button,
                              uint32_t duration_ms)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            (int32_t)duration_ms)) {
        status = sample_button(water_button);
        if (status == BENTO_OK) {
            status = sample_button(auto_button);
        }
        if (status == BENTO_OK) {
            status = sleep_checked(SAMPLE_MS);
        }
    }
    return status;
}

/* ---- 3) สมอง: อ่าน JSON ---- */
/* ---- 3) สมอง: อ่านคำสั่ง JSON ---- */
static bool is_digit(uint8_t byte)
{
    return (byte >= (uint8_t)'0') && (byte <= (uint8_t)'9');
}

static bool is_hex(uint8_t byte)
{
    return is_digit(byte) ||
           ((byte >= (uint8_t)'a') && (byte <= (uint8_t)'f')) ||
           ((byte >= (uint8_t)'A') && (byte <= (uint8_t)'F'));
}

static bool is_space(uint8_t byte)
{
    return (byte == (uint8_t)' ') || (byte == (uint8_t)'\t') ||
           (byte == (uint8_t)'\n') || (byte == (uint8_t)'\r');
}

static bool next_is(const json_reader_t *reader, char byte)
{
    return (reader->at < reader->len) &&
           (reader->text[reader->at] == (uint8_t)byte);
}

static void skip_space(json_reader_t *reader)
{
    while ((reader->at < reader->len) &&
           is_space(reader->text[reader->at])) {
        reader->at++;
    }
}

/* Bytes an escape takes: \u and four hex digits, any other one
 * byte more; 0 when the text ends first. */
static uint32_t escape_span(const json_reader_t *reader, uint32_t at)
{
    const uint32_t unicode_span = 6U;
    uint32_t span = ((at + 1U) < reader->len) ? 2U : 0U;
    uint32_t i;

    if ((span != 0U) && (reader->text[at + 1U] == (uint8_t)'u')) {
        span = ((at + unicode_span) <= reader->len) ? unicode_span : 0U;
    }
    for (i = 2U; (span == unicode_span) && (i < unicode_span); i++) {
        if (!is_hex(reader->text[at + i])) {
            span = 0U;
        }
    }
    return span;
}

/* A string, both quotes included; a raw control byte is not JSON. */
static bool skip_string(json_reader_t *reader)
{
    const uint8_t control_end = 0x20U;
    uint32_t at = reader->at + 1U;
    bool closed = false;
    bool ok = true;

    while (ok && !closed && (at < reader->len)) {
        const uint8_t byte = reader->text[at];
        uint32_t span = 1U;

        if (byte == (uint8_t)'"') {
            closed = true;
        } else if (byte == (uint8_t)'\\') {
            span = escape_span(reader, at);
            ok = span != 0U;
        } else {
            ok = byte >= control_end;
        }
        at += span;
    }
    reader->at = at;
    return ok && closed;
}

static bool skip_digits(json_reader_t *reader)
{
    const uint32_t start = reader->at;

    while ((reader->at < reader->len) &&
           is_digit(reader->text[reader->at])) {
        reader->at++;
    }
    return reader->at > start;
}

static bool skip_number(json_reader_t *reader)
{
    bool ok;

    if (next_is(reader, '-')) {
        reader->at++;
    }
    ok = skip_digits(reader);
    if (ok && next_is(reader, '.')) {
        reader->at++;
        ok = skip_digits(reader);
    }
    if (ok && (next_is(reader, 'e') || next_is(reader, 'E'))) {
        reader->at++;
        if (next_is(reader, '+') || next_is(reader, '-')) {
            reader->at++;
        }
        ok = skip_digits(reader);
    }
    return ok;
}

static bool skip_word(json_reader_t *reader, const char *word)
{
    const uint32_t length = (uint32_t)strlen(word);
    const bool same = ((reader->len - reader->at) >= length) &&
                      (strncmp((const char *)&reader->text[reader->at],
                               word, length) == 0);

    if (same) {
        reader->at += length;
    }
    return same;
}

/* A list or an object, skipped by its brackets (strings inside
 * may hold brackets of their own). */
static bool skip_container(json_reader_t *reader)
{
    uint32_t depth = 0U;
    bool ok = true;
    bool done = false;

    while (ok && !done) {
        const uint8_t byte =
            (reader->at < reader->len) ? reader->text[reader->at] : 0U;

        if (reader->at >= reader->len) {
            ok = false;
        } else if (byte == (uint8_t)'"') {
            ok = skip_string(reader);
        } else if ((byte == (uint8_t)'[') || (byte == (uint8_t)'{')) {
            depth++;
            reader->at++;
        } else if ((byte == (uint8_t)']') || (byte == (uint8_t)'}')) {
            depth--;
            reader->at++;
            done = depth == 0U;
        } else {
            reader->at++;
        }
    }
    return ok;
}

static bool read_value(json_reader_t *reader, json_value_t *value)
{
    const uint8_t first =
        (reader->at < reader->len) ? reader->text[reader->at] : 0U;
    bool ok = reader->at < reader->len;

    value->start = reader->at;
    value->kind = JSON_NUMBER;
    if (!ok) {
        value->kind = JSON_MISSING;
    } else if (first == (uint8_t)'"') {
        value->kind = JSON_STRING;
        ok = skip_string(reader);
    } else if ((first == (uint8_t)'[') || (first == (uint8_t)'{')) {
        value->kind = (first == (uint8_t)'[') ? JSON_LIST : JSON_OBJECT;
        ok = skip_container(reader);
    } else if (first == (uint8_t)'t') {
        value->kind = JSON_TRUE;
        ok = skip_word(reader, "true");
    } else if (first == (uint8_t)'f') {
        value->kind = JSON_FALSE;
        ok = skip_word(reader, "false");
    } else if (first == (uint8_t)'n') {
        value->kind = JSON_NULL;
        ok = skip_word(reader, "null");
    } else {
        ok = skip_number(reader);
    }
    value->end = reader->at;
    return ok;
}

/* The field a key names, FIELD_COUNT for a key the program does
 * not read. */
static uint32_t field_of(const json_reader_t *reader,
                         const json_value_t *key)
{
    static const char *const names[FIELD_COUNT] = {
        "cmd", "on", "sec", "text", "value", "pump", "why", "left_s"};
    const uint32_t length = key->end - key->start - 2U;
    uint32_t field = FIELD_COUNT;
    uint32_t i;

    for (i = 0U; (field == FIELD_COUNT) && (i < FIELD_COUNT); i++) {
        if ((strlen(names[i]) == length) &&
            (strncmp((const char *)&reader->text[key->start + 1U],
                     names[i], length) == 0)) {
            field = i;
        }
    }
    return field;
}

/* "key": value; a key given twice keeps the last value. */
static bool read_member(json_reader_t *reader, command_t *command)
{
    json_value_t key;
    json_value_t value;
    bool ok = next_is(reader, '"') && read_value(reader, &key);

    if (ok) {
        skip_space(reader);
        ok = next_is(reader, ':');
    }
    if (ok) {
        reader->at++;
        skip_space(reader);
        ok = read_value(reader, &value);
    }
    if (ok) {
        const uint32_t field = field_of(reader, &key);

        if (field < (uint32_t)FIELD_COUNT) {
            command->fields[field] = value;
        }
        command->members++;
        skip_space(reader);
    }
    return ok;
}

static void read_command(const uint8_t *payload, uint32_t len,
                         command_t *command)
{
    json_reader_t reader = {payload, len, 0U};
    bool ok;
    bool more;

    (void)memset(command, 0, sizeof(*command));
    command->payload = payload;
    skip_space(&reader);
    ok = next_is(&reader, '{');
    if (ok) {
        reader.at++;
        skip_space(&reader);
    }
    more = ok && !next_is(&reader, '}');
    while (ok && more) {
        ok = read_member(&reader, command);
        more = ok && next_is(&reader, ',');
        if (more) {
            reader.at++;
            skip_space(&reader);
        }
    }
    ok = ok && next_is(&reader, '}');
    if (ok) {
        reader.at++;
        skip_space(&reader);
    }
    command->is_object = ok && (reader.at == len);
}

/* ---- 3) สมอง: ค่าในข้อความ ---- */
/* ---- 3) สมอง: ค่าในคำสั่ง ---- */
static uint32_t put_byte(char *out, uint32_t cap, uint32_t length,
                         uint8_t byte)
{
    if ((length + 1U) < cap) {
        out[length] = (char)byte;
        out[length + 1U] = '\0';
    }
    return length + 1U;
}

/* One \u code point as UTF-8 (a lone surrogate as its 3 bytes, as
 * MicroPython writes it). */
static uint32_t put_code(char *out, uint32_t cap, uint32_t length,
                         uint32_t code)
{
    const uint32_t two_bytes_from = 0x80U;
    const uint32_t three_bytes_from = 0x800U;
    const uint32_t follow = 0x80U;
    const uint32_t two_lead = 0xC0U;
    const uint32_t three_lead = 0xE0U;
    const uint32_t six_bits = 0x3FU;
    const uint32_t shift = 6U;
    uint32_t used = length;

    if (code < two_bytes_from) {
        used = put_byte(out, cap, used, (uint8_t)code);
    } else if (code < three_bytes_from) {
        used = put_byte(out, cap, used,
                        (uint8_t)(two_lead | (code >> shift)));
        used = put_byte(out, cap, used,
                        (uint8_t)(follow | (code & six_bits)));
    } else {
        used = put_byte(out, cap, used,
                        (uint8_t)(three_lead | (code >> (shift * 2U))));
        used =
            put_byte(out, cap, used,
                     (uint8_t)(follow | ((code >> shift) & six_bits)));
        used = put_byte(out, cap, used,
                        (uint8_t)(follow | (code & six_bits)));
    }
    return used;
}

static uint32_t hex_code(const uint8_t *digits)
{
    const uint32_t ten = 10U;
    const uint32_t bits_per_digit = 4U;
    const uint32_t digit_count = 4U;
    const uint32_t lower_case_bit = 0x20U;
    uint32_t code = 0U;
    uint32_t i;

    for (i = 0U; i < digit_count; i++) {
        const uint8_t byte = digits[i];
        uint32_t digit = (uint32_t)byte - (uint32_t)'0';

        if (!is_digit(byte)) {
            digit =
                (((uint32_t)byte | lower_case_bit) - (uint32_t)'a') +
                ten;
        }
        code = (code << bits_per_digit) | digit;
    }
    return code;
}

static uint8_t escaped_byte(uint8_t letter)
{
    uint8_t byte = letter; /* \" \\ \/ and any other letter: itself */

    if (letter == (uint8_t)'b') {
        byte = (uint8_t)'\b';
    } else if (letter == (uint8_t)'f') {
        byte = (uint8_t)'\f';
    } else if (letter == (uint8_t)'n') {
        byte = (uint8_t)'\n';
    } else if (letter == (uint8_t)'r') {
        byte = (uint8_t)'\r';
    } else if (letter == (uint8_t)'t') {
        byte = (uint8_t)'\t';
    } else {
        /* kept as it is */
    }
    return byte;
}

/* The text of a JSON string, escapes decoded. Answers the whole
 * length, also when only cap - 1 bytes fit. */
static uint32_t decode_string(const command_t *command,
                              const json_value_t *value, char *out,
                              uint32_t cap)
{
    const uint32_t unicode_span = 6U;
    const uint8_t *text = command->payload;
    const uint32_t end = value->end - 1U;
    uint32_t at = value->start + 1U;
    uint32_t length = 0U;

    out[0] = '\0';
    while (at < end) {
        if (text[at] != (uint8_t)'\\') {
            length = put_byte(out, cap, length, text[at]);
            at++;
        } else if (text[at + 1U] == (uint8_t)'u') {
            length =
                put_code(out, cap, length, hex_code(&text[at + 2U]));
            at += unicode_span;
        } else {
            length =
                put_byte(out, cap, length, escaped_byte(text[at + 1U]));
            at += 2U;
        }
    }
    return length;
}

static void copy_text(char *out, uint32_t cap, const char *text)
{
    (void)snprintf(out, cap, "%s", text);
}

/* str() of a value. */
static void value_text(const command_t *command,
                       const json_value_t *value, char *out,
                       uint32_t cap)
{
    if (value->kind == JSON_STRING) {
        (void)decode_string(command, value, out, cap);
    } else if (value->kind == JSON_TRUE) {
        copy_text(out, cap, "True");
    } else if (value->kind == JSON_FALSE) {
        copy_text(out, cap, "False");
    } else if ((value->kind == JSON_NULL) ||
               (value->kind == JSON_MISSING)) {
        copy_text(out, cap, "None");
    } else {
        const uint32_t length = value->end - value->start;
        const uint32_t keep = (length < cap) ? length : (cap - 1U);

        (void)memcpy(out, &command->payload[value->start], keep);
        out[keep] = '\0';
    }
}

/* text[:keep], counted in whole UTF-8 characters. */
static void cut_text(char *text, uint32_t keep)
{
    const uint8_t follow_mask = 0xC0U;
    const uint8_t follow = 0x80U;
    uint32_t count = 0U;
    uint32_t i = 0U;
    bool cut = false;

    while (!cut && (text[i] != '\0')) {
        if (((uint8_t)text[i] & follow_mask) != follow) {
            count++;
        }
        cut = count > keep;
        if (cut) {
            text[i] = '\0';
        }
        i++;
    }
}

static bool number_is_zero(const command_t *command,
                           const json_value_t *value)
{
    bool zero = true;
    bool in_exponent = false; /* the exponent does not matter */
    uint32_t i = value->start;

    while (!in_exponent && (i < value->end)) {
        const uint8_t byte = command->payload[i];

        if ((byte == (uint8_t)'e') || (byte == (uint8_t)'E')) {
            in_exponent = true;
        } else if (is_digit(byte) && (byte != (uint8_t)'0')) {
            zero = false;
        } else {
            /* a sign, a dot or a zero */
        }
        i++;
    }
    return zero;
}

/* An empty list or object holds nothing but spaces. */
static bool container_has_members(const command_t *command,
                                  const json_value_t *value)
{
    bool members = false;
    uint32_t i;

    for (i = value->start + 1U; i < (value->end - 1U); i++) {
        members = members || !is_space(command->payload[i]);
    }
    return members;
}

/* Python's truth of a value; a missing "on" is 1. */
static bool value_is_true(const command_t *command,
                          const json_value_t *value)
{
    bool truth = true;

    if (value->kind == JSON_STRING) {
        truth = (value->end - value->start) > 2U;
    } else if (value->kind == JSON_NUMBER) {
        truth = !number_is_zero(command, value);
    } else if ((value->kind == JSON_FALSE) ||
               (value->kind == JSON_NULL)) {
        truth = false;
    } else if ((value->kind == JSON_LIST) ||
               (value->kind == JSON_OBJECT)) {
        truth = container_has_members(command, value);
    } else {
        /* true, or missing */
    }
    return truth;
}

/* A whole number above 0, else the default; never above the cap. */
static int32_t order_seconds(const command_t *command)
{
    const json_value_t *value = &command->fields[FIELD_SEC];
    const uint8_t *text = command->payload;
    bool whole = (value->kind == JSON_NUMBER) &&
                 (text[value->start] != (uint8_t)'-') &&
                 !number_is_zero(command, value);
    const int32_t decimal = 10;
    int32_t sec = 0;
    uint32_t i;

    for (i = value->start; whole && (i < value->end); i++) {
        whole = is_digit(text[i]);
        if (whole && (sec < PUMP_MAX_S)) {
            sec = (sec * decimal) + (int32_t)text[i] - (int32_t)'0';
        }
    }
    if (!whole) {
        sec = PUMP_SEC;
    }
    return (sec < PUMP_MAX_S) ? sec : PUMP_MAX_S;
}

/* The digits of a JSON number before its exponent, and the power of
 * ten they are off by. */
typedef struct {
    double digits;
    int32_t scale;
    uint32_t used;
} mantissa_t;

static void add_digit(mantissa_t *mantissa, uint8_t byte,
                      bool in_fraction)
{
    const double ten = 10.0;
    const int32_t digit = (int32_t)byte - (int32_t)'0';

    if (mantissa->used < (uint32_t)REAL_DIGITS_MAX) {
        mantissa->digits = (mantissa->digits * ten) + (double)digit;
        if (mantissa->digits > 0.0) {
            mantissa->used++;
        }
        if (in_fraction) {
            mantissa->scale--;
        }
    } else if (!in_fraction) {
        mantissa->scale++; /* a digit too many: only its place counts */
    } else {
        /* a fraction digit beyond what a double holds */
    }
}

/* Reads up to the exponent; answers where it stopped. */
static uint32_t scan_mantissa(const uint8_t *text, uint32_t start,
                              uint32_t end, mantissa_t *mantissa)
{
    bool in_fraction = false;
    uint32_t at = start;

    while ((at < end) && (text[at] != (uint8_t)'e') &&
           (text[at] != (uint8_t)'E')) {
        if (text[at] == (uint8_t)'.') {
            in_fraction = true;
        } else if (is_digit(text[at])) {
            add_digit(mantissa, text[at], in_fraction);
        } else {
            /* the sign, read by the caller */
        }
        at++;
    }
    return at;
}

/* The exponent after an e or E, kept within EXPONENT_MAX. */
static int32_t scan_exponent(const uint8_t *text, uint32_t start,
                             uint32_t end)
{
    bool negative = false;
    int32_t exponent = 0;
    uint32_t at;

    for (at = start + 1U; at < end; at++) {
        const bool digit = is_digit(text[at]);

        if (digit && (exponent < EXPONENT_MAX)) {
            exponent = (exponent * DECIMAL) +
                       ((int32_t)text[at] - (int32_t)'0');
        } else if (!digit) {
            negative = negative || (text[at] == (uint8_t)'-');
        } else {
            /* a digit past EXPONENT_MAX: already beyond any double */
        }
    }
    return negative ? -exponent : exponent;
}

/* A JSON number at the board's float width; true, false count as 1
 * and 0 (bool is an int in Python). false for anything else. */
static bool number_value(const command_t *command,
                         const json_value_t *value, bento_real_t *out)
{
    const uint8_t *text = command->payload;
    const bool number = value->kind == JSON_NUMBER;

    *out = (value->kind == JSON_TRUE) ? 1.0F : 0.0F;
    if (number) {
        const double ten = 10.0;
        mantissa_t mantissa = {0.0, 0, 0U};
        const uint32_t at =
            scan_mantissa(text, value->start, value->end, &mantissa);
        const int32_t exponent =
            (at < value->end) ? scan_exponent(text, at, value->end) : 0;
        const double size =
            pow(ten, (double)mantissa.scale + (double)exponent);
        const double magnitude = mantissa.digits * size;
        const double signed_value = (text[value->start] == (uint8_t)'-')
                                        ? -magnitude
                                        : magnitude;

        *out = (bento_real_t)signed_value;
    }
    return number || (value->kind == JSON_TRUE) ||
           (value->kind == JSON_FALSE);
}

/* json.dumps of a text: " and \ escaped, \n \r \t by name, other
 * control bytes as \\u00XX, everything else as it is. */
static void json_quote(const char *text, char *out, uint32_t cap)
{
    const uint8_t control_end = 0x20U;
    uint32_t used = put_byte(out, cap, 0U, (uint8_t)'"');
    uint32_t i;

    for (i = 0U; text[i] != '\0'; i++) {
        const uint8_t byte = (uint8_t)text[i];
        char piece[WORD_BYTES];

        if ((byte == (uint8_t)'"') || (byte == (uint8_t)'\\')) {
            (void)snprintf(piece, sizeof(piece), "\\%c", (char)byte);
        } else if (byte >= control_end) {
            (void)snprintf(piece, sizeof(piece), "%c", (char)byte);
        } else if (byte == (uint8_t)'\n') {
            copy_text(piece, sizeof(piece), "\\n");
        } else if (byte == (uint8_t)'\r') {
            copy_text(piece, sizeof(piece), "\\r");
        } else if (byte == (uint8_t)'\t') {
            copy_text(piece, sizeof(piece), "\\t");
        } else {
            (void)snprintf(piece, sizeof(piece), "\\u%04x",
                           (unsigned int)byte);
        }
        (void)snprintf(&out[used], cap - used, "%s", piece);
        used += (uint32_t)strlen(piece);
    }
    (void)snprintf(&out[used], cap - used, "\"");
}

static act_t act_of_word(const char *word)
{
    static const char *const words[] = {"pump", "auto", "beep", "say",
                                        "ack"};
    static const act_t acts[] = {ACT_PUMP, ACT_AUTO, ACT_BEEP, ACT_SAY,
                                 ACT_ACKNOWLEDGE};
    act_t act = ACT_OTHER;
    uint32_t i;

    for (i = 0U; i < (sizeof(acts) / sizeof(acts[0])); i++) {
        if (strcmp(word, words[i]) == 0) {
            act = acts[i];
        }
    }
    return act;
}

/* cmd.get("cmd") if cmd else None: an empty object asks for nothing. */
static act_t act_of(const command_t *command)
{
    const json_value_t *value = &command->fields[FIELD_CMD];
    act_t act = ACT_OTHER;

    if (command->is_object && (command->members != 0U) &&
        (value->kind == JSON_STRING)) {
        char word[WORD_BYTES];
        const uint32_t length =
            decode_string(command, value, word, sizeof(word));

        if (length < sizeof(word)) {
            act = act_of_word(word);
        }
    }
    return act;
}

/* Only the printable English letters: the matrix shows nothing
 * else. */
static void ascii_only(const command_t *command, char *out)
{
    const uint8_t first = (uint8_t)' ';
    const uint8_t last = (uint8_t)'~';
    char text[VALUE_BYTES];
    uint32_t length = 0U;
    uint32_t i;

    text[0] = '\0';
    if (command->fields[FIELD_TEXT].kind != JSON_MISSING) {
        value_text(command, &command->fields[FIELD_TEXT], text,
                   sizeof(text));
    }
    for (i = 0U; (text[i] != '\0') && (length < (uint32_t)SAY_MAX);
         i++) {
        const uint8_t byte = (uint8_t)text[i];

        if ((byte >= first) && (byte <= last)) {
            out[length] = text[i];
            length++;
        }
    }
    out[length] = '\0';
}

/* ---- 3) สมอง: ตัดสินใจ ---- */
/* The node's value while it is fresh, else None. */
static maybe_t field_now(const field_t *node, uint32_t now_ms)
{
    maybe_t now = {false, 0};

    if (node->heard &&
        (bento_time_ticks_diff(now_ms, node->at_ms) < STALE_MS)) {
        now.known = true;
        now.value = node->value;
    }
    return now;
}

/* The auto rule: auto on, dry soil, enough water, the PLC says the
 * pump stands still, and the wait since the last order is over. */
static bool should_water(const farm_t *farm, maybe_t soil, maybe_t tank,
                         int32_t since_order_ms)
{
    return farm->auto_mode && soil.known && (soil.value < SOIL_MIN) &&
           tank.known && (tank.value >= TANK_MIN) && farm->pump.known &&
           (farm->pump.value == 0) && (since_order_ms >= COOLDOWN_MS);
}

/* A command from the app (contract 4.1): an order for the PLC or
 * none, a new auto mode or none, and the note. */
static void app_request(const command_t *command, maybe_t tank,
                        order_t *order, maybe_t *auto_mode, char *note)
{
    const act_t act = act_of(command);

    order->wanted = false;
    auto_mode->known = false;
    if ((act == ACT_PUMP) &&
        !value_is_true(command, &command->fields[FIELD_ON])) {
        order->wanted = true;
        order->pump = 0;
        copy_text(note, TEXT_BYTES, "หยุดปั๊ม");
    } else if ((act == ACT_PUMP) &&
               (!tank.known || (tank.value < TANK_MIN))) {
        copy_text(note, TEXT_BYTES, "ถังน้ำไม่พอ");
    } else if (act == ACT_PUMP) {
        order->wanted = true;
        order->pump = 1;
        order->sec = order_seconds(command);
        (void)snprintf(note, TEXT_BYTES, "รดน้ำ %" PRId32 " วิ",
                       order->sec);
    } else if (act == ACT_AUTO) {
        auto_mode->known = true;
        auto_mode->value =
            value_is_true(command, &command->fields[FIELD_ON]) ? 1 : 0;
        copy_text(note, TEXT_BYTES, "ตั้งออโต้");
    } else {
        copy_text(note, TEXT_BYTES, "ไม่รู้จักคำสั่ง");
    }
}

/* SW5 is the app's {"cmd": "pump"}: the same rules, the default
 * time. */
static void water_request(maybe_t tank, order_t *order, char *note)
{
    order->wanted = (tank.known && (tank.value >= TANK_MIN));
    order->pump = 1;
    order->sec = PUMP_SEC;
    if (order->wanted) {
        (void)snprintf(note, TEXT_BYTES, "รดน้ำ %d วิ", PUMP_SEC);
    } else {
        copy_text(note, TEXT_BYTES, "ถังน้ำไม่พอ");
    }
}

/* ---- 5) หน้าจอ ---- */
/* MicroPython's widget setters ignore a refusal (modui.c). */
static void set_text(const farm_t *farm, bento_ui_id_t widget,
                     const char *text)
{
    (void)bento_ui_set_text(farm->ui, widget, text,
                            (uint32_t)strlen(text));
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

static void show_note(const farm_t *farm, const char *text,
                      uint32_t color)
{
    (void)bento_ui_set_color(farm->ui, farm->screen.note, color);
    set_text(farm, farm->screen.note, text);
}

static void show_link(const farm_t *farm, bool ok)
{
    (void)bento_ui_set_value(farm->ui, farm->screen.link_lamp,
                             ok ? 1 : 0);
    set_text(farm, farm->screen.link_text,
             ok ? "MQTT: เชื่อมต่อแล้ว" : "MQTT: ออฟไลน์");
}

static bento_status_t add_widget(bento_ui_t *ui, factory_t factory,
                                 const spec_t *spec, const char *text,
                                 bento_ui_id_t *out)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    if (text != NULL) {
        args.text = text;
        args.text_len = (uint32_t)strlen(text);
    }
    args.x = spec->x;
    args.y = spec->y;
    args.w = spec->w;
    args.h = spec->h;
    args.color = spec->color;
    args.min_val = spec->min;
    args.max_val = spec->max;
    args.font_px = spec->value;
    return factory(ui, &args, out);
}

static bento_status_t add_card(bento_ui_t *ui, const rect_t *place,
                               const char *title)
{
    const spec_t panel = {place->x, place->y, place->w, place->h,
                          COL_CARD, COL_DIM,  12,       1};
    const int16_t title_x = px(place->x + 12);
    const int16_t title_y = px(place->y + 6);
    const spec_t label = {title_x, title_y, -1, -1, COL_INFO, 0, 0, 20};
    bento_ui_id_t panel_id = 0U;
    bento_ui_id_t label_id = 0U;
    bento_status_t status =
        add_widget(ui, bento_ui_panel_create, &panel, NULL, &panel_id);

    if (status == BENTO_OK) {
        status = add_widget(ui, bento_ui_label_create, &label, title,
                            &label_id);
    }
    return status;
}

/* A line chart of at most CHART_WIDTH_MAX px, one point per pixel. */
static bento_status_t add_line_chart(bento_ui_t *ui, const spec_t *spec,
                                     bento_ui_id_t *out)
{
    spec_t place = *spec;
    bento_status_t status;

    if (spec->w > CHART_WIDTH_MAX) {
        place.w = px(CHART_WIDTH_MAX);
    }
    status = add_widget(ui, bento_ui_chart_create, &place, NULL, out);
    if (status == BENTO_OK) {
        (void)bento_ui_set_prop(ui, *out,
                                (uint8_t)BENTO_UI_PROP_CHART_POINTS,
                                CHART_POINTS);
    }
    return status;
}

/* Left: the soil ring. */
static bento_status_t add_soil(farm_t *farm)
{
    const rect_t card = {12, 44, 240, 200};
    const spec_t arc = {24, 80, 120, 120, 0U, 0, 0, 0};
    const spec_t soil = {160, 120, -1, -1, COL_TEXT, 0, 0, 28};
    bento_status_t status =
        add_card(farm->ui, &card, "ดิน % (field/soil)");

    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_arc_create, &arc, NULL,
                            &farm->screen.arc);
    }
    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_label_create, &soil,
                            "--", &farm->screen.soil);
    }
    return status;
}

/* The auto switch under the PLC card. */
static bento_status_t add_auto(farm_t *farm)
{
    const spec_t toggle = {276, 190, 64, 32, 0U, 0, 0, 1};
    const spec_t rule = {352, 194, -1, -1, COL_TEXT, 0, 0, 20};
    char text[TEXT_BYTES];
    bento_ui_id_t label = 0U;
    bento_status_t status =
        add_widget(farm->ui, bento_ui_switch_create, &toggle, NULL,
                   &farm->screen.auto_switch);

    if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text), "ออโต้: ดิน < %d%%", SOIL_MIN);
        status = add_widget(farm->ui, bento_ui_label_create, &rule,
                            text, &label);
    }
    return status;
}

/* Right: the tank and the PLC as it reports itself. */
static bento_status_t add_plc(farm_t *farm)
{
    const rect_t card = {262, 44, 518, 200};
    const spec_t bar = {276, 80, 380, 24, 0U, 0, 0, 0};
    const spec_t tank = {670, 76, -1, -1, COL_TEXT, 0, 0, 0};
    const spec_t lamp = {276, 124, 40, 40, COL_INFO, 0, 0, 0};
    const spec_t plc = {330, 132, -1, -1, COL_DIM, 0, 0, 0};
    bento_status_t status = add_card(farm->ui, &card, "ถังน้ำ + PLC");

    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_bar_create, &bar, NULL,
                            &farm->screen.tank_bar);
    }
    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_label_create, &tank,
                            "--", &farm->screen.tank);
    }
    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_led_create, &lamp, NULL,
                            &farm->screen.lamp);
    }
    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_label_create, &plc,
                            "PLC: ?", &farm->screen.plc);
    }
    if (status == BENTO_OK) {
        status = add_auto(farm);
    }
    return status;
}

/* Bottom: the soil and the pump over time, and the buttons. */
static bento_status_t add_history(farm_t *farm)
{
    static const char auto_button_name[] = "SW6"; /* BTN_NAMES[1] */
    const spec_t chart = {12, 252, 400, 86, COL_OK, 0, PERCENT, 0};
    const spec_t hint = {424, 290, -1, -1, COL_DIM, 0, 0, 20};
    char text[TEXT_BYTES];
    bento_ui_id_t label = 0U;
    bento_status_t status =
        add_line_chart(farm->ui, &chart, &farm->screen.chart);

    if (status == BENTO_OK) {
        status = bento_ui_chart_add_series(farm->ui, farm->screen.chart,
                                           COL_WARN,
                                           &farm->screen.pump_series);
    }
    if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text), "%s=รดน้ำ %s=ออโต้",
                       WATER_BUTTON_NAME, auto_button_name);
        status = add_widget(farm->ui, bento_ui_label_create, &hint,
                            text, &label);
    }
    return status;
}

static bento_status_t add_status_line(farm_t *farm)
{
    const spec_t note = {12, 352, -1, -1, COL_DIM, 0, 0, 0};
    const spec_t lamp = {590, 12, 18, 18, COL_OK, 0, 0, 0};
    const spec_t link = {616, 10, -1, -1, COL_DIM, 0, 0, 20};
    bento_status_t status =
        add_widget(farm->ui, bento_ui_label_create, &note, "กำลังเริ่ม",
                   &farm->screen.note);

    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_led_create, &lamp, NULL,
                            &farm->screen.link_lamp);
    }
    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_label_create, &link,
                            "MQTT: ออฟไลน์", &farm->screen.link_text);
    }
    return status;
}

static bento_status_t build_screen(farm_t *farm)
{
    const spec_t title = {12, 6, -1, -1, COL_TEXT, 0, 0, 24};
    bento_ui_id_t label = 0U;
    bento_status_t status = bento_ui_screen_default(farm->ui);

    if (status == BENTO_OK) {
        status = sleep_checked(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_label_create, &title,
                            "Smart IoT Gateway", &label);
    }
    if (status == BENTO_OK) {
        status = add_soil(farm);
    }
    if (status == BENTO_OK) {
        status = add_plc(farm);
    }
    if (status == BENTO_OK) {
        status = add_history(farm);
    }
    if (status == BENTO_OK) {
        status = add_status_line(farm);
    }
    if (status == BENTO_OK) {
        drain_events(farm->ui);
    }
    return status;
}

static uint32_t soil_color(maybe_t soil)
{
    uint32_t color = COL_DIM;

    if (soil.known) {
        color = (soil.value < SOIL_MIN) ? COL_BAD : COL_OK;
    }
    return color;
}

/* What the PLC reported, lost, or "?" before it ever spoke. */
static void show_plc(const farm_t *farm)
{
    const bool running = farm->pump.known && (farm->pump.value != 0);
    char text[TEXT_BYTES];

    (void)bento_ui_set_value(
        farm->ui, farm->screen.lamp,
        (farm->pump.known && (farm->pump.value == 1)) ? 1 : 0);
    if (farm->lost) {
        (void)bento_ui_set_color(farm->ui, farm->screen.plc, COL_BAD);
        copy_text(text, sizeof(text), "PLC หลุด!");
    } else if (running) {
        (void)bento_ui_set_color(farm->ui, farm->screen.plc, COL_OK);
        (void)snprintf(text, sizeof(text), "PLC: เดิน อีก %" PRId32 " วิ",
                       farm->left);
    } else {
        (void)bento_ui_set_color(farm->ui, farm->screen.plc, COL_DIM);
        copy_text(text, sizeof(text), "PLC: หยุด");
    }
    set_text(farm, farm->screen.plc, text);
}

static void show_farm(const farm_t *farm, maybe_t soil, maybe_t tank)
{
    char text[TEXT_BYTES];

    if (soil.known) {
        (void)snprintf(text, sizeof(text), "%" PRId32, soil.value);
    } else {
        copy_text(text, sizeof(text), "--");
    }
    set_text(farm, farm->screen.soil, text);
    (void)bento_ui_set_value(farm->ui, farm->screen.arc,
                             soil.known ? soil.value : 0);
    (void)bento_ui_set_color(farm->ui, farm->screen.arc,
                             soil_color(soil));
    if (tank.known) {
        (void)snprintf(text, sizeof(text), "%" PRId32 " %%",
                       tank.value);
    } else {
        copy_text(text, sizeof(text), "--");
    }
    set_text(farm, farm->screen.tank, text);
    (void)bento_ui_set_value(farm->ui, farm->screen.tank_bar,
                             tank.known ? tank.value : 0);
    show_plc(farm);
}

/* ---- 4) เครือข่าย ---- */
/* wifi.connect(): *joined is its True or False. A refused or
 * timed-out join is False; any other failure ends the program. */
static bento_status_t join_wifi(bool *joined)
{
    /* ตั้งเอง: อังกฤษ/ตัวเลขสั้น ๆ ไม่มีเว้นวรรค */
    static const char wifi_ssid[] = "<ชื่อ Hotspot ของกลุ่ม>";
    /* อย่างน้อย 8 ตัว · อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร */
    static const char wifi_pass[] = "<รหัส Hotspot ของกลุ่ม>";
    bento_wifi_connect_params_t params;
    bento_status_t status;

    bento_wifi_connect_params_init(&params);
    params.ssid = wifi_ssid;
    params.ssid_len = (uint32_t)strlen(wifi_ssid);
    params.pass = wifi_pass;
    params.pass_len = (uint32_t)strlen(wifi_pass);
    params.flags = BENTO_WIFI_F_SAVE_CREDS; /* the binding saves too */
    status = bento_wifi_connect_start(&params);
    if (status == BENTO_OK) {
        status = BENTO_E_IN_PROGRESS;
    }
    while (status == BENTO_E_IN_PROGRESS) {
        status = sleep_checked(CONNECT_POLL_MS);
        if (status == BENTO_OK) {
            status = bento_wifi_connect_poll();
        } else { /* Stop between two polls: leave the radio alone */
            (void)bento_wifi_connect_cancel(); /* nothing to report */
        }
    }
    *joined = (status == BENTO_OK);
    if ((status == BENTO_E_FAILED) || (status == BENTO_E_TIMEOUT)) {
        status = BENTO_OK;
    }
    return status;
}

/* wifi.ip() is not "0.0.0.0"; an address that cannot be read is. */
static bool has_address(void)
{
    uint32_t ip_v4 = 0U;

    return (bento_wifi_ip(BENTO_WIFI_IF_STA, &ip_v4) == BENTO_OK) &&
           (ip_v4 != 0U);
}

static bento_status_t wait_for_broker(uint32_t broker)
{
    bento_status_t status = BENTO_E_IN_PROGRESS;

    while (status == BENTO_E_IN_PROGRESS) {
        status = sleep_checked(CONNECT_POLL_MS);
        if (status == BENTO_OK) {
            status = bento_mqtt_open_poll(broker);
        }
    }
    return status;
}

/* One mqtt.connect() with a new client id; a client that cannot
 * start or a broker that does not answer is the next try. */
static bento_status_t open_broker(farm_t *farm, bool *linked)
{
    static const char broker_name[] = "broker.hivemq.com";
    /* + a number from the board's clock on every connect: never an
     * old id */
    static const char client_prefix[] = "bento-gw-";
    char client_id[CLIENT_ID_BYTES];
    bento_mqtt_config_t config;
    bento_status_t status;

    (void)snprintf(
        client_id, sizeof(client_id), "%s%s-%04" PRIx32, client_prefix,
        TEAM, bento_time_ticks_ms() & (uint32_t)CLIENT_SUFFIX_MASK);
    bento_mqtt_config_init(&config);
    config.host = broker_name;
    config.host_len = (uint32_t)strlen(broker_name);
    config.port = (uint16_t)BROKER_PORT;
    config.keepalive_s = (uint16_t)KEEPALIVE_S;
    config.client_id = client_id;
    config.client_id_len = (uint32_t)strlen(client_id);
    *linked = false;
    status = bento_mqtt_open_start(&config, &farm->broker);
    farm->has_broker = status == BENTO_OK;
    if (farm->has_broker) {
        status = wait_for_broker(farm->broker);
        *linked = status == BENTO_OK;
    }
    if (farm->has_broker && !*linked && (status != BENTO_E_CANCELLED)) {
        (void)bento_mqtt_close(farm->broker); /* the next try opens */
        farm->has_broker = false;
    }
    if (status != BENTO_E_CANCELLED) {
        status = BENTO_OK;
    }
    return status;
}

/* Three tries, a new client id every time. */
static bento_status_t connect_broker(farm_t *farm, bool *linked)
{
    char text[TEXT_BYTES];
    bento_status_t status = BENTO_OK;
    int32_t attempt;

    *linked = false;
    for (attempt = 1;
         (status == BENTO_OK) && !*linked && (attempt <= BROKER_TRIES);
         attempt++) {
        if (attempt > 1) {
            (void)snprintf(text, sizeof(text),
                           "ลองต่อ broker ใหม่ %" PRId32 "/%d", attempt,
                           BROKER_TRIES);
            show_note(farm, text, COL_WARN);
            drain_events(farm->ui);
        }
        status = open_broker(farm, linked);
    }
    return status;
}

/* WiFi, then its address, then the broker and the three topics.
 * *problem says which step failed, NULL when none did. */
static bento_status_t connect_gateway(farm_t *farm,
                                      const char **problem)
{
    static const char *const topics[TOPIC_COUNT] = {"field/+",
                                                    "plc/state", "cmd"};
    char topic[NAME_BYTES];
    uint8_t granted_qos = 0U;
    bool joined = false;
    bool ok = false;
    bento_status_t status;
    uint32_t i;

    *problem = NULL;
    show_note(farm, "ต่อ WiFi...", COL_WARN);
    drain_events(farm->ui); /* the note shows before the long wait */
    status = join_wifi(&joined);
    if ((status == BENTO_OK) && (!joined || !has_address())) {
        *problem = "ต่อ WiFi ไม่ได้";
    }
    if ((status == BENTO_OK) && (*problem == NULL)) {
        status = connect_broker(farm, &ok);
    }
    for (i = 0U;
         (status == BENTO_OK) && ok && (i < (uint32_t)TOPIC_COUNT);
         i++) {
        (void)snprintf(topic, sizeof(topic), "%s%s", farm->base,
                       topics[i]);
        ok = bento_mqtt_subscribe(farm->broker, topic,
                                  (uint32_t)strlen(topic), 0U,
                                  &granted_qos) == BENTO_OK;
    }
    if ((status == BENTO_OK) && (*problem == NULL) && !ok) {
        *problem = "broker ไม่ตอบ: รอ 1 นาทีแล้วรันใหม่";
    }
    return status;
}

/* send(): false only when the link is gone (the original's
 * OSError); a publish the broker refuses is a False nobody reads. */
static bool send_text(const farm_t *farm, const char *sub_topic,
                      const char *text)
{
    char topic[NAME_BYTES];
    bento_status_t status;

    (void)snprintf(topic, sizeof(topic), "%s%s", farm->base, sub_topic);
    status = bento_mqtt_publish(
        farm->broker, topic, (uint32_t)strlen(topic),
        (const uint8_t *)text, (uint32_t)strlen(text), 0U, 0U);
    return status != BENTO_E_SESSION;
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

/* get_message() is one slot a newer message overwrites: take every
 * message waiting and keep the newest, with its topic. */
static bool take_newest(uint32_t broker, char *topic, uint8_t *payload,
                        uint32_t *len)
{
    bento_mqtt_msg_t info;
    bool found = false;
    bool more = true;

    while (more) {
        bento_mqtt_msg_init(&info);
        more =
            bento_mqtt_poll(broker, &info, topic, BENTO_MQTT_TOPIC_MAX,
                            payload, PAYLOAD_KEEP) == BENTO_OK;
        if (more) {
            *len = info.payload_len;
            found = true;
        }
    }
    return found;
}

/* ---- 6) โปรแกรมหลัก ---- */
static bool going_on(bento_status_t status, const farm_t *farm)
{
    return (status == BENTO_OK) && !farm->stopped;
}

/* The pump LED goes off first, always; then the program ends
 * politely. */
static bento_status_t stop_here(farm_t *farm, const char *text)
{
    bento_status_t status;

    show_link(farm, false);
    status = set_led(farm, false);
    if (status == BENTO_OK) {
        show_note(farm, text, COL_BAD);
        drain_events(farm->ui);
    }
    farm->stopped = true;
    return status;
}

/* field/<name> {"value": 0..100}: soil and tank are kept. */
static void on_field(farm_t *farm, const char *name,
                     const command_t *body, uint32_t now_ms)
{
    const bento_real_t low = (bento_real_t)0.0;
    const bento_real_t high = (bento_real_t)PERCENT;
    bento_real_t value = low;
    field_t *node = NULL;

    if (strcmp(name, "soil") == 0) {
        node = &farm->soil_node;
    } else if (strcmp(name, "tank") == 0) {
        node = &farm->tank_node;
    } else {
        /* a node nobody reads */
    }
    if ((node != NULL) &&
        number_value(body, &body->fields[FIELD_VALUE], &value) &&
        (value >= low) && (value <= high)) {
        node->heard = true;
        node->value = (int32_t)value;
        node->at_ms = now_ms;
    }
}

/* "pump" must be 0 or 1 (true and false count, as in Python). */
static bool plc_pump(const command_t *body, int32_t *pump)
{
    bento_real_t value = (bento_real_t)0.0;
    const bool number =
        number_value(body, &body->fields[FIELD_PUMP], &value);

    *pump = (value == (bento_real_t)1.0) ? 1 : 0;
    return number && ((value == (bento_real_t)0.0) ||
                      (value == (bento_real_t)1.0));
}

static int32_t plc_left(const command_t *body)
{
    const bento_real_t limit =
        (bento_real_t)PUMP_MAX_S * (bento_real_t)PERCENT;
    bento_real_t value = (bento_real_t)0.0;
    int32_t left = 0;

    if (number_value(body, &body->fields[FIELD_LEFT], &value) &&
        (value > -limit) && (value < limit)) {
        left = (int32_t)value;
    }
    return left;
}

/* str(body.get("why", ""))[:16] */
static void plc_why(const command_t *body, char *why)
{
    why[0] = '\0';
    if (body->fields[FIELD_WHY].kind != JSON_MISSING) {
        value_text(body, &body->fields[FIELD_WHY], why, WHY_BYTES);
        cut_text(why, WHY_MAX);
    }
}

/* A change of the pump, except the first report of a stopped one. */
static bento_status_t pump_changed(farm_t *farm, int32_t pump,
                                   const char *why)
{
    char quoted[QUOTED_BYTES];
    char event[REPORT_BYTES];
    const bento_status_t status =
        beep(farm->ui, (pump != 0) ? TUNE_START : TUNE_STOP);

    if (status == BENTO_OK) {
        show_note(farm, (pump != 0) ? "ปั๊มเดิน" : "ปั๊มหยุด", COL_OK);
        json_quote(why, quoted, sizeof(quoted));
        (void)snprintf(
            event, sizeof(event),
            "{\"id\": \"%s\", \"event\": \"pump\", \"pump\": "
            "%" PRId32 ", \"why\": %s}",
            TEAM, pump, quoted);
        /* the original does not read send()'s answer here */
        (void)send_text(farm, "event", event);
    }
    return status;
}

/* plc/state: what the PLC says it does. */
static bento_status_t on_plc(farm_t *farm, const command_t *body,
                             uint32_t now_ms)
{
    const maybe_t was = farm->pump;
    char why[WHY_BYTES];
    int32_t pump = 0;
    bento_status_t status = BENTO_OK;

    plc_why(body, why);
    if (plc_pump(body, &pump)) {
        farm->pump.known = true;
        farm->pump.value = pump;
        farm->left = plc_left(body);
        farm->plc_heard = true;
        farm->plc_ms = now_ms;
        farm->lost = false;
        if (strcmp(why, "blocked_tank") == 0) {
            status = beep(farm->ui, TUNE_BAD);
            show_note(farm, "PLC: ถังต่ำ", COL_BAD);
        }
        if ((status == BENTO_OK) &&
            (!was.known || (was.value != pump)) &&
            !(!was.known && (pump == 0))) {
            status = pump_changed(farm, pump, why);
        }
    }
    return status;
}

/* beep, say and ack go to the people at the farm, not the PLC. */
static bento_status_t farm_request(farm_t *farm, const command_t *body,
                                   act_t act)
{
    char say[SAY_BYTES];
    char note[TEXT_BYTES];
    bento_status_t status = BENTO_OK;

    if (act == ACT_BEEP) {
        status = beep(farm->ui, TUNE_HIT);
        copy_text(note, sizeof(note), "เจ้าของฟาร์มเรียก!");
    } else if (act == ACT_SAY) {
        ascii_only(body, say);
        if ((say[0] != '\0') && farm->has_matrix) {
            /* the matrix scrolls by itself; the loop need not */
            status = bento_rgbmatrix_scroll(say, (uint32_t)strlen(say),
                                            BENTO_RGBMATRIX_PURPLE,
                                            SCROLL_MS);
        }
        if ((status == BENTO_OK) && (say[0] != '\0')) {
            status = beep(farm->ui, TUNE_TAP);
        }
        (void)snprintf(note, sizeof(note), "ข้อความ: %s",
                       (say[0] != '\0') ? say : "ไม่มีอักษรอังกฤษ");
    } else { /* ack belongs to sf2_04 */
        copy_text(note, sizeof(note), "ไม่มีแจ้งเตือน (sf2_04)");
    }
    if (status == BENTO_OK) {
        show_note(farm, note, COL_INFO);
    }
    return status;
}

/* cmd: the people at the farm, or the PLC through the app's rules. */
static bento_status_t on_command(farm_t *farm, const command_t *body,
                                 uint32_t now_ms, order_t *order)
{
    const act_t act = act_of(body);
    char note[TEXT_BYTES];
    maybe_t auto_mode = {false, 0};
    bento_status_t status = BENTO_OK;

    if ((act == ACT_BEEP) || (act == ACT_SAY) ||
        (act == ACT_ACKNOWLEDGE)) {
        status = farm_request(farm, body, act);
    } else {
        app_request(body, field_now(&farm->tank_node, now_ms), order,
                    &auto_mode, note);
        if (auto_mode.known) {
            farm->auto_mode = auto_mode.value != 0;
            (void)bento_ui_set_value(farm->ui, farm->screen.auto_switch,
                                     auto_mode.value);
        }
        show_note(farm, note, COL_INFO);
    }
    return status;
}

/* One message: which topic it came on decides what it is. */
static bento_status_t on_message(farm_t *farm, const char *topic,
                                 const command_t *body, uint32_t now_ms,
                                 order_t *order)
{
    const uint32_t base_len = (uint32_t)strlen(farm->base);
    const char *name =
        ((uint32_t)strlen(topic) >= base_len) ? &topic[base_len] : "";
    const bool filled = body->is_object && (body->members != 0U);
    bento_status_t status = BENTO_OK;

    if ((strncmp(name, "field/", FIELD_PREFIX_LEN) == 0) && filled) {
        on_field(farm, &name[FIELD_PREFIX_LEN], body, now_ms);
    } else if ((strcmp(name, "plc/state") == 0) && filled) {
        status = on_plc(farm, body, now_ms);
    } else if (strcmp(name, "cmd") == 0) {
        status = on_command(farm, body, now_ms, order);
    } else {
        /* a topic the gateway has nothing to do with */
    }
    return status;
}

static bento_status_t take_message(farm_t *farm, uint32_t now_ms,
                                   order_t *order)
{
    char topic[BENTO_MQTT_TOPIC_MAX];
    uint8_t payload[PAYLOAD_KEEP];
    uint32_t len = 0U;
    bento_status_t status = BENTO_OK;

    order->wanted = false;
    if (take_newest(farm->broker, topic, payload, &len)) {
        command_t body;

        read_command(payload, len, &body);
        status = on_message(farm, topic, &body, now_ms, order);
    }
    return status;
}

/* A PLC silent for STALE_MS: nobody knows whether the pump runs, so
 * the people hear it at once (a tone, a red note and an event). */
static bento_status_t check_plc_alive(farm_t *farm, uint32_t now_ms)
{
    char event[REPORT_BYTES];
    bento_status_t status = BENTO_OK;

    if (farm->plc_heard && !farm->lost &&
        (bento_time_ticks_diff(now_ms, farm->plc_ms) >= STALE_MS)) {
        farm->lost = true;
        farm->pump.known = false;
        status = beep(farm->ui, TUNE_BAD);
        if (status == BENTO_OK) {
            show_note(farm, "PLC หลุด!", COL_BAD);
            (void)snprintf(event, sizeof(event),
                           "{\"id\": \"%s\", \"event\": \"plc_lost\"}",
                           TEAM);
            /* the original does not read send()'s answer here */
            (void)send_text(farm, "event", event);
        }
    }
    return status;
}

/* The switch on the screen, SW6 for the auto mode; answers whether
 * SW5 was pressed. */
static bento_status_t buttons_and_touch(farm_t *farm, bool *water)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;
    bento_status_t status = BENTO_OK;
    uint32_t i;

    /* a failed poll answers no events, as ui.poll() returns [] */
    if (bento_ui_poll(farm->ui, events, BENTO_UI_POLL_EVENTS_MAX,
                      &count, NULL) != BENTO_OK) {
        count = 0U;
    }
    for (i = 0U; i < count; i++) {
        if ((events[i].id == farm->screen.auto_switch) &&
            (events[i].type == (uint8_t)BENTO_UI_EVENT_TOGGLED)) {
            farm->auto_mode = events[i].value != 0;
        }
    }
    if (take_click(&farm->auto_button)) {
        farm->auto_mode = !farm->auto_mode;
        (void)bento_ui_set_value(farm->ui, farm->screen.auto_switch,
                                 farm->auto_mode ? 1 : 0);
        status = beep(farm->ui, TUNE_TAP);
    }
    *water = take_click(&farm->water_button);
    return status;
}

/* SW5 goes through the app's rules; else the auto rule may water. */
static void decide(farm_t *farm, maybe_t soil, maybe_t tank,
                   uint32_t now_ms, bool water, order_t *order)
{
    char note[TEXT_BYTES];
    char text[TEXT_BYTES];
    char event[REPORT_BYTES];

    if (water) {
        water_request(tank, order, note);
        (void)snprintf(text, sizeof(text), "%s: %s", WATER_BUTTON_NAME,
                       note);
        show_note(farm, text, COL_INFO);
    } else if (!order->wanted &&
               should_water(
                   farm, soil, tank,
                   bento_time_ticks_diff(now_ms, farm->command_ms))) {
        order->wanted = true;
        order->pump = 1;
        order->sec = PUMP_SEC;
        show_note(farm, "ดินแห้ง รดน้ำ", COL_WARN);
        (void)snprintf(event, sizeof(event),
                       "{\"id\": \"%s\", \"event\": \"auto_water\", "
                       "\"soil\": %" PRId32 "}",
                       TEAM, soil.value);
        /* the original does not read send()'s answer here */
        (void)send_text(farm, "event", event);
    } else {
        /* nothing to order this round */
    }
}

static bento_status_t send_order(farm_t *farm, const order_t *order,
                                 uint32_t now_ms)
{
    char text[REPORT_BYTES];
    bento_status_t status = BENTO_OK;

    if (order->wanted) {
        farm->command_ms = now_ms;
        if (order->pump != 0) {
            (void)snprintf(text, sizeof(text),
                           "{\"pump\": 1, \"sec\": %" PRId32 "}",
                           order->sec);
        } else {
            copy_text(text, sizeof(text), "{\"pump\": 0}");
        }
        if (!send_text(farm, "plc/cmd", text)) {
            status = stop_here(farm, "สายหลุด");
        }
    }
    return status;
}

static void maybe_json(maybe_t value, char *out, uint32_t cap)
{
    if (value.known) {
        (void)snprintf(out, cap, "%" PRId32, value.value);
    } else {
        copy_text(out, cap, "null");
    }
}

static bento_status_t report_if_due(farm_t *farm, uint32_t now_ms,
                                    maybe_t soil, maybe_t tank)
{
    char soil_text[WORD_BYTES * 2U];
    char tank_text[WORD_BYTES * 2U];
    char pump_text[WORD_BYTES * 2U];
    char report[REPORT_BYTES];
    bento_status_t status = BENTO_OK;

    if (bento_time_ticks_diff(now_ms, farm->send_ms) >= SEND_MS) {
        farm->send_ms = now_ms;
        farm->sent++;
        maybe_json(soil, soil_text, sizeof(soil_text));
        maybe_json(tank, tank_text, sizeof(tank_text));
        maybe_json(farm->pump, pump_text, sizeof(pump_text));
        (void)snprintf(report, sizeof(report),
                       "{\"id\": \"%s\", \"n\": %" PRId32
                       ", \"soil\": %s, \"tank\": %s, \"pump\": %s"
                       ", \"auto\": %d, \"by\": \"gateway\"}",
                       TEAM, farm->sent, soil_text, tank_text,
                       pump_text, farm->auto_mode ? 1 : 0);
        if (!send_text(farm, "telemetry", report)) {
            status = stop_here(farm, "สายหลุด");
        } else {
            const bool running =
                farm->pump.known && (farm->pump.value == 1);

            /* fire and forget in modui.c */
            (void)bento_ui_chart_set_next(farm->ui, farm->screen.chart,
                                          0U,
                                          soil.known ? soil.value : 0);
            (void)bento_ui_chart_set_next(
                farm->ui, farm->screen.chart, farm->screen.pump_series,
                running ? PUMP_LINE_ON : PUMP_LINE_OFF);
        }
    }
    return status;
}

static bento_status_t draw_if_due(farm_t *farm, uint32_t now_ms,
                                  maybe_t soil, maybe_t tank)
{
    bento_status_t status = BENTO_OK;

    if (bento_time_ticks_diff(now_ms, farm->draw_ms) >= DRAW_MS) {
        farm->draw_ms = now_ms;
        status =
            set_led(farm, farm->pump.known && (farm->pump.value == 1));
        show_farm(farm, soil, tank);
    }
    return status;
}

/* One round of about POLL_MS: the inbox holds one message and a
 * newer one overwrites it, so never a long sleep. */
static bento_status_t listen_once(farm_t *farm)
{
    const uint32_t now_ms = bento_time_ticks_ms();
    const maybe_t soil = field_now(&farm->soil_node, now_ms);
    const maybe_t tank = field_now(&farm->tank_node, now_ms);
    order_t order = {false, 0, 0};
    bool water = false;
    bento_status_t status = take_message(farm, now_ms, &order);

    if (status == BENTO_OK) {
        status = buttons_and_touch(farm, &water);
    }
    if (status == BENTO_OK) {
        decide(farm, soil, tank, now_ms, water, &order);
        status = send_order(farm, &order, now_ms);
    }
    if (going_on(status, farm)) {
        status = check_plc_alive(farm, now_ms);
    }
    if (going_on(status, farm)) {
        status = draw_if_due(farm, now_ms, soil, tank);
    }
    if (going_on(status, farm)) {
        status = report_if_due(farm, now_ms, soil, tank);
    }
    if (going_on(status, farm) && !link_is_up(farm->broker)) {
        status = stop_here(farm, "สายหลุด");
    }
    if (going_on(status, farm)) {
        status =
            wait_ms(&farm->water_button, &farm->auto_button, POLL_MS);
    }
    return status;
}

static bento_status_t listen(farm_t *farm)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    farm->send_ms = start_ms;
    farm->draw_ms = start_ms;
    /* the first auto order needs no wait */
    farm->command_ms = bento_time_ticks_add(start_ms, -COOLDOWN_MS);
    while (going_on(status, farm) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = listen_once(farm);
    }
    if (going_on(status, farm)) {
        status = stop_here(farm, "จบรอบ");
    }
    return status;
}

/* Six letters ending in two digits, and not the teacher's team00. */
static bool team_is_set(const char *team)
{
    const uint32_t length = (uint32_t)strlen(team);
    bool set =
        (length == (uint32_t)TEAM_LEN) && (strcmp(team, "team00") != 0);
    uint32_t i;

    for (i = TEAM_DIGITS_AT; set && (i < length); i++) {
        set = is_digit((uint8_t)team[i]);
    }
    return set;
}

static bento_status_t run_gateway(farm_t *farm)
{
    const char *problem = NULL;
    char text[TEXT_BYTES];
    bento_status_t status;

    /* fire and forget in modui.c; E_UNSUPPORTED = no ui.volume */
    (void)bento_ui_sfx_volume_set(farm->ui, SPEAKER);
    status = build_screen(farm);
    if (status == BENTO_OK) {
        farm->has_led = find_led("RGB_BLUE", &farm->led);
        if (farm->has_matrix) {
            status = bento_rgbmatrix_clear();
        }
    }
    if ((status == BENTO_OK) && !team_is_set(TEAM)) {
        status = stop_here(farm, "แก้ TEAM ก่อน");
    }
    if (going_on(status, farm)) {
        status = connect_gateway(farm, &problem);
    }
    if (going_on(status, farm) && (problem != NULL)) {
        status = stop_here(farm, problem);
    }
    if (going_on(status, farm)) {
        show_link(farm, true);
        (void)snprintf(text, sizeof(text), "ออนไลน์ %s", TEAM);
        show_note(farm, text, COL_OK);
        status = listen(farm);
    }
    return status;
}

int main(void)
{
    static const char topic_root[] = "bento-aiot";
    farm_t farm = {0};
    bento_status_t status;

    farm.ui = bento_runtime_get_ui();
    farm.water_button.index = WATER_BUTTON;
    farm.auto_button.index = AUTO_BUTTON;
    farm.has_matrix = bento_rgbmatrix_available();
    farm.auto_mode = true;
    (void)snprintf(farm.base, sizeof(farm.base), "%s/%s/", topic_root,
                   TEAM);
    status = run_gateway(&farm);
    if (farm.has_broker) {
        (void)bento_mqtt_close(
            farm.broker); /* the finally ignores it */
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
