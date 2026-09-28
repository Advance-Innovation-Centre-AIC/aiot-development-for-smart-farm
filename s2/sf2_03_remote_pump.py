# sf2_03_remote_pump.py - เปิดปั๊มน้ำในโรงเรือนจากมือถือหรือแอปของกลุ่ม
#
# ภารกิจ   : ฟังหัวข้อคำสั่งของกลุ่ม แล้วเปิด/ปิดปั๊มตามที่สั่ง ปั๊มดับเองเมื่อครบเวลา เปิดครั้งละไม่เกิน
#            30 วิ ไม่ยอมเปิดถ้าน้ำในถัง (VR4) ต่ำกว่า 10 % และรายงานดิน ถัง ปั๊ม ทุก 5 วิ
# ลองเล่น  : เปิด farm_web.html?team=<เลขกลุ่ม> กด "รดน้ำ 10 วินาที" / "ปิดปั๊ม"
#            หรือให้แอป farm_monitor.py ของกลุ่มสั่งเองเมื่อดิน (VR1) แห้ง · หมุน VR4 ต่ำกว่า 10 % แล้วสั่งอีกที
# ของบนบอร์ดที่ใช้ : ไฟ RGB_BLUE บนบอร์ด = ปั๊มน้ำ (รีเลย์) · VR1 = ความชื้นดิน (จำลอง) · VR4 = น้ำในถัง (จำลอง)
#            SW4 = ปุ่มหยุดฉุกเฉินหน้าฟาร์ม ปั๊มดับทันทีโดยไม่ต้องพึ่งเน็ต
#            จอไฟ RGB 16x8 = นับถอยหลังวินาทีที่ปั๊มเปิด หรือตัววิ่งจากคำสั่ง say · ลำโพงดังเฉพาะตอนคำสั่งเข้า
# บนจอ     : ไฟปั๊ม (Led), วงแหวนนับถอยหลัง (Arc), หลอดน้ำในถัง (Bar), ตารางคำสั่งล่าสุด (Table),
#            วงหมุนตอนกำลังต่อเน็ต (Spinner)
# แนวคิด AIoT: Command -> Check -> Act  ไม่เชื่อคนส่ง ตรวจทุกคำสั่งก่อนแตะของจริง
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.1 ขึ้นไป) และ BENTO Emulator (MQTT ใน Emulator เป็นแบบจำลอง)
# คำสั่งที่รู้จัก: {"cmd":"pump","on":1,"sec":10}  {"cmd":"pump","on":0}  {"cmd":"led","n":0,"on":1}
#            {"cmd":"beep"}  {"cmd":"say","text":"HELLO"}  (สัญญาเต็มอยู่ใน app/MQTT_CONTRACT_th.md)
# กับดัก    : get_message() ไม่บล็อก และกล่องรับมีช่องเดียว ลูปจึงต้องถามทุก 100 ms ห้ามหลับยาว

import buttons
import gpio
import json
import mqtt
import pots
import rgbmatrix
import time
import ui
import wifi

# ---- 1) ตั้งค่า (แก้ได้) ----
WIFI_SSID = "bento-teamXX"
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"
TEAM = "teamXX"                        # team01 ถึง team20 (team00 = บอร์ดผู้สอนหน้าห้อง)

BROKER = "broker.hivemq.com"           # สำรอง: "test.mosquitto.org" ถ้าผู้สอนประกาศ
ROOT = "bento-aiot"
CLIENT_ID = "bento-farm-" + TEAM       # ต้องไม่ซ้ำกับใครบน broker (แอปจึงต่อท้ายด้วยตัวสุ่ม)
TOPIC_CMD = ROOT + "/" + TEAM + "/cmd"
TOPIC = ROOT + "/" + TEAM + "/telemetry"
PUMP_DEFAULT_S, PUMP_MAX_S = 10, 30    # ใครสั่ง 9999 วินาที ก็ได้แค่ 30
TANK_MIN = 10                          # น้ำในถังต่ำกว่านี้ ห้ามปั๊มทำงาน (ปั๊มแห้งพัง)
SEND_MS, POLL_MS, LISTEN_MS = 5000, 100, 1800000
LOG_ROWS = 4                           # ตารางจำคำสั่งล่าสุดกี่แถว

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF
TONE_COLORS = {"ok": COL_OK, "warn": COL_WARN, "bad": COL_BAD, "info": COL_INFO}


# ---- 2) ฮาร์ดแวร์ ----
def knob_percent(i):
    """ลูกบิด VR1-VR4 (i = 0-3) เป็น 0-100 %  บอร์ดไม่มี pots.percent() จึงคิดเอง"""
    return pots.read(i) * 100 // 4095


def led_named(name):
    """หา LED ด้วยชื่อ ไม่ใช่เลข: บน Dev Kit ดวง LED1/LED2 (เลข 0, 1) อยู่บน SoM
    มองไม่เห็น ดวงที่เห็นคือ RGB_RED / RGB_GREEN / RGB_BLUE"""
    try:
        names = gpio.board_info()["led_names"]
        led = gpio.led(names.index(name) if name in names else 0)
        led.off()
        return led
    except Exception:
        return None


def set_pump(pump, running):
    if pump is None:
        return
    if running:
        pump.on()
    else:
        pump.off()


class Edge:
    """จับจังหวะ "เพิ่งกด" ของปุ่ม: กดหนึ่งครั้ง = ทำงานหนึ่งครั้ง แม้จะกดค้างไว้"""

    def __init__(self):
        self.was_down = False

    def pressed_now(self, is_down):
        fired = is_down and not self.was_down
        self.was_down = is_down
        return fired


def matrix_countdown(sec_left, shown):
    """จอไฟ RGB: นับถอยหลังวินาทีที่ปั๊มเปิด เขียนเฉพาะตอนเลขเปลี่ยน (ทุกครั้งคือการเขียนบัส I2C)"""
    if sec_left == shown:
        return shown
    if shown <= 0 or sec_left == 0:
        rgbmatrix.scroll("")           # ปั๊มเริ่มหรือจบ: หยุดตัววิ่งก่อน จอไฟใช้ร่วมกัน
        rgbmatrix.clear()
    if sec_left:
        rgbmatrix.score(sec_left, rgbmatrix.BLUE)
    return sec_left


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ไม่แตะเน็ต ----
def valid_team(team):
    return len(team) == 6 and team[:4] == "team" and team[4:].isdigit()


def parse_command(raw):
    """bytes จาก broker -> dict หรือ None  (5, null, [] ก็เป็น JSON ได้ แต่ไม่ใช่คำสั่ง)"""
    try:
        cmd = json.loads(raw.decode())
    except ValueError:
        return None
    return cmd if isinstance(cmd, dict) else None


def pump_seconds(sec):
    """ตรวจเวลาที่สั่ง: ไม่ใช่จำนวนเต็มบวก = ค่าตั้งต้น  เกินเพดาน = เพดาน"""
    if not isinstance(sec, int) or sec <= 0:
        sec = PUMP_DEFAULT_S
    return min(sec, PUMP_MAX_S)


def ascii_only(text):
    """จอไฟ RGB รับแต่อักษรอังกฤษ ตัวเลข เครื่องหมาย กรองที่เหลือทิ้งและตัดให้สั้น"""
    return "".join(c for c in str(text) if " " <= c <= "~")[:20]


def handle_command(cmd, tank):
    """ตัดสินว่าจะทำอะไรกับคำสั่งหนึ่งใบ คืน dict: do = สิ่งที่ต้องทำ, note = ข้อความขึ้นจอ, tone = สี"""
    if cmd is None:
        return {"do": "deny", "note": "ไม่ใช่คำสั่งที่อ่านได้", "tone": "bad"}
    act, on = cmd.get("cmd", ""), cmd.get("on", 1)
    if act == "led" and cmd.get("n", 0) != 0:
        return {"do": "deny", "note": "ไม่มีปั๊มดวงที่ " + str(cmd.get("n")), "tone": "warn"}
    if act in ("led", "pump") and not on:
        return {"do": "pump_off", "note": "สั่งปิดปั๊ม", "tone": "ok"}
    if act in ("led", "pump") and tank < TANK_MIN:
        return {"do": "deny", "note": "น้ำเหลือ " + str(tank) + "% ไม่ยอมเปิด", "tone": "bad"}
    if act in ("led", "pump"):
        sec = pump_seconds(cmd.get("sec", PUMP_DEFAULT_S))
        return {"do": "pump_on", "sec": sec, "note": "เปิดปั๊ม " + str(sec) + " วิ", "tone": "ok"}
    if act == "beep":
        return {"do": "beep", "note": "เจ้าของฟาร์มเรียก!", "tone": "info"}
    if act == "say":
        text = ascii_only(cmd.get("text", ""))
        return {"do": "say", "text": text, "note": "ข้อความ: " + (text or "ไม่มีอักษรอังกฤษ"), "tone": "info"}
    if act == "ack":                   # ปุ่มรับทราบในหน้าเว็บเป็นของ sf2_04 ไฟล์นี้ไม่มีแจ้งเตือน
        return {"do": "none", "note": "ไม่มีแจ้งเตือนให้รับทราบ (ใช้กับ sf2_04)", "tone": "info"}
    if act != "":
        return {"do": "deny", "note": "ไม่รู้จัก " + str(act)[:12], "tone": "warn"}
    return {"do": "none", "note": "", "tone": "info"}


def seconds_left(pump_ms, pump_t0, now):
    """เหลือกี่มิลลิวินาทีก่อนปั๊มดับเอง (0 = ปั๊มไม่ได้เปิด)"""
    return pump_ms - time.ticks_diff(now, pump_t0) if pump_ms else 0


def build_report(n, soil, tank, running):
    """รายงานสถานะ คีย์ตามสัญญา MQTT: sim บอกว่าคีย์ไหนมาจากลูกบิดจำลอง"""
    return {"id": TEAM, "n": n, "soil": soil, "tank": tank, "pump": 1 if running else 0, "sim": "soil tank"}


# ---- 4) เครือข่าย ----
def connect_farm(w):
    """บันไดสามขั้น WiFi -> IP -> broker + subscribe ขั้นไหนพังบอกบนจอว่าพังตรงไหน"""
    show_status(w, "กำลังต่อ WiFi จอจะนิ่งสักครู่", COL_WARN)
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "ต่อ WiFi ไม่ได้ ตรวจชื่อวงกับรหัส"
    show_status(w, "ได้ IP แล้ว กำลังต่อ broker", COL_WARN)
    try:
        linked = mqtt.connect(BROKER, port=1883, client_id=CLIENT_ID, keepalive=60)
    except OSError:
        linked = False
    if not linked or not mqtt.subscribe(TOPIC_CMD):     # subscribe ต้องมาหลัง connect เสมอ
        return "broker ไม่ตอบ เน็ตกันพอร์ต 1883?"
    return ""


def send_report(report):
    """ส่งรายงาน คืน False ถ้าสายหลุด (publish ตอนสายหลุดโยน OSError ไม่ใช่คืน False)"""
    try:
        mqtt.publish(TOPIC, json.dumps(report))
        return True
    except OSError:
        return False


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    """การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า (ทุกไฟล์ใช้แบบเดียวกัน) คืนป้ายหัวเรื่อง"""
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    return ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def build_pump_card(w, sw4):
    card(12, 44, 380, 290, "ปั๊มน้ำ (ไฟ RGB สีฟ้าบนบอร์ด)")
    w["led"] = ui.Led(x=28, y=80, w=48, h=48, color=COL_INFO, value=0)    # 0 = หรี่ (ไม่ดับมืด)
    w["pump"] = ui.Label("ปิด", x=90, y=90, color=COL_DIM, value=24)
    w["arc"] = ui.Arc(x=236, y=74, w=140, h=140, min=0, max=PUMP_MAX_S, value=0)
    w["arc"].color(COL_OK)             # สีตอนสร้างใช้ไม่ได้กับ Arc ต้องตั้งหลังสร้าง
    w["sec"] = ui.Label("0", x=290, y=126, color=COL_TEXT, value=28)
    ui.Label("วงแหวน = วินาทีที่เหลือ (เต็ม 30)", x=28, y=222, color=COL_DIM, value=14)
    w["soil"] = ui.Label("ดิน (VR1) -- %", x=28, y=150, color=COL_TEXT, value=20)
    ui.Label(sw4 + " = หยุดฉุกเฉิน ไม่ต้องพึ่งเน็ต", x=28, y=250, color=COL_WARN, value=16)
    ui.Label("ปั๊มเปิดได้ครั้งละไม่เกิน " + str(PUMP_MAX_S) + " วิ", x=28, y=282, color=COL_DIM, value=14)


def build_tank_card(w):
    card(402, 44, 378, 100, "น้ำในถัง (VR4)")
    w["tank_bar"] = ui.Bar(x=414, y=78, w=276, h=24, min=0, max=100, value=0)
    w["tank"] = ui.Label("-- %", x=704, y=74, color=COL_TEXT, value=20)
    ui.Label("ต่ำกว่า " + str(TANK_MIN) + " % ปั๊มไม่ยอมเปิด", x=414, y=112, color=COL_DIM, value=14)


def build_log_card(w):
    w["log_title"] = card(402, 152, 378, 182, "คำสั่งล่าสุด (รับแล้ว 0)")
    table = ui.Table(x=414, y=180, w=354, h=146, cols=2, rows=LOG_ROWS + 1)
    table.col_width(0, 70)
    table.col_width(1, 284)
    table.add_row("วินาที", "คำสั่ง -> บอร์ดทำ")
    w["table"] = table


def build_screen(sw4):
    """สร้างทุกอย่างบนจอครั้งเดียว แล้วคืน dict ของ widget ที่ต้องอัปเดตภายหลัง"""
    ui.screen()
    time.sleep_ms(200)
    ui.Label("ปั๊มน้ำสั่งจากที่ไกล", x=12, y=6, color=COL_TEXT, value=24)
    w = {"status": ui.Label("กำลังเริ่ม", x=300, y=12, color=COL_DIM, value=16),
         "spin": ui.Spinner(x=744, y=4, w=36, h=36)}
    build_pump_card(w, sw4)
    build_tank_card(w)
    build_log_card(w)
    w["note"] = ui.Label("ยังไม่มีคำสั่ง", x=12, y=352, color=COL_DIM, value=20)
    show_log(w, [])
    ui.poll()
    return w


def show_status(w, msg, col):
    w["status"].color(col)
    w["status"].text(msg)
    ui.poll()


def show_note(w, text, col):
    w["note"].color(col)
    w["note"].text(text[:40])          # ป้ายรับได้ 126 ไบต์ ไทยตัวละ 3 ไบต์ ตัดก่อนเสมอ


def show_log(w, log):
    """เขียนตารางคำสั่งใหม่ทั้งตาราง (เรียกเฉพาะตอนมีคำสั่งเข้า ไม่ใช่ทุกรอบ)"""
    for r in range(LOG_ROWS):
        row = log[r] if r < len(log) else ("-", "-")
        w["table"].cell(r + 1, 0, row[0])
        w["table"].cell(r + 1, 1, row[1])


def show_pump(w, running, sec_left):
    w["led"].value(1 if running else 0)
    w["pump"].color(COL_OK if running else COL_DIM)
    w["pump"].text("เปิด" if running else "ปิด")
    w["arc"].value(sec_left)
    w["sec"].text(str(sec_left))


def show_knobs(w, soil, tank):
    w["soil"].text("ดิน (VR1) " + str(soil) + " %")
    w["tank_bar"].value(tank)
    w["tank_bar"].color(COL_INFO if tank >= TANK_MIN else COL_BAD)
    w["tank"].text(str(tank) + " %")


# ---- 6) โปรแกรมหลัก ----
def stop(w, pump, msg, col=COL_BAD):
    """จบเพราะอะไรก็ตาม ปั๊มต้องดับก่อน แล้วค่อยบอกเหตุผล"""
    set_pump(pump, False)
    rgbmatrix.scroll("")
    rgbmatrix.clear()
    w["spin"].hide()
    show_status(w, msg, col)
    raise SystemExit


def act_on(w, result, now, pump_ms, pump_t0, running):
    """ทำตามผลตัดสินของ handle_command แล้วคืน (pump_ms, pump_t0) ใหม่"""
    do = result["do"]
    if do == "pump_off":
        pump_ms = 0
    elif do == "pump_on":
        pump_ms, pump_t0 = result["sec"] * 1000, now
    elif do == "beep":
        ui.tone(69, ui.WAVE_SQUARE, 90, 150)          # โน้ต MIDI ไม่ใช่ความถี่
    elif do == "say" and result["text"] and not running:
        rgbmatrix.scroll(result["text"], rgbmatrix.PURPLE, 80)
    sfx = {"deny": ui.SFX_UI_DENY, "pump_off": ui.SFX_UI_BACK, "pump_on": ui.SFX_UI_SELECT,
           "say": ui.SFX_UI_MOVE}.get(do)
    if sfx is not None:
        ui.sfx(sfx)                    # เสียงดังเฉพาะตอนมีคำสั่งเข้า ไม่ใช่ทุกรอบลูป
    if result["note"]:
        show_note(w, result["note"], TONE_COLORS[result["tone"]])
    return pump_ms, pump_t0


def main():
    sw4 = buttons.name(0)              # ถามชื่อปุ่มจากเฟิร์มแวร์ ไม่พิมพ์เอง
    w = build_screen(sw4)
    pump = led_named("RGB_BLUE")       # ไฟสีฟ้าบนบอร์ด = ปั๊มน้ำ
    rgbmatrix.clear()
    if not valid_team(TEAM):
        stop(w, pump, "แก้ TEAM เป็นเลขกลุ่มก่อน เช่น team05")
    problem = connect_farm(w)
    if problem:
        stop(w, pump, problem)
    w["spin"].hide()
    show_status(w, "ฟัง " + TOPIC_CMD, COL_OK)

    stop_btn = Edge()
    log, got, n = [], 0, 0
    pump_ms = pump_t0 = 0
    shown = -1
    t_send = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < LISTEN_MS:
        now = time.ticks_ms()
        sec = time.ticks_diff(now, t0) // 1000
        if stop_btn.pressed_now(buttons.pressed(0)) and pump_ms:     # SW4 ไม่ผ่านเน็ตเลย
            pump_ms = 0
            show_note(w, "หยุดฉุกเฉินจาก " + sw4, COL_BAD)
            ui.sfx(ui.SFX_UI_BACK)
        soil, tank = knob_percent(0), knob_percent(3)

        msg = mqtt.get_message()       # None = ยังไม่มีอะไรมา / (topic, bytes)
        if msg is not None:
            got += 1
            result = handle_command(parse_command(msg[1]), tank)
            pump_ms, pump_t0 = act_on(w, result, now, pump_ms, pump_t0, pump_ms > 0)
            log.insert(0, (str(sec), result["note"][:28] or "(ไม่มีคำสั่ง)"))
            del log[LOG_ROWS:]
            show_log(w, log)
            w["log_title"].text("คำสั่งล่าสุด (รับแล้ว " + str(got) + ")")

        left = seconds_left(pump_ms, pump_t0, now)    # ปั๊มดับเองเมื่อครบเวลาหรือน้ำหมดถัง
        if pump_ms and (left <= 0 or tank < TANK_MIN):
            pump_ms = 0
            show_note(w, "ครบเวลา ปั๊มดับเอง" if left <= 0 else "น้ำหมดถัง ปั๊มดับเอง", COL_INFO)
            ui.sfx(ui.SFX_UI_BACK)
        set_pump(pump, pump_ms > 0)
        sec_left = left // 1000 + 1 if pump_ms else 0
        if sec_left != shown:
            shown = matrix_countdown(sec_left, shown)
            show_pump(w, pump_ms > 0, sec_left)
        show_knobs(w, soil, tank)

        if time.ticks_diff(now, t_send) >= SEND_MS:   # รายงานทุก 5 วิ ให้แอปเห็นว่าปั๊มทำงานจริง
            t_send, n = now, n + 1
            if not send_report(build_report(n, soil, tank, pump_ms > 0)):
                stop(w, pump, "สายหลุดตอนรายงาน")
        if not mqtt.is_connected():
            stop(w, pump, "สายหลุดระหว่างฟัง")
        ui.poll()
        time.sleep_ms(POLL_MS)

    stop(w, pump, "เลิกฟังแล้ว ได้รับ " + str(got) + " คำสั่ง", COL_DIM)   # ดับปั๊มก่อนจบเสมอ


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) ส่ง {"cmd":"pump","on":1,"sec":9999} จากช่อง JSON ใน mqtt_dashboard.html (ใส่เลขกลุ่มในช่องทีม)
#    ปั๊มเปิดนานเท่าไร ทำไมฟาร์มจริงต้องมีเพดานนี้ (ดูฟังก์ชัน pump_seconds)
# 2) เพิ่มคำสั่ง {"cmd":"set","tank_min":20} ใน handle_command ให้แอปของกลุ่มเปลี่ยนเกณฑ์ถังน้ำได้จากที่ไกล
#    อย่าลืมกันค่าแปลก ๆ เช่น -5 หรือ 500 (handle_command ต้องไม่แตะฮาร์ดแวร์ แค่ตัดสิน)
