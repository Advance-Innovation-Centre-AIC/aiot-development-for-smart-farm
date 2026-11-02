/*
 * idea_5_night_pen.c -- คอกสัตว์กลางคืน: เรดาร์จับผู้บุกรุกที่รั้วคอก
 * แจ้งมือถือ (ขั้นสูง)
 * (An animal pen at night: the radar catches an intruder at the pen's
 * fence and tells the phone; advanced)
 *
 * ต่อ WiFi และ broker (ต่อไม่ได้ก็เฝ้าออฟไลน์ต่อ) ตอนเริ่มให้ถอยห่าง
 * บอร์ด บอร์ดนับถอยหลัง 3 วินาทีแล้วจำฉากนิ่ง จากนั้นอ่านเรดาร์ทุก
 * 250 ms ระยะเป้าผ่านค่ากลางของ 5 ค่าล่าสุด ใกล้กว่ารั้วคอก (VR3
 * 50-250 cm) ติดกัน CONFIRM_N รอบ = มีผู้บุกรุก: จอแดง ไซเรน ส่ง
 * event intruder พร้อม shift_s (เฝ้ามากี่วินาที) และ quiet_s (สงบมา
 * กี่วินาที) ไซเรนซ้ำทุก 5 วินาทีจนกด SW6 หรือแอปส่ง {"cmd":"ack"}
 * จอมีนาฬิกากะกลางคืน เวลาสงบ บันทึก 3 ครั้งล่าสุด กราฟระยะ และ RGB
 * matrix SW5 หรือสวิตช์บนจอ = เปิด/ปิดเฝ้า แก้ WIFI_SSID WIFI_PASS
 * และ TEAM ก่อนรัน เกณฑ์เป็นค่าสาธิต ไม่ใช่ระบบรักษาความปลอดภัยจริง
 * ใน Emulator VR1 = ระยะเป้า Shake = เคลื่อนไหว
 *
 * Joins Wi-Fi and the broker (and guards offline when it cannot).
 * Step back at the start: the board counts down 3 s and learns the
 * still scene. Then the radar is read every 250 ms; the distance is
 * the median of the last 5. Closer than the pen's fence (VR3,
 * 50-250 cm) CONFIRM_N rounds in a row is an intruder: a red screen,
 * a siren, and an "intruder" event with shift_s (seconds on guard)
 * and quiet_s (seconds of quiet before it). The siren repeats every
 * 5 s until SW6 or {"cmd":"ack"} from the app. The screen shows the
 * night-shift clock, the quiet time, the last 3 intruders, a
 * distance chart and the RGB matrix. SW5 or the on-screen switch
 * arms and disarms. Edit WIFI_SSID, WIFI_PASS and TEAM first. Demo
 * values, not a real security system. In the Emulator VR1 is the
 * distance and Shake is motion.
 *
 * Twin of idea_5_night_pen.py
 * (aiot-development-for-smart-farm/project/idea_5_night_pen.py).
 * Style: CLEAN_C.md.
 * Differs from the .py:
 *   - json: the events are built with snprintf, keys in the order
 *     the .py writes them (MicroPython's dict may print them in
 *     another order; a JSON reader does not care).
 *   - json.loads: asks_to_stop_siren() reads one standard JSON object and finds
 *     "cmd" at its top level (the last of two equal keys wins).
 *     MicroPython also takes some broken texts (a comma or colon
 *     missing); here those are not JSON. Inside a nested list or
 *     object only strings and brackets are checked. A string is
 *     compared as written: an escaped letter is not decoded.
 *   - THRESH_DB is THRESH_DB_TENTHS (4.0 dB = 40): an enum holds no
 *     real number.
 *   - try/except around the radar and the RGB matrix: the status is
 *     checked and the same fallback is taken; any radar failure
 *     skips the round, as the .py's OSError does.
 *   - get_message() keeps one message, a newer one overwrites it;
 *     the C client keeps a small ring, so each round takes them all
 *     and keeps the newest. Payloads past 255 bytes are cut, as the
 *     board's binding cuts them.
 *   - wifi.connect(): a name or password the radio refuses outright
 *     counts as not joined (the .py's binding cuts it to fit).
 *   - send(): the .py catches only the OSError of a dropped link; a
 *     publish the client refuses still counts as sent, here too.
 *   - mqtt.connect() is start and poll here; a failed attempt closes
 *     its client so the next attempt has a free one.
 *   - On a board without VR3, the buttons or the RGB matrix (the .py
 *     cannot import them there) VR3 reads 0, the buttons are never
 *     down and the matrix is skipped; any other failure of VR3 or
 *     the buttons ends the program.
 *   - The wifi and mqtt modules' own console lines are not printed.
 *   - Stop ends the program quietly, where MicroPython raises
 *     KeyboardInterrupt and its finally still draws the end screen;
 *     the client is closed in both.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_buttons.h"
#include "bento_mqtt.h"
#include "bento_pots.h"
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_sensors.h"
#include "bento_time.h"
#include "bento_wifi.h"

/* ---- 1) Settings (edit these) ---- */
/* WIFI_SSID and WIFI_PASS: edit them at the top of join_wifi(). */
static const char TEAM[] = "teamXX";

enum {
    BEEP_MS = 5000, /* the siren again this often until answered */
    MIN_CM = 50,    /* VR3 sets the fence at 50-250 cm */
    SPAN_CM = 200,
    CONFIRM_N = 3,         /* rounds in a row before we believe (1+) */
    THRESH_DB_TENTHS = 40, /* a target 4.0 dB above the still scene */
    MAX_CM = 300,          /* the far end of the chart */
    VOLUME = 25,
    SPEAKER = 40,
    RUN_MS = 600000, /* one round: 10 minutes */
    SAMPLE_MS = 250, /* read the radar this often */
    TICK_MS = 500    /* redraw this often */
};

/* Hardware and timing. */
enum {
    SW5_INDEX = 1, /* SW5, the lower button */
    SW6_INDEX = 0, /* SW6, the upper one */
    ZONE_POT = 2,  /* VR3 */
    POT_FULL = 4095,
    CM_PER_M = 100,
    TENTHS_PER_DB = 10,
    NOTE_MS = 120,
    SIREN_TONE_MS = 140,
    SIREN_STEP_MS = 150,
    SCENE_SETTLE_MS = 500,
    SETTLE_MS = 200, /* let the cleared screen settle */
    COUNTDOWN_S = 3,
    MS_PER_S = 1000,
    S_PER_MIN = 60,
    S_PER_HOUR = 3600,
    BUTTON_POLL_MS = 20,
    CONNECT_POLL_MS = 20,
    HISTORY_LEN = 5, /* the median of the last 5 distances */
    LOG_LINES = 3,   /* the night's last 3 intruders */
    CHART_POINTS = 400,
    ALERT_MATRIX_MS = 60,
    MATRIX_MS = 80,
    BROKER_PORT = 1883,
    KEEPALIVE_S = 60,
    BROKER_TRIES = 3,
    CLIENT_SUFFIX_MASK = 0xFFFF,
    TEAM_LENGTH = 6,
    TEAM_PREFIX_LENGTH = 4
};

/* A moonlit night, 0xRRGGBB. */
enum {
    COL_TEXT = 0xE9E7FF,
    COL_DIM = 0x8C8FB8,
    COL_CARD = 0x161A3A,
    COL_NIGHT = 0x0E1030,
    COL_MOON = 0xF4E3A1,
    COL_SAFE = 0x6EE7B7,
    COL_BAD = 0xFF4D6D,
    COL_WARN = 0xFFB86B,
    COL_ALERT_BG = 0x4A1230
};

/* The cards. */
enum {
    CARD_RADIUS = 10,
    CARD_TITLE_DX = 12,
    CARD_TITLE_DY = 6,
    CARD_TITLE_PX = 20,
    AUTO = -1 /* the factory's own size */
};

/* Buffer sizes, in bytes. */
enum {
    TEXT_BYTES = 128, /* a label text: 126 at most */
    LOG_BYTES = 40,   /* "hh:mm น. cm", any int32_t */
    CLOCK_BYTES = 32, /* "hh:mm" or "m:ss" of any int32_t */
    TOPIC_BYTES = 48,
    CLIENT_BYTES = 32,
    PAYLOAD_BYTES = 256, /* the binding keeps 255 and a NUL */
    TOPIC_KEEP = 64,     /* the program never reads the topic */
    JSON_BYTES = 192
};

/* The widgets the program changes after drawing them. */
enum {
    SLOT_LINK_TEXT = 0,
    SLOT_PEN,
    SLOT_STATE,
    SLOT_CM,
    SLOT_CHART,
    SLOT_LED,
    SLOT_SHIFT,
    SLOT_QUIET,
    SLOT_COUNT_TEXT,
    SLOT_LOG,
    SLOT_SWITCH,
    SLOT_ARM,
    SLOT_ZONE,
    SLOT_STATUS,
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

/* A dark card and its title. */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    const char *title;
    uint8_t slot; /* the panel's slot */
} card_t;

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t ids[SLOT_COUNT + 1]; /* + the SLOT_NONE scratch */
    uint8_t zone_series;               /* the chart's red fence line */
} screen_t;

/* One board button: a press is remembered until it is asked for. */
typedef struct {
    uint32_t index;
    bool down;
    bool clicked;
} board_button_t;

/* One radar reading: the distance (when there is a target), motion. */
typedef struct {
    bool has_target;
    int32_t cm;
    bool moving;
} radar_reading_t;

/* The guard: what it believes, the night so far and the link. */
typedef struct {
    bool armed;
    bool inside;
    int32_t streak;
    int32_t count;
    int32_t history[HISTORY_LEN];
    uint32_t history_len;
    char log[LOG_LINES][LOG_BYTES];
    uint32_t log_len;
    bool radar_ok;
    bool acknowledged;
    bool online;
    uint32_t beep_ms;
    bool has_shown; /* the matrix picture last sent */
    bool shown_armed;
    bool shown_inside;
    int32_t shown_count;
    uint32_t start_ms;
    uint32_t quiet_ms;
    uint32_t broker; /* the MQTT client, 0 when none */
    board_button_t sw5;
    board_button_t sw6;
    char topic_event[TOPIC_BYTES];
    char topic_cmd[TOPIC_BYTES];
} guard_t;

/* An incoming command: where its fields are in the payload. */
typedef struct {
    bool found;
    bool is_text;
    bool is_integer;
    uint32_t start; /* a text's bytes, inside the quotes */
    uint32_t end;
    int32_t number;
} json_field_t;

typedef struct {
    const uint8_t *text;
    uint32_t length;
    uint32_t at;
    bool valid; /* false once the text is not JSON */
    json_field_t cmd;
} command_t;

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

/* MIDI note numbers, not hertz. */
static bento_status_t beep(bento_ui_t *ui, const uint8_t *notes,
                           uint32_t count)
{
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < count) && (status == BENTO_OK); i++) {
        /* ui.tone is fire-and-forget; a build without sound refuses */
        (void)bento_ui_sfx_tone(
            ui, notes[i], (uint8_t)BENTO_UI_WAVE_SINE, VOLUME, NOTE_MS);
        status = rest(NOTE_MS);
    }
    return status;
}

/* VR3: the fence, 50-250 cm; a board without pots reads 50. */
static bento_status_t zone_cm(int32_t *alert_cm)
{
    uint16_t raw = 0U;
    const bento_status_t status = bento_pots_read(ZONE_POT, &raw);

    *alert_cm = ((int32_t)raw * (int32_t)SPAN_CM) / (int32_t)POT_FULL;
    *alert_cm += (int32_t)MIN_CM;
    return (status == BENTO_E_UNSUPPORTED) ? BENTO_OK : status;
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
        reading->cm = (int32_t)(range.distance_m * (float)CM_PER_M);
    }
    reading->moving = radar.presence != 0U;
    return answered;
}

/* radar_config(0) learns the still scene again (nobody may move in
 * front of the board), then the threshold says how much stronger
 * than that scene a target must be. *learned false: the radar did
 * not answer. */
static bento_status_t calibrate_radar(bool *learned)
{
    const float threshold_db =
        (float)THRESH_DB_TENTHS / (float)TENTHS_PER_DB;
    bento_status_t status = BENTO_OK;

    *learned = bento_sensors_radar_config(0.0F) == BENTO_OK;
    if (*learned) {
        status = rest(SCENE_SETTLE_MS);
    }
    if ((status == BENTO_OK) && *learned) {
        *learned = bento_sensors_radar_config(threshold_db) == BENTO_OK;
    }
    return status;
}

/* Reads a board button and remembers a new press. A board without
 * buttons never has one down. */
static bento_status_t sample_button(board_button_t *button)
{
    bool pressed = false;
    bento_status_t status =
        bento_buttons_pressed(button->index, &pressed);

    if (status == BENTO_E_UNSUPPORTED) {
        pressed = false;
        status = BENTO_OK;
    }
    if (pressed && !button->down) {
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

/* Waits wait_ms, reading both buttons every 20 ms so a short tap is
 * not missed. */
static bento_status_t wait_ms(guard_t *guard, int32_t duration_ms)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;
    bool waiting = true;

    while (waiting) {
        int32_t left_ms;

        status = sample_button(&guard->sw5);
        if (status == BENTO_OK) {
            status = sample_button(&guard->sw6);
        }
        left_ms = duration_ms - bento_time_ticks_diff(
                                    bento_time_ticks_ms(), start_ms);
        waiting = (status == BENTO_OK) && (left_ms > 0);
        if (waiting) {
            status = rest((left_ms < BUTTON_POLL_MS) ? (uint32_t)left_ms
                                                     : BUTTON_POLL_MS);
            waiting = status == BENTO_OK;
        }
    }
    return status;
}

/* Only when the picture changes. The .py passes an OSError of the
 * matrix, and a board without one answers UNSUPPORTED: both ignored. */
static void show_matrix(bool armed, bool inside, int32_t count)
{
    static const char alert[] = "PEN ALERT";

    (void)bento_rgbmatrix_scroll(NULL, 0U,
                                 BENTO_RGBMATRIX_SCROLL_COLOR_DEFAULT,
                                 BENTO_RGBMATRIX_SCROLL_MS_DEFAULT);
    (void)bento_rgbmatrix_clear();
    if (armed && inside) {
        (void)bento_rgbmatrix_scroll(alert, (uint32_t)strlen(alert),
                                     BENTO_RGBMATRIX_RED,
                                     ALERT_MATRIX_MS);
    } else if (armed) {
        (void)bento_rgbmatrix_score(count, BENTO_RGBMATRIX_CYAN);
    } else {
        /* disarmed: the matrix stays dark */
    }
}

static bento_status_t siren(bento_ui_t *ui)
{
    static const uint8_t notes[] = {81U, 74U, 81U, 74U};
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < (uint32_t)sizeof(notes)) && (status == BENTO_OK);
         i++) {
        (void)bento_ui_sfx_tone(ui, notes[i],
                                (uint8_t)BENTO_UI_WAVE_SQUARE, VOLUME,
                                SIREN_TONE_MS);
        status = rest(SIREN_STEP_MS);
    }
    return status;
}

/* ---- 3) The brain ---- */

/* The median of the last 5 distances: a radar jumps between frames. */
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

/* A new state must hold CONFIRM_N rounds in a row before it is
 * believed. */
static void confirm(guard_t *guard, bool near)
{
    if (near == guard->inside) {
        guard->streak = 0;
    } else if ((guard->streak + 1) >= CONFIRM_N) {
        guard->inside = near;
        guard->streak = 0;
    } else {
        guard->streak++;
    }
}

static void hhmm(int32_t seconds, char *text)
{
    (void)snprintf(text, CLOCK_BYTES, "%02" PRId32 ":%02" PRId32,
                   seconds / S_PER_HOUR,
                   (seconds / S_PER_MIN) % S_PER_MIN);
}

static void mmss(int32_t seconds, char *text)
{
    (void)snprintf(text, CLOCK_BYTES, "%" PRId32 ":%02" PRId32,
                   seconds / S_PER_MIN, seconds % S_PER_MIN);
}

static int32_t seconds_since(uint32_t now_ms, uint32_t then_ms)
{
    return bento_time_ticks_diff(now_ms, then_ms) / MS_PER_S;
}

static bool is_digit(uint8_t byte)
{
    return (byte >= (uint8_t)'0') && (byte <= (uint8_t)'9');
}

/* "team" and two digits, never the teacher's team00. */
static bool valid_team(const char *team)
{
    const uint32_t length = (uint32_t)strlen(team);
    bool valid = (length == (uint32_t)TEAM_LENGTH) &&
                 (strncmp(team, "team", TEAM_PREFIX_LENGTH) == 0);
    uint32_t i;

    for (i = TEAM_PREFIX_LENGTH; valid && (i < length); i++) {
        valid = is_digit((uint8_t)team[i]);
    }
    return valid && (strcmp(team, "team00") != 0);
}

/* ---- Reading a command ---- */

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
    field->is_text = true;
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
        any = true;
        command->at++;
    }
    command->valid = command->valid && any;
}

/* A number; an integer when it has no fraction and no exponent. */
static void read_number(command_t *command, json_field_t *field)
{
    const bool negative = peek(command) == (uint8_t)'-';

    if (negative) {
        command->at++;
    }
    field->number = 0;
    read_digits(command, field);
    field->is_integer = true;
    if (peek(command) == (uint8_t)'.') {
        command->at++;
        read_digits(command, field);
        field->is_integer = false;
    }
    if ((peek(command) == (uint8_t)'e') ||
        (peek(command) == (uint8_t)'E')) {
        command->at++;
        if ((peek(command) == (uint8_t)'+') ||
            (peek(command) == (uint8_t)'-')) {
            command->at++;
        }
        read_digits(command, field);
        field->is_integer = false;
    }
    field->number = negative ? -field->number : field->number;
}

/* true, false or null: every letter must be there. */
static void read_word(command_t *command)
{
    static const char *const words[] = {"true", "false", "null"};
    bool known = false;
    uint32_t i;

    for (i = 0U; !known && (i < (sizeof(words) / sizeof(words[0])));
         i++) {
        const uint32_t length = (uint32_t)strlen(words[i]);

        known = ((command->length - command->at) >= length) &&
                (strncmp((const char *)&command->text[command->at],
                         words[i], length) == 0);
        if (known) {
            command->at += length;
        }
    }
    command->valid = known;
}

/* A nested list or object: its strings and brackets are checked. */
static void skip_nested(command_t *command)
{
    json_field_t ignored = {false, false, false, 0U, 0U, 0};
    int32_t depth = 0;

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
}

static void read_value(command_t *command, json_field_t *field)
{
    const uint8_t byte = peek(command);

    field->found = true;
    if (byte == (uint8_t)'"') {
        read_string(command, field);
    } else if ((byte == (uint8_t)'{') || (byte == (uint8_t)'[')) {
        skip_nested(command);
    } else if ((byte == (uint8_t)'-') || is_digit(byte)) {
        read_number(command, field);
    } else {
        read_word(command);
    }
}

static bool text_is(const command_t *command, const json_field_t *field,
                    const char *wanted)
{
    const uint32_t length = (uint32_t)strlen(wanted);

    return field->is_text && ((field->end - field->start) == length) &&
           (strncmp((const char *)&command->text[field->start], wanted,
                    length) == 0);
}

/* One "key": value pair; only "cmd" is kept. */
static void read_member(command_t *command)
{
    json_field_t key = {false, false, false, 0U, 0U, 0};
    json_field_t value = {false, false, false, 0U, 0U, 0};

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
    if (command->valid && text_is(command, &key, "cmd")) {
        command->cmd = value;
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

/* {"cmd": "ack"}; anything else, or no JSON object, is not. */
static bool asks_to_stop_siren(const uint8_t *payload, uint32_t length)
{
    command_t command;

    (void)memset(&command, 0, sizeof(command));
    command.text = payload;
    command.length = length;
    command.valid = true;
    read_command(&command);
    return command.valid && text_is(&command, &command.cmd, "ack");
}

/* ---- 3b) The network ---- */

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
static bento_status_t try_broker(guard_t *guard, bool *linked)
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
    status = bento_mqtt_open_start(&config, &guard->broker);
    if (status == BENTO_OK) {
        status = wait_for_broker(guard->broker);
    }
    *linked = status == BENTO_OK;
    if (!*linked && (guard->broker != 0U)) {
        (void)bento_mqtt_close(guard->broker); /* a failed attempt */
        guard->broker = 0U;
    }
    if (status != BENTO_E_CANCELLED) {
        status = BENTO_OK;
    }
    return status;
}

/* The public broker is down now and then: 3 tries. */
static bento_status_t connect_broker(guard_t *guard, bool *linked)
{
    int32_t attempt;
    bento_status_t status = BENTO_OK;

    *linked = false;
    for (attempt = 1;
         (attempt <= BROKER_TRIES) && (status == BENTO_OK) && !*linked;
         attempt++) {
        status = try_broker(guard, linked);
    }
    return status;
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

static void say(const screen_t *screen, const char *text,
                uint32_t color)
{
    show_color(screen, SLOT_STATUS, color);
    show_text(screen, SLOT_STATUS, text);
}

/* why: "" when online; the guard goes on offline either way. */
static void show_link(const screen_t *screen, const char *why)
{
    char line[TEXT_BYTES];

    show_color(screen, SLOT_LINK_TEXT,
               (why[0] != '\0') ? COL_DIM : COL_SAFE);
    if (why[0] != '\0') {
        (void)snprintf(line, sizeof(line), "MQTT: ออฟไลน์ (%s)", why);
        show_text(screen, SLOT_LINK_TEXT, line);
    } else {
        show_text(screen, SLOT_LINK_TEXT, "MQTT: เชื่อมต่อแล้ว");
    }
}

/* "" when online, else why not; the guard goes on offline. */
static bento_status_t go_online(const screen_t *screen, guard_t *guard,
                                const char **why)
{
    uint8_t granted_qos = 0U;
    bool joined = false;
    bool linked = false;
    bento_status_t status = BENTO_OK;

    *why = "แก้ TEAM ก่อน";
    if (valid_team(TEAM)) {
        say(screen, "ต่อ WiFi... จอนิ่งได้", COL_WARN);
        drain_events(screen->ui); /* the label shows before the wait */
        status = join_wifi(&joined);
        *why = "WiFi ไม่ติด";
    }
    if ((status == BENTO_OK) && joined) {
        status = connect_broker(guard, &linked);
        *why = "broker ไม่ตอบ";
    }
    if ((status == BENTO_OK) && linked) {
        linked =
            bento_mqtt_subscribe(guard->broker, guard->topic_cmd,
                                 (uint32_t)strlen(guard->topic_cmd), 0U,
                                 &granted_qos) == BENTO_OK;
    }
    if ((status == BENTO_OK) && linked) {
        *why = "";
    }
    return status;
}

/* A dropped link means offline, and the guard goes on. */
static void send(const screen_t *screen, guard_t *guard,
                 const char *json)
{
    bento_status_t status;

    if (guard->online) {
        status = bento_mqtt_publish(
            guard->broker, guard->topic_event,
            (uint32_t)strlen(guard->topic_event), (const uint8_t *)json,
            (uint32_t)strlen(json), 0U, 0U);
        if ((status == BENTO_E_SESSION) || (status == BENTO_E_STALE)) {
            guard->online = false;
            show_link(screen, "สายหลุด");
        }
    }
}

/* ---- 4) The screen ---- */

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

static int16_t px(int32_t value)
{
    return (int16_t)value;
}

static bento_status_t add_card(screen_t *screen, const card_t *card)
{
    const widget_spec_t panel = {UI_WIDGET_PANEL, card->slot, NULL,
                                 card->x,         card->y,    card->w,
                                 card->h,         COL_CARD,   COL_DIM,
                                 CARD_RADIUS,     1};
    const widget_spec_t title = {UI_WIDGET_LABEL,
                                 SLOT_NONE,
                                 card->title,
                                 px(card->x + CARD_TITLE_DX),
                                 px(card->y + CARD_TITLE_DY),
                                 AUTO,
                                 AUTO,
                                 COL_MOON,
                                 0,
                                 0,
                                 CARD_TITLE_PX};
    bento_status_t status = add_widget(screen, &panel);

    if (status == BENTO_OK) {
        status = add_widget(screen, &title);
    }
    return status;
}

/* The title, the link line and the pen's fence card. */
static bento_status_t add_pen_card(screen_t *screen)
{
    static const widget_spec_t head[] = {
        {UI_WIDGET_LABEL, SLOT_NONE, "เวรยามคอกสัตว์กลางคืน", 12, 6, AUTO,
         AUTO, COL_MOON, 0, 0, 24},
        {UI_WIDGET_LABEL, SLOT_LINK_TEXT, "MQTT: ออฟไลน์", 500, 10, AUTO,
         AUTO, COL_DIM, 0, 0, 20},
    };
    static const card_t card = {12,      44, 520, 190, "รั้วคอก (เรดาร์)",
                                SLOT_PEN};
    static const widget_spec_t specs[] = {
        {UI_WIDGET_LABEL, SLOT_STATE, "สัตว์ปลอดภัย", 24, 80, AUTO, AUTO,
         COL_SAFE, 0, 0, 28},
        {UI_WIDGET_LABEL, SLOT_CM, "ระยะ --- cm", 330, 84, AUTO, AUTO,
         COL_TEXT, 0, 0, 20},
        /* 400 points for 390 px: the board draws no round dots */
        {UI_WIDGET_CHART, SLOT_CHART, NULL, 24, 120, 390, 104, COL_MOON,
         0, MAX_CM, 0},
    };
    bento_status_t status =
        add_widgets(screen, head, sizeof(head) / sizeof(head[0]));

    if (status == BENTO_OK) {
        status = add_card(screen, &card);
    }
    if (status == BENTO_OK) {
        status = add_widgets(screen, specs,
                             sizeof(specs) / sizeof(specs[0]));
    }
    if (status == BENTO_OK) {
        /* prop() is a setter: a refusal was ignored before too */
        (void)bento_ui_set_prop(screen->ui, screen->ids[SLOT_CHART],
                                BENTO_UI_PROP_CHART_POINTS,
                                CHART_POINTS);
        status = bento_ui_chart_add_series(
            screen->ui, screen->ids[SLOT_CHART], COL_BAD,
            &screen->zone_series);
    }
    return status;
}

/* The chart's key, the motion lamp and the shift card. */
static bento_status_t add_shift_card(screen_t *screen)
{
    static const widget_spec_t lamp[] = {
        {UI_WIDGET_LABEL, SLOT_NONE, "เหลือง = ระยะ\nแดง = รั้ว", 420, 122,
         AUTO, AUTO, COL_DIM, 0, 0, 20},
        {UI_WIDGET_LED, SLOT_LED, NULL, 434, 184, 22, 22, COL_WARN, 0,
         0, 0},
        {UI_WIDGET_LABEL, SLOT_NONE, "ขยับ", 464, 184, AUTO, AUTO,
         COL_DIM, 0, 0, 20},
    };
    static const card_t card = {
        544, 44, 236, 190, "เฝ้ามาแล้ว (ชม:นาที)", SLOT_NONE};
    static const widget_spec_t specs[] = {
        {UI_WIDGET_SEG7, SLOT_SHIFT, "00:00", 560, 80, 200, 60,
         COL_MOON, 0, 0, 0},
        {UI_WIDGET_LABEL, SLOT_QUIET, "สงบมา 0:00", 560, 150, AUTO,
         AUTO, COL_SAFE, 0, 0, 20},
        {UI_WIDGET_LABEL, SLOT_COUNT_TEXT, "คืนนี้ 0 ครั้ง", 560, 182, AUTO,
         AUTO, COL_TEXT, 0, 0, 20},
    };
    bento_status_t status =
        add_widgets(screen, lamp, sizeof(lamp) / sizeof(lamp[0]));

    if (status == BENTO_OK) {
        status = add_card(screen, &card);
    }
    if (status == BENTO_OK) {
        status = add_widgets(screen, specs,
                             sizeof(specs) / sizeof(specs[0]));
    }
    return status;
}

/* The night's log and the arming card; the bottom-right corner stays
 * free for the Console button. */
static bento_status_t add_log_and_arm(screen_t *screen)
{
    static const card_t log_card = {
        12, 244, 462, 96, "บันทึกคืนนี้ (3 ครั้งล่าสุด)", SLOT_NONE};
    static const widget_spec_t log_line = {
        UI_WIDGET_LABEL, SLOT_LOG, "ยังเงียบ", 22, 282, AUTO, AUTO,
        COL_TEXT,        0,        0,        20};
    static const card_t arm_card = {482, 244,       250,
                                    96,  "ระบบเฝ้า", SLOT_NONE};
    static const widget_spec_t specs[] = {
        {UI_WIDGET_SWITCH, SLOT_SWITCH, NULL, 494, 276, 64, 32, 0U, 0,
         0, 1},
        {UI_WIDGET_LABEL, SLOT_ARM, " ", 568, 272, AUTO, AUTO, COL_SAFE,
         0, 0, 20},
        {UI_WIDGET_LABEL, SLOT_ZONE, "รั้ว - cm (VR3)", 568, 302, AUTO,
         AUTO, COL_DIM, 0, 0, 20},
        {UI_WIDGET_LABEL, SLOT_STATUS, " ", 12, 352, AUTO, AUTO,
         COL_WARN, 0, 0, 20},
    };
    bento_status_t status = add_card(screen, &log_card);

    if (status == BENTO_OK) {
        status = add_widget(screen, &log_line);
    }
    if (status == BENTO_OK) {
        status = add_card(screen, &arm_card);
    }
    if (status == BENTO_OK) {
        status = add_widgets(screen, specs,
                             sizeof(specs) / sizeof(specs[0]));
    }
    return status;
}

static void show_arm(const screen_t *screen, bool armed)
{
    show_text(screen, SLOT_ARM, armed ? "เปิดเฝ้า (SW5)" : "ปิดเฝ้า (SW5)");
    show_color(screen, SLOT_ARM, armed ? COL_SAFE : COL_DIM);
}

/* The whole screen in the .py's order. */
static bento_status_t build_screen(screen_t *screen)
{
    bento_status_t status =
        bento_ui_screen(screen->ui, BENTO_RUNTIME_SCREEN_WIDTH_PX,
                        BENTO_RUNTIME_SCREEN_HEIGHT_PX);

    if (status == BENTO_OK) {
        status = rest(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_pen_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_shift_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_log_and_arm(screen);
    }
    if (status == BENTO_OK) {
        show_arm(screen, true);
        drain_events(screen->ui);
    }
    return status;
}

/* The pen card: its colour, the motion lamp, the state and the
 * distance. */
static void show_state(const screen_t *screen, const guard_t *guard,
                       const radar_reading_t *reading, int32_t cm)
{
    char line[TEXT_BYTES];
    uint32_t color = COL_DIM;
    const char *state = "ปิดระบบเฝ้า";

    if (guard->inside) {
        color = COL_BAD;
        state = "มีผู้บุกรุก!";
    } else if (guard->armed) {
        color = COL_SAFE;
        state = "สัตว์ปลอดภัย";
    } else {
        /* disarmed */
    }
    show_color(screen, SLOT_PEN,
               guard->inside ? COL_ALERT_BG : COL_CARD);
    show_value(screen, SLOT_LED, reading->moving ? 1 : 0);
    show_color(screen, SLOT_STATE, color);
    show_text(screen, SLOT_STATE, state);
    if (reading->has_target) {
        (void)snprintf(line, sizeof(line), "ระยะ %" PRId32 " cm", cm);
        show_text(screen, SLOT_CM, line);
    } else {
        show_text(screen, SLOT_CM, "ระยะ --- cm");
    }
}

/* The chart, the fence, the shift clock and the quiet time. */
static void show_pen(const screen_t *screen, const guard_t *guard,
                     const radar_reading_t *reading, int32_t cm,
                     int32_t alert_cm)
{
    const uint32_t now_ms = bento_time_ticks_ms();
    char clock[CLOCK_BYTES];
    char line[TEXT_BYTES];
    const int32_t shown_cm =
        (reading->has_target && (cm < MAX_CM)) ? cm : (int32_t)MAX_CM;

    show_state(screen, guard, reading, cm);
    /* set_next() is fire-and-forget in MicroPython too */
    (void)bento_ui_chart_set_next(screen->ui, screen->ids[SLOT_CHART],
                                  0U, shown_cm);
    (void)bento_ui_chart_set_next(screen->ui, screen->ids[SLOT_CHART],
                                  screen->zone_series, alert_cm);
    (void)snprintf(line, sizeof(line), "รั้ว %" PRId32 " cm (VR3)",
                   alert_cm);
    show_text(screen, SLOT_ZONE, line);
    hhmm(seconds_since(now_ms, guard->start_ms), clock);
    show_text(screen, SLOT_SHIFT, clock);
    if (guard->inside) {
        show_text(screen, SLOT_QUIET, "กำลังเตือน");
    } else {
        mmss(seconds_since(now_ms, guard->quiet_ms), clock);
        (void)snprintf(line, sizeof(line), "สงบมา %s", clock);
        show_text(screen, SLOT_QUIET, line);
    }
}

/* ---- 5) The program ---- */

/* Step back: 3 s to go, then the still scene is learned. */
static bento_status_t start_guard(const screen_t *screen,
                                  guard_t *guard)
{
    static const uint8_t notes[] = {72U, 79U};
    char line[TEXT_BYTES];
    bool learned = false;
    int32_t left_s = COUNTDOWN_S;
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) && (left_s > 0)) {
        (void)snprintf(line, sizeof(line),
                       "ถอยห่างบอร์ด! จำฉากนิ่งใน %" PRId32 " วินาที",
                       left_s);
        say(screen, line, COL_WARN);
        status = rest(MS_PER_S);
        left_s--;
    }
    if (status == BENTO_OK) {
        status = calibrate_radar(&learned);
    }
    if (status == BENTO_OK) {
        say(screen,
            learned ? "เฝ้าคอกแล้ว เดินเข้าหาบอร์ดได้เลย"
                    : "จำฉากไม่สำเร็จ ใช้ฉากเดิม",
            learned ? COL_SAFE : COL_BAD);
        guard->start_ms = bento_time_ticks_ms();
        guard->quiet_ms = guard->start_ms;
        status = beep(screen->ui, notes, (uint32_t)sizeof(notes));
    }
    return status;
}

/* The arming changed: a tune and an "armed" event. */
static bento_status_t arm(const screen_t *screen, guard_t *guard,
                          bool armed)
{
    static const uint8_t notes[] = {76U};
    char json[JSON_BYTES];
    bento_status_t status;

    guard->armed = armed;
    guard->inside = false;
    guard->streak = 0;
    guard->acknowledged = true;
    show_arm(screen, armed);
    status = beep(screen->ui, notes, (uint32_t)sizeof(notes));
    if (status == BENTO_OK) {
        (void)snprintf(
            json, sizeof(json),
            "{\"id\": \"%s\", \"event\": \"armed\", \"on\": %d}", TEAM,
            armed ? 1 : 0);
        send(screen, guard, json);
    }
    return status;
}

/* The switch on the screen, or SW5. ui.poll() is called only here;
 * it answers no events when it fails. */
static bento_status_t on_arm(const screen_t *screen, guard_t *guard)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;
    uint32_t i;
    bool armed = guard->armed;
    bento_status_t status = BENTO_OK;

    if (bento_ui_poll(screen->ui, events, BENTO_UI_POLL_EVENTS_MAX,
                      &count, NULL) != BENTO_OK) {
        count = 0U;
    }
    for (i = 0U; i < count; i++) {
        if ((events[i].id == screen->ids[SLOT_SWITCH]) &&
            (events[i].type == (uint8_t)BENTO_UI_EVENT_TOGGLED)) {
            armed = events[i].value == 1;
        }
    }
    if (button_clicked(&guard->sw5)) {
        armed = !armed;
        show_value(screen, SLOT_SWITCH, armed ? 1 : 0);
    }
    if (armed != guard->armed) {
        status = arm(screen, guard, armed);
    }
    return status;
}

/* Stops the repeating siren (the screen stays red). */
static bento_status_t on_answer(const screen_t *screen, guard_t *guard,
                                const char *by)
{
    static const uint8_t notes[] = {76U};
    char line[TEXT_BYTES];
    char json[JSON_BYTES];
    bento_status_t status = BENTO_OK;

    if (guard->inside && !guard->acknowledged) {
        guard->acknowledged = true;
        (void)snprintf(line, sizeof(line), "รับทราบแล้ว (%s)", by);
        say(screen, line, COL_SAFE);
        status = beep(screen->ui, notes, (uint32_t)sizeof(notes));
        (void)snprintf(
            json, sizeof(json),
            "{\"id\": \"%s\", \"event\": \"ack\", \"by\": \"%s\", "
            "\"count\": %" PRId32 "}",
            TEAM, by, guard->count);
        if (status == BENTO_OK) {
            send(screen, guard, json);
        }
    }
    return status;
}

/* Written only when the radar comes or goes. */
static void on_radar(const screen_t *screen, guard_t *guard,
                     bool answered)
{
    if (answered != guard->radar_ok) {
        guard->radar_ok = answered;
        say(screen,
            answered ? "เรดาร์กลับมาแล้ว" : "เรดาร์ไม่ตอบ กำลังลองใหม่",
            answered ? COL_SAFE : COL_BAD);
    }
}

/* One more line in the night's log; the oldest of 3 goes. */
static void log_intruder(const screen_t *screen, guard_t *guard,
                         int32_t shift_s, int32_t cm)
{
    char clock[CLOCK_BYTES];
    char line[TEXT_BYTES];
    uint32_t i;

    if (guard->log_len == (uint32_t)LOG_LINES) {
        for (i = 1U; i < (uint32_t)LOG_LINES; i++) {
            (void)memcpy(guard->log[i - 1U], guard->log[i], LOG_BYTES);
        }
        guard->log_len--;
    }
    hhmm(shift_s, clock);
    (void)snprintf(guard->log[guard->log_len], LOG_BYTES,
                   "%s น. %" PRId32 " cm", clock, cm);
    guard->log_len++;
    line[0] = '\0';
    for (i = 0U; i < guard->log_len; i++) {
        const uint32_t used = (uint32_t)strlen(line);

        (void)snprintf(&line[used], sizeof(line) - used, "%s%s",
                       (i == 0U) ? "" : "  ", guard->log[i]);
    }
    show_text(screen, SLOT_LOG, line);
}

/* Someone came in: count, log, tell the phone, sound the siren. */
static bento_status_t intruder_in(const screen_t *screen,
                                  guard_t *guard, int32_t cm,
                                  uint32_t now_ms)
{
    char line[TEXT_BYTES];
    char json[JSON_BYTES];
    const int32_t quiet_s = seconds_since(now_ms, guard->quiet_ms);
    const int32_t shift_s = seconds_since(now_ms, guard->start_ms);

    guard->count++;
    log_intruder(screen, guard, shift_s, cm);
    (void)snprintf(line, sizeof(line), "คืนนี้ %" PRId32 " ครั้ง",
                   guard->count);
    show_text(screen, SLOT_COUNT_TEXT, line);
    guard->acknowledged = false;
    guard->beep_ms = now_ms;
    (void)snprintf(
        json, sizeof(json),
        "{\"id\": \"%s\", \"event\": \"intruder\", \"count\": %" PRId32
        ", \"cm\": %" PRId32 ", \"shift_s\": %" PRId32
        ", \"quiet_s\": %" PRId32 "}",
        TEAM, guard->count, cm, shift_s, quiet_s);
    send(screen, guard, json);
    return siren(screen->ui);
}

/* Acts only when the state changes. */
static bento_status_t decide(const screen_t *screen, guard_t *guard,
                             const radar_reading_t *reading, int32_t cm,
                             int32_t alert_cm)
{
    static const uint8_t notes[] = {79U, 84U};
    char json[JSON_BYTES];
    const bool was = guard->inside;
    uint32_t now_ms;
    bento_status_t status = BENTO_OK;

    confirm(guard,
            guard->armed && reading->has_target && (cm < alert_cm));
    now_ms = bento_time_ticks_ms();
    if (guard->inside && !was) {
        status = intruder_in(screen, guard, cm, now_ms);
    } else if (was && !guard->inside) {
        guard->acknowledged = true;
        guard->quiet_ms = now_ms;
        (void)snprintf(
            json, sizeof(json),
            "{\"id\": \"%s\", \"event\": \"clear\", \"count\": "
            "%" PRId32 "}",
            TEAM, guard->count);
        send(screen, guard, json);
        status = beep(screen->ui, notes, (uint32_t)sizeof(notes));
    } else {
        /* no change */
    }
    return status;
}

static void matrix_tick(guard_t *guard)
{
    if (!guard->has_shown || (guard->shown_armed != guard->armed) ||
        (guard->shown_inside != guard->inside) ||
        (guard->shown_count != guard->count)) {
        show_matrix(guard->armed, guard->inside, guard->count);
        guard->has_shown = true;
        guard->shown_armed = guard->armed;
        guard->shown_inside = guard->inside;
        guard->shown_count = guard->count;
    }
}

/* The link, the app's ack and SW6. */
static bento_status_t listen_round(const screen_t *screen,
                                   guard_t *guard)
{
    uint8_t payload[PAYLOAD_BYTES];
    char topic[TOPIC_KEEP];
    bento_mqtt_msg_t info;
    uint32_t state = BENTO_MQTT_STATE_CLOSED;
    uint32_t length = 0U;
    bool found = false;
    bool more;
    bento_status_t status = BENTO_OK;

    if (guard->online &&
        ((bento_mqtt_state(guard->broker, &state) != BENTO_OK) ||
         (state != BENTO_MQTT_STATE_CONNECTED))) {
        guard->online = false;
        show_link(screen, "สายหลุด");
    }
    more = guard->online; /* offline: no message is asked for */
    while (more) {
        bento_mqtt_msg_init(&info);
        more =
            bento_mqtt_poll(guard->broker, &info, topic, sizeof(topic),
                            payload, PAYLOAD_BYTES - 1U) == BENTO_OK;
        if (more) {
            length = info.payload_len;
            found = true;
        }
    }
    if (found && asks_to_stop_siren(payload, length)) {
        status = on_answer(screen, guard, "app");
    }
    if ((status == BENTO_OK) && button_clicked(&guard->sw6)) {
        status = on_answer(screen, guard, "sw6");
    }
    return status;
}

/* Read, decide and act; redraw every other round. */
static bento_status_t radar_round(const screen_t *screen,
                                  guard_t *guard, bool redraw)
{
    radar_reading_t reading = {false, 0, false};
    int32_t cm = 0;
    int32_t alert_cm = MIN_CM;
    const bool answered = read_radar(&reading);
    bento_status_t status = BENTO_OK;

    on_radar(screen, guard, answered);
    if (answered) {
        cm = reading.has_target ? smooth(guard, reading.cm) : 0;
        status = zone_cm(&alert_cm);
    }
    if ((status == BENTO_OK) && answered) {
        status = decide(screen, guard, &reading, cm, alert_cm);
    }
    if ((status == BENTO_OK) && answered && redraw) {
        show_pen(screen, guard, &reading, cm, alert_cm);
    }
    return status;
}

static bento_status_t guard_round(const screen_t *screen,
                                  guard_t *guard, bool redraw)
{
    bento_status_t status = on_arm(screen, guard);

    if (status == BENTO_OK) {
        status = listen_round(screen, guard);
    }
    if (status == BENTO_OK) {
        status = radar_round(screen, guard, redraw);
    }
    if ((status == BENTO_OK) && guard->inside && !guard->acknowledged &&
        (bento_time_ticks_diff(bento_time_ticks_ms(), guard->beep_ms) >=
         BEEP_MS)) {
        guard->beep_ms = bento_time_ticks_ms();
        status = siren(screen->ui); /* again until someone answers */
    }
    if (status == BENTO_OK) {
        matrix_tick(guard);
        status = wait_ms(guard, SAMPLE_MS);
    }
    return status;
}

static bento_status_t run_guard(const screen_t *screen, guard_t *guard)
{
    const int32_t every = TICK_MS / SAMPLE_MS;
    const uint32_t start_ms = bento_time_ticks_ms();
    int32_t tick = 0;
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = guard_round(screen, guard, (tick % every) == 0);
        tick++;
    }
    return status;
}

/* The end: the matrix dark, a last line and the console's sum. */
static void finish(const screen_t *screen, const guard_t *guard)
{
    show_matrix(false, false, guard->count);
    say(screen, "จบรอบแล้ว - กด Program to Device เพื่อเล่นใหม่", COL_WARN);
    drain_events(screen->ui);
    (void)printf("เวรคืนนี้จบ พบผู้บุกรุก %" PRId32 " ครั้ง\n", guard->count);
}

static bento_status_t run(const screen_t *screen, guard_t *guard)
{
    const char *why = "";
    bento_status_t status = go_online(screen, guard, &why);

    if (status == BENTO_OK) {
        guard->online = why[0] == '\0';
        show_link(screen, why);
        status = start_guard(screen, guard);
    }
    if (status == BENTO_OK) {
        status = run_guard(screen, guard);
    }
    if (status == BENTO_OK) {
        finish(screen, guard);
    }
    return status;
}

int main(void)
{
    static const char root[] = "bento-aiot";
    screen_t screen;
    guard_t guard;
    bento_status_t status;

    (void)memset(&screen, 0, sizeof(screen));
    (void)memset(&guard, 0, sizeof(guard));
    screen.ui = bento_runtime_get_ui();
    guard.armed = true;
    guard.radar_ok = true;
    guard.acknowledged = true;
    guard.sw5.index = SW5_INDEX;
    guard.sw6.index = SW6_INDEX;
    guard.start_ms = bento_time_ticks_ms();
    guard.quiet_ms = guard.start_ms;
    (void)snprintf(guard.topic_event, sizeof(guard.topic_event),
                   "%s/%s/event", root, TEAM);
    (void)snprintf(guard.topic_cmd, sizeof(guard.topic_cmd),
                   "%s/%s/cmd", root, TEAM);
    /* a build without sound refuses it, as hasattr() skips it */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    status = build_screen(&screen);
    if (status == BENTO_OK) {
        status = run(&screen, &guard);
    }
    if (guard.broker != 0U) {
        /* the .py ignores every error of its disconnect */
        (void)bento_mqtt_close(guard.broker);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
