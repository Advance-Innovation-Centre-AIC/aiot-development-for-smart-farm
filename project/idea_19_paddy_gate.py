# idea_19_paddy_gate.py - ประตูน้ำแปลงนา: น้ำในแปลงต่ำ สั่งเปิดประตูน้ำจากมือถือ มีเพดานเวลา
# แม่แบบ B (สั่งของจริงจากไกล) · จอ: ภาพตัดคลองกับแปลงนา ประตูกลาง + กราฟระดับน้ำ
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# ต่อยอดจาก s2/sf2_03_remote_pump.py (เน็ต คำสั่ง กฎความปลอดภัยเหมือนเดิม)
# ทีมต้องแก้ 3 อย่าง: 1) SOIL_DRY PUMP_MAX_S จากค่าที่ลองเอง 2) หัวจอและป้าย 3) เสียงใน TUNES
# MQTT: หัวข้อ/คีย์/คำสั่งเหมือน sf2_03 ทุกตัว (สัญญา 3.2, 4) · soil = น้ำในแปลงนา (VR1) · tank = น้ำในคลองส่ง (VR4)
#   คีย์เพิ่ม: open_s = ประตูเปิดรวมกี่วินาที · low_min = น้ำในแปลงต่ำสุดตั้งแต่เริ่ม (%)
# เกณฑ์ = ค่าสาธิตในห้องเรียน ไม่ใช่คำแนะนำทางเกษตร
#
# ลองเล่น  : สั่งจาก web/idea_19.html?team=<เลขกลุ่ม> · หมุน VR1 ลงต่ำกว่า 30 = น้ำน้อย
# ต้องแก้ก่อนรัน: WIFI_SSID, WIFI_PASS, TEAM (เลขกลุ่ม)
# กับดัก    : get_message() ไม่บล็อก กล่องรับมีช่องเดียว ต้องถามทุก 100 ms

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
TANK_MIN = 10  # คลองต่ำกว่านี้ห้ามเปิด (ไม่มีน้ำให้ปล่อย)
SOIL_DRY = 30  # ค่าสาธิต: น้ำในแปลง (VR1) ต่ำกว่านี้ขึ้นว่าน้ำน้อย
SEND_MS, POLL_MS, LISTEN_MS = 5000, 100, 1800000
CHART_MS = 1000

SPEAKER = 40
VOLUME = 25
# สีทุ่งนา
COL_TEXT, COL_DIM = 0xEEF6E4, 0x93A88A
COL_CARD, COL_MUD = 0x13261A, 0x6B4F2A
COL_WATER, COL_RICE = 0x2F9BFF, 0x8BD450
COL_OK, COL_WARN, COL_BAD = 0x4CC38A, 0xF2C14E, 0xE5484D


# ---- 2) ฮาร์ดแวร์ ----
TUNES = {"tap": (74,), "start": (64, 69, 76), "stop": (76, 64),
         "bad": (84, 76), "hit": (86,)}


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
    # จอไฟ RGB วาดเฉพาะตอนเลขเปลี่ยน
    if shown <= 0 or sec_left == 0:
        rgbmatrix.scroll("")
        rgbmatrix.clear()
    if sec_left:
        rgbmatrix.score(sec_left, rgbmatrix.BLUE)
    return sec_left


# ---- 3) สมอง ----
def pump_seconds(sec):
    # ไม่ใช่จำนวนเต็มบวก = ค่าตั้งต้น  เกินเพดาน = เพดาน
    if isinstance(sec, bool) or not isinstance(sec, int) or sec <= 0: # bool ไม่นับ (สัญญาข้อ 4)
        sec = PUMP_DEFAULT_S
    return min(sec, PUMP_MAX_S)


def ascii_only(text):
    return "".join(c for c in str(text) if " " <= c <= "~")[:20]


def handle_command(raw, tank):
    # คำสั่งหนึ่งใบ -> (ทำอะไร, ค่า, ข้อความจอ, สี) ไม่แตะของจริง
    try:
        cmd = json.loads(raw.decode())
    except ValueError:
        cmd = None
    if not isinstance(cmd, dict):      # 5, null, [] ก็เป็น JSON ได้ แต่ไม่ใช่คำสั่ง
        return "deny", 0, "อ่านคำสั่งไม่ได้", COL_BAD
    act, on = cmd.get("cmd", ""), cmd.get("on", 1)
    if act == "led" and cmd.get("n", 0) != 0:
        return "deny", 0, "ไม่มีประตูบานที่ " + str(cmd.get("n"))[:8], COL_WARN
    if act in ("led", "pump") and not on:
        return "off", 0, "สั่งปิดประตูน้ำ", COL_OK
    if act in ("led", "pump") and tank < TANK_MIN:
        return "deny", 0, "น้ำในคลองเหลือ %d%% ไม่เปิด" % tank, COL_BAD
    if act in ("led", "pump"):
        sec = pump_seconds(cmd.get("sec", PUMP_DEFAULT_S))
        return "on", sec, "เปิดประตูน้ำ %d วิ" % sec, COL_OK
    if act == "beep":
        return "beep", 0, "เจ้าของนาเรียก!", COL_WATER
    if act == "say":
        text = ascii_only(cmd.get("text", ""))
        return "say", text, "ข้อความ: " + (text or "ไม่มีอักษรอังกฤษ"), COL_WATER
    if act == "ack":  # ack เป็นของ sf2_04
        return "", 0, "ไม่มีแจ้งเตือน (sf2_04)", COL_WATER
    if act != "":
        return "deny", 0, "ไม่รู้จัก " + str(act)[:12], COL_WARN
    return "", 0, "", COL_WATER


# ---- 4) เครือข่าย ----
def connect_broker(w):
    # ลอง 3 ครั้ง client_id ใหม่ทุกครั้ง
    for n in (1, 2, 3):
        if n > 1:
            show_note(w, "ลองต่อ broker ใหม่ %d/3" % n, COL_WARN)
            ui.poll()
        try:
            if mqtt.connect(BROKER, port=1883, keepalive=60,
                            client_id=CLIENT_ID + "-%04x" % (time.ticks_ms() & 0xFFFF)):
                return True
        except OSError:
            pass
    return False


def connect_farm(w):
    # WiFi -> IP -> broker + subscribe คืนข้อความว่าพังขั้นไหน
    show_note(w, "ต่อ WiFi... จอนิ่งได้", COL_WARN)
    ui.poll()
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "ต่อ WiFi ไม่ได้"
    linked = connect_broker(w)
    if not linked or not mqtt.subscribe(TOPIC_CMD): # subscribe หลัง connect
        return "broker ไม่ตอบ: รอ 1 นาทีแล้วรันใหม่"
    return ""


def send_report(n, soil, tank, running, open_s, low_min):
    # publish ตอนสายหลุดโยน OSError
    try:
        mqtt.publish(TOPIC, json.dumps({"id": TEAM, "n": n, "soil": soil, "tank": tank,
                                        "pump": 1 if running else 0, "sim": "soil tank",
                                        "open_s": open_s, "low_min": low_min}))
        return True
    except OSError:
        return False


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=8, value=1)
    return ui.Label(title, x=x + 12, y=y + 6, color=COL_RICE, value=16)


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: เชื่อมต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("แปลงนา: เปิดประตูน้ำจากมือถือ", x=12, y=6, color=COL_TEXT, value=24)
    w = {}
    # ภาพตัด คลอง | ประตู | นา
    w["title"] = card(12, 44, 520, 290, "ประตูน้ำ = ไฟสีฟ้า (คำสั่ง 0)")
    w["canal"] = ui.Bar(x=32, y=80, w=60, h=200, min=0, max=100, value=0)
    w["gate"] = ui.Panel(x=104, y=150, w=20, h=130, color=COL_MUD, min=COL_DIM, max=4, value=1)
    w["paddy"] = ui.Bar(x=136, y=150, w=64, h=130, min=0, max=100, value=0)
    w["canal_t"] = ui.Label("คลอง -- %", x=24, y=290, color=COL_WATER, value=16)
    w["paddy_t"] = ui.Label("นา -- %", x=132, y=290, color=COL_RICE, value=16)
    w["gate_t"] = ui.Label("ประตูปิด", x=230, y=84, color=COL_DIM, value=28)
    w["left"] = ui.Bar(x=230, y=132, w=282, h=16, min=0, max=PUMP_MAX_S, value=0)
    w["warn"] = ui.Label("", x=230, y=162, color=COL_WARN, value=16)
    ui.Label("คลองต่ำกว่า %d%% ไม่เปิด" % TANK_MIN, x=230, y=230, color=COL_DIM, value=14)
    ui.Label("เปิดได้นานสุด %d วิ" % PUMP_MAX_S, x=230, y=252, color=COL_DIM, value=14)
    ui.Label(BTN_NAMES[0] + " = ปิดประตูฉุกเฉิน ไม่พึ่งเน็ต", x=230, y=290, color=COL_WARN, value=14)
    # มุมขวาล่างสุดเว้นไว้ให้ปุ่ม Console
    card(544, 44, 236, 174, "น้ำในแปลงย้อนหลัง")
    w["chart"] = ui.Chart(x=556, y=72, w=212, h=110, color=COL_RICE, min=0, max=100)
    w["chart"].prop(ui.PROP_CHART_POINTS, 240)  # จุด >= ความกว้าง ไม่วาดจุดกลม · 4 นาที
    w["s_dry"] = w["chart"].add_series(COL_WARN)
    ui.Label("เขียว = น้ำในนา  เหลือง = เกณฑ์", x=556, y=192, color=COL_DIM, value=14)
    card(544, 228, 236, 102, "ตั้งแต่เริ่ม")
    w["open"] = ui.Label("ประตูเปิดรวม 0 วิ", x=556, y=256, color=COL_TEXT, value=16)
    w["low"] = ui.Label("น้ำต่ำสุด -- %", x=556, y=284, color=COL_TEXT, value=16)
    w["note"] = ui.Label("ยังไม่มีคำสั่ง", x=12, y=350, color=COL_DIM)
    w["mq"] = ui.Led(x=600, y=12, w=18, h=18, color=COL_OK, value=0)
    w["mq_t"] = ui.Label("MQTT: ออฟไลน์", x=626, y=10, color=COL_DIM, value=16)
    ui.poll()
    return w


def show_note(w, text, col):
    w["note"].color(col)
    w["note"].text(text)


def show_gate(w, sec_left):
    w["gate"].color(COL_WATER if sec_left else COL_MUD)
    w["gate_t"].color(COL_WATER if sec_left else COL_DIM)
    w["gate_t"].text("เปิดอยู่ อีก %d วิ" % sec_left if sec_left else "ประตูปิด")
    w["left"].value(sec_left)


def show_levels(w, soil, tank, open_s, low_min):
    w["canal"].value(tank)
    w["canal"].color(COL_BAD if tank < TANK_MIN else COL_MUD)
    w["paddy"].value(soil)
    w["paddy"].color(COL_WARN if soil < SOIL_DRY else COL_MUD)
    w["canal_t"].text("คลอง %d %%" % tank)
    w["paddy_t"].text("นา %d %%" % soil)
    w["warn"].text("น้ำน้อย ควรเปิด" if soil < SOIL_DRY else "")
    w["open"].text("ประตูเปิดรวม %d วิ" % open_s)
    w["low"].text("น้ำต่ำสุด %d %%" % low_min)


# ---- 6) โปรแกรมหลัก ----
class Stop(Exception):
    # ไม่ใช้ SystemExit: บอร์ดอาจค้าง
    pass


def stop(w, pump, msg, col=COL_BAD):
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
    tune = {"deny": "bad", "off": "stop", "on": "start", "say": "tap"}.get(do)
    if tune is not None:
        beep(tune)
    if text:
        show_note(w, text, col)
    return pump_ms, pump_t0


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
    show_note(w, "ฟัง " + TOPIC_CMD, COL_OK)
    stop_btn = Button(1)
    got = n = pump_ms = pump_t0 = open_ms = 0
    low_min, shown = 100, -1
    t_send = t0 = t_last = t_chart = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < LISTEN_MS:
        now = time.ticks_ms()
        if stop_btn.pressed_now() and pump_ms:                      # SW5 ไม่ผ่านเน็ตเลย
            pump_ms = 0
            show_note(w, "ปิดประตูฉุกเฉิน " + BTN_NAMES[0], COL_BAD)
            beep("stop")
        soil, tank = knob_percent(0), knob_percent(3)
        low_min = min(low_min, soil)
        if pump_ms:
            open_ms += time.ticks_diff(now, t_last)
        t_last = now
        msg = mqtt.get_message()
        if msg is not None:
            got += 1
            w["title"].text("ประตูน้ำ = ไฟสีฟ้า (คำสั่ง %d)" % got)
            pump_ms, pump_t0 = act_on(w, msg[1], tank, now, pump_ms, pump_t0)
        left = pump_ms - time.ticks_diff(now, pump_t0) if pump_ms else 0
        if pump_ms and (left <= 0 or tank < TANK_MIN):   # ครบเวลาหรือคลองแห้ง
            pump_ms = 0
            show_note(w, "ครบเวลา ปิดประตูเอง" if left <= 0 else "คลองแห้ง ปิดประตูเอง", COL_WATER)
            beep("stop")
        set_led(pump, pump_ms)
        sec_left = (left + 999) // 1000 if pump_ms else 0     # ปัดขึ้น
        if sec_left != shown:
            shown = matrix_countdown(sec_left, shown)
            show_gate(w, sec_left)
        show_levels(w, soil, tank, open_ms // 1000, low_min)
        if time.ticks_diff(now, t_chart) >= CHART_MS:
            t_chart = now
            w["chart"].set_next(0, soil)
            w["chart"].set_next(w["s_dry"], SOIL_DRY)
        if time.ticks_diff(now, t_send) >= SEND_MS:
            t_send, n = now, n + 1
            if not send_report(n, soil, tank, pump_ms, open_ms // 1000, low_min):
                stop(w, pump, "สายหลุด")
        if not mqtt.is_connected():
            stop(w, pump, "สายหลุด")
        ui.poll()
        wait_ms(POLL_MS, (stop_btn,))
    stop(w, pump, "เลิกฟัง ได้รับ %d คำสั่ง" % got, COL_DIM)


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
# 1) ส่ง {"cmd":"pump","on":1,"sec":9999} เปิดกี่วิ ทำไมต้องมีเพดาน (ประตูจริงเปิดเป็นชั่วโมง: เปลี่ยนหน่วยเป็นนาที)
# 2) ระดับ 2: น้ำในแปลงต่ำกว่า SOIL_DRY = เปิดเองอัตโนมัติ เว้น 60 วิก่อนเปิดซ้ำ (ดู s1/sf1_03_auto_irrigation.py)
# 3) น้ำในแปลงสูงเกิน (ฝนตกหนัก) = ห้ามเปิด แม้มีคนสั่ง
