/*
 * idea_15_hydroponic.c -- โรงเรือนไฮโดรโปนิกส์: อากาศร้อน/ชื้นเกิน
 * ผักใบเหี่ยว มือถือรู้ทันที (Hydroponic greenhouse: too hot or too
 * damp wilts the leaves, the phone knows at once)
 *
 * ไฟล์นี้เป็นจุดเริ่มต้นของโครงงาน แม่แบบ A (idea_3_silo) โครงเน็ตเดิม
 * หน้าจอใหม่: แผนที่อากาศ 3x3 (แถว = อุณหภูมิ, คอลัมน์ = ความชื้น) ช่องที่
 * ผักอยู่ติดไฟ และ Arc ของ VPD บอร์ดคิดเอง 2 อย่าง: VPD (kPa) แรงดึงน้ำ
 * จากใบ คิดจาก T กับ RH และเวลาที่ผักเครียดสะสม ส่ง MQTT หัวข้อและคีย์
 * เหมือน sf2_04 + "vpd" (ทศนิยม 2 ตำแหน่ง) และ "stress_min" คำสั่ง ack
 * และ set t_hi เหมือนเดิม ลองเล่น: หมุน VR3 หรือเป่าลมหายใจใส่บอร์ด จน
 * ขึ้นเตือน แล้วส่ง {"cmd":"ack"} หรือกด SW5 ต้องแก้ก่อนรัน: WIFI_SSID,
 * WIFI_PASS, TEAM, CROP, TEMP_OFFSET เกณฑ์ = ค่าสาธิตในห้อง 25-30 C
 * ไม่ใช่คำแนะนำทางเกษตร
 *
 * A project starter on template A (idea_3_silo): the same network
 * code, a new screen. A 3x3 climate map (rows = temperature, columns
 * = humidity) lights the cell the vegetables are in, and an Arc shows
 * the VPD. The board works out the VPD (kPa, how hard the air pulls
 * water from the leaves) from T and RH, and the time under stress,
 * and sends the sf2_04 topic and keys plus "vpd" (2 decimals) and
 * "stress_min"; the commands are still ack and set t_hi. Try it:
 * turn VR3 or breathe on the board until the alert shows, then send
 * {"cmd":"ack"} or press SW5. Edit WIFI_SSID, WIFI_PASS, TEAM, CROP
 * and TEMP_OFFSET first. The limits are a 25-30 C classroom demo,
 * not farming advice.
 *
 * Twin of idea_15_hydroponic.py
 * (aiot-development-for-smart-farm/project/idea_15_hydroponic.py).
 * Style: CLEAN_C.md.

 * Differs from the original:
 *   - json.loads: a small reader takes "cmd" and "t_hi" from one
 *     top-level object, skipping nested values by their brackets.
 *     Text that is not one object changes nothing, as the except
 *     does; so does text only MicroPython's lenient reader takes
 *     (a missing comma or colon).
 *   - "cmd" is compared as written: a backslash escape in it is
 *     not decoded, and the payload is not checked for UTF-8.
 *   - json.dumps: the same keys and values, written in the order the
 *     .py lists them (MicroPython's dict prints its hash-slot order).
 *   - get_message() keeps one slot a newer message overwrites; the C
 *     client keeps a ring, so each round takes every waiting message
 *     and keeps the newest.
 *   - Prints: the [WiFi] and [MQTT] lines C can know; the binding's
 *     lines with radio or cy result codes are not printed.
 *   - The Stop exception is a halted flag that ends every loop; the
 *     finally is the client close and "[MQTT] Disconnected" in main.
 *   - The IDE's Stop ends the program quietly, where MicroPython
 *     raises KeyboardInterrupt.
 *   - import pots, buttons and rgbmatrix fail before anything is
 *     drawn on a board without the QWA309 base board; here the first
 *     such call ends the program with its status.
 *   - hasattr(ui, "volume"): the volume is set; a board without it
 *     answers E_UNSUPPORTED, which is skipped like the hasattr.
 *   - The power in the VPD is powf (pow on the reTerminal), the
 *     function MicroPython's ** calls at the board's float width.
 *   - In the BENTO Emulator, MQTT from C is not available yet: the
 *     program says so and ends.
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
#include "bento_ui_core.h"
#include "bento_wifi.h"

/* ---- แก้ก่อนรัน: settings (MISRA deviation CT02) ---- */
static const char WIFI_SSID[] = "<ชื่อ Hotspot ของกลุ่ม>";
static const char WIFI_PASS[] = "<รหัส Hotspot ของกลุ่ม>";
static const char TEAM[] = "teamXX";
static const char CROP[] = "ผักสลัด";
/* บอร์ดอ่านสูงกว่าห้อง: ใส่ค่าชดเชย เช่น -7.0 */
static const bento_real_t TEMP_OFFSET = (bento_real_t)0.0;

static const char BROKER[] = "broker.hivemq.com";
static const char ROOT[] = "bento-aiot";
/* CLIENT_ID = "bento-farm-" + TEAM, + a number at each connect,
 * built in open_broker() */
/* TOPIC_EVENT = ROOT/TEAM/event, built in main() */
/* TOPIC_CMD = ROOT/TEAM/cmd, built in main() */
/* ---- end of settings ---- */

enum { /* timing, in ms */
    READ_MS = 1000,
    BEEP_MS = 5000,
    POLL_MS = 20,
    RUN_MS = 600000,
    SETTLE_MS = 200, /* the cleared screen settles */
    NOTE_MS = 90,
    NOTE_GAP_MS = 100,
    CONNECT_POLL_MS = 100,
    MS_PER_S = 1000,
    SCROLL_MS = 80
};
enum { /* the broker */
    BROKER_PORT = 1883,
    KEEPALIVE_S = 60,
    BROKER_TRIES = 3,
    CLIENT_SUFFIX_MASK = 0xFFFF
};
enum { /* the farm */
    T_HI_MAX = 45,
    FAR_C = 3,   /* this far past a limit is level 2 */
    FAR_RH = 10, /* the same for humidity */
    SUN_POT = 2, /* VR3 */
    SUN_MAX_C = 10,
    POT_FULL = 4095,
    CONFIRM_BUTTON = 1, /* SW5 */
    RH_FULL = 100,
    ARC_MAX = 300, /* the VPD arc: kPa x 100 */
    KPA_TO_ARC = 100,
    GRID_SIDE = 3, /* the climate map is 3 x 3 */
    CELL_COUNT = GRID_SIDE * GRID_SIDE,
    NO_CELL = -1,
    S_PER_MIN = 60,
    CROP_COUNT = 3,
    TEAM_CHARS = 6,
    TEAM_PREFIX_CHARS = 4,
    INT_CAP = 1000000, /* any larger JSON integer is out of range */
    DECIMAL_BASE = 10
};
enum { /* sound */
    SPEAKER = 40,
    VOLUME = 25
};
enum { /* alert levels */
    LEVEL_NONE = -1,
    LEVEL_OK = 0,
    LEVEL_WARN = 1,
    LEVEL_BAD = 2,
    LEVEL_COUNT = 3
};
enum { /* one crop's limits, in this order */
    LIM_T_LO,
    LIM_T_HI,
    LIM_H_LO,
    LIM_H_HI,
    LIM_COUNT
};
enum { /* โรงเรือน: เขียวน้ำลึก ใบผัก น้ำ, 0xRRGGBB */
    COL_TEXT = 0xE6FFF4,
    COL_DIM = 0x86B8A8,
    COL_CARD = 0x0B2724,
    COL_EDGE = 0x1F5C52,
    COL_CELL = 0x14403A,
    COL_OK = 0x7CE38B,
    COL_WARN = 0xF2C14E,
    COL_BAD = 0xFF6B6B,
    COL_LEAF = 0xB8F27C,
    COL_WATER = 0x3FC1C9,
    CARD_TITLE_COLOR = COL_LEAF
};
enum { /* layout, in px */
    AUTO_PX = -1,
    BOX_X = 212,
    BOX_Y = 90,
    BOX_W = 368,
    BOX_H = 150,
    CARD_RADIUS = 14,
    CARD_BORDER = 1,
    ROW_NAME_X = 22,
    ROW_NAME_DY = 24, /* a row name sits level with its cells */
    CELL_X = 72,
    CELL_Y = 72,
    CELL_STEP_PX = 88,
    ROW_STEP_PX = 72,
    CELL_W = 82,
    CELL_H = 66,
    CELL_RADIUS = 10,
    CELL_BORDER = 1,
    COLUMN_NAME_X = 96,
    COLUMN_NAME_Y = 292,
    GRID_NAME_PX = 20,
    CARD_TITLE_DX = 12,
    CARD_TITLE_DY = 6,
    TITLE_PX = 20
};
enum {                      /* buffers, in bytes */
    TEXT_BYTES = 128,       /* a SET_TEXT carries at most 126 */
    WHY_THAI_BYTES = 64,    /* two Thai problems and " + " */
    WHY_ENGLISH_BYTES = 16, /* "cold+dry" */
    TOPIC_BYTES = 40,
    CLIENT_ID_BYTES = 24, /* 22 used, a broker takes 23 */
    PAYLOAD_BYTES = 256,
    TOPIC_KEEP = 128,  /* the binding keeps 127 bytes */
    PAYLOAD_KEEP = 255 /* and 255 of the payload */
};

/* The kept widgets, in the order build_screen() makes them. */
enum {
    SLOT_CLIMATE,
    SLOT_VPD,
    SLOT_VPD_TEXT,
    SLOT_CROP,
    SLOT_ARC,
    SLOT_MOOD,
    SLOT_STRESS,
    SLOT_REPLY,
    SLOT_STATUS,
    SLOT_MQ,
    SLOT_MQ_TEXT,
    SLOT_COUNT
};
enum { /* a widget the program never changes */
    SLOT_NONE = SLOT_COUNT
};
enum { /* the kinds of widget on this screen */
    KIND_LABEL,
    KIND_CARD, /* a Panel and its title */
    KIND_PANEL,
    KIND_SEG7,
    KIND_BAR,
    KIND_LED,
    KIND_ARC
};
enum { /* tunes */
    TUNE_TAP,
    TUNE_GOOD,
    TUNE_BAD,
    TUNE_EMPTY,
    TUNE_COUNT
};
enum { /* the longest tune */
    TUNE_NOTES_MAX = 3
};
enum { /* the Emulator notice: two lines of 20 px wrap in GAP_W */
    GAP_X = 316,
    GAP_Y = 346,
    GAP_W = 370,
    GAP_H = 52,
    GAP_PX = 20
};
enum { /* JSON values */
    VALUE_STRING,
    VALUE_NUMBER,
    VALUE_TRUE,
    VALUE_FALSE,
    VALUE_NULL,
    VALUE_NESTED
};

typedef struct {
    uint8_t kind;
    uint8_t slot;
    const char *text;
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    uint32_t color;
    int32_t min;
    int32_t max;
    int32_t value; /* value=: a font size or a start value */
} widget_spec_t;

typedef struct {
    const char *thai;
    const char *code;
    int32_t limits[LIM_COUNT];
} crop_t;

typedef struct {
    uint8_t count;
    uint8_t notes[TUNE_NOTES_MAX];
} tune_t;

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t ids[SLOT_COUNT];
    bento_ui_id_t cells[CELL_COUNT]; /* row by row, hot row first */
    int32_t painted_cell; /* NO_CELL until the first reading */

    bento_ui_id_t box;
    bool has_box;
} screen_t;

/* What the .py keeps in its Watch object. */
typedef struct {
    const crop_t *crop;
    int32_t lim[LIM_COUNT];
    int32_t level;
    bool acked;
    int32_t alerts;
    bool has_reading; /* false: read again in the next round */
    int32_t stress_s; /* seconds at level 1 or 2 */
    bento_real_t vpd;
} watch_t;

typedef struct {
    screen_t screen;
    watch_t watch;
    uint32_t broker;   /* 0 = no client */
    bool halted;       /* the .py raised Stop */
    bool emulator_gap; /* the BENTO Emulator has no MQTT from C */
    char topic_event[TOPIC_BYTES];
    char topic_cmd[TOPIC_BYTES];
} farm_t;

typedef struct {
    bento_real_t temp_c; /* with the offset and the sun */
    bento_real_t rh;
    int32_t sun_c;
} reading_t;

typedef struct {
    int32_t level;
    char why_thai[WHY_THAI_BYTES];
    char why_english[WHY_ENGLISH_BYTES];
} verdict_t;

typedef struct {
    uint32_t read_ms;
    uint32_t beep_ms;
    bool was_down;
} timers_t;

typedef struct {
    uint8_t payload[PAYLOAD_KEEP];
    uint32_t len;
} message_t;

typedef struct {
    const uint8_t *text;
    uint32_t len;
    uint32_t at;
} json_reader_t;

typedef struct {
    uint8_t kind;
    uint32_t start;
    uint32_t len;
} json_value_t;

typedef struct {
    bool has_cmd;
    json_value_t cmd;
    bool has_t_hi;
    json_value_t t_hi;
} command_t;

/* (ชื่อไทย, รหัสอังกฤษ, T ต่ำ, T สูง, RH ต่ำ, RH สูง) ค่าสาธิต: ห้อง
 * 27 C = ปกติ VR3 ครึ่งรอบ (+5) = เริ่มเครียด เป่าลมหายใจ = ชื้นเกิน */
static const crop_t CROPS[CROP_COUNT] = {
    {"ผักสลัด", "lettuce", {18, 30, 40, 85}},
    {"ผักบุ้ง", "kangkong", {20, 33, 40, 85}},
    {"คะน้า", "kale", {18, 30, 40, 85}}};

/* ---- small helpers ---- */

/* Sleeps, and turns Stop into BENTO_E_CANCELLED. */
static bento_status_t wait_ms(uint32_t duration_ms)
{
    bento_status_t status = bento_time_sleep_ms(duration_ms);

    if ((status == BENTO_OK) && !bento_runtime_is_running()) {
        status = BENTO_E_CANCELLED;
    }
    return status;
}

static bool going_on(bento_status_t status, const farm_t *farm)
{
    return (status == BENTO_OK) && !farm->halted;
}

/* The widget setters ignore a refusal, as modui.c does. */
static void set_text(const screen_t *screen, uint32_t slot,
                     const char *text)
{
    (void)bento_ui_set_text(screen->ui, screen->ids[slot], text,
                            (uint32_t)strlen(text));
}

static void set_color(const screen_t *screen, uint32_t slot,
                      uint32_t color)
{
    (void)bento_ui_set_color(screen->ui, screen->ids[slot], color);
}

static void set_value(const screen_t *screen, uint32_t slot,
                      int32_t value)
{
    (void)bento_ui_set_value(screen->ui, screen->ids[slot], value);
}

/* ui.poll() here only lets the screen catch up; it answers no
 * events when it fails. */
static void drain_events(const screen_t *screen)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    (void)bento_ui_poll(screen->ui, events, BENTO_UI_POLL_EVENTS_MAX,
                        &count, NULL);
}

static bento_status_t beep(bento_ui_t *ui, uint32_t tune)
{
    static const tune_t tunes[TUNE_COUNT] = {{1U, {76U, 0U, 0U}},
                                             {3U, {72U, 79U, 84U}},
                                             {2U, {84U, 76U, 0U}},
                                             {3U, {84U, 76U, 69U}}};
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (status == BENTO_OK) && (i < tunes[tune].count); i++) {
        /* ui.tone() sends and forgets (modui.c) */
        (void)bento_ui_sfx_tone(ui, tunes[tune].notes[i],
                                (uint8_t)BENTO_UI_WAVE_SINE,
                                (uint8_t)VOLUME, (uint16_t)NOTE_MS);
        status = wait_ms(NOTE_GAP_MS);
    }
    return status;
}

/* round(value, n): nearbyint(value * 10**n) / 10**n in the board's
 * float width (py/modbuiltins.c), a tie going to the even one. */
static bento_real_t round_scaled(bento_real_t value, bento_real_t scale)
{
    const bento_real_t half = (bento_real_t)0.5;
    const bento_real_t scaled = value * scale;
    int32_t whole = (int32_t)scaled; /* toward zero */
    const bento_real_t rest = scaled - (bento_real_t)whole;
    const bool odd = (whole % 2) != 0;

    if ((rest > half) || ((rest == half) && odd)) {
        whole++;
    } else if ((rest < -half) || ((rest == -half) && odd)) {
        whole--;
    } else {
        /* already the nearest */
    }
    return (bento_real_t)whole / scale;
}

static bento_real_t round_tenths(bento_real_t value)
{
    const bento_real_t ten = (bento_real_t)10.0;

    return round_scaled(value, ten);
}

static bento_real_t round_hundredths(bento_real_t value)
{
    const bento_real_t hundred = (bento_real_t)100.0;

    return round_scaled(value, hundred);
}

/* x ** y at the board's float width. */
static bento_real_t real_pow(bento_real_t base, bento_real_t exponent)
{
#if BENTO_FMT_REAL_DOUBLE
    return pow(base, exponent);
#else
    return powf(base, exponent);
#endif
}

/* VPD in kPa: how hard the air pulls water from the leaves. */
static bento_real_t vpd_kpa(bento_real_t temp_c, bento_real_t rh)
{
    const bento_real_t es_scale = (bento_real_t)0.6108;
    const bento_real_t base = (bento_real_t)2.718281828;
    const bento_real_t slope = (bento_real_t)17.27;
    const bento_real_t offset_c = (bento_real_t)237.3;
    const bento_real_t rise = slope * temp_c;
    const bento_real_t exponent = rise / (temp_c + offset_c);
    const bento_real_t es = es_scale * real_pow(base, exponent);
    const bento_real_t dry =
        (bento_real_t)1 - (rh / (bento_real_t)RH_FULL);

    return es * dry;
}

/* ---- 2) ฮาร์ดแวร์ ---- */

/* false when the sensor cannot be read, as the except does. */
static bool read_climate(reading_t *reading)
{
    float temp_c = 0.0F;
    float rh = 0.0F;
    bool ok = bento_sensors_sht40_temperature(&temp_c) == BENTO_OK;

    ok = ok && (bento_sensors_sht40_humidity(&rh) == BENTO_OK);
    if (ok) {
        reading->temp_c = (bento_real_t)temp_c + TEMP_OFFSET;
        reading->rh = (bento_real_t)rh;
    }
    return ok;
}

/* ---- 3) สมอง ---- */

/* "team" and two digits, never team00: another team's name would
 * knock that team off the broker. */
static bool valid_team(void)
{
    bool valid =
        (strlen(TEAM) == (size_t)TEAM_CHARS) &&
        (strncmp(TEAM, "team", (size_t)TEAM_PREFIX_CHARS) == 0) &&
        (strcmp(TEAM, "team00") != 0);
    size_t i;

    for (i = (size_t)TEAM_PREFIX_CHARS;
         valid && (i < (size_t)TEAM_CHARS); i++) {
        valid = (TEAM[i] >= '0') && (TEAM[i] <= '9');
    }
    return valid;
}

/* The index of CROP in CROPS, or -1 when it is none of them. */
static int32_t find_crop(void)
{
    int32_t found = -1;
    int32_t i;

    for (i = 0; (found < 0) && (i < CROP_COUNT); i++) {
        if (strcmp(CROPS[i].thai, CROP) == 0) {
            found = i;
        }
    }
    return found;
}

static void append(char *text, size_t size, const char *separator,
                   const char *part)
{
    const size_t used = strlen(text);

    (void)snprintf(&text[used], size - used, "%s%s",
                   (used > 0U) ? separator : "", part);
}

/* The level 0-2 and the problems in Thai and English; level 2 is
 * far out (FAR_C past a temperature limit or FAR_RH past RH). */
static void judge(const reading_t *reading, const int32_t *lim,
                  verdict_t *verdict)
{
    static const char *const thai[LIM_COUNT] = {"หนาวไป", "โรงร้อน",
                                                "แห้งไป", "ชื้นเกิน"};
    static const char *const english[LIM_COUNT] = {"cold", "hot", "dry",
                                                   "wet"};
    const bento_real_t temp_c = reading->temp_c;
    const bento_real_t rh = reading->rh;
    bool out[LIM_COUNT];
    bool far;
    uint32_t i;

    out[LIM_T_LO] = temp_c < (bento_real_t)lim[LIM_T_LO];
    out[LIM_T_HI] = temp_c > (bento_real_t)lim[LIM_T_HI];
    out[LIM_H_LO] = rh < (bento_real_t)lim[LIM_H_LO];
    out[LIM_H_HI] = rh > (bento_real_t)lim[LIM_H_HI];
    far = (temp_c < (bento_real_t)(lim[LIM_T_LO] - FAR_C)) ||
          (temp_c > (bento_real_t)(lim[LIM_T_HI] + FAR_C)) ||
          (rh < (bento_real_t)(lim[LIM_H_LO] - FAR_RH)) ||
          (rh > (bento_real_t)(lim[LIM_H_HI] + FAR_RH));
    verdict->why_thai[0] = '\0';
    verdict->why_english[0] = '\0';
    for (i = 0U; i < (uint32_t)LIM_COUNT; i++) {
        if (out[i]) {
            append(verdict->why_thai, sizeof(verdict->why_thai), " + ",
                   thai[i]);
            append(verdict->why_english, sizeof(verdict->why_english),
                   "+", english[i]);
        }
    }
    if (verdict->why_english[0] == '\0') {
        verdict->level = LEVEL_OK;
        (void)snprintf(verdict->why_thai, sizeof(verdict->why_thai),
                       "สบายดี");
        (void)snprintf(verdict->why_english,
                       sizeof(verdict->why_english), "ok");
    } else {
        verdict->level = far ? LEVEL_BAD : LEVEL_WARN;
    }
}

/* ---- JSON: the two fields on_command() asks for ---- */

static bool at_end(const json_reader_t *reader)
{
    return reader->at >= reader->len;
}

static uint8_t peek(const json_reader_t *reader)
{
    return at_end(reader) ? 0U : reader->text[reader->at];
}

static bool is_space(uint8_t byte)
{
    return (byte == (uint8_t)' ') || (byte == (uint8_t)'\t') ||
           (byte == (uint8_t)'\n') || (byte == (uint8_t)'\r');
}

static bool is_digit(uint8_t byte)
{
    return (byte >= (uint8_t)'0') && (byte <= (uint8_t)'9');
}

static bool is_token_byte(uint8_t byte)
{
    return is_digit(byte) ||
           ((byte >= (uint8_t)'a') && (byte <= (uint8_t)'z')) ||
           (byte == (uint8_t)'E') || (byte == (uint8_t)'.') ||
           (byte == (uint8_t)'+') || (byte == (uint8_t)'-');
}

static void skip_space(json_reader_t *reader)
{
    while (!at_end(reader) && is_space(peek(reader))) {
        reader->at++;
    }
}

/* Takes byte after any spaces; false when something else is there. */
static bool take_byte(json_reader_t *reader, uint8_t byte)
{
    bool taken;

    skip_space(reader);
    taken = !at_end(reader) && (peek(reader) == byte);
    if (taken) {
        reader->at++;
    }
    return taken;
}

/* A string from its opening quote; value holds what is inside. */
static bool read_string(json_reader_t *reader, json_value_t *value)
{
    bool closed = false;

    reader->at++;
    value->kind = VALUE_STRING;
    value->start = reader->at;
    while (!closed && !at_end(reader)) {
        const uint8_t byte = peek(reader);

        if (byte == (uint8_t)'\\') {
            reader->at += 2U; /* the escaped byte is never the end */
        } else {
            closed = byte == (uint8_t)'"';
            reader->at++;
        }
    }
    value->len = closed ? ((reader->at - value->start) - 1U) : 0U;
    return closed;
}

/* One bracket byte: a { or [ goes one level in, a } or ] one out
 * (either closes either kind, as MicroPython's reader allows). */
static void step_bracket(uint8_t byte, uint32_t *depth, bool *done)
{
    if ((byte == (uint8_t)'{') || (byte == (uint8_t)'[')) {
        (*depth)++;
    } else if ((byte == (uint8_t)'}') || (byte == (uint8_t)']')) {
        (*depth)--;
        *done = *depth == 0U;
    } else {
        /* a byte inside the nested value */
    }
}

/* A list or object inside the command: skipped by its brackets. */
static bool skip_nested(json_reader_t *reader, json_value_t *value)
{
    json_value_t inner;
    uint32_t depth = 0U;
    bool ok = true;
    bool done = false;

    value->kind = VALUE_NESTED;
    while (ok && !done) {
        if (at_end(reader)) {
            ok = false;
        } else if (peek(reader) == (uint8_t)'"') {
            ok = read_string(reader, &inner);
        } else {
            step_bracket(peek(reader), &depth, &done);
            reader->at++;
        }
    }
    return ok;
}

/* The len bytes at text are exactly word (text needs no NUL). */
static bool bytes_are(const uint8_t *text, uint32_t len,
                      const char *word)
{
    bool same = len == (uint32_t)strlen(word);
    uint32_t i;

    for (i = 0U; same && (i < len); i++) {
        same = text[i] == (uint8_t)word[i];
    }
    return same;
}

static bool is_word(const json_reader_t *reader,
                    const json_value_t *value, const char *word)
{
    return bytes_are(&reader->text[value->start], value->len, word);
}

/* Moves past digits; false when there is none. */
static bool skip_digits(const uint8_t *text, uint32_t len, uint32_t *at)
{
    const uint32_t first = *at;

    while ((*at < len) && is_digit(text[*at])) {
        (*at)++;
    }
    return *at > first;
}

/* -?digits(.digits)?([eE][+-]?digits)? and nothing else. */
static bool is_number(const json_reader_t *reader,
                      const json_value_t *value)
{
    const uint8_t *text = &reader->text[value->start];
    const uint32_t len = value->len;
    uint32_t at = ((len > 0U) && (text[0] == (uint8_t)'-')) ? 1U : 0U;
    bool ok = skip_digits(text, len, &at);

    if (ok && (at < len) && (text[at] == (uint8_t)'.')) {
        at++;
        ok = skip_digits(text, len, &at);
    }
    if (ok && (at < len) &&
        ((text[at] == (uint8_t)'e') || (text[at] == (uint8_t)'E'))) {
        at++;
        if ((at < len) && ((text[at] == (uint8_t)'+') ||
                           (text[at] == (uint8_t)'-'))) {
            at++;
        }
        ok = skip_digits(text, len, &at);
    }
    return ok && (at == len);
}

/* A number, true, false or null. */
static bool read_token(json_reader_t *reader, json_value_t *value)
{
    bool ok = true;

    value->start = reader->at;
    while (!at_end(reader) && is_token_byte(peek(reader))) {
        reader->at++;
    }
    value->len = reader->at - value->start;
    if (is_word(reader, value, "true")) {
        value->kind = VALUE_TRUE;
    } else if (is_word(reader, value, "false")) {
        value->kind = VALUE_FALSE;
    } else if (is_word(reader, value, "null")) {
        value->kind = VALUE_NULL;
    } else {
        value->kind = VALUE_NUMBER;
        ok = is_number(reader, value);
    }
    return ok;
}

static bool read_value(json_reader_t *reader, json_value_t *value)
{
    bool ok;

    skip_space(reader);
    if (peek(reader) == (uint8_t)'"') {
        ok = read_string(reader, value);
    } else if ((peek(reader) == (uint8_t)'{') ||
               (peek(reader) == (uint8_t)'[')) {
        ok = skip_nested(reader, value);
    } else {
        ok = read_token(reader, value);
    }
    return ok;
}

/* "key": value; a later "cmd" or "t_hi" wins, as in a dict. */
static bool read_member(json_reader_t *reader, command_t *command)
{
    json_value_t key;
    json_value_t value;
    bool ok;

    skip_space(reader);
    ok = (peek(reader) == (uint8_t)'"') && read_string(reader, &key);
    ok = ok && take_byte(reader, (uint8_t)':');
    ok = ok && read_value(reader, &value);
    if (ok && is_word(reader, &key, "cmd")) {
        command->cmd = value;
        command->has_cmd = true;
    } else if (ok && is_word(reader, &key, "t_hi")) {
        command->t_hi = value;
        command->has_t_hi = true;
    } else {
        /* a key the program does not ask for */
    }
    return ok;
}

/* json.loads of one object: false when the text is anything else. */
static bool read_command(const message_t *message, command_t *command)
{
    json_reader_t reader = {message->payload, message->len, 0U};
    bool ok = take_byte(&reader, (uint8_t)'{');
    bool closed = ok && take_byte(&reader, (uint8_t)'}');

    (void)memset(command, 0, sizeof(*command));
    while (ok && !closed) {
        ok = read_member(&reader, command);
        closed = ok && take_byte(&reader, (uint8_t)'}');
        if (ok && !closed) {
            ok = take_byte(&reader, (uint8_t)',');
        }
    }
    skip_space(&reader);
    return ok && at_end(&reader);
}

/* isinstance(v, int): an integer, or true and false (bool is an
 * int); one past INT_CAP stands for any larger value. */
static bool value_as_int(const message_t *message,
                         const json_value_t *value, int32_t *number)
{
    const uint8_t *text = &message->payload[value->start];
    const bool negative =
        (value->len > 0U) && (text[0] == (uint8_t)'-');
    bool is_int = value->kind == VALUE_NUMBER;
    int32_t magnitude = 0;
    uint32_t i;

    for (i = negative ? 1U : 0U; is_int && (i < value->len); i++) {
        is_int = is_digit(text[i]);
        if (is_int && (magnitude <= INT_CAP)) {
            magnitude = (magnitude * DECIMAL_BASE) + (int32_t)text[i] -
                        (int32_t)'0';
        }
    }
    *number = negative ? -magnitude : magnitude;
    if (value->kind == VALUE_TRUE) {
        is_int = true;
        *number = 1;
    } else if (value->kind == VALUE_FALSE) {
        is_int = true;
        *number = 0;
    } else {
        /* a number, or no integer at all */
    }
    return is_int;
}

static bool cmd_is(const message_t *message, const command_t *command,
                   const char *word)
{
    const json_value_t *cmd = &command->cmd;

    return command->has_cmd && (cmd->kind == VALUE_STRING) &&
           bytes_are(&message->payload[cmd->start], cmd->len, word);
}

/* ---- 4) เครือข่าย ---- */

/* wifi.connect(): *joined is its True or False. A refused or
 * timed-out join is False; any other failure ends the program, as
 * the binding's OSError does. */
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
    (void)printf("[WiFi] Connecting...\r\n");
    status = bento_wifi_connect_start(&params);
    if (status == BENTO_OK) {
        status = BENTO_E_IN_PROGRESS;
    }
    while (status == BENTO_E_IN_PROGRESS) {
        status = wait_ms(CONNECT_POLL_MS);
        if (status == BENTO_OK) {
            status = bento_wifi_connect_poll();
        } else { /* Stop between two polls: leave the radio alone */
            (void)bento_wifi_connect_cancel(); /* nothing to report */
        }
    }
    *joined = status == BENTO_OK;
    if ((status == BENTO_E_FAILED) || (status == BENTO_E_TIMEOUT)) {
        status = BENTO_OK;
    }
    return status;
}

/* wifi.ip() is "0.0.0.0" until DHCP answers; so is an address that
 * cannot be read. Prints the line wifi.connect() prints. */
static bool has_address(void)
{
    char address[BENTO_WIFI_IPV4_TEXT_MAX];
    uint32_t ip_v4 = 0U;
    uint32_t length = 0U;

    if (bento_wifi_ip(BENTO_WIFI_IF_STA, &ip_v4) != BENTO_OK) {
        ip_v4 = 0U;
    }
    /* a BENTO_WIFI_IPV4_TEXT_MAX buffer holds any address */
    (void)bento_wifi_ipv4_format(ip_v4, address,
                                 (uint32_t)sizeof(address), &length);
    (void)printf("[WiFi] Connected! IP=%s\r\n", address);
    return ip_v4 != 0U;
}

/* One mqtt.connect() with a fresh client id: *linked is its True or
 * False. A client that did not link is closed, as the binding does. */
static bento_status_t open_broker(farm_t *farm, bool *linked)
{
    char client_id[CLIENT_ID_BYTES];
    bento_mqtt_config_t config;
    bento_status_t status;

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
    (void)printf("[MQTT] Connecting to %s:%d ...\r\n", BROKER,
                 BROKER_PORT);
    status = bento_mqtt_open_start(&config, &farm->broker);
    farm->emulator_gap = status == BENTO_E_UNSUPPORTED;
    if (status == BENTO_OK) {
        status = BENTO_E_IN_PROGRESS;
    }
    while (status == BENTO_E_IN_PROGRESS) {
        status = wait_ms(CONNECT_POLL_MS);
        if (status == BENTO_OK) {
            status = bento_mqtt_open_poll(farm->broker);
        }
    }
    *linked = status == BENTO_OK;
    if (*linked) {
        (void)printf("[MQTT] Connected to %s:%d as '%s'\r\n", BROKER,
                     BROKER_PORT, client_id);
    } else if (farm->broker != 0U) {
        (void)bento_mqtt_close(farm->broker); /* freed either way */
        farm->broker = 0U;
    } else {
        /* no client started */
    }
    return (status == BENTO_E_CANCELLED) ? status : BENTO_OK;
}

static void show_status(const screen_t *screen, const char *text,
                        uint32_t color)
{
    set_color(screen, SLOT_STATUS, color);
    set_text(screen, SLOT_STATUS, text);
}

/* The public broker does not answer now and then: three tries. */
static bento_status_t connect_broker(farm_t *farm, bool *linked)
{
    char text[TEXT_BYTES];
    int32_t attempt;
    bento_status_t status = BENTO_OK;

    *linked = false;
    for (attempt = 1; (status == BENTO_OK) && !*linked &&
                      !farm->emulator_gap && (attempt <= BROKER_TRIES);
         attempt++) {
        if (attempt > 1) {
            (void)snprintf(text, sizeof(text),
                           "ลองต่อ broker ใหม่ %" PRId32 "/3", attempt);
            show_status(&farm->screen, text, COL_WARN);
            drain_events(&farm->screen);
        }
        status = open_broker(farm, linked);
    }
    return status;
}

/* mqtt.subscribe(): True or False. A lost link raises there, which
 * nothing catches: that status ends the program here. */
static bento_status_t subscribe_cmd(const farm_t *farm,
                                    bool *subscribed)
{
    uint8_t granted_qos = 0U;
    bento_status_t status = bento_mqtt_subscribe(
        farm->broker, farm->topic_cmd,
        (uint32_t)strlen(farm->topic_cmd), 0U, &granted_qos);

    *subscribed = status == BENTO_OK;
    if (status == BENTO_OK) {
        (void)printf("[MQTT] Subscribed to '%s' (QoS=%d)\r\n",
                     farm->topic_cmd, (int)granted_qos);
    } else if (status == BENTO_E_FAILED) {
        (void)printf("[MQTT] subscribe rejected by broker\r\n");
    } else {
        /* a failure the binding prints with a cy code */
    }
    return (status == BENTO_E_SESSION) ? status : BENTO_OK;
}

/* The BENTO Emulator answers MQTT from C with E_UNSUPPORTED: one
 * label (two lines, as the Emulator does not wrap) and one console
 * line say so, then the run stops as for a broker that does not
 * answer. */
static bento_status_t show_emulator_gap(const screen_t *screen)
{
    static const char notice[] =
        "MQTT จาก C ยังไม่มีใน BENTO Emulator\nให้รันบน Dev Kit หรือ Eva Kit";
    static const char console_line[] =
        "MQTT จาก C ยังไม่มีใน BENTO Emulator ให้รันบน Dev Kit หรือ Eva Kit";
    bento_ui_widget_args_t args;
    bento_ui_id_t label = 0U;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.text = notice;
    args.text_len = (uint32_t)strlen(notice);
    args.x = GAP_X;
    args.y = GAP_Y;
    args.w = GAP_W;
    args.h = GAP_H;
    args.color = COL_BAD;
    args.font_px = GAP_PX;
    status = bento_ui_label_create(screen->ui, &args, &label);
    (void)printf(
        "%s / MQTT from C is not in the BENTO Emulator yet: run "
        "it on a Dev Kit or Eva Kit.\n",
        console_line);
    drain_events(screen);
    return status;
}

/* WiFi, an address, the broker and the subscribe; *problem says
 * which step failed, or stays NULL. */
static bento_status_t connect_farm(farm_t *farm, const char **problem)
{
    bool joined = false;
    bool linked = false;
    bool subscribed = false;
    bento_status_t status;

    *problem = NULL;
    show_status(&farm->screen, "ต่อ WiFi...", COL_WARN);
    drain_events(&farm->screen); /* the line shows before the wait */
    status = join_wifi(&joined);
    if ((status == BENTO_OK) && (!joined || !has_address())) {
        *problem = "WiFi ไม่ติด: ตรวจชื่อ/รหัส";
    }
    if ((status == BENTO_OK) && (*problem == NULL)) {
        status = connect_broker(farm, &linked);
    }
    if ((status == BENTO_OK) && farm->emulator_gap) {
        status = show_emulator_gap(&farm->screen);
    }
    if ((status == BENTO_OK) && linked) {
        status = subscribe_cmd(farm, &subscribed);
    }
    if ((status == BENTO_OK) && (*problem == NULL) && !subscribed) {
        *problem = "broker ไม่ตอบ: รอ 1 นาทีแล้วรันใหม่";
    }
    return status;
}

/* get_message(): every message waiting, keeping the newest. */
static bool take_newest(uint32_t broker, message_t *message)
{
    char topic[TOPIC_KEEP]; /* the program never reads the topic */
    bento_mqtt_msg_t info;
    bool found = false;
    bool more = true;

    while (more) {
        bento_mqtt_msg_init(&info);
        more =
            bento_mqtt_poll(broker, &info, topic, sizeof(topic),
                            message->payload, PAYLOAD_KEEP) == BENTO_OK;
        if (more) {
            message->len = info.payload_len;
            found = true;
        }
    }
    return found;
}

/* ---- 5) หน้าจอ ---- */

static void fill_args(bento_ui_widget_args_t *args,
                      const widget_spec_t *spec)
{
    bento_ui_widget_args_init(args);
    if (spec->text != NULL) {
        args->text = spec->text;
        args->text_len = (uint32_t)strlen(spec->text);
    }
    args->x = spec->x;
    args->y = spec->y;
    args->w = spec->w;
    args->h = spec->h;
    args->color = spec->color;
    args->min_val = spec->min;
    args->max_val = spec->max;
    args->font_px = spec->value;
}

/* A card: a rounded Panel, then its title inside the top left. */
static bento_status_t add_card(bento_ui_t *ui,
                               const widget_spec_t *spec)
{
    bento_ui_widget_args_t args;
    bento_ui_id_t id = 0U;
    bento_status_t status;

    fill_args(&args, spec);
    args.text = NULL;
    args.text_len = 0U;
    status = bento_ui_panel_create(ui, &args, &id);
    if (status == BENTO_OK) {
        bento_ui_widget_args_init(&args);
        args.text = spec->text;
        args.text_len = (uint32_t)strlen(spec->text);
        args.x = (int16_t)(spec->x + CARD_TITLE_DX);
        args.y = (int16_t)(spec->y + CARD_TITLE_DY);
        args.color = CARD_TITLE_COLOR;
        args.font_px = TITLE_PX;
        status = bento_ui_label_create(ui, &args, &id);
    }
    return status;
}

static bento_status_t
add_widget(bento_ui_t *ui, const widget_spec_t *spec, bento_ui_id_t *id)
{
    bento_ui_widget_args_t args;
    bento_status_t status;

    fill_args(&args, spec);
    switch (spec->kind) {
    case KIND_CARD:
        status = add_card(ui, spec);
        break;
    case KIND_SEG7:
        status = bento_ui_seg7_create(ui, &args, id);
        break;
    case KIND_PANEL:
        status = bento_ui_panel_create(ui, &args, id);
        break;
    case KIND_BAR: /* made in the theme colour, then .color() */
        args.color = 0U;
        status = bento_ui_bar_create(ui, &args, id);
        if ((status == BENTO_OK) && (spec->color != 0U)) {
            (void)bento_ui_set_color(ui, *id, spec->color); /* setter */
        }
        break;
    case KIND_LED:
        status = bento_ui_led_create(ui, &args, id);
        break;
    case KIND_ARC: /* made in the theme colour, then .color() */
        args.color = 0U;
        status = bento_ui_arc_create(ui, &args, id);
        if (status == BENTO_OK) {
            (void)bento_ui_set_color(ui, *id, spec->color); /* setter */
        }
        break;
    default:
        status = bento_ui_label_create(ui, &args, id);
        break;
    }
    return status;
}

/* The screen above the climate map, in the .py's order. */
static const widget_spec_t *top_widgets(size_t *count)
{
    static const widget_spec_t widgets[] = {
        {KIND_LABEL, SLOT_NONE, "ไฮโดรโปนิกส์  แผนที่อากาศของผัก", 12, 4,
         AUTO_PX, AUTO_PX, COL_LEAF, 0, 0, 24},
        {KIND_CARD, SLOT_NONE, "ผักอยู่ช่องไหน", 12, 40, 330, 300, COL_CARD,
         COL_EDGE, CARD_RADIUS, CARD_BORDER}};

    *count = sizeof(widgets) / sizeof(widgets[0]);
    return widgets;
}

/* The screen after the map; card() is one KIND_CARD row. */
static const widget_spec_t *bottom_widgets(size_t *count)
{
    static const widget_spec_t widgets[] = {
        {KIND_LABEL, SLOT_NONE, "แถว = อุณหภูมิ  คอลัมน์ = ความชื้น", 24, 316,
         AUTO_PX, AUTO_PX, COL_DIM, 0, 0, 20},
        {KIND_CARD, SLOT_NONE, "อากาศในโรงเรือน", 352, 40, 428, 150,
         COL_CARD, COL_EDGE, CARD_RADIUS, CARD_BORDER},
        {KIND_LABEL, SLOT_CLIMATE, "-- C  -- %", 366, 70, AUTO_PX,
         AUTO_PX, COL_TEXT, 0, 0, 24},
        {KIND_LABEL, SLOT_VPD, "VPD -.-- kPa", 366, 104, AUTO_PX,
         AUTO_PX, COL_WATER, 0, 0, 20},
        {KIND_LABEL, SLOT_VPD_TEXT, "-", 366, 128, AUTO_PX, AUTO_PX,
         COL_DIM, 0, 0, 20},
        {KIND_LABEL, SLOT_CROP, "-", 366, 156, AUTO_PX, AUTO_PX,
         COL_DIM, 0, 0, 20},
        {KIND_ARC, SLOT_ARC, NULL, 650, 62, 116, 116, COL_WATER, 0,
         ARC_MAX, 0},
        {KIND_CARD, SLOT_NONE, "ผักเครียดไหม", 352, 200, 428, 110,
         COL_CARD, COL_EDGE, CARD_RADIUS, CARD_BORDER},
        {KIND_LABEL, SLOT_MOOD, "...", 366, 226, AUTO_PX, AUTO_PX,
         COL_WARN, 0, 0, 20},
        {KIND_LABEL, SLOT_STRESS, "เครียดสะสม 00:00", 366, 252, AUTO_PX,
         AUTO_PX, COL_TEXT, 0, 0, 20},
        {KIND_LABEL, SLOT_REPLY, "-", 366, 278, AUTO_PX, AUTO_PX,
         COL_DIM, 0, 0, 20},
        {KIND_LABEL, SLOT_NONE,
         "VR3 = แดดบนหลังคา  เป่าลม = ชื้นขึ้น  SW5 = รับทราบ", 352, 312,
         AUTO_PX, AUTO_PX, COL_DIM, 0, 0, 20},
        {KIND_LABEL, SLOT_STATUS, "กำลังเริ่ม", 12, 356, AUTO_PX, AUTO_PX,
         COL_DIM, 0, 0, 20},
        {KIND_LED, SLOT_MQ, NULL, 600, 10, 16, 16, COL_OK, 0, 0, 0},
        {KIND_LABEL, SLOT_MQ_TEXT, "MQTT: ออฟไลน์", 624, 8, AUTO_PX,
         AUTO_PX, COL_DIM, 0, 0, 20}};

    *count = sizeof(widgets) / sizeof(widgets[0]);
    return widgets;
}

/* Makes the rows of a table, keeping the ids of the kept ones. */
static bento_status_t
add_table(screen_t *screen, const widget_spec_t *table, size_t count)
{
    bento_ui_id_t unkept = 0U;
    bento_status_t status = BENTO_OK;
    size_t i;

    for (i = 0U; (status == BENTO_OK) && (i < count); i++) {
        const uint8_t slot = table[i].slot;

        status = add_widget(screen->ui, &table[i],
                            (slot == (uint8_t)SLOT_NONE)
                                ? &unkept
                                : &screen->ids[slot]);
    }
    return status;
}

/* A widget spec with no text, no range and the theme's value. */
static void start_spec(widget_spec_t *spec, uint8_t kind, int16_t x,
                       int16_t y)
{
    (void)memset(spec, 0, sizeof(*spec));
    spec->kind = kind;
    spec->slot = (uint8_t)SLOT_NONE;
    spec->x = x;
    spec->y = y;
    spec->w = AUTO_PX;
    spec->h = AUTO_PX;
}

/* A dim grid name: a row's temperature or a column's humidity. */
static bento_status_t add_grid_name(screen_t *screen, const char *text,
                                    int32_t x, int32_t y)
{
    widget_spec_t spec;

    start_spec(&spec, KIND_LABEL, (int16_t)x, (int16_t)y);
    spec.text = text;
    spec.color = COL_DIM;
    spec.value = GRID_NAME_PX;
    return add_table(screen, &spec, 1U);
}

/* One row of the map: its name, then its three cells. */
static bento_status_t add_grid_row(screen_t *screen, int32_t row)
{
    static const char *const row_names[GRID_SIDE] = {"ร้อน", "พอดี",
                                                     "หนาว"};
    const int32_t y = CELL_Y + (row * ROW_STEP_PX);
    widget_spec_t spec;
    int32_t column;
    bento_status_t status = add_grid_name(screen, row_names[row],
                                          ROW_NAME_X, y + ROW_NAME_DY);

    for (column = 0; (status == BENTO_OK) && (column < GRID_SIDE);
         column++) {
        start_spec(&spec, KIND_PANEL,
                   (int16_t)(CELL_X + (column * CELL_STEP_PX)),
                   (int16_t)y);
        spec.w = CELL_W;
        spec.h = CELL_H;
        spec.color = COL_CELL;
        spec.min = COL_EDGE;
        spec.max = CELL_RADIUS;
        spec.value = CELL_BORDER;
        status = add_widget(screen->ui, &spec,
                            &screen->cells[(row * GRID_SIDE) + column]);
    }
    return status;
}

/* The 3 x 3 climate map with its row and column names. */
static bento_status_t add_grid(screen_t *screen)
{
    static const char *const column_names[GRID_SIDE] = {"แห้ง", "พอดี",
                                                        "ชื้น"};
    int32_t i;
    bento_status_t status = BENTO_OK;

    for (i = 0; (status == BENTO_OK) && (i < GRID_SIDE); i++) {
        status = add_grid_row(screen, i);
    }
    for (i = 0; (status == BENTO_OK) && (i < GRID_SIDE); i++) {
        status = add_grid_name(screen, column_names[i],
                               COLUMN_NAME_X + (i * CELL_STEP_PX),
                               COLUMN_NAME_Y);
    }
    return status;
}

static bento_status_t build_screen(screen_t *screen)
{
    const widget_spec_t *table = NULL;
    size_t count = 0U;
    bento_status_t status = bento_ui_screen_default(screen->ui);

    screen->painted_cell = NO_CELL;
    if (status == BENTO_OK) {
        status = wait_ms(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        table = top_widgets(&count);
        status = add_table(screen, table, count);
    }
    if (status == BENTO_OK) {
        status = add_grid(screen);
    }
    if (status == BENTO_OK) {
        table = bottom_widgets(&count);
        status = add_table(screen, table, count);
    }
    if (status == BENTO_OK) {
        drain_events(screen);
    }
    return status;
}

static void show_link(const screen_t *screen, bool linked)
{
    set_value(screen, SLOT_MQ, linked ? 1 : 0);
    set_text(screen, SLOT_MQ_TEXT,
             linked ? "MQTT: ต่อแล้ว" : "MQTT: ออฟไลน์");
}

static void show_reply(const screen_t *screen, const watch_t *watch,
                       const char *text, uint32_t color)
{
    char line[TEXT_BYTES];

    (void)snprintf(line, sizeof(line), "แจ้งแล้ว %" PRId32 " ครั้ง  %s",
                   watch->alerts, text);
    set_color(screen, SLOT_REPLY, color);
    set_text(screen, SLOT_REPLY, line);
}

static void show_crop(const screen_t *screen, const watch_t *watch)
{
    char line[TEXT_BYTES];

    (void)snprintf(
        line, sizeof(line),
        "%s ชอบ %" PRId32 "-%" PRId32 " C, %" PRId32 "-%" PRId32 " %%",
        watch->crop->thai, watch->lim[LIM_T_LO], watch->lim[LIM_T_HI],
        watch->lim[LIM_H_LO], watch->lim[LIM_H_HI]);
    set_text(screen, SLOT_CROP, line);
}

/* The map cell: the row by temperature (hot first), the column by
 * humidity (dry first). */
static int32_t cell_of(const reading_t *reading, const int32_t *lim)
{
    int32_t row = 1;
    int32_t column = 1;

    if (reading->temp_c > (bento_real_t)lim[LIM_T_HI]) {
        row = 0;
    } else if (reading->temp_c < (bento_real_t)lim[LIM_T_LO]) {
        row = 2;
    } else {
        /* the middle row */
    }
    if (reading->rh < (bento_real_t)lim[LIM_H_LO]) {
        column = 0;
    } else if (reading->rh > (bento_real_t)lim[LIM_H_HI]) {
        column = 2;
    } else {
        /* the middle column */
    }
    return (row * GRID_SIDE) + column;
}

/* The air card: T and RH, the VPD, what it means and the arc. */
static void show_air(const screen_t *screen, const watch_t *watch,
                     const reading_t *reading)
{
    const bento_real_t vpd_lo = (bento_real_t)0.4; /* ค่าสาธิต */
    const bento_real_t vpd_hi = (bento_real_t)1.6;
    const bento_real_t arc = watch->vpd * (bento_real_t)KPA_TO_ARC;
    char real[BENTO_FMT_BUF_SIZE];
    char line[TEXT_BYTES];
    uint32_t length = 0U;

    /* a BENTO_FMT_BUF_SIZE buffer always fits */
    (void)bento_fmt_real_precision(real, sizeof(real), reading->temp_c,
                                   1, &length);
    (void)snprintf(line, sizeof(line), "%s C  %" PRId32 " %%", real,
                   (int32_t)reading->rh);
    set_text(screen, SLOT_CLIMATE, line);
    (void)bento_fmt_real_precision(real, sizeof(real), watch->vpd, 2,
                                   &length);
    (void)snprintf(line, sizeof(line), "VPD %s kPa  (แดด +%" PRId32 ")",
                   real, reading->sun_c);
    set_text(screen, SLOT_VPD, line);
    if (watch->vpd < vpd_lo) {
        set_text(screen, SLOT_VPD_TEXT, "อับชื้น เสี่ยงเน่าโคน");
    } else if (watch->vpd > vpd_hi) {
        set_text(screen, SLOT_VPD_TEXT, "อากาศดึงน้ำจากใบแรง ใบเหี่ยว");
    } else {
        set_text(screen, SLOT_VPD_TEXT, "ใบคายน้ำพอดี");
    }
    set_value(screen, SLOT_ARC,
              (arc < (bento_real_t)ARC_MAX) ? (int32_t)arc : ARC_MAX);
}

/* The map: the old cell back to plain, the vegetables' cell in the
 * level's colour. */
static void show_cell(screen_t *screen, int32_t cell, uint32_t color)
{
    /* the colour setter's refusal was ignored before too */
    if ((cell != screen->painted_cell) && (screen->painted_cell >= 0)) {
        (void)bento_ui_set_color(
            screen->ui, screen->cells[screen->painted_cell], COL_CELL);
    }
    screen->painted_cell = cell;
    (void)bento_ui_set_color(screen->ui, screen->cells[cell], color);
}

/* The stress card: the mood and the time under stress. */
static void show_stress(const screen_t *screen, const watch_t *watch,
                        const verdict_t *verdict, uint32_t color)
{
    static const char *const moods[LEVEL_COUNT] = {
        "ผักสบาย โตดี", "เริ่มเครียด: ", "ผักเครียดหนัก: "};
    char clock[TEXT_BYTES];
    char line[TEXT_BYTES];

    set_color(screen, SLOT_MOOD, color);
    (void)snprintf(line, sizeof(line), "%s%s", moods[verdict->level],
                   (verdict->level == LEVEL_OK) ? ""
                                                : verdict->why_thai);
    set_text(screen, SLOT_MOOD, line);
    (void)snprintf(clock, sizeof(clock), "%02" PRId32 ":%02" PRId32,
                   watch->stress_s / S_PER_MIN,
                   watch->stress_s % S_PER_MIN);
    (void)snprintf(line, sizeof(line), "เครียดสะสม %s", clock);
    set_text(screen, SLOT_STRESS, line);
}

static void show_reading(screen_t *screen, const watch_t *watch,
                         const reading_t *reading,
                         const verdict_t *verdict)
{
    static const uint32_t level_colors[LEVEL_COUNT] = {COL_OK, COL_WARN,
                                                       COL_BAD};
    const uint32_t color = level_colors[verdict->level];

    show_air(screen, watch, reading);
    show_cell(screen, cell_of(reading, watch->lim), color);
    show_stress(screen, watch, verdict, color);
}

/* The box is deleted by message only (modui.c sends and forgets). */
static void close_box(screen_t *screen)
{
    if (screen->has_box) {
        (void)bento_ui_delete(screen->ui, screen->box);
        screen->has_box = false;
    }
}

/* A MsgBox takes 126 bytes at most. */
static bento_status_t open_box(screen_t *screen,
                               const verdict_t *verdict)
{
    char text[TEXT_BYTES];
    bento_ui_widget_args_t args;
    bento_status_t status;

    (void)snprintf(text, sizeof(text),
                   "ผักเครียดหนัก!\n%s\nSW5 / ack = รับทราบ",
                   verdict->why_thai);
    bento_ui_widget_args_init(&args);
    args.text = text;
    args.text_len = (uint32_t)strlen(text);
    args.x = BOX_X;
    args.y = BOX_Y;
    args.w = BOX_W;
    args.h = BOX_H;
    args.font_px = 0;
    status = bento_ui_msgbox_create(screen->ui, &args, &screen->box);
    screen->has_box = status == BENTO_OK;
    return status;
}

/* ---- 6) โปรแกรมหลัก ---- */

/* The .py's stop(): the matrix and the box cleared, the reason on
 * the screen, then the program ends (halted). */
static bento_status_t stop_here(farm_t *farm, const char *text,
                                uint32_t color)
{
    screen_t *screen = &farm->screen;
    bento_status_t status;

    show_link(screen, false);
    status = bento_rgbmatrix_scroll(
        NULL, 0U, BENTO_RGBMATRIX_SCROLL_COLOR_DEFAULT,
        BENTO_RGBMATRIX_SCROLL_MS_DEFAULT);
    if (status == BENTO_OK) {
        status = bento_rgbmatrix_clear();
    }
    if (status == BENTO_OK) {
        close_box(screen);
        show_status(screen, text, color);
        drain_events(screen);
    }
    farm->halted = true;
    return status;
}

/* send(): false only when the link is gone (publish raises then);
 * a publish the network refuses answers False, which send ignores. */
static bool send_event(const farm_t *farm, const reading_t *reading,
                       const verdict_t *verdict)
{
    char payload[PAYLOAD_BYTES];
    char temp_c[BENTO_FMT_BUF_SIZE];
    char rh[BENTO_FMT_BUF_SIZE];
    char vpd[BENTO_FMT_BUF_SIZE];
    uint32_t length = 0U;
    const watch_t *watch = &farm->watch;

    /* a BENTO_FMT_BUF_SIZE buffer always fits */
    (void)bento_fmt_real(temp_c, sizeof(temp_c),
                         round_tenths(reading->temp_c), &length);
    (void)bento_fmt_real(rh, sizeof(rh), round_tenths(reading->rh),
                         &length);
    (void)bento_fmt_real(vpd, sizeof(vpd), round_hundredths(watch->vpd),
                         &length);

    (void)snprintf(
        payload, sizeof(payload),
        "{\"id\": \"%s\", \"crop\": \"%s\", \"level\": %" PRId32
        ", \"alert\": \"%s\", \"temp_c\": %s, \"rh\": %s, "
        "\"sun_c\": %" PRId32 ", \"vpd\": %s, \"stress_min\": %" PRId32
        "}",
        TEAM, watch->crop->code, verdict->level, verdict->why_english,
        temp_c, rh, reading->sun_c, vpd, watch->stress_s / S_PER_MIN);
    return bento_mqtt_publish(farm->broker, farm->topic_event,
                              (uint32_t)strlen(farm->topic_event),
                              (const uint8_t *)payload,
                              (uint32_t)strlen(payload), 0U,
                              0U) != BENTO_E_SESSION;
}

/* The level changed: send first, then the matrix, the tune, the ack
 * line and, at level 2, the alert box. */
static bento_status_t on_level(farm_t *farm, const reading_t *reading,
                               const verdict_t *verdict)
{
    static const char *const words[LEVEL_COUNT] = {"GROW", "CHECK",
                                                   "STRESS"};
    static const int32_t colors[LEVEL_COUNT] = {BENTO_RGBMATRIX_GREEN,
                                                BENTO_RGBMATRIX_YELLOW,
                                                BENTO_RGBMATRIX_RED};
    static const uint32_t tunes[LEVEL_COUNT] = {TUNE_GOOD, TUNE_TAP,
                                                TUNE_EMPTY};
    const int32_t level = verdict->level;
    const bool bad = level == LEVEL_BAD;
    bento_status_t status = BENTO_OK;

    farm->watch.level = level;
    farm->watch.acked = level < LEVEL_BAD;
    farm->watch.alerts++;
    if (!send_event(farm, reading, verdict)) {
        status = stop_here(farm, "สายหลุดตอนส่ง", COL_BAD);
    }
    if (going_on(status, farm)) {
        status = bento_rgbmatrix_scroll(words[level],
                                        (uint32_t)strlen(words[level]),
                                        colors[level], SCROLL_MS);
    }
    if (going_on(status, farm)) {
        status = beep(farm->screen.ui, tunes[level]);
    }
    if (going_on(status, farm)) {
        show_reply(&farm->screen, &farm->watch,
                   bad ? "รอคนดูแลโรงเรือน" : "ส่งแล้ว",
                   bad ? (uint32_t)COL_BAD : (uint32_t)COL_DIM);
        close_box(&farm->screen);
        if (bad) {
            status = open_box(&farm->screen, verdict);
        }
    }
    return status;
}

/* Time under stress: every second at level 1 or 2. */
static void count_stress(watch_t *watch, int32_t level)
{
    if (level > LEVEL_OK) {
        watch->stress_s += READ_MS / MS_PER_S;
    }
}

/* *alerted when the level changed and was just reported. A sensor
 * that cannot be read is tried again in the next round. */
static bento_status_t read_and_judge(farm_t *farm, bool *alerted)
{
    reading_t reading = {(bento_real_t)0.0, (bento_real_t)0.0, 0};
    verdict_t verdict;
    uint16_t raw = 0U;
    bento_status_t status = bento_pots_read(SUN_POT, &raw);

    *alerted = false;
    if (status == BENTO_OK) {
        /* VR3 = แดดบนหลังคา +0..10 C */
        reading.sun_c = ((int32_t)raw * SUN_MAX_C) / POT_FULL;
        farm->watch.has_reading = read_climate(&reading);
    }
    if ((status == BENTO_OK) && farm->watch.has_reading) {
        reading.temp_c += (bento_real_t)reading.sun_c;
        judge(&reading, farm->watch.lim, &verdict);
        farm->watch.vpd = vpd_kpa(reading.temp_c, reading.rh);
        count_stress(&farm->watch, verdict.level);
        show_reading(&farm->screen, &farm->watch, &reading, &verdict);
    }
    if ((status == BENTO_OK) && farm->watch.has_reading &&
        (verdict.level != farm->watch.level)) {
        status = on_level(farm, &reading, &verdict);
        *alerted = true;
    }
    return status;
}

/* A command from the app: *confirmed for "ack"; "set" takes a new t_hi
 * in a sensible range; anything else is ignored quietly. */
static bento_status_t on_command(farm_t *farm, const message_t *message,
                                 bool *confirmed)
{
    watch_t *watch = &farm->watch;
    command_t command;
    int32_t t_hi = 0;
    bento_status_t status = BENTO_OK;

    if (read_command(message, &command)) {
        *confirmed = cmd_is(message, &command, "ack");
        if (cmd_is(message, &command, "set") && command.has_t_hi &&
            value_as_int(message, &command.t_hi, &t_hi) &&
            (watch->lim[LIM_T_LO] < t_hi) && (t_hi <= T_HI_MAX)) {
            watch->lim[LIM_T_HI] = t_hi;
            watch->level = LEVEL_NONE; /* judge again, report at once */
            watch->has_reading = false;
            show_crop(&farm->screen, watch);
            status = beep(farm->screen.ui, TUNE_TAP);
        }
    }
    return status;
}

/* SW5 = acknowledged in the greenhouse, without the network; a press
 * counts when the button has just gone down. */
static bento_status_t check_button(farm_t *farm, timers_t *timers,
                                   bool *confirmed)
{
    bool down = false;
    bento_status_t status =
        bento_buttons_pressed(CONFIRM_BUTTON, &down);

    if ((status == BENTO_OK) && down && !timers->was_down) {
        *confirmed = true;
        if (farm->watch.acked) { /* nothing to acknowledge yet */
            show_reply(&farm->screen, &farm->watch, "ยังไม่มีเหตุเตือน",
                       COL_DIM);
            status = beep(farm->screen.ui, TUNE_TAP);
        }
    }
    timers->was_down = down;
    return status;
}

/* Acknowledged: the beeping stops, the box closes, the matrix says
 * ACK. */
static bento_status_t acknowledge(farm_t *farm)
{
    static const char done_word[] = "ACK";
    bento_status_t status;

    farm->watch.acked = true;
    close_box(&farm->screen);
    show_reply(&farm->screen, &farm->watch, "รับทราบแล้ว", COL_OK);
    status =
        bento_rgbmatrix_scroll(done_word, (uint32_t)strlen(done_word),
                               BENTO_RGBMATRIX_CYAN, SCROLL_MS);
    if (status == BENTO_OK) {
        status = beep(farm->screen.ui, TUNE_TAP);
    }
    return status;
}

/* The inbox every round, the button, then an acknowledgement. */
static bento_status_t handle_inputs(farm_t *farm, timers_t *timers)
{
    message_t message;
    bool confirmed = false;
    bento_status_t status = BENTO_OK;

    if (take_newest(farm->broker, &message)) {
        status = on_command(farm, &message, &confirmed);
    }
    if (status == BENTO_OK) {
        status = check_button(farm, timers, &confirmed);
    }
    if ((status == BENTO_OK) && confirmed && !farm->watch.acked) {
        status = acknowledge(farm);
    }
    return status;
}

/* One round: listen every POLL_MS, read the sensor every READ_MS,
 * and beep every BEEP_MS while level 2 waits for an ack. */
static bento_status_t run_round(farm_t *farm, timers_t *timers)
{
    const uint32_t now_ms = bento_time_ticks_ms();
    bool alerted = false;
    bento_status_t status = handle_inputs(farm, timers);

    if (status == BENTO_OK) {
        drain_events(&farm->screen);
    }
    if ((status == BENTO_OK) &&
        (!farm->watch.has_reading ||
         (bento_time_ticks_diff(now_ms, timers->read_ms) >= READ_MS))) {
        timers->read_ms = now_ms;
        status = read_and_judge(farm, &alerted);
        if (alerted) {
            timers->beep_ms = now_ms;
        }
    }
    if (going_on(status, farm) && (farm->watch.level == LEVEL_BAD) &&
        !farm->watch.acked &&
        (bento_time_ticks_diff(now_ms, timers->beep_ms) >= BEEP_MS)) {
        timers->beep_ms = now_ms;
        status = beep(farm->screen.ui, TUNE_BAD);
    }
    if (going_on(status, farm)) {
        status = wait_ms(POLL_MS);
    }
    return status;
}

static bento_status_t watch_house(farm_t *farm)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    timers_t timers = {start_ms, start_ms, false};
    bento_status_t status = BENTO_OK;

    while (going_on(status, farm) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = run_round(farm, &timers);
    }
    if (going_on(status, farm)) {
        char line[TEXT_BYTES];
        (void)snprintf(line, sizeof(line),
                       "จบรอบ เครียดสะสม %02" PRId32 ":%02" PRId32,
                       farm->watch.stress_s / S_PER_MIN,
                       farm->watch.stress_s % S_PER_MIN);
        status = stop_here(farm, line, COL_DIM);
    }
    return status;
}

static void start_watch(watch_t *watch, const crop_t *crop)
{
    watch->crop = crop;
    (void)memcpy(watch->lim, crop->limits, sizeof(watch->lim));
    watch->level = LEVEL_NONE;
    watch->acked = true;
    watch->alerts = 0;
    watch->has_reading = false;
    watch->stress_s = 0;
    watch->vpd = (bento_real_t)0.0;
}

/* From the settings check to the end of the run; each step runs only
 * while nothing has stopped the program. */
static bento_status_t run_farm(farm_t *farm, int32_t crop_index)
{
    const char *problem = NULL;
    bento_status_t status = BENTO_OK;

    if (crop_index < 0) {
        status = stop_here(farm, "แก้ CROP ให้ตรงกับ CROPS", COL_BAD);
    }
    if (going_on(status, farm)) {
        start_watch(&farm->watch, &CROPS[crop_index]);
        show_crop(&farm->screen, &farm->watch);
        if (!valid_team()) {
            status = stop_here(farm, "แก้ TEAM ก่อน", COL_BAD);
        }
    }
    if (going_on(status, farm)) {
        status = connect_farm(farm, &problem);
    }
    if (going_on(status, farm) && (problem != NULL)) {
        status = stop_here(farm, problem, COL_BAD);
    }
    if (going_on(status, farm)) {
        show_link(&farm->screen, true);
        show_status(&farm->screen, "เฝ้าโรงเรือนอยู่", COL_OK);
        status = watch_house(farm);
    }
    return status;
}

int main(void)
{
    const int32_t crop_index = find_crop();
    farm_t farm;
    bento_status_t status;

    (void)memset(&farm, 0, sizeof(farm));
    farm.screen.ui = bento_runtime_get_ui();
    (void)snprintf(farm.topic_event, sizeof(farm.topic_event),
                   "%s/%s/event", ROOT, TEAM);
    (void)snprintf(farm.topic_cmd, sizeof(farm.topic_cmd), "%s/%s/cmd",
                   ROOT, TEAM);
    /* sent and forgotten; E_UNSUPPORTED is the hasattr() that fails */
    (void)bento_ui_sfx_volume_set(farm.screen.ui, SPEAKER);
    status = build_screen(&farm.screen);
    if (status == BENTO_OK) {
        status = run_farm(&farm, crop_index);
    }
    if (farm.broker != 0U) {
        (void)bento_mqtt_close(farm.broker); /* the finally's except */
    }
    (void)printf("[MQTT] Disconnected\r\n");
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
