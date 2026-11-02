/*
 * idea_2_delivery.c -- รถส่งผลผลิตถึงตลาด: นาฬิกาเที่ยวรถ กระบะร้อน
 * กี่วินาที ลูกค้ากดรับของ
 * (A farm-to-market delivery: the trip clock, how long the truck bed
 * ran hot, and the customer's receipt)
 *
 * ต่อ WiFi และ broker แล้วเฝ้ากระบะรถ 10 นาที นาฬิกาเที่ยวตัวใหญ่
 * (Seg7) เดินเป็นนาที.วินาที แถบเส้นทาง (Bar) เดินจากฟาร์มถึงตลาดตาม
 * ETA_MIN อ่าน SHT40 ทุกวินาที บวกแดดบนกระบะที่หมุนจาก VR3 (0-10 C)
 * นับวินาทีที่กระบะร้อนเกินเกณฑ์ ระดับเปลี่ยนเมื่อไรส่ง JSON พร้อม
 * hot_s ไปหัวข้อ event เลื่อนคำบน RGB matrix และส่งเสียง ร้อนเกินมี
 * กล่องเตือนและเสียงซ้ำทุก 5 วินาที ลูกค้ากด SW5 หรือแอปส่ง
 * {"cmd":"ack"} = รับของ (ครั้งเดียวต่อเที่ยว) แล้วส่ง event
 * delivered พร้อมเวลาเที่ยว {"cmd":"set","t_hi":..} ตั้งเกณฑ์ร้อนใหม่
 * แก้ WIFI_SSID WIFI_PASS TEAM CROP TEMP_OFFSET และ ETA_MIN ก่อนรัน
 * เกณฑ์เป็นค่าสาธิตในห้อง ไม่ใช่คำแนะนำทางเกษตร
 *
 * Joins Wi-Fi and the broker, then watches a truck bed for 10
 * minutes. A big trip clock (Seg7) runs in minutes.seconds and a
 * route bar (Bar) moves from the farm to the market over ETA_MIN.
 * The SHT40 is read every second, plus sun on the bed from VR3
 * (0-10 C), and the seconds above the limit are counted. Each change
 * of level sends JSON with hot_s to the event topic, scrolls a word
 * on the RGB matrix and plays a tune; too hot also opens a message
 * box and beeps every 5 s. The customer's SW5, or {"cmd":"ack"}
 * from the app, is the receipt (once per trip): a "delivered" event
 * with the trip minutes goes out. {"cmd":"set","t_hi":..} sets a new
 * hot limit. Edit WIFI_SSID, WIFI_PASS, TEAM, CROP, TEMP_OFFSET and
 * ETA_MIN first. The limits are classroom demo values, not farming
 * advice.
 *
 * Twin of idea_2_delivery.py
 * (aiot-development-for-smart-farm/project/idea_2_delivery.py).
 * Style: CLEAN_C.md.
 * Differs from the .py:
 *   - json: the events are built with snprintf, keys in the order
 *     the .py writes them (MicroPython's dict may print them in
 *     another order; a JSON reader does not care). round(x, 1) and
 *     str() of a real number are bento_fmt's, as on the board.
 *   - json.loads: read_command() reads one standard JSON object and
 *     finds "cmd" and "t_hi" at its top level (the last of two equal
 *     keys wins). MicroPython also takes some broken texts (a comma
 *     or colon missing); here those are not JSON. Inside a nested
 *     list or object only strings and brackets are checked. A string
 *     is compared as written: an escaped letter is not decoded.
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
 *   - On a board without VR3, SW5 or the RGB matrix (the .py cannot
 *     import them there) VR3 reads 0, SW5 is never down and the
 *     matrix is skipped; any other failure of them ends the program.
 *   - The wifi and mqtt modules' own console lines are not printed.
 *   - In the BENTO Emulator, MQTT from C is not available yet: the
 *     program says so and ends on the
 *     .py's broker-down screen.
 *   - Stop ends the program quietly, where MicroPython raises
 *     KeyboardInterrupt; the client is closed in both.
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

/* ---- 1) Settings: edit these before running ---- */
/* WIFI_SSID and WIFI_PASS: edit them at the top of join_wifi(). */
/* CROP: edit it at the top of run(). */
/* TEMP_OFFSET: edit it at the top of read_climate(). */
static const char TEAM[] = "teamXX";
/* The demo trip: farm to market in about 10 minutes. */
enum {
    ETA_MIN = 10
};

/* Thai name, English code, temperature and humidity limits. A 27 C
 * room is normal; VR3 half way (+5) is hot. */
typedef struct {
    const char *name;
    const char *code;
    int32_t temp_low_c;
    int32_t temp_high_c;
    int32_t rh_low;
    int32_t rh_high;
} crop_t;

enum {
    CROP_COUNT = 3
};

static const crop_t CROPS[CROP_COUNT] = {
    {"ผักสด", "veg", 15, 30, 20, 95},
    {"ผลไม้", "fruit", 15, 31, 20, 95},
    {"ดอกไม้", "flower", 15, 29, 20, 95},
};

/* Times, limits and the hardware. */
enum {
    T_HI_MAX = 45, /* the highest limit "set" may give */
    READ_MS = 1000,
    BEEP_MS = 5000,
    POLL_MS = 20,
    RUN_MS = 600000,
    SETTLE_MS = 200, /* let the cleared screen settle */
    CONNECT_POLL_MS = 20,
    MS_PER_S = 1000,
    S_PER_MIN = 60,
    MS_PER_MIN = 60000,
    CLOCK_MIN_WRAP = 100, /* the clock shows two minute digits */
    SPEAKER = 40,
    VOLUME = 25,
    NOTE_MS = 90,
    NOTE_GAP_MS = 100,
    MATRIX_MS = 80,
    BROKER_PORT = 1883,
    KEEPALIVE_S = 60,
    BROKER_TRIES = 3,
    CLIENT_SUFFIX_MASK = 0xFFFF,
    SUN_POT = 2, /* VR3 */
    SUN_MAX_C = 10,
    POT_FULL = 4095,
    ANSWER_BUTTON = 1, /* SW5 */
    FAR_C = 3,         /* this far past a limit is level 2 */
    FAR_RH = 10,
    PERCENT = 100,
    TEAM_LENGTH = 6,
    TEAM_PREFIX_LENGTH = 4
};

/* The road theme: asphalt grey, a yellow traffic line, 0xRRGGBB. */
enum {
    COL_TEXT = 0xF5F5F7,
    COL_DIM = 0xA1A1AA,
    COL_CARD = 0x1C1C1E,
    COL_EDGE = 0x3A3A3C,
    COL_ROAD = 0xFFD23F,
    COL_WARN = 0xFF9F0A,
    COL_BAD = 0xFF453A,
    COL_OK = 0x34C759
};

/* The layout: the message box and the cards. */
enum {
    BOX_X = 196,
    BOX_Y = 100,
    BOX_W = 380,
    BOX_H = 140,
    CARD_RADIUS = 6,
    CARD_BORDER_PX = 2,
    CARD_TITLE_DX = 12,
    CARD_TITLE_DY = 6,
    CARD_TITLE_PX = 20,
    AUTO = -1 /* the factory's own size */
};

/* Where the emulator's note goes: free space on this screen. */
enum {
    GAP_X = 340,
    GAP_Y = 330,
    GAP_W = 350, /* wraps the note onto two lines */
    GAP_PX = 20
};

/* Buffer sizes, in bytes. */
enum {
    TEXT_BYTES = 128, /* a label or box text: 126 at most */
    WHY_BYTES = 64,   /* two Thai problems and " + " */
    TOPIC_BYTES = 48,
    CLIENT_BYTES = 32,
    PAYLOAD_BYTES = 256, /* the binding keeps 255 and a NUL */
    TOPIC_KEEP = 64,     /* the program never reads the topic */
    JSON_BYTES = 256
};

/* Levels: 0 fresh, 1 warming, 2 too hot (far past a limit). */
enum {
    LEVEL_NONE = -1,
    LEVEL_OK = 0,
    LEVEL_WARM = 1,
    LEVEL_BAD = 2,
    LEVEL_COUNT = 3
};

/* The tunes, by name. */
enum {
    TUNE_TAP = 0,
    TUNE_GOOD,
    TUNE_BAD,
    TUNE_EMPTY,
    TUNE_COUNT
};

/* The longest tune, in notes. */
enum {
    TUNE_NOTES_MAX = 3
};

/* How each level looks and sounds. */
typedef struct {
    uint32_t color;
    const char *mood; /* the first words of the mood line */
    const char *matrix;
    int32_t matrix_color;
    int32_t tune;
} level_look_t;

static const level_look_t LEVELS[LEVEL_COUNT] = {
    {COL_OK, "ของสดดี", "ON ROAD", BENTO_RGBMATRIX_GREEN, TUNE_GOOD},
    {COL_WARN, "เริ่มร้อน: ", "WARM", BENTO_RGBMATRIX_YELLOW, TUNE_TAP},
    {COL_BAD, "ร้อนเกิน!: ", "HOT", BENTO_RGBMATRIX_RED, TUNE_EMPTY},
};

/* The widgets the program changes after drawing them. */
enum {
    SLOT_CLOCK = 0,
    SLOT_ETA,
    SLOT_CROP,
    SLOT_CLIMATE,
    SLOT_MOOD,
    SLOT_HOT,
    SLOT_ROAD,
    SLOT_LED_OK,
    SLOT_LED_WARM,
    SLOT_LED_BAD,
    SLOT_TRIP,
    SLOT_NOTICE,
    SLOT_STATUS,
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

/* A dark card with its title. */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    const char *title;
} card_t;

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t ids[SLOT_COUNT + 1]; /* + the SLOT_NONE scratch */
    bento_ui_id_t box;                 /* 0 when no box is open */
} screen_t;

typedef struct {
    int32_t temp_low_c;
    int32_t temp_high_c;
    int32_t rh_low;
    int32_t rh_high;
} limits_t;

/* The truck bed being watched, the trip and the link. */
typedef struct {
    const crop_t *crop;
    limits_t limits;
    int32_t level;
    int32_t alerts;
    int32_t hot_s;       /* seconds above the hot limit so far */
    bento_real_t temp_c; /* the last reading, with the sun */
    bool has_reading;    /* false: read again at once */
    bool acknowledged;
    bool delivered;    /* the customer has the goods */
    bool stopped;      /* the .py's raise Stop */
    bool no_mqtt;      /* the BENTO Emulator has no MQTT from C */
    uint32_t start_ms; /* the trip clock's zero */
    uint32_t broker;   /* the MQTT client, 0 when none */
    char topic_event[TOPIC_BYTES];
    char topic_cmd[TOPIC_BYTES];
} watch_t;

typedef struct {
    bento_real_t temp_c; /* with the offset and the sun */
    bento_real_t rh;
    int32_t sun_c;
} reading_t;

typedef struct {
    int32_t level;
    char why_thai[WHY_BYTES];
    char why_english[WHY_BYTES];
} verdict_t;

/* An incoming command: where its fields are in the payload. */
typedef struct {
    bool found;
    bool is_text;
    bool is_integer;
    uint32_t start; /* a text's bytes, inside the quotes */
    uint32_t end;
    int32_t number; /* above any limit when it does not fit */
} json_field_t;

typedef struct {
    const uint8_t *text;
    uint32_t length;
    uint32_t at;
    bool valid; /* false once the text is not JSON */
    json_field_t cmd;
    json_field_t t_hi;
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

static bento_status_t beep(bento_ui_t *ui, int32_t tune)
{
    typedef struct {
        uint8_t notes[TUNE_NOTES_MAX];
        uint32_t count;
    } tune_t;
    static const tune_t tunes[TUNE_COUNT] = {
        {{76U, 0U, 0U}, 1U},
        {{72U, 79U, 84U}, 3U},
        {{84U, 76U, 0U}, 2U},
        {{84U, 76U, 69U}, 3U},
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
static bento_status_t scroll_matrix(const char *text, int32_t color)
{
    bento_status_t status = bento_rgbmatrix_scroll(
        text, (uint32_t)strlen(text), color, MATRIX_MS);

    if (status == BENTO_E_UNSUPPORTED) {
        status = BENTO_OK;
    }
    return status;
}

/* VR3: heat from outside the box, 0-10 C. */
static bento_status_t read_sun(int32_t *sun_c)
{
    uint16_t raw = 0U;
    bento_status_t status = bento_pots_read(SUN_POT, &raw);

    *sun_c = ((int32_t)raw * SUN_MAX_C) / POT_FULL;
    if (status == BENTO_E_UNSUPPORTED) {
        *sun_c = 0;
        status = BENTO_OK;
    }
    return status;
}

/* Any failed read is "no reading": the next round tries again. */
static bool read_climate(reading_t *reading)
{
    /* the board reads above the room: an offset such as -7.0 */
    const bento_real_t temp_offset = (bento_real_t)0.0;
    float temp_c = 0.0F;
    float rh = 0.0F;
    bool read = bento_sensors_sht40_temperature(&temp_c) == BENTO_OK;

    read = read && (bento_sensors_sht40_humidity(&rh) == BENTO_OK);
    reading->temp_c = (bento_real_t)temp_c + temp_offset;
    reading->rh = (bento_real_t)rh;
    return read;
}

static bento_status_t read_button(bool *down)
{
    bento_status_t status = bento_buttons_pressed(ANSWER_BUTTON, down);

    if (status == BENTO_E_UNSUPPORTED) {
        *down = false;
        status = BENTO_OK;
    }
    return status;
}

/* ---- 3) The brain ---- */

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

static void join_word(char *text, uint32_t size, const char *separator,
                      const char *word)
{
    const uint32_t used = (uint32_t)strlen(text);

    if (used < size) {
        (void)snprintf(&text[used], size - used, "%s%s",
                       (used == 0U) ? "" : separator, word);
    }
}

/* Level 2 is far out: past a limit by 3 C or by 10 %RH. */
static void judge(const reading_t *reading, const limits_t *limits,
                  verdict_t *verdict)
{
    static const char *const problems[][2] = {
        {"กระบะเย็นจัด", "cold"},
        {"กระบะร้อน", "hot"},
        {"ลมแห้ง", "dry"},
        {"ชื้นอบ", "wet"},
    };
    const bento_real_t temp_c = reading->temp_c;
    const bento_real_t rh = reading->rh;
    const bool found[] = {
        (temp_c < (bento_real_t)limits->temp_low_c),
        (temp_c > (bento_real_t)limits->temp_high_c),
        (rh < (bento_real_t)limits->rh_low),
        (rh > (bento_real_t)limits->rh_high),
    };
    const bool far =
        (temp_c < (bento_real_t)(limits->temp_low_c - FAR_C)) ||
        (temp_c > (bento_real_t)(limits->temp_high_c + FAR_C)) ||
        (rh < (bento_real_t)(limits->rh_low - FAR_RH)) ||
        (rh > (bento_real_t)(limits->rh_high + FAR_RH));
    uint32_t i;

    verdict->why_thai[0] = '\0';
    verdict->why_english[0] = '\0';
    for (i = 0U; i < (sizeof(found) / sizeof(found[0])); i++) {
        if (found[i]) {
            join_word(verdict->why_thai, WHY_BYTES, " + ",
                      problems[i][0]);
            join_word(verdict->why_english, WHY_BYTES, "+",
                      problems[i][1]);
        }
    }
    if (verdict->why_thai[0] == '\0') {
        verdict->level = LEVEL_OK;
        join_word(verdict->why_thai, WHY_BYTES, "", "ของสดดี");
        join_word(verdict->why_english, WHY_BYTES, "", "ok");
    } else {
        verdict->level = far ? LEVEL_BAD : LEVEL_WARM;
    }
}

/* round(x, 1) as MicroPython does it, in the board's float width. */
static bento_real_t round_tenth(bento_real_t value)
{
    const bento_real_t scale = (bento_real_t)10.0;
    const bento_real_t scaled = value * scale;

    return (bento_real_t)nearbyint((double)scaled) / scale;
}

/* str() of a real number, as json.dumps writes it. */
static void format_real(char *text, bento_real_t value)
{
    uint32_t length = 0U;

    /* a BENTO_FMT_BUF_SIZE buffer holds any real number */
    (void)bento_fmt_real(text, BENTO_FMT_BUF_SIZE, value, &length);
}

/* "%.1f" of a real number. */
static void format_tenths(char *text, bento_real_t value)
{
    uint32_t length = 0U;

    (void)bento_fmt_real_precision(text, BENTO_FMT_BUF_SIZE, value, 1,
                                   &length);
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
static bento_status_t try_broker(watch_t *watch, bool *linked)
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
    status = bento_mqtt_open_start(&config, &watch->broker);
    watch->no_mqtt = status == BENTO_E_UNSUPPORTED;
    if (status == BENTO_OK) {
        status = wait_for_broker(watch->broker);
    }
    *linked = status == BENTO_OK;
    if (!*linked && (watch->broker != 0U)) {
        (void)bento_mqtt_close(watch->broker); /* a failed attempt */
        watch->broker = 0U;
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

static void show_status(const screen_t *screen, const char *text,
                        uint32_t color)
{
    show_color(screen, SLOT_STATUS, color);
    show_text(screen, SLOT_STATUS, text);
}

static void show_link(const screen_t *screen, bool linked)
{
    show_value(screen, SLOT_LINK_LED, linked ? 1 : 0);
    show_text(screen, SLOT_LINK_TEXT,
              linked ? "MQTT: ต่อแล้ว" : "MQTT: ออฟไลน์");
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
        UI_WIDGET_PANEL, SLOT_NONE,   NULL,          card->x,
        card->y,         card->w,     card->h,       COL_CARD,
        COL_EDGE,        CARD_RADIUS, CARD_BORDER_PX};
    const widget_spec_t title = {UI_WIDGET_LABEL,
                                 SLOT_NONE,
                                 card->title,
                                 px(card->x + CARD_TITLE_DX),
                                 px(card->y + CARD_TITLE_DY),
                                 AUTO,
                                 AUTO,
                                 COL_ROAD,
                                 0,
                                 0,
                                 CARD_TITLE_PX};
    bento_status_t status = add_widget(screen, &panel);

    if (status == BENTO_OK) {
        status = add_widget(screen, &title);
    }
    return status;
}

static bento_status_t add_card_with(screen_t *screen,
                                    const card_t *card,
                                    const widget_spec_t *specs,
                                    uint32_t count)
{
    bento_status_t status = add_card(screen, card);

    if (status == BENTO_OK) {
        status = add_widgets(screen, specs, count);
    }
    return status;
}

/* The title and the trip clock card. */
static bento_status_t add_clock_card(screen_t *screen)
{
    static const widget_spec_t title = {UI_WIDGET_LABEL,
                                        SLOT_NONE,
                                        "รถส่งผลผลิตถึงตลาด",
                                        12,
                                        6,
                                        AUTO,
                                        AUTO,
                                        COL_TEXT,
                                        0,
                                        0,
                                        24};
    static const card_t card = {12, 44, 372, 160,
                                "นาฬิกาเที่ยวนี้ (นาที.วินาที)"};
    static const widget_spec_t clock = {
        UI_WIDGET_SEG7, SLOT_CLOCK, "00.00", 24, 80, 240, 80,
        COL_ROAD,       0,          0,       0};
    char eta_text[TEXT_BYTES];
    bento_status_t status = add_widget(screen, &title);

    (void)snprintf(eta_text, sizeof(eta_text),
                   "ถึงตลาดอีกราว %" PRId32 " นาที", (int32_t)ETA_MIN);
    if (status == BENTO_OK) {
        status = add_card_with(screen, &card, &clock, 1U);
    }
    if (status == BENTO_OK) {
        const widget_spec_t eta = {
            UI_WIDGET_LABEL, SLOT_ETA, eta_text, 24, 166, AUTO, AUTO,
            COL_DIM,         0,        0,        20};

        status = add_widget(screen, &eta);
    }
    return status;
}

static bento_status_t add_bed_card(screen_t *screen)
{
    static const card_t card = {394, 44, 386, 160, "ท้ายกระบะ"};
    static const widget_spec_t specs[] = {
        {UI_WIDGET_LABEL, SLOT_CROP, "-", 406, 80, AUTO, AUTO, COL_DIM,
         0, 0, 20},
        {UI_WIDGET_LABEL, SLOT_CLIMATE, "-- C  -- %", 406, 108, AUTO,
         AUTO, COL_TEXT, 0, 0, 24},
        {UI_WIDGET_LABEL, SLOT_MOOD, "...", 406, 142, AUTO, AUTO,
         COL_WARN, 0, 0, 20},
        {UI_WIDGET_LABEL, SLOT_HOT, "ร้อนมาแล้ว 0 วิ", 406, 172, AUTO,
         AUTO, COL_DIM, 0, 0, 20},
    };

    return add_card_with(screen, &card, specs,
                         sizeof(specs) / sizeof(specs[0]));
}

/* The route from the farm to the market, and the level lamps. */
static bento_status_t add_route_card(screen_t *screen)
{
    static const card_t card = {12, 214, 768, 82, "เส้นทาง"};
    static const widget_spec_t specs[] = {
        {UI_WIDGET_LABEL, SLOT_NONE, "ฟาร์ม", 24, 252, AUTO, AUTO,
         COL_DIM, 0, 0, 20},
        {UI_WIDGET_BAR, SLOT_ROAD, NULL, 80, 254, 470, 20, 0U, 0,
         PERCENT, 0},
        {UI_WIDGET_LABEL, SLOT_NONE, "ตลาด", 560, 252, AUTO, AUTO,
         COL_DIM, 0, 0, 20},
        {UI_WIDGET_LED, SLOT_LED_OK, NULL, 620, 254, 18, 18, COL_OK, 0,
         0, 0},
        {UI_WIDGET_LED, SLOT_LED_WARM, NULL, 672, 254, 18, 18, COL_WARN,
         0, 0, 0},
        {UI_WIDGET_LED, SLOT_LED_BAD, NULL, 724, 254, 18, 18, COL_BAD,
         0, 0, 0},
    };

    return add_card_with(screen, &card, specs,
                         sizeof(specs) / sizeof(specs[0]));
}

/* The trip line, the hint, the notice, the status and the link. */
static bento_status_t add_footer(screen_t *screen)
{
    static const widget_spec_t specs[] = {
        {UI_WIDGET_LABEL, SLOT_TRIP, "กำลังขนส่ง", 12, 300, AUTO, AUTO,
         COL_ROAD, 0, 0, 20},
        {UI_WIDGET_LABEL, SLOT_NONE,
         "VR3 = แดดบนกระบะ  SW5 = ลูกค้ารับของ", 260, 300, AUTO, AUTO,
         COL_DIM, 0, 0, 20},
        {UI_WIDGET_LABEL, SLOT_NOTICE, "-", 12, 328, AUTO, AUTO,
         COL_DIM, 0, 0, 20},
        {UI_WIDGET_LABEL, SLOT_STATUS, "กำลังเริ่ม", 12, 356, AUTO, AUTO,
         COL_DIM, 0, 0, 20},
        {UI_WIDGET_LED, SLOT_LINK_LED, NULL, 606, 12, 18, 18, COL_OK, 0,
         0, 0},
        {UI_WIDGET_LABEL, SLOT_LINK_TEXT, "MQTT: ออฟไลน์", 632, 10, AUTO,
         AUTO, COL_DIM, 0, 0, 20},
    };

    return add_widgets(screen, specs, sizeof(specs) / sizeof(specs[0]));
}

/* The whole screen in the .py's order. The road bar's colour is set
 * once everything is drawn: the same look. */
static bento_status_t build_screen(screen_t *screen)
{
    bento_status_t status =
        bento_ui_screen(screen->ui, BENTO_RUNTIME_SCREEN_WIDTH_PX,
                        BENTO_RUNTIME_SCREEN_HEIGHT_PX);

    if (status == BENTO_OK) {
        status = rest(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_clock_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_bed_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_route_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_footer(screen);
    }
    if (status == BENTO_OK) {
        /* on the Emulator a Bar's colour paints its track */
        show_color(screen, SLOT_ROAD, COL_ROAD);
        drain_events(screen->ui);
    }
    return status;
}

static void show_notice(const screen_t *screen, const watch_t *watch,
                        const char *text, uint32_t color)
{
    char line[TEXT_BYTES];

    (void)snprintf(line, sizeof(line), "แจ้งไปแล้ว %" PRId32 " ครั้ง: %s",
                   watch->alerts, text);
    show_color(screen, SLOT_NOTICE, color);
    show_text(screen, SLOT_NOTICE, line);
}

static void show_crop(const screen_t *screen, const watch_t *watch)
{
    char line[TEXT_BYTES];

    (void)snprintf(line, sizeof(line), "%s ไม่ควรเกิน %" PRId32 " C",
                   watch->crop->name, watch->limits.temp_high_c);
    show_text(screen, SLOT_CROP, line);
}

/* The trip clock, the route bar and the time to the market. */
static void show_trip(const screen_t *screen, int32_t elapsed_ms)
{
    char line[TEXT_BYTES];
    const int32_t seconds = elapsed_ms / MS_PER_S;
    const int32_t travelled = (seconds * (int32_t)PERCENT) /
                              ((int32_t)ETA_MIN * (int32_t)S_PER_MIN);
    const int32_t left_min =
        (int32_t)ETA_MIN - (seconds / (int32_t)S_PER_MIN);

    (void)snprintf(line, sizeof(line), "%02" PRId32 ".%02" PRId32,
                   (seconds / S_PER_MIN) % CLOCK_MIN_WRAP,
                   seconds % S_PER_MIN);
    show_text(screen, SLOT_CLOCK, line);
    show_value(screen, SLOT_ROAD,
               (travelled < PERCENT) ? travelled : PERCENT);
    if (left_min > 0) {
        (void)snprintf(line, sizeof(line),
                       "ถึงตลาดอีกราว %" PRId32 " นาที", left_min);
        show_text(screen, SLOT_ETA, line);
    } else {
        show_text(screen, SLOT_ETA, "เลยเวลาที่คาดไว้แล้ว");
    }
}

/* The bed's climate, the mood line, the hot seconds and the lamps. */
static void show_reading(const screen_t *screen, const watch_t *watch,
                         const reading_t *reading,
                         const verdict_t *verdict)
{
    char number[BENTO_FMT_BUF_SIZE];
    char line[TEXT_BYTES];
    const int32_t level = verdict->level;
    int32_t i;

    format_tenths(number, reading->temp_c);
    (void)snprintf(line, sizeof(line),
                   "%s C  %" PRId32 " %%  แดด +%" PRId32, number,
                   (int32_t)reading->rh, reading->sun_c);
    show_text(screen, SLOT_CLIMATE, line);
    show_color(screen, SLOT_MOOD, LEVELS[level].color);
    (void)snprintf(line, sizeof(line), "%s%s", LEVELS[level].mood,
                   (level == LEVEL_OK) ? "" : verdict->why_thai);
    show_text(screen, SLOT_MOOD, line);
    (void)snprintf(line, sizeof(line), "ร้อนมาแล้ว %" PRId32 " วิ",
                   watch->hot_s);
    show_text(screen, SLOT_HOT, line);
    show_color(screen, SLOT_HOT,
               (watch->hot_s == 0) ? COL_DIM : COL_WARN);
    for (i = 0; i < LEVEL_COUNT; i++) {
        show_value(screen, (uint32_t)SLOT_LED_OK + (uint32_t)i,
                   (i == level) ? 1 : 0);
    }
}

static void close_box(screen_t *screen)
{
    if (screen->box != 0U) {
        /* delete() ignores a refusal in MicroPython (modui.c) */
        (void)bento_ui_delete(screen->ui, screen->box);
        screen->box = 0U;
    }
}

/* The box takes 126 bytes at most; the longest text here fits. */
static bento_status_t open_box(screen_t *screen, const char *why)
{
    char text[TEXT_BYTES];
    bento_ui_widget_args_t args;

    (void)snprintf(text, sizeof(text), "ร้อนเกิน!\n%s\nจอดตรวจ กด SW5",
                   why);
    bento_ui_widget_args_init(&args);
    args.text = text;
    args.text_len = (uint32_t)strlen(text);
    args.x = BOX_X;
    args.y = BOX_Y;
    args.w = BOX_W;
    args.h = BOX_H;
    args.font_px = 0;
    return bento_ui_msgbox_create(screen->ui, &args, &screen->box);
}

/* ---- 6) The program ---- */

/* The .py's stop(): say why on the screen, then end politely. */
static void stop_here(screen_t *screen, watch_t *watch,
                      const char *text, uint32_t color)
{
    show_link(screen, false);
    /* the program ends either way: the matrix is only tidied */
    (void)bento_rgbmatrix_scroll(NULL, 0U,
                                 BENTO_RGBMATRIX_SCROLL_COLOR_DEFAULT,
                                 BENTO_RGBMATRIX_SCROLL_MS_DEFAULT);
    (void)bento_rgbmatrix_clear();
    close_box(screen);
    show_status(screen, text, color);
    drain_events(screen->ui);
    watch->stopped = true;
}

static void start_watch(watch_t *watch, const crop_t *crop)
{
    watch->crop = crop;
    watch->limits.temp_low_c = crop->temp_low_c;
    watch->limits.temp_high_c = crop->temp_high_c;
    watch->limits.rh_low = crop->rh_low;
    watch->limits.rh_high = crop->rh_high;
    watch->level = LEVEL_NONE;
    watch->acknowledged = true;
    watch->alerts = 0;
    watch->has_reading = false;
    watch->delivered = false;
    watch->hot_s = 0;
}

static bool publish(const watch_t *watch, const char *json)
{
    const bento_status_t status = bento_mqtt_publish(
        watch->broker, watch->topic_event,
        (uint32_t)strlen(watch->topic_event), (const uint8_t *)json,
        (uint32_t)strlen(json), 0U, 0U);

    /* false only when the link is gone (the .py's OSError) */
    return (status != BENTO_E_SESSION) && (status != BENTO_E_STALE);
}

/* json.dumps of a level event. */
static bool send_event(const watch_t *watch, const reading_t *reading,
                       const verdict_t *verdict)
{
    char json[JSON_BYTES];
    char temp_c[BENTO_FMT_BUF_SIZE];
    char rh[BENTO_FMT_BUF_SIZE];

    format_real(temp_c, round_tenth(reading->temp_c));
    format_real(rh, round_tenth(reading->rh));
    (void)snprintf(
        json, sizeof(json),
        "{\"id\": \"%s\", \"crop\": \"%s\", \"level\": %" PRId32
        ", \"alert\": \"%s\", \"temp_c\": %s, \"rh\": %s, "
        "\"sun_c\": %" PRId32 ", \"hot_s\": %" PRId32 "}",
        TEAM, watch->crop->code, verdict->level, verdict->why_english,
        temp_c, rh, reading->sun_c, watch->hot_s);
    return publish(watch, json);
}

/* json.dumps of the receipt; temp_c is null with no reading. */
static bool send_delivered(const watch_t *watch, bool by_button,
                           int32_t trip_min)
{
    char json[JSON_BYTES];
    char temp_c[BENTO_FMT_BUF_SIZE];

    if (watch->has_reading) {
        format_real(temp_c, round_tenth(watch->temp_c));
    } else {
        (void)snprintf(temp_c, sizeof(temp_c), "null");
    }
    (void)snprintf(json, sizeof(json),
                   "{\"id\": \"%s\", \"event\": \"delivered\", \"by\": "
                   "\"%s\", \"trip_min\": %" PRId32
                   ", \"temp_c\": %s, \"hot_s\": %" PRId32 "}",
                   TEAM, by_button ? "sw5" : "app", trip_min, temp_c,
                   watch->hot_s);
    return publish(watch, json);
}

/* The matrix, the tune, the notice and, when too hot, the box. */
static bento_status_t announce_level(screen_t *screen,
                                     const watch_t *watch,
                                     const verdict_t *verdict)
{
    const int32_t level = verdict->level;
    const bool hot = level == LEVEL_BAD;
    bento_status_t status =
        scroll_matrix(LEVELS[level].matrix, LEVELS[level].matrix_color);

    if (status == BENTO_OK) {
        status = beep(screen->ui, LEVELS[level].tune);
    }
    if (status == BENTO_OK) {
        show_notice(screen, watch,
                    hot ? "คนขับ: ตรวจความเย็น" : "แจ้งมือถือแล้ว",
                    hot ? COL_BAD : COL_DIM);
        close_box(screen);
    }
    if ((status == BENTO_OK) && hot && !watch->delivered) {
        status = open_box(screen, verdict->why_thai);
    }
    return status;
}

/* The level changed: send first, then the matrix, the tune and the
 * box. */
static bento_status_t on_level(screen_t *screen, watch_t *watch,
                               const reading_t *reading,
                               const verdict_t *verdict)
{
    const bool hot = verdict->level == LEVEL_BAD;
    bento_status_t status = BENTO_OK;

    watch->level = verdict->level;
    watch->acknowledged = !hot || watch->delivered;
    watch->alerts++;
    if (!send_event(watch, reading, verdict)) {
        stop_here(screen, watch, "สายหลุดตอนส่ง", COL_BAD);
    } else {
        status = announce_level(screen, watch, verdict);
    }
    return status;
}

/* One reading; *alerted when the level changed and went out. */
static bento_status_t read_and_judge(screen_t *screen, watch_t *watch,
                                     bool *alerted)
{
    reading_t reading = {(bento_real_t)0.0, (bento_real_t)0.0, 0};
    verdict_t verdict;
    bento_status_t status = read_sun(&reading.sun_c);

    *alerted = false;
    watch->has_reading = (status == BENTO_OK) && read_climate(&reading);
    if (watch->has_reading) {
        reading.temp_c += (bento_real_t)reading.sun_c;
        watch->temp_c = reading.temp_c;
        if ((reading.temp_c >
             (bento_real_t)watch->limits.temp_high_c) &&
            !watch->delivered) {
            watch->hot_s += READ_MS / MS_PER_S; /* read every second */
        }
        judge(&reading, &watch->limits, &verdict);
        show_reading(screen, watch, &reading, &verdict);
        if (verdict.level != watch->level) {
            *alerted = true;
            status = on_level(screen, watch, &reading, &verdict);
        }
    }
    return status;
}

/* ---- Reading an incoming command ---- */

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

/* One "key": value pair; the value lands in the field it names. */
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
    if (command->valid && text_is(command, &key, "t_hi")) {
        command->t_hi = value;
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

/* {"cmd":"set","t_hi":..}: a new limit, an integer in a sensible
 * range; the box is judged again and reported at once. */
static bento_status_t set_limit(screen_t *screen, watch_t *watch,
                                const json_field_t *t_hi)
{
    bento_status_t status = BENTO_OK;

    if (t_hi->found && t_hi->is_integer &&
        (watch->limits.temp_low_c < t_hi->number) &&
        (t_hi->number <= T_HI_MAX)) {
        watch->limits.temp_high_c = t_hi->number;
        watch->level = LEVEL_NONE;
        watch->has_reading = false;
        show_crop(screen, watch);
        status = beep(screen->ui, TUNE_TAP);
    }
    return status;
}

/* A command from the app; *answered for an ack, an unknown one is
 * silent. Room for the team's "led" and "pump" commands here. */
static bento_status_t on_command(screen_t *screen, watch_t *watch,
                                 const uint8_t *payload,
                                 uint32_t length, bool *answered)
{
    command_t command;
    bento_status_t status = BENTO_OK;

    (void)memset(&command, 0, sizeof(command));
    command.text = payload;
    command.length = length;
    command.valid = true;
    read_command(&command);
    *answered = command.valid && text_is(&command, &command.cmd, "ack");
    if (command.valid && text_is(&command, &command.cmd, "set")) {
        status = set_limit(screen, watch, &command.t_hi);
    }
    return status;
}

/* get_message(): the newest message waiting, if any. */
static bento_status_t take_command(screen_t *screen, watch_t *watch,
                                   bool *answered)
{
    uint8_t payload[PAYLOAD_BYTES];
    char topic[TOPIC_KEEP];
    bento_mqtt_msg_t info;
    uint32_t length = 0U;
    bool found = false;
    bool more = true;
    bento_status_t status = BENTO_OK;

    while (more) {
        bento_mqtt_msg_init(&info);
        more =
            bento_mqtt_poll(watch->broker, &info, topic, sizeof(topic),
                            payload, PAYLOAD_BYTES - 1U) == BENTO_OK;
        if (more) {
            length = info.payload_len;
            found = true;
        }
    }
    *answered = false;
    if (found) {
        status = on_command(screen, watch, payload, length, answered);
    }
    return status;
}

/* SW5: the customer takes the goods at the truck, no network. */
static bento_status_t check_button(screen_t *screen, watch_t *watch,
                                   bool *was_down, bool *answered,
                                   bool *by_button)
{
    bool down = false;
    bento_status_t status = read_button(&down);

    if ((status == BENTO_OK) && down && !*was_down) {
        *answered = true;
        *by_button = true;
        if (watch->delivered) { /* already received: show it works */
            show_notice(screen, watch, "รับของไปแล้ว", COL_DIM);
            status = beep(screen->ui, TUNE_TAP);
        }
    }
    *was_down = down;
    return status;
}

/* The customer has the goods: once per trip, quiet, box closed, the
 * receipt on the screen, the matrix and the broker. */
static bento_status_t deliver(screen_t *screen, watch_t *watch,
                              bool by_button, uint32_t now_ms)
{
    char line[TEXT_BYTES];
    const int32_t trip_min =
        bento_time_ticks_diff(now_ms, watch->start_ms) / MS_PER_MIN;
    bento_status_t status;

    watch->delivered = true;
    watch->acknowledged = true;
    close_box(screen);
    show_notice(screen, watch, "ลูกค้ารับของแล้ว", COL_OK);
    show_color(screen, SLOT_TRIP, COL_OK);
    (void)snprintf(line, sizeof(line), "ส่งถึงแล้ว %" PRId32 " นาที",
                   trip_min);
    show_text(screen, SLOT_TRIP, line);
    status = scroll_matrix("DELIVERED", BENTO_RGBMATRIX_CYAN);
    if (status == BENTO_OK) {
        status = beep(screen->ui, TUNE_GOOD);
    }
    if ((status == BENTO_OK) &&
        !send_delivered(watch, by_button, trip_min)) {
        stop_here(screen, watch, "สายหลุดตอนส่ง", COL_BAD);
    }
    return status;
}

typedef struct {
    uint32_t read_ms;
    uint32_t beep_ms;
} timers_t;

/* Read every second (at once when there is no reading), move the trip
 * clock until delivery, and beep again every 5 s while too hot and
 * not answered. */
static bento_status_t keep_time(screen_t *screen, watch_t *watch,
                                timers_t *timers, uint32_t now_ms)
{
    bool alerted = false;
    bento_status_t status = BENTO_OK;

    if (!watch->has_reading ||
        (bento_time_ticks_diff(now_ms, timers->read_ms) >= READ_MS)) {
        timers->read_ms = now_ms;
        if (!watch->delivered) {
            show_trip(screen,
                      bento_time_ticks_diff(now_ms, watch->start_ms));
        }
        status = read_and_judge(screen, watch, &alerted);
        if (alerted) {
            timers->beep_ms = now_ms;
        }
    }
    if ((status == BENTO_OK) && !watch->stopped &&
        (watch->level == LEVEL_BAD) && !watch->acknowledged &&
        (bento_time_ticks_diff(now_ms, timers->beep_ms) >= BEEP_MS)) {
        timers->beep_ms = now_ms;
        status = beep(screen->ui, TUNE_BAD);
    }
    return status;
}

/* One round: listen every 20 ms, read the sensor every second. */
static bento_status_t watch_once(screen_t *screen, watch_t *watch,
                                 timers_t *timers, bool *was_down)
{
    const uint32_t now_ms = bento_time_ticks_ms();
    bool answered = false;
    bool by_button = false; /* else the app */
    bento_status_t status = take_command(screen, watch, &answered);

    if (status == BENTO_OK) {
        status = check_button(screen, watch, was_down, &answered,
                              &by_button);
    }
    if ((status == BENTO_OK) && answered && !watch->delivered) {
        status = deliver(screen, watch, by_button, now_ms);
    }
    if ((status == BENTO_OK) && !watch->stopped) {
        drain_events(screen->ui);
        status = keep_time(screen, watch, timers, now_ms);
    }
    if ((status == BENTO_OK) && !watch->stopped) {
        status = rest(POLL_MS);
    }
    return status;
}

static bento_status_t watch_trip(screen_t *screen, watch_t *watch)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    timers_t timers = {start_ms, start_ms};
    bool was_down = false;
    bento_status_t status = BENTO_OK;

    watch->start_ms = start_ms;
    while ((status == BENTO_OK) && !watch->stopped &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = watch_once(screen, watch, &timers, &was_down);
    }
    if ((status == BENTO_OK) && !watch->stopped) {
        stop_here(screen, watch, "จบรอบ", COL_DIM);
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
    args.color = COL_WARN;
    args.font_px = GAP_PX;
    return bento_ui_label_create(ui, &args, &label);
}

/* The public broker is down now and then: 3 tries. */
static bento_status_t connect_broker(const screen_t *screen,
                                     watch_t *watch, bool *linked)
{
    char line[TEXT_BYTES];
    int32_t attempt;
    bento_status_t status = BENTO_OK;

    *linked = false;
    for (attempt = 1;
         (attempt <= BROKER_TRIES) && (status == BENTO_OK) &&
         !*linked && !watch->no_mqtt;
         attempt++) {
        if (attempt > 1) {
            (void)snprintf(line, sizeof(line),
                           "ลองต่อ broker ใหม่ %" PRId32 "/3", attempt);
            show_status(screen, line, COL_WARN);
            drain_events(screen->ui);
        }
        status = try_broker(watch, linked);
    }
    return status;
}

/* Wi-Fi, its address, the broker and the subscription; *problem
 * names the step that failed, NULL when all are up. */
static bento_status_t connect_farm(const screen_t *screen,
                                   watch_t *watch, const char **problem)
{
    uint8_t granted_qos = 0U;
    bool joined = false;
    bool linked = false;
    bento_status_t status;

    *problem = NULL;
    show_status(screen, "ต่อ WiFi...", COL_WARN);
    drain_events(screen->ui); /* the label shows before the wait */
    status = join_wifi(&joined);
    if ((status == BENTO_OK) && !joined) {
        *problem = "WiFi ไม่ติด: ตรวจชื่อ/รหัส";
    }
    if ((status == BENTO_OK) && joined) {
        status = connect_broker(screen, watch, &linked);
    }
    if ((status == BENTO_OK) && watch->no_mqtt) {
        status = show_emulator_gap(screen->ui);
    }
    /* subscribe always after connect */
    if ((status == BENTO_OK) && linked) {
        linked =
            bento_mqtt_subscribe(watch->broker, watch->topic_cmd,
                                 (uint32_t)strlen(watch->topic_cmd), 0U,
                                 &granted_qos) == BENTO_OK;
    }
    if ((status == BENTO_OK) && joined && !linked) {
        *problem = "broker ไม่ตอบ: รอ 1 นาทีแล้วรันใหม่";
    }
    return status;
}

static int32_t find_crop(const char *name)
{
    int32_t found = -1;
    int32_t i;

    for (i = 0; (i < CROP_COUNT) && (found < 0); i++) {
        if (strcmp(CROPS[i].name, name) == 0) {
            found = i;
        }
    }
    return found;
}

/* From the settings check to the end of the watch. */
static bento_status_t run(screen_t *screen, watch_t *watch)
{
    static const char crop_name[] = "ผักสด"; /* CROP: one of CROPS */
    const int32_t crop = find_crop(crop_name);
    const char *problem = NULL;
    bento_status_t status = BENTO_OK;

    if (crop < 0) {
        stop_here(screen, watch, "แก้ CROP ให้ตรงกับ CROPS", COL_BAD);
    } else {
        start_watch(watch, &CROPS[crop]);
        show_crop(screen, watch);
    }
    if (!watch->stopped && !valid_team(TEAM)) {
        stop_here(screen, watch, "แก้ TEAM ก่อน", COL_BAD);
    }
    if (!watch->stopped) {
        status = connect_farm(screen, watch, &problem);
    }
    if ((status == BENTO_OK) && (problem != NULL)) {
        stop_here(screen, watch, problem, COL_BAD);
    }
    if ((status == BENTO_OK) && !watch->stopped) {
        show_link(screen, true);
        show_status(screen, "ออกรถแล้ว เฝ้ากระบะอยู่", COL_OK);
        status = watch_trip(screen, watch);
    }
    return status;
}

int main(void)
{
    static const char root[] = "bento-aiot";
    screen_t screen;
    watch_t watch;
    bento_status_t status;

    (void)memset(&screen, 0, sizeof(screen));
    (void)memset(&watch, 0, sizeof(watch));
    screen.ui = bento_runtime_get_ui();
    (void)snprintf(watch.topic_event, sizeof(watch.topic_event),
                   "%s/%s/event", root, TEAM);
    (void)snprintf(watch.topic_cmd, sizeof(watch.topic_cmd),
                   "%s/%s/cmd", root, TEAM);
    /* a build without sound refuses it, as hasattr() skips it */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    status = build_screen(&screen);
    if (status == BENTO_OK) {
        status = run(&screen, &watch);
    }
    if (watch.broker != 0U) {
        /* the .py ignores every error of its disconnect */
        (void)bento_mqtt_close(watch.broker);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
