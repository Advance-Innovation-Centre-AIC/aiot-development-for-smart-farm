# idea_23_egg_tray_transit.py - ถาดไข่ระหว่างขนส่ง: นับแรงกระแทกทั้งเที่ยว กระแทกต่อเนื่อง = แจ้งคนขับชะลอ แจ้งมือถือ
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# ต่อยอดจาก s2/sf2_03 + กฎการสั่นจาก s3/sf3_03 / จอ: ตัวนับกระแทก + นาฬิกาเที่ยว (Seg7) กราฟแรง แถบระดับ
# MQTT: ของ sf2_03 ครบ + telemetry "vib" (null ตอนเริ่ม) "vib_bad" 0/1 + event "vib" ตอนเปลี่ยน
#   + "bumps" ครั้งที่กระแทกในเที่ยวนี้  "trip_s" วินาทีตั้งแต่เริ่มเที่ยว
#   ไฟสีฟ้า (pump/led) = ไฟขับปกติจากคนคุมรถ / vib_bad 1 = ไม่รับคำสั่งเปิดไฟ / VR1 = ความเร็ว VR4 = แบต
# ลองเล่น: วางนิ่ง 5 วิแรก (รถจอด) แล้วเคาะบอร์ด (หลุมบนถนน) / แก้ก่อนรัน: WIFI_SSID, WIFI_PASS, TEAM

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
RISK_BUMPS = (3, 8)  # กระแทกครบ = เสี่ยงร้าว / เสี่ยงแตก (เท่ากับหน้าเว็บ)
CHART_MAX = 100  # กราฟ: เท่าของถนนปกติ x10
SEND_MS, POLL_MS, LISTEN_MS = 5000, 100, 1800000

SPEAKER = 40
VOLUME = 25
# ธีมไข่: น้ำตาลเข้ม + เหลืองไข่แดง
COL_TEXT, COL_DIM = 0xFFF8E7, 0xB8A88A
COL_CARD = 0x241C14
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x9BC53D, 0xFFC145, 0xFF6B6B, 0xF2C94C
COL_MID = 0xFF8C42


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
        return "deny", 0, "ยังกระแทกอยู่ ให้ชะลอก่อน", COL_BAD
    if act in ("led", "pump") and tank < TANK_MIN:
        return "deny", 0, "แบตเหลือ %d%% ไม่เปิดไฟ" % tank, COL_BAD
    if act in ("led", "pump"):
        sec = pump_seconds(cmd.get("sec", PUMP_DEFAULT_S))
        return "on", sec, "ไฟขับปกติ %d วิ" % sec, COL_OK
    if act == "beep":
        return "beep", 0, "คนคุมรถเรียกคนขับ!", COL_INFO
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
        # กี่เท่าของถนนปกติ
        return 0 if self.base is None else self.vib / self.base


def severity(r):
    # 0 = ไม่กระแทก 1 เบา 2 กลาง 3 แรง
    return min(3, int(r // SEV_STEP))


class Trip:
    # หนึ่งเที่ยว: นับกระแทก (ขาขึ้น) และจำระดับสูงสุดของครั้งล่าสุด
    def __init__(self, now):
        self.t0, self.bumps, self.sev, self.last = now, 0, 0, 0

    def feed(self, s):
        # คืน True เมื่อมีอะไรบนจอต้องวาดใหม่
        before = (self.bumps, self.last)
        if s and not self.sev:
            self.bumps, self.last = self.bumps + 1, s
        elif s:
            self.last = max(self.last, s)
        self.sev = s
        return before != (self.bumps, self.last)

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
    ui.Label("ถาดไข่บนรถ นับแรงกระแทก", x=12, y=6, color=COL_TEXT, value=24)
    w = {}
    card(12, 44, 240, 150, "ไข่โดนกระแทก")
    w["bumps"] = ui.Seg7(text="0", x=28, y=80, w=130, h=56, color=COL_INFO)
    ui.Label("ครั้ง", x=120, y=96, color=COL_DIM, value=16)
    w["risk"] = ui.Label("ไข่ปลอดภัย", x=28, y=154, color=COL_OK, value=20)
    card(262, 44, 250, 150, "เวลาเดินทาง")
    w["trip"] = ui.Seg7(text="00:00", x=278, y=80, w=200, h=56, color=COL_TEXT)
    ui.Label(BTN_NAMES[1] + " = เริ่มเที่ยวใหม่", x=278, y=156, color=COL_DIM, value=14)
    w["title"] = card(522, 44, 258, 150, "ไฟขับปกติ (คำสั่ง 0)")
    w["led"] = ui.Led(x=536, y=78, w=36, h=36, color=COL_INFO)
    w["pump"] = ui.Label("ดับ", x=584, y=82, color=COL_DIM, value=20)
    w["speed"] = ui.Label("ความเร็ว (VR1) -- %", x=536, y=128, color=COL_TEXT, value=16)
    w["tank"] = ui.Label("แบต (VR4) -- %", x=536, y=156, color=COL_TEXT, value=16)
    card(12, 204, 510, 140, "แรงกระแทก (เท่าของถนนปกติ x10)")
    w["chart"] = ui.Chart(x=22, y=232, w=400, h=100, color=COL_INFO, min=0, max=CHART_MAX)
    w["chart"].prop(ui.PROP_CHART_POINTS, 400)
    w["s_lim"] = w["chart"].add_series(COL_BAD)
    w["state"] = ui.Label("จอดนิ่ง\n5 วิ", x=432, y=240, color=COL_WARN, value=16)
    card(532, 204, 248, 140, "ระดับครั้งล่าสุด")
    w["sev"] = []
    for i, c in enumerate((COL_WARN, COL_MID, COL_BAD)):
        w["sev"].append(ui.Led(x=548 + i * 76, y=240, w=30, h=30, color=c, value=0))
        ui.Label(("เบา", "กลาง", "แรง")[i], x=548 + i * 76, y=278, color=COL_DIM, value=14)
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
    w["pump"].text("ขับต่อได้ %d วิ" % sec_left if sec_left else "ดับ")


def show_trip(w, trip):
    w["bumps"].text(str(trip.bumps))
    for i in range(3):
        w["sev"][i].value(1 if i < trip.last else 0)
    risk = 0 if trip.bumps < RISK_BUMPS[0] else (1 if trip.bumps < RISK_BUMPS[1] else 2)
    w["risk"].color((COL_OK, COL_WARN, COL_BAD)[risk])
    w["risk"].text(("ไข่ปลอดภัย", "เสี่ยงร้าว", "เสี่ยงแตก ตรวจถาด")[risk])


def show_vib(w, vib, last):
    if vib.base is None:
        return last
    w["chart"].set_next(0, min(CHART_MAX, int(vib.ratio() * 10)))
    w["chart"].set_next(w["s_lim"], int(SEV_STEP * 10))
    if vib.bad == last:
        return last
    w["state"].color(COL_BAD if vib.bad else COL_OK)
    w["state"].text("กระแทก\nต่อเนื่อง!" if vib.bad else "ถนน\nปกติ")
    return vib.bad


def show_knobs(w, speed, tank):
    w["speed"].text("ความเร็ว (VR1) %d %%" % speed)
    w["tank"].color(COL_TEXT if tank >= TANK_MIN else COL_BAD)
    w["tank"].text("แบต (VR4) %d %%" % tank)


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
    # ดับไฟขับปกติ บอกคนขับชะลอ แจ้งออกเน็ต
    rgbmatrix.scroll("")
    rgbmatrix.clear()
    if vib.bad:
        set_led(pump, False)
        show_pump(w, 0)
        show_note(w, "กระแทกต่อเนื่อง! แจ้งคนขับชะลอ", COL_BAD)
        rgbmatrix.scroll("SLOW", rgbmatrix.RED, 80)
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
    shown, drawn, sec_shown = -1, None, -1
    t_send = t0 = time.ticks_ms()
    vib, trip = Vib(t0), Trip(t0)
    while time.ticks_diff(time.ticks_ms(), t0) < LISTEN_MS:
        now = time.ticks_ms()
        if stop_btn.pressed_now() and pump_ms:                      # SW5 ไม่ผ่านเน็ตเลย
            pump_ms = 0
            show_note(w, "ดับไฟขับปกติ " + BTN_NAMES[0], COL_BAD)
            beep("stop")
        if trip_btn.pressed_now():                                  # SW6 = เที่ยวใหม่
            trip = Trip(now)
            show_trip(w, trip)
            show_note(w, "เริ่มเที่ยวใหม่ นับกระแทกจาก 0", COL_INFO)
            beep("tap")
        m = magnitude()
        if m is not None and vib.push(m, now):
            on_vib(w, vib, pump)
            pump_ms, shown = 0, 0
        bumps = trip.bumps
        if vib.base is not None and trip.feed(severity(vib.ratio())):
            show_trip(w, trip)
            if trip.bumps != bumps:
                beep("hit")
        speed, tank = knob_percent(0), knob_percent(3)
        msg = mqtt.get_message()
        if msg is not None:
            got += 1
            w["title"].text("ไฟขับปกติ (คำสั่ง %d)" % got)
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
        if trip.seconds(now) != sec_shown:
            sec_shown = trip.seconds(now)
            w["trip"].text(mmss(sec_shown))
        show_knobs(w, speed, tank)
        drawn = show_vib(w, vib, drawn)
        if time.ticks_diff(now, t_send) >= SEND_MS:
            t_send, n = now, n + 1
            v = None if vib.vib is None else round(vib.vib, 2)
            if not send(TOPIC, {"id": TEAM, "n": n, "soil": speed, "tank": tank, "pump": 1 if pump_ms else 0,
                                "sim": "soil tank", "vib": v, "vib_bad": int(vib.bad),
                                "bumps": trip.bumps, "trip_s": trip.seconds(now)}):
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
# 1) เคาะเบา ๆ กับเคาะแรง แล้วปรับ SEV_STEP ให้ "แรง" ตรงกับแรงที่ไข่ร้าวจริง
# 2) จบเที่ยว (SW6) ให้ส่ง event "trip" สรุป bumps และ trip_s ไปมือถือ
