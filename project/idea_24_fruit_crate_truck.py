# idea_24_fruit_crate_truck.py - ลังผลไม้บนรถกระบะ: แม่ค้ารู้ว่าผลไม้ช้ำช่วงไหนของทาง กระแทกต่อเนื่อง = แจ้งคนขับชะลอ
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# ต่อยอดจาก s2/sf2_03 + กฎการสั่นจาก s3/sf3_03 / จอ: แถบช้ำ 8 ช่วงตามระยะทาง + นาฬิกาเที่ยว + เกรดผลไม้
# MQTT: ของ sf2_03 ครบ + telemetry "vib" (null ตอนเริ่ม) "vib_bad" 0/1 + event "vib" ตอนเปลี่ยน
#   + "bumps" ครั้งที่กระแทกในเที่ยวนี้  "bruise_at" ระยะทาง % ที่กระแทกแรงสุด (null = ยังไม่มี)
#   ไฟสีฟ้า (pump/led) = ไฟขับปกติจากแม่ค้า / vib_bad 1 = ไม่รับคำสั่งเปิดไฟ / VR1 = ระยะทาง VR4 = แบต
# ลองเล่น: วางนิ่ง 5 วิแรก (รถจอด) หมุน VR1 ไปตามทาง แล้วเคาะบอร์ด / แก้ก่อนรัน: WIFI_SSID, WIFI_PASS, TEAM

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
TANK_MIN = 10  # แบตกล่องเฝ้า ต่ำกว่านี้ห้ามเปิด
LEARN_MS, WIN = 5000, 10
K_BAD, MIN_BASE, CONFIRM_N = 4.0, 0.05, 5  # ค่าสาธิต
SEV_STEP = K_BAD  # ระดับกระแทก: เกิน 1, 2, 3 เท่าของ SEV_STEP = เบา กลาง แรง
SEGS = 8  # แบ่งทางสวนถึงตลาดเป็น 8 ช่วง
GRADE_BUMPS = (3, 8)  # กระแทกครบ = เกรด B / C (เท่ากับหน้าเว็บ)
SEND_MS, POLL_MS, LISTEN_MS = 5000, 100, 1800000

SPEAKER = 40
VOLUME = 25
# ธีมแผงผลไม้: เขียวใบมะม่วง + ส้มมะม่วงสุก
COL_TEXT, COL_DIM = 0xFFF4E6, 0xA8B59A
COL_CARD = 0x1A2614
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x9CCC65, 0xFFB020, 0xE5533D, 0xFFD180


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
        return "deny", 0, "ไม่มีไฟดวงที่ " + str(cmd.get("n"))[:6], COL_WARN
    if act in ("led", "pump") and not on:
        return "off", 0, "สั่งดับไฟขับปกติ", COL_OK
    if act in ("led", "pump") and bad:
        return "deny", 0, "ลังยังกระแทก ให้ชะลอก่อน", COL_BAD
    if act in ("led", "pump") and tank < TANK_MIN:
        return "deny", 0, "แบตเหลือ %d%% ไม่เปิดไฟ" % tank, COL_BAD
    if act in ("led", "pump"):
        sec = pump_seconds(cmd.get("sec", PUMP_DEFAULT_S))
        return "on", sec, "ไฟขับปกติ %d วิ" % sec, COL_OK
    if act == "beep":
        return "beep", 0, "แม่ค้าเรียกคนขับ!", COL_INFO
    if act == "say":
        text = ascii_only(cmd.get("text", ""))
        return "say", text, "ข้อความ: " + (text or "ไม่มีอักษรอังกฤษ"), COL_INFO
    if act == "ack":  # ack เป็นของ sf2_04
        return "", 0, "ไม่มีแจ้งเตือน (sf2_04)", COL_INFO
    if act != "":
        return "deny", 0, "ไม่รู้จัก " + str(act)[:12], COL_WARN
    return "", 0, "", COL_INFO


class Vib:
    # ย่อจาก sf3_03 
    def __init__(self, now):
        self.buf, self.vib, self.base, self.total, self.n = [], None, None, 0.0, 0
        self.t0, self.bad, self.streak = now, False, 0

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

    def ratio(self):
        return 0 if self.base is None else self.vib / self.base


def severity(r):
    # 0 = ไม่กระแทก 1 เบา 2 กลาง 3 แรง
    return min(3, int(r // SEV_STEP))


class Route:
    # หนึ่งเที่ยว: ช้ำสุดของแต่ละช่วงทาง + ที่ไหนแรงสุด
    def __init__(self, now):
        self.t0, self.bumps, self.hard, self.sev, self.big = now, 0, 0, 0, False
        self.segs, self.worst, self.worst_at = [0] * SEGS, 0, None

    def feed(self, s, where):
        # คืนช่วงทางที่ต้องวาดใหม่ หรือ -1
        if s and not self.sev:
            self.bumps, self.big = self.bumps + 1, False
        if s >= 2 and not self.big:                    # ครั้งนี้แรง นับครั้งเดียว
            self.hard, self.big = self.hard + 1, True
        self.sev = s
        i = min(SEGS - 1, where * SEGS // 100)
        if s <= self.segs[i]:
            return -1
        self.segs[i] = s
        if s > self.worst:
            self.worst, self.worst_at = s, where
        return i

    def grade(self):
        return 0 if self.bumps < GRADE_BUMPS[0] else (1 if self.bumps < GRADE_BUMPS[1] else 2)

    def seconds(self, now):
        return time.ticks_diff(now, self.t0) // 1000


def mmss(sec):
    return "%02d:%02d" % (sec // 60 % 100, sec % 60)


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
    ui.Label("ลังผลไม้บนกระบะ ช้ำช่วงไหน", x=12, y=6, color=COL_TEXT, value=24)
    w = {}
    card(12, 44, 768, 150, "ช้ำตรงไหนของทาง (สูง = แรง / VR1 = ระยะทาง)")
    w["segs"] = []
    for i in range(SEGS):
        w["segs"].append(ui.Bar(x=48 + i * 92, y=74, w=40, h=66, min=0, max=3, value=0))
    ui.Label("สวน", x=28, y=150, color=COL_DIM, value=16)
    w["where"] = ui.Bar(x=88, y=158, w=600, h=10, min=0, max=100, value=0)
    ui.Label("ตลาด", x=706, y=150, color=COL_DIM, value=16)
    card(12, 204, 300, 140, "เวลาเดินทาง")
    w["trip"] = ui.Seg7(text="00:00", x=28, y=232, w=200, h=56, color=COL_INFO)
    w["bumps"] = ui.Label("กระแทก 0 ครั้ง", x=28, y=292, color=COL_TEXT, value=16)
    ui.Label(BTN_NAMES[1] + " = เริ่มเที่ยวใหม่", x=28, y=318, color=COL_DIM, value=14)
    card(322, 204, 250, 140, "เกรดผลไม้ถึงตลาด")
    w["grade"] = ui.Label("A ราคาเต็ม", x=336, y=236, color=COL_OK, value=28)
    w["worst"] = ui.Label("ยังไม่ช้ำ", x=336, y=284, color=COL_DIM, value=16)
    w["title"] = card(582, 204, 198, 140, "ไฟขับ (คำสั่ง 0)")
    w["led"] = ui.Led(x=596, y=238, w=32, h=32, color=COL_INFO)
    w["pump"] = ui.Label("ดับ", x=638, y=240, color=COL_DIM, value=20)
    w["tank"] = ui.Label("แบต -- %", x=596, y=286, color=COL_TEXT, value=16)
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
    w["pump"].color(COL_OK if sec_left else COL_DIM)
    w["pump"].text("%d วิ" % sec_left if sec_left else "ดับ")


def show_route(w, route):
    for i in range(SEGS):
        w["segs"][i].value(route.segs[i])
        w["segs"][i].color(COL_BAD if route.segs[i] >= 2 else COL_DIM)
    g = route.grade()
    w["grade"].color((COL_OK, COL_WARN, COL_BAD)[g])
    w["grade"].text(("A ราคาเต็ม", "B ลดราคานิด", "C คัดช้ำออก")[g])
    w["bumps"].text("กระแทก %d ครั้ง (แรง %d)" % (route.bumps, route.hard))
    if route.worst_at is None:
        w["worst"].text("ยังไม่ช้ำ")
    else:
        w["worst"].text("ช้ำสุดที่ระยะ %d %%" % route.worst_at)


def show_knobs(w, where, tank):
    w["where"].value(where)
    w["tank"].color(COL_TEXT if tank >= TANK_MIN else COL_BAD)
    w["tank"].text("แบต %d %%" % tank)


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
    # ดับไฟขับปกติ บอกคนขับชะลอ แจ้งแม่ค้า
    rgbmatrix.scroll("")
    rgbmatrix.clear()
    if vib.bad:
        set_led(pump, False)
        show_pump(w, 0)
        show_note(w, "ลังกระแทกต่อเนื่อง! แจ้งคนขับชะลอ", COL_BAD)
        rgbmatrix.scroll("BUMP", rgbmatrix.RED, 80)
        beep("empty")
    else:
        show_note(w, "ถนนเรียบแล้ว ขับต่อได้", COL_OK)
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
    stop_btn, trip_btn = Button(1), Button(0)
    got = n = pump_ms = pump_t0 = 0
    shown, sec_shown = -1, -1
    t_send = t0 = time.ticks_ms()
    vib, route = Vib(t0), Route(t0)
    while time.ticks_diff(time.ticks_ms(), t0) < LISTEN_MS:
        now = time.ticks_ms()
        if stop_btn.pressed_now() and pump_ms:                      # SW5 ไม่ผ่านเน็ตเลย
            pump_ms = 0
            show_note(w, "ดับไฟขับปกติ " + BTN_NAMES[0], COL_BAD)
            beep("stop")
        where, tank = knob_percent(0), knob_percent(3)
        if trip_btn.pressed_now():                                  # SW6 = เที่ยวใหม่
            route = Route(now)
            show_route(w, route)
            show_note(w, "เริ่มเที่ยวใหม่ ล้างแถบช้ำ", COL_INFO)
            beep("tap")
        m = magnitude()
        if m is not None and vib.push(m, now):
            on_vib(w, vib, pump)
            pump_ms, shown = 0, 0
        if vib.base is not None:
            before = (route.bumps, route.hard)
            if route.feed(severity(vib.ratio()), where) >= 0 or (route.bumps, route.hard) != before:
                show_route(w, route)
                if route.bumps != before[0]:
                    beep("hit")
        msg = mqtt.get_message()
        if msg is not None:
            got += 1
            w["title"].text("ไฟขับ (คำสั่ง %d)" % got)
            pump_ms, pump_t0 = act_on(w, msg[1], tank, now, pump_ms, pump_t0, vib.bad)
        left = pump_ms - time.ticks_diff(now, pump_t0) if pump_ms else 0
        if pump_ms and (left <= 0 or tank < TANK_MIN):
            pump_ms = 0
            show_note(w, "ครบเวลา ดับเอง" if left <= 0 else "แบตหมด ดับเอง", COL_INFO)
            beep("stop")
        set_led(pump, pump_ms)
        sec_left = (left + 999) // 1000 if pump_ms else 0
        if sec_left != shown:
            shown = matrix_countdown(sec_left, shown)
            show_pump(w, sec_left)
        if route.seconds(now) != sec_shown:
            sec_shown = route.seconds(now)
            w["trip"].text(mmss(sec_shown))
        show_knobs(w, where, tank)
        if time.ticks_diff(now, t_send) >= SEND_MS:
            t_send, n = now, n + 1
            v = None if vib.vib is None else round(vib.vib, 2)
            if not send(TOPIC, {"id": TEAM, "n": n, "soil": where, "tank": tank, "pump": 1 if pump_ms else 0,
                                "sim": "soil tank", "vib": v, "vib_bad": int(vib.bad),
                                "bumps": route.bumps, "bruise_at": route.worst_at}):
                stop(w, pump, "สายหลุด")
        if not mqtt.is_connected():
            stop(w, pump, "สายหลุด")
        ui.poll()
        wait_ms(POLL_MS, (stop_btn, trip_btn))
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
# 1) เปลี่ยน SEGS เป็นชื่อหมู่บ้านจริงบนทาง แล้วให้จอบอกชื่อช่วงที่ช้ำ
# 2) จบเที่ยว (SW6) ส่ง event "trip" พร้อม segs ไปให้แม่ค้าตั้งราคา
