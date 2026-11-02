/*
 * idea_4_greenhouse.c -- โรงเรือนอัจฉริยะ: กราฟดินแห้ง สั่งเปิดวาล์ว
 * รดน้ำจากมือถือ
 * (A smart greenhouse: a dry-soil chart and a watering valve opened
 * from the phone)
 *
 * ต่อ WiFi และ broker แล้วฟังหัวข้อ bento-aiot/<ทีม>/cmd นาน 30 นาที
 * ทุก 100 ms VR1 คือความชื้นดิน (จำลอง) VR4 คือถังพัก กราฟดินย้อนหลัง
 * 2 นาที (จุดละ 2 วินาที) เส้นแดงคือเกณฑ์แห้ง คำสั่ง {"cmd":"pump",
 * "on":1,"sec":10} เปิดวาล์ว (ไฟ RGB สีฟ้าบนบอร์ด) นับถอยหลังบนวง
 * แหวนและ RGB matrix สั่งได้ไม่เกิน PUMP_MAX_S วินาที ถังพักต่ำกว่า
 * TANK_MIN ไม่เปิดและปิดเอง SW5 ปิดวาล์วทันทีโดยไม่ผ่านเน็ต ทุก 5
 * วินาทีส่ง telemetry พร้อม dry_min (นาทีที่ดินแห้งสะสม) และ water_s
 * (วินาทีที่วาล์วเปิดรวม) แก้ WIFI_SSID WIFI_PASS และ TEAM ก่อนรัน
 * เกณฑ์เป็นค่าสาธิตในห้องเรียน ไม่ใช่คำแนะนำทางเกษตร
 *
 * Joins Wi-Fi and the broker and listens on bento-aiot/<team>/cmd
 * for 30 minutes, every 100 ms. VR1 is the soil moisture (simulated)
 * and VR4 the buffer tank. A chart shows the last 2 minutes of soil
 * (a point every 2 s); the red line is the dry limit.
 * {"cmd":"pump","on":1,"sec":10} opens the valve (the board's blue
 * RGB LED) and counts down on a ring and on the RGB matrix, for at
 * most PUMP_MAX_S seconds. Below TANK_MIN the valve does not open
 * and closes itself. SW5 closes it at once, without the network.
 * Every 5 s a telemetry message goes out with dry_min (minutes of
 * dry soil) and water_s (seconds of watering). Edit WIFI_SSID,
 * WIFI_PASS and TEAM first. The limits are classroom demo values,
 * not farming advice.
 *
 * Twin of idea_4_greenhouse.py
 * (aiot-development-for-smart-farm/project/idea_4_greenhouse.py).
 * Style: CLEAN_C.md.
 * Differs from the .py:
 *   - json: the report is built with snprintf, keys in the order the
 *     .py writes them (MicroPython's dict may print them in another
 *     order; a JSON reader does not care).
 *   - json.loads: read_command() reads one standard JSON object and
 *     finds "cmd", "on", "n", "sec" and "text" at its top level (the
 *     last of two equal keys wins). MicroPython also takes some
 *     broken texts (a comma or colon missing); here those are not
 *     JSON. Inside a nested list or object only strings and brackets
 *     are checked.
 *   - str() of a value: a string, an integer, true, false and null
 *     read as MicroPython writes them; a real number, a list or an
 *     object is shown as its JSON text, and a string's escapes are
 *     not decoded. Texts are cut in bytes, as on the PSoC Edge.
 *   - get_message() keeps one message, a newer one overwrites it;
 *     the C client keeps a small ring, so each round takes them all
 *     and keeps the newest. Payloads past 255 bytes are cut, as the
 *     board's binding cuts them.
 *   - wifi.connect(): a name or password the radio refuses outright
 *     counts as not joined (the .py's binding cuts it to fit).
 *   - send_report(): the .py catches only the OSError of a dropped
 *     link; a publish the client refuses still counts as sent here.
 *   - mqtt.connect() is start and poll here; a failed attempt closes
 *     its client so the next attempt has a free one.
 *   - On a board without pots, SW5 or the RGB matrix (the .py cannot
 *     import them there) the pots read 0, SW5 is never down and the
 *     matrix is skipped; any other failure of them ends the program.
 *   - The wifi and mqtt modules' own console lines are not printed.
 *   - In the BENTO Emulator, MQTT from C is not available yet: the
 *     program says so and ends on the
 *     .py's broker-down screen.
 *   - Stop ends the program quietly, where MicroPython raises
 *     KeyboardInterrupt; the client is closed in both.
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

/* ---- 1) Settings: edit these before running ---- */
/* WIFI_SSID and WIFI_PASS: edit them at the top of join_wifi(). */
static const char TEAM[] = "teamXX";

/* The valve, the soil and the timing. */
enum {
    PUMP_DEFAULT_S = 10,
    PUMP_MAX_S = 30, /* whoever asks for 9999 s gets 30 */
    TANK_MIN = 10,   /* below this the valve stays shut: no dry run */
    SOIL_DRY = 30,   /* demo value: VR1 below this is dry */
    SEND_MS = 5000,
    POLL_MS = 100,
    LISTEN_MS = 1800000,
    CHART_MS = 2000, /* 60 points 2 s apart: the last 2 minutes */
    CHART_POINTS = 60,
    SAMPLE_MS = 20,  /* how often SW5 is looked at while waiting */
    SETTLE_MS = 200, /* let the cleared screen settle */
    CONNECT_POLL_MS = 20,
    MS_PER_S = 1000,
    MS_PER_MIN = 60000,
    ROUND_UP_MS = 999,
    SPEAKER = 40,
    VOLUME = 25,
    NOTE_MS = 90,
    NOTE_GAP_MS = 100,
    MATRIX_MS = 80,
    BROKER_PORT = 1883,
    KEEPALIVE_S = 60,
    BROKER_TRIES = 3,
    CLIENT_SUFFIX_MASK = 0xFFFF,
    SOIL_POT = 0, /* VR1 */
    TANK_POT = 3, /* VR4 */
    POT_FULL = 4095,
    PERCENT = 100,
    STOP_BUTTON = 1, /* SW5 */
    VALVE_SHOWN_MAX = 6,
    ACT_SHOWN_MAX = 12,
    SAY_MAX = 20,
    TEAM_LENGTH = 6,
    TEAM_PREFIX_LENGTH = 4
};

/* The greenhouse: leaf green, soil brown, water blue, 0xRRGGBB. */
enum {
    COL_TEXT = 0xE6F0E4,
    COL_DIM = 0x8FA394,
    COL_CARD = 0x14221A,
    COL_LEAF = 0x5CC27E,
    COL_SOIL = 0xC28A4E,
    COL_RAIN = 0x4FB3E8,
    COL_DRY = 0xF0A030,
    COL_BAD = 0xE5484D
};

/* The layout of the cards. */
enum {
    CARD_RADIUS = 12,
    CARD_TITLE_DX = 12,
    CARD_TITLE_DY = 6,
    CARD_TITLE_PX = 20,
    AUTO = -1 /* the factory's own size */
};

/* Where the emulator's note goes: free space on this screen. */
enum {
    GAP_X = 316,
    GAP_Y = 336,
    GAP_W = 360, /* wraps the note onto two lines */
    GAP_PX = 20
};

/* Buffer sizes, in bytes. */
enum {
    TEXT_BYTES = 128, /* a label text: 126 at most */
    TOPIC_BYTES = 48,
    CLIENT_BYTES = 32,
    PAYLOAD_BYTES = 256, /* the binding keeps 255 and a NUL */
    TOPIC_KEEP = 64,     /* the program never reads the topic */
    JSON_BYTES = 256,
    SAY_BYTES = SAY_MAX + 1
};

/* The tunes, by name. */
enum {
    TUNE_TAP = 0,
    TUNE_START,
    TUNE_STOP,
    TUNE_GOOD,
    TUNE_BAD,
    TUNE_EMPTY,
    TUNE_HIT,
    TUNE_COUNT
};

/* The longest tune, in notes. */
enum {
    TUNE_NOTES_MAX = 3
};

/* The widgets the program changes after drawing them. */
enum {
    SLOT_CHART = 0,
    SLOT_SOIL,
    SLOT_TITLE,
    SLOT_LED,
    SLOT_PUMP,
    SLOT_ARC,
    SLOT_TANK_BAR,
    SLOT_TANK,
    SLOT_STATS,
    SLOT_NOTE,
    SLOT_LINK_LED,
    SLOT_LINK_TEXT,
    SLOT_COUNT
};

/* A widget nobody changes. */
enum {
    SLOT_NONE = SLOT_COUNT
};

/* One widget as the factory takes it. */
typedef struct {
    uint8_t kind;
    uint8_t slot;
    const char *text;
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    uint32_t color;
    int32_t minimum;
    int32_t maximum;
    int32_t value;
} widget_spec_t;

/* A dark card with its title in its own colour. */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    const char *title;
    uint32_t color;
    uint8_t slot; /* the title's slot */
} card_t;

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t ids[SLOT_COUNT + 1]; /* + the SLOT_NONE scratch */
    uint8_t dry_series;                /* the chart's red dry line */
} screen_t;

/* The valve, the counters and the link. */
typedef struct {
    uint32_t broker; /* the MQTT client, 0 when none */
    bool has_led;    /* the blue LED that stands for the valve */
    uint32_t led;
    int32_t got;     /* commands received */
    int32_t sent;    /* reports sent */
    int32_t pump_ms; /* how long the valve was opened for, 0 shut */
    uint32_t pump_start_ms;
    int32_t dry_ms;   /* time the soil was dry */
    int32_t water_ms; /* time the valve was open */
    int32_t shown_s;  /* the countdown on show, -1 none yet */
    int32_t shown_dry_min;
    int32_t shown_water_s;
    bool stats_shown;
    uint32_t send_ms;
    uint32_t chart_ms;
    uint32_t last_ms;
    bool stop_down; /* SW5 as last sampled */
    bool stop_clicked;
    bool stopped; /* the .py's raise Stop */
    bool no_mqtt; /* the BENTO Emulator has no MQTT from C */
    char topic_cmd[TOPIC_BYTES];
    char topic[TOPIC_BYTES];
} valve_t;

/* What a JSON value is. */
typedef enum {
    JSON_ABSENT = 0,
    JSON_TEXT,
    JSON_INTEGER,
    JSON_REAL,
    JSON_TRUE,
    JSON_FALSE,
    JSON_NULL,
    JSON_LIST,
    JSON_OBJECT
} json_kind_t;

/* One value: its kind and where it is in the payload. */
typedef struct {
    json_kind_t kind;
    uint32_t
        start; /* a text's bytes inside the quotes, else the token */
    uint32_t end;
    int32_t number; /* an integer; above any limit when it is huge */
    bool zero;      /* a number whose digits are all 0 */
    bool empty;     /* a list or object with nothing in it */
} json_field_t;

typedef struct {
    const uint8_t *text;
    uint32_t length;
    uint32_t at;
    bool valid; /* false once the text is not JSON */
    json_field_t cmd;
    json_field_t on;
    json_field_t n;
    json_field_t sec;
    json_field_t say;
} command_t;

/* What one command asks for. */
typedef enum {
    ACTION_NONE = 0,
    ACTION_DENY,
    ACTION_OFF,
    ACTION_ON,
    ACTION_BEEP,
    ACTION_SAY
} action_t;

typedef struct {
    action_t action;
    int32_t seconds;
    char say[SAY_BYTES];   /* the English text for the matrix */
    char note[TEXT_BYTES]; /* the screen's line, "" for none */
    uint32_t color;
} decision_t;

/* ---- 2) Hardware ---- */

static bento_status_t rest(uint32_t wait_ms)
{
    bento_status_t status = bento_time_sleep_ms(wait_ms);

    if ((status == BENTO_OK) && !bento_runtime_is_running()) {
        status = BENTO_E_CANCELLED;
    }
    return status;
}

/* ui.poll() answers no events this program reads. */
static void drain_events(bento_ui_t *ui)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    (void)bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                        NULL);
}

static bento_status_t beep(bento_ui_t *ui, int32_t tune)
{
    typedef struct {
        uint8_t notes[TUNE_NOTES_MAX];
        uint32_t count;
    } tune_t;
    static const tune_t tunes[TUNE_COUNT] = {
        {{76U, 0U, 0U}, 1U},  {{72U, 79U, 0U}, 2U},
        {{79U, 72U, 0U}, 2U}, {{72U, 79U, 84U}, 3U},
        {{84U, 76U, 0U}, 2U}, {{84U, 76U, 69U}, 3U},
        {{88U, 0U, 0U}, 1U},
    };
    const tune_t *chosen = &tunes[tune];
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < chosen->count) && (status == BENTO_OK); i++) {
        /* ui.tone is fire-and-forget; a build without sound refuses */
        (void)bento_ui_sfx_tone(ui, chosen->notes[i],
                                (uint8_t)BENTO_UI_WAVE_SINE, VOLUME,
                                NOTE_MS);
        status = rest(NOTE_GAP_MS);
    }
    return status;
}

/* A board without the matrix just goes without it. */
static bento_status_t matrix_done(bento_status_t status)
{
    return (status == BENTO_E_UNSUPPORTED) ? BENTO_OK : status;
}

/* A pot as 0-100; a board without pots reads 0. */
static bento_status_t knob_percent(uint32_t pot, int32_t *percent)
{
    uint16_t raw = 0U;
    const bento_status_t status = bento_pots_read(pot, &raw);

    *percent = ((int32_t)raw * PERCENT) / POT_FULL;
    return (status == BENTO_E_UNSUPPORTED) ? BENTO_OK : status;
}

/* The LED with this name, else LED 0, switched off; none when the
 * board cannot say or has no LED. */
static void find_led(valve_t *valve, const char *wanted)
{
    bento_gpio_board_info_t info;
    const char *name = NULL;
    uint32_t found = 0U;
    uint32_t i;

    bento_gpio_board_info_init(&info);
    valve->has_led = (bento_gpio_board_info(&info) == BENTO_OK) &&
                     (bento_gpio_num_leds() > 0U);
    for (i = bento_gpio_num_leds(); valve->has_led && (i > 0U); i--) {
        if ((bento_gpio_led_name(i - 1U, &name) == BENTO_OK) &&
            (name != NULL) && (strcmp(name, wanted) == 0)) {
            found = i - 1U;
        }
    }
    valve->led = found;
    valve->has_led =
        valve->has_led && (bento_gpio_led_off(found) == BENTO_OK);
}

static bento_status_t set_led(const valve_t *valve, bool on)
{
    bento_status_t status = BENTO_OK;

    if (valve->has_led) {
        status = on ? bento_gpio_led_on(valve->led)
                    : bento_gpio_led_off(valve->led);
    }
    return status;
}

/* SW5 is sampled while the program waits, so a short press counts. */
static bento_status_t sample_stop(valve_t *valve)
{
    bool down = false;
    bento_status_t status = bento_buttons_pressed(STOP_BUTTON, &down);

    if (status == BENTO_E_UNSUPPORTED) {
        down = false;
        status = BENTO_OK;
    }
    if (down && !valve->stop_down) {
        valve->stop_clicked = true;
    }
    valve->stop_down = down;
    return status;
}

static bool stop_pressed_now(valve_t *valve)
{
    const bool fired = valve->stop_clicked;

    valve->stop_clicked = false;
    return fired;
}

static bento_status_t wait_sampling(valve_t *valve, int32_t wait_ms)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            wait_ms)) {
        status = sample_stop(valve);
        if (status == BENTO_OK) {
            status = rest(SAMPLE_MS);
        }
    }
    return status;
}

/* The seconds left as a score, cleared when it starts or ends. */
static bento_status_t matrix_countdown(int32_t left_s, int32_t shown_s)
{
    bento_status_t status = BENTO_OK;

    if ((shown_s <= 0) || (left_s == 0)) {
        status = matrix_done(bento_rgbmatrix_scroll(
            NULL, 0U, BENTO_RGBMATRIX_SCROLL_COLOR_DEFAULT,
            BENTO_RGBMATRIX_SCROLL_MS_DEFAULT));
        if (status == BENTO_OK) {
            status = matrix_done(bento_rgbmatrix_clear());
        }
    }
    if ((status == BENTO_OK) && (left_s != 0)) {
        status = matrix_done(
            bento_rgbmatrix_score(left_s, BENTO_RGBMATRIX_GREEN));
    }
    return status;
}

/* ---- 3) Reading a command ---- */

static bool is_digit(uint8_t byte)
{
    return (byte >= (uint8_t)'0') && (byte <= (uint8_t)'9');
}

static bool is_space(uint8_t byte)
{
    return (byte == (uint8_t)' ') || (byte == (uint8_t)'\t') ||
           (byte == (uint8_t)'\n') || (byte == (uint8_t)'\r');
}

/* The byte under the reader, 0 past the end. */
static uint8_t peek(const command_t *command)
{
    return (command->at < command->length) ? command->text[command->at]
                                           : 0U;
}

static void skip_space(command_t *command)
{
    while ((command->at < command->length) &&
           is_space(command->text[command->at])) {
        command->at++;
    }
}

static void expect(command_t *command, uint8_t wanted)
{
    skip_space(command);
    if (peek(command) == wanted) {
        command->at++;
    } else {
        command->valid = false;
    }
}

/* A string: the reader stands on its opening quote. */
static void read_string(command_t *command, json_field_t *field)
{
    bool closed = false;

    command->at++;
    field->start = command->at;
    while (command->valid && !closed) {
        const uint8_t byte = peek(command);

        if ((command->at >= command->length) || (byte < (uint8_t)' ')) {
            command->valid = false;
        } else if (byte == (uint8_t)'"') {
            closed = true;
            field->end = command->at;
            command->at++;
        } else {
            command->at += (byte == (uint8_t)'\\') ? 2U : 1U;
        }
    }
    field->kind = JSON_TEXT;
}

static void read_digits(command_t *command, json_field_t *field)
{
    const int32_t too_big = 1000000; /* past every limit asked for */
    const int32_t base = 10;
    bool any = false;

    while (is_digit(peek(command))) {
        if (field->number <= too_big) {
            field->number = (field->number * base) +
                            (int32_t)peek(command) - (int32_t)'0';
        }
        field->zero = field->zero && (peek(command) == (uint8_t)'0');
        any = true;
        command->at++;
    }
    command->valid = command->valid && any;
}

static void read_exponent(command_t *command)
{
    json_field_t ignored = {JSON_ABSENT, 0U, 0U, 0, true, false};

    command->at++;
    if ((peek(command) == (uint8_t)'+') ||
        (peek(command) == (uint8_t)'-')) {
        command->at++;
    }
    read_digits(command, &ignored);
}

/* A number; an integer when it has no fraction and no exponent. */
static void read_number(command_t *command, json_field_t *field)
{
    const bool negative = peek(command) == (uint8_t)'-';

    field->start = command->at;
    field->kind = JSON_INTEGER;
    field->number = 0;
    field->zero = true;
    if (negative) {
        command->at++;
    }
    read_digits(command, field);
    if (peek(command) == (uint8_t)'.') {
        command->at++;
        read_digits(command, field);
        field->kind = JSON_REAL;
    }
    if ((peek(command) == (uint8_t)'e') ||
        (peek(command) == (uint8_t)'E')) {
        read_exponent(command);
        field->kind = JSON_REAL;
    }
    field->number = negative ? -field->number : field->number;
    field->end = command->at;
}

/* true, false or null: every letter must be there. */
static void read_word(command_t *command, json_field_t *field)
{
    typedef struct {
        const char *word;
        json_kind_t kind;
    } word_t;
    static const word_t words[] = {{"true", JSON_TRUE},
                                   {"false", JSON_FALSE},
                                   {"null", JSON_NULL}};
    bool known = false;
    uint32_t i;

    field->start = command->at;
    for (i = 0U; !known && (i < (sizeof(words) / sizeof(words[0])));
         i++) {
        const uint32_t length = (uint32_t)strlen(words[i].word);

        known = ((command->length - command->at) >= length) &&
                (strncmp((const char *)&command->text[command->at],
                         words[i].word, length) == 0);
        if (known) {
            command->at += length;
            field->kind = words[i].kind;
        }
    }
    field->end = command->at;
    command->valid = known;
}

/* A nested list or object: its strings and brackets are checked. */
static void skip_nested(command_t *command, json_field_t *field)
{
    json_field_t ignored = {JSON_ABSENT, 0U, 0U, 0, false, false};
    int32_t depth = 0;

    field->kind =
        (peek(command) == (uint8_t)'{') ? JSON_OBJECT : JSON_LIST;
    field->start = command->at;
    do {
        const uint8_t byte = peek(command);

        if (command->at >= command->length) {
            command->valid = false;
        } else if (byte == (uint8_t)'"') {
            read_string(command, &ignored);
        } else {
            depth += ((byte == (uint8_t)'{') || (byte == (uint8_t)'['))
                         ? 1
                         : 0;
            depth -= ((byte == (uint8_t)'}') || (byte == (uint8_t)']'))
                         ? 1
                         : 0;
            command->at++;
        }
    } while (command->valid && (depth > 0));
    field->end = command->at;
}

/* A list or object holding nothing but spaces. */
static bool holds_nothing(const command_t *command,
                          const json_field_t *field)
{
    bool nothing = true;
    uint32_t i;

    for (i = field->start + 1U; nothing && ((i + 1U) < field->end);
         i++) {
        nothing = is_space(command->text[i]);
    }
    return nothing;
}

static void read_value(command_t *command, json_field_t *field)
{
    const uint8_t byte = peek(command);

    if (byte == (uint8_t)'"') {
        read_string(command, field);
    } else if ((byte == (uint8_t)'{') || (byte == (uint8_t)'[')) {
        skip_nested(command, field);
        field->empty = command->valid && holds_nothing(command, field);
    } else if ((byte == (uint8_t)'-') || is_digit(byte)) {
        read_number(command, field);
    } else {
        read_word(command, field);
    }
}

static bool text_is(const command_t *command, const json_field_t *field,
                    const char *wanted)
{
    const uint32_t length = (uint32_t)strlen(wanted);

    return (field->kind == JSON_TEXT) &&
           ((field->end - field->start) == length) &&
           (strncmp((const char *)&command->text[field->start], wanted,
                    length) == 0);
}

/* The field a key names, NULL for one the program never reads. */
static json_field_t *field_named(command_t *command,
                                 const json_field_t *key)
{
    json_field_t *field = NULL;

    if (text_is(command, key, "cmd")) {
        field = &command->cmd;
    } else if (text_is(command, key, "on")) {
        field = &command->on;
    } else if (text_is(command, key, "n")) {
        field = &command->n;
    } else if (text_is(command, key, "sec")) {
        field = &command->sec;
    } else if (text_is(command, key, "text")) {
        field = &command->say;
    } else {
        /* a key nobody asks for */
    }
    return field;
}

/* One "key": value pair; the value lands in the field it names. */
static void read_member(command_t *command)
{
    json_field_t key = {JSON_ABSENT, 0U, 0U, 0, false, false};
    json_field_t value = {JSON_ABSENT, 0U, 0U, 0, false, false};
    json_field_t *field = NULL;

    skip_space(command);
    if (peek(command) == (uint8_t)'"') {
        read_string(command, &key);
    } else {
        command->valid = false;
    }
    expect(command, (uint8_t)':');
    skip_space(command);
    if (command->valid) {
        read_value(command, &value);
    }
    if (command->valid) {
        field = field_named(command, &key);
    }
    if (field != NULL) {
        *field = value;
    }
}

/* json.loads of one object; command->valid says if it was one. */
static void read_command(command_t *command)
{
    bool more = true;

    expect(command, (uint8_t)'{');
    skip_space(command);
    if (command->valid && (peek(command) == (uint8_t)'}')) {
        command->at++;
        more = false;
    }
    while (command->valid && more) {
        read_member(command);
        skip_space(command);
        more = peek(command) == (uint8_t)',';
        if (more) {
            command->at++;
        } else {
            expect(command, (uint8_t)'}');
        }
    }
    skip_space(command);
    command->valid = command->valid && (command->at == command->length);
}

/* Python's truth of a value: 0, "", false, null, [] and {} are
 * false. */
static bool is_true(const json_field_t *field)
{
    bool truth = false;

    if (field->kind == JSON_TEXT) {
        truth = field->end > field->start;
    } else if ((field->kind == JSON_INTEGER) ||
               (field->kind == JSON_REAL)) {
        truth = !field->zero;
    } else if ((field->kind == JSON_LIST) ||
               (field->kind == JSON_OBJECT)) {
        truth = !field->empty;
    } else {
        truth = field->kind == JSON_TRUE;
    }
    return truth;
}

/* str() of a value, at most max_bytes of it. */
static void value_text(const command_t *command,
                       const json_field_t *field, char *text,
                       uint32_t size, uint32_t max_bytes)
{
    const char *word = NULL;
    uint32_t length = field->end - field->start;

    if (field->kind == JSON_TRUE) {
        word = "True";
    } else if (field->kind == JSON_FALSE) {
        word = "False";
    } else if (field->kind == JSON_NULL) {
        word = "None";
    } else {
        /* the bytes as they are written */
    }
    if (word != NULL) {
        length = (uint32_t)strlen(word);
    }
    length = (length < max_bytes) ? length : max_bytes;
    length = (length < (size - 1U)) ? length : (size - 1U);
    (void)memcpy(text,
                 (word != NULL)
                     ? word
                     : (const char *)&command->text[field->start],
                 length);
    text[length] = '\0';
}

/* ---- 3) The brain ---- */

/* The seconds asked for: not a positive integer means the default,
 * too many means the ceiling. */
static int32_t pump_seconds(const json_field_t *sec)
{
    int32_t seconds = PUMP_DEFAULT_S;

    if ((sec->kind == JSON_INTEGER) && (sec->number > 0)) {
        seconds = sec->number;
    }
    return (seconds < PUMP_MAX_S) ? seconds : PUMP_MAX_S;
}

/* str(text), printable English only, 20 bytes at most. */
static void ascii_only(const command_t *command, char *say)
{
    char text[PAYLOAD_BYTES];
    uint32_t kept = 0U;
    uint32_t i;

    value_text(command, &command->say, text, sizeof(text),
               PAYLOAD_BYTES);
    for (i = 0U; (text[i] != '\0') && (kept < (uint32_t)SAY_MAX); i++) {
        if ((text[i] >= ' ') && (text[i] <= '~')) {
            say[kept] = text[i];
            kept++;
        }
    }
    say[kept] = '\0';
}

static void decide(decision_t *decision, action_t action,
                   const char *note, uint32_t color)
{
    decision->action = action;
    (void)snprintf(decision->note, sizeof(decision->note), "%s", note);
    decision->color = color;
}

/* "led" (valve 0 only) and "pump": open, close or refuse. */
static void decide_valve(const command_t *command, int32_t tank,
                         decision_t *decision)
{
    char shown[TEXT_BYTES];
    const bool valve_zero = (command->n.kind == JSON_ABSENT) ||
                            (command->n.kind == JSON_FALSE) ||
                            (((command->n.kind == JSON_INTEGER) ||
                              (command->n.kind == JSON_REAL)) &&
                             command->n.zero);
    const bool on =
        (command->on.kind == JSON_ABSENT) || is_true(&command->on);

    if (text_is(command, &command->cmd, "led") && !valve_zero) {
        value_text(command, &command->n, shown, sizeof(shown),
                   VALVE_SHOWN_MAX);
        (void)snprintf(decision->note, sizeof(decision->note),
                       "ไม่มีวาล์วที่ %s", shown);
        decision->action = ACTION_DENY;
        decision->color = COL_DRY;
    } else if (!on) {
        decide(decision, ACTION_OFF, "สั่งปิดวาล์วแล้ว", COL_LEAF);
    } else if (tank < TANK_MIN) {
        (void)snprintf(decision->note, sizeof(decision->note),
                       "ถังพักเหลือ %" PRId32 "%% ไม่เปิดวาล์ว", tank);
        decision->action = ACTION_DENY;
        decision->color = COL_BAD;
    } else {
        decision->seconds = pump_seconds(&command->sec);
        (void)snprintf(decision->note, sizeof(decision->note),
                       "เปิดวาล์วรดแปลง %" PRId32 " วิ",
                       decision->seconds);
        decision->action = ACTION_ON;
        decision->color = COL_RAIN;
    }
}

/* "beep", "say", "ack" and anything else. */
static void decide_other(const command_t *command, decision_t *decision)
{
    char shown[TEXT_BYTES];
    const json_field_t *act = &command->cmd;

    if (text_is(command, act, "beep")) {
        decide(decision, ACTION_BEEP, "เจ้าของโรงเรือนเรียก!", COL_RAIN);
    } else if (text_is(command, act, "say")) {
        ascii_only(command, decision->say);
        (void)snprintf(decision->note, sizeof(decision->note),
                       "ข้อความ: %s",
                       (decision->say[0] != '\0') ? decision->say
                                                  : "ไม่มีอักษรอังกฤษ");
        decision->action = ACTION_SAY;
        decision->color = COL_RAIN;
    } else if (text_is(command, act, "ack")) { /* ack is sf2_04's */
        decide(decision, ACTION_NONE, "ไม่มีแจ้งเตือน (sf2_04)", COL_DIM);
    } else if ((act->kind != JSON_ABSENT) &&
               !text_is(command, act, "")) {
        value_text(command, act, shown, sizeof(shown), ACT_SHOWN_MAX);
        (void)snprintf(decision->note, sizeof(decision->note),
                       "ไม่รู้จัก %s", shown);
        decision->action = ACTION_DENY;
        decision->color = COL_DRY;
    } else {
        decide(decision, ACTION_NONE, "", COL_DIM);
    }
}

/* One command decided; nothing real is touched here. */
static void handle_command(const uint8_t *payload, uint32_t length,
                           int32_t tank, decision_t *decision)
{
    command_t command;

    (void)memset(&command, 0, sizeof(command));
    (void)memset(decision, 0, sizeof(*decision));
    command.text = payload;
    command.length = length;
    command.valid = true;
    read_command(&command);
    if (!command.valid) { /* 5, null or [] are JSON, not commands */
        decide(decision, ACTION_DENY, "อ่านคำสั่งไม่ได้", COL_BAD);
    } else if (text_is(&command, &command.cmd, "led") ||
               text_is(&command, &command.cmd, "pump")) {
        decide_valve(&command, tank, decision);
    } else {
        decide_other(&command, decision);
    }
}

/* ---- 4) The network ---- */

/* wifi.connect(): any refusal is "not joined", Stop is Stop. */
static bento_status_t join_wifi(bool *joined)
{
    /* edit these two: short, English or digits; 8 or more, share no
     * real one */
    static const char wifi_ssid[] = "<ชื่อ Hotspot ของกลุ่ม>";
    static const char wifi_pass[] = "<รหัส Hotspot ของกลุ่ม>";
    bento_wifi_connect_params_t params;
    uint32_t address = 0U;
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
        status = rest(CONNECT_POLL_MS);
        if (status == BENTO_OK) {
            status = bento_wifi_connect_poll();
        } else { /* Stop between two polls: leave the radio alone */
            (void)bento_wifi_connect_cancel(); /* nothing to report */
        }
    }
    *joined = status == BENTO_OK;
    if (*joined &&
        (bento_wifi_ip(BENTO_WIFI_IF_ANY, &address) != BENTO_OK)) {
        address = 0U; /* an address that cannot be read is 0.0.0.0 */
    }
    *joined = *joined && (address != 0U);
    if (status != BENTO_E_CANCELLED) {
        status = BENTO_OK;
    }
    return status;
}

static bento_status_t wait_for_broker(uint32_t broker)
{
    bento_status_t status = BENTO_E_IN_PROGRESS;

    while (status == BENTO_E_IN_PROGRESS) {
        status = rest(CONNECT_POLL_MS);
        if (status == BENTO_OK) {
            status = bento_mqtt_open_poll(broker);
        }
    }
    return status;
}

/* One mqtt.connect() with a new client id, so an old session on
 * the broker never clashes with it. */
static bento_status_t try_broker(valve_t *valve, bool *linked)
{
    static const char broker[] = "broker.hivemq.com";
    static const char client_base[] =
        "bento-farm-"; /* + TEAM + a suffix */
    char client_id[CLIENT_BYTES];
    bento_mqtt_config_t config;
    const uint32_t suffix =
        bento_time_ticks_ms() & (uint32_t)CLIENT_SUFFIX_MASK;
    bento_status_t status;

    (void)snprintf(client_id, sizeof(client_id), "%s%s-%04" PRIx32,
                   client_base, TEAM, suffix);
    bento_mqtt_config_init(&config);
    config.host = broker;
    config.host_len = (uint32_t)strlen(broker);
    config.port = (uint16_t)BROKER_PORT;
    config.keepalive_s = (uint16_t)KEEPALIVE_S;
    config.client_id = client_id;
    config.client_id_len = (uint32_t)strlen(client_id);
    status = bento_mqtt_open_start(&config, &valve->broker);
    valve->no_mqtt = status == BENTO_E_UNSUPPORTED;
    if (status == BENTO_OK) {
        status = wait_for_broker(valve->broker);
    }
    *linked = status == BENTO_OK;
    if (!*linked && (valve->broker != 0U)) {
        (void)bento_mqtt_close(valve->broker); /* a failed attempt */
        valve->broker = 0U;
    }
    if (status != BENTO_E_CANCELLED) {
        status = BENTO_OK;
    }
    return status;
}

/* ---- 5) The screen ---- */

static int16_t px(int32_t value)
{
    return (int16_t)value;
}

/* The widget setters ignore a refusal in MicroPython (modui.c). */
static void show_text(const screen_t *screen, uint32_t slot,
                      const char *text)
{
    (void)bento_ui_set_text(screen->ui, screen->ids[slot], text,
                            (uint32_t)strlen(text));
}

static void show_color(const screen_t *screen, uint32_t slot,
                       uint32_t color)
{
    (void)bento_ui_set_color(screen->ui, screen->ids[slot], color);
}

static void show_value(const screen_t *screen, uint32_t slot,
                       int32_t value)
{
    (void)bento_ui_set_value(screen->ui, screen->ids[slot], value);
}

static void show_note(const screen_t *screen, const char *text,
                      uint32_t color)
{
    show_color(screen, SLOT_NOTE, color);
    show_text(screen, SLOT_NOTE, text);
}

static void show_link(const screen_t *screen, bool linked)
{
    show_value(screen, SLOT_LINK_LED, linked ? 1 : 0);
    show_text(screen, SLOT_LINK_TEXT,
              linked ? "MQTT: เชื่อมต่อแล้ว" : "MQTT: ออฟไลน์");
}

static bento_status_t add_widget(screen_t *screen,
                                 const widget_spec_t *spec)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    if (spec->text != NULL) {
        args.text = spec->text;
        args.text_len = (uint32_t)strlen(spec->text);
    }
    args.x = spec->x;
    args.y = spec->y;
    args.w = spec->w;
    args.h = spec->h;
    args.color = spec->color;
    args.min_val = spec->minimum;
    args.max_val = spec->maximum;
    args.font_px = spec->value;
    return bento_ui_create(screen->ui, spec->kind, &args,
                           &screen->ids[spec->slot]);
}

static bento_status_t add_widgets(screen_t *screen,
                                  const widget_spec_t *specs,
                                  uint32_t count)
{
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < count) && (status == BENTO_OK); i++) {
        status = add_widget(screen, &specs[i]);
    }
    return status;
}

static bento_status_t add_card(screen_t *screen, const card_t *card)
{
    const widget_spec_t panel = {
        UI_WIDGET_PANEL, SLOT_NONE, NULL,
        card->x,         card->y,   card->w,
        card->h,         COL_CARD,  (int32_t)card->color,
        CARD_RADIUS,     1};
    const widget_spec_t title = {UI_WIDGET_LABEL,
                                 card->slot,
                                 card->title,
                                 px(card->x + CARD_TITLE_DX),
                                 px(card->y + CARD_TITLE_DY),
                                 AUTO,
                                 AUTO,
                                 card->color,
                                 0,
                                 0,
                                 CARD_TITLE_PX};
    bento_status_t status = add_widget(screen, &panel);

    if (status == BENTO_OK) {
        status = add_widget(screen, &title);
    }
    return status;
}

/* The title and the soil chart card, with its red dry line. */
static bento_status_t add_soil_card(screen_t *screen)
{
    static const widget_spec_t title = {
        UI_WIDGET_LABEL,
        SLOT_NONE,
        "โรงเรือน: ดินแห้งตรงไหน รดจากมือถือ",
        12,
        6,
        AUTO,
        AUTO,
        COL_LEAF,
        0,
        0,
        24};
    static const card_t card = {
        12,       44,       470, 252, "ความชื้นดิน (VR1) ย้อนหลัง 2 นาที",
        COL_SOIL, SLOT_NONE};
    static const widget_spec_t specs[] = {
        {UI_WIDGET_CHART, SLOT_CHART, NULL, 24, 80, 446, 150, COL_SOIL,
         0, PERCENT, 0},
        {UI_WIDGET_LABEL, SLOT_SOIL, "ดิน -- %", 24, 234, AUTO, AUTO,
         COL_TEXT, 0, 0, 28},
    };
    bento_status_t status = add_widget(screen, &title);

    if (status == BENTO_OK) {
        status = add_card(screen, &card);
    }
    if (status == BENTO_OK) {
        status = add_widget(screen, &specs[0]);
    }
    if (status == BENTO_OK) {
        /* prop() is a setter: a refusal was ignored before too */
        (void)bento_ui_set_prop(screen->ui, screen->ids[SLOT_CHART],
                                BENTO_UI_PROP_CHART_POINTS,
                                CHART_POINTS);
        status = bento_ui_chart_add_series(
            screen->ui, screen->ids[SLOT_CHART], COL_BAD,
            &screen->dry_series);
    }
    if (status == BENTO_OK) {
        status = add_widget(screen, &specs[1]);
    }
    return status;
}

/* The dry line's key, the valve card and its countdown ring. */
static bento_status_t add_valve_card(screen_t *screen)
{
    static const card_t card = {
        494, 44, 286, 252, "วาล์วแปลง 0 (คำสั่ง 0)", COL_RAIN, SLOT_TITLE};
    static const widget_spec_t specs[] = {
        {UI_WIDGET_LED, SLOT_LED, NULL, 510, 82, 40, 40, COL_RAIN, 0, 0,
         0},
        {UI_WIDGET_LABEL, SLOT_PUMP, "ปิด", 562, 88, AUTO, AUTO, COL_DIM,
         0, 0, 24},
        {UI_WIDGET_ARC, SLOT_ARC, NULL, 576, 134, 120, 120, 0U, 0,
         PUMP_MAX_S, 0},
        {UI_WIDGET_LABEL, SLOT_NONE, "SW5 = ปิดวาล์วทันที", 508, 266, AUTO,
         AUTO, COL_DRY, 0, 0, 20},
    };
    char key_text[TEXT_BYTES];
    bento_status_t status;

    (void)snprintf(key_text, sizeof(key_text),
                   "เส้นแดง = แห้งที่ %" PRId32 " %%", (int32_t)SOIL_DRY);
    {
        const widget_spec_t key = {
            UI_WIDGET_LABEL, SLOT_NONE, key_text, 220, 244, AUTO, AUTO,
            COL_BAD,         0,         0,        20};

        status = add_widget(screen, &key);
    }
    if (status == BENTO_OK) {
        status = add_card(screen, &card);
    }
    if (status == BENTO_OK) {
        status = add_widgets(screen, specs,
                             sizeof(specs) / sizeof(specs[0]));
    }
    return status;
}

/* The tank, the counters, the note line and the link. */
static bento_status_t add_footer(screen_t *screen)
{
    static const widget_spec_t specs[] = {
        {UI_WIDGET_LABEL, SLOT_NONE, "ถังพัก (VR4)", 12, 306, AUTO, AUTO,
         COL_DIM, 0, 0, 20},
        {UI_WIDGET_BAR, SLOT_TANK_BAR, NULL, 120, 312, 162, 16, 0U, 0,
         0, 0},
        {UI_WIDGET_LABEL, SLOT_TANK, "-- %", 292, 304, AUTO, AUTO,
         COL_TEXT, 0, 0, 16},
        {UI_WIDGET_LABEL, SLOT_STATS, "ดินแห้งสะสม 0 นาที  รดไป 0 วิ", 370,
         304, AUTO, AUTO, COL_LEAF, 0, 0, 20},
        {UI_WIDGET_LABEL, SLOT_NOTE, "ยังไม่มีคำสั่ง", 12, 352, AUTO, AUTO,
         COL_DIM, 0, 0, 20},
        {UI_WIDGET_LED, SLOT_LINK_LED, NULL, 596, 12, 18, 18, COL_LEAF,
         0, 0, 0},
        {UI_WIDGET_LABEL, SLOT_LINK_TEXT, "MQTT: ออฟไลน์", 622, 10, AUTO,
         AUTO, COL_DIM, 0, 0, 20},
    };

    return add_widgets(screen, specs, sizeof(specs) / sizeof(specs[0]));
}

/* The whole screen in the .py's order. The ring's colour is set once
 * everything is drawn: the same look. */
static bento_status_t build_screen(screen_t *screen)
{
    bento_status_t status =
        bento_ui_screen(screen->ui, BENTO_RUNTIME_SCREEN_WIDTH_PX,
                        BENTO_RUNTIME_SCREEN_HEIGHT_PX);

    if (status == BENTO_OK) {
        status = rest(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_soil_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_valve_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_footer(screen);
    }
    if (status == BENTO_OK) {
        show_color(screen, SLOT_ARC, COL_RAIN);
        drain_events(screen->ui);
    }
    return status;
}

static void show_pump(const screen_t *screen, int32_t left_s)
{
    char line[TEXT_BYTES];

    show_value(screen, SLOT_LED, (left_s != 0) ? 1 : 0);
    show_color(screen, SLOT_PUMP, (left_s != 0) ? COL_RAIN : COL_DIM);
    if (left_s != 0) {
        (void)snprintf(line, sizeof(line), "รดอยู่ อีก %" PRId32 " วิ",
                       left_s);
        show_text(screen, SLOT_PUMP, line);
    } else {
        show_text(screen, SLOT_PUMP, "ปิด");
    }
    show_value(screen, SLOT_ARC, left_s);
}

static void show_knobs(const screen_t *screen, int32_t soil,
                       int32_t tank)
{
    char line[TEXT_BYTES];
    const bool dry = soil < SOIL_DRY;

    show_color(screen, SLOT_SOIL, dry ? COL_DRY : COL_TEXT);
    (void)snprintf(line, sizeof(line), "ดิน %" PRId32 " %%%s", soil,
                   dry ? " แห้ง" : "");
    show_text(screen, SLOT_SOIL, line);
    show_value(screen, SLOT_TANK_BAR, tank);
    show_color(screen, SLOT_TANK_BAR,
               (tank >= TANK_MIN) ? COL_RAIN : COL_BAD);
    (void)snprintf(line, sizeof(line), "%" PRId32 " %%", tank);
    show_text(screen, SLOT_TANK, line);
}

static void show_stats(const screen_t *screen, int32_t dry_min,
                       int32_t water_s)
{
    char line[TEXT_BYTES];

    (void)snprintf(line, sizeof(line),
                   "ดินแห้งสะสม %" PRId32 " นาที  รดไป %" PRId32 " วิ",
                   dry_min, water_s);
    show_text(screen, SLOT_STATS, line);
}

/* ---- 6) The program ---- */

/* The .py's stop(): the valve shuts first, then the screen says why
 * and the program ends politely. */
static void stop_here(const screen_t *screen, valve_t *valve,
                      const char *text, uint32_t color)
{
    show_link(screen, false);
    /* the program ends either way: the LED and matrix are tidied */
    (void)set_led(valve, false);
    (void)bento_rgbmatrix_scroll(NULL, 0U,
                                 BENTO_RGBMATRIX_SCROLL_COLOR_DEFAULT,
                                 BENTO_RGBMATRIX_SCROLL_MS_DEFAULT);
    (void)bento_rgbmatrix_clear();
    show_note(screen, text, color);
    drain_events(screen->ui);
    valve->stopped = true;
}

/* json.dumps of a report; false only when the link is gone. */
static bool send_report(const valve_t *valve, int32_t soil,
                        int32_t tank, int32_t dry_min, int32_t water_s)
{
    const int32_t pump_on = (valve->pump_ms != 0) ? 1 : 0;
    char json[JSON_BYTES];
    bento_status_t status;

    (void)snprintf(
        json, sizeof(json),
        "{\"id\": \"%s\", \"n\": %" PRId32 ", \"soil\": %" PRId32
        ", \"tank\": %" PRId32 ", \"pump\": %" PRId32
        ", \"sim\": \"soil tank\", \"dry_min\": %" PRId32
        ", \"water_s\": %" PRId32 "}",
        TEAM, valve->sent, soil, tank, pump_on, dry_min, water_s);
    status = bento_mqtt_publish(
        valve->broker, valve->topic, (uint32_t)strlen(valve->topic),
        (const uint8_t *)json, (uint32_t)strlen(json), 0U, 0U);
    return (status != BENTO_E_SESSION) && (status != BENTO_E_STALE);
}

/* Does what one command decided: the valve, the tunes, the note. */
static bento_status_t act_on(const screen_t *screen, valve_t *valve,
                             const decision_t *decision,
                             uint32_t now_ms)
{
    static const int32_t tunes[] = {-1,         TUNE_BAD, TUNE_STOP,
                                    TUNE_START, -1,       TUNE_TAP};
    const action_t action = decision->action;
    bento_status_t status = BENTO_OK;

    if (action == ACTION_OFF) {
        valve->pump_ms = 0;
    } else if (action == ACTION_ON) {
        valve->pump_ms = decision->seconds * MS_PER_S;
        valve->pump_start_ms = now_ms;
    } else if (action == ACTION_BEEP) {
        status = beep(screen->ui, TUNE_HIT);
    } else if ((action == ACTION_SAY) && (decision->say[0] != '\0') &&
               (valve->pump_ms == 0)) {
        status = matrix_done(bento_rgbmatrix_scroll(
            decision->say, (uint32_t)strlen(decision->say),
            BENTO_RGBMATRIX_PURPLE, MATRIX_MS));
    } else {
        /* nothing to do but the note */
    }
    if ((status == BENTO_OK) && (tunes[action] >= 0)) {
        status = beep(screen->ui, tunes[action]);
    }
    if ((status == BENTO_OK) && (decision->note[0] != '\0')) {
        show_note(screen, decision->note, decision->color);
    }
    return status;
}

/* get_message(): the newest message waiting, decided and done. */
static bento_status_t take_command(const screen_t *screen,
                                   valve_t *valve, int32_t tank,
                                   uint32_t now_ms)
{
    uint8_t payload[PAYLOAD_BYTES];
    char topic[TOPIC_KEEP];
    char line[TEXT_BYTES];
    decision_t decision;
    bento_mqtt_msg_t info;
    uint32_t length = 0U;
    bool found = false;
    bool more = true;
    bento_status_t status = BENTO_OK;

    while (more) {
        bento_mqtt_msg_init(&info);
        more =
            bento_mqtt_poll(valve->broker, &info, topic, sizeof(topic),
                            payload, PAYLOAD_BYTES - 1U) == BENTO_OK;
        if (more) {
            length = info.payload_len;
            found = true;
        }
    }
    if (found) {
        valve->got++;
        (void)snprintf(line, sizeof(line),
                       "วาล์วแปลง 0 (คำสั่ง %" PRId32 ")", valve->got);
        show_text(screen, SLOT_TITLE, line);
        handle_command(payload, length, tank, &decision);
        status = act_on(screen, valve, &decision, now_ms);
    }
    return status;
}

/* SW5 needs no network: it shuts an open valve at once. */
static bento_status_t check_stop_button(const screen_t *screen,
                                        valve_t *valve)
{
    bento_status_t status = BENTO_OK;

    if (stop_pressed_now(valve) && (valve->pump_ms != 0)) {
        valve->pump_ms = 0;
        show_note(screen, "ปิดวาล์วฉุกเฉิน SW5", COL_BAD);
        status = beep(screen->ui, TUNE_STOP);
    }
    return status;
}

/* The valve shuts itself when its time is up or the tank runs dry;
 * returns the milliseconds left, 0 when shut. */
static bento_status_t close_by_itself(const screen_t *screen,
                                      valve_t *valve, int32_t tank,
                                      uint32_t now_ms, int32_t *left_ms)
{
    bento_status_t status = BENTO_OK;

    *left_ms = 0;
    if (valve->pump_ms != 0) {
        *left_ms = valve->pump_ms -
                   bento_time_ticks_diff(now_ms, valve->pump_start_ms);
    }
    if ((valve->pump_ms != 0) &&
        ((*left_ms <= 0) || (tank < TANK_MIN))) {
        valve->pump_ms = 0;
        show_note(screen,
                  (*left_ms <= 0) ? "ครบเวลา ปิดวาล์วเอง"
                                  : "ถังพักแห้ง ปิดวาล์วเอง",
                  COL_RAIN);
        status = beep(screen->ui, TUNE_STOP);
    }
    if (status == BENTO_OK) {
        status = set_led(valve, valve->pump_ms != 0);
    }
    return status;
}

/* The counters, the countdown, the knobs and the chart. */
static bento_status_t show_round(const screen_t *screen, valve_t *valve,
                                 int32_t soil, int32_t tank,
                                 int32_t left_ms)
{
    const int32_t left_s = (valve->pump_ms != 0)
                               ? ((left_ms + ROUND_UP_MS) / MS_PER_S)
                               : 0;
    bento_status_t status = BENTO_OK;

    if (left_s != valve->shown_s) {
        status = matrix_countdown(left_s, valve->shown_s);
        valve->shown_s = left_s;
        show_pump(screen, left_s);
    }
    show_knobs(screen, soil, tank);
    return status;
}

/* A chart point every 2 s, the totals when they change. */
static void show_history(const screen_t *screen, valve_t *valve,
                         int32_t soil, uint32_t now_ms)
{
    const int32_t dry_min = valve->dry_ms / MS_PER_MIN;
    const int32_t water_s = valve->water_ms / MS_PER_S;

    if (bento_time_ticks_diff(now_ms, valve->chart_ms) >= CHART_MS) {
        valve->chart_ms = now_ms;
        /* set_next() is fire-and-forget in MicroPython too */
        (void)bento_ui_chart_set_next(
            screen->ui, screen->ids[SLOT_CHART], 0U, soil);
        (void)bento_ui_chart_set_next(screen->ui,
                                      screen->ids[SLOT_CHART],
                                      screen->dry_series, SOIL_DRY);
    }
    if (!valve->stats_shown || (dry_min != valve->shown_dry_min) ||
        (water_s != valve->shown_water_s)) { /* only when they change */
        valve->stats_shown = true;
        valve->shown_dry_min = dry_min;
        valve->shown_water_s = water_s;
        show_stats(screen, dry_min, water_s);
    }
}

/* A report every 5 s; a dropped link ends the program. */
static void report(const screen_t *screen, valve_t *valve, int32_t soil,
                   int32_t tank, uint32_t now_ms)
{
    uint32_t state = BENTO_MQTT_STATE_CLOSED;

    if (bento_time_ticks_diff(now_ms, valve->send_ms) >= SEND_MS) {
        valve->send_ms = now_ms;
        valve->sent++;
        if (!send_report(valve, soil, tank, valve->dry_ms / MS_PER_MIN,
                         valve->water_ms / MS_PER_S)) {
            stop_here(screen, valve, "สายหลุด", COL_BAD);
        }
    }
    if (bento_mqtt_state(valve->broker, &state) != BENTO_OK) {
        state = BENTO_MQTT_STATE_CLOSED; /* unknown counts as dropped */
    }
    if (!valve->stopped && (state != BENTO_MQTT_STATE_CONNECTED)) {
        stop_here(screen, valve, "สายหลุด", COL_BAD);
    }
}

/* One round, POLL_MS long. */
static bento_status_t listen_once(const screen_t *screen,
                                  valve_t *valve)
{
    const uint32_t now_ms = bento_time_ticks_ms();
    const int32_t step_ms =
        bento_time_ticks_diff(now_ms, valve->last_ms);
    int32_t soil = 0;
    int32_t tank = 0;
    int32_t left_ms = 0;
    bento_status_t status = check_stop_button(screen, valve);

    valve->last_ms = now_ms;
    if (status == BENTO_OK) {
        status = knob_percent(SOIL_POT, &soil);
    }
    if (status == BENTO_OK) {
        status = knob_percent(TANK_POT, &tank);
    }
    if (status == BENTO_OK) {
        status = take_command(screen, valve, tank, now_ms);
    }
    if (status == BENTO_OK) {
        status = close_by_itself(screen, valve, tank, now_ms, &left_ms);
    }
    if (status == BENTO_OK) {
        valve->dry_ms += (soil < SOIL_DRY) ? step_ms : 0;
        valve->water_ms += (valve->pump_ms != 0) ? step_ms : 0;
        status = show_round(screen, valve, soil, tank, left_ms);
    }
    if (status == BENTO_OK) {
        show_history(screen, valve, soil, now_ms);
        report(screen, valve, soil, tank, now_ms);
    }
    if ((status == BENTO_OK) && !valve->stopped) {
        drain_events(screen->ui);
        status = wait_sampling(valve, POLL_MS);
    }
    return status;
}

static bento_status_t listen(const screen_t *screen, valve_t *valve)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    char line[TEXT_BYTES];
    bento_status_t status = BENTO_OK;

    valve->send_ms = start_ms;
    valve->chart_ms = start_ms;
    valve->last_ms = start_ms;
    valve->shown_s = -1;
    while ((status == BENTO_OK) && !valve->stopped &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            LISTEN_MS)) {
        status = listen_once(screen, valve);
    }
    if ((status == BENTO_OK) && !valve->stopped) {
        (void)snprintf(line, sizeof(line),
                       "เลิกฟัง ได้รับ %" PRId32 " คำสั่ง", valve->got);
        stop_here(screen, valve, line, COL_DIM);
    }
    return status;
}

/* The BENTO Emulator has no MQTT from C yet: say so on the screen and
 * in the console; the caller then takes the .py's broker-down path. */
static bento_status_t show_emulator_gap(bento_ui_t *ui)
{
    static const char gap[] =
        "MQTT จาก C ยังไม่มีใน BENTO Emulator ให้รันบน "
        "Dev Kit หรือ Eva Kit";
    bento_ui_widget_args_t args;
    bento_ui_id_t label;

    (void)printf(
        "%s / MQTT from C is not in the BENTO Emulator yet: run it "
        "on a Dev Kit or Eva Kit.\n",
        gap);
    bento_ui_widget_args_init(&args);
    args.text = gap;
    args.text_len = (uint32_t)strlen(gap);
    args.x = GAP_X;
    args.y = GAP_Y;
    args.w = GAP_W;
    args.color = COL_DRY;
    args.font_px = GAP_PX;
    return bento_ui_label_create(ui, &args, &label);
}

/* The public broker is down now and then: 3 tries. */
static bento_status_t connect_broker(const screen_t *screen,
                                     valve_t *valve, bool *linked)
{
    char line[TEXT_BYTES];
    int32_t attempt;
    bento_status_t status = BENTO_OK;

    *linked = false;
    for (attempt = 1;
         (attempt <= BROKER_TRIES) && (status == BENTO_OK) &&
         !*linked && !valve->no_mqtt;
         attempt++) {
        if (attempt > 1) {
            (void)snprintf(line, sizeof(line),
                           "ลองต่อ broker ใหม่ %" PRId32 "/3", attempt);
            show_note(screen, line, COL_DRY);
            drain_events(screen->ui);
        }
        status = try_broker(valve, linked);
    }
    return status;
}

/* Wi-Fi, its address, the broker and the subscription; *problem
 * names the step that failed, NULL when all are up. */
static bento_status_t connect_farm(const screen_t *screen,
                                   valve_t *valve, const char **problem)
{
    uint8_t granted_qos = 0U;
    bool joined = false;
    bool linked = false;
    bento_status_t status;

    *problem = NULL;
    show_note(screen, "ต่อ WiFi... จอนิ่งได้", COL_DRY);
    drain_events(screen->ui); /* the label shows before the wait */
    status = join_wifi(&joined);
    if ((status == BENTO_OK) && !joined) {
        *problem = "ต่อ WiFi ไม่ได้";
    }
    if ((status == BENTO_OK) && joined) {
        status = connect_broker(screen, valve, &linked);
    }
    if ((status == BENTO_OK) && valve->no_mqtt) {
        status = show_emulator_gap(screen->ui);
    }
    /* subscribe always after connect */
    if ((status == BENTO_OK) && linked) {
        linked =
            bento_mqtt_subscribe(valve->broker, valve->topic_cmd,
                                 (uint32_t)strlen(valve->topic_cmd), 0U,
                                 &granted_qos) == BENTO_OK;
    }
    if ((status == BENTO_OK) && joined && !linked) {
        *problem = "broker ไม่ตอบ: รอ 1 นาทีแล้วรันใหม่";
    }
    return status;
}

/* "team" and two digits. */
static bool valid_team(const char *team)
{
    const uint32_t length = (uint32_t)strlen(team);
    bool valid = (length == (uint32_t)TEAM_LENGTH) &&
                 (strncmp(team, "team", TEAM_PREFIX_LENGTH) == 0);
    uint32_t i;

    for (i = TEAM_PREFIX_LENGTH; valid && (i < length); i++) {
        valid = is_digit((uint8_t)team[i]);
    }
    return valid;
}

/* From the team check to the end of listening. */
static bento_status_t run(const screen_t *screen, valve_t *valve)
{
    const char *problem = NULL;
    char line[TEXT_BYTES];
    bento_status_t status;

    find_led(valve, "RGB_BLUE");
    status = matrix_done(bento_rgbmatrix_clear());
    if ((status == BENTO_OK) && !valid_team(TEAM)) {
        stop_here(screen, valve, "แก้ TEAM เป็นเลขกลุ่มก่อน", COL_BAD);
    }
    if ((status == BENTO_OK) && !valve->stopped) {
        status = connect_farm(screen, valve, &problem);
    }
    if ((status == BENTO_OK) && (problem != NULL)) {
        stop_here(screen, valve, problem, COL_BAD);
    }
    if ((status == BENTO_OK) && !valve->stopped) {
        show_link(screen, true);
        (void)snprintf(line, sizeof(line), "ฟัง %s", valve->topic_cmd);
        show_note(screen, line, COL_LEAF);
        status = listen(screen, valve);
    }
    return status;
}

int main(void)
{
    static const char root[] = "bento-aiot";
    screen_t screen;
    valve_t valve;
    bento_status_t status;

    (void)memset(&screen, 0, sizeof(screen));
    (void)memset(&valve, 0, sizeof(valve));
    screen.ui = bento_runtime_get_ui();
    (void)snprintf(valve.topic_cmd, sizeof(valve.topic_cmd),
                   "%s/%s/cmd", root, TEAM);
    (void)snprintf(valve.topic, sizeof(valve.topic), "%s/%s/telemetry",
                   root, TEAM);
    /* a build without sound refuses it, as hasattr() skips it */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    status = build_screen(&screen);
    if (status == BENTO_OK) {
        status = run(&screen, &valve);
    }
    if (valve.broker != 0U) {
        /* the .py ignores every error of its disconnect */
        (void)bento_mqtt_close(valve.broker);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
