/*
 * idea_17_pond_aerator.c -- เติมอากาศบ่อปลา: ออกซิเจนตกตอนกลางคืน
 * สั่งเครื่องตีน้ำจากมือถือ
 *
 * ไฟล์นี้เป็นจุดเริ่มต้นของโครงงาน ต่อยอดจาก s2/sf2_03_remote_pump.py
 * ปั๊มคือเครื่องตีน้ำ (ไฟสีฟ้า) tank คือน้ำในบ่อจาก VR4 และ soil คือ DO
 * (ออกซิเจนละลายน้ำ) จำลองจาก VR1 (0-100 = 0.0-10.0 mg/L) จอซ้ายเป็นเลข DO
 * กับบันไดไฟ 3 ขั้น (สบาย เฝ้าระวัง ปลาลอยหัว) นับว่า DO ตกต่ำกว่า 3.0 กี่ครั้ง
 * และต่ำมานานเท่าไร จอขวาเป็นเครื่องตีน้ำกับวงแหวนนับถอยหลัง สั่งจาก
 * project/web/idea_17.html?team=<เลขกลุ่ม> ผ่าน MQTT (ของเดิม sf2_03 ครบ
 * และเพิ่มคีย์ dips กับ do_min) เครื่องหมุนได้นานสุด 30 วินาทีต่อคำสั่ง
 * น้ำในบ่อต่ำกว่า 10% ไม่เปิด กด SW5 หยุดเครื่องทันทีโดยไม่พึ่งเน็ต
 * บอร์ดส่งรายงานทุก 5 วินาที และเฝ้านาน 30 นาที
 * ก่อนรันแก้ WIFI_SSID, WIFI_PASS และ TEAM (เลขกลุ่ม) ในบล็อกตั้งค่าที่หัวไฟล์
 * เกณฑ์เป็นค่าสาธิตในห้องเรียน
 *
 * The fish pond aerator. Oxygen drops at night; a phone switches the
 * paddle aerator on through MQTT from
 * project/web/idea_17.html?team=<team> (every topic, key and command
 * of sf2_03, plus the keys dips and do_min). VR1 stands in for the
 * dissolved oxygen (0-100 = 0.0-10.0 mg/L), shown as a number and a
 * three-step lamp ladder; the board counts the dips below 3.0 and
 * times how long the latest one lasts. VR4 is the water in the pond
 * (below 10 % the aerator stays off). One command runs it for at most
 * 30 s; SW5 stops it at once without the network. A report goes out
 * every 5 s and the board watches for 30 minutes. Edit WIFI_SSID,
 * WIFI_PASS and TEAM in the settings block at the top before
 * running.
 *
 * Twin of idea_17_pond_aerator.py (aiot-development-for-smart-farm/project/idea_17_pond_aerator.py). Style: CLEAN_C.md.
 * Where C differs:
 *   - json.loads: a small reader of one JSON object finds "cmd",
 *     "n", "on", "sec" and "text" at its top level. A nested list
 *     or object is skipped by its brackets, not checked inside; a
 *     key spelt with escapes is not recognised; MicroPython's reader
 *     is looser (commas and colons count as spaces there), so some
 *     broken text it takes reads "อ่านคำสั่งไม่ได้" here.
 *   - str() of a value: a text, an integer, true, false and null
 *     read as MicroPython writes them; a real number, a list or an
 *     object shows as its JSON text. [:n] keeps whole characters
 *     (the reTerminal's way; the PSoC Edge's MicroPython cuts bytes).
 *   - json.dumps: built with snprintf, the keys in the order the .py
 *     writes them (MicroPython writes its dict's slot order).
 *   - get_message() is one slot a newer message overwrites; the C
 *     client keeps a ring of BENTO_MQTT_RING_SLOTS, and each round
 *     takes them all and keeps the newest.
 *   - try/except OSError: a status. A broker client that cannot
 *     start, or a connect that fails, is the next try; a publish
 *     on a lost link is the original's False.
 *   - led_named(): a board without "RGB_BLUE" uses LED 0; an LED
 *     that cannot be switched off is the original's None.
 *   - dip_t0 = None: a bool beside the time.
 *   - ui.volume and ui.tone are fire and forget in modui.c, so their
 *     status is ignored here too; a board without volume
 *     (E_UNSUPPORTED) is the original's hasattr() False.
 *   - rgbmatrix is on the Dev Kit only (elsewhere the .py stops at
 *     its import); here the matrix calls are skipped where
 *     bento_rgbmatrix_available() is false.
 *   - raise Stop is a stopped flag that ends the status chain; the
 *     finally's mqtt.disconnect() is the close at the end of main.
 *   - wifi.* and mqtt.* print console lines of their own in
 *     MicroPython; the C library prints none.
 *   - CLIENT_ID and the topics are built in code from ROOT and TEAM
 *     with snprintf, where the .py adds the texts together; ROOT is
 *     the "bento-aiot" the .py writes inside its topics.
 *   - Stop ends the program quietly at the next sleep, where
 *     MicroPython raises KeyboardInterrupt.
 *   - In the BENTO Emulator, MQTT from C is not available yet: the
 *     program says so and ends (after the .py's own "broker ไม่ตอบ"
 *     note, as if the broker did not answer).
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
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_time.h"
#include "bento_wifi.h"

/* ---- แก้ก่อนรัน: settings (MISRA deviation CT02) ---- */
/* ตั้งเอง: อังกฤษ/ตัวเลขสั้น ๆ ไม่มีเว้นวรรค */
static const char WIFI_SSID[] = "<ชื่อ Hotspot ของกลุ่ม>";
/* อย่างน้อย 8 ตัว · อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร */
static const char WIFI_PASS[] = "<รหัส Hotspot ของกลุ่ม>";
static const char TEAM[] = "teamXX";

static const char BROKER[] = "broker.hivemq.com";
/* CLIENT_ID = "bento-farm-" + TEAM, built in open_broker() */
static const char ROOT[] = "bento-aiot"; /* the topics start with it */
/* TOPIC_CMD = ROOT/TEAM/cmd, built in make_topics() */
/* TOPIC = ROOT/TEAM/telemetry, built in make_topics() */
/* ---- end of settings ---- */

/* ---- 1) ตั้งค่า ---- */
static const char STOP_BUTTON_NAME[] = "SW5"; /* BTN_NAMES[0] */

/* The aerator, the thresholds and the times. */
enum {
    PUMP_DEFAULT_S = 10, /* a command without "sec" */
    PUMP_MAX_S = 30,     /* whoever asks 9999 s gets 30 */
    TANK_MIN = 10,       /* below this the paddles beat air: stay off */
    DO_LOW = 30, /* x10: below 3.0 the fish gasp at the surface */
    DO_OK = 50,  /* x10: from 5.0 the fish are calm */
    SEND_MS = 5000,
    POLL_MS = 100,
    LISTEN_MS = 1800000,
    SPEAKER = 40, /* speaker volume, percent */
    VOLUME = 25   /* a tone's velocity */
};

/* สีบ่อกลางคืน, 0xRRGGBB */
enum {
    COL_TEXT = 0xE2F2F0,
    COL_DIM = 0x86A3A0,
    COL_CARD = 0x0F262B,
    COL_O2 = 0x3FD6C6,
    COL_NIGHT = 0x6C7BD9,
    COL_WATCH = 0xF2B636,
    COL_GASP = 0xE5484D,
    COL_CALM = 0x47C774
};

/* The board, the network and the texts. */
enum {
    STOP_BUTTON = 1, /* SW5 is buttons index 1 */
    KNOB_OXYGEN = 0, /* VR1 */
    KNOB_TANK = 3,   /* VR4 */
    KNOB_FULL = 4095,
    PERCENT = 100,
    DO_FULL = 100, /* the lowest DO starts at the top of the scale */
    TENTHS = 10,   /* DO is kept in tenths of a mg/L */
    MS_PER_S = 1000,
    S_PER_MIN = 60,
    SAMPLE_MS = 20,  /* how often SW5 is read while waiting */
    SETTLE_MS = 200, /* let the cleared screen settle */
    TONE_MS = 90,
    TONE_GAP_MS = 100,
    TUNE_NOTES_MAX = 4,
    BROKER_PORT = 1883,
    KEEPALIVE_S = 60,
    BROKER_TRIES = 3,
    CONNECT_POLL_MS = 20,
    CLIENT_SUFFIX_MASK = 0xFFFF,
    SCROLL_MS = 80,
    SAY_MAX = 20,          /* letters the matrix gets */
    NUMBER_SHOW_MAX = 6,   /* letters of a wrong "n" */
    UNKNOWN_SHOW_MAX = 12, /* letters of an unknown command */
    TEAM_LEN = 6,          /* "team" and two digits */
    TEAM_PREFIX_LEN = 4
};

/* The three steps of the lamp ladder. */
enum {
    STEP_CALM = 0,
    STEP_WATCH = 1,
    STEP_GASP = 2,
    STEP_COUNT = 3
};

/* Buffer sizes, in bytes. */
enum {
    TEXT_BYTES = 128,     /* a label text: 126 at most */
    NAME_BYTES = 48,      /* a topic */
    CLIENT_ID_BYTES = 32, /* the broker takes 23 */
    PAYLOAD_KEEP = 255,   /* what the MicroPython binding keeps */
    VALUE_BYTES = 256, /* a decoded value: never longer than its text */
    WORD_BYTES = 8,    /* the longest command word, and its NUL */
    SAY_BYTES = 24,
    CUT_BYTES = 49, /* 12 letters of up to 4 bytes, and the NUL */
    DO_BYTES = 24,  /* "%d.%d" of any int32_t */
    /* the telemetry with every int32_t at its widest */
    REPORT_BYTES = 224
};

/* The step colours, calm to gasping. */
static const uint32_t STEP_COLS[STEP_COUNT] = {COL_CALM, COL_WATCH,
                                               COL_GASP};

/* SW5, sampled while the program waits so a short press counts. */
typedef struct {
    uint32_t index;
    bool down;
    bool clicked;
} button_t;

/* The widgets the program changes after drawing them. */
typedef struct {
    bento_ui_id_t oxygen;
    bento_ui_id_t step;
    bento_ui_id_t ladder[STEP_COUNT];
    bento_ui_id_t dip;
    bento_ui_id_t stats;
    bento_ui_id_t title;
    bento_ui_id_t pump_lamp;
    bento_ui_id_t pump_text;
    bento_ui_id_t ring;
    bento_ui_id_t tank;
    bento_ui_id_t tank_bar;
    bento_ui_id_t note;
    bento_ui_id_t link_lamp;
    bento_ui_id_t link_text;
} screen_t;

/* Everything one run of the program keeps. */
typedef struct {
    bento_ui_t *ui;
    screen_t screen;
    button_t stop_button;
    char topic_cmd[NAME_BYTES];
    char topic[NAME_BYTES];
    uint32_t broker;
    bool has_broker;
    uint32_t pump_led;
    bool has_pump_led;
    bool has_matrix;
    bool stopped; /* the original's raise Stop */
    bool no_mqtt; /* the BENTO Emulator: no MQTT from C yet */
    int32_t got;  /* commands received */
    int32_t sent; /* reports sent, the "n" key */
    int32_t pump_ms;
    uint32_t pump_start_ms;
    int32_t dips;       /* times DO fell below DO_LOW */
    int32_t do_min;     /* the lowest DO, x10 */
    int32_t step_shown; /* -1 = not drawn yet */
    bool dipping;       /* false = the original's dip_t0 None */
    uint32_t dip_start_ms;
    int32_t shown; /* the seconds the matrix shows, -1 = none */
    uint32_t send_ms;
} farm_t;

/* The two knobs of one round, in percent. */
typedef struct {
    int32_t oxygen; /* DO x10 from VR1, the "soil" key */
    int32_t tank;   /* the water in the pond, VR4 */
} knobs_t;

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

typedef bento_ui_status_t (*factory_t)(
    bento_ui_t *ui, const bento_ui_widget_args_t *args,
    bento_ui_id_t *out);

typedef enum {
    TUNE_TAP = 0,
    TUNE_START,
    TUNE_STOP,
    TUNE_GOOD,
    TUNE_BAD,
    TUNE_GASP,
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

/* The keys the program reads from the command. */
enum {
    FIELD_CMD = 0,
    FIELD_N,
    FIELD_ON,
    FIELD_SEC,
    FIELD_TEXT,
    FIELD_COUNT
};

typedef struct {
    const uint8_t *text;
    uint32_t len;
    uint32_t at;
} json_reader_t;

/* One command as far as the program asks. */
typedef struct {
    const uint8_t *payload;
    bool is_object;
    json_value_t fields[FIELD_COUNT];
} command_t;

typedef enum {
    ACT_NONE = 0, /* no "cmd", or an empty one */
    ACT_LED,
    ACT_PUMP,
    ACT_BEEP,
    ACT_SAY,
    ACT_ACKNOWLEDGE,
    ACT_OTHER
} act_t;

typedef enum {
    DO_NOTHING = 0,
    DO_DENY,
    DO_OFF,
    DO_ON,
    DO_BEEP,
    DO_SAY,
    ACTION_COUNT
} action_t;

/* What one command asks for, before anything real is touched. */
typedef struct {
    action_t action;
    int32_t sec;
    char say[SAY_BYTES];
    char note[TEXT_BYTES];
    uint32_t color;
} outcome_t;

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
        [TUNE_TAP] = {{76U, 0U, 0U, 0U}, 1U},
        [TUNE_START] = {{67U, 74U, 0U, 0U}, 2U},
        [TUNE_STOP] = {{74U, 67U, 0U, 0U}, 2U},
        [TUNE_GOOD] = {{72U, 79U, 84U, 0U}, 3U},
        [TUNE_BAD] = {{84U, 76U, 0U, 0U}, 2U},
        [TUNE_GASP] = {{88U, 84U, 88U, 84U}, 4U},
        [TUNE_HIT] = {{88U, 0U, 0U, 0U}, 1U}};
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

static bento_status_t knob_percent(uint32_t knob, int32_t *percent)
{
    uint16_t raw = 0U;
    const bento_status_t status = bento_pots_read(knob, &raw);

    *percent = ((int32_t)raw * PERCENT) / KNOB_FULL;
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

static bento_status_t set_pump_led(const farm_t *farm, bool on)
{
    bento_status_t status = BENTO_OK;

    if (farm->has_pump_led) {
        status = on ? bento_gpio_led_on(farm->pump_led)
                    : bento_gpio_led_off(farm->pump_led);
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

/* A wait that keeps reading SW5, so a press between two rounds
 * still counts. */
static bento_status_t wait_ms(button_t *button, uint32_t duration_ms)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            (int32_t)duration_ms)) {
        status = sample_button(button);
        if (status == BENTO_OK) {
            status = sleep_checked(SAMPLE_MS);
        }
    }
    return status;
}

static bento_status_t matrix_off(const farm_t *farm)
{
    bento_status_t status = BENTO_OK;

    if (farm->has_matrix) {
        status = bento_rgbmatrix_scroll("", 0U, BENTO_RGBMATRIX_WHITE,
                                        SCROLL_MS);
        if (status == BENTO_OK) {
            status = bento_rgbmatrix_clear();
        }
    }
    return status;
}

/* The matrix is drawn only when the number changes. */
static bento_status_t matrix_countdown(farm_t *farm, int32_t sec_left)
{
    bento_status_t status = BENTO_OK;

    if ((farm->shown <= 0) || (sec_left == 0)) {
        status = matrix_off(farm);
    }
    if ((status == BENTO_OK) && farm->has_matrix && (sec_left != 0)) {
        status = bento_rgbmatrix_score(sec_left, BENTO_RGBMATRIX_GREEN);
    }
    farm->shown = sec_left;
    return status;
}

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

/* The payload's bytes spell this word (the payload is bytes, the
 * word a C text, so strncmp does not apply). */
static bool bytes_match(const uint8_t *bytes, const char *word,
                        uint32_t length)
{
    bool same = true;
    uint32_t i;

    for (i = 0U; same && (i < length); i++) {
        same = bytes[i] == (uint8_t)word[i];
    }
    return same;
}

static bool skip_word(json_reader_t *reader, const char *word)
{
    const uint32_t length = (uint32_t)strlen(word);
    const bool same =
        ((reader->len - reader->at) >= length) &&
        bytes_match(&reader->text[reader->at], word, length);

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
    static const char *const names[FIELD_COUNT] = {"cmd", "n", "on",
                                                   "sec", "text"};
    const uint32_t length = key->end - key->start - 2U;
    uint32_t field = FIELD_COUNT;
    uint32_t i;

    for (i = 0U; (field == FIELD_COUNT) && (i < FIELD_COUNT); i++) {
        if ((strlen(names[i]) == length) &&
            bytes_match(&reader->text[key->start + 1U], names[i],
                        length)) {
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

/* The first byte of a UTF-8 character (not a 10xxxxxx follower). */
static bool starts_character(uint8_t byte)
{
    const uint8_t follow_mask = 0xC0U;
    const uint8_t follow = 0x80U;

    return (byte & follow_mask) != follow;
}

/* The bytes of text[:keep], counted in whole UTF-8 characters, that
 * fit in cap bytes with the NUL; a character that does not fit whole
 * is left out. */
static uint32_t kept_bytes(const char *text, uint32_t keep,
                           uint32_t cap)
{
    uint32_t count = 0U;
    uint32_t end = 0U;
    uint32_t i = 0U;

    while ((text[i] != '\0') && (i < (cap - 1U)) && (count <= keep)) {
        if (starts_character((uint8_t)text[i])) {
            end = i; /* every byte before i is a whole character */
            count++;
        }
        i++;
    }
    if ((count <= keep) &&
        ((text[i] == '\0') || starts_character((uint8_t)text[i]))) {
        end = i; /* the text or the room ended on a whole character */
    }
    return end;
}

/* text[:keep], counted in whole UTF-8 characters, copied into out
 * (cap bytes). */
static void cut_text(const char *text, uint32_t keep, char *out,
                     uint32_t cap)
{
    const uint32_t end = kept_bytes(text, keep, cap);

    (void)memcpy(out, text, end);
    out[end] = '\0';
}

static bool number_is_zero(const command_t *command,
                           const json_value_t *value)
{
    bool zero = true;
    bool exponent = false; /* the exponent does not matter */
    uint32_t i = value->start;

    while (!exponent && (i < value->end)) {
        const uint8_t byte = command->payload[i];

        if ((byte == (uint8_t)'e') || (byte == (uint8_t)'E')) {
            exponent = true;
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

/* cmd.get("n", 0) == 0: missing, a zero, or false. */
static bool pump_number_is_zero(const command_t *command)
{
    const json_value_t *value = &command->fields[FIELD_N];

    return (value->kind == JSON_MISSING) ||
           (value->kind == JSON_FALSE) ||
           ((value->kind == JSON_NUMBER) &&
            number_is_zero(command, value));
}

/* A whole number above 0, else the default; never above the cap. */
static int32_t pump_seconds(const command_t *command)
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
        sec = PUMP_DEFAULT_S;
    }
    return (sec < PUMP_MAX_S) ? sec : PUMP_MAX_S;
}

static act_t act_of_word(const char *word)
{
    static const char *const words[] = {"led", "pump", "beep", "say",
                                        "ack"};
    static const act_t acts[] = {ACT_LED, ACT_PUMP, ACT_BEEP, ACT_SAY,
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

/* cmd.get("cmd", ""): a missing or empty one asks for nothing. */
static act_t act_of(const command_t *command)
{
    const json_value_t *value = &command->fields[FIELD_CMD];
    act_t act = (value->kind == JSON_MISSING) ? ACT_NONE : ACT_OTHER;

    if (value->kind == JSON_STRING) {
        char word[WORD_BYTES];
        const uint32_t length =
            decode_string(command, value, word, sizeof(word));

        if (length == 0U) {
            act = ACT_NONE;
        } else if (length < sizeof(word)) {
            act = act_of_word(word);
        } else {
            /* longer than every command word */
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

static void set_outcome(outcome_t *outcome, action_t action,
                        const char *note, uint32_t color)
{
    outcome->action = action;
    copy_text(outcome->note, sizeof(outcome->note), note);
    outcome->color = color;
}

/* "led" and "pump" switch the aerator; nothing real is touched here. */
static void pump_command(const command_t *command, act_t act,
                         int32_t tank, outcome_t *outcome)
{
    char shown[VALUE_BYTES];
    char cut[CUT_BYTES];

    if ((act == ACT_LED) && !pump_number_is_zero(command)) {
        value_text(command, &command->fields[FIELD_N], shown,
                   sizeof(shown));
        cut_text(shown, NUMBER_SHOW_MAX, cut, sizeof(cut));
        set_outcome(outcome, DO_DENY, "", COL_WATCH);
        (void)snprintf(outcome->note, sizeof(outcome->note),
                       "ไม่มีเครื่องที่ %s", cut);
    } else if (!value_is_true(command, &command->fields[FIELD_ON])) {
        set_outcome(outcome, DO_OFF, "สั่งหยุดเครื่องตีน้ำ", COL_CALM);
    } else if (tank < TANK_MIN) {
        set_outcome(outcome, DO_DENY, "", COL_GASP);
        (void)snprintf(outcome->note, sizeof(outcome->note),
                       "น้ำในบ่อเหลือ %" PRId32 "%% ไม่เปิด", tank);
    } else {
        outcome->sec = pump_seconds(command);
        set_outcome(outcome, DO_ON, "", COL_O2);
        (void)snprintf(outcome->note, sizeof(outcome->note),
                       "เปิดเครื่องตีน้ำ %" PRId32 " วิ", outcome->sec);
    }
}

static void other_command(const command_t *command, act_t act,
                          outcome_t *outcome)
{
    char shown[VALUE_BYTES];
    char cut[CUT_BYTES];

    if (act == ACT_BEEP) {
        set_outcome(outcome, DO_BEEP, "เจ้าของบ่อเรียก!", COL_NIGHT);
    } else if (act == ACT_SAY) {
        ascii_only(command, outcome->say);
        set_outcome(outcome, DO_SAY, "", COL_NIGHT);
        (void)snprintf(
            outcome->note, sizeof(outcome->note), "ข้อความ: %s",
            (outcome->say[0] != '\0') ? outcome->say : "ไม่มีอักษรอังกฤษ");
    } else if (act == ACT_ACKNOWLEDGE) { /* ack belongs to sf2_04 */
        set_outcome(outcome, DO_NOTHING, "ไม่มีแจ้งเตือน (sf2_04)",
                    COL_DIM);
    } else if (act == ACT_OTHER) {
        value_text(command, &command->fields[FIELD_CMD], shown,
                   sizeof(shown));
        cut_text(shown, UNKNOWN_SHOW_MAX, cut, sizeof(cut));
        set_outcome(outcome, DO_DENY, "", COL_WATCH);
        (void)snprintf(outcome->note, sizeof(outcome->note), "ไม่รู้จัก %s",
                       cut);
    } else {
        set_outcome(outcome, DO_NOTHING, "", COL_DIM);
    }
}

/* One command -> what to do, its value, the note and its colour. */
static void handle_command(const command_t *command, int32_t tank,
                           outcome_t *outcome)
{
    (void)memset(outcome, 0, sizeof(*outcome));
    if (!command->is_object) { /* 5, null and [] are JSON too */
        set_outcome(outcome, DO_DENY, "อ่านคำสั่งไม่ได้", COL_GASP);
    } else {
        const act_t act = act_of(command);

        if ((act == ACT_LED) || (act == ACT_PUMP)) {
            pump_command(command, act, tank, outcome);
        } else {
            other_command(command, act, outcome);
        }
    }
}

/* DO x10 as "5.0"; the knob never gives a negative one. */
static void do_text(int32_t do_tenths, char *out, uint32_t cap)
{
    (void)snprintf(out, cap, "%" PRId32 ".%" PRId32, do_tenths / TENTHS,
                   do_tenths % TENTHS);
}

/* 0 calm, 1 watch, 2 the fish gasp. */
static int32_t do_step(int32_t do_tenths)
{
    int32_t step = STEP_GASP;

    if (do_tenths >= DO_OK) {
        step = STEP_CALM;
    } else if (do_tenths >= DO_LOW) {
        step = STEP_WATCH;
    } else {
        /* below DO_LOW */
    }
    return step;
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

/* The lamp ladder: one lamp per step, top to bottom. */
static bento_status_t add_ladder(farm_t *farm)
{
    enum {
        LADDER_X = 330,
        LADDER_Y = 70,
        LADDER_GAP = 52,
        LAMP_SIZE = 36
    };
    bento_status_t status = BENTO_OK;
    int32_t i;

    for (i = 0; (status == BENTO_OK) && (i < STEP_COUNT); i++) {
        const spec_t lamp = {LADDER_X,
                             px(LADDER_Y + (i * LADDER_GAP)),
                             LAMP_SIZE,
                             LAMP_SIZE,
                             STEP_COLS[i],
                             0,
                             0,
                             0};

        status = add_widget(farm->ui, bento_ui_led_create, &lamp, NULL,
                            &farm->screen.ladder[i]);
    }
    return status;
}

/* The lower half of the oxygen card: the dip lines and the rule. */
static bento_status_t add_dip_lines(farm_t *farm)
{
    const spec_t dip = {26, 180, -1, -1, COL_DIM, 0, 0, 20};
    const spec_t stats = {26, 212, -1, -1, COL_O2, 0, 0, 20};
    const spec_t rule = {26, 258, -1, -1, COL_DIM, 0, 0, 20};
    char low[DO_BYTES];
    char text[TEXT_BYTES];
    bento_ui_id_t label = 0U;
    bento_status_t status =
        add_widget(farm->ui, bento_ui_label_create, &dip, "ยังไม่เคยต่ำ",
                   &farm->screen.dip);

    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_label_create, &stats,
                            "ตกต่ำ 0 ครั้ง  ต่ำสุด --", &farm->screen.stats);
    }
    if (status == BENTO_OK) {
        do_text(DO_LOW, low, sizeof(low));
        (void)snprintf(text, sizeof(text), "ต่ำกว่า %s = ปลาลอยหัว", low);
        status = add_widget(farm->ui, bento_ui_label_create, &rule,
                            text, &label);
    }
    return status;
}

/* Left: the dissolved oxygen card. */
static bento_status_t add_oxygen(farm_t *farm)
{
    const spec_t panel = {12, 44, 400, 250, COL_CARD, COL_NIGHT, 16, 1};
    const spec_t title = {26, 52, -1, -1, COL_NIGHT, 0, 0, 20};
    const spec_t oxygen = {26, 88, -1, -1, COL_TEXT, 0, 0, 28};
    const spec_t unit = {190, 96, -1, -1, COL_DIM, 0, 0, 16};
    const spec_t step = {26, 132, -1, -1, COL_DIM, 0, 0, 24};
    bento_ui_id_t label = 0U;
    bento_status_t status = add_widget(farm->ui, bento_ui_panel_create,
                                       &panel, NULL, &label);

    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_label_create, &title,
                            "ออกซิเจนละลายน้ำ (VR1)", &label);
    }
    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_label_create, &oxygen,
                            "DO --", &farm->screen.oxygen);
    }
    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_label_create, &unit,
                            "mg/L", &label);
    }
    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_label_create, &step,
                            "--", &farm->screen.step);
    }
    if (status == BENTO_OK) {
        status = add_ladder(farm);
    }
    if (status == BENTO_OK) {
        status = add_dip_lines(farm);
    }
    return status;
}

/* The lower half of the aerator card: the pond water and SW5. */
static bento_status_t add_pond_water(farm_t *farm)
{
    const spec_t tank = {438, 150, -1, -1, COL_TEXT, 0, 0, 20};
    const spec_t bar = {440, 184, 160, 14, 0U, 0, 0, 0};
    const spec_t hint = {438, 262, -1, -1, COL_WATCH, 0, 0, 20};
    char text[TEXT_BYTES];
    bento_ui_id_t label = 0U;
    bento_status_t status =
        add_widget(farm->ui, bento_ui_label_create, &tank,
                   "น้ำในบ่อ (VR4) -- %", &farm->screen.tank);

    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_bar_create, &bar, NULL,
                            &farm->screen.tank_bar);
    }
    if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text), "%s = หยุดเครื่องทันที",
                       STOP_BUTTON_NAME);
        status = add_widget(farm->ui, bento_ui_label_create, &hint,
                            text, &label);
    }
    return status;
}

/* Right: the aerator, its lamp and its countdown ring. */
static bento_status_t add_aerator(farm_t *farm)
{
    const spec_t panel = {424, 44, 356, 250, COL_CARD, COL_O2, 16, 1};
    const spec_t title = {438, 52, -1, -1, COL_O2, 0, 0, 20};
    const spec_t lamp = {440, 86, 40, 40, COL_O2, 0, 0, 0};
    const spec_t pump = {494, 92, -1, -1, COL_DIM, 0, 0, 24};
    const spec_t ring = {620, 130, 120, 120, 0U, 0, PUMP_MAX_S, 0};
    bento_ui_id_t label = 0U;
    bento_status_t status = add_widget(farm->ui, bento_ui_panel_create,
                                       &panel, NULL, &label);

    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_label_create, &title,
                            "เครื่องตีน้ำ (คำสั่ง 0)", &farm->screen.title);
    }
    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_led_create, &lamp, NULL,
                            &farm->screen.pump_lamp);
    }
    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_label_create, &pump,
                            "หยุด", &farm->screen.pump_text);
    }
    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_arc_create, &ring, NULL,
                            &farm->screen.ring);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_color(farm->ui, farm->screen.ring, COL_O2);
        status = add_pond_water(farm);
    }
    return status;
}

static bento_status_t add_status_line(farm_t *farm)
{
    const spec_t note = {12, 352, -1, -1, COL_DIM, 0, 0, 20};
    const spec_t lamp = {596, 12, 18, 18, COL_CALM, 0, 0, 0};
    const spec_t link = {622, 10, -1, -1, COL_DIM, 0, 0, 20};
    bento_status_t status =
        add_widget(farm->ui, bento_ui_label_create, &note, "ยังไม่มีคำสั่ง",
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
    const spec_t title = {12, 6, -1, -1, COL_O2, 0, 0, 24};
    bento_ui_id_t label = 0U;
    bento_status_t status = bento_ui_screen_default(farm->ui);

    if (status == BENTO_OK) {
        status = sleep_checked(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_label_create, &title,
                            "บ่อปลา: เฝ้าออกซิเจนทั้งคืน", &label);
    }
    if (status == BENTO_OK) {
        status = add_oxygen(farm);
    }
    if (status == BENTO_OK) {
        status = add_aerator(farm);
    }
    if (status == BENTO_OK) {
        status = add_status_line(farm);
    }
    if (status == BENTO_OK) {
        drain_events(farm->ui);
    }
    return status;
}

static void show_pump(const farm_t *farm, int32_t sec_left)
{
    char text[TEXT_BYTES];

    (void)bento_ui_set_value(farm->ui, farm->screen.pump_lamp,
                             (sec_left != 0) ? 1 : 0);
    (void)bento_ui_set_color(farm->ui, farm->screen.pump_text,
                             (sec_left != 0) ? COL_O2 : COL_DIM);
    if (sec_left != 0) {
        (void)snprintf(text, sizeof(text), "ตีน้ำ อีก %" PRId32 " วิ",
                       sec_left);
    } else {
        copy_text(text, sizeof(text), "หยุด");
    }
    set_text(farm, farm->screen.pump_text, text);
    (void)bento_ui_set_value(farm->ui, farm->screen.ring, sec_left);
}

static void show_step(const farm_t *farm, int32_t step)
{
    static const char *const step_words[STEP_COUNT] = {
        "ปลาสบาย", "เฝ้าระวัง", "ปลาลอยหัว!"};
    int32_t i;

    for (i = 0; i < STEP_COUNT; i++) {
        (void)bento_ui_set_value(farm->ui, farm->screen.ladder[i],
                                 (i == step) ? 1 : 0);
    }
    (void)bento_ui_set_color(farm->ui, farm->screen.step,
                             STEP_COLS[step]);
    set_text(farm, farm->screen.step, step_words[step]);
}

static void show_pond(const farm_t *farm, const knobs_t *knobs)
{
    char value[DO_BYTES];
    char text[TEXT_BYTES];

    (void)bento_ui_set_color(farm->ui, farm->screen.oxygen,
                             STEP_COLS[do_step(knobs->oxygen)]);
    do_text(knobs->oxygen, value, sizeof(value));
    (void)snprintf(text, sizeof(text), "DO %s", value);
    set_text(farm, farm->screen.oxygen, text);
    (void)bento_ui_set_value(farm->ui, farm->screen.tank_bar,
                             knobs->tank);
    (void)bento_ui_set_color(farm->ui, farm->screen.tank_bar,
                             (knobs->tank >= TANK_MIN) ? COL_O2
                                                       : COL_GASP);
    (void)snprintf(text, sizeof(text), "น้ำในบ่อ (VR4) %" PRId32 " %%",
                   knobs->tank);
    set_text(farm, farm->screen.tank, text);
}

/* How long the dip has lasted, or that DO is not low now; and the
 * dip count with the lowest DO. */
static void show_dip(const farm_t *farm, uint32_t now_ms)
{
    char lowest[DO_BYTES];
    char text[TEXT_BYTES];

    if (farm->dipping) {
        const int32_t dip_s =
            bento_time_ticks_diff(now_ms, farm->dip_start_ms) /
            MS_PER_S;

        (void)bento_ui_set_color(farm->ui, farm->screen.dip, COL_GASP);
        (void)snprintf(text, sizeof(text),
                       "ต่ำมาแล้ว %" PRId32 ":%02" PRId32 " นาที",
                       dip_s / S_PER_MIN, dip_s % S_PER_MIN);
        set_text(farm, farm->screen.dip, text);
    } else {
        (void)bento_ui_set_color(farm->ui, farm->screen.dip, COL_DIM);
        set_text(farm, farm->screen.dip, "ตอนนี้ไม่ต่ำ");
    }
    do_text(farm->do_min, lowest, sizeof(lowest));
    (void)snprintf(text, sizeof(text), "ตกต่ำ %" PRId32 " ครั้ง ต่ำสุด %s",
                   farm->dips, lowest);
    set_text(farm, farm->screen.stats, text);
}

/* ---- 4) เครือข่าย ---- */
/* wifi.connect(): *joined is its True or False. A refused or
 * timed-out join is False; any other failure ends the program. */
static bento_status_t join_wifi(bool *joined)
{
    bento_wifi_connect_params_t params;
    bento_status_t status;

    bento_wifi_connect_params_init(&params);
    params.ssid = WIFI_SSID;
    params.ssid_len = (uint32_t)strlen(WIFI_SSID);
    params.pass = WIFI_PASS;
    params.pass_len = (uint32_t)strlen(WIFI_PASS);
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
    char client_id[CLIENT_ID_BYTES];
    bento_mqtt_config_t config;
    bento_status_t status;

    /* CLIENT_ID + a number from the board's clock: never an old id */
    (void)snprintf(
        client_id, sizeof(client_id), "bento-farm-%s-%04" PRIx32, TEAM,
        bento_time_ticks_ms() & (uint32_t)CLIENT_SUFFIX_MASK);
    bento_mqtt_config_init(&config);
    config.host = BROKER;
    config.host_len = (uint32_t)strlen(BROKER);
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
    if (status == BENTO_E_UNSUPPORTED) {
        farm->no_mqtt = true; /* the Emulator: no try will answer */
    }
    if (status != BENTO_E_CANCELLED) {
        status = BENTO_OK;
    }
    return status;
}

/* The BENTO Emulator answers every MQTT call with E_UNSUPPORTED: one
 * label says so in the empty strip between the cards and the note,
 * then the program goes on as if the broker did not answer. */
static bento_status_t show_emulator_gap(const farm_t *farm)
{
    static const char gap_text[] =
        "MQTT จาก C ยังไม่มีใน BENTO Emulator ให้รันบน Dev Kit หรือ Eva Kit";
    /* one line from x 12 (593 px), y 306-337 */
    const spec_t gap = {12, 306, -1, -1, COL_WATCH, 0, 0, 20};
    bento_ui_id_t label = 0U;

    (void)printf(
        "%s / MQTT from C is not in the BENTO Emulator yet: run "
        "it on a Dev Kit or Eva Kit.\n",
        gap_text);
    return add_widget(farm->ui, bento_ui_label_create, &gap, gap_text,
                      &label);
}

/* Three tries, a new client id every time; none more once the
 * Emulator has said it has no MQTT. */
static bento_status_t connect_broker(farm_t *farm, bool *linked)
{
    char text[TEXT_BYTES];
    bento_status_t status = BENTO_OK;
    int32_t attempt;

    *linked = false;
    for (attempt = 1; (status == BENTO_OK) && !*linked &&
                      !farm->no_mqtt && (attempt <= BROKER_TRIES);
         attempt++) {
        if (attempt > 1) {
            (void)snprintf(text, sizeof(text),
                           "ลองต่อ broker ใหม่ %" PRId32 "/%d", attempt,
                           BROKER_TRIES);
            show_note(farm, text, COL_WATCH);
            drain_events(farm->ui);
        }
        status = open_broker(farm, linked);
    }
    if ((status == BENTO_OK) && farm->no_mqtt) {
        status = show_emulator_gap(farm);
    }
    return status;
}

/* WiFi, then its address, then the broker and the subscription.
 * *problem says which step failed, NULL when none did. */
static bento_status_t connect_farm(farm_t *farm, const char **problem)
{
    uint8_t granted_qos = 0U;
    bool joined = false;
    bool linked = false;
    bento_status_t status;

    *problem = NULL;
    show_note(farm, "ต่อ WiFi... จอนิ่งได้", COL_WATCH);
    drain_events(farm->ui);
    status = join_wifi(&joined);
    if ((status == BENTO_OK) && (!joined || !has_address())) {
        *problem = "ต่อ WiFi ไม่ได้";
    }
    if ((status == BENTO_OK) && (*problem == NULL)) {
        status = connect_broker(farm, &linked);
    }
    if ((status == BENTO_OK) && (*problem == NULL) &&
        (!linked ||
         (bento_mqtt_subscribe(farm->broker, farm->topic_cmd,
                               (uint32_t)strlen(farm->topic_cmd), 0U,
                               &granted_qos) != BENTO_OK))) {
        *problem = "broker ไม่ตอบ: รอ 1 นาทีแล้วรันใหม่";
    }
    return status;
}

/* false only when the link is gone (the original's OSError); a
 * publish the broker refuses is a False nobody reads there. */
static bool send_report(const farm_t *farm, const knobs_t *knobs)
{
    char report[REPORT_BYTES];
    bento_status_t status;

    (void)snprintf(
        report, sizeof(report),
        "{\"id\": \"%s\", \"n\": %" PRId32 ", \"soil\": %" PRId32
        ", \"tank\": %" PRId32 ", \"pump\": %d, \"sim\": \"soil tank\""
        ", \"dips\": %" PRId32 ", \"do_min\": %" PRId32 "}",
        TEAM, farm->sent, knobs->oxygen, knobs->tank,
        (farm->pump_ms != 0) ? 1 : 0, farm->dips, farm->do_min);
    status = bento_mqtt_publish(
        farm->broker, farm->topic, (uint32_t)strlen(farm->topic),
        (const uint8_t *)report, (uint32_t)strlen(report), 0U, 0U);
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
 * message waiting and keep the newest. */
static bool take_newest(uint32_t broker, uint8_t *payload,
                        uint32_t *len)
{
    char topic[BENTO_MQTT_TOPIC_MAX]; /* the program never reads it */
    bento_mqtt_msg_t info;
    bool found = false;
    bool more = true;

    while (more) {
        bento_mqtt_msg_init(&info);
        more = bento_mqtt_poll(broker, &info, topic, sizeof(topic),
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

/* The aerator stops first, always; then the program ends politely. */
static bento_status_t stop_here(farm_t *farm, const char *text,
                                uint32_t color)
{
    bento_status_t status;

    show_link(farm, false);
    status = set_pump_led(farm, false);
    if (status == BENTO_OK) {
        status = matrix_off(farm);
    }
    if (status == BENTO_OK) {
        show_note(farm, text, color);
        drain_events(farm->ui);
    }
    farm->stopped = true;
    return status;
}

static bento_status_t act_on(farm_t *farm, const outcome_t *outcome,
                             uint32_t now_ms)
{
    /* DO_NOTHING plays nothing and DO_BEEP plays its own tune: their
     * entries are never read */
    static const tune_t tunes[ACTION_COUNT] = {
        TUNE_TAP, TUNE_BAD, TUNE_STOP, TUNE_START, TUNE_HIT, TUNE_TAP};
    bento_status_t status = BENTO_OK;

    if (outcome->action == DO_OFF) {
        farm->pump_ms = 0;
    } else if (outcome->action == DO_ON) {
        farm->pump_ms = outcome->sec * MS_PER_S;
        farm->pump_start_ms = now_ms;
    } else if (outcome->action == DO_BEEP) {
        status = beep(farm->ui, TUNE_HIT);
    } else if ((outcome->action == DO_SAY) &&
               (outcome->say[0] != '\0') && (farm->pump_ms == 0) &&
               farm->has_matrix) {
        status = bento_rgbmatrix_scroll(
            outcome->say, (uint32_t)strlen(outcome->say),
            BENTO_RGBMATRIX_PURPLE, SCROLL_MS);
    } else {
        /* nothing real to change */
    }
    if ((status == BENTO_OK) && (outcome->action != DO_NOTHING) &&
        (outcome->action != DO_BEEP)) {
        status = beep(farm->ui, tunes[outcome->action]);
    }
    if ((status == BENTO_OK) && (outcome->note[0] != '\0')) {
        show_note(farm, outcome->note, outcome->color);
    }
    return status;
}

static bento_status_t take_command(farm_t *farm, int32_t tank,
                                   uint32_t now_ms)
{
    uint8_t payload[PAYLOAD_KEEP];
    uint32_t len = 0U;
    bento_status_t status = BENTO_OK;

    if (take_newest(farm->broker, payload, &len)) {
        char text[TEXT_BYTES];
        command_t command;
        outcome_t outcome;

        farm->got++;
        (void)snprintf(text, sizeof(text),
                       "เครื่องตีน้ำ (คำสั่ง %" PRId32 ")", farm->got);
        set_text(farm, farm->screen.title, text);
        read_command(payload, len, &command);
        handle_command(&command, tank, &outcome);
        status = act_on(farm, &outcome, now_ms);
    }
    return status;
}

/* SW5 needs no network at all. */
static bento_status_t stop_on_button(farm_t *farm)
{
    char text[TEXT_BYTES];
    bento_status_t status = BENTO_OK;

    if (take_click(&farm->stop_button) && (farm->pump_ms != 0)) {
        farm->pump_ms = 0;
        (void)snprintf(text, sizeof(text), "หยุดเครื่องตีน้ำฉุกเฉิน %s",
                       STOP_BUTTON_NAME);
        show_note(farm, text, COL_GASP);
        status = beep(farm->ui, TUNE_STOP);
    }
    return status;
}

/* A new dip below DO_LOW: count it, sound it, and say it on the
 * matrix while the aerator is not counting down there. */
static bento_status_t start_dip(farm_t *farm, uint32_t now_ms)
{
    static const char low_text[] = "O2 LOW";
    bento_status_t status;

    farm->dips++;
    farm->dipping = true;
    farm->dip_start_ms = now_ms;
    status = beep(farm->ui, TUNE_GASP);
    if ((status == BENTO_OK) && (farm->pump_ms == 0) &&
        farm->has_matrix) {
        status =
            bento_rgbmatrix_scroll(low_text, (uint32_t)strlen(low_text),
                                   BENTO_RGBMATRIX_RED, SCROLL_MS);
    }
    return status;
}

/* DO is back above DO_LOW: the matrix clears unless the aerator
 * is counting down there. */
static bento_status_t end_dip(farm_t *farm)
{
    bento_status_t status = BENTO_OK;

    farm->dipping = false;
    if (farm->pump_ms == 0) {
        status = matrix_off(farm);
    }
    return status;
}

/* The step is drawn once, when it changes. */
static bento_status_t watch_step(farm_t *farm, int32_t step,
                                 uint32_t now_ms)
{
    bento_status_t status = BENTO_OK;

    if (step != farm->step_shown) {
        if ((step == STEP_GASP) && !farm->dipping) {
            status = start_dip(farm, now_ms);
        } else if ((step < STEP_GASP) && farm->dipping) {
            status = end_dip(farm);
        } else {
            /* the dip goes on, or there was none */
        }
        farm->step_shown = step;
        show_step(farm, step);
    }
    return status;
}

/* Both knobs; the lowest DO, the step and the dip. */
static bento_status_t read_knobs(farm_t *farm, uint32_t now_ms,
                                 knobs_t *knobs)
{
    bento_status_t status = knob_percent(KNOB_OXYGEN, &knobs->oxygen);

    if (status == BENTO_OK) {
        status = knob_percent(KNOB_TANK, &knobs->tank);
    }
    if (status == BENTO_OK) {
        if (knobs->oxygen < farm->do_min) {
            farm->do_min = knobs->oxygen;
        }
        status = watch_step(farm, do_step(knobs->oxygen), now_ms);
    }
    if (status == BENTO_OK) {
        show_dip(farm, now_ms);
    }
    return status;
}

static int32_t pump_left_ms(const farm_t *farm, uint32_t now_ms)
{
    return (farm->pump_ms != 0)
               ? (farm->pump_ms -
                  bento_time_ticks_diff(now_ms, farm->pump_start_ms))
               : 0;
}

/* Time is up, or the pond is too low for the paddles. */
static bento_status_t end_pump_if_due(farm_t *farm, uint32_t now_ms,
                                      int32_t tank)
{
    const int32_t left_ms = pump_left_ms(farm, now_ms);
    bento_status_t status = BENTO_OK;

    if ((farm->pump_ms != 0) && ((left_ms <= 0) || (tank < TANK_MIN))) {
        farm->pump_ms = 0;
        show_note(farm,
                  (left_ms <= 0) ? "ครบเวลา หยุดเครื่องเอง"
                                 : "น้ำต่ำกว่าใบพัด หยุดเอง",
                  COL_O2);
        status = beep(farm->ui, TUNE_STOP);
    }
    return status;
}

static bento_status_t show_countdown(farm_t *farm, uint32_t now_ms)
{
    /* rounded up: a 10 s command starts at 10, not 11 */
    int32_t sec_left = 0;
    bento_status_t status = set_pump_led(farm, farm->pump_ms != 0);

    if (farm->pump_ms != 0) {
        sec_left =
            ((pump_left_ms(farm, now_ms) + MS_PER_S) - 1) / MS_PER_S;
    }

    if ((status == BENTO_OK) && (sec_left != farm->shown)) {
        status = matrix_countdown(farm, sec_left);
        show_pump(farm, sec_left);
    }
    return status;
}

static bento_status_t report_if_due(farm_t *farm, uint32_t now_ms,
                                    const knobs_t *knobs)
{
    bento_status_t status = BENTO_OK;

    if (bento_time_ticks_diff(now_ms, farm->send_ms) >= SEND_MS) {
        farm->send_ms = now_ms;
        farm->sent++;
        if (!send_report(farm, knobs)) {
            status = stop_here(farm, "สายหลุด", COL_GASP);
        }
    }
    return status;
}

/* One round of about POLL_MS: get_message() does not block, so
 * never a long sleep. */
static bento_status_t listen_once(farm_t *farm)
{
    const uint32_t now_ms = bento_time_ticks_ms();
    knobs_t knobs = {0, 0};
    bento_status_t status = stop_on_button(farm);

    if (status == BENTO_OK) {
        status = read_knobs(farm, now_ms, &knobs);
    }
    if (status == BENTO_OK) {
        status = take_command(farm, knobs.tank, now_ms);
    }
    if (status == BENTO_OK) {
        status = end_pump_if_due(farm, now_ms, knobs.tank);
    }
    if (status == BENTO_OK) {
        status = show_countdown(farm, now_ms);
    }
    if (status == BENTO_OK) {
        show_pond(farm, &knobs);
        status = report_if_due(farm, now_ms, &knobs);
    }
    if (going_on(status, farm) && !link_is_up(farm->broker)) {
        status = stop_here(farm, "สายหลุด", COL_GASP);
    }
    if (going_on(status, farm)) {
        drain_events(farm->ui);
        status = wait_ms(&farm->stop_button, POLL_MS);
    }
    return status;
}

static bento_status_t listen(farm_t *farm)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    char text[TEXT_BYTES];
    bento_status_t status = BENTO_OK;

    farm->send_ms = start_ms;
    while (going_on(status, farm) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            LISTEN_MS)) {
        status = listen_once(farm);
    }
    if (going_on(status, farm)) {
        (void)snprintf(text, sizeof(text),
                       "เลิกเฝ้า DO ตกต่ำ %" PRId32 " ครั้ง", farm->dips);
        status = stop_here(farm, text, COL_DIM);
    }
    return status;
}

/* Another team's name would make the broker drop that team's
 * board: "team" and two digits. */
static bool team_is_set(const char *team)
{
    const uint32_t length = (uint32_t)strlen(team);
    bool set = (length == (uint32_t)TEAM_LEN) &&
               (strncmp(team, "team", TEAM_PREFIX_LEN) == 0);
    uint32_t i;

    for (i = TEAM_PREFIX_LEN; set && (i < length); i++) {
        set = is_digit((uint8_t)team[i]);
    }
    return set;
}

static bento_status_t run_farm(farm_t *farm)
{
    const char *problem = NULL;
    char text[TEXT_BYTES];
    bento_status_t status;

    /* fire and forget in modui.c; E_UNSUPPORTED = no ui.volume */
    (void)bento_ui_sfx_volume_set(farm->ui, SPEAKER);
    status = build_screen(farm);
    if (status == BENTO_OK) {
        farm->has_pump_led = find_led("RGB_BLUE", &farm->pump_led);
        if (farm->has_matrix) {
            status = bento_rgbmatrix_clear();
        }
    }
    if ((status == BENTO_OK) && !team_is_set(TEAM)) {
        status = stop_here(farm, "แก้ TEAM เป็นเลขกลุ่มก่อน", COL_GASP);
    }
    if (going_on(status, farm)) {
        status = connect_farm(farm, &problem);
    }
    if (going_on(status, farm) && (problem != NULL)) {
        status = stop_here(farm, problem, COL_GASP);
    }
    if (going_on(status, farm)) {
        show_link(farm, true);
        (void)snprintf(text, sizeof(text), "ฟัง %s", farm->topic_cmd);
        show_note(farm, text, COL_CALM);
        status = listen(farm);
    }
    return status;
}

/* TOPIC_CMD and TOPIC of the .py: ROOT/TEAM/cmd and
 * ROOT/TEAM/telemetry. */
static void make_topics(farm_t *farm)
{
    (void)snprintf(farm->topic_cmd, sizeof(farm->topic_cmd),
                   "%s/%s/cmd", ROOT, TEAM);
    (void)snprintf(farm->topic, sizeof(farm->topic), "%s/%s/telemetry",
                   ROOT, TEAM);
}

int main(void)
{
    farm_t farm = {0};
    bento_status_t status;

    farm.ui = bento_runtime_get_ui();
    farm.stop_button.index = STOP_BUTTON;
    farm.has_matrix = bento_rgbmatrix_available();
    farm.shown = -1;
    farm.step_shown = -1;
    farm.do_min = DO_FULL;
    make_topics(&farm);
    status = run_farm(&farm);
    if (farm.has_broker) {
        (void)bento_mqtt_close(
            farm.broker); /* the finally ignores it */
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
