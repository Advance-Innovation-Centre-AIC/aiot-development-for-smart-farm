/*
 * cp3_06_practise_solution.c -- เฉลย: สร้างกรอบ Modbus TCP FC06 เอง
 * (Code Quest level 3 answer: build a Modbus TCP FC06 frame)
 *
 * เฉลยของแบบฝึก cp3_06_practise: ช่อง A B ใน fc06_write() เติมครบแล้ว
 * โปรแกรมคือ cp3_06_modbus_frame ในโหมดดูกรอบ (ไม่มีส่วนเครือข่าย)
 * fc06_write() สร้างกรอบสองท่อน: PDU 5 ไบต์ (fc, addr, value) แล้วต่อท้าย
 * MBAP 7 ไบต์ ก่อนเปิดจอ โปรแกรมตรวจ 3 กรอบ (self_test) เทียบกับกรอบที่รู้
 * คำตอบแน่นอน ผ่านครบ = Console ขึ้น "ผ่าน!" แล้วเล่นต่อได้: SW5 = กรอบ
 * FC03, SW6 = กรอบ FC06 (สลับ 1/0) จอโชว์ 12 ไบต์ ฟ้า = MBAP เขียว = PDU
 * เล่น 3 นาทีแล้วจบเอง ใช้กับ TESAIoT Dev Kit และ BENTO Emulator
 *
 * The answer to the cp3_06_practise blanks A and B in fc06_write().
 * The program is cp3_06_modbus_frame in frame-view mode, without the
 * network. fc06_write() builds the frame in two parts: a 5-byte PDU
 * (fc, addr, value), then the 7-byte MBAP header in front of it.
 * Before the screen opens, self_test checks 3 frames against known
 * answers; when all pass the console shows "ผ่าน!" and SW5 shows an
 * FC03 frame, SW6 an FC06 frame (writing 1 and 0 in turn). The 12
 * bytes show blue for the MBAP and green for the PDU. It ends after
 * 3 minutes.
 *
 * Twin of cp3_06_practise_solution.py (aiot-development-for-smart-farm/core/practise/solutions/cp3_06_practise_solution.py). Style: CLEAN_C.md.
 * Differs from the .py:
 *   - C has no struct module: pack() reads the same format text
 *     (">" = big-endian, B = 1 byte, H = 2 bytes; without ">" the
 *     board's own little-endian order with H on an even offset, as
 *     MicroPython's native mode lays it out). A value is packed as
 *     its low 8 or 16 bits.
 *   - struct.pack raises struct.error when the format does not fit
 *     the values; pack() then packs nothing, so the frame comes out
 *     short and self_test shows how many bytes it got.
 *   - hasattr(ui, "volume") is a fact of the build: the volume is
 *     always set, and a build without it refuses, which is ignored.
 *   - ui.tone() is fire-and-forget there, so its status is ignored.
 *   - Stop ends the program quietly where MicroPython raises
 *     KeyboardInterrupt.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_buttons.h"
#include "bento_runtime.h"
#include "bento_time.h"

/* ---- 1) ตั้งค่า (แก้ได้) ---- */
enum {
    UNIT_ID = 1, /* เลขเครื่องของ PLC (unit id) ใน MBAP */
    /* อ่านปุ่มทุกกี่ ms (เฟิร์มแวร์กรองสั่นทุกครั้งที่อ่าน) */
    SAMPLE_MS = 20,
    RUN_MS = 180000, /* เล่นนาน 3 นาทีแล้วจบเอง */
    SPEAKER = 40,    /* ความดังลำโพงรวม 0-100% (firmware 2.4.2+) */
    VOLUME = 25      /* ความดังเสียง 0-127 (ราว 20%) ทุกเสียงในไฟล์ */
};

enum { /* colours, 0xRRGGBB */
    COL_TEXT = 0xE8EAED,
    COL_DIM = 0x9AA3AF,
    COL_CARD = 0x171B22,
    COL_OK = 0x30A46C,
    COL_WARN = 0xF5A623,
    COL_BAD = 0xE5484D,
    COL_INFO = 0x4A9EFF
};

enum {               /* Modbus TCP */
    FC_READ = 3,     /* FC03: read holding registers */
    FC_WRITE = 6,    /* FC06: write one holding register */
    MBAP_LENGTH = 6, /* bytes after the len field: unit 1 + PDU 5 */
    TID_MASK = 0xFFFF,
    FRAME_BYTES = 12, /* MBAP 7 + PDU 5 */
    MBAP_BYTES = 7,
    PACK_ROOM = 16,  /* room for a frame packed in native order */
    READ_COUNT = 3,  /* SW5 asks for 3 registers */
    PDU_VALUES = 3,  /* fc, addr, value */
    MBAP_VALUES = 4, /* tid, protocol, len, unit */
    NIBBLE_BITS = 4, /* one hex digit */
    NIBBLE_MASK = 0x0F
};

enum { /* hardware and sound */
    SW6_INDEX = 0,
    BUTTONS = 2,
    TONE_MS = 90,
    NOTE_GAP_MS = 100,
    TUNE_NOTES_MAX = 3
};

enum { /* screen */
    SETTLE_MS = 200,
    MARGIN_X = 12,
    TITLE_Y = 6,
    NOTE_Y = 38,
    BYTES_CARD_Y = 62,
    BYTES_CARD_H = 112,
    CARD_W = 768,
    ASK_CARD_Y = 186,
    ASK_CARD_H = 152,
    CARD_RADIUS = 12,
    CARD_TITLE_DX = 12,
    CARD_TITLE_DY = 6,
    BYTE_X = 28, /* byte i sits at x = 28 + i * 62 */
    BYTE_STEP_X = 62,
    BYTE_Y = 96,
    FIELD_Y = 136,
    ASKED_X = 28,
    ASKED_Y = 224,
    HELP_Y = 352,
    FONT_TITLE = 24,
    FONT_TEXT = 16,
    FONT_FIELD = 14,
    FONT_BYTE = 28,
    FONT_DEFAULT = 0, /* no value= : the board's 20 px font */
    TEXT_CHARS = 96,
    HEX_CHARS = (PACK_ROOM * 2) + 1
};

/* ---- 2) Hardware ---- */
typedef enum {
    TUNE_TAP,
    TUNE_START,
    TUNE_STOP,
    TUNE_GOOD,
    TUNE_BAD,
    TUNE_EMPTY,
    TUNE_HIT
} tune_t;

typedef struct {
    uint32_t count;
    uint8_t notes[TUNE_NOTES_MAX];
} tune_notes_t;

/* ui.tone, not ui.sfx: only a tone takes a loudness on this firmware */
static const tune_notes_t TUNES[] = {
    {1U, {76U, 0U, 0U}},   {2U, {72U, 79U, 0U}}, {2U, {79U, 72U, 0U}},
    {3U, {72U, 79U, 84U}}, {2U, {84U, 76U, 0U}}, {3U, {84U, 76U, 69U}},
    {1U, {88U, 0U, 0U}}};

/* Stop is a status: the chain of steps ends at once. */
static bento_status_t sleep_ms(uint32_t ms)
{
    bento_status_t status = bento_time_sleep_ms(ms);

    if ((status == BENTO_OK) && !bento_runtime_is_running()) {
        status = BENTO_E_CANCELLED;
    }
    return status;
}

static bento_status_t beep(bento_ui_t *ui, tune_t name)
{
    const tune_notes_t *tune = &TUNES[name];
    uint32_t i;
    bento_status_t status = BENTO_OK;

    for (i = 0U; (i < tune->count) && (status == BENTO_OK); i++) {
        /* a tone is fire-and-forget in MicroPython */
        (void)bento_ui_sfx_tone(ui, tune->notes[i], BENTO_UI_WAVE_SINE,
                                (uint8_t)VOLUME, (uint16_t)TONE_MS);
        status = sleep_ms(NOTE_GAP_MS);
    }
    return status;
}

/* ---- 3) The brain: decides, never touches the hardware ---- */
typedef struct {
    uint8_t bytes[PACK_ROOM];
    uint32_t length;
} frame_t;

/* B = 1 byte, H = 2 bytes, anything else 0 (not a code pack knows). */
static uint32_t code_bytes(char code)
{
    uint32_t size = 0U;

    if (code == 'B') {
        size = 1U;
    } else if (code == 'H') {
        size = 2U;
    } else {
        /* an unknown code */
    }
    return size;
}

/* Appends the low `size` bytes of value, in the order asked for. */
static void put_value(frame_t *frame, uint32_t value, uint32_t size,
                      bool big_endian)
{
    const uint32_t low = value & 0xFFU;
    const uint32_t high = (value >> 8U) & 0xFFU;

    if (size == 1U) {
        frame->bytes[frame->length] = (uint8_t)low;
    } else if (big_endian) {
        frame->bytes[frame->length] = (uint8_t)high;
        frame->bytes[frame->length + 1U] = (uint8_t)low;
    } else {
        frame->bytes[frame->length] = (uint8_t)low;
        frame->bytes[frame->length + 1U] = (uint8_t)high;
    }
    frame->length += size;
}

/* struct.pack(format, *values) for the codes this lesson uses: ">"
 * first = big-endian and packed tight; without it, the board's own
 * order, with each value on an offset its size divides. Packs nothing
 * and answers false when the format does not fit the values. */
static bool pack(frame_t *frame, const char *format,
                 const uint32_t *values, uint32_t count)
{
    const bool big_endian = format[0] == '>';
    uint32_t at = big_endian ? 1U : 0U;
    uint32_t used = 0U;
    bool fits = true;

    frame->length = 0U;
    for (; fits && (format[at] != '\0'); at++) {
        const uint32_t size = code_bytes(format[at]);

        fits = (size != 0U) && (used < count);
        while (fits && !big_endian && ((frame->length % size) != 0U)) {
            frame->bytes[frame->length] = 0U;
            frame->length++;
        }
        fits = fits && ((frame->length + size) <= (uint32_t)PACK_ROOM);
        if (fits) {
            put_value(frame, values[used], size, big_endian);
            used++;
        }
    }
    if (!fits || (used != count)) {
        frame->length = 0U;
    }
    return fits && (used == count);
}

/* `front` + `back`, as Python's bytes + bytes. */
static void join(frame_t *frame, const frame_t *front,
                 const frame_t *back)
{
    (void)memcpy(frame->bytes, front->bytes, front->length);
    (void)memcpy(&frame->bytes[front->length], back->bytes,
                 back->length);
    frame->length = front->length + back->length;
}

/* FC03 reads qty holding registers from register addr (done: a model
 * to follow). ">HHHBBHH" = big-endian: tid(H) protocol(H) len(H)
 * unit(B) | fc(B) addr(H) qty(H), 12 bytes in all. */
static void fc03_read(frame_t *frame, uint32_t tid, uint32_t unit,
                      uint32_t addr, uint32_t qty)
{
    const uint32_t values[] = {tid & (uint32_t)TID_MASK,
                               0U,
                               MBAP_LENGTH,
                               unit,
                               FC_READ,
                               addr,
                               qty};

    /* the format fits its 7 values */
    (void)pack(frame, ">HHHBBHH", values, MBAP_BYTES);
}

/* FC06 writes value into the one holding register addr, built in two
 * parts and joined. */
static void fc06_write(frame_t *frame, uint32_t tid, uint32_t unit,
                       uint32_t addr, uint32_t value)
{
    const uint32_t pdu_values[] = {FC_WRITE, addr, value};
    const uint32_t mbap_values[] = {tid & (uint32_t)TID_MASK, 0U,
                                    MBAP_LENGTH, unit};
    frame_t pdu;
    frame_t mbap;

    /* a format that does not fit leaves its part empty; self_test
     * shows the short frame */
    /* ช่อง A (เฉลย): big-endian, fc 1 ไบต์ + addr 2 + value 2 */
    (void)pack(&pdu, ">BHH", pdu_values, PDU_VALUES);
    /* ช่อง B (เฉลย): unit 1 + PDU 5 = 6 (หรือ len(pdu) + 1) */
    (void)pack(&mbap, ">HHHB", mbap_values, MBAP_VALUES);
    join(frame, &mbap, &pdu);
}

/* The bytes as hex, 2 digits each, lower case as "%02x" writes. */
static void frame_hex(const frame_t *frame, char *text)
{
    static const char hex_digits[] = "0123456789abcdef";
    uint32_t out = 0U;
    uint32_t i;

    for (i = 0U; i < frame->length; i++) {
        text[out] =
            hex_digits[(uint32_t)frame->bytes[i] >> NIBBLE_BITS];
        text[out + 1U] = hex_digits[(uint32_t)frame->bytes[i] &
                                    (uint32_t)NIBBLE_MASK];
        out += 2U;
    }
    text[out] = '\0';
}

typedef struct {
    const char *name;
    bool is_write;
    uint32_t tid;
    uint32_t unit;
    uint32_t addr;
    uint32_t value; /* FC06's value, or FC03's qty */
    const char *want;
} frame_case_t;

/* Checks 3 frames before the screen opens; the first is the slide's. */
static bool self_test(void)
{
    static const frame_case_t cases[] = {
        {"fc06_write(1, 1, 0, 1)", true, 1U, 1U, 0U, 1U,
         "000100000006010600000001"},
        {"fc06_write(258, 2, 0, 0)", true, 258U, 2U, 0U, 0U,
         "010200000006020600000000"},
        {"fc03_read(7, 1, 0, 3)", false, 7U, 1U, 0U, 3U,
         "000700000006010300000003"}};
    const uint32_t count = (uint32_t)(sizeof(cases) / sizeof(cases[0]));
    char got[HEX_CHARS];
    frame_t frame;
    bool passed = true;
    uint32_t i;

    for (i = 0U; passed && (i < count); i++) {
        const frame_case_t *test = &cases[i];

        if (test->is_write) {
            fc06_write(&frame, test->tid, test->unit, test->addr,
                       test->value);
        } else {
            fc03_read(&frame, test->tid, test->unit, test->addr,
                      test->value);
        }
        frame_hex(&frame, got);
        if (strcmp(got, cases[i].want) != 0) {
            (void)printf("ยังไม่ถูก: %s ควรได้ %s (12 ไบต์) แต่ได้ %s "
                         "(%" PRIu32 " ไบต์)\n",
                         cases[i].name, cases[i].want, got,
                         frame.length);
            passed = false;
        }
    }
    if (passed) {
        (void)printf("ผ่าน! fc06_write สร้างกรอบ Modbus TCP ถูกทุกไบต์\n");
    }
    return passed;
}

/* ---- 4) Network (this file uses none) ----
 * The full cp3_06_modbus_frame sends the frame over MQTT to a gateway
 * on a laptop that speaks Modbus TCP; this practice leaves it out. */

/* ---- 5) The screen ---- */
typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t bytes[FRAME_BYTES];
    bento_ui_id_t asked;
    bento_ui_id_t note;
} frame_screen_t;

static int16_t px(int32_t value)
{
    return (int16_t)value;
}

static bento_status_t add_label(bento_ui_t *ui, const char *text,
                                const int32_t place[2], uint32_t color,
                                int32_t font_px, bento_ui_id_t *out)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.text = text;
    args.text_len = (uint32_t)strlen(text);
    args.x = px(place[0]);
    args.y = px(place[1]);
    args.color = color;
    args.font_px = font_px;
    return bento_ui_label_create(ui, &args, out);
}

/* A card: a dark box with a grey edge and a blue title. */
static bento_status_t add_card(bento_ui_t *ui, int32_t y, int32_t h,
                               const char *title)
{
    const int32_t title_place[2] = {MARGIN_X + CARD_TITLE_DX,
                                    y + CARD_TITLE_DY};
    bento_ui_widget_args_t args;
    bento_ui_id_t widget;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = px(MARGIN_X);
    args.y = px(y);
    args.w = px(CARD_W);
    args.h = px(h);
    args.color = COL_CARD;
    args.min_val = COL_DIM;
    args.max_val = CARD_RADIUS;
    args.font_px = 1;
    status = bento_ui_panel_create(ui, &args, &widget);
    if (status == BENTO_OK) {
        status = add_label(ui, title, title_place, COL_INFO, FONT_TEXT,
                           &widget);
    }
    return status;
}

/* The 12 byte labels, and the field names under the first byte of
 * each field: tid 2 bytes, proto 2, len 2, unit 1 | fc 1, addr 2,
 * qty/value 2. */
static bento_status_t add_bytes(frame_screen_t *screen)
{
    static const uint32_t field_at[] = {0U, 2U, 4U, 6U, 7U, 8U, 10U};
    static const char *const field_names[] = {
        "tid", "proto", "len", "unit", "fc", "addr", "qty/value"};
    bento_ui_id_t widget;
    uint32_t i;
    bento_status_t status = BENTO_OK;

    for (i = 0U; (i < (uint32_t)FRAME_BYTES) && (status == BENTO_OK);
         i++) {
        const int32_t place[2] = {BYTE_X + ((int32_t)i * BYTE_STEP_X),
                                  BYTE_Y};

        status =
            add_label(screen->ui, "--", place,
                      (i < (uint32_t)MBAP_BYTES) ? COL_INFO : COL_OK,
                      FONT_BYTE, &screen->bytes[i]);
    }
    for (i = 0U;
         (i < (uint32_t)(sizeof(field_at) / sizeof(field_at[0]))) &&
         (status == BENTO_OK);
         i++) {
        const int32_t place[2] = {
            BYTE_X + ((int32_t)field_at[i] * BYTE_STEP_X), FIELD_Y};

        status = add_label(screen->ui, field_names[i], place, COL_DIM,
                           FONT_FIELD, &widget);
    }
    return status;
}

/* The .py's ui.poll() only lets the screen catch up; its events are
 * not read, and a failed poll answers none. */
static void refresh(bento_ui_t *ui)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    (void)bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                        NULL);
}

static bento_status_t add_texts(frame_screen_t *screen)
{
    const int32_t title_place[2] = {MARGIN_X, TITLE_Y};
    const int32_t note_place[2] = {MARGIN_X, NOTE_Y};
    bento_ui_id_t widget;
    bento_status_t status =
        add_label(screen->ui, "Modbus TCP 12 ไบต์", title_place,
                  COL_TEXT, FONT_TITLE, &widget);

    if (status == BENTO_OK) {
        status = add_label(screen->ui, "แบบฝึก: โหมดดูกรอบ ไม่ส่งไปไหน",
                           note_place, COL_WARN, FONT_TEXT, &widget);
    }
    if (status == BENTO_OK) {
        status = add_card(screen->ui, BYTES_CARD_Y, BYTES_CARD_H,
                          "ฟ้า = MBAP  เขียว = PDU");
    }
    return status;
}

static bento_status_t build_screen(frame_screen_t *screen)
{
    const int32_t asked_place[2] = {ASKED_X, ASKED_Y};
    const int32_t help_place[2] = {MARGIN_X, HELP_Y};
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = sleep_ms(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_texts(screen);
    }
    if (status == BENTO_OK) {
        status = add_bytes(screen);
    }
    if (status == BENTO_OK) {
        status =
            add_card(screen->ui, ASK_CARD_Y, ASK_CARD_H, "กรอบนี้ขออะไร");
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", asked_place, COL_TEXT,
                           FONT_DEFAULT, &screen->asked);
    }
    if (status == BENTO_OK) {
        status =
            add_label(screen->ui, "SW5 = FC03 อ่าน  SW6 = FC06 เขียน",
                      help_place, COL_DIM, FONT_TEXT, &screen->note);
    }
    if (status == BENTO_OK) {
        refresh(screen->ui);
    }
    return status;
}

/* A widget setter's refusal was ignored in MicroPython too (modui.c). */
static void set_text(bento_ui_t *ui, bento_ui_id_t widget,
                     const char *text)
{
    (void)bento_ui_set_text(ui, widget, text, (uint32_t)strlen(text));
}

/* ---- 6) The main program ---- */
typedef struct {
    uint32_t want;   /* what SW6 writes next: 1 and 0 in turn */
    uint32_t number; /* the request count, used as tid */
    bool down[BUTTONS];
} frame_play_t;

/* A press edge of SW5 (i = 0) or SW6 (i = 1): build the frame, show
 * its bytes and what it asks for. */
static bento_status_t show_request(const frame_screen_t *screen,
                                   frame_play_t *play, uint32_t i)
{
    const bool is_write = i != 0U;
    const uint32_t arg = is_write ? play->want : (uint32_t)READ_COUNT;
    char text[TEXT_CHARS];
    frame_t frame;
    uint32_t k;

    play->number++;
    if (is_write) {
        fc06_write(&frame, play->number, UNIT_ID, 0U, arg);
    } else {
        fc03_read(&frame, play->number, UNIT_ID, 0U, arg);
    }
    for (k = 0U; k < frame.length; k++) {
        (void)snprintf(text, sizeof(text), "%02X",
                       (unsigned int)frame.bytes[k]);
        set_text(screen->ui, screen->bytes[k], text);
    }
    if (is_write) {
        (void)snprintf(text, sizeof(text),
                       "#%" PRIu32 " FC06 เขียน HR0 = %" PRIu32,
                       play->number, arg);
    } else {
        (void)snprintf(text, sizeof(text),
                       "#%" PRIu32 " FC03 อ่าน HR0 %" PRIu32 " ช่อง",
                       play->number, arg);
    }
    set_text(screen->ui, screen->asked, text);
    refresh(screen->ui);
    if (is_write) {
        play->want = 1U - play->want;
    }
    return beep(screen->ui, TUNE_TAP);
}

/* Both buttons, SW5 first: SW5 = pressed(1), SW6 = pressed(0). */
static bento_status_t check_buttons(const frame_screen_t *screen,
                                    frame_play_t *play)
{
    bool down = false;
    uint32_t i;
    bento_status_t status = BENTO_OK;

    for (i = 0U; (i < (uint32_t)BUTTONS) && (status == BENTO_OK); i++) {
        status = bento_buttons_pressed(1U - i, &down);
        if ((status == BENTO_OK) && down && !play->down[i]) {
            status = show_request(screen, play, i);
        }
        play->down[i] = down;
    }
    return status;
}

static bento_status_t run_loop(const frame_screen_t *screen)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    frame_play_t play = {1U, 0U, {false, false}};
    bento_status_t status = beep(screen->ui, TUNE_GOOD);

    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = check_buttons(screen, &play);
        if (status == BENTO_OK) {
            status = sleep_ms(SAMPLE_MS);
        }
    }
    return status;
}

int main(void)
{
    frame_screen_t screen;
    bento_status_t status = BENTO_OK;

    (void)memset(&screen, 0, sizeof(screen));
    screen.ui = bento_runtime_get_ui();
    /* a build without a speaker volume refuses it, as hasattr() skips
     * it */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    /* fc06_write() is checked first; a failed frame ends the program
     * before the screen opens */
    if (self_test()) {
        status = build_screen(&screen);
        if (status == BENTO_OK) {
            status = run_loop(&screen);
        }
        if (status == BENTO_OK) {
            (void)bento_ui_set_color(screen.ui, screen.note, COL_WARN);
            set_text(screen.ui, screen.note,
                     "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่");
            refresh(screen.ui);
        }
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
