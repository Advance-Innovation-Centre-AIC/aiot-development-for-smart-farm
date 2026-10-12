# idea_22_rice_mill.py - เครื่องสีข้าวชุมชน: สั่นผิดปกติ = หยุดเครื่อง ไม่รับคำสั่งเดินเครื่อง แจ้งมือถือ
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# ต่อยอดจาก s2/sf2_03 + กฎการสั่นจาก s3/sf3_03 / จอ: วงแหวนสุขภาพลูกปืน + กระบะข้าวเปลือก + ตัวนับรอบสี
# MQTT: ของ sf2_03 ครบ + telemetry "vib" (null ตอนเริ่ม) "vib_bad" 0/1 + event "vib" ตอนเปลี่ยน
#   + "wear" วินาทีที่สั่นแรงเกิน WEAR_K เท่า (ลูกปืนสึกสะสม)  "batches" รอบสีที่เดินครบเวลา
#   vib_bad 1 = ไม่รับคำสั่งเดินเครื่อง / VR1 = ข้าวสารในถังพัก (soil) VR4 = ข้าวเปลือกในกระบะ (tank)
# ลองเล่น: วางนิ่ง 5 วิแรก แล้วเขย่าบอร์ด (ลูกปืนสึก/สายพานหย่อน) / แก้ก่อนรัน: WIFI_SSID, WIFI_PASS, TEAM

import buttons
import gpio
import json
import math
import mqtt
import pots
import rgbmatrix
import sensors
import time
import ui
import wifi

# ---- 1) ตั้งค่า ----
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"
TEAM = "teamXX"

BROKER = "broker.hivemq.com"
CLIENT_ID = "bento-farm-" + TEAM
TOPIC_CMD = "bento-aiot/" + TEAM + "/cmd"
TOPIC = "bento-aiot/" + TEAM + "/telemetry"
TOPIC_EVENT = "bento-aiot/" + TEAM + "/event"
BTN_NAMES = ("SW5", "SW6")
PUMP_DEFAULT_S, PUMP_MAX_S = 10, 30
TANK_MIN = 10  # ข้าวเปลือก ต่ำกว่านี้ห้ามเปิด
LEARN_MS, WIN = 5000, 10
K_BAD, MIN_BASE, CONFIRM_N = 4.0, 0.05, 5  # ค่าสาธิต
WEAR_K = 2.0  # สั่นเกิน 2 เท่าของปกติ = ลูกปืนกำลังสึก
WEAR_LIFE_S = 120  # ค่าสาธิต: สึกสะสมครบเท่านี้ สุขภาพ = 0 %
SEND_MS, POLL_MS, LISTEN_MS = 5000, 100, 1800000

SPEAKER = 40
VOLUME = 25
# ธีมโรงสี: ฟางข้าว + ทอง
COL_TEXT, COL_DIM = 0xFFF6E0, 0xB3A27E
COL_CARD = 0x2A2112
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x8BC34A, 0xFF9F1C, 0xE4572E, 0xE9B949


# ---- 2) ฮาร์ดแวร์ ----
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)


def knob_percent(i):
    return pots.read(i) * 100 // 4095


def magnitude():
    try:
        ax, ay, az, _, _, _ = sensors.bmi270.motion()
    except OSError:
        return None
    return math.sqrt(ax * ax + ay * ay + az * az)


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
    if isinstance(sec, bool) or not isinstance(sec, int) or sec <= 0:
        sec = PUMP_DEFAULT_S
    return min(sec, PUMP_MAX_S)


def ascii_only(text):
    # จอไฟ RGB รับเฉพาะอักษรอังกฤษตัวใหญ่
    return "".join(c for c in str(text) if " " <= c <= "~")[:20].upper()


def handle_command(raw, tank, bad):
    # -> (ทำอะไร, ค่า, ข้อความจอ, สี)
    try:
        cmd = json.loads(raw.decode())
    except ValueError:
        cmd = None
    if not isinstance(cmd, dict):
        return "deny", 0, "อ่านคำสั่งไม่ได้", COL_BAD
    act, on = cmd.get("cmd", ""), cmd.get("on", 1)
    if act == "led" and cmd.get("n", 0) != 0:
        return "deny", 0, "ไม่มีเครื่องที่ " + str(cmd.get("n"))[:6], COL_WARN
    if act in ("led", "pump") and not on:
        return "off", 0, "สั่งหยุดเครื่องสี", COL_OK
    if act in ("led", "pump") and bad:
        return "deny", 0, "ลูกปืนสั่นผิดปกติ ไม่เดินเครื่อง", COL_BAD
    if act in ("led", "pump") and tank < TANK_MIN:
        return "deny", 0, "ข้าวเปลือกเหลือ %d%% ไม่เดินเครื่อง" % tank, COL_BAD
    if act in ("led", "pump"):
        sec = pump_seconds(cmd.get("sec", PUMP_DEFAULT_S))
        return "on", sec, "เดินเครื่องสี %d วิ" % sec, COL_OK
    if act == "beep":
        return "beep", 0, "มีคนเรียกที่โรงสี!", COL_INFO
    if act == "say":
        text = ascii_only(cmd.get("text", ""))
        return "say", text, "ข้อความ: " + (text or "ไม่มีอักษรอังกฤษ"), COL_INFO
    if act == "ack":  # ack เป็นของ sf2_04
        return "", 0, "ไม่มีแจ้งเตือน (sf2_04)", COL_INFO
    if act != "":
        return "deny", 0, "ไม่รู้จัก " + str(act)[:12], COL_WARN
    return "", 0, "", COL_INFO


class Vib:
    # ย่อจาก sf3_03 + นับเวลาสึก
    def __init__(self, now):
        self.buf, self.vib, self.base, self.total, self.n = [], None, None, 0.0, 0
        self.t0, self.bad, self.streak = now, False, 0
        self.wear_ms = 0

    def push(self, m, now):
        self.buf.append(m)
        if len(self.buf) > WIN:
            self.buf.pop(0)
        if len(self.buf) < WIN:
            return False
        mean = sum(self.buf) / WIN
        self.vib = math.sqrt(sum((x - mean) * (x - mean) for x in self.buf) / WIN)
        if self.base is None:
            self.total, self.n = self.total + self.vib, self.n + 1
            if time.ticks_diff(now, self.t0) >= LEARN_MS:
                self.base = max(MIN_BASE, self.total / self.n)
            return False
        self.streak = self.streak + 1 if (self.vib > self.base * K_BAD) != self.bad else 0
        if self.streak < CONFIRM_N:
            return False
        self.bad, self.streak = not self.bad, 0
        return True

    def wear(self, dt):
        # สั่นเกิน WEAR_K เท่า = ลูกปืนสึกเพิ่ม
        if self.base is not None and self.vib > self.base * WEAR_K:
            self.wear_ms += dt
        return self.wear_ms // 1000


def health(wear_s):
    return max(0, 100 - wear_s * 100 // WEAR_LIFE_S)


# ---- 4) เครือข่าย ----
def connect_broker(w):
    # ลอง 3 ครั้ง
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
    show_note(w, "ต่อ WiFi... จอนิ่งได้", COL_WARN)
    ui.poll()
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "ต่อ WiFi ไม่ได้"
    linked = connect_broker(w)
    if not linked or not mqtt.subscribe(TOPIC_CMD):
        return "broker ไม่ตอบ: รอ 1 นาทีแล้วรันใหม่"
    return ""


def send(topic, obj):
    try:
        mqtt.publish(topic, json.dumps(obj))
        return True
    except OSError:
        return False


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    return ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: เชื่อมต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("โรงสีข้าวชุมชน ฟังลูกปืน", x=12, y=6, color=COL_TEXT, value=24)
    w = {}
    card(12, 44, 290, 300, "สุขภาพลูกปืน")
    w["health"] = ui.Arc(x=46, y=78, w=180, h=180, min=0, max=100, value=100)
    w["health"].color(COL_OK)
    w["pct"] = ui.Label("100 %", x=98, y=152, color=COL_TEXT, value=28)
    w["wear"] = ui.Label("สึกสะสม 0 วิ", x=24, y=268, color=COL_DIM, value=16)
    w["verdict"] = ui.Label("วางนิ่ง 5 วิ เรียนรู้", x=24, y=298, color=COL_WARN, value=20)
    card(312, 44, 130, 300, "กระบะ (VR4)")
    w["tank_bar"] = ui.Bar(x=332, y=80, w=50, h=210, min=0, max=100, value=0)
    ui.Label("เต็ม", x=392, y=80, color=COL_DIM, value=14)
    ui.Label("ต่ำ", x=392, y=262, color=COL_BAD, value=14)
    w["tank"] = ui.Label("-- %", x=332, y=304, color=COL_TEXT, value=16)
    w["title"] = card(452, 44, 328, 300, "เครื่องสี = ไฟสีฟ้า (คำสั่ง 0)")
    w["led"] = ui.Led(x=466, y=80, w=40, h=40, color=COL_INFO)
    w["mill"] = ui.Label("หยุด", x=518, y=86, color=COL_DIM, value=24)
    w["left"] = ui.Bar(x=466, y=132, w=300, h=14, min=0, max=PUMP_MAX_S, value=0)
    ui.Label("สีครบรอบแล้ว", x=466, y=160, color=COL_DIM, value=16)
    w["batches"] = ui.Seg7(text="0", x=466, y=186, w=120, h=50, color=COL_INFO)
    ui.Label("รอบ", x=530, y=200, color=COL_DIM, value=16)
    w["rice"] = ui.Label("ข้าวสาร (VR1) -- %", x=466, y=254, color=COL_TEXT, value=16)
    ui.Label(BTN_NAMES[0] + " = หยุดฉุกเฉิน", x=466, y=290, color=COL_DIM, value=14)
    w["note"] = ui.Label("ยังไม่มีคำสั่ง", x=12, y=352, color=COL_DIM)
    w["mq"] = ui.Led(x=606, y=12, w=18, h=18, color=COL_OK, value=0)
    w["mq_t"] = ui.Label("MQTT: ออฟไลน์", x=632, y=10, color=COL_DIM, value=16)
    ui.poll()
    return w


def show_note(w, text, col):
    w["note"].color(col)
    w["note"].text(text)


def show_pump(w, sec_left):
    w["led"].value(1 if sec_left else 0)
    w["mill"].color(COL_OK if sec_left else COL_DIM)
    w["mill"].text("เดิน อีก %d วิ" % sec_left if sec_left else "หยุด")
    w["left"].value(sec_left)


def show_vib(w, vib, wear_s, last):
    # วาดใหม่เมื่อเปลี่ยน
    if vib.base is None:
        return last
    hp = health(wear_s)
    rough = vib.vib > vib.base * WEAR_K
    if (hp, vib.bad, rough) == last:
        return last
    col = COL_BAD if vib.bad or hp < 30 else (COL_WARN if hp < 70 or rough else COL_OK)
    w["health"].value(hp)
    w["health"].color(col)
    w["pct"].text("%d %%" % hp)
    w["wear"].text("สึกสะสม %d วิ" % wear_s)
    w["verdict"].color(col)
    w["verdict"].text("ผิดปกติ! หยุดเครื่อง" if vib.bad else ("สั่นแรง กำลังสึก" if rough else "เดินเรียบ"))
    return (hp, vib.bad, rough)


def show_knobs(w, soil, tank):
    w["rice"].text("ข้าวสาร (VR1) %d %%" % soil)
    w["tank_bar"].value(tank)
    w["tank_bar"].color(COL_DIM if tank >= TANK_MIN else COL_BAD)
    w["tank"].text("%d %%" % tank)


# ---- 6) โปรแกรมหลัก ----
class Stop(Exception):
    pass


def stop(w, pump, msg, col=COL_BAD):
    # ดับปั๊มก่อนเสมอ
    show_link(w, False)
    set_led(pump, False)
    rgbmatrix.scroll("")
    rgbmatrix.clear()
    show_note(w, msg, col)
    ui.poll()
    raise Stop


def on_vib(w, vib, pump):
    # หยุดเครื่อง แจ้งออกเน็ต
    rgbmatrix.scroll("")
    rgbmatrix.clear()
    if vib.bad:
        set_led(pump, False)
        show_pump(w, 0)
        show_note(w, "สั่นผิดปกติ! หยุดเครื่องสีแล้ว เรียกช่าง", COL_BAD)
        rgbmatrix.scroll("MILL STOP", rgbmatrix.RED, 80)
        beep("empty")
    else:
        show_note(w, "สั่นปกติแล้ว เดินเครื่องได้", COL_OK)
        beep("good")
    if not send(TOPIC_EVENT, {"id": TEAM, "event": "vib", "bad": int(vib.bad), "vib": round(vib.vib, 2)}):
        stop(w, pump, "สายหลุด")


def act_on(w, raw, tank, now, pump_ms, pump_t0, bad):
    do, arg, text, col = handle_command(raw, tank, bad)
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
    got = n = pump_ms = pump_t0 = batches = wear_s = 0
    shown, drawn = -1, None
    t_send = t_loop = t0 = time.ticks_ms()
    vib = Vib(t0)
    while time.ticks_diff(time.ticks_ms(), t0) < LISTEN_MS:
        now = time.ticks_ms()
        if stop_btn.pressed_now() and pump_ms:                      # SW5 ไม่ผ่านเน็ตเลย
            pump_ms = 0
            show_note(w, "หยุดฉุกเฉิน " + BTN_NAMES[0], COL_BAD)
            beep("stop")
        m = magnitude()
        if m is not None and vib.push(m, now):
            on_vib(w, vib, pump)
            pump_ms, shown = 0, 0
        wear_s = vib.wear(time.ticks_diff(now, t_loop))
        t_loop = now
        soil, tank = knob_percent(0), knob_percent(3)
        msg = mqtt.get_message()
        if msg is not None:
            got += 1
            w["title"].text("เครื่องสี = ไฟสีฟ้า (คำสั่ง %d)" % got)
            pump_ms, pump_t0 = act_on(w, msg[1], tank, now, pump_ms, pump_t0, vib.bad)
        left = pump_ms - time.ticks_diff(now, pump_t0) if pump_ms else 0
        if pump_ms and (left <= 0 or tank < TANK_MIN):
            pump_ms = 0
            if left <= 0:                                           # เดินครบเวลา = สีเสร็จ 1 รอบ
                batches += 1
                w["batches"].text(str(batches))
            show_note(w, "สีเสร็จ 1 รอบ หยุดเอง" if left <= 0 else "ข้าวเปลือกหมดกระบะ หยุดเอง", COL_INFO)
            beep("stop")
        set_led(pump, pump_ms)
        sec_left = (left + 999) // 1000 if pump_ms else 0
        if sec_left != shown:
            shown = matrix_countdown(sec_left, shown)
            show_pump(w, sec_left)
        show_knobs(w, soil, tank)
        drawn = show_vib(w, vib, wear_s, drawn)
        if time.ticks_diff(now, t_send) >= SEND_MS:
            t_send, n = now, n + 1
            v = None if vib.vib is None else round(vib.vib, 2)
            if not send(TOPIC, {"id": TEAM, "n": n, "soil": soil, "tank": tank, "pump": 1 if pump_ms else 0,
                                "sim": "soil tank", "vib": v, "vib_bad": int(vib.bad),
                                "wear": wear_s, "batches": batches}):
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
# 1) CONFIRM_N = 1 แล้วเคาะโต๊ะทีเดียว เทียบกับ 5
# 2) สุขภาพต่ำกว่า 30 % = ไม่รับคำสั่งเดินเครื่อง จนช่างกด SW6 (Button(0)) ยืนยันว่าเปลี่ยนลูกปืนแล้ว
