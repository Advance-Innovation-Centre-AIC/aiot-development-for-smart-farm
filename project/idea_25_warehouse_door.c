/*
 * idea_25_warehouse_door.c -- ประตูโกดังกันขโมย: เรดาร์จับคนที่ประตู
 * แจ้งมือถือ (ขั้นสูง)
 *
 * ไฟล์นี้เป็นจุดเริ่มต้นของโครงงาน ต่อยอดจาก s3/sf3_01_pen_guard.py กับ
 * WiFi/MQTT, send() และ ack แบบ s2/sf2_04_crop_alert.py จอมีบานประตูที่
 * เปลี่ยนสีตามเขต สองเขต (หน้าโกดังและประตู) และนับถอยหลังก่อนเฝ้า
 * ตอนเริ่มให้ถอยห่างบอร์ด (บอร์ดจำฉากนิ่ง) รอนับถอยหลัง EXIT_S วินาที
 * แล้วเดินเข้าหา VR3 = ระยะเขตประตู 30-150 cm (หน้าโกดัง = 2 เท่า)
 * SW5 หรือสวิตช์บนจอ = เปิด/ปิดเฝ้า SW6 = รับทราบ คนถึงประตู = ไซเรน
 * ซ้ำทุก 5 วินาทีจนมีคนรับทราบ (SW6 หรือ {"cmd":"ack"} จากแอป) บอร์ดส่ง
 * event ทาง MQTT เฉพาะตอนเกิด (armed intruder clear ack) ต่อเน็ตไม่ได้ก็
 * เฝ้าออฟไลน์ต่อ รอบละ 10 นาที ก่อนรันแก้ TEAM ที่หัวไฟล์ และชื่อกับรหัส
 * Hotspot (WIFI_SSID, WIFI_PASS ของ .py) ใน join_wifi()
 * เกณฑ์เป็นค่าสาธิต ไม่ใช่ระบบรักษาความปลอดภัยจริง
 *
 * The warehouse door guard. A radar watches the door and tells a
 * phone. Step back at the start (the board learns the still scene),
 * wait for the EXIT_S countdown, then walk in. VR3 sets the door
 * zone (30-150 cm; the yard in front is twice that). SW5 or the
 * on-screen switch arms and disarms; SW6 acknowledges. Someone at the
 * door sounds a siren every 5 s until it is acknowledged (SW6, or
 * {"cmd":"ack"} from the app). Events go out over MQTT only when
 * they happen (armed, intruder, clear, ack); without the network
 * the guard keeps watching offline. One round lasts 10 minutes.
 * Dev Kit; in the Emulator VR1 is the distance and Shake is motion.
 * Edit TEAM at the top, and the hotspot name and password (the
 * .py's WIFI_SSID, WIFI_PASS) in join_wifi(), before running.
 *
 * Twin of idea_25_warehouse_door.py (aiot-development-for-smart-farm/project/idea_25_warehouse_door.py). Style: CLEAN_C.md.
 * Where C differs:
 *   - THRESH_DB is THRESH_DB_TENTHS (4.0 dB = 40): an enum holds no
 *     real number, and the radar takes tenths of a dB anyway.
 *   - json.loads in is_acknowledge(): a small reader of one JSON object
 *     finds "cmd" at its top level and compares its text with "ack";
 *     a nested list or object is skipped by its brackets; text that
 *     is not one JSON object is not an ack, as the .py's except.
 *   - json.dumps: built with snprintf, the keys in the order the .py
 *     writes them (MicroPython writes its dict's slot order).
 *   - get_message() is one slot a newer message overwrites; the C
 *     client keeps a ring of BENTO_MQTT_RING_SLOTS, and each round
 *     takes them all and keeps the newest.
 *   - try/except OSError around the radar, the broker and the RGB
 *     matrix: the call's status is checked and the same fallback is
 *     taken (a publish on a lost link is the original's OSError).
 *   - The Guard class, the LOOK dict and the widget dict are structs
 *     and tables; None (no target, no exit countdown, nothing shown
 *     yet) is a bool beside the value.
 *   - hasattr(ui, "volume"): the volume is always set, and a build
 *     without sound refuses it, which is ignored; ui.tone is fire
 *     and forget in modui.c, so its status is ignored here too.
 *   - mqtt.disconnect() is the close at the end of main, which also
 *     runs when Stop comes during the countdown.
 *   - wifi.* and mqtt.* print console lines of their own in
 *     MicroPython; the C library prints none.
 *   - Stop ends the program at the next sleep (MicroPython's
 *     KeyboardInterrupt); the finally's finish() still runs.
 */
#include <inttypes.h>
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

/* ---- 1) ตั้งค่า (แก้ได้) ---- */
/* WIFI_SSID, WIFI_PASS ของ .py = wifi_ssid, wifi_pass ใน join_wifi()
 * BROKER = broker_host ใน open_broker() · หัวข้อ MQTT = topic_root ใน
 * main() (MISRA 8.9: ค่าที่ฟังก์ชันเดียวอ่านอยู่ในฟังก์ชันนั้น) */
static const char TEAM[] = "teamXX";

/* Settings: change them and run again. */
enum {
    BEEP_MS = 5000, /* the siren again this often until acknowledged */
    MIN_CM = 30,    /* VR3: the door zone 30-150 cm, the yard twice */
    SPAN_CM = 120,
    CONFIRM_N = 3,         /* rounds in a row before we believe */
    THRESH_DB_TENTHS = 40, /* 4.0 dB above the still scene */
    MAX_CM = 300,          /* far end of the bar (cm) */
    EXIT_S = 15,           /* seconds for the workers to leave */
    VOLUME = 25,
    SPEAKER = 40,
    RUN_MS = 600000, /* 10 minutes a round */
    SAMPLE_MS = 250, /* read the radar this often */
    TICK_MS = 500    /* redraw this often */
};

/* สีโรงงาน, 0xRRGGBB */
enum {
    COL_TEXT = 0xF2F2F2,
    COL_DIM = 0x9AA0A8,
    COL_CARD = 0x1C2026,
    COL_STEEL = 0x3A4048,
    COL_HAZ = 0xFFC300,
    COL_OK = 0x34C759,
    COL_BAD = 0xFF3B30,
    COL_INFO = 0x5AC8FA
};

/* The board, the sounds and the network. */
enum {
    ARM_BUTTON = 1,         /* SW5 is buttons.pressed(1) */
    ACKNOWLEDGE_BUTTON = 0, /* SW6 is buttons.pressed(0) */
    BUTTON_COUNT = 2,
    ZONE_POT = 2, /* VR3 */
    POT_FULL = 4095,
    CM_PER_M = 100,
    TENTHS_PER_DB = 10,
    TONE_MS = 100,
    TONE_GAP_MS = 110,
    NOTE_TAP = 76,  /* the arm switch and an acknowledgement */
    NOTE_YARD = 84, /* someone in the yard */
    SIREN_TONE_MS = 110,
    SIREN_STEP_MS = 120,
    SCENE_SETTLE_MS = 500,
    SETTLE_MS = 200, /* let the cleared screen settle */
    COUNTDOWN_S = 3,
    MS_PER_S = 1000,
    BUTTON_POLL_MS = 20,
    HISTORY_LEN = 5, /* median of the last 5 distances */
    SCROLL_MS = 60,
    RULER_TICKS = 7,
    BROKER_PORT = 1883,
    KEEPALIVE_S = 60,
    BROKER_TRIES = 3,
    CONNECT_POLL_MS = 20,
    CLIENT_SUFFIX_MASK = 0xFFFF,
    TEAM_LEN = 6, /* "team" and two digits */
    TEAM_PREFIX_LEN = 4
};

/* Buffer sizes, in bytes. */
enum {
    TEXT_BYTES = 128,     /* a label text: 126 at most */
    NAME_BYTES = 48,      /* a topic */
    CLIENT_ID_BYTES = 32, /* the broker takes 23 */
    PAYLOAD_KEEP = 255,   /* what the MicroPython binding keeps */
    WORD_BYTES = 8,       /* "ack", and room to see a longer word */
    NUMBER_BYTES = 12,    /* any int32_t and its NUL */
    /* an event with every int32_t at its widest */
    EVENT_BYTES = 128
};

/* What the door shows: the .py's LOOK keys. */
typedef enum {
    MODE_THIEF = 0,
    MODE_NEAR,
    MODE_WAIT,
    MODE_WATCH,
    MODE_OFF,
    MODE_COUNT
} door_mode_t;

/* One board button: a press is remembered until it is asked for. */
typedef struct {
    uint32_t index;
    bool down;
    bool clicked;
} board_button_t;

/* One radar reading: the distance, when there is a target. */
typedef struct {
    bool has_target;
    int32_t cm;
} radar_reading_t;

/* What the guard believes and how many rounds the new view held. */
typedef struct {
    bool inside;
    int32_t streak;
} belief_t;

/* The widgets the program changes after drawing them. */
typedef struct {
    bento_ui_id_t link;
    bento_ui_id_t door;
    bento_ui_id_t door_text;
    bento_ui_id_t cm;
    bento_ui_id_t bar;
    bento_ui_id_t zones;
    bento_ui_id_t state;
    bento_ui_id_t big_title;
    bento_ui_id_t big;
    bento_ui_id_t toggle;
    bento_ui_id_t arm;
    bento_ui_id_t count;
    bento_ui_id_t last;
    bento_ui_id_t status;
} screen_t;

/* The guard's memory: the .py's Guard. */
typedef struct {
    bool armed;
    bool inside;
    int32_t streak;
    int32_t count;
    bool near;
    int32_t near_streak;
    int32_t loiter;
    int32_t history[HISTORY_LEN];
    uint32_t history_len;
    bool radar_ok;
    bool acknowledged;
    bool online;
    uint32_t beep_ms;
    bool has_shown; /* false = shown None */
    door_mode_t shown_mode;
    int32_t shown_number;
    bool exit_running; /* false = t_arm None */
    uint32_t arm_ms;
} guard_t;

/* Everything one run of the program keeps. */
typedef struct {
    bento_ui_t *ui;
    screen_t screen;
    guard_t guard;
    board_button_t buttons[BUTTON_COUNT];
    char topic_event[NAME_BYTES];
    char topic_cmd[NAME_BYTES];
    uint32_t broker;
    bool has_broker;
} door_app_t;

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

/* The one key the program reads from a command. */
enum {
    FIELD_CMD = 0,
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

/* ---- 2) ฮาร์ดแวร์ ---- */
/* Sleeps, and turns Stop into BENTO_E_CANCELLED. */
static bento_status_t pause_ms(uint32_t duration_ms)
{
    bento_status_t status = bento_time_sleep_ms(duration_ms);

    if ((status == BENTO_OK) && !bento_runtime_is_running()) {
        status = BENTO_E_CANCELLED;
    }
    return status;
}

/* Soft MIDI notes, 110 ms apart. ui.tone() is sent and forgotten,
 * so its status is not read. */
static bento_status_t beep(bento_ui_t *ui, const uint8_t *notes,
                           uint32_t count)
{
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < count) && (status == BENTO_OK); i++) {
        (void)bento_ui_sfx_tone(ui, notes[i],
                                (uint8_t)BENTO_UI_WAVE_SINE,
                                (uint8_t)VOLUME, (uint16_t)TONE_MS);
        status = pause_ms(TONE_GAP_MS);
    }
    return status;
}

/* One note: the arm switch and the acknowledgement. */
static bento_status_t beep_once(bento_ui_t *ui, uint8_t note)
{
    return beep(ui, &note, 1U);
}

/* The door zone from VR3, in cm. */
static bento_status_t zone_cm(int32_t *door_cm)
{
    uint16_t raw = 0U;
    const bento_status_t status = bento_pots_read(ZONE_POT, &raw);

    *door_cm = (int32_t)MIN_CM +
               (((int32_t)raw * (int32_t)SPAN_CM) / (int32_t)POT_FULL);
    return status;
}

/* One radar reading; false when the radar does not answer (that
 * round is skipped). */
static bool read_radar(radar_reading_t *reading)
{
    bento_sensors_radar_status_t radar;
    bento_sensors_radar_range_t range;
    bool answered;

    bento_sensors_radar_status_init(&radar);
    bento_sensors_radar_range_init(&range);
    answered = (bento_sensors_radar_status(&radar) == BENTO_OK) &&
               (bento_sensors_radar_range(&range) == BENTO_OK);
    reading->has_target = answered && (range.target != 0U);
    reading->cm = 0;
    if (reading->has_target) {
        const bento_real_t cm =
            (bento_real_t)range.distance_m * (bento_real_t)CM_PER_M;

        reading->cm = (int32_t)cm;
    }
    return answered;
}

/* 0 learns the still scene again, then the threshold. *learned
 * false: the radar did not answer. */
static bento_status_t calibrate_radar(bool *learned)
{
    const float threshold_db =
        (float)THRESH_DB_TENTHS / (float)TENTHS_PER_DB;
    bento_status_t status = BENTO_OK;

    *learned = bento_sensors_radar_config(0.0F) == BENTO_OK;
    if (*learned) {
        status = pause_ms(SCENE_SETTLE_MS);
    }
    if ((status == BENTO_OK) && *learned) {
        *learned = bento_sensors_radar_config(threshold_db) == BENTO_OK;
    }
    return status;
}

/* Reads a board button and remembers a new press (a rising edge). */
static bento_status_t sample_button(board_button_t *button)
{
    bool pressed = false;
    const bento_status_t status =
        bento_buttons_pressed(button->index, &pressed);

    if ((status == BENTO_OK) && pressed && !button->down) {
        button->clicked = true;
    }
    button->down = pressed;
    return status;
}

/* True once per press: asking clears it. */
static bool button_clicked(board_button_t *button)
{
    const bool fired = button->clicked;

    button->clicked = false;
    return fired;
}

static bento_status_t sample_buttons(board_button_t *buttons)
{
    bento_status_t status = sample_button(&buttons[0]);

    if (status == BENTO_OK) {
        status = sample_button(&buttons[1]);
    }
    return status;
}

/* Waits duration_ms, reading both buttons every 20 ms so a short
 * tap is not missed. */
static bento_status_t wait_ms(uint32_t duration_ms,
                              board_button_t *buttons)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;
    bool waiting = true;

    while (waiting) {
        int32_t left_ms;

        status = sample_buttons(buttons);
        left_ms =
            (int32_t)duration_ms -
            bento_time_ticks_diff(bento_time_ticks_ms(), start_ms);
        waiting = (status == BENTO_OK) && (left_ms > 0);
        if (waiting) {
            status = pause_ms((left_ms < BUTTON_POLL_MS)
                                  ? (uint32_t)left_ms
                                  : (uint32_t)BUTTON_POLL_MS);
            waiting = status == BENTO_OK;
        }
    }
    return status;
}

/* Called only when the picture changes. A refused call ends that
 * picture, as the .py's except OSError does. */
static void show_matrix(door_mode_t mode, int32_t number)
{
    static const char thief[] = "THIEF";
    static const char leave[] = "EXIT NOW";
    bento_status_t status = bento_rgbmatrix_scroll(
        "", 0U, BENTO_RGBMATRIX_WHITE, SCROLL_MS);

    if (status == BENTO_OK) {
        status = bento_rgbmatrix_clear();
    }
    if ((status == BENTO_OK) && (mode == MODE_THIEF)) {
        (void)bento_rgbmatrix_scroll(thief, (uint32_t)strlen(thief),
                                     BENTO_RGBMATRIX_RED, SCROLL_MS);
    } else if ((status == BENTO_OK) && (mode == MODE_WAIT)) {
        (void)bento_rgbmatrix_scroll(leave, (uint32_t)strlen(leave),
                                     BENTO_RGBMATRIX_YELLOW, SCROLL_MS);
    } else if ((status == BENTO_OK) && (mode == MODE_WATCH)) {
        (void)bento_rgbmatrix_score(number, BENTO_RGBMATRIX_GREEN);
    } else {
        /* off, near, or the matrix refused: the picture stays dark */
    }
}

/* MIDI note numbers, not hertz. */
static bento_status_t siren(bento_ui_t *ui)
{
    static const uint8_t notes[] = {88U, 76U, 88U, 76U, 88U};
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < (uint32_t)sizeof(notes)) && (status == BENTO_OK);
         i++) {
        (void)bento_ui_sfx_tone(
            ui, notes[i], (uint8_t)BENTO_UI_WAVE_SQUARE,
            (uint8_t)VOLUME, (uint16_t)SIREN_TONE_MS);
        status = pause_ms(SIREN_STEP_MS);
    }
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
    static const char *const names[FIELD_COUNT] = {"cmd"};
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

/* ---- 3) สมอง: ข้อความในคำสั่ง ---- */
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

/* {"cmd": "ack"}: one JSON object whose "cmd" is the text "ack". */
static bool is_acknowledge(const uint8_t *payload, uint32_t len)
{
    const json_value_t *value;
    command_t command;
    char word[WORD_BYTES];
    bool acknowledged = false;

    read_command(payload, len, &command);
    value = &command.fields[FIELD_CMD];
    if (command.is_object && (value->kind == JSON_STRING)) {
        const uint32_t length =
            decode_string(&command, value, word, sizeof(word));

        acknowledged =
            (length < sizeof(word)) && (strcmp(word, "ack") == 0);
    }
    return acknowledged;
}

/* ---- 3) สมอง (ตัดสินใจ) ---- */
/* The median of the last 5 distances. */
static int32_t smooth(guard_t *guard, int32_t cm)
{
    int32_t sorted[HISTORY_LEN];
    uint32_t i;
    uint32_t j;

    if (guard->history_len == (uint32_t)HISTORY_LEN) {
        for (i = 1U; i < (uint32_t)HISTORY_LEN; i++) {
            guard->history[i - 1U] = guard->history[i];
        }
        guard->history_len--;
    }
    guard->history[guard->history_len] = cm;
    guard->history_len++;
    for (i = 0U; i < guard->history_len; i++) {
        const int32_t value = guard->history[i];

        j = i;
        while ((j > 0U) && (sorted[j - 1U] > value)) {
            sorted[j] = sorted[j - 1U];
            j--;
        }
        sorted[j] = value;
    }
    return sorted[guard->history_len / 2U];
}

static bool in_zone(bool watching, const radar_reading_t *reading,
                    int32_t cm, int32_t alert_cm)
{
    return watching && reading->has_target && (cm < alert_cm);
}

static belief_t belief(bool inside, int32_t streak)
{
    belief_t result;

    result.inside = inside;
    result.streak = streak;
    return result;
}

/* A new state must hold CONFIRM_N rounds in a row. */
static belief_t confirm(bool inside, int32_t streak, bool near)
{
    belief_t result;

    if (near == inside) {
        result = belief(inside, 0);
    } else if ((streak + 1) >= CONFIRM_N) {
        result = belief(near, 0);
    } else {
        result = belief(inside, streak + 1);
    }
    return result;
}

/* 0 = watching or disarmed. When the countdown has run out it stays
 * off (the .py's t_arm = None) until the guard is armed again. */
static int32_t exit_left(guard_t *guard, uint32_t now_ms)
{
    int32_t left = 0;

    if (guard->armed && guard->exit_running) {
        left = EXIT_S - (bento_time_ticks_diff(now_ms, guard->arm_ms) /
                         MS_PER_S);
        if (left <= 0) {
            left = 0;
            guard->exit_running = false;
        }
    }
    return left;
}

static door_mode_t guard_mode(guard_t *guard, uint32_t now_ms)
{
    door_mode_t mode = MODE_WATCH;

    if (!guard->armed) {
        mode = MODE_OFF;
    } else if (guard->inside) {
        mode = MODE_THIEF;
    } else if (exit_left(guard, now_ms) != 0) {
        mode = MODE_WAIT;
    } else if (guard->near) {
        mode = MODE_NEAR;
    } else {
        /* armed, nobody near: watching */
    }
    return mode;
}

/* ---- 4) หน้าจอ ---- */
/* A setter's refusal is ignored, as MicroPython's widget setters
 * ignore it (modui.c). */
static void set_text(const door_app_t *app, bento_ui_id_t widget,
                     const char *text)
{
    (void)bento_ui_set_text(app->ui, widget, text,
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

/* A label whose id nobody needs again. */
static bento_status_t add_label(bento_ui_t *ui, const spec_t *spec,
                                const char *text)
{
    bento_ui_id_t unused = 0U;

    return add_widget(ui, bento_ui_label_create, spec, text, &unused);
}

/* The bottom status line. */
static void say(const door_app_t *app, const char *text, uint32_t color)
{
    (void)bento_ui_set_color(app->ui, app->screen.status, color);
    set_text(app, app->screen.status, text);
}

/* why: NULL or "" = connected. */
static void show_link(const door_app_t *app, const char *why)
{
    const bool down = (why != NULL) && (why[0] != '\0');
    char text[TEXT_BYTES];

    (void)bento_ui_set_color(app->ui, app->screen.link,
                             down ? COL_DIM : COL_OK);
    if (down) {
        (void)snprintf(text, sizeof(text), "MQTT: ออฟไลน์ (%s)", why);
    } else {
        (void)snprintf(text, sizeof(text), "MQTT: เชื่อมต่อแล้ว");
    }
    set_text(app, app->screen.link, text);
}

static void show_arm(const door_app_t *app, bool armed)
{
    static const char arm_button_name[] = "SW5"; /* BTN_NAMES[0] */
    char text[TEXT_BYTES];

    (void)snprintf(text, sizeof(text), "%s (%s)",
                   armed ? "เฝ้าอยู่" : "ปิดเฝ้า", arm_button_name);
    set_text(app, app->screen.arm, text);
    (void)bento_ui_set_color(app->ui, app->screen.arm,
                             armed ? COL_OK : COL_DIM);
}

/* Left: the door leaf, its colour following the zone. */
static bento_status_t add_door(door_app_t *app)
{
    const spec_t door = {12, 44, 170, 290, COL_STEEL, COL_HAZ, 6, 3};
    const spec_t door_text = {28, 70, -1, -1, COL_TEXT, 0, 0, 28};
    const spec_t cm = {28, 290, -1, -1, COL_TEXT, 0, 0, 16};
    bento_status_t status = add_widget(app->ui, bento_ui_panel_create,
                                       &door, NULL, &app->screen.door);

    if (status == BENTO_OK) {
        status = add_widget(app->ui, bento_ui_label_create, &door_text,
                            "ปิดล็อก", &app->screen.door_text);
    }
    if (status == BENTO_OK) {
        status = add_widget(app->ui, bento_ui_label_create, &cm,
                            "--- cm", &app->screen.cm);
    }
    return status;
}

/* The bar and the ruler under it, both 0 to MAX_CM. */
static bento_status_t add_ruler(door_app_t *app)
{
    const spec_t bar = {206, 84, 314, 24, 0U, 0, MAX_CM, 0};
    const spec_t ruler = {206, 110, 314, 34, COL_DIM, 0, MAX_CM, 0};
    bento_ui_id_t scale = 0U;
    bento_status_t status = add_widget(app->ui, bento_ui_bar_create,
                                       &bar, NULL, &app->screen.bar);

    if (status == BENTO_OK) {
        status = add_widget(app->ui, bento_ui_scale_create, &ruler,
                            NULL, &scale);
    }
    if (status == BENTO_OK) {
        /* ticks() is a setter: a refusal is ignored there too */
        (void)bento_ui_set_ticks(app->ui, scale, RULER_TICKS, 2);
    }
    return status;
}

/* Middle: the distance from the door, the zones and the state. */
static bento_status_t add_distance(door_app_t *app)
{
    const spec_t card = {194, 44, 338, 190, COL_CARD, COL_STEEL, 4, 1};
    const spec_t title = {206, 50, -1, -1, COL_HAZ, 0, 0, 16};
    const spec_t zones = {206, 154, -1, -1, COL_INFO, 0, 0, 14};
    const spec_t state = {206, 184, -1, -1, COL_DIM, 0, 0, 24};
    bento_ui_id_t panel = 0U;
    bento_status_t status =
        add_widget(app->ui, bento_ui_panel_create, &card, NULL, &panel);

    if (status == BENTO_OK) {
        status = add_label(app->ui, &title, "ระยะจากประตู (เรดาร์)");
    }
    if (status == BENTO_OK) {
        status = add_ruler(app);
    }
    if (status == BENTO_OK) {
        status = add_widget(app->ui, bento_ui_label_create, &zones,
                            "ประตู < - cm  หน้าโกดัง < - cm",
                            &app->screen.zones);
    }
    if (status == BENTO_OK) {
        status = add_widget(app->ui, bento_ui_label_create, &state,
                            "รอเริ่มเฝ้า", &app->screen.state);
    }
    return status;
}

/* Top right, clear of the Console button in the corner: the big
 * number and the arm switch. */
static bento_status_t add_big(door_app_t *app)
{
    const spec_t card = {544, 44, 236, 190, COL_CARD, COL_STEEL, 4, 1};
    const spec_t big_title = {556, 50, -1, -1, COL_HAZ, 0, 0, 16};
    const spec_t big = {556, 80, 150, 64, COL_HAZ, 0, 0, 0};
    const spec_t toggle = {556, 166, 64, 32, 0U, 0, 0, 1};
    const spec_t arm = {630, 172, -1, -1, COL_OK, 0, 0, 14};
    bento_ui_id_t panel = 0U;
    bento_status_t status =
        add_widget(app->ui, bento_ui_panel_create, &card, NULL, &panel);

    if (status == BENTO_OK) {
        status = add_widget(app->ui, bento_ui_label_create, &big_title,
                            "ออกจากโกดังใน (วิ)", &app->screen.big_title);
    }
    if (status == BENTO_OK) {
        status = add_widget(app->ui, bento_ui_seg7_create, &big, "--",
                            &app->screen.big);
    }
    if (status == BENTO_OK) {
        status = add_widget(app->ui, bento_ui_switch_create, &toggle,
                            NULL, &app->screen.toggle);
    }
    if (status == BENTO_OK) {
        status = add_widget(app->ui, bento_ui_label_create, &arm, " ",
                            &app->screen.arm);
    }
    return status;
}

/* Bottom: the counts and the last visitor at the door. */
static bento_status_t add_log(door_app_t *app)
{
    const spec_t card = {194, 244, 490, 90, COL_CARD, COL_STEEL, 4, 1};
    const spec_t count = {206, 256, -1, -1, COL_TEXT, 0, 0, 16};
    const spec_t last = {206, 290, -1, -1, COL_DIM, 0, 0, 16};
    const spec_t status_line = {12, 352, -1, -1, COL_HAZ, 0, 0, 16};
    bento_ui_id_t panel = 0U;
    bento_status_t status =
        add_widget(app->ui, bento_ui_panel_create, &card, NULL, &panel);

    if (status == BENTO_OK) {
        status = add_widget(app->ui, bento_ui_label_create, &count,
                            "เดินผ่านหน้าโกดัง 0  ถึงประตู 0 ครั้ง",
                            &app->screen.count);
    }
    if (status == BENTO_OK) {
        status = add_widget(app->ui, bento_ui_label_create, &last,
                            "ยังไม่มีใครถึงประตู", &app->screen.last);
    }
    if (status == BENTO_OK) {
        status = add_widget(app->ui, bento_ui_label_create,
                            &status_line, " ", &app->screen.status);
    }
    return status;
}

static bento_status_t build_screen(door_app_t *app)
{
    const spec_t title = {12, 6, -1, -1, COL_HAZ, 0, 0, 24};
    const spec_t link = {500, 10, -1, -1, COL_DIM, 0, 0, 16};
    bento_status_t status =
        bento_ui_screen(app->ui, BENTO_RUNTIME_SCREEN_WIDTH_PX,
                        BENTO_RUNTIME_SCREEN_HEIGHT_PX);

    if (status == BENTO_OK) {
        status = pause_ms(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_label(app->ui, &title, "ประตูโกดังกันขโมย");
    }
    if (status == BENTO_OK) {
        status = add_widget(app->ui, bento_ui_label_create, &link,
                            "MQTT: ออฟไลน์", &app->screen.link);
    }
    if (status == BENTO_OK) {
        status = add_door(app);
    }
    if (status == BENTO_OK) {
        status = add_distance(app);
    }
    if (status == BENTO_OK) {
        status = add_big(app);
    }
    if (status == BENTO_OK) {
        status = add_log(app);
    }
    if (status == BENTO_OK) {
        show_arm(app, true);
        drain_events(app->ui);
    }
    return status;
}

/* The door's look in each mode: the .py's LOOK. */
typedef struct {
    uint32_t door_color;
    const char *door_text;
    uint32_t color;
    const char *text;
} look_t;

static const look_t *look_of(door_mode_t mode)
{
    static const look_t looks[MODE_COUNT] = {
        [MODE_THIEF] = {COL_BAD, "มีคน!", COL_BAD, "มีคนที่ประตู!"},
        [MODE_NEAR] = {COL_HAZ, "มีคนเดิน", COL_HAZ, "มีคนหน้าโกดัง"},
        [MODE_WAIT] = {COL_STEEL, "รอปิด", COL_HAZ, "ยังไม่เฝ้า: รอคนออก"},
        [MODE_WATCH] = {COL_STEEL, "ปิดล็อก", COL_OK, "เฝ้าอยู่ ปลอดภัย"},
        [MODE_OFF] = {COL_CARD, "เปิดทำงาน", COL_DIM, "ปิดระบบเฝ้า"}};

    return &looks[mode];
}

/* The distance in words and on the bar; no target = the far end. The
 * Bar keeps its own fill colour, which shows best on the board (the
 * .py leaves Bar.color() out on purpose). */
static void show_distance(const door_app_t *app,
                          const radar_reading_t *reading, int32_t cm,
                          int32_t door_cm)
{
    char text[TEXT_BYTES];

    if (reading->has_target) {
        (void)snprintf(text, sizeof(text), "%" PRId32 " cm", cm);
    } else {
        (void)snprintf(text, sizeof(text), "--- cm");
    }
    set_text(app, app->screen.cm, text);
    (void)bento_ui_set_value(
        app->ui, app->screen.bar,
        (!reading->has_target || (cm > MAX_CM)) ? MAX_CM : cm);
    (void)snprintf(text, sizeof(text),
                   "ประตู < %" PRId32 " cm  หน้าโกดัง < %" PRId32 " cm",
                   door_cm, door_cm * 2);
    set_text(app, app->screen.zones, text);
}

static void show_door(door_app_t *app, const radar_reading_t *reading,
                      int32_t cm, int32_t door_cm, uint32_t now_ms)
{
    const int32_t left = exit_left(&app->guard, now_ms);
    const look_t *look = look_of(guard_mode(&app->guard, now_ms));
    char number[NUMBER_BYTES];

    (void)bento_ui_set_color(app->ui, app->screen.door,
                             look->door_color);
    set_text(app, app->screen.door_text, look->door_text);
    show_distance(app, reading, cm, door_cm);
    (void)bento_ui_set_color(app->ui, app->screen.state, look->color);
    set_text(app, app->screen.state, look->text);
    set_text(app, app->screen.big_title,
             (left != 0) ? "ออกจากโกดังใน (วิ)" : "ถึงประตูคืนนี้ (ครั้ง)");
    (void)snprintf(number, sizeof(number), "%" PRId32,
                   (left != 0) ? left : app->guard.count);
    set_text(app, app->screen.big, number);
}

/* ---- 3b) เครือข่าย ---- */
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
        status = pause_ms(CONNECT_POLL_MS);
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
        status = pause_ms(CONNECT_POLL_MS);
        if (status == BENTO_OK) {
            status = bento_mqtt_open_poll(broker);
        }
    }
    return status;
}

/* One mqtt.connect() with a new client id; a client that cannot
 * start or a broker that does not answer is the next try. */
static bento_status_t open_broker(door_app_t *app, bool *linked)
{
    static const char broker_host[] = "broker.hivemq.com";
    /* + a number from the board's clock: never an old id */
    static const char client_prefix[] = "bento-farm-";
    char client_id[CLIENT_ID_BYTES];
    bento_mqtt_config_t config;
    bento_status_t status;

    (void)snprintf(
        client_id, sizeof(client_id), "%s%s-%04" PRIx32, client_prefix,
        TEAM, bento_time_ticks_ms() & (uint32_t)CLIENT_SUFFIX_MASK);
    bento_mqtt_config_init(&config);
    config.host = broker_host;
    config.host_len = (uint32_t)strlen(broker_host);
    config.port = (uint16_t)BROKER_PORT;
    config.keepalive_s = (uint16_t)KEEPALIVE_S;
    config.client_id = client_id;
    config.client_id_len = (uint32_t)strlen(client_id);
    *linked = false;
    status = bento_mqtt_open_start(&config, &app->broker);
    app->has_broker = status == BENTO_OK;
    if (app->has_broker) {
        status = wait_for_broker(app->broker);
        *linked = status == BENTO_OK;
    }
    if (app->has_broker && !*linked && (status != BENTO_E_CANCELLED)) {
        (void)bento_mqtt_close(app->broker); /* the next try opens */
        app->has_broker = false;
    }
    if (status != BENTO_E_CANCELLED) {
        status = BENTO_OK;
    }
    return status;
}

/* Three tries, a new client id every time. */
static bento_status_t connect_broker(door_app_t *app, bool *linked)
{
    bento_status_t status = BENTO_OK;
    int32_t attempt;

    *linked = false;
    for (attempt = 1;
         (status == BENTO_OK) && !*linked && (attempt <= BROKER_TRIES);
         attempt++) {
        status = open_broker(app, linked);
    }
    return status;
}

/* Another team's name would make the broker drop that team's board:
 * "team" and two digits, and never the example "team00". */
static bool team_is_set(const char *team)
{
    const uint32_t length = (uint32_t)strlen(team);
    bool set = (length == (uint32_t)TEAM_LEN) &&
               (strncmp(team, "team", TEAM_PREFIX_LEN) == 0) &&
               (strcmp(team, "team00") != 0);
    uint32_t i;

    for (i = TEAM_PREFIX_LEN; set && (i < length); i++) {
        set = is_digit((uint8_t)team[i]);
    }
    return set;
}

/* *why: "" = online; otherwise why the guard watches offline. */
static bento_status_t go_online(door_app_t *app, const char **why)
{
    uint8_t granted_qos = 0U;
    bool joined = false;
    bool linked = false;
    bento_status_t status = BENTO_OK;

    *why = "";
    if (!team_is_set(TEAM)) {
        *why = "แก้ TEAM ก่อน";
    } else {
        say(app, "ต่อ WiFi... จอนิ่งได้", COL_HAZ);
        drain_events(app->ui);
        status = join_wifi(&joined);
        if ((status == BENTO_OK) && (!joined || !has_address())) {
            *why = "WiFi ไม่ติด";
        }
    }
    if ((status == BENTO_OK) && ((*why)[0] == '\0')) {
        status = connect_broker(app, &linked);
    }
    if ((status == BENTO_OK) && ((*why)[0] == '\0') &&
        (!linked ||
         (bento_mqtt_subscribe(app->broker, app->topic_cmd,
                               (uint32_t)strlen(app->topic_cmd), 0U,
                               &granted_qos) != BENTO_OK))) {
        *why = "broker ไม่ตอบ";
    }
    return status;
}

/* A lost link means "watch on offline". */
static void send(door_app_t *app, const char *event)
{
    bento_status_t status = BENTO_OK;

    if (app->guard.online) {
        status = bento_mqtt_publish(app->broker, app->topic_event,
                                    (uint32_t)strlen(app->topic_event),
                                    (const uint8_t *)event,
                                    (uint32_t)strlen(event), 0U, 0U);
    }
    if (status == BENTO_E_SESSION) {
        app->guard.online = false;
        show_link(app, "สายหลุด");
    }
}

/* {"id", "event": "armed", "on", "delay_s"} */
static void send_armed(door_app_t *app, bool on)
{
    char event[EVENT_BYTES];

    (void)snprintf(event, sizeof(event),
                   "{\"id\": \"%s\", \"event\": \"armed\", \"on\": %d, "
                   "\"delay_s\": %d}",
                   TEAM, on ? 1 : 0, on ? EXIT_S : 0);
    send(app, event);
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

/* ---- 5) โปรแกรมหลัก ---- */
/* Everybody steps back while the board counts down, then it learns
 * the still scene and gives the workers EXIT_S seconds to leave. */
static bento_status_t start_guard(door_app_t *app)
{
    static const uint8_t notes[] = {72U, 79U};
    char text[TEXT_BYTES];
    int32_t second;
    bool learned = false;
    bento_status_t status = BENTO_OK;

    for (second = COUNTDOWN_S; (second > 0) && (status == BENTO_OK);
         second--) {
        (void)snprintf(text, sizeof(text),
                       "ถอยห่างบอร์ด! จำฉากนิ่งใน %" PRId32 " วินาที",
                       second);
        say(app, text, COL_HAZ);
        status = pause_ms(MS_PER_S);
    }
    if (status == BENTO_OK) {
        status = calibrate_radar(&learned);
    }
    if (status == BENTO_OK) {
        if (learned) {
            (void)snprintf(text, sizeof(text),
                           "จำฉากแล้ว ออกจากโกดังภายใน %d วินาที", EXIT_S);
            say(app, text, COL_OK);
        } else {
            say(app, "จำฉากไม่สำเร็จ ใช้ฉากเดิม", COL_BAD);
        }
        app->guard.exit_running = true;
        app->guard.arm_ms = bento_time_ticks_ms();
        status = beep(app->ui, notes, (uint32_t)sizeof(notes));
    }
    if (status == BENTO_OK) {
        send_armed(app, true);
    }
    return status;
}

/* The switch's new position, if the screen moved it. ui.poll() is
 * called only here; it answers no events when it fails. */
static bool switch_position(const door_app_t *app, bool armed)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;
    uint32_t i;
    bool position = armed;

    if (bento_ui_poll(app->ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                      NULL) != BENTO_OK) {
        count = 0U;
    }
    for (i = 0U; i < count; i++) {
        if ((events[i].id == app->screen.toggle) &&
            (events[i].type == (uint8_t)BENTO_UI_EVENT_TOGGLED)) {
            position = events[i].value == 1;
        }
    }
    return position;
}

/* The switch on the screen, or SW5. */
static bento_status_t on_arm(door_app_t *app)
{
    guard_t *guard = &app->guard;
    bool armed = switch_position(app, guard->armed);
    bento_status_t status = BENTO_OK;

    if (button_clicked(&app->buttons[ARM_BUTTON])) {
        armed = !armed;
        (void)bento_ui_set_value(app->ui, app->screen.toggle,
                                 armed ? 1 : 0);
    }
    if (armed != guard->armed) {
        guard->armed = armed;
        guard->inside = false;
        guard->streak = 0;
        guard->acknowledged = true;
        guard->near = false;
        guard->near_streak = 0;
        guard->exit_running = true;
        guard->arm_ms = bento_time_ticks_ms();
        show_arm(app, armed);
        status = beep_once(app->ui, NOTE_TAP);
        if (status == BENTO_OK) {
            send_armed(app, armed);
        }
    }
    return status;
}

/* Stops the repeated siren; by is "app" or "sw6". */
static bento_status_t on_acknowledge(door_app_t *app, const char *by)
{
    guard_t *guard = &app->guard;
    char text[TEXT_BYTES];
    bento_status_t status = BENTO_OK;

    if (guard->inside && !guard->acknowledged) {
        guard->acknowledged = true;
        (void)snprintf(text, sizeof(text), "รับทราบแล้ว (%s)", by);
        say(app, text, COL_OK);
        status = beep_once(app->ui, NOTE_TAP);
        if (status == BENTO_OK) {
            (void)snprintf(
                text, sizeof(text),
                "{\"id\": \"%s\", \"event\": \"ack\", \"by\": "
                "\"%s\", \"count\": %" PRId32 "}",
                TEAM, by, guard->count);
            send(app, text);
        }
    }
    return status;
}

/* Written only when the radar comes or goes. */
static void on_radar(door_app_t *app, bool answered)
{
    if (answered != app->guard.radar_ok) {
        app->guard.radar_ok = answered;
        if (answered) {
            say(app, "เรดาร์กลับมาแล้ว", COL_OK);
        } else {
            say(app, "เรดาร์ไม่ตอบ กำลังลองใหม่", COL_BAD);
        }
    }
}

/* Someone reached the door: count, log, tell the phone, siren. */
static bento_status_t door_reached(door_app_t *app, int32_t cm,
                                   uint32_t now_ms)
{
    guard_t *guard = &app->guard;
    char text[EVENT_BYTES];

    guard->count++;
    (void)snprintf(text, sizeof(text),
                   "ครั้งที่ %" PRId32 " ระยะ %" PRId32 " cm", guard->count,
                   cm);
    set_text(app, app->screen.last, text);
    guard->acknowledged = false;
    guard->beep_ms = now_ms;
    (void)snprintf(
        text, sizeof(text),
        "{\"id\": \"%s\", \"event\": \"intruder\", \"count\": "
        "%" PRId32 ", \"cm\": %" PRId32 ", \"loiter\": %" PRId32 "}",
        TEAM, guard->count, cm, guard->loiter);
    send(app, text);
    return siren(app->ui);
}

/* The door is clear again. */
static bento_status_t door_cleared(door_app_t *app)
{
    static const uint8_t notes[] = {79U, 84U};
    char event[EVENT_BYTES];

    app->guard.acknowledged = true;
    (void)snprintf(event, sizeof(event),
                   "{\"id\": \"%s\", \"event\": \"clear\", \"count\": "
                   "%" PRId32 ", \"loiter\": %" PRId32 "}",
                   TEAM, app->guard.count, app->guard.loiter);
    send(app, event);
    return beep(app->ui, notes, (uint32_t)sizeof(notes));
}

/* The yard in front: counted, with one short note. */
static bento_status_t watch_yard(door_app_t *app,
                                 const radar_reading_t *reading,
                                 int32_t cm, int32_t yard_cm,
                                 bool watching)
{
    guard_t *guard = &app->guard;
    const bool was_near = guard->near;
    const belief_t near =
        confirm(guard->near, guard->near_streak,
                in_zone(watching, reading, cm, yard_cm));
    bento_status_t status = BENTO_OK;

    guard->near = near.inside;
    guard->near_streak = near.streak;
    if (guard->near && !was_near) {
        guard->loiter++;
        status = beep_once(app->ui, NOTE_YARD);
    }
    return status;
}

/* The yard counts, the door raises the alarm and the siren. */
static bento_status_t decide(door_app_t *app,
                             const radar_reading_t *reading, int32_t cm,
                             int32_t door_cm, uint32_t now_ms)
{
    guard_t *guard = &app->guard;
    const bool watching =
        guard->armed && (exit_left(guard, now_ms) == 0);
    const bool was_near = guard->near;
    const bool was_inside = guard->inside;
    belief_t inside;
    bento_status_t status =
        watch_yard(app, reading, cm, door_cm * 2, watching);

    inside = confirm(guard->inside, guard->streak,
                     in_zone(watching, reading, cm, door_cm));
    guard->inside = inside.inside;
    guard->streak = inside.streak;
    if ((status == BENTO_OK) && guard->inside && !was_inside) {
        status = door_reached(app, cm, now_ms);
    } else if ((status == BENTO_OK) && was_inside && !guard->inside) {
        status = door_cleared(app);
    } else {
        /* no change at the door */
    }
    if ((guard->inside != was_inside) || (guard->near != was_near)) {
        char text[TEXT_BYTES];

        (void)snprintf(text, sizeof(text),
                       "เดินผ่านหน้าโกดัง %" PRId32 "  ถึงประตู %" PRId32
                       " ครั้ง",
                       guard->loiter, guard->count);
        set_text(app, app->screen.count, text);
    }
    return status;
}

/* The matrix is drawn only when its picture changes. */
static void matrix_tick(guard_t *guard, uint32_t now_ms)
{
    const door_mode_t mode = guard_mode(guard, now_ms);
    const int32_t number = (mode == MODE_WATCH) ? guard->count : 0;

    if (!guard->has_shown || (mode != guard->shown_mode) ||
        (number != guard->shown_number)) {
        show_matrix(mode, number);
        guard->has_shown = true;
        guard->shown_mode = mode;
        guard->shown_number = number;
    }
}

/* The ack from the app and from SW6. */
static bento_status_t take_acknowledgements(door_app_t *app)
{
    uint8_t payload[PAYLOAD_KEEP];
    uint32_t len = 0U;
    bento_status_t status = BENTO_OK;

    if (app->guard.online && !link_is_up(app->broker)) {
        app->guard.online = false;
        show_link(app, "สายหลุด");
    }
    if (app->guard.online && take_newest(app->broker, payload, &len) &&
        is_acknowledge(payload, len)) {
        status = on_acknowledge(app, "app");
    }
    if ((status == BENTO_OK) &&
        button_clicked(&app->buttons[ACKNOWLEDGE_BUTTON])) {
        status = on_acknowledge(app, "sw6");
    }
    return status;
}

/* A reading that answered: smooth, decide, and on a TICK_MS round
 * show it. */
static bento_status_t on_reading(door_app_t *app,
                                 const radar_reading_t *reading,
                                 uint32_t now_ms, bool show_now)
{
    int32_t cm = 0;
    int32_t door_cm = 0;
    bento_status_t status;

    if (reading->has_target) {
        cm = smooth(&app->guard, reading->cm);
    }
    status = zone_cm(&door_cm);
    if (status == BENTO_OK) {
        status = decide(app, reading, cm, door_cm, now_ms);
    }
    if ((status == BENTO_OK) && show_now) {
        show_door(app, reading, cm, door_cm, now_ms);
    }
    return status;
}

/* One round: the switch, the acks, a reading, the siren, the
 * matrix. */
static bento_status_t guard_round(door_app_t *app, bool show_now)
{
    radar_reading_t reading;
    uint32_t now_ms = 0U;
    bento_status_t status = on_arm(app);

    if (status == BENTO_OK) {
        status = take_acknowledgements(app);
    }
    if (status == BENTO_OK) {
        const bool answered = read_radar(&reading);

        on_radar(app, answered);
        now_ms = bento_time_ticks_ms();
        if (answered) {
            status = on_reading(app, &reading, now_ms, show_now);
        }
    }
    if ((status == BENTO_OK) && app->guard.inside &&
        !app->guard.acknowledged &&
        (bento_time_ticks_diff(now_ms, app->guard.beep_ms) >=
         BEEP_MS)) {
        app->guard.beep_ms = now_ms;
        status = siren(app->ui);
    }
    if (status == BENTO_OK) {
        matrix_tick(&app->guard, now_ms);
    }
    return status;
}

/* Every SAMPLE_MS until RUN_MS; the screen every TICK_MS. */
static bento_status_t run_guard(door_app_t *app)
{
    const int32_t every = TICK_MS / SAMPLE_MS;
    const uint32_t start_ms = bento_time_ticks_ms();
    int32_t tick = 0;
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = guard_round(app, (tick % every) == 0);
        if (status == BENTO_OK) {
            tick++;
            status = wait_ms(SAMPLE_MS, app->buttons);
        }
    }
    return status;
}

/* Stopped halfway or not: the matrix goes dark and the round is
 * summed up. */
static void finish(const door_app_t *app)
{
    show_matrix(MODE_OFF, 0);
    say(app, "จบรอบแล้ว - กด Program to Device เพื่อเล่นใหม่", COL_HAZ);
    drain_events(app->ui);
    (void)printf("จบรอบ ถึงประตู %" PRId32 " เดินผ่าน %" PRId32 "\n",
                 app->guard.count, app->guard.loiter);
}

static bento_status_t run_door(door_app_t *app)
{
    const char *why = "";
    bento_status_t status;

    /* a build without sound refuses it, as hasattr() skips it */
    (void)bento_ui_sfx_volume_set(app->ui, SPEAKER);
    status = build_screen(app);
    if (status == BENTO_OK) {
        status = go_online(app, &why);
    }
    if (status == BENTO_OK) {
        app->guard.online = why[0] == '\0';
        show_link(app, why);
        status = start_guard(app);
    }
    if (status == BENTO_OK) {
        status = run_guard(app);
        finish(app);
    }
    return status;
}

int main(void)
{
    static const char topic_root[] = "bento-aiot";
    door_app_t app;
    bento_status_t status;

    (void)memset(&app, 0, sizeof(app));
    app.ui = bento_runtime_get_ui();
    app.buttons[ARM_BUTTON].index = ARM_BUTTON;
    app.buttons[ACKNOWLEDGE_BUTTON].index = ACKNOWLEDGE_BUTTON;
    app.guard.armed = true;
    app.guard.radar_ok = true;
    app.guard.acknowledged = true;
    app.guard.exit_running = true;
    app.guard.arm_ms = bento_time_ticks_ms();
    (void)snprintf(app.topic_event, sizeof(app.topic_event),
                   "%s/%s/event", topic_root, TEAM);
    (void)snprintf(app.topic_cmd, sizeof(app.topic_cmd), "%s/%s/cmd",
                   topic_root, TEAM);
    status = run_door(&app);
    if (app.has_broker) {
        (void)bento_mqtt_close(app.broker); /* the .py ignores it */
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
