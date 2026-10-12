# idea_7_pump_doctor.py - หมอฟังปั๊ม: จับชีพจรปั๊มน้ำ สั่นผิดปกติ = หยุดปั๊ม ไม่รับคำสั่งเปิด แจ้งมือถือ
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# ต่อยอดจาก s2/sf2_03 + กฎการสั่นจาก s3/sf3_03 / จอ: หน้าปัดชีพจร (100 = เส้นเสีย) ชั่วโมงเดินสะสม ค่าสูงสุด
# MQTT: ของ sf2_03 ครบ + telemetry "vib" (null ตอนเริ่ม) "vib_bad" 0/1 + event "vib" ตอนเปลี่ยน
#   + "run_s" วินาทีที่ปั๊มเดินสะสม  "vib_max" ค่าสั่นสูงสุด / vib_bad 1 = ไม่รับคำสั่งเปิด
# ลองเล่น: วางนิ่ง 5 วิแรก แล้วเขย่าบอร์ด / ต้องแก้ก่อนรัน: WIFI_SSID, WIFI_PASS, TEAM
# กับดัก: get_message() ไม่บล็อก กล่องรับมีช่องเดียว ต้องถามทุก 100 ms

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
TANK_MIN = 10  # ถังต่ำกว่านี้ห้ามปั๊ม
LEARN_MS, WIN = 5000, 10
K_BAD, MIN_BASE, CONFIRM_N = 4.0, 0.05, 5  # ค่าสาธิต
WARN_PCT = 60  # หน้าปัดเกินนี้ = เริ่มสั่น (100 = เส้นเสีย)
NEEDLE_LEN = 56
SEND_MS, POLL_MS, LISTEN_MS = 5000, 100, 1800000

SPEAKER = 40
VOLUME = 25
# ธีมห้องตรวจ: น้ำเงินเข้ม + เขียวน้ำทะเล
COL_TEXT, COL_DIM = 0xE6F4F1, 0x8FA9A6
COL_CARD = 0x0F2629
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x2EC4B6, 0xFFB347, 0xFF5964, 0x5BC0EB


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
        return "deny", 0, "ไม่มีปั๊มเครื่องที่ " + str(cmd.get("n"))[:6], COL_WARN
    if act in ("led", "pump") and not on:
        return "off", 0, "สั่งปิดปั๊ม", COL_OK
    if act in ("led", "pump") and bad:
        return "deny", 0, "หมอสั่งพักปั๊ม ยังสั่นผิดปกติ", COL_BAD
    if act in ("led", "pump") and tank < TANK_MIN:
        return "deny", 0, "น้ำเหลือ %d%% ไม่ยอมเปิด" % tank, COL_BAD
    if act in ("led", "pump"):
        sec = pump_seconds(cmd.get("sec", PUMP_DEFAULT_S))
        return "on", sec, "เปิดปั๊ม %d วิ" % sec, COL_OK
    if act == "beep":
        return "beep", 0, "เจ้าของฟาร์มเรียกที่โรงสูบ!", COL_INFO
    if act == "say":
        text = ascii_only(cmd.get("text", ""))
        return "say", text, "ข้อความ: " + (text or "ไม่มีอักษรอังกฤษ"), COL_INFO
    if act == "ack":  # ack เป็นของ sf2_04
        return "", 0, "ไม่มีแจ้งเตือน (sf2_04)", COL_INFO
    if act != "":
        return "deny", 0, "ไม่รู้จัก " + str(act)[:12], COL_WARN
    return "", 0, "", COL_INFO


class Vib:
    # ย่อจาก sf3_03 + จำค่าสูงสุด
    def __init__(self, now):
        self.buf, self.vib, self.base, self.total, self.n = [], None, None, 0.0, 0
        self.t0, self.bad, self.streak, self.peak = now, False, 0, None

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
                self.base, self.peak = max(MIN_BASE, self.total / self.n), self.vib
            return False
        self.peak = self.vib if self.peak is None else max(self.peak, self.vib)
        self.streak = self.streak + 1 if (self.vib > self.base * K_BAD) != self.bad else 0
        if self.streak < CONFIRM_N:
            return False
        self.bad, self.streak = not self.bad, 0
        return True

    def percent(self):
        # 100 = ถึงเส้นเสีย (base x K_BAD)
        if self.base is None or self.vib is None:
            return 0
        return min(100, int(self.vib * 100 / (self.base * K_BAD)))


def hms(sec):
    return "%02d:%02d:%02d" % (sec // 3600, sec // 60 % 60, sec % 60)


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


def set_needle(scale, value):
    # ยาวเข็ม 16 บิตบน ค่า 16 บิตล่าง
    scale.prop(ui.PROP_SCALE_NEEDLE, (NEEDLE_LEN << 16) | (int(value) & 0xFFFF))


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: เชื่อมต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("หมอฟังปั๊ม จับชีพจรปั๊มน้ำ", x=12, y=6, color=COL_TEXT, value=24)
    w = {}
    # ซ้าย: หน้าปัดชีพจร
    card(12, 44, 380, 300, "ชีพจรปั๊ม (100 = เส้นเสีย)")
    w["ring"] = ui.Arc(x=28, y=76, w=200, h=200, min=0, max=100, value=0)
    w["ring"].color(COL_OK)
    gauge = ui.Scale(x=48, y=96, w=160, h=160, color=COL_TEXT, min=0, max=100)
    gauge.prop(ui.PROP_SCALE_MODE, ui.SCALE_ROUND_IN)
    gauge.ticks(11, 2)
    gauge.prop(ui.PROP_SCALE_NEEDLE_COLOR, COL_WARN)
    w["gauge"] = gauge
    w["diag"] = ui.Label("กำลังฟัง\nค่าปกติ 5 วิ", x=240, y=96, color=COL_WARN, value=20)
    w["detail"] = ui.Label("วางบอร์ดนิ่ง ๆ", x=240, y=190, color=COL_DIM, value=14)
    w["peak"] = ui.Label("สูงสุด --", x=28, y=290, color=COL_DIM, value=16)
    # ขวาบน: ปั๊ม
    w["title"] = card(402, 44, 378, 150, "ปั๊มบ่อ = ไฟสีฟ้า (คำสั่ง 0)")
    w["led"] = ui.Led(x=416, y=78, w=40, h=40, color=COL_INFO)
    w["pump"] = ui.Label("ปิด", x=468, y=84, color=COL_DIM, value=24)
    w["left"] = ui.Bar(x=416, y=130, w=350, h=14, min=0, max=PUMP_MAX_S, value=0)
    w["run"] = ui.Label("เดินสะสม 00:00:00", x=416, y=154, color=COL_TEXT, value=16)
    # ขวาล่าง: น้ำ
    card(402, 204, 378, 140, "น้ำในถัง (VR4)  ดินแปลง (VR1)")
    w["tank_bar"] = ui.Bar(x=416, y=240, w=260, h=22)
    w["tank"] = ui.Label("-- %", x=690, y=236, color=COL_TEXT)
    w["soil"] = ui.Label("ดิน -- %", x=416, y=274, color=COL_TEXT, value=16)
    ui.Label("ต่ำกว่า %d%% ไม่เปิด  " % TANK_MIN + BTN_NAMES[0] + " = หยุดฉุกเฉิน", x=416, y=304, color=COL_DIM, value=14)
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
    w["pump"].text("เปิด อีก %d วิ" % sec_left if sec_left else "ปิด")
    w["left"].value(sec_left)


def show_vib(w, vib, last):
    # วาดใหม่เฉพาะเมื่อค่าเปลี่ยน
    if vib.base is None:
        return last
    pct = vib.percent()
    key = (pct, vib.bad)
    if key == last:
        return last
    col = COL_BAD if vib.bad else (COL_WARN if pct >= WARN_PCT else COL_OK)
    w["ring"].value(pct)
    w["ring"].color(col)
    set_needle(w["gauge"], pct)
    w["diag"].color(col)
    w["diag"].text("ผิดปกติ!\nปั๊มหยุด" if vib.bad else ("เริ่มสั่น\nเฝ้าดูไว้" if pct >= WARN_PCT else "ชีพจรปกติ"))
    w["detail"].text("ค่าสั่น %.2f\nปกติ %.2f\nเส้นเสีย %.2f" % (vib.vib, vib.base, vib.base * K_BAD))
    w["peak"].text("สูงสุดตั้งแต่เริ่ม %.2f m/s2" % vib.peak)
    return key


def show_knobs(w, soil, tank):
    w["soil"].text("ดินแปลง %d %%" % soil)
    w["tank_bar"].value(tank)
    w["tank_bar"].color(COL_INFO if tank >= TANK_MIN else COL_BAD)
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
    # ดับปั๊ม แจ้งออกเน็ต
    rgbmatrix.scroll("")
    rgbmatrix.clear()
    if vib.bad:
        set_led(pump, False)
        show_pump(w, 0)
        show_note(w, "สั่นผิดปกติ! หยุดปั๊มแล้ว เรียกช่าง", COL_BAD)
        rgbmatrix.scroll("CHECK PUMP", rgbmatrix.RED, 80)
        beep("empty")
    else:
        show_note(w, "ชีพจรกลับมาปกติ สั่งเปิดได้", COL_OK)
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
    got = n = pump_ms = pump_t0 = run_ms = 0
    shown, drawn, run_shown = -1, None, -1
    t_send = t_loop = t0 = time.ticks_ms()
    vib = Vib(t0)
    while time.ticks_diff(time.ticks_ms(), t0) < LISTEN_MS:
        now = time.ticks_ms()
        if pump_ms:                                                  # นับเวลาเดินสะสม
            run_ms += time.ticks_diff(now, t_loop)
        t_loop = now
        if stop_btn.pressed_now() and pump_ms:                      # SW5 ไม่ผ่านเน็ตเลย
            pump_ms = 0
            show_note(w, "หยุดฉุกเฉิน " + BTN_NAMES[0], COL_BAD)
            beep("stop")
        m = magnitude()
        if m is not None and vib.push(m, now):
            on_vib(w, vib, pump)
            pump_ms, shown = 0, 0
        soil, tank = knob_percent(0), knob_percent(3)
        msg = mqtt.get_message()
        if msg is not None:
            got += 1
            w["title"].text("ปั๊มบ่อ = ไฟสีฟ้า (คำสั่ง %d)" % got)
            pump_ms, pump_t0 = act_on(w, msg[1], tank, now, pump_ms, pump_t0, vib.bad)
        left = pump_ms - time.ticks_diff(now, pump_t0) if pump_ms else 0
        if pump_ms and (left <= 0 or tank < TANK_MIN):
            pump_ms = 0
            show_note(w, "ครบเวลา ดับเอง" if left <= 0 else "ถังแห้ง ดับเอง", COL_INFO)
            beep("stop")
        set_led(pump, pump_ms)
        sec_left = (left + 999) // 1000 if pump_ms else 0
        if sec_left != shown:
            shown = matrix_countdown(sec_left, shown)
            show_pump(w, sec_left)
        if run_ms // 1000 != run_shown:
            run_shown = run_ms // 1000
            w["run"].text("เดินสะสม " + hms(run_shown))
        show_knobs(w, soil, tank)
        drawn = show_vib(w, vib, drawn)
        if time.ticks_diff(now, t_send) >= SEND_MS:
            t_send, n = now, n + 1
            v = None if vib.vib is None else round(vib.vib, 2)
            peak = None if vib.peak is None else round(vib.peak, 2)
            if not send(TOPIC, {"id": TEAM, "n": n, "soil": soil, "tank": tank, "pump": 1 if pump_ms else 0,
                                "sim": "soil tank", "vib": v, "vib_bad": int(vib.bad),
                                "run_s": run_ms // 1000, "vib_max": peak}):
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
# 2) ให้ SW6 (Button(0)) = เรียนรู้ค่าปกติใหม่ (vib = Vib(now))
