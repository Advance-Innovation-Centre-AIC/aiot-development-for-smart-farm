/*
 * sf3_05_farm_all_in_one.c -- ฟาร์มครบวงจรในไฟล์เดียว: บอร์ดคือ Smart
 * IoT Gateway
 *
 * ครบห้าเสาในโปรแกรมเดียว: โรงเรือน (SHT40 DPS368 + พัดลม) แปลงผัก
 * (VR2 = ดินจำลอง + ปั๊ม) คอกสัตว์ (เรดาร์ + ไซเรน) ไฟ Led บนจอ
 * จอไฟ RGB และลำโพงทำหน้าที่ "รีเลย์" ลองเล่น: หมุน VR2 ลง (ดินแห้ง)
 * VR3 ลง (เกณฑ์พัดลม) VR4 = เขตคอก เดินเข้าหาบอร์ดแล้วกด SW6
 * (รับทราบผู้บุกรุก) กด SW5 ค้าง = รดน้ำ แตะ "ส่งรายงานเลย" บรรทัดล่าง
 * สุดคือ JSON ใบล่าสุดที่ส่งขึ้น MQTT แอปสั่งกลับได้:
 * {"cmd":"pump","on":1,"sec":10} {"cmd":"pump","on":0} {"cmd":"beep"}
 * {"cmd":"ack"} กฎทั้งหมดอยู่บนบอร์ด เน็ตหลุดฟาร์มก็ไม่หยุด
 * ต้องแก้ก่อนรัน: WIFI_SSID WIFI_PASS TEAM (broker ไม่เข้ารหัส
 * ห้ามส่งของลับ)
 *
 * All five pillars in one program: the greenhouse (SHT40, DPS368
 * and a fan), the vegetable plot (VR2 is the simulated soil, with a
 * pump) and the animal pen (radar and a siren). The on-screen LEDs,
 * the RGB matrix and the speaker play the relays. Turn VR2 down (dry
 * soil), VR3 down (the fan threshold), VR4 sets the pen zone; walk up
 * to the board and press SW6 to acknowledge an intruder; hold SW5 to
 * water; tap the report button. The bottom line is the last JSON
 * sent over MQTT, and the app can send the commands above back. The
 * rules live on the board, so the farm keeps working offline. Edit
 * WIFI_SSID, WIFI_PASS and TEAM before running; the broker is not
 * encrypted, so send nothing secret.
 *
 * Twin of sf3_05_farm_all_in_one.py
 * (aiot-development-for-smart-farm/s3/sf3_05_farm_all_in_one.py).
 * Style: CLEAN_C.md.
 * Differs from the original:
 *   - TEMP_OFFSET is TEMP_OFFSET_TENTHS (tenths of a degree): an enum
 *     holds no real number.
 *   - json.dumps writes the keys in the slot order of the board's
 *     MicroPython dict (py/map.c, 1-byte qstr hash on the PSoC Edge,
 *     2-byte on the reTerminal); the text is built with snprintf in
 *     that order, None as null, a float as str() writes it.
 *   - json.loads: a small reader takes one top-level object, skips
 *     nested values by their brackets, and finds cmd, on and sec. It
 *     compares strings byte for byte (no escapes); a payload only
 *     MicroPython's looser reader accepts is treated as not JSON.
 *   - wifi.connect() and mqtt.connect() wait inside the binding; here
 *     they are started and polled, so Stop ends a wait. A broker
 *     link that fails to subscribe is closed (the original leaves
 *     it open and unused).
 *   - hasattr(ui, "volume"): the volume is always set, and a build
 *     without sound refuses it, which is ignored.
 *   - try/except around the sensors, the RGB matrix and publish: the
 *     call's status is checked and the same fallback is taken.
 *   - The room humidity uses exp() in double, then the board's float:
 *     it may differ from MicroPython's math.exp in the last bit (only
 *     when TEMP_OFFSET_TENTHS is not 0).
 *   - The Button and Farm classes and the widget dict are structs
 *     kept in main() and passed by pointer; the incoming payload and
 *     the JSON line are file-scope buffers (CLEAN_C.md C9).
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

/* Edit these before running. */
static const char TEAM[] = "teamXX"; /* team01-team99, as in the app */
/* WIFI_SSID and WIFI_PASS are wifi_ssid and wifi_pass in
 * join_wifi(), BROKER is broker in open_broker(): each is read there
 * only. */

static const char OFFLINE[] = "ออฟไลน์ (ทำงานต่อ)";

/* Settings: change them and run again. */
enum {
    TEMP_OFFSET_TENTHS = 0, /* temperature fix in 0.1 C, -70 = -7.0 C */
    FAN_LO = 25,
    FAN_SPAN = 20, /* fan threshold 25-45 C (VR3) */
    PUMP_ON = 35,
    PUMP_OFF = 45, /* soil % pump on / off */
    GUARD_LO = 50,
    GUARD_SPAN = 200, /* pen zone 50-250 cm (VR4) */
    TICK_MS = 500,
    REPORT_MS = 5000,
    RUN_MS = 600000,
    VOLUME = 25, /* 0-127 (about 20 %) */
    SPEAKER = 40 /* speaker 0-100 % (2.4.2+) */
};

/* Hardware, network and timing. */
enum {
    SOIL_POT = 1,  /* VR2 */
    FAN_POT = 2,   /* VR3 */
    GUARD_POT = 3, /* VR4 */
    POT_FULL = 4095,
    PERCENT = 100,
    WATER_INDEX = 1,       /* SW5, the lower button */
    ACKNOWLEDGE_INDEX = 0, /* SW6, the upper button */
    AIR_TRIES = 3,
    AIR_RETRY_MS = 20,
    RADAR_THRESHOLD_DB = 4,
    RADAR_SETTLE_MS = 500,
    CM_PER_M = 100,
    TENTHS = 10,
    CONFIRM_ROUNDS = 3,
    COOL_MARGIN = 1, /* off 1 C below the threshold */
    REMOTE_OFF_MS = -30000,
    SECOND_MS = 1000,
    PUMP_MAX_S = 30,
    PUMP_DEFAULT_S = 10,
    MATRIX_RESEND_MS = 3000,
    MATRIX_OFF = -1,
    MATRIX_INTRUDER = 2,
    INTRUDER_SCROLL_MS = 45,
    WAIT_MS = 100, /* the command box has one slot */
    BUTTON_POLL_MS = 20,
    BEEP_MS = 120,
    SCREEN_SETTLE_MS = 200,
    MQTT_PORT = 1883,
    KEEPALIVE_S = 60,
    NET_POLL_MS = 50,
    TEAM_CHARS = 6,
    TEAM_PREFIX = 4,
    ID_SUFFIX_MASK = 0xFFFF,
    CLIENT_ID_BYTES = 40,
    TOPIC_BYTES = 64,
    TEXT_BYTES = 128,
    JSON_SHOWN = 76, /* the bottom line shows this much */
    JSON_LINE_BYTES = 512
};

/* MIDI notes of the beeps. */
enum {
    NOTE_A4 = 69,
    NOTE_C5 = 72,
    NOTE_E5 = 76,
    NOTE_G5 = 79,
    NOTE_C6 = 84
};

/* Layout on the 800 x 400 canvas. */
enum {
    TITLE_X = 12,
    TITLE_Y = 6,
    TITLE_PX = 24,
    BIG_PX = 28,
    TEXT_PX = 16,
    SMALL_PX = 14,
    CARD_Y = 44,
    CARD_H = 226,
    CARD_RADIUS = 12,
    CARD_BORDER = 1,
    CARD_TITLE_DX = 12,
    CARD_TITLE_DY = 6,
    HOUSE_X = 12,
    HOUSE_W = 252,
    VALUE_Y = 80,
    HOUSE_VALUE_X = 26,
    LED_Y = 176,
    LED_SIZE = 36,
    LED_NOTE_Y = 184,
    FAN_AT_X = 72,
    PLOT_X = 272,
    PLOT_W = 252,
    ARC_X = 284,
    ARC_Y = 76,
    ARC_SIZE = 112,
    SOIL_X = 406,
    SOIL_Y = 112,
    PUMP_X = 286,
    PUMP_Y = 204,
    PUMP_NOTE_X = 332,
    PUMP_NOTE_Y = 212,
    PEN_X = 532,
    PEN_W = 248,
    PEN_VALUE_X = 546,
    GUARD_Y = 124,
    PEN_NOTE_X = 592,
    BOX_X = 12,
    BOX_Y = 278,
    BOX_W = 668, /* the right corner is the Console button's */
    BOX_H = 62,
    NOTE_X = 24,
    NOTE_Y = 286,
    COMMAND_Y = 312,
    SEND_X = 520,
    SEND_Y = 286,
    SEND_W = 150,
    SEND_H = 46,
    JSON_X = 12,
    JSON_Y = 352
};

/* Colours, 0xRRGGBB. */
enum {
    COLOR_TEXT = 0xE8EAED,
    COLOR_DIM = 0x9AA3AF,
    COLOR_CARD = 0x171B22,
    COLOR_OK = 0x30A46C,
    COLOR_WARN = 0xF5A623,
    COLOR_BAD = 0xE5484D,
    COLOR_INFO = 0x4A9EFF
};

/* What an app command asks for. */
typedef enum {
    ASK_NONE = 0,
    ASK_PUMP,
    ASK_ACKNOWLEDGE,
    ASK_BEEP
} ask_t;

/* One JSON value the reader found: its kind and where it sits (a
 * string without its quotes). */
typedef enum {
    JSON_ABSENT = 0,
    JSON_STRING,
    JSON_NUMBER,
    JSON_TRUE,
    JSON_FALSE,
    JSON_NULL,
    JSON_NESTED_EMPTY, /* [] or {} */
    JSON_NESTED
} json_kind_t;

typedef struct {
    json_kind_t kind;
    uint32_t start;
    uint32_t length;
} json_field_t;

/* The fields of an app command that the farm reads. */
typedef struct {
    json_field_t command;
    json_field_t on;
    json_field_t seconds;
} app_fields_t;

typedef struct {
    const uint8_t *text;
    uint32_t length;
    uint32_t at;
} json_reader_t;

typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
} rect_t;

typedef struct {
    uint32_t index;
    bool down;
    bool clicked;
} board_button_t;

/* The widgets the program changes after it has drawn them. */
typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t temperature;
    bento_ui_id_t fan;
    bento_ui_id_t fan_at;
    bento_ui_id_t arc;
    bento_ui_id_t soil;
    bento_ui_id_t pump;
    bento_ui_id_t distance;
    bento_ui_id_t guard;
    bento_ui_id_t siren;
    bento_ui_id_t pen;
    bento_ui_id_t note;
    bento_ui_id_t command;
    bento_ui_id_t send;
    bento_ui_id_t json;
} farm_screen_t;

/* The farm's readings and decisions; has_* false is Python's None. */
typedef struct {
    bool has_air;
    bento_real_t temperature;
    bento_real_t humidity;
    bool has_pressure;
    bento_real_t pressure;
    bool has_target;
    int32_t distance_cm;
    int32_t soil;
    bento_real_t fan_at;
    int32_t guard;
    bool fan;
    bool automatic;
    bool pump;
    bool inside;
    bool silenced;
    bool online;
    int32_t streak;
    int32_t count;
    int32_t reports;
    int32_t remote_ms; /* > 0 the app waters, < 0 no auto watering */
    bool has_shown;
    int32_t shown;
    uint32_t matrix_ms;
    uint32_t broker;
    char client_id[CLIENT_ID_BYTES];
} farm_t;

/* The incoming payload and the outgoing JSON line: file scope,
 * zeroed by the loader on every load and set again at the start of
 * main() (CLEAN_C.md C9, deviation BENTO-MISRA-E03/E04). */
extern uint8_t g_payload[BENTO_MQTT_PAYLOAD_MAX];
uint8_t g_payload[BENTO_MQTT_PAYLOAD_MAX];
extern char g_json_line[JSON_LINE_BYTES];
char g_json_line[JSON_LINE_BYTES];

static int16_t px(int32_t value)
{
    return (int16_t)value;
}

/* Sleeps, and turns Stop into BENTO_E_CANCELLED. */
static bento_status_t pause_ms(uint32_t duration_ms)
{
    bento_status_t status = bento_time_sleep_ms(duration_ms);

    if ((status == BENTO_OK) && !bento_runtime_is_running()) {
        status = BENTO_E_CANCELLED;
    }
    return status;
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

/* A setter's refusal is ignored, as MicroPython's widget setters
 * ignore it (modui.c). */
static void set_text(bento_ui_t *ui, bento_ui_id_t widget,
                     const char *text)
{
    (void)bento_ui_set_text(ui, widget, text, (uint32_t)strlen(text));
}

static bento_status_t add_label(bento_ui_t *ui, const char *text,
                                const rect_t *place, uint32_t color,
                                int32_t size_px, bento_ui_id_t *out_id)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.text = text;
    args.text_len = (uint32_t)strlen(text);
    args.x = place->x;
    args.y = place->y;
    args.color = color;
    args.font_px = size_px;
    return bento_ui_label_create(ui, &args, out_id);
}

/* label(): grey 16 px unless told otherwise. */
static bento_status_t add_line(bento_ui_t *ui, const char *text,
                               int32_t x, int32_t y,
                               bento_ui_id_t *out_id)
{
    const rect_t place = {px(x), px(y), 0, 0};

    return add_label(ui, text, &place, COLOR_DIM, TEXT_PX, out_id);
}

/* A big white value. */
static bento_status_t add_value(bento_ui_t *ui, int32_t x, int32_t y,
                                bento_ui_id_t *out_id)
{
    const rect_t place = {px(x), px(y), 0, 0};

    return add_label(ui, "--", &place, COLOR_TEXT, BIG_PX, out_id);
}

/* led(): a 36 px light, off. */
static bento_status_t add_led(bento_ui_t *ui, int32_t x, int32_t y,
                              uint32_t color, bento_ui_id_t *out_id)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.x = px(x);
    args.y = px(y);
    args.w = LED_SIZE;
    args.h = LED_SIZE;
    args.color = color;
    return bento_ui_led_create(ui, &args, out_id);
}

/* A dark card, with its title when it has one. */
static bento_status_t add_card(bento_ui_t *ui, const rect_t *place,
                               const char *title)
{
    const rect_t title_place = {px(place->x + CARD_TITLE_DX),
                                px(place->y + CARD_TITLE_DY), 0, 0};
    bento_ui_widget_args_t args;
    bento_ui_id_t unused;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = place->x;
    args.y = place->y;
    args.w = place->w;
    args.h = place->h;
    args.color = COLOR_CARD;
    args.min_val = COLOR_DIM; /* a panel's min= is its border */
    args.max_val = CARD_RADIUS;
    args.font_px = CARD_BORDER;
    status = bento_ui_panel_create(ui, &args, &unused);
    if ((status == BENTO_OK) && (title != NULL)) {
        status = add_label(ui, title, &title_place, COLOR_INFO, TEXT_PX,
                           &unused);
    }
    return status;
}

/* Soft MIDI notes (ui.sfx cannot be made quieter), 120 ms apart.
 * ui.tone() is sent and forgotten, so its status is not read. */
static bento_status_t beep(bento_ui_t *ui, const uint8_t *notes,
                           uint32_t count)
{
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < count) && (status == BENTO_OK); i++) {
        (void)bento_ui_sfx_tone(ui, notes[i],
                                (uint8_t)BENTO_UI_WAVE_SINE,
                                (uint8_t)VOLUME, (uint16_t)BEEP_MS);
        status = pause_ms(BEEP_MS);
    }
    return status;
}

static bento_status_t beep_one(bento_ui_t *ui, int32_t note)
{
    const uint8_t notes[] = {(uint8_t)note};

    return beep(ui, notes, 1U);
}

static bento_status_t beep_two(bento_ui_t *ui, int32_t first,
                               int32_t second)
{
    const uint8_t notes[] = {(uint8_t)first, (uint8_t)second};

    return beep(ui, notes, 2U);
}

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

/* Waits duration_ms, reading both buttons every 20 ms so a short
 * press is not missed. */
static bento_status_t wait_ms(uint32_t duration_ms,
                              board_button_t *water,
                              board_button_t *acknowledge)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            (int32_t)duration_ms)) {
        status = sample_button(water);
        if (status == BENTO_OK) {
            status = sample_button(acknowledge);
        }
        if (status == BENTO_OK) {
            status = pause_ms(BUTTON_POLL_MS);
        }
    }
    return status;
}

/* round(x, 1): nearbyint(x * 10) / 10 in the board's float width
 * (py/modbuiltins.c). */
static bento_real_t round_tenth(bento_real_t value)
{
    const bento_real_t scale = (bento_real_t)TENTHS;
    const bento_real_t scaled = value * scale;

    return (bento_real_t)nearbyint((double)scaled) / scale;
}

/* The room's humidity from the sensor's (the Magnus formula), when
 * the board's own heat warms the sensor. */
static bento_real_t room_humidity(bento_real_t raw_humidity,
                                  bento_real_t raw_c,
                                  bento_real_t room_c)
{
    const bento_real_t slope = (bento_real_t)17.62;
    const bento_real_t base_c = (bento_real_t)243.12;
    const bento_real_t sensor_part = (slope * raw_c) / (base_c + raw_c);
    const bento_real_t room_part = (slope * room_c) / (base_c + room_c);
    const bento_real_t exponent = sensor_part - room_part;
    const bento_real_t factor = (bento_real_t)exp((double)exponent);
    const bento_real_t humidity = raw_humidity * factor;

    return (humidity > (bento_real_t)PERCENT) ? (bento_real_t)PERCENT
                                              : humidity;
}

/* One try at the SHT40: both values, or none. */
static bool read_sht40(farm_t *farm)
{
    const bento_real_t offset =
        (bento_real_t)TEMP_OFFSET_TENTHS / (bento_real_t)TENTHS;
    float raw_c = 0.0F;
    float raw_humidity = 0.0F;
    const bool answered =
        (bento_sensors_sht40_temperature(&raw_c) == BENTO_OK) &&
        (bento_sensors_sht40_humidity(&raw_humidity) == BENTO_OK);

    if (answered) {
        bento_real_t humidity = (bento_real_t)raw_humidity;
        const bento_real_t room_c = (bento_real_t)raw_c + offset;

        if (TEMP_OFFSET_TENTHS != 0) {
            humidity =
                room_humidity(humidity, (bento_real_t)raw_c, room_c);
        }
        farm->temperature = round_tenth(room_c);
        farm->humidity = round_tenth(humidity);
    }
    return answered;
}

/* The air: three tries at the SHT40, then the pressure. A sensor
 * that does not answer leaves None. */
static bento_status_t read_air(farm_t *farm)
{
    float pressure = 0.0F;
    int32_t tries = 0;
    bento_status_t status = BENTO_OK;

    farm->has_air = false;
    while (!farm->has_air && (tries < AIR_TRIES) &&
           (status == BENTO_OK)) {
        farm->has_air = read_sht40(farm);
        if (!farm->has_air) {
            status = pause_ms(AIR_RETRY_MS);
        }
        tries++;
    }
    farm->has_pressure =
        bento_sensors_dps368_pressure(&pressure) == BENTO_OK;
    if (farm->has_pressure) {
        farm->pressure = round_tenth((bento_real_t)pressure);
    }
    return status;
}

/* The radar's distance; no target, or no answer, is None. */
static void read_distance(farm_t *farm)
{
    bento_sensors_radar_range_t range;

    bento_sensors_radar_range_init(&range);
    farm->has_target =
        (bento_sensors_radar_range(&range) == BENTO_OK) &&
        (range.target != 0U);
    farm->distance_cm = 0;
    if (farm->has_target) {
        const bento_real_t cm =
            (bento_real_t)range.distance_m * (bento_real_t)CM_PER_M;

        farm->distance_cm = (int32_t)cm;
    }
}

/* Learns the still scene, then sets the 4 dB threshold. *learned is
 * false when the radar did not answer. */
static bento_status_t radar_start(bool *learned)
{
    bento_status_t status = BENTO_OK;

    *learned = bento_sensors_radar_config(0.0F) == BENTO_OK;
    if (*learned) {
        status = pause_ms(RADAR_SETTLE_MS);
    }
    if ((status == BENTO_OK) && *learned) {
        *learned = bento_sensors_radar_config(
                       (float)RADAR_THRESHOLD_DB) == BENTO_OK;
    }
    return status;
}

/* One picture: 2 INTRUDER, -1 dark, 0 green, 1 yellow. */
static bento_status_t draw_mode(int32_t mode)
{
    static const char intruder[] = "INTRUDER";
    bento_status_t status;

    if (mode == MATRIX_INTRUDER) {
        status = bento_rgbmatrix_scroll(
            intruder, (uint32_t)strlen(intruder), BENTO_RGBMATRIX_RED,
            INTRUDER_SCROLL_MS);
    } else if (mode < 0) {
        status = bento_rgbmatrix_clear();
    } else {
        status =
            bento_rgbmatrix_fill((mode != 0) ? BENTO_RGBMATRIX_YELLOW
                                             : BENTO_RGBMATRIX_GREEN);
    }
    return status;
}

/* The scroll is stopped first. A refusal anywhere skips the rest,
 * as the original's try does, and the next change or resend draws
 * it again. */
static void matrix(int32_t mode)
{
    if (bento_rgbmatrix_scroll(
            "", 0U, BENTO_RGBMATRIX_SCROLL_COLOR_DEFAULT,
            BENTO_RGBMATRIX_SCROLL_MS_DEFAULT) == BENTO_OK) {
        (void)draw_mode(mode); /* refused: drawn again on resend */
    }
}

/* The whole farm's rules in one place; true when the pen changes (an
 * intruder comes or goes). */
static bool decide(farm_t *farm, bool near)
{
    const bento_real_t fan_off =
        farm->fan_at - (bento_real_t)COOL_MARGIN;
    bool changed = false;

    if (farm->has_air) { /* no temperature: the fan stays as it is */
        farm->fan = (farm->temperature > farm->fan_at) ||
                    (farm->fan && (farm->temperature > fan_off));
    }
    /* the gap between on and off keeps the pump from chattering */
    farm->automatic = (farm->soil < PUMP_ON) ||
                      (farm->automatic && (farm->soil < PUMP_OFF));
    /* the radar must see the same 3 rounds in a row (leaves move) */
    farm->streak = (near != farm->inside) ? (farm->streak + 1) : 0;
    if (farm->streak >= CONFIRM_ROUNDS) {
        farm->inside = near;
        farm->streak = 0;
        farm->silenced = false;
        if (near) {
            farm->count++; /* counted only on the way in */
        }
        changed = true;
    }
    return changed;
}

/* bento_ui_utf8_prefix_length keeps a cut on a character boundary. */
static void copy_text(char *out, size_t cap, const char *text,
                      size_t limit)
{
    const size_t length =
        bento_ui_utf8_prefix_length(text, strlen(text), limit);

    (void)snprintf(out, cap, "%.*s", (int)length, text);
}

/* team01-team99: "team", two digits, not team00. Another team's
 * name would make the broker kick that team's board off. */
static bool team_is_set(void)
{
    const char *digits = &TEAM[TEAM_PREFIX];
    bool good = (strlen(TEAM) == (size_t)TEAM_CHARS) &&
                (strncmp(TEAM, "team", (size_t)TEAM_PREFIX) == 0);

    good = good && (digits[0] >= '0') && (digits[0] <= '9') &&
           (digits[1] >= '0') && (digits[1] <= '9');
    return good && (strcmp(TEAM, "team00") != 0);
}

/* Is this the reTerminal? The dict order of json.dumps depends on
 * it; asked by the name the board gives, so one Emulator build
 * serves both board models. */
static bool on_reterminal(void)
{
    bento_gpio_board_info_t info;
#if defined(BENTO_TARGET_RETERMINAL)
    bool answer = true;
#else
    bool answer = false;
#endif

    bento_gpio_board_info_init(&info);
    if ((bento_gpio_board_info(&info) == BENTO_OK) &&
        (info.name != NULL)) {
        answer = (strcmp(info.name, "Seeed reTerminal") == 0);
    }
    return answer;
}

static void make_topic(char *topic, size_t cap, const char *leaf)
{
    (void)snprintf(topic, cap, "bento-aiot/%s/%s", TEAM, leaf);
}

/* Waits for a started WiFi connect; Stop cancels it. */
static bento_status_t wait_wifi(void)
{
    bento_status_t status = BENTO_E_IN_PROGRESS;

    while (status == BENTO_E_IN_PROGRESS) {
        status = pause_ms(NET_POLL_MS);
        if (status == BENTO_OK) {
            status = bento_wifi_connect_poll();
        } else {
            (void)bento_wifi_connect_cancel(); /* always BENTO_OK */
        }
    }
    return status;
}

/* wifi.connect(), then an address that is not 0.0.0.0. */
static bento_status_t join_wifi(void)
{
    static const char wifi_ssid[] =
        "<ชื่อ Hotspot ของกลุ่ม>"; /* no spaces */
    static const char wifi_pass[] = "<รหัส Hotspot ของกลุ่ม>"; /* 8+ */
    bento_wifi_connect_params_t params;
    uint32_t address = 0U;
    bento_status_t status;

    bento_wifi_connect_params_init(&params);
    params.ssid = wifi_ssid;
    params.ssid_len = (uint32_t)strlen(wifi_ssid);
    params.pass = wifi_pass;
    params.pass_len = (uint32_t)strlen(wifi_pass);
    params.flags = BENTO_WIFI_F_SAVE_CREDS; /* as the binding does */
    status = bento_wifi_connect_start(&params);
    if (status == BENTO_OK) {
        status = wait_wifi();
    }
    if (status == BENTO_OK) {
        status = bento_wifi_ip(BENTO_WIFI_IF_ANY, &address);
    }
    if ((status == BENTO_OK) && (address == 0U)) {
        status = BENTO_E_NOT_READY;
    }
    return status;
}

/* mqtt.connect(BROKER, port=1883, client_id=, keepalive=60). A
 * connect that fails gives its client back at once. */
static bento_status_t open_broker(farm_t *farm)
{
    /* or test.mosquitto.org */
    static const char broker[] = "broker.hivemq.com";
    bento_mqtt_config_t config;
    bento_status_t status;
    bool waiting;

    bento_mqtt_config_init(&config);
    config.host = broker;
    config.host_len = (uint32_t)strlen(broker);
    config.port = (uint16_t)MQTT_PORT;
    config.keepalive_s = (uint16_t)KEEPALIVE_S;
    config.client_id = farm->client_id;
    config.client_id_len = (uint32_t)strlen(farm->client_id);
    status = bento_mqtt_open_start(&config, &farm->broker);
    waiting = status == BENTO_OK;
    while (waiting) {
        status = pause_ms(NET_POLL_MS);
        if (status == BENTO_OK) {
            status = bento_mqtt_open_poll(farm->broker);
        }
        waiting = status == BENTO_E_IN_PROGRESS;
        if (!waiting && (status != BENTO_OK)) {
            (void)bento_mqtt_close(farm->broker); /* nothing to undo */
        }
    }
    return status;
}

/* The bottom note. No ui.poll() here: it would eat the main loop's
 * touches. */
static void show_note(const farm_screen_t *screen, const char *text,
                      uint32_t color)
{
    (void)bento_ui_set_color(screen->ui, screen->note, color);
    set_text(screen->ui, screen->note, text);
}

/* WiFi, the broker, then the command topic; any step that fails
 * is the original's False or caught OSError. */
static bento_status_t connect_all(farm_t *farm)
{
    char topic[TOPIC_BYTES];
    uint8_t granted = 0U;
    bento_status_t step = join_wifi();

    if (step == BENTO_OK) {
        step = open_broker(farm);
    }
    if (step == BENTO_OK) {
        make_topic(topic, sizeof(topic), "cmd");
        step = bento_mqtt_subscribe(
            farm->broker, topic, (uint32_t)strlen(topic), 0U, &granted);
        if (step != BENTO_OK) {
            (void)bento_mqtt_close(farm->broker); /* stays offline */
        }
    }
    return step;
}

/* Without a real TEAM nothing connects (client ids would clash).
 * farm->online false is offline; only Stop is an error. */
static bento_status_t go_online(const farm_screen_t *screen,
                                farm_t *farm)
{
    char text[TEXT_BYTES];
    bento_status_t step = BENTO_E_FAILED;

    farm->online = false;
    if (!team_is_set()) {
        (void)snprintf(text, sizeof(text), "แก้ TEAM ก่อน: %s", OFFLINE);
        show_note(screen, text, COLOR_WARN);
    } else {
        show_note(screen, "ต่อ WiFi...", COLOR_WARN);
        drain_events(screen->ui); /* the note shows before the wait */
        step = connect_all(farm);
        farm->online = step == BENTO_OK;
        (void)snprintf(text, sizeof(text), "ออนไลน์ %s", TEAM);
        show_note(screen, farm->online ? text : OFFLINE,
                  farm->online ? COLOR_OK : COLOR_WARN);
    }
    return (step == BENTO_E_CANCELLED) ? step : BENTO_OK;
}

/* Publishes the line when online; a failed publish is passed, as
 * the original passes its OSError. */
static void send(const farm_t *farm, const char *leaf, const char *line)
{
    char topic[TOPIC_BYTES];

    if (farm->online) {
        make_topic(topic, sizeof(topic), leaf);
        (void)bento_mqtt_publish(
            farm->broker, topic, (uint32_t)strlen(topic),
            (const uint8_t *)line, (uint32_t)strlen(line), 0U, 0U);
    }
}

/* A real number as str() writes it, or null for None. */
static void real_or_null(bool has_value, bento_real_t value, char *text)
{
    uint32_t length = 0U;

    if (!has_value || (bento_fmt_real(text, BENTO_FMT_BUF_SIZE, value,
                                      &length) != BENTO_OK)) {
        (void)snprintf(text, BENTO_FMT_BUF_SIZE, "%s", "null");
    }
}

/* A real number as '%.<digits>f' writes it on this board. */
static void real_text(bento_real_t value, int32_t digits, char *text)
{
    uint32_t length = 0U;

    if (bento_fmt_real_precision(text, BENTO_FMT_BUF_SIZE, value,
                                 digits, &length) != BENTO_OK) {
        text[0] = '\0';
    }
}

/* The values of one report, as text. */
typedef struct {
    char temperature[BENTO_FMT_BUF_SIZE];
    char humidity[BENTO_FMT_BUF_SIZE];
    char pressure[BENTO_FMT_BUF_SIZE];
} report_texts_t;

/* json.dumps of the telemetry dict, in the slot order of the
 * board's MicroPython dict (header). */
static void write_telemetry(const farm_t *farm, const char *by)
{
    report_texts_t texts;

    real_or_null(farm->has_air, farm->temperature, texts.temperature);
    real_or_null(farm->has_air, farm->humidity, texts.humidity);
    real_or_null(farm->has_pressure, farm->pressure, texts.pressure);
    if (on_reterminal()) {
        (void)snprintf(
            g_json_line, sizeof(g_json_line),
            "{\"soil\": %" PRId32 ", \"intrusions\": %" PRId32
            ", \"sim\": \"soil\", \"n\": %" PRId32
            ", \"id\": \"%s\", \"temp_c\": %s, \"pump\": %d"
            ", \"by\": \"%s\", \"hpa\": %s, \"fan\": %d"
            ", \"intruder\": %d, \"rh\": %s}",
            farm->soil, farm->count, farm->reports, TEAM,
            texts.temperature, farm->pump ? 1 : 0, by, texts.pressure,
            farm->fan ? 1 : 0, farm->inside ? 1 : 0, texts.humidity);
    } else {
        (void)snprintf(
            g_json_line, sizeof(g_json_line),
            "{\"intruder\": %d, \"intrusions\": %" PRId32
            ", \"sim\": \"soil\", \"by\": \"%s\", \"id\": \"%s\""
            ", \"hpa\": %s, \"soil\": %" PRId32 ", \"rh\": %s"
            ", \"fan\": %d, \"temp_c\": %s, \"pump\": %d, \"n\": "
            "%" PRId32 "}",
            farm->inside ? 1 : 0, farm->count, by, TEAM, texts.pressure,
            farm->soil, texts.humidity, farm->fan ? 1 : 0,
            texts.temperature, farm->pump ? 1 : 0, farm->reports);
    }
}

/* One report: counted, sent, printed and shown cut short. */
static void report(const farm_screen_t *screen, farm_t *farm,
                   const char *by)
{
    char shown[TEXT_BYTES];

    farm->reports++;
    write_telemetry(farm, by);
    send(farm, "telemetry", g_json_line);
    (void)printf("%s\n", g_json_line);
    copy_text(shown, sizeof(shown), g_json_line, (size_t)JSON_SHOWN);
    set_text(screen->ui, screen->json, shown);
}

/* The pen changed: the sound, then the event for the app. */
static bento_status_t announce_pen(const farm_screen_t *screen,
                                   const farm_t *farm)
{
    char distance[BENTO_FMT_BUF_SIZE];
    const bento_status_t status =
        farm->inside ? beep_two(screen->ui, NOTE_C6, NOTE_E5)
                     : beep_two(screen->ui, NOTE_G5, NOTE_C6);

    (void)snprintf(distance, sizeof(distance), "%s", "null");
    if (farm->has_target) {
        (void)snprintf(distance, sizeof(distance), "%" PRId32,
                       farm->distance_cm);
    }
    if (status == BENTO_OK) {
        (void)snprintf(
            g_json_line, sizeof(g_json_line),
            "{\"event\": \"%s\", \"id\": \"%s\", \"cm\": %s}",
            farm->inside ? "intruder" : "clear", TEAM, distance);
        send(farm, "event", g_json_line);
    }
    return status;
}

/* ---- json.loads of an app command: the fields the farm reads ---- */

static bool is_space(uint8_t byte)
{
    return (byte == (uint8_t)' ') ||
           ((byte >= (uint8_t)'\t') && (byte <= (uint8_t)'\r'));
}

static bool is_digit(uint8_t byte)
{
    return (byte >= (uint8_t)'0') && (byte <= (uint8_t)'9');
}

static void skip_space(json_reader_t *reader)
{
    while ((reader->at < reader->length) &&
           is_space(reader->text[reader->at])) {
        reader->at++;
    }
}

/* The next byte is this one: it is taken. */
static bool take(json_reader_t *reader, uint8_t byte)
{
    const bool found = (reader->at < reader->length) &&
                       (reader->text[reader->at] == byte);

    if (found) {
        reader->at++;
    }
    return found;
}

/* A string; the field is its bytes without the quotes. */
static bool read_string(json_reader_t *reader, json_field_t *field)
{
    bool closed = false;

    field->kind = JSON_STRING;
    field->start = reader->at + 1U;
    reader->at++;
    while (!closed && (reader->at < reader->length)) {
        const uint8_t byte = reader->text[reader->at];

        if (byte == (uint8_t)'\\') {
            reader->at++; /* the escaped byte is skipped */
        } else {
            closed = byte == (uint8_t)'"';
        }
        reader->at++;
    }
    field->length = closed ? (reader->at - field->start - 1U) : 0U;
    return closed;
}

static bool is_number_byte(uint8_t byte)
{
    return is_digit(byte) || (byte == (uint8_t)'-') ||
           (byte == (uint8_t)'+') || (byte == (uint8_t)'.') ||
           (byte == (uint8_t)'e') || (byte == (uint8_t)'E');
}

static bool read_number(json_reader_t *reader, json_field_t *field)
{
    field->kind = JSON_NUMBER;
    field->start = reader->at;
    while ((reader->at < reader->length) &&
           is_number_byte(reader->text[reader->at])) {
        reader->at++;
    }
    field->length = reader->at - field->start;
    return field->length > 0U;
}

/* The bytes at text are the word's letters, length of them. */
static bool same_bytes(const uint8_t *text, const char *word,
                       uint32_t length)
{
    bool same = true;
    uint32_t i;

    for (i = 0U; (i < length) && same; i++) {
        same = text[i] == (uint8_t)word[i];
    }
    return same;
}

/* true, false or null, every letter of it. */
static bool read_word(json_reader_t *reader, const char *word,
                      json_kind_t kind, json_field_t *field)
{
    const uint32_t length = (uint32_t)strlen(word);
    const bool same =
        ((reader->length - reader->at) >= length) &&
        same_bytes(&reader->text[reader->at], word, length);

    if (same) {
        field->kind = kind;
        reader->at += length;
    }
    return same;
}

/* +1 opens a nested value, -1 closes one, 0 any other byte. */
static int32_t bracket_step(uint8_t byte)
{
    int32_t step = 0;

    if ((byte == (uint8_t)'[') || (byte == (uint8_t)'{')) {
        step = 1;
    } else if ((byte == (uint8_t)']') || (byte == (uint8_t)'}')) {
        step = -1;
    } else {
        /* any other byte inside */
    }
    return step;
}

/* A nested array or object, skipped by its brackets (strings
 * inside are stepped over). Empty ones are false in Python. */
static bool skip_nested(json_reader_t *reader, json_field_t *field)
{
    int32_t depth = 0;
    bool done = false;
    json_field_t inner;

    field->start = reader->at;
    while (!done && (reader->at < reader->length)) {
        const uint8_t byte = reader->text[reader->at];

        if (byte == (uint8_t)'"') {
            done = !read_string(reader, &inner);
        } else {
            depth += bracket_step(byte);
            reader->at++;
            done = depth == 0;
        }
    }
    field->length = reader->at - field->start;
    field->kind =
        (field->length == 2U) ? JSON_NESTED_EMPTY : JSON_NESTED;
    return done && (depth == 0);
}

static bool read_value(json_reader_t *reader, json_field_t *field)
{
    const uint8_t byte =
        (reader->at < reader->length) ? reader->text[reader->at] : 0U;
    bool read;

    if (byte == (uint8_t)'"') {
        read = read_string(reader, field);
    } else if ((byte == (uint8_t)'[') || (byte == (uint8_t)'{')) {
        read = skip_nested(reader, field);
    } else if ((byte == (uint8_t)'-') || is_digit(byte)) {
        read = read_number(reader, field);
    } else {
        read = read_word(reader, "true", JSON_TRUE, field) ||
               read_word(reader, "false", JSON_FALSE, field) ||
               read_word(reader, "null", JSON_NULL, field);
    }
    return read;
}

/* Is this field's string exactly the word? */
static bool field_is(const json_reader_t *reader,
                     const json_field_t *field, const char *word)
{
    const uint32_t length = (uint32_t)strlen(word);

    return (field->kind == JSON_STRING) && (field->length == length) &&
           same_bytes(&reader->text[field->start], word, length);
}

/* One "key": value pair; a key read twice keeps its last value. */
static bool read_member(json_reader_t *reader, app_fields_t *fields)
{
    json_field_t key;
    json_field_t value;
    bool read = (reader->at < reader->length) &&
                (reader->text[reader->at] == (uint8_t)'"') &&
                read_string(reader, &key);

    skip_space(reader);
    read = read && take(reader, (uint8_t)':');
    skip_space(reader);
    read = read && read_value(reader, &value);
    if (read && field_is(reader, &key, "cmd")) {
        fields->command = value;
    } else if (read && field_is(reader, &key, "on")) {
        fields->on = value;
    } else if (read && field_is(reader, &key, "sec")) {
        fields->seconds = value;
    } else {
        /* a key the farm does not read */
    }
    return read;
}

/* The payload as one JSON object; false for anything else (not
 * JSON, or JSON with no .get()). */
static bool read_object(json_reader_t *reader, app_fields_t *fields)
{
    bool read;
    bool more;

    (void)memset(fields, 0, sizeof(*fields));
    skip_space(reader);
    read = take(reader, (uint8_t)'{');
    skip_space(reader);
    more = read && !take(reader, (uint8_t)'}');
    while (more) {
        read = read_member(reader, fields);
        skip_space(reader);
        more = read && take(reader, (uint8_t)',');
        if (more) {
            skip_space(reader);
        } else {
            read = read && take(reader, (uint8_t)'}');
        }
    }
    skip_space(reader);
    return read && (reader->at == reader->length);
}

/* Python's truth of a value; a missing "on" counts as 1. */
static bool field_true(const json_reader_t *reader,
                       const json_field_t *field)
{
    bool truth = true;
    uint32_t i;

    if ((field->kind == JSON_FALSE) || (field->kind == JSON_NULL) ||
        (field->kind == JSON_NESTED_EMPTY)) {
        truth = false;
    } else if (field->kind == JSON_STRING) {
        truth = field->length > 0U;
    } else if (field->kind == JSON_NUMBER) {
        truth = false; /* zero until a digit says otherwise */
        for (i = 0U; (i < field->length) &&
                     (reader->text[field->start + i] != (uint8_t)'e') &&
                     (reader->text[field->start + i] != (uint8_t)'E');
             i++) {
            const uint8_t byte = reader->text[field->start + i];

            truth = truth || (is_digit(byte) && (byte != (uint8_t)'0'));
        }
    } else {
        /* absent, true, a non-empty array or object */
    }
    return truth;
}

/* "sec": a positive whole number, at most 30; anything else 10. */
static int32_t pump_seconds(const json_reader_t *reader,
                            const json_field_t *field)
{
    int32_t seconds = 0;
    bool whole = (field->kind == JSON_NUMBER) && (field->length > 0U);
    uint32_t i;

    for (i = 0U; whole && (i < field->length); i++) {
        const uint8_t byte = reader->text[field->start + i];

        whole = is_digit(byte) || ((i == 0U) && (byte == (uint8_t)'-'));
        if (whole && is_digit(byte) && (seconds <= PUMP_MAX_S)) {
            const uint8_t digit = byte - (uint8_t)'0';

            seconds = (seconds * (int32_t)TENTHS) + (int32_t)digit;
        }
    }
    if (whole && (reader->text[field->start] == (uint8_t)'-')) {
        seconds = -seconds;
    }
    if (!whole || (seconds <= 0)) {
        seconds = PUMP_DEFAULT_S;
    }
    return (seconds > PUMP_MAX_S) ? PUMP_MAX_S : seconds;
}

/* app_request(): the command and its seconds; unknown is ASK_NONE. */
static ask_t app_request(uint32_t length, int32_t *seconds)
{
    json_reader_t reader = {g_payload, length, 0U};
    app_fields_t fields;
    ask_t ask = ASK_NONE;

    *seconds = 0;
    if (read_object(&reader, &fields)) {
        if (field_is(&reader, &fields.command, "pump")) {
            ask = ASK_PUMP;
            /* "on" 0: off, with no seconds */
            *seconds = field_true(&reader, &fields.on)
                           ? pump_seconds(&reader, &fields.seconds)
                           : 0;
        } else if (field_is(&reader, &fields.command, "ack")) {
            ask = ASK_ACKNOWLEDGE;
        } else if (field_is(&reader, &fields.command, "beep")) {
            ask = ASK_BEEP;
        } else {
            /* not a command the farm knows */
        }
    }
    return ask;
}

/* ---- the screen ---- */

/* The greenhouse card: temperature, the fan light, its threshold. */
static bento_status_t build_house(farm_screen_t *screen)
{
    const rect_t card = {HOUSE_X, CARD_Y, HOUSE_W, CARD_H};
    bento_status_t status = add_card(screen->ui, &card, "โรงเรือน");

    if (status == BENTO_OK) {
        status = add_value(screen->ui, HOUSE_VALUE_X, VALUE_Y,
                           &screen->temperature);
    }
    if (status == BENTO_OK) {
        status = add_led(screen->ui, HOUSE_VALUE_X, LED_Y, COLOR_OK,
                         &screen->fan);
    }
    if (status == BENTO_OK) {
        status = add_line(screen->ui, " ", FAN_AT_X, LED_NOTE_Y,
                          &screen->fan_at);
    }
    return status;
}

/* The plot card: the soil arc, its value, the pump light. */
static bento_status_t build_plot(farm_screen_t *screen)
{
    const rect_t card = {PLOT_X, CARD_Y, PLOT_W, CARD_H};
    const rect_t note = {PUMP_NOTE_X, PUMP_NOTE_Y, 0, 0};
    bento_ui_widget_args_t args;
    bento_ui_id_t unused;
    char text[TEXT_BYTES];
    bento_status_t status = add_card(screen->ui, &card, "แปลงผัก");

    if (status == BENTO_OK) {
        bento_ui_widget_args_init(&args);
        args.x = ARC_X;
        args.y = ARC_Y;
        args.w = ARC_SIZE;
        args.h = ARC_SIZE;
        status = bento_ui_arc_create(screen->ui, &args, &screen->arc);
    }
    if (status == BENTO_OK) {
        status = add_value(screen->ui, SOIL_X, SOIL_Y, &screen->soil);
    }
    if (status == BENTO_OK) {
        status = add_led(screen->ui, PUMP_X, PUMP_Y, COLOR_INFO,
                         &screen->pump);
    }
    if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text), "ปั๊ม <%d%% เปิด >%d%% ปิด",
                       PUMP_ON, PUMP_OFF);
        status = add_label(screen->ui, text, &note, COLOR_DIM, SMALL_PX,
                           &unused);
    }
    return status;
}

/* The pen card: distance, zone, the siren light and its words. */
static bento_status_t build_pen(farm_screen_t *screen)
{
    const rect_t card = {PEN_X, CARD_Y, PEN_W, CARD_H};
    bento_status_t status = add_card(screen->ui, &card, "คอกสัตว์");

    if (status == BENTO_OK) {
        status = add_value(screen->ui, PEN_VALUE_X, VALUE_Y,
                           &screen->distance);
    }
    if (status == BENTO_OK) {
        status = add_line(screen->ui, " ", PEN_VALUE_X, GUARD_Y,
                          &screen->guard);
    }
    if (status == BENTO_OK) {
        status = add_led(screen->ui, PEN_VALUE_X, LED_Y, COLOR_BAD,
                         &screen->siren);
    }
    if (status == BENTO_OK) {
        status = add_line(screen->ui, "ถอยห่างบอร์ด", PEN_NOTE_X,
                          LED_NOTE_Y, &screen->pen);
    }
    return status;
}

/* The bottom box: the note, the app's last command, the report
 * button, and the last JSON under it. */
static bento_status_t build_box(farm_screen_t *screen)
{
    const rect_t card = {BOX_X, BOX_Y, BOX_W, BOX_H};
    const rect_t command = {NOTE_X, COMMAND_Y, 0, 0};
    const rect_t json = {JSON_X, JSON_Y, 0, 0};
    bento_ui_widget_args_t args;
    bento_status_t status = add_card(screen->ui, &card, NULL);

    if (status == BENTO_OK) {
        status =
            add_line(screen->ui, " ", NOTE_X, NOTE_Y, &screen->note);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "แอป: -", &command, COLOR_INFO,
                           TEXT_PX, &screen->command);
    }
    if (status == BENTO_OK) {
        bento_ui_widget_args_init(&args);
        args.text = "ส่งรายงานเลย";
        args.text_len = (uint32_t)strlen(args.text);
        args.x = SEND_X;
        args.y = SEND_Y;
        args.w = SEND_W;
        args.h = SEND_H;
        status =
            bento_ui_button_create(screen->ui, &args, &screen->send);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", &json, COLOR_TEXT, SMALL_PX,
                           &screen->json);
    }
    return status;
}

static bento_status_t build_screen(farm_screen_t *screen)
{
    const rect_t title = {TITLE_X, TITLE_Y, 0, 0};
    bento_ui_id_t unused;
    bento_status_t status =
        bento_ui_screen(screen->ui, BENTO_RUNTIME_SCREEN_WIDTH_PX,
                        BENTO_RUNTIME_SCREEN_HEIGHT_PX);

    if (status == BENTO_OK) {
        status = pause_ms(SCREEN_SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "Smart IoT Gateway", &title,
                           COLOR_TEXT, TITLE_PX, &unused);
    }
    if (status == BENTO_OK) {
        status = build_house(screen);
    }
    if (status == BENTO_OK) {
        status = build_plot(screen);
    }
    if (status == BENTO_OK) {
        status = build_pen(screen);
    }
    if (status == BENTO_OK) {
        status = build_box(screen);
    }
    if (status == BENTO_OK) {
        drain_events(screen->ui);
    }
    return status;
}

/* The greenhouse and the plot on the screen. */
static void show_crops(const farm_screen_t *screen, const farm_t *farm)
{
    bento_ui_t *ui = screen->ui;
    char text[TEXT_BYTES];
    char first[BENTO_FMT_BUF_SIZE];
    char second[BENTO_FMT_BUF_SIZE];

    real_text(farm->temperature, 1, first);
    real_text(farm->humidity, 0, second);
    (void)snprintf(text, sizeof(text), "%s C  %s%%", first, second);
    set_text(ui, screen->temperature, farm->has_air ? text : "--");
    (void)bento_ui_set_value(ui, screen->fan, farm->fan ? 1 : 0);
    real_text(farm->fan_at, 1, first);
    (void)snprintf(text, sizeof(text), "พัดลม > %s C (VR3)", first);
    set_text(ui, screen->fan_at, text);
    (void)bento_ui_set_value(ui, screen->arc, farm->soil);
    (void)bento_ui_set_color(
        ui, screen->arc, (farm->soil < PUMP_ON) ? COLOR_BAD : COLOR_OK);
    (void)snprintf(text, sizeof(text), "%" PRId32 " %%", farm->soil);
    set_text(ui, screen->soil, text);
    (void)bento_ui_set_value(ui, screen->pump, farm->pump ? 1 : 0);
}

/* The pen on the screen. */
static void show_pen(const farm_screen_t *screen, const farm_t *farm)
{
    static const char *const button_names[] = {"SW5", "SW6"};
    bento_ui_t *ui = screen->ui;
    const bool siren = farm->inside && !farm->silenced;
    char text[TEXT_BYTES];

    (void)snprintf(text, sizeof(text), "%" PRId32 " cm",
                   farm->distance_cm);
    set_text(ui, screen->distance, farm->has_target ? text : "--");
    (void)snprintf(text, sizeof(text), "เขต %" PRId32 " cm (VR4)",
                   farm->guard);
    set_text(ui, screen->guard, text);
    (void)bento_ui_set_value(ui, screen->siren, siren ? 1 : 0);
    if (siren) {
        (void)snprintf(text, sizeof(text), "ไซเรน! กด %s",
                       button_names[1]);
    } else {
        (void)snprintf(text, sizeof(text), "บุกรุก %" PRId32 " ครั้ง",
                       farm->count);
    }
    set_text(ui, screen->pen, text);
}

/* ---- the main program ---- */

/* The knobs: soil (simulated), the fan threshold, the pen zone. */
static bento_status_t read_knobs(farm_t *farm)
{
    uint16_t soil = 0U;
    uint16_t fan = 0U;
    uint16_t guard = 0U;
    int32_t fan_part;
    bento_status_t status = bento_pots_read(SOIL_POT, &soil);

    if (status == BENTO_OK) {
        status = bento_pots_read(FAN_POT, &fan);
    }
    if (status == BENTO_OK) {
        status = bento_pots_read(GUARD_POT, &guard);
    }
    fan_part = (int32_t)fan * (int32_t)FAN_SPAN;
    farm->soil = ((int32_t)soil * (int32_t)PERCENT) / (int32_t)POT_FULL;
    farm->fan_at = (bento_real_t)FAN_LO +
                   ((bento_real_t)fan_part / (bento_real_t)POT_FULL);
    farm->guard =
        (int32_t)GUARD_LO +
        (((int32_t)guard * (int32_t)GUARD_SPAN) / (int32_t)POT_FULL);
    return status;
}

/* Sent when the picture changes, and again every 3 s in case a
 * frame was lost. */
static void matrix_tick(farm_t *farm, uint32_t now_ms)
{
    int32_t mode = (farm->fan || farm->pump) ? 1 : 0;

    if (farm->inside && !farm->silenced) {
        mode = MATRIX_INTRUDER;
    }
    if (!farm->has_shown || (mode != farm->shown) ||
        (bento_time_ticks_diff(now_ms, farm->matrix_ms) >=
         MATRIX_RESEND_MS)) {
        farm->has_shown = true;
        farm->shown = mode;
        farm->matrix_ms = now_ms;
        matrix(mode);
    }
}

/* Every TICK_MS: read, decide, act, show. water_down = SW5 held. */
static bento_status_t tick(const farm_screen_t *screen, farm_t *farm,
                           bool water_down, uint32_t now_ms)
{
    int32_t step = farm->remote_ms;
    bento_status_t status = read_air(farm);

    if (status == BENTO_OK) {
        status = read_knobs(farm);
    }
    if (status == BENTO_OK) {
        read_distance(farm);
        /* remote_ms runs down to 0 by TICK_MS a round */
        step = (step > TICK_MS) ? TICK_MS : step;
        step = (step < -TICK_MS) ? -TICK_MS : step;
        farm->remote_ms -= step;
        if (decide(farm, farm->has_target &&
                             (farm->distance_cm < farm->guard))) {
            status = announce_pen(screen, farm);
        }
    }
    if (status == BENTO_OK) {
        farm->pump = (farm->automatic && (farm->remote_ms >= 0)) ||
                     water_down || (farm->remote_ms > 0);
        show_crops(screen, farm);
        show_pen(screen, farm);
        matrix_tick(farm, now_ms);
    }
    return status;
}

/* mqtt.is_connected(): a state that cannot be read is not. */
static bool broker_connected(const farm_t *farm)
{
    uint32_t state = BENTO_MQTT_STATE_CLOSED;

    return (bento_mqtt_state(farm->broker, &state) == BENTO_OK) &&
           (state == BENTO_MQTT_STATE_CONNECTED);
}

/* What the app asked for: the pump, an acknowledge, a beep. */
static bento_status_t obey(const farm_screen_t *screen, farm_t *farm,
                           ask_t ask, int32_t seconds)
{
    static const char *const ask_names[] = {"?", "pump", "ack", "beep"};
    char text[TEXT_BYTES];
    bento_status_t status;

    if (ask == ASK_PUMP) {
        farm->remote_ms =
            (seconds != 0) ? (seconds * SECOND_MS) : REMOTE_OFF_MS;
    }
    farm->silenced = farm->silenced || (ask == ASK_ACKNOWLEDGE);
    if (ask == ASK_BEEP) {
        status = beep_one(screen->ui, NOTE_A4);
    } else if (ask != ASK_NONE) {
        status = beep_one(screen->ui, NOTE_E5);
    } else {
        status = beep_two(screen->ui, NOTE_C6, NOTE_E5);
    }
    if ((status == BENTO_OK) && (ask == ASK_PUMP)) {
        (void)snprintf(text, sizeof(text), "แอป: pump %" PRId32 " วิ",
                       seconds);
        set_text(screen->ui, screen->command, text);
    } else if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text), "แอป: %s",
                       ask_names[(uint32_t)ask]);
        set_text(screen->ui, screen->command, text);
    } else {
        /* Stop during the beep */
    }
    return status;
}

/* The link, then one waiting command (the box holds one). */
static bento_status_t check_net(const farm_screen_t *screen,
                                farm_t *farm)
{
    char topic[BENTO_MQTT_TOPIC_MAX];
    bento_mqtt_msg_t message;
    int32_t seconds = 0;
    bento_status_t status = BENTO_OK;

    if (farm->online && !broker_connected(farm)) {
        farm->online = false;
        show_note(screen, OFFLINE, COLOR_WARN);
    }
    bento_mqtt_msg_init(&message);
    if (farm->online &&
        (bento_mqtt_poll(farm->broker, &message, topic, sizeof(topic),
                         g_payload, sizeof(g_payload)) == BENTO_OK)) {
        const ask_t ask = app_request(message.payload_len, &seconds);

        status = obey(screen, farm, ask, seconds);
    }
    return status;
}

/* Any touch on the report button (the original reads only the
 * handle, so every kind of event counts); ui.poll() answers no
 * events when it fails. */
static bool report_touched(const farm_screen_t *screen)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;
    uint32_t i;
    bool touched = false;

    if (bento_ui_poll(screen->ui, events, BENTO_UI_POLL_EVENTS_MAX,
                      &count, NULL) != BENTO_OK) {
        count = 0U;
    }
    for (i = 0U; i < count; i++) {
        touched = touched || (events[i].id == screen->send);
    }
    return touched;
}

/* The farm's clocks and buttons. */
typedef struct {
    board_button_t water;
    board_button_t acknowledge;
    uint32_t tick_ms;
    uint32_t report_ms;
} farm_loop_t;

/* One pass: buttons, touches, the network, SW6, a tick, a report. */
static bento_status_t farm_round(const farm_screen_t *screen,
                                 farm_t *farm, farm_loop_t *loop)
{
    bool force = false;
    uint32_t now_ms = 0U;
    bento_status_t status =
        wait_ms(WAIT_MS, &loop->water, &loop->acknowledge);

    if (status == BENTO_OK) {
        now_ms = bento_time_ticks_ms();
        force = report_touched(screen);
        status = check_net(screen, farm);
    }
    if ((status == BENTO_OK) && button_clicked(&loop->acknowledge) &&
        farm->inside) {
        farm->silenced = true; /* quiet until the pen changes */
        status = beep_one(screen->ui, NOTE_E5);
    }
    if ((status == BENTO_OK) &&
        (bento_time_ticks_diff(now_ms, loop->tick_ms) >= TICK_MS)) {
        loop->tick_ms = now_ms;
        status = tick(screen, farm, loop->water.down, now_ms);
    }
    if ((status == BENTO_OK) &&
        (force || (bento_time_ticks_diff(now_ms, loop->report_ms) >=
                   REPORT_MS))) {
        loop->report_ms = now_ms;
        report(screen, farm, force ? "touch" : "timer");
    }
    return status;
}

/* Until RUN_MS; Stop or not, the matrix goes dark and the broker is
 * left. */
static bento_status_t run_farm(const farm_screen_t *screen,
                               farm_t *farm)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    farm_loop_t loop = {{WATER_INDEX, false, false},
                        {ACKNOWLEDGE_INDEX, false, false},
                        start_ms,
                        start_ms};
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = farm_round(screen, farm, &loop);
    }
    matrix(MATRIX_OFF);
    if (farm->online) {
        (void)bento_mqtt_close(farm->broker); /* nothing to undo */
    }
    if (status == BENTO_OK) {
        show_note(screen, "จบรอบ", COLOR_DIM);
    }
    return status;
}

/* Online if it can be, the radar's still scene, then the farm. */
static bento_status_t start_farm(const farm_screen_t *screen,
                                 farm_t *farm)
{
    bool learned = false;
    bento_status_t status = go_online(screen, farm);

    if (status == BENTO_OK) {
        status = radar_start(&learned);
    }
    if ((status == BENTO_OK) && !learned) {
        set_text(screen->ui, screen->command, "เรดาร์ไม่ตอบ");
    }
    if (status == BENTO_OK) {
        status = beep_two(screen->ui, NOTE_C5, NOTE_G5);
    }
    if (status == BENTO_OK) {
        status = run_farm(screen, farm);
    }
    return status;
}

int main(void)
{
    farm_screen_t screen;
    farm_t farm;
    bento_status_t status;

    (void)memset(g_payload, 0, sizeof(g_payload));
    (void)memset(g_json_line, 0, sizeof(g_json_line));
    (void)memset(&screen, 0, sizeof(screen));
    (void)memset(&farm, 0, sizeof(farm));
    /* a random tail, so an old session's id does not clash */
    (void)snprintf(farm.client_id, sizeof(farm.client_id),
                   "bento-farm-%s-%04" PRIx32, TEAM,
                   bento_time_ticks_ms() & (uint32_t)ID_SUFFIX_MASK);
    screen.ui = bento_runtime_get_ui();
    /* a build without sound refuses it, as hasattr() skips it */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    status = build_screen(&screen);
    if (status == BENTO_OK) {
        status = start_farm(&screen, &farm);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
