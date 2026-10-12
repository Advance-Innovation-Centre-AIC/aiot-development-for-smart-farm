# idea_18_coop_fan.py - พัดลมเล้าไก่: เล้าร้อนตอนบ่าย สั่งพัดลมจากมือถือ มีเพดานเวลา
# แม่แบบ B (สั่งของจริงจากไกล) · จอ: เทอร์โมมิเตอร์แนวตั้ง + นาฬิการ้อนสะสม
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# ต่อยอดจาก s2/sf2_03_remote_pump.py (เน็ต คำสั่ง กฎความปลอดภัยเหมือนเดิม)
# ทีมต้องแก้ 3 อย่าง: 1) SOIL_DRY PUMP_MAX_S จากค่าที่ลองเอง 2) หัวจอและป้าย 3) เสียงใน TUNES
# MQTT: หัวข้อ/คีย์/คำสั่งเหมือน sf2_03 ทุกตัว (สัญญา 3.2, 4) · soil = ความร้อนในเล้า (VR1) · tank = ไฟแบตสำรอง (VR4)
#   คีย์เพิ่ม: hot_s = วินาทีที่ร้อนเกินเกณฑ์สะสม · runs = เปิดพัดลมกี่ครั้ง
# เกณฑ์ = ค่าสาธิตในห้องเรียน ไม่ใช่คำแนะนำทางเกษตร
#
# ลองเล่น  : สั่งจาก web/idea_18.html?team=<เลขกลุ่ม> · หมุน VR1 ขึ้นเกิน 70 = เล้าร้อน
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
TANK_MIN = 10  # แบตต่ำกว่านี้ห้ามเปิด (กันแบตหมด)
SOIL_DRY = 70  # ค่าสาธิต: VR1 สูงกว่านี้ = ร้อน (เตือนฝั่งค่าสูง)
SEND_MS, POLL_MS, LISTEN_MS = 5000, 100, 1800000

SPEAKER = 40
VOLUME = 25
# สีแดดบ่าย
COL_TEXT, COL_DIM = 0xFFF1E6, 0xB59A88
COL_CARD, COL_TRACK = 0x2A1A12, 0x4A3328
COL_HOT, COL_WARM, COL_COOL = 0xFF5A36, 0xFFB347, 0x3CC8E0
COL_OK, COL_BAD = 0x5BD18B, 0xE5484D


# ---- 2) ฮาร์ดแวร์ ----
TUNES = {"tap": (76,), "start": (67, 74, 79), "stop": (79, 67), "good": (72, 79, 84),
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
    # จอไฟ RGB วาดเฉพาะตอนเลขเปลี่ยน
    if shown <= 0 or sec_left == 0:
        rgbmatrix.scroll("")
        rgbmatrix.clear()
    if sec_left:
        rgbmatrix.score(sec_left, rgbmatrix.CYAN)
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
        return "deny", 0, "ไม่มีพัดลมตัวที่ " + str(cmd.get("n"))[:8], COL_WARM
    if act in ("led", "pump") and not on:
        return "off", 0, "สั่งหยุดพัดลม", COL_OK
    if act in ("led", "pump") and tank < TANK_MIN:
        return "deny", 0, "แบตเหลือ %d%% ไม่เปิด" % tank, COL_BAD
    if act in ("led", "pump"):
        sec = pump_seconds(cmd.get("sec", PUMP_DEFAULT_S))
        return "on", sec, "เปิดพัดลม %d วิ" % sec, COL_OK
    if act == "beep":
        return "beep", 0, "เจ้าของฟาร์มเรียก!", COL_COOL
    if act == "say":
        text = ascii_only(cmd.get("text", ""))
        return "say", text, "ข้อความ: " + (text or "ไม่มีอักษรอังกฤษ"), COL_COOL
    if act == "ack":  # ack เป็นของ sf2_04
        return "", 0, "ไม่มีแจ้งเตือน (sf2_04)", COL_COOL
    if act != "":
        return "deny", 0, "ไม่รู้จัก " + str(act)[:12], COL_WARM
    return "", 0, "", COL_COOL


def mmss(sec):
    return "%02d:%02d" % (min(sec // 60, 99), sec % 60)


# ---- 4) เครือข่าย ----
def connect_broker(w):
    # ลอง 3 ครั้ง client_id ใหม่ทุกครั้ง
    for n in (1, 2, 3):
        if n > 1:
            show_note(w, "ลองต่อ broker ใหม่ %d/3" % n, COL_WARM)
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
    show_note(w, "ต่อ WiFi... จอนิ่งได้", COL_WARM)
    ui.poll()
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "ต่อ WiFi ไม่ได้"
    linked = connect_broker(w)
    if not linked or not mqtt.subscribe(TOPIC_CMD): # subscribe หลัง connect
        return "broker ไม่ตอบ: รอ 1 นาทีแล้วรันใหม่"
    return ""


def send_report(n, soil, tank, running, hot_s, runs):
    # publish ตอนสายหลุดโยน OSError
    try:
        mqtt.publish(TOPIC, json.dumps({"id": TEAM, "n": n, "soil": soil, "tank": tank,
                                        "pump": 1 if running else 0, "sim": "soil tank",
                                        "hot_s": hot_s, "runs": runs}))
        return True
    except OSError:
        return False


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=14, value=1)
    return ui.Label(title, x=x + 12, y=y + 6, color=COL_WARM, value=16)


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: เชื่อมต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("เล้าไก่ร้อนไหม? เปิดพัดลมจากมือถือ", x=12, y=6, color=COL_TEXT, value=24)
    w = {}
    # ซ้าย: เทอร์โมมิเตอร์ (Bar สูงกว่ากว้าง = แนวตั้ง)
    card(12, 44, 210, 290, "ความร้อนในเล้า VR1")
    w["thermo"] = ui.Bar(x=34, y=78, w=40, h=240, min=0, max=100, value=0)
    ui.Label("100", x=82, y=74, color=COL_DIM, value=14)
    ui.Label("%d เกณฑ์" % SOIL_DRY, x=82, y=78 + (100 - SOIL_DRY) * 240 // 100 - 8, color=COL_HOT, value=14)
    w["heat"] = ui.Label("-- %", x=88, y=200, color=COL_TEXT, value=28)
    w["heat_t"] = ui.Label("", x=88, y=244, color=COL_HOT, value=16)
    # กลาง: วงแหวนพัดลม
    w["title"] = card(232, 44, 290, 290, "พัดลม = ไฟสีฟ้า (คำสั่ง 0)")
    w["arc"] = ui.Arc(x=297, y=80, w=160, h=160, max=PUMP_MAX_S)
    w["arc"].color(COL_COOL)
    w["led"] = ui.Led(x=357, y=140, w=40, h=40, color=COL_COOL)
    w["pump"] = ui.Label("พัดลมหยุด", x=248, y=252, color=COL_DIM, value=24)
    ui.Label(BTN_NAMES[0] + " = หยุดพัดลมฉุกเฉิน ไม่พึ่งเน็ต", x=248, y=298, color=COL_WARM, value=14)
    # ขวาบน: นาฬิการ้อนสะสม (ฟีเจอร์เฉพาะเรื่อง)
    card(532, 44, 248, 168, "ร้อนเกินเกณฑ์สะสม")
    w["hot"] = ui.Seg7(text="00:00", x=548, y=78, w=200, h=64, color=COL_HOT)
    w["runs"] = ui.Label("เปิดพัดลมไป 0 ครั้ง", x=548, y=160, color=COL_DIM, value=16)
    # มุมขวาล่างสุดเว้นไว้ให้ปุ่ม Console
    card(532, 222, 248, 108, "ไฟแบตสำรอง VR4")
    w["tank_bar"] = ui.Bar(x=548, y=256, w=150, h=22)
    w["tank"] = ui.Label("-- %", x=708, y=254, color=COL_TEXT, value=16)
    ui.Label("ต่ำกว่า %d%% ไม่เปิด / สูงสุด %d วิ" % (TANK_MIN, PUMP_MAX_S), x=548, y=292, color=COL_DIM, value=14)
    w["note"] = ui.Label("ยังไม่มีคำสั่ง", x=12, y=350, color=COL_DIM)
    w["mq"] = ui.Led(x=600, y=12, w=18, h=18, color=COL_OK, value=0)
    w["mq_t"] = ui.Label("MQTT: ออฟไลน์", x=626, y=10, color=COL_DIM, value=16)
    ui.poll()
    return w


def show_note(w, text, col):
    w["note"].color(col)
    w["note"].text(text)


def show_pump(w, sec_left):
    w["led"].value(1 if sec_left else 0)
    w["pump"].color(COL_COOL if sec_left else COL_DIM)
    w["pump"].text("หมุนอยู่ อีก %d วิ" % sec_left if sec_left else "พัดลมหยุด")
    w["arc"].value(sec_left)


def show_heat(w, soil, tank, hot_s, runs):
    hot = soil > SOIL_DRY
    w["thermo"].value(soil)
    pass  # Bar.color() ตั้งสีส่วนที่เติมบนบอร์ดจริง ใช้สีเดิมของ Bar ให้เห็นชัด
    w["heat"].color(COL_HOT if hot else COL_TEXT)
    w["heat"].text("%d %%" % soil)
    w["heat_t"].text("ร้อน ควรเปิดพัดลม" if hot else "")
    w["hot"].text(mmss(hot_s))
    w["runs"].text("เปิดพัดลมไป %d ครั้ง" % runs)
    w["tank_bar"].value(tank)
    pass  # Bar.color() ตั้งสีส่วนที่เติมบนบอร์ดจริง ใช้สีเดิมของ Bar ให้เห็นชัด
    w["tank"].text("%d %%" % tank)


# ---- 6) โปรแกรมหลัก ----
class Stop(Exception):
    # ไม่ใช้ SystemExit: บอร์ดอาจค้าง
    pass


def stop(w, pump, msg, col=COL_BAD):
    # ดับพัดลมก่อนเสมอ
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
    got = n = pump_ms = pump_t0 = runs = hot_ms = 0
    shown = -1
    t_send = t0 = t_last = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < LISTEN_MS:
        now = time.ticks_ms()
        if stop_btn.pressed_now() and pump_ms:                      # SW5 ไม่ผ่านเน็ตเลย
            pump_ms = 0
            show_note(w, "หยุดพัดลมฉุกเฉิน " + BTN_NAMES[0], COL_BAD)
            beep("stop")
        soil, tank = knob_percent(0), knob_percent(3)
        if soil > SOIL_DRY:
            hot_ms += time.ticks_diff(now, t_last)
        t_last = now
        msg = mqtt.get_message()       # None = ยังไม่มีอะไรมา / (topic, bytes)
        if msg is not None:
            got += 1
            w["title"].text("พัดลม = ไฟสีฟ้า (คำสั่ง %d)" % got)
            was = pump_ms
            pump_ms, pump_t0 = act_on(w, msg[1], tank, now, pump_ms, pump_t0)
            if pump_ms and not was:
                runs += 1
        left = pump_ms - time.ticks_diff(now, pump_t0) if pump_ms else 0
        if pump_ms and (left <= 0 or tank < TANK_MIN):   # ครบเวลาหรือแบตหมด
            pump_ms = 0
            show_note(w, "ครบเวลา หยุดพัดลมเอง" if left <= 0 else "แบตหมด หยุดพัดลมเอง", COL_COOL)
            beep("stop")
        set_led(pump, pump_ms)
        sec_left = (left + 999) // 1000 if pump_ms else 0 # ปัดขึ้น
        if sec_left != shown:
            shown = matrix_countdown(sec_left, shown)
            show_pump(w, sec_left)
        show_heat(w, soil, tank, hot_ms // 1000, runs)
        if time.ticks_diff(now, t_send) >= SEND_MS:
            t_send, n = now, n + 1
            if not send_report(n, soil, tank, pump_ms, hot_ms // 1000, runs):
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
# 1) ส่ง {"cmd":"pump","on":1,"sec":9999} หมุนกี่วิ ทำไมต้องมีเพดาน
# 2) ระดับ 2: ความร้อนสูงกว่า SOIL_DRY = เปิดพัดลมเองอัตโนมัติ เว้น 60 วิก่อนเปิดซ้ำ (ดู s1/sf1_03_auto_irrigation.py)
# 3) ใช้อุณหภูมิจริงจาก SHT40 แทน VR1 (ดู idea_8_coop.py read_climate) แล้วนับ hot_s จากองศาจริง
