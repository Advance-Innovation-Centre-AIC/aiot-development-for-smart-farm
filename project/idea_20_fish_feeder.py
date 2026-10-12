# idea_20_fish_feeder.py - เครื่องให้อาหารปลา: สั่งให้อาหารจากมือถือ ถังอาหารหมดไม่ยอมหมุน มีเพดานเวลา
# แม่แบบ B · จอ: เกจถังอาหารวงใหญ่ + นับมื้อ + นับถอยหลังมื้อถัดไป
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# ต่อยอดจาก s2/sf2_03_remote_pump.py (เน็ต คำสั่ง กฎความปลอดภัยเหมือนเดิม)
# ทีมต้องแก้ 3 อย่าง: 1) SOIL_DRY PUMP_MAX_S MEAL_EVERY_S 2) หัวจอและป้าย 3) เสียงใน TUNES
# MQTT: หัวข้อ/คีย์/คำสั่งเหมือน sf2_03 ทุกตัว (สัญญา 3.2, 4) · soil = อาหารเหลือในบ่อ (VR1) · tank = อาหารในถังเก็บ (VR4)
#   คีย์เพิ่ม: meals = ให้อาหารไปกี่มื้อ · next_s = อีกกี่วินาทีถึงมื้อถัดไป (0 = ถึงเวลาแล้ว)
# บอร์ดแค่เตือนว่าถึงมื้อ ไม่ให้เอง (ตาคุณข้อ 2)
# เกณฑ์ = ค่าสาธิตในห้องเรียน ไม่ใช่คำแนะนำทางเกษตร
#
# ลองเล่น  : สั่งจาก web/idea_20.html?team=<เลขกลุ่ม> · หมุน VR4 ลงต่ำกว่า 10 = ถังอาหารหมด
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
TANK_MIN = 10  # ต่ำกว่านี้ห้ามหมุน (เครื่องหมุนเปล่า)
SOIL_DRY = 30  # ค่าสาธิต: อาหารในบ่อ (VR1) ต่ำกว่านี้ขึ้นว่าหมด
MEAL_EVERY_S = 120  # ค่าสาธิต: มื้อละ 2 นาที (ของจริงหลายชั่วโมง)
SEND_MS, POLL_MS, LISTEN_MS = 5000, 100, 1800000

SPEAKER = 40
VOLUME = 25
# สีบ่อปลา
COL_TEXT, COL_DIM = 0xE6F4F8, 0x86A4B0
COL_CARD, COL_TRACK = 0x0F2633, 0x1E4252
COL_FISH, COL_POND = 0xFF8C42, 0x2EC4B6
COL_OK, COL_WARN, COL_BAD = 0x5CD08A, 0xFFD166, 0xEF476F


# ---- 2) ฮาร์ดแวร์ ----
TUNES = {"tap": (79,), "start": (72, 76, 79, 84), "stop": (84, 72), "meal": (84, 88, 84),
         "bad": (84, 76), "hit": (91,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 80)
        time.sleep_ms(90)

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
        rgbmatrix.score(sec_left, rgbmatrix.YELLOW)
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
        return "deny", 0, "ไม่มีเครื่องที่ " + str(cmd.get("n"))[:8], COL_WARN
    if act in ("led", "pump") and not on:
        return "off", 0, "สั่งหยุดให้อาหาร", COL_OK
    if act in ("led", "pump") and tank < TANK_MIN:
        return "deny", 0, "อาหารในถังเหลือ %d%% ไม่ให้" % tank, COL_BAD
    if act in ("led", "pump"):
        sec = pump_seconds(cmd.get("sec", PUMP_DEFAULT_S))
        return "on", sec, "ให้อาหาร %d วิ" % sec, COL_OK
    if act == "beep":
        return "beep", 0, "เจ้าของบ่อเรียก!", COL_POND
    if act == "say":
        text = ascii_only(cmd.get("text", ""))
        return "say", text, "ข้อความ: " + (text or "ไม่มีอักษรอังกฤษ"), COL_POND
    if act == "ack":  # ack เป็นของ sf2_04
        return "", 0, "ไม่มีแจ้งเตือน (sf2_04)", COL_POND
    if act != "":
        return "deny", 0, "ไม่รู้จัก " + str(act)[:12], COL_WARN
    return "", 0, "", COL_POND


def mmss(sec):
    return "%02d:%02d" % (sec // 60, sec % 60)


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


def send_report(n, soil, tank, running, meals, next_s):
    # publish ตอนสายหลุดโยน OSError
    try:
        mqtt.publish(TOPIC, json.dumps({"id": TEAM, "n": n, "soil": soil, "tank": tank,
                                        "pump": 1 if running else 0, "sim": "soil tank",
                                        "meals": meals, "next_s": next_s}))
        return True
    except OSError:
        return False


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_TRACK, max=18, value=2)
    return ui.Label(title, x=x + 12, y=y + 6, color=COL_POND, value=16)


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: เชื่อมต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("บ่อปลา: ให้อาหารจากมือถือ", x=12, y=6, color=COL_TEXT, value=24)
    w = {}
    card(12, 44, 262, 290, "อาหารในถังเก็บ VR4")
    w["hopper"] = ui.Arc(x=38, y=76, w=210, h=210, min=0, max=100, value=0)
    w["hopper_t"] = ui.Label("-- %", x=104, y=160, color=COL_TEXT, value=28)
    ui.Label("ต่ำกว่า %d%% ไม่หมุน" % TANK_MIN, x=70, y=296, color=COL_DIM, value=14)
    w["title"] = card(286, 44, 246, 290, "เครื่องให้อาหาร = ไฟสีฟ้า")
    w["led"] = ui.Led(x=302, y=80, w=40, h=40, color=COL_FISH)
    w["feed"] = ui.Label("พัก", x=354, y=84, color=COL_DIM, value=24)
    w["left"] = ui.Bar(x=302, y=134, w=214, h=14, min=0, max=PUMP_MAX_S, value=0)
    ui.Label("อาหารเหลือในบ่อ VR1", x=302, y=170, color=COL_POND, value=16)
    w["pond"] = ui.Bar(x=302, y=198, w=214, h=26, min=0, max=100, value=0)
    w["pond_t"] = ui.Label("-- %", x=302, y=232, color=COL_TEXT, value=16)
    ui.Label(BTN_NAMES[0] + " = หยุดให้อาหารฉุกเฉิน", x=302, y=296, color=COL_WARN, value=14)
    # มุมขวาล่างสุดเว้นไว้ให้ปุ่ม Console
    card(544, 44, 236, 150, "มื้อที่ให้ไปแล้ว")
    w["meals"] = ui.Seg7(text="0", x=560, y=78, w=110, h=64, color=COL_FISH)
    ui.Label("มื้อ", x=640, y=110, color=COL_DIM, value=16)
    card(544, 204, 236, 126, "มื้อถัดไปอีก")
    w["next"] = ui.Label("--:--", x=560, y=234, color=COL_TEXT, value=28)
    w["next_t"] = ui.Label("ทุก %d วิ (สาธิต)" % MEAL_EVERY_S, x=560, y=280, color=COL_DIM, value=14)
    w["note"] = ui.Label("ยังไม่มีคำสั่ง", x=12, y=350, color=COL_DIM)
    w["mq"] = ui.Led(x=600, y=12, w=18, h=18, color=COL_OK, value=0)
    w["mq_t"] = ui.Label("MQTT: ออฟไลน์", x=626, y=10, color=COL_DIM, value=16)
    ui.poll()
    return w


def show_note(w, text, col):
    w["note"].color(col)
    w["note"].text(text)


def show_feed(w, sec_left):
    w["led"].value(1 if sec_left else 0)
    w["feed"].color(COL_FISH if sec_left else COL_DIM)
    w["feed"].text("โปรยอยู่ อีก %d วิ" % sec_left if sec_left else "พัก")
    w["left"].value(sec_left)


def show_pond(w, soil, tank, meals, next_s):
    w["hopper"].value(tank)
    w["hopper"].color(COL_BAD if tank < TANK_MIN else (COL_WARN if tank < 30 else COL_POND))
    w["hopper_t"].text("%d %%" % tank)
    w["pond"].value(soil)
    pass  # Bar.color() ตั้งสีส่วนที่เติมบนบอร์ดจริง ใช้สีเดิมของ Bar ให้เห็นชัด
    w["pond_t"].text("%d %%%s" % (soil, " หมด ควรให้" if soil < SOIL_DRY else ""))
    w["meals"].text(str(meals))
    w["next"].color(COL_WARN if next_s == 0 else COL_TEXT)
    w["next"].text("ถึงเวลา!" if next_s == 0 else mmss(next_s))


# ---- 6) โปรแกรมหลัก ----
class Stop(Exception):
    # ไม่ใช้ SystemExit
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
    got = n = pump_ms = pump_t0 = meals = 0
    shown, due = -1, False
    t_send = t0 = t_meal = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < LISTEN_MS:
        now = time.ticks_ms()
        if stop_btn.pressed_now() and pump_ms:                      # SW5 ไม่ผ่านเน็ตเลย
            pump_ms = 0
            show_note(w, "หยุดให้อาหารฉุกเฉิน " + BTN_NAMES[0], COL_BAD)
            beep("stop")
        soil, tank = knob_percent(0), knob_percent(3)
        msg = mqtt.get_message()
        if msg is not None:
            got += 1
            w["title"].text("เครื่องให้อาหาร (คำสั่ง %d)" % got)
            was = pump_ms
            pump_ms, pump_t0 = act_on(w, msg[1], tank, now, pump_ms, pump_t0)
            if pump_ms and not was: # มื้อใหม่
                meals, t_meal, due = meals + 1, now, False
        left = pump_ms - time.ticks_diff(now, pump_t0) if pump_ms else 0
        if pump_ms and (left <= 0 or tank < TANK_MIN):   # ครบเวลาหรือถังอาหารหมด
            pump_ms = 0
            show_note(w, "ครบเวลา หยุดให้เอง" if left <= 0 else "ถังอาหารหมด หยุดเอง", COL_POND)
            beep("stop")
        set_led(pump, pump_ms)
        sec_left = (left + 999) // 1000 if pump_ms else 0     # ปัดขึ้น
        if sec_left != shown:
            shown = matrix_countdown(sec_left, shown)
            show_feed(w, sec_left)
        next_s = max(0, MEAL_EVERY_S - time.ticks_diff(now, t_meal) // 1000)
        if next_s == 0 and not due: # เตือนครั้งเดียว
            due = True
            show_note(w, "ถึงมื้อแล้ว สั่งให้อาหารจากแอป", COL_WARN)
            beep("meal")
        show_pond(w, soil, tank, meals, next_s)
        if time.ticks_diff(now, t_send) >= SEND_MS:
            t_send, n = now, n + 1
            if not send_report(n, soil, tank, pump_ms, meals, next_s):
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
# 1) ส่ง {"cmd":"pump","on":1,"sec":9999} หมุนกี่วิ ทำไมต้องมีเพดาน (ให้มากไปน้ำเสีย)
# 2) ระดับ 2: ให้อาหารเองเมื่อ next_s ถึง 0 และไม่ให้ซ้ำถ้าอาหารในบ่อยังเหลือ
# 3) ส่ง event ตอนถังอาหารต่ำกว่า 20 % ให้เจ้าของไปเติม
