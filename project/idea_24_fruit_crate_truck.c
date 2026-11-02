/*
 * idea_24_fruit_crate_truck.c -- ลังผลไม้บนรถกระบะ: แม่ค้ารู้ว่าผลไม้ช้ำ
 * ช่วงไหนของทาง กระแทกต่อเนื่อง = แจ้งคนขับชะลอ
 *
 * กล่องเฝ้าลังผลไม้บนรถกระบะ ฟังการกระแทกด้วยกฎการสั่นของ sf3_03: วาง
 * บอร์ดนิ่ง 5 วินาทีแรก (รถจอด) ให้เรียนรู้ค่าปกติ หมุน VR1 ไปตามทาง (สวน
 * ถึงตลาด) แล้วเคาะบอร์ด แถบช้ำ 8 ช่วงจำแรงกระแทกสูงสุดของแต่ละช่วงทาง
 * นาฬิกาเที่ยวนับเวลาเดินทาง เกรดผลไม้ (A B C) ตามจำนวนครั้งที่กระแทก
 * กระแทกต่อเนื่อง 5 รอบ = แจ้งคนขับชะลอ ดับไฟขับปกติ และส่ง event "vib"
 * ไฟสีฟ้าคือไฟขับปกติที่แม่ค้าสั่งผ่าน MQTT (คำสั่งของ sf2_03 ครบ) VR4 =
 * แบตกล่องเฝ้า SW6 = เริ่มเที่ยวใหม่ SW5 = ดับไฟ รายงานทุก 5 วินาที
 * เพิ่มคีย์ vib vib_bad bumps และ bruise_at ฟังนาน 30 นาที ก่อนรันแก้
 * WIFI_SSID, WIFI_PASS และ TEAM (เลขกลุ่ม) ที่หัวไฟล์
 *
 * A fruit crate on a pickup truck: the seller learns where on the road
 * the fruit got bruised. The board listens to the bumps with the
 * sf3_03 shaking rule: lie it still for the first 5 s (the truck
 * parked), turn VR1 along the road from the orchard to the market,
 * then tap the board. Eight bars keep the hardest bump of each part of
 * the road, a trip clock counts the time and the grade (A, B, C)
 * follows the number of bumps. Five shaky rounds in a row tell the
 * driver to slow down, switch the drive light off and send a "vib"
 * event. The blue LED is the drive light the seller switches through
 * MQTT (all of sf2_03's commands); VR4 is the box's battery; SW6
 * starts a new trip and SW5 switches the light off. Every 5 s a report
 * adds vib, vib_bad, bumps and bruise_at; the board listens for 30
 * minutes. Edit WIFI_SSID, WIFI_PASS and TEAM at the top before
 * running.
 *
 * Twin of idea_24_fruit_crate_truck.py (aiot-development-for-smart-farm/project/idea_24_fruit_crate_truck.py). Style: CLEAN_C.md.
 * Where C differs:
 *   - SEV_STEP = K_BAD in the .py; C cannot set one real constant
 *     from another, so sev_step in severity() repeats the value of
 *     k_bad in judge(): change both.
 *   - The Route class is a struct; Route(now) is a fresh struct, and
 *     worst_at None is a flag beside the number.
 *   - r floor-divided by SEV_STEP is written as MicroPython divides
 *     two reals:
 *     (r - fmod(r, s)) / s, floored, plus 1 when more than half is
 *     left; the min(3, ...) comes before the cast, so the cast never
 *     sees a value out of range; the answer is the same.
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
 *   - math.sqrt is sqrt() of <math.h> on a real number of the board's
 *     float width (bento_real_t: float on the PSoC Edge, double on the
 *     reTerminal), as its MicroPython computes; round(x, 2) is
 *     nearbyint(x * 100) / 100 as MicroPython's round(); json.dumps of
 *     a real number is bento_fmt_real(), MicroPython's own text.
 *   - sensors.bmi270.motion() raising OSError is any failed status here.
 *   - get_message() is one slot a newer message overwrites; the C
 *     client keeps a ring of BENTO_MQTT_RING_SLOTS, and each round
 *     takes them all and keeps the newest.
 *   - try/except OSError: a status. A broker client that cannot
 *     start, or a connect that fails, is the next try; a publish
 *     on a lost link is the original's False.
 *   - led_named(): a board without "RGB_BLUE" uses LED 0; an LED
 *     that cannot be switched off is the original's None.
 *   - ui.volume, ui.tone and the widget setters are fire and forget
 *     in modui.c, so their status is ignored here too; a board without
 *     volume (E_UNSUPPORTED) is the original's hasattr() False.
 *   - rgbmatrix is on the Dev Kit only (elsewhere the .py stops at
 *     its import); here the matrix calls are skipped where
 *     bento_rgbmatrix_available() is false.
 *   - raise Stop is a stopped flag that ends the status chain; the
 *     finally's mqtt.disconnect() is the close at the end of main.
 *   - wifi.* and mqtt.* print console lines of their own in
 *     MicroPython; the C library prints none.
 *   - Stop ends the program quietly at the next sleep, where
 *     MicroPython raises KeyboardInterrupt.
 *   - In the BENTO Emulator, MQTT from C is not available yet: the
 *     program says so and ends, after the .py's own "broker ไม่ตอบ"
 *     note, as it does when the broker is unreachable.
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

static const char BROKER[] = "broker.hivemq.com";
static const char CLIENT_PREFIX[] = "bento-farm-"; /* in CLIENT_ID */
/* CLIENT_ID = CLIENT_PREFIX + TEAM, built in open_broker() with a
 * number from the board's clock: never an old id */
static const char TOPIC_ROOT[] = "bento-aiot"; /* in the topics */
/* TOPIC_CMD = TOPIC_ROOT/TEAM/cmd, built in main() */
/* TOPIC = TOPIC_ROOT/TEAM/telemetry, built in main() */
/* TOPIC_EVENT = TOPIC_ROOT/TEAM/event, built in main() */
/* ---- end of settings ---- */

/* The shaking rule must give MicroPython's numbers: the compiler may not
 * fuse a multiply and an add into one step. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

/* The drive light, the shaking rule (demo values), the road and the
 * times. */
enum {
    PUMP_DEFAULT_S = 10, /* a command without "sec" */
    PUMP_MAX_S = 30,     /* whoever asks 9999 s gets 30 */
    TANK_MIN = 10,       /* the box's battery below this: never on */
    LEARN_MS = 5000,     /* lie still this long to learn the base */
    WIN = 10,            /* readings in one spread */
    CONFIRM_N = 5,       /* rounds in a row before the verdict flips */
    SEGS = 8,            /* the road from orchard to market, in parts */
    GRADE_B_BUMPS = 3,   /* this many bumps = grade B (as the web) */
    GRADE_C_BUMPS = 8,   /* this many bumps = grade C */
    SEND_MS = 5000,
    POLL_MS = 100,
    LISTEN_MS = 1800000,
    SPEAKER = 40, /* speaker volume, percent */
    VOLUME = 25   /* a tone's velocity */
};

/* ธีมแผงผลไม้: เขียวใบมะม่วง + ส้มมะม่วงสุก, 0xRRGGBB */
enum {
    COL_TEXT = 0xFFF4E6,
    COL_DIM = 0xA8B59A,
    COL_CARD = 0x1A2614,
    COL_OK = 0x9CCC65,
    COL_WARN = 0xFFB020,
    COL_BAD = 0xE5533D,
    COL_INFO = 0xFFD180
};

/* The board, the network and the texts. */
enum {
    STOP_BUTTON = 1, /* SW5 is buttons index 1 */
    TRIP_BUTTON = 0, /* SW6 is buttons index 0 */
    KNOB_WHERE = 0,  /* VR1: how far along the road */
    KNOB_TANK = 3,   /* VR4: the battery */
    KNOB_FULL = 4095,
    PERCENT = 100,
    SEV_MAX = 3,  /* a hard bump */
    SEV_HARD = 2, /* medium or more counts as hard */
    GRADE_A = 0,
    GRADE_B = 1,
    GRADE_C = 2,
    GRADE_COUNT = 3,
    SEG_BAR_X = 48,    /* the first part's bar */
    SEG_BAR_STEP = 92, /* one bar to the next */
    MS_PER_S = 1000,
    S_PER_MIN = 60,
    MINUTES_SHOWN = 100, /* the clock shows two digits of minutes */
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
    SAY_MAX = 20,          /* letters the matrix gets */
    NUMBER_SHOW_MAX = 6,   /* letters of a wrong "n" */
    UNKNOWN_SHOW_MAX = 12, /* letters of an unknown command */
    TEAM_LEN = 6,          /* "team" and two digits */
    TEAM_PREFIX_LEN = 4
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
    CLOCK_BYTES = 24,  /* two int32_t of digits */
    NUMBER_BYTES = 12, /* an int32_t, or null */
    /* the telemetry with every number at its widest */
    REPORT_BYTES = 320
};

/* SW5, sampled while the program waits so a short press counts. */
typedef struct {
    uint32_t index;
    bool down;
    bool clicked;
} button_t;

/* The widgets the program changes after drawing them. */
typedef struct {
    bento_ui_id_t segs[SEGS];
    bento_ui_id_t where;
    bento_ui_id_t trip;
    bento_ui_id_t bumps;
    bento_ui_id_t grade;
    bento_ui_id_t worst;
    bento_ui_id_t title;
    bento_ui_id_t lamp;
    bento_ui_id_t pump;
    bento_ui_id_t tank;
    bento_ui_id_t note;
    bento_ui_id_t link_lamp;
    bento_ui_id_t link_text;
} screen_t;
/* The shaking rule of sf3_03: the spread of the last WIN readings,
 * a base learnt while the board lies still, and a verdict that flips
 * only after CONFIRM_N rounds that agree. */
typedef struct {
    bento_real_t window[WIN];
    uint32_t count;
    bool
        has_vib; /* the original's vib is None until the window fills */
    bento_real_t vib;
    bool has_base;
    bento_real_t base;
    bento_real_t total;
    int32_t samples;
    uint32_t start_ms;
    bool bad;
    int32_t streak;
} vib_t;

/* One trip: the worst bump of each part of the road, and where the
 * worst of all was. */
typedef struct {
    uint32_t start_ms;
    int32_t bumps;
    int32_t hard;
    int32_t sev; /* the level last round */
    bool big;    /* this bump already counted as hard */
    int32_t segs[SEGS];
    int32_t worst;
    bool has_worst_at; /* the original's worst_at is None until then */
    int32_t worst_at;
} route_t;

/* Everything one run of the program keeps. */
typedef struct {
    bento_ui_t *ui;
    screen_t screen;
    button_t stop_button;
    button_t trip_button;
    char topic_cmd[NAME_BYTES];
    char topic[NAME_BYTES];
    char topic_event[NAME_BYTES];
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
    vib_t vib;
    route_t route;
    int32_t shown;     /* the seconds the matrix shows, -1 = none */
    int32_t sec_shown; /* the trip clock's seconds, -1 = none */
    uint32_t send_ms;
} farm_t;

/* The two knobs of one round, in percent. */
typedef struct {
    int32_t where; /* how far along the road, VR1 */
    int32_t tank;  /* the box's battery, VR4 */
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
    DO_COUNT
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

/* A wait that keeps reading SW5 and SW6, so a press between two
 * rounds still counts. */
static bento_status_t wait_ms(farm_t *farm, uint32_t duration_ms)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            (int32_t)duration_ms)) {
        status = sample_button(&farm->stop_button);
        if (status == BENTO_OK) {
            status = sample_button(&farm->trip_button);
        }
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
        status = bento_rgbmatrix_score(sec_left, BENTO_RGBMATRIX_BLUE);
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

/* The payload's bytes from at hold this word. */
static bool same_bytes(const uint8_t *text, uint32_t at,
                       const char *word, uint32_t length)
{
    bool same = true;
    uint32_t i;

    for (i = 0U; same && (i < length); i++) {
        same = text[at + i] == (uint8_t)word[i];
    }
    return same;
}

static bool skip_word(json_reader_t *reader, const char *word)
{
    const uint32_t length = (uint32_t)strlen(word);
    const bool same =
        ((reader->len - reader->at) >= length) &&
        same_bytes(reader->text, reader->at, word, length);

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
            same_bytes(reader->text, key->start + 1U, names[i],
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

static char to_upper(char letter)
{
    const uint8_t byte = (uint8_t)letter;
    const uint8_t case_bit = 0x20U;
    char upper = letter;

    if ((byte >= (uint8_t)'a') && (byte <= (uint8_t)'z')) {
        upper = (char)(uint8_t)(byte ^ case_bit);
    }
    return upper;
}

/* Only the printable English letters, in capitals: the matrix
 * shows nothing else. */
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
            out[length] = to_upper(text[i]);
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

/* "led" and "pump" switch the drive light; never while the crate
 * still bumps; nothing real is touched here. */
static void pump_command(const command_t *command, act_t act,
                         int32_t tank, bool bad, outcome_t *outcome)
{
    char shown[VALUE_BYTES];

    if ((act == ACT_LED) && !pump_number_is_zero(command)) {
        value_text(command, &command->fields[FIELD_N], shown,
                   sizeof(shown));
        cut_text(shown, NUMBER_SHOW_MAX);
        set_outcome(outcome, DO_DENY, "", COL_WARN);
        (void)snprintf(outcome->note, sizeof(outcome->note),
                       "ไม่มีไฟดวงที่ %s", shown);
    } else if (!value_is_true(command, &command->fields[FIELD_ON])) {
        set_outcome(outcome, DO_OFF, "สั่งดับไฟขับปกติ", COL_OK);
    } else if (bad) {
        set_outcome(outcome, DO_DENY, "ลังยังกระแทก ให้ชะลอก่อน", COL_BAD);
    } else if (tank < TANK_MIN) {
        set_outcome(outcome, DO_DENY, "", COL_BAD);
        (void)snprintf(outcome->note, sizeof(outcome->note),
                       "แบตเหลือ %" PRId32 "%% ไม่เปิดไฟ", tank);
    } else {
        outcome->sec = pump_seconds(command);
        set_outcome(outcome, DO_ON, "", COL_OK);
        (void)snprintf(outcome->note, sizeof(outcome->note),
                       "ไฟขับปกติ %" PRId32 " วิ", outcome->sec);
    }
}

static void other_command(const command_t *command, act_t act,
                          outcome_t *outcome)
{
    char shown[VALUE_BYTES];

    if (act == ACT_BEEP) {
        set_outcome(outcome, DO_BEEP, "แม่ค้าเรียกคนขับ!", COL_INFO);
    } else if (act == ACT_SAY) {
        ascii_only(command, outcome->say);
        set_outcome(outcome, DO_SAY, "", COL_INFO);
        (void)snprintf(
            outcome->note, sizeof(outcome->note), "ข้อความ: %s",
            (outcome->say[0] != '\0') ? outcome->say : "ไม่มีอักษรอังกฤษ");
    } else if (act == ACT_ACKNOWLEDGE) { /* ack belongs to sf2_04 */
        set_outcome(outcome, DO_NOTHING, "ไม่มีแจ้งเตือน (sf2_04)",
                    COL_INFO);
    } else if (act == ACT_OTHER) {
        value_text(command, &command->fields[FIELD_CMD], shown,
                   sizeof(shown));
        cut_text(shown, UNKNOWN_SHOW_MAX);
        set_outcome(outcome, DO_DENY, "", COL_WARN);
        (void)snprintf(outcome->note, sizeof(outcome->note), "ไม่รู้จัก %s",
                       shown);
    } else {
        set_outcome(outcome, DO_NOTHING, "", COL_INFO);
    }
}

/* One command -> what to do, its value, the note and its colour. */
static void handle_command(const command_t *command, int32_t tank,
                           bool bad, outcome_t *outcome)
{
    (void)memset(outcome, 0, sizeof(*outcome));
    if (!command->is_object) { /* 5, null and [] are JSON too */
        set_outcome(outcome, DO_DENY, "อ่านคำสั่งไม่ได้", COL_BAD);
    } else {
        const act_t act = act_of(command);

        if ((act == ACT_LED) || (act == ACT_PUMP)) {
            pump_command(command, act, tank, bad, outcome);
        } else {
            other_command(command, act, outcome);
        }
    }
}

/* The trip clock: two digits of minutes, then the seconds. */
static void clock_text(int32_t sec, char *out, uint32_t cap)
{
    (void)snprintf(out, cap, "%02" PRId32 ":%02" PRId32,
                   (sec / S_PER_MIN) % MINUTES_SHOWN, sec % S_PER_MIN);
}

/* ---- 3) สมอง: กฎการสั่น ---- */
/* The length of the acceleration; a failed read is no reading. */
static bool magnitude(bento_real_t *out)
{
    bento_sensors_imu6_t motion;
    bool ok;

    bento_sensors_imu6_init(&motion);
    ok = bento_sensors_bmi270_motion(&motion) == BENTO_OK;
    if (ok) {
        const bento_real_t ax = (bento_real_t)motion.ax;
        const bento_real_t ay = (bento_real_t)motion.ay;
        const bento_real_t az = (bento_real_t)motion.az;
        const bento_real_t xx = ax * ax;
        const bento_real_t yy = ay * ay;
        const bento_real_t zz = az * az;
        const bento_real_t xy = xx + yy;
        const bento_real_t sum = xy + zz;

        *out = (bento_real_t)sqrt((double)sum);
    }
    return ok;
}

/* Keeps the last WIN readings, oldest first; true once it is full. */
static bool window_add(vib_t *vib, bento_real_t reading)
{
    uint32_t i;

    if (vib->count == (uint32_t)WIN) {
        for (i = 1U; i < (uint32_t)WIN; i++) {
            vib->window[i - 1U] = vib->window[i];
        }
        vib->count--;
    }
    vib->window[vib->count] = reading;
    vib->count++;
    return vib->count == (uint32_t)WIN;
}

/* The standard deviation of the window, summed in the original's
 * order. */
static bento_real_t window_spread(const vib_t *vib)
{
    const bento_real_t size = (bento_real_t)WIN;
    bento_real_t sum = (bento_real_t)0.0;
    bento_real_t squares = (bento_real_t)0.0;
    bento_real_t mean;
    uint32_t i;

    for (i = 0U; i < (uint32_t)WIN; i++) {
        sum += vib->window[i];
    }
    mean = sum / size;
    for (i = 0U; i < (uint32_t)WIN; i++) {
        const bento_real_t away = vib->window[i] - mean;
        const bento_real_t square = away * away;

        squares += square;
    }
    const bento_real_t variance = squares / size;

    return (bento_real_t)sqrt((double)variance);
}

/* The first LEARN_MS: the board lies still and learns its base. */
static void learn_base(vib_t *vib, uint32_t now_ms)
{
    /* the lowest base (demo value) */
    static const bento_real_t min_base = (bento_real_t)0.05;

    vib->total += vib->vib;
    vib->samples++;
    if (bento_time_ticks_diff(now_ms, vib->start_ms) >= LEARN_MS) {
        const bento_real_t average =
            vib->total / (bento_real_t)vib->samples;

        vib->base = (average > min_base) ? average : min_base;
        vib->has_base = true;
    }
}

/* Each round over the line (or back under it) counts; CONFIRM_N in a
 * row flips the verdict. */
static bool judge(vib_t *vib)
{
    /* over the base this many times = a bad round (demo value) */
    static const bento_real_t k_bad = (bento_real_t)4.0;
    const bento_real_t limit = vib->base * k_bad;
    const bool over = vib->vib > limit;
    bool flipped;

    vib->streak = (over != vib->bad) ? (vib->streak + 1) : 0;
    flipped = vib->streak >= CONFIRM_N;
    if (flipped) {
        vib->bad = !vib->bad;
        vib->streak = 0;
    }
    return flipped;
}

/* One reading in; true when the verdict has just flipped. */
static bool vib_push(vib_t *vib, bento_real_t reading, uint32_t now_ms)
{
    bool flipped = false;

    if (window_add(vib, reading)) {
        vib->vib = window_spread(vib);
        vib->has_vib = true;
        if (!vib->has_base) {
            learn_base(vib, now_ms);
        } else {
            flipped = judge(vib);
        }
    }
    return flipped;
}

/* round(x, 2) as MicroPython does it, in the board's float width. */
static bento_real_t round_hundredth(bento_real_t value)
{
    const bento_real_t scale = (bento_real_t)100.0;
    const bento_real_t scaled = value * scale;

    return (bento_real_t)nearbyint((double)scaled) / scale;
}

/* json.dumps of the window's spread: null before the window fills. */
static void vib_json(const vib_t *vib, char *out, uint32_t cap)
{
    uint32_t length = 0U;

    if (vib->has_vib) {
        if (bento_fmt_real(out, cap, round_hundredth(vib->vib),
                           &length) != BENTO_OK) {
            copy_text(out, cap, "null");
        }
    } else {
        copy_text(out, cap, "null");
    }
}

/* ---- 3) สมอง: ช้ำช่วงไหนของทาง ---- */
/* 0 = no bump, 1 light, 2 medium, 3 hard: the spread over the base,
 * floor-divided by sev_step as MicroPython divides two reals. */
static int32_t severity(const vib_t *vib)
{
    /* past 1, 2, 3 times this = light, medium, hard; the .py sets it
     * to k_bad (in judge()): keep the two equal */
    static const bento_real_t sev_step = (bento_real_t)4.0;
    const bento_real_t ratio = vib->vib / vib->base;
    const bento_real_t rest =
        (bento_real_t)fmod((double)ratio, (double)sev_step);
    const bento_real_t whole = ratio - rest;
    const bento_real_t steps = whole / sev_step;
    const bento_real_t below = (bento_real_t)floor((double)steps);
    const bento_real_t half = (bento_real_t)0.5;
    const bento_real_t level =
        ((steps - below) > half) ? (below + (bento_real_t)1.0) : below;
    const bento_real_t most = (bento_real_t)SEV_MAX;

    return (level < most) ? (int32_t)level : SEV_MAX;
}

/* A new trip: nothing bruised yet. */
static void route_start(route_t *route, uint32_t now_ms)
{
    (void)memset(route, 0, sizeof(*route));
    route->start_ms = now_ms;
}

/* Counts the bump and its hardness: a new bump starts when the level
 * leaves 0, and it counts as hard once. */
static void count_bump(route_t *route, int32_t sev)
{
    if ((sev != 0) && (route->sev == 0)) {
        route->bumps++;
        route->big = false;
    }
    if ((sev >= SEV_HARD) && !route->big) {
        route->hard++;
        route->big = true;
    }
    route->sev = sev;
}

/* One level at one place on the road; answers the part of the road
 * to draw again, or -1. */
static int32_t route_feed(route_t *route, int32_t sev, int32_t where)
{
    const int32_t parts = SEGS;
    const int32_t full = PERCENT;
    const int32_t last = parts - 1;
    const int32_t part = (where * parts) / full;
    const int32_t i = (part < last) ? part : last;
    int32_t changed = -1;

    count_bump(route, sev);
    if (sev > route->segs[i]) {
        route->segs[i] = sev;
        if (sev > route->worst) {
            route->worst = sev;
            route->worst_at = where;
            route->has_worst_at = true;
        }
        changed = i;
    }
    return changed;
}

static int32_t route_grade(const route_t *route)
{
    int32_t grade = GRADE_C;

    if (route->bumps < GRADE_B_BUMPS) {
        grade = GRADE_A;
    } else if (route->bumps < GRADE_C_BUMPS) {
        grade = GRADE_B;
    } else {
        /* many bumps */
    }
    return grade;
}

static int32_t route_seconds(const route_t *route, uint32_t now_ms)
{
    return bento_time_ticks_diff(now_ms, route->start_ms) / MS_PER_S;
}

/* json.dumps of worst_at: null until the first bruise. */
static void worst_json(const route_t *route, char *out, uint32_t cap)
{
    if (route->has_worst_at) {
        (void)snprintf(out, cap, "%" PRId32, route->worst_at);
    } else {
        copy_text(out, cap, "null");
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

/* A dark card with its title; answers the title's id. */
static bento_status_t add_card(bento_ui_t *ui, const rect_t *place,
                               const char *title, bento_ui_id_t *out)
{
    const spec_t panel = {place->x, place->y, place->w, place->h,
                          COL_CARD, COL_DIM,  12,       1};
    const int16_t title_x = px(place->x + 12);
    const int16_t title_y = px(place->y + 6);
    const spec_t label = {title_x, title_y, -1, -1, COL_INFO, 0, 0, 20};
    bento_ui_id_t panel_id = 0U;
    bento_status_t status =
        add_widget(ui, bento_ui_panel_create, &panel, NULL, &panel_id);

    if (status == BENTO_OK) {
        status =
            add_widget(ui, bento_ui_label_create, &label, title, out);
    }
    return status;
}

/* Top: one bar per part of the road, the orchard and the market at
 * the ends of the "where" bar. */
static bento_status_t add_road(farm_t *farm)
{
    const rect_t card = {12, 44, 768, 150};
    const spec_t orchard = {28, 150, -1, -1, COL_DIM, 0, 0, 20};
    const spec_t where = {88, 158, 600, 10, 0U, 0, PERCENT, 0};
    const spec_t market = {706, 150, -1, -1, COL_DIM, 0, 0, 20};
    bento_ui_id_t label = 0U;
    bento_status_t status =
        add_card(farm->ui, &card,
                 "ช้ำตรงไหนของทาง (สูง = แรง / VR1 = ระยะทาง)", &label);
    int32_t i;

    for (i = 0; (status == BENTO_OK) && (i < SEGS); i++) {
        const spec_t bar = {px(SEG_BAR_X + (i * SEG_BAR_STEP)),
                            80,
                            40,
                            66,
                            0U,
                            0,
                            SEV_MAX,
                            0};

        status = add_widget(farm->ui, bento_ui_bar_create, &bar, NULL,
                            &farm->screen.segs[i]);
    }
    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_label_create, &orchard,
                            "สวน", &label);
    }
    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_bar_create, &where, NULL,
                            &farm->screen.where);
    }
    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_label_create, &market,
                            "ตลาด", &label);
    }
    return status;
}

/* Bottom left: the trip clock and its bumps. */
static bento_status_t add_trip(farm_t *farm)
{
    static const char trip_button_name[] = "SW6"; /* BTN_NAMES[1] */
    const rect_t card = {12, 204, 300, 140};
    const spec_t trip = {28, 236, 200, 52, COL_INFO, 0, 0, 0};
    const spec_t bumps = {28, 292, -1, -1, COL_TEXT, 0, 0, 20};
    const spec_t hint = {28, 318, -1, -1, COL_DIM, 0, 0, 20};
    char text[TEXT_BYTES];
    bento_ui_id_t label = 0U;
    bento_status_t status =
        add_card(farm->ui, &card, "เวลาเดินทาง", &label);

    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_seg7_create, &trip,
                            "00:00", &farm->screen.trip);
    }
    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_label_create, &bumps,
                            "กระแทก 0 ครั้ง", &farm->screen.bumps);
    }
    if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text), "%s = เริ่มเที่ยวใหม่",
                       trip_button_name);
        status = add_widget(farm->ui, bento_ui_label_create, &hint,
                            text, &label);
    }
    return status;
}

/* Bottom middle: the grade the fruit gets at the market. */
static bento_status_t add_grade(farm_t *farm)
{
    const rect_t card = {322, 204, 250, 140};
    const spec_t grade = {336, 236, -1, -1, COL_OK, 0, 0, 28};
    const spec_t worst = {336, 284, -1, -1, COL_DIM, 0, 0, 20};
    bento_ui_id_t label = 0U;
    bento_status_t status =
        add_card(farm->ui, &card, "เกรดผลไม้ถึงตลาด", &label);

    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_label_create, &grade,
                            "A ราคาเต็ม", &farm->screen.grade);
    }
    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_label_create, &worst,
                            "ยังไม่ช้ำ", &farm->screen.worst);
    }
    return status;
}

/* Bottom right: the drive light and the box's battery. */
static bento_status_t add_light(farm_t *farm)
{
    const rect_t card = {582, 204, 198, 140};
    const spec_t lamp = {596, 238, 32, 32, COL_INFO, 0, 0, 0};
    const spec_t pump = {638, 240, -1, -1, COL_DIM, 0, 0, 20};
    const spec_t tank = {596, 286, -1, -1, COL_TEXT, 0, 0, 20};
    bento_status_t status =
        add_card(farm->ui, &card, "ไฟขับ (คำสั่ง 0)", &farm->screen.title);

    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_led_create, &lamp, NULL,
                            &farm->screen.lamp);
    }
    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_label_create, &pump,
                            "ดับ", &farm->screen.pump);
    }
    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_label_create, &tank,
                            "แบต -- %", &farm->screen.tank);
    }
    return status;
}

static bento_status_t add_status_line(farm_t *farm)
{
    const spec_t note = {12, 352, -1, -1, COL_DIM, 0, 0, 20};
    const spec_t lamp = {596, 12, 18, 18, COL_OK, 0, 0, 0};
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
    const spec_t title = {12, 6, -1, -1, COL_TEXT, 0, 0, 24};
    bento_ui_id_t label = 0U;
    bento_status_t status = bento_ui_screen_default(farm->ui);

    if (status == BENTO_OK) {
        status = sleep_checked(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_widget(farm->ui, bento_ui_label_create, &title,
                            "ลังผลไม้บนกระบะ ช้ำช่วงไหน", &label);
    }
    if (status == BENTO_OK) {
        status = add_road(farm);
    }
    if (status == BENTO_OK) {
        status = add_trip(farm);
    }
    if (status == BENTO_OK) {
        status = add_grade(farm);
    }
    if (status == BENTO_OK) {
        status = add_light(farm);
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

    (void)bento_ui_set_value(farm->ui, farm->screen.lamp,
                             (sec_left != 0) ? 1 : 0);
    (void)bento_ui_set_color(farm->ui, farm->screen.pump,
                             (sec_left != 0) ? COL_OK : COL_DIM);
    if (sec_left != 0) {
        (void)snprintf(text, sizeof(text), "%" PRId32 " วิ", sec_left);
    } else {
        copy_text(text, sizeof(text), "ดับ");
    }
    set_text(farm, farm->screen.pump, text);
}

/* Every part of the road, the grade, the bumps and the worst place. */
static void show_route(const farm_t *farm)
{
    static const uint32_t grade_colors[GRADE_COUNT] = {COL_OK, COL_WARN,
                                                       COL_BAD};
    static const char *const grade_texts[GRADE_COUNT] = {
        "A ราคาเต็ม", "B ลดราคานิด", "C คัดช้ำออก"};
    const route_t *route = &farm->route;
    const int32_t grade = route_grade(route);
    char text[TEXT_BYTES];
    int32_t i;

    for (i = 0; i < SEGS; i++) {
        (void)bento_ui_set_value(farm->ui, farm->screen.segs[i],
                                 route->segs[i]);
        (void)bento_ui_set_color(
            farm->ui, farm->screen.segs[i],
            (route->segs[i] >= SEV_HARD) ? COL_BAD : COL_DIM);
    }
    (void)bento_ui_set_color(farm->ui, farm->screen.grade,
                             grade_colors[grade]);
    set_text(farm, farm->screen.grade, grade_texts[grade]);
    (void)snprintf(text, sizeof(text),
                   "กระแทก %" PRId32 " ครั้ง (แรง %" PRId32 ")",
                   route->bumps, route->hard);
    set_text(farm, farm->screen.bumps, text);
    if (route->has_worst_at) {
        (void)snprintf(text, sizeof(text), "ช้ำสุดที่ระยะ %" PRId32 " %%",
                       route->worst_at);
    } else {
        copy_text(text, sizeof(text), "ยังไม่ช้ำ");
    }
    set_text(farm, farm->screen.worst, text);
}

static void show_knobs(const farm_t *farm, const knobs_t *knobs)
{
    char text[TEXT_BYTES];

    (void)bento_ui_set_value(farm->ui, farm->screen.where,
                             knobs->where);
    (void)bento_ui_set_color(farm->ui, farm->screen.tank,
                             (knobs->tank >= TANK_MIN) ? COL_TEXT
                                                       : COL_BAD);
    (void)snprintf(text, sizeof(text), "แบต %" PRId32 " %%",
                   knobs->tank);
    set_text(farm, farm->screen.tank, text);
}

/* The trip clock, written only when a second has passed. */
static void show_trip(farm_t *farm, uint32_t now_ms)
{
    const int32_t sec = route_seconds(&farm->route, now_ms);
    char clock[CLOCK_BYTES];

    if (sec != farm->sec_shown) {
        farm->sec_shown = sec;
        clock_text(sec, clock, sizeof(clock));
        set_text(farm, farm->screen.trip, clock);
    }
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

    (void)snprintf(
        client_id, sizeof(client_id), "%s%s-%04" PRIx32, CLIENT_PREFIX,
        TEAM, bento_time_ticks_ms() & (uint32_t)CLIENT_SUFFIX_MASK);
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
    farm->no_mqtt = status == BENTO_E_UNSUPPORTED;
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

/* The BENTO Emulator answers every MQTT call with E_UNSUPPORTED: one
 * label says so in the empty strip right of the note, then the
 * program goes on as if the broker did not answer. */
static bento_status_t show_emulator_gap(const farm_t *farm)
{
    static const char gap_text[] =
        "MQTT จาก C ยังไม่มีใน BENTO Emulator ให้รันบน Dev Kit หรือ Eva Kit";
    /* the BENTO Emulator does not wrap a label to its width, so the
     * label breaks its lines itself (a new line for a space) */
    static const char lines[] = "MQTT จาก C ยังไม่มีใน BENTO Emulator\n"
                                "ให้รันบน Dev Kit หรือ Eva Kit";
    /* two lines of 26 px under the cards, ending at x 690, left
     * of the firmware's Console button */
    const spec_t gap = {320, 346, 370, -1, COL_WARN, 0, 0, 20};
    bento_ui_id_t label = 0U;

    (void)printf(
        "%s / MQTT from C is not in the BENTO Emulator yet: run "
        "it on a Dev Kit or Eva Kit.\n",
        gap_text);
    return add_widget(farm->ui, bento_ui_label_create, &gap, lines,
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
            show_note(farm, text, COL_WARN);
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
    show_note(farm, "ต่อ WiFi... จอนิ่งได้", COL_WARN);
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
static bool publish_text(const farm_t *farm, const char *topic,
                         const char *text)
{
    const bento_status_t status = bento_mqtt_publish(
        farm->broker, topic, (uint32_t)strlen(topic),
        (const uint8_t *)text, (uint32_t)strlen(text), 0U, 0U);

    return status != BENTO_E_SESSION;
}

static bool send_report(const farm_t *farm, const knobs_t *knobs)
{
    char vib_text[BENTO_FMT_BUF_SIZE];
    char worst_text[NUMBER_BYTES];
    char report[REPORT_BYTES];

    vib_json(&farm->vib, vib_text, sizeof(vib_text));
    worst_json(&farm->route, worst_text, sizeof(worst_text));
    (void)snprintf(
        report, sizeof(report),
        "{\"id\": \"%s\", \"n\": %" PRId32 ", \"soil\": %" PRId32
        ", \"tank\": %" PRId32
        ", \"pump\": %d, \"sim\": \"soil tank\", \"vib\": %s"
        ", \"vib_bad\": %d, \"bumps\": %" PRId32 ", \"bruise_at\": %s}",
        TEAM, farm->sent, knobs->where, knobs->tank,
        (farm->pump_ms != 0) ? 1 : 0, vib_text, farm->vib.bad ? 1 : 0,
        farm->route.bumps, worst_text);
    return publish_text(farm, farm->topic, report);
}

/* The flip goes out at once on the event topic. */
static bool send_vib_event(const farm_t *farm)
{
    char vib_text[BENTO_FMT_BUF_SIZE];
    char event[REPORT_BYTES];

    vib_json(&farm->vib, vib_text, sizeof(vib_text));
    (void)snprintf(event, sizeof(event),
                   "{\"id\": \"%s\", \"event\": \"vib\", \"bad\": %d"
                   ", \"vib\": %s}",
                   TEAM, farm->vib.bad ? 1 : 0, vib_text);
    return publish_text(farm, farm->topic_event, event);
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

/* The light goes off first, always; then the program ends politely. */
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
    /* DO_NOTHING and DO_BEEP are never looked up: no tune follows
     * them here */
    static const tune_t tunes[DO_COUNT] = {
        [DO_NOTHING] = TUNE_TAP, [DO_DENY] = TUNE_BAD,
        [DO_OFF] = TUNE_STOP,    [DO_ON] = TUNE_START,
        [DO_BEEP] = TUNE_HIT,    [DO_SAY] = TUNE_TAP};
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

/* The light goes off, the driver is told to slow down and the
 * seller hears of it at once. */
static bento_status_t on_vib(farm_t *farm)
{
    const char stop_word[] = "BUMP";
    bento_status_t status = matrix_off(farm);

    if ((status == BENTO_OK) && farm->vib.bad) {
        status = set_pump_led(farm, false);
        show_pump(farm, 0);
        show_note(farm, "ลังกระแทกต่อเนื่อง! แจ้งคนขับชะลอ", COL_BAD);
        if ((status == BENTO_OK) && farm->has_matrix) {
            status = bento_rgbmatrix_scroll(
                stop_word, (uint32_t)strlen(stop_word),
                BENTO_RGBMATRIX_RED, SCROLL_MS);
        }
        if (status == BENTO_OK) {
            status = beep(farm->ui, TUNE_EMPTY);
        }
    } else if (status == BENTO_OK) {
        show_note(farm, "ถนนเรียบแล้ว ขับต่อได้", COL_OK);
        status = beep(farm->ui, TUNE_GOOD);
    } else {
        /* the matrix refused: the status ends the program */
    }
    if ((status == BENTO_OK) && !send_vib_event(farm)) {
        status = stop_here(farm, "สายหลุด", COL_BAD);
    }
    return status;
}

/* One reading of the shaking; a flip switches the light off either
 * way. */
static bento_status_t check_vibration(farm_t *farm, uint32_t now_ms)
{
    bento_real_t reading = (bento_real_t)0.0;
    bento_status_t status = BENTO_OK;

    if (magnitude(&reading) && vib_push(&farm->vib, reading, now_ms)) {
        status = on_vib(farm);
        farm->pump_ms = 0;
        farm->shown = 0;
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
        (void)snprintf(text, sizeof(text), "ไฟขับ (คำสั่ง %" PRId32 ")",
                       farm->got);
        set_text(farm, farm->screen.title, text);
        read_command(payload, len, &command);
        handle_command(&command, tank, farm->vib.bad, &outcome);
        status = act_on(farm, &outcome, now_ms);
    }
    return status;
}

/* SW5 needs no network at all. */
static bento_status_t stop_on_button(farm_t *farm)
{
    static const char stop_button_name[] = "SW5"; /* BTN_NAMES[0] */
    char text[TEXT_BYTES];
    bento_status_t status = BENTO_OK;

    if (take_click(&farm->stop_button) && (farm->pump_ms != 0)) {
        farm->pump_ms = 0;
        (void)snprintf(text, sizeof(text), "ดับไฟขับปกติ %s",
                       stop_button_name);
        show_note(farm, text, COL_BAD);
        status = beep(farm->ui, TUNE_STOP);
    }
    return status;
}

static bento_status_t read_knobs(knobs_t *knobs)
{
    bento_status_t status = knob_percent(KNOB_WHERE, &knobs->where);

    if (status == BENTO_OK) {
        status = knob_percent(KNOB_TANK, &knobs->tank);
    }
    return status;
}

/* SW6 starts a new trip: the bruise bars are cleared. */
static bento_status_t new_trip_on_button(farm_t *farm, uint32_t now_ms)
{
    bento_status_t status = BENTO_OK;

    if (take_click(&farm->trip_button)) {
        route_start(&farm->route, now_ms);
        show_route(farm);
        show_note(farm, "เริ่มเที่ยวใหม่ ล้างแถบช้ำ", COL_INFO);
        status = beep(farm->ui, TUNE_TAP);
    }
    return status;
}

/* Once the base is known, every round puts the bump level on the
 * road; a new bump gets a tone. */
static bento_status_t feed_route(farm_t *farm, int32_t where)
{
    route_t *route = &farm->route;
    const int32_t bumps = route->bumps;
    const int32_t hard = route->hard;
    bool redraw = false;
    bento_status_t status = BENTO_OK;

    if (farm->vib.has_base) {
        redraw =
            (route_feed(route, severity(&farm->vib), where) >= 0) ||
            (route->bumps != bumps) || (route->hard != hard);
    }
    if (redraw) {
        show_route(farm);
    }
    if (redraw && (route->bumps != bumps)) {
        status = beep(farm->ui, TUNE_HIT);
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

/* Time is up, or the battery is flat. */
static bento_status_t end_pump_if_due(farm_t *farm, uint32_t now_ms,
                                      int32_t tank)
{
    const int32_t left_ms = pump_left_ms(farm, now_ms);
    bento_status_t status = BENTO_OK;

    if ((farm->pump_ms != 0) && ((left_ms <= 0) || (tank < TANK_MIN))) {
        farm->pump_ms = 0;
        show_note(farm,
                  (left_ms <= 0) ? "ครบเวลา ดับเอง" : "แบตหมด ดับเอง",
                  COL_INFO);
        status = beep(farm->ui, TUNE_STOP);
    }
    return status;
}

static bento_status_t show_countdown(farm_t *farm, uint32_t now_ms)
{
    /* rounded up: 0.2 s left still shows 1 */
    const int32_t sec_left =
        (farm->pump_ms != 0)
            ? (((pump_left_ms(farm, now_ms) + MS_PER_S) - 1) / MS_PER_S)
            : 0;
    bento_status_t status = set_pump_led(farm, farm->pump_ms != 0);

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
            status = stop_here(farm, "สายหลุด", COL_BAD);
        }
    }
    return status;
}

/* The second half of a round: the light, the screen, the report
 * and the link. */
static bento_status_t finish_round(farm_t *farm, uint32_t now_ms,
                                   const knobs_t *knobs)
{
    bento_status_t status = end_pump_if_due(farm, now_ms, knobs->tank);

    if (going_on(status, farm)) {
        status = show_countdown(farm, now_ms);
    }
    if (going_on(status, farm)) {
        show_trip(farm, now_ms);
        show_knobs(farm, knobs);
        status = report_if_due(farm, now_ms, knobs);
    }
    if (going_on(status, farm) && !link_is_up(farm->broker)) {
        status = stop_here(farm, "สายหลุด", COL_BAD);
    }
    if (going_on(status, farm)) {
        drain_events(farm->ui);
        status = wait_ms(farm, POLL_MS);
    }
    return status;
}

/* One round of about POLL_MS: the inbox holds one message and a
 * newer one overwrites it, so never a long sleep. */
static bento_status_t listen_once(farm_t *farm)
{
    const uint32_t now_ms = bento_time_ticks_ms();
    knobs_t knobs = {0, 0};
    bento_status_t status = stop_on_button(farm);

    if (status == BENTO_OK) {
        status = read_knobs(&knobs);
    }
    if (status == BENTO_OK) {
        status = new_trip_on_button(farm, now_ms);
    }
    if (status == BENTO_OK) {
        status = check_vibration(farm, now_ms);
    }
    if (going_on(status, farm)) {
        status = feed_route(farm, knobs.where);
    }
    if (going_on(status, farm)) {
        status = take_command(farm, knobs.tank, now_ms);
    }
    if (going_on(status, farm)) {
        status = finish_round(farm, now_ms, &knobs);
    }
    return status;
}

static bento_status_t listen(farm_t *farm)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    char text[TEXT_BYTES];
    bento_status_t status = BENTO_OK;

    farm->send_ms = start_ms;
    farm->vib.start_ms = start_ms;
    route_start(&farm->route, start_ms);
    while (going_on(status, farm) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            LISTEN_MS)) {
        status = listen_once(farm);
    }
    if (going_on(status, farm)) {
        (void)snprintf(text, sizeof(text),
                       "เลิกฟัง ได้รับ %" PRId32 " คำสั่ง", farm->got);
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
        status = stop_here(farm, "แก้ TEAM เป็นเลขกลุ่มก่อน", COL_BAD);
    }
    if (going_on(status, farm)) {
        status = connect_farm(farm, &problem);
    }
    if (going_on(status, farm) && (problem != NULL)) {
        status = stop_here(farm, problem, COL_BAD);
    }
    if (going_on(status, farm)) {
        show_link(farm, true);
        (void)snprintf(text, sizeof(text), "ฟัง %s", farm->topic_cmd);
        show_note(farm, text, COL_OK);
        status = listen(farm);
    }
    return status;
}

int main(void)
{
    farm_t farm = {0};
    bento_status_t status;

    farm.ui = bento_runtime_get_ui();
    farm.stop_button.index = STOP_BUTTON;
    farm.trip_button.index = TRIP_BUTTON;
    farm.has_matrix = bento_rgbmatrix_available();
    farm.shown = -1;
    farm.sec_shown = -1;
    (void)snprintf(farm.topic_cmd, sizeof(farm.topic_cmd), "%s/%s/cmd",
                   TOPIC_ROOT, TEAM);
    (void)snprintf(farm.topic, sizeof(farm.topic), "%s/%s/telemetry",
                   TOPIC_ROOT, TEAM);
    (void)snprintf(farm.topic_event, sizeof(farm.topic_event),
                   "%s/%s/event", TOPIC_ROOT, TEAM);
    status = run_farm(&farm);
    if (farm.has_broker) {
        (void)bento_mqtt_close(
            farm.broker); /* the finally ignores it */
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
