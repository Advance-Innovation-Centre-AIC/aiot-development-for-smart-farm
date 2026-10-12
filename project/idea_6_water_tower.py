# idea_6_water_tower.py - หอถังน้ำของฟาร์ม: น้ำต่ำ ปั๊มไม่ยอมเปิด บอกชัดว่าปฏิเสธกี่ครั้ง
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# ต่อยอดจาก s2/sf2_03_remote_pump.py · VR4 = หอถัง, VR1 = ดินปลายท่อ · ภาพหลัก = หลอดถังแนวตั้ง
# MQTT: ของเดิม sf2_03 ครบ (สัญญา 3.2, 4) + 2 คีย์ใหม่ที่บอร์ดคิดเอง
#   refused = ปฏิเสธ/ดับเองเพราะถังต่ำกี่ครั้ง  tank_lo = ถังต่ำสุดตั้งแต่เปิดเครื่อง
# ทีมต้องแก้ 3 อย่าง: 1) TANK_MIN PUMP_MAX_S จากค่าที่ลองเอง 2) หัวจอและป้าย 3) คำบนจอไฟ RGB และเสียง
# เกณฑ์ = ค่าสาธิตในห้องเรียน
#
# ลองเล่น  : สั่งจาก project/web/idea_6.html?team=<เลขกลุ่ม>
# ต้องแก้ก่อนรัน: WIFI_SSID, WIFI_PASS, TEAM (เลขกลุ่ม)
# กับดัก    : get_message() ไม่บล็อก ต้องถามทุก 100 ms

import buttons
import gpio
import json
import mqtt
import pots
import rgbmatrix
import time
import ui
import wifi

# ---- 1) ตั้งค่า ----
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"   # ตั้งเอง: อังกฤษ/ตัวเลขสั้น ๆ ไม่มีเว้นวรรค
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"   # อย่างน้อย 8 ตัว · อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร
TEAM = "teamXX"

BROKER = "broker.hivemq.com"
CLIENT_ID = "bento-farm-" + TEAM       # + เลขจากนาฬิกาบอร์ดทุกครั้งที่ต่อ: ไม่ชน id เก่า
TOPIC_CMD = "bento-aiot/" + TEAM + "/cmd"
TOPIC = "bento-aiot/" + TEAM + "/telemetry"
BTN_NAMES = ("SW5", "SW6")
PUMP_DEFAULT_S, PUMP_MAX_S = 10, 30    # ใครสั่ง 9999 วินาที ก็ได้แค่ 30
TANK_MIN = 10  # หอถังต่ำกว่านี้ห้ามปั๊ม (ปั๊มดูดลม เดินตัวเปล่าจนพัง)
TANK_HALF = 50  # ขีดกลางถัง: ต่ำกว่านี้ขึ้นสีเหลือง
SEND_MS, POLL_MS, LISTEN_MS = 5000, 100, 1800000
BAR_Y, BAR_H = 70, 260                 # ตำแหน่งหลอดถังแนวตั้งบนจอ

SPEAKER = 40
VOLUME = 25
# สีหอถัง: เหล็กเทา น้ำฟ้าเข้ม ขีดแดง
COL_TEXT, COL_DIM = 0xE4E9F0, 0x8C97A6
COL_CARD = 0x1A1F27
COL_STEEL, COL_WATER, COL_HALF = 0x7A8FA6, 0x2F86F0, 0xF2C230
COL_LOW, COL_GO = 0xE5484D, 0x3FB97A


# ---- 2) ฮาร์ดแวร์ ----
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)

def knob_percent(i):
    return pots.read(i) * 100 // 4095


def led_named(name):
    try:
        names = gpio.board_info()["led_names"]
        led = gpio.led(names.index(name) if name in names else 0)
        led.off()
        return led
    except Exception:
        return None


def set_led(led, on):
    if led:
        led.on() if on else led.off()


class Button:
    def __init__(self, index):
        self.index, self.down, self.clicked = index, False, False

    def sample(self):
        now_down = buttons.pressed(self.index)
        if now_down and not self.down:
            self.clicked = True
        self.down = now_down

    def pressed_now(self):
        fired, self.clicked = self.clicked, False
        return fired


def wait_ms(ms, btns):
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < ms:
        for b in btns:
            b.sample()
        time.sleep_ms(20)


def matrix_countdown(sec_left, shown):
    if shown <= 0 or sec_left == 0:
        rgbmatrix.scroll("")
        rgbmatrix.clear()
    if sec_left:
        rgbmatrix.score(sec_left, rgbmatrix.BLUE)
    return sec_left


# ---- 3) สมอง ----
def pump_seconds(sec):
    # ตรวจเวลาที่สั่ง: ไม่ใช่จำนวนเต็มบวก = ค่าตั้งต้น  เกินเพดาน = เพดาน
    if isinstance(sec, bool) or not isinstance(sec, int) or sec <= 0:  # bool ไม่ใช่จำนวนเต็ม (สัญญาข้อ 4)
        sec = PUMP_DEFAULT_S
    return min(sec, PUMP_MAX_S)


def ascii_only(text):
    return "".join(c for c in str(text) if " " <= c <= "~")[:20]


def handle_command(raw, tank):
    # ตัดสินคำสั่งหนึ่งใบ -> (ทำอะไร, วินาทีหรือข้อความ, ข้อความขึ้นจอ, สี)  ไม่แตะของจริงในนี้
    try:
        cmd = json.loads(raw.decode())
    except ValueError:
        cmd = None
    if not isinstance(cmd, dict):      # 5, null, [] ก็เป็น JSON ได้ แต่ไม่ใช่คำสั่ง
        return "deny", 0, "อ่านคำสั่งไม่ได้", COL_LOW
    act, on = cmd.get("cmd", ""), cmd.get("on", 1)
    if act == "led" and cmd.get("n", 0) != 0:
        return "deny", 0, "ไม่มีปั๊มดวงที่ " + str(cmd.get("n"))[:6], COL_HALF
    if act in ("led", "pump") and not on:
        return "off", 0, "สั่งปิดปั๊มแล้ว", COL_GO
    if act in ("led", "pump") and tank < TANK_MIN:
        return "low", 0, "ไม่เปิด! หอถังเหลือ %d%% ต่ำกว่า %d%%" % (tank, TANK_MIN), COL_LOW
    if act in ("led", "pump"):
        sec = pump_seconds(cmd.get("sec", PUMP_DEFAULT_S))
        return "on", sec, "ปั๊มจากหอถัง %d วิ" % sec, COL_WATER
    if act == "beep":
        return "beep", 0, "เรียกคนดูแลหอถัง!", COL_WATER
    if act == "say":
        text = ascii_only(cmd.get("text", ""))
        return "say", text, "ข้อความ: " + (text or "ไม่มีอักษรอังกฤษ"), COL_WATER
    if act == "ack":  # ack เป็นของ sf2_04
        return "", 0, "ไม่มีแจ้งเตือน (sf2_04)", COL_DIM
    if act != "":
        return "deny", 0, "ไม่รู้จัก " + str(act)[:12], COL_HALF
    return "", 0, "", COL_DIM


# ---- 4) เครือข่าย ----
def connect_broker(w):
    # broker สาธารณะไม่ตอบเป็นพัก ๆ: ลอง 3 ครั้ง client_id ใหม่ทุกครั้ง
    for n in (1, 2, 3):
        if n > 1:
            show_note(w, "ลองต่อ broker ใหม่ %d/3" % n, COL_HALF)
            ui.poll()
        try:
            if mqtt.connect(BROKER, port=1883, keepalive=60,
                            client_id=CLIENT_ID + "-%04x" % (time.ticks_ms() & 0xFFFF)):
                return True
        except OSError:
            pass
    return False


def connect_farm(w):
    # WiFi -> IP -> broker + subscribe ขั้นไหนพังบอกตรงนั้น
    show_note(w, "ต่อ WiFi... จอนิ่งได้", COL_HALF)
    ui.poll()                          # ป้ายต้องขึ้นจอก่อนบรรทัดที่บล็อก
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "ต่อ WiFi ไม่ได้"
    linked = connect_broker(w)         # ลองได้ 3 ครั้ง (ดู connect_broker)
    if not linked or not mqtt.subscribe(TOPIC_CMD):     # subscribe ต้องมาหลัง connect เสมอ
        return "broker ไม่ตอบ: รอ 1 นาทีแล้วรันใหม่"
    return ""


def send_report(n, soil, tank, running, refused, tank_lo):
    # publish ตอนสายหลุดโยน OSError
    try:
        mqtt.publish(TOPIC, json.dumps({"id": TEAM, "n": n, "soil": soil, "tank": tank,
                                        "pump": 1 if running else 0, "sim": "soil tank",
                                        "refused": refused, "tank_lo": tank_lo}))
        return True
    except OSError:
        return False


# ---- 5) หน้าจอ ----
def level_y(pct):
    # ระดับ % -> พิกัด y บนหลอดถัง (บน = เต็ม)
    return BAR_Y + BAR_H - pct * BAR_H // 100


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: เชื่อมต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("หอถังน้ำของฟาร์ม", x=12, y=6, color=COL_STEEL, value=28)
    w = {}
    # Bar สูงกว่ากว้าง = แท่งแนวตั้ง
    ui.Panel(x=24, y=BAR_Y - 8, w=112, h=BAR_H + 16, color=COL_CARD, min=COL_STEEL, max=10, value=2)
    w["tank_bar"] = ui.Bar(x=40, y=BAR_Y, w=80, h=BAR_H, min=0, max=100, value=0)
    ui.Panel(x=30, y=level_y(TANK_MIN), w=100, h=3, color=COL_LOW)
    ui.Label("เต็ม", x=146, y=BAR_Y - 6, color=COL_DIM, value=14)
    ui.Label("ครึ่งถัง", x=146, y=level_y(TANK_HALF) - 10, color=COL_HALF, value=14)
    ui.Label("%d%% ห้ามปั๊ม" % TANK_MIN, x=146, y=level_y(TANK_MIN) - 10, color=COL_LOW, value=14)
    w["tank"] = ui.Label("-- %", x=244, y=72, color=COL_TEXT, value=28)
    w["tank_lo"] = ui.Label("ต่ำสุดรอบนี้ -- %", x=244, y=112, color=COL_DIM, value=16)
    w["refused"] = ui.Label("ปฏิเสธ 0 ครั้ง", x=244, y=170, color=COL_GO, value=28)
    ui.Label("ปั๊มไม่ยอมเปิดเพราะน้ำต่ำ", x=244, y=208, color=COL_DIM, value=14)
    w["soil"] = ui.Label("แปลงปลายท่อ (VR1) -- %", x=244, y=262, color=COL_TEXT, value=16)
    ui.Panel(x=500, y=62, w=280, h=222, color=COL_CARD, min=COL_WATER, max=12, value=1)
    w["title"] = ui.Label("ปั๊มจากหอถัง (คำสั่ง 0)", x=512, y=68, color=COL_WATER, value=16)
    w["led"] = ui.Led(x=516, y=102, w=40, h=40, color=COL_WATER)
    w["pump"] = ui.Label("ปิด", x=568, y=108, color=COL_DIM, value=24)
    w["arc"] = ui.Arc(x=600, y=150, w=110, h=110, max=PUMP_MAX_S)
    w["arc"].color(COL_WATER)
    ui.Label(BTN_NAMES[0] + " = ปิดปั๊มทันที", x=244, y=300, color=COL_HALF, value=14)
    w["note"] = ui.Label("ยังไม่มีคำสั่ง", x=12, y=352, color=COL_DIM)
    w["mq"] = ui.Led(x=606, y=12, w=18, h=18, color=COL_GO, value=0)
    w["mq_t"] = ui.Label("MQTT: ออฟไลน์", x=632, y=10, color=COL_DIM, value=16)
    ui.poll()
    return w


def show_note(w, text, col):
    w["note"].color(col)
    w["note"].text(text)


def show_pump(w, sec_left):
    w["led"].value(1 if sec_left else 0)
    w["pump"].color(COL_WATER if sec_left else COL_DIM)
    w["pump"].text("เปิด อีก %d วิ" % sec_left if sec_left else "ปิด")
    w["arc"].value(sec_left)


def show_tank(w, tank, tank_lo):
    col = COL_LOW if tank < TANK_MIN else COL_HALF if tank < TANK_HALF else COL_WATER
    w["tank_bar"].value(tank)
    w["tank_bar"].color(col)
    w["tank"].color(col)
    w["tank"].text("%d %%" % tank)
    w["tank_lo"].text("ต่ำสุดรอบนี้ %d %%" % tank_lo)


def show_refused(w, refused):
    w["refused"].color(COL_LOW if refused else COL_GO)
    w["refused"].text("ปฏิเสธ %d ครั้ง" % refused)


# ---- 6) โปรแกรมหลัก ----
class Stop(Exception):
    # ไม่ใช้ SystemExit: บอร์ดอาจค้าง
    pass


def stop(w, pump, msg, col=COL_LOW):
    # ดับปั๊มก่อนเสมอ
    show_link(w, False)
    set_led(pump, False)
    rgbmatrix.scroll("")
    rgbmatrix.clear()
    show_note(w, msg, col)
    ui.poll()
    raise Stop


def act_on(w, raw, tank, now, pump_ms, pump_t0):
    do, arg, text, col = handle_command(raw, tank)
    if do == "off":
        pump_ms = 0
    elif do == "on":
        pump_ms, pump_t0 = arg * 1000, now
    elif do == "beep":
        beep("hit")
    elif do == "say" and arg and not pump_ms:
        rgbmatrix.scroll(arg, rgbmatrix.PURPLE, 80)
    elif do == "low":
        rgbmatrix.scroll("LOW", rgbmatrix.RED, 80)     # เปลี่ยนสถานะครั้งเดียว ไม่วาดซ้ำทุกรอบ
    tune = {"deny": "bad", "low": "empty", "off": "stop", "on": "start", "say": "tap"}.get(do)
    if tune is not None:
        beep(tune)
    if text:
        show_note(w, text, col)
    return pump_ms, pump_t0, do == "low"


def main():
    if hasattr(ui, "volume"):
        ui.volume(SPEAKER)
    w = build_screen()
    pump = led_named("RGB_BLUE")
    rgbmatrix.clear()
    if len(TEAM) != 6 or TEAM[:4] != "team" or not TEAM[4:].isdigit():
        stop(w, pump, "แก้ TEAM เป็นเลขกลุ่มก่อน")
    problem = connect_farm(w)
    if problem:
        stop(w, pump, problem)
    show_link(w, True)
    show_note(w, "ฟัง " + TOPIC_CMD, COL_GO)
    stop_btn = Button(1)
    got = n = pump_ms = pump_t0 = refused = 0
    tank_lo = 100                      # ต่ำสุดตั้งแต่เปิดเครื่อง
    shown = -1
    t_send = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < LISTEN_MS:
        now = time.ticks_ms()
        if stop_btn.pressed_now() and pump_ms:                      # SW5 ไม่ผ่านเน็ตเลย
            pump_ms = 0
            show_note(w, "ปิดปั๊มฉุกเฉิน " + BTN_NAMES[0], COL_LOW)
            beep("stop")
        soil, tank = knob_percent(0), knob_percent(3)
        tank_lo = min(tank_lo, tank)
        msg = mqtt.get_message()       # None = ยังไม่มีอะไรมา / (topic, bytes)
        if msg is not None:
            got += 1
            w["title"].text("ปั๊มจากหอถัง (คำสั่ง %d)" % got)
            pump_ms, pump_t0, low = act_on(w, msg[1], tank, now, pump_ms, pump_t0)
            if low:
                refused += 1
                show_refused(w, refused)
        left = pump_ms - time.ticks_diff(now, pump_t0) if pump_ms else 0
        if pump_ms and (left <= 0 or tank < TANK_MIN):   # ปั๊มดับเองเมื่อครบเวลาหรือน้ำหมดถัง
            pump_ms = 0
            shown = matrix_countdown(0, shown)   # ล้างเลขก่อน LOW
            show_pump(w, 0)
            if left > 0:               # ดับเพราะน้ำหมดกลางทาง นับเป็นการปฏิเสธด้วย
                refused += 1
                show_refused(w, refused)
                rgbmatrix.scroll("LOW", rgbmatrix.RED, 80)
            show_note(w, "ครบเวลา ปิดปั๊มเอง" if left <= 0 else "หอถังแห้ง ดับปั๊มเอง", COL_WATER if left <= 0 else COL_LOW)
            beep("stop" if left <= 0 else "empty")
        set_led(pump, pump_ms)
        sec_left = (left + 999) // 1000 if pump_ms else 0     # ปัดขึ้น: สั่ง 10 วิ เริ่มนับที่ 10 ไม่ใช่ 11
        if sec_left != shown:
            shown = matrix_countdown(sec_left, shown)
            show_pump(w, sec_left)
        show_tank(w, tank, tank_lo)
        w["soil"].text("แปลงปลายท่อ (VR1) %d %%" % soil)
        if time.ticks_diff(now, t_send) >= SEND_MS:
            t_send, n = now, n + 1
            if not send_report(n, soil, tank, pump_ms, refused, tank_lo):
                stop(w, pump, "สายหลุด")
        if not mqtt.is_connected():
            stop(w, pump, "สายหลุด")
        ui.poll()
        wait_ms(POLL_MS, (stop_btn,))
    stop(w, pump, "เลิกฟัง ได้รับ %d คำสั่ง ปฏิเสธ %d" % (got, refused), COL_DIM)


try:
    main()
except Stop:
    pass
finally:
    try:
        mqtt.disconnect()
    except Exception:
        pass

# ---- ตาคุณ ----
# 1) หมุน VR4 ลงต่ำกว่า 10 แล้วสั่ง {"cmd":"pump","on":1} จอและเสียงบอกอะไร ตัวนับขึ้นไหม
# 2) ระดับ 2: ถังต่ำกว่าครึ่งนานเกิน 5 นาที = เตือนหน้าเว็บ
