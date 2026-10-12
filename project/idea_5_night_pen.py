# idea_5_night_pen.py - คอกสัตว์กลางคืน: เรดาร์จับผู้บุกรุกที่รั้วคอก แจ้งมือถือ (ขั้นสูง)
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# ต่อยอดจาก s3/sf3_01_pen_guard.py + WiFi/MQTT, send(), ack แบบ s2/sf2_04_crop_alert.py
# จอ: นาฬิกากะกลางคืน + เวลาสงบ + บันทึก 3 ครั้งล่าสุดของคืนนี้ · ไซเรนซ้ำทุก BEEP_MS จนรับทราบ
# ทีมต้องแก้ 3 อย่าง: 1) MIN_CM SPAN_CM CONFIRM_N จากการลองเดินเอง 2) หัวจอและป้าย 3) คำบนจอไฟ RGB และเสียง
# MQTT (ใหม่ ไม่มีในสัญญา) ทาง event เฉพาะตอนเกิด · รับ cmd {"cmd":"ack"} อย่างเดียว:
#   {"id":"team05","event":"intruder","count":3,"cm":120,"shift_s":1830,"quiet_s":600}
#   {"id":"team05","event":"clear","count":3}  {"id":"team05","event":"armed","on":0}
#   {"id":"team05","event":"ack","by":"app","count":3} (by app/sw6)
#   คีย์เพิ่ม: shift_s = เฝ้ามากี่วินาทีแล้วคืนนี้ · quiet_s = สงบมากี่วินาทีก่อนครั้งนี้
# เกณฑ์ = ค่าสาธิต ไม่ใช่ระบบรักษาความปลอดภัยจริง
#
# ลองเล่น : ถอยห่างตอนเริ่ม แล้วเดินเข้าหา · VR3 = รั้วคอก · SW5 = เปิด/ปิดเฝ้า · SW6 = รับทราบ
# ต้องแก้ก่อนรัน: WIFI_SSID, WIFI_PASS, TEAM · Emulator: VR1 = ระยะเป้า, Shake = เคลื่อนไหว

import buttons
import json
import mqtt
import pots
import rgbmatrix
import sensors
import time
import ui
import wifi

# ---- 1) ตั้งค่า (แก้ได้) ----
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"   # ตั้งเอง: อังกฤษ/ตัวเลขสั้น ๆ ไม่มีเว้นวรรค
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"   # อย่างน้อย 8 ตัว · อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร
TEAM = "teamXX"
BROKER = "broker.hivemq.com"
CLIENT_ID = "bento-farm-" + TEAM       # + เลขจากนาฬิกาบอร์ดทุกครั้งที่ต่อ: ไม่ชน id เก่า
TOPIC_EVENT = "bento-aiot/" + TEAM + "/event"
TOPIC_CMD = "bento-aiot/" + TEAM + "/cmd"
BEEP_MS = 5000  # ไซเรนซ้ำทุกกี่ ms จนมีคนรับทราบ
MIN_CM, SPAN_CM = 50, 200  # VR3 ตั้งรั้วคอกได้ 50-250 cm
CONFIRM_N = 3  # ต้องเห็นติดกันกี่รอบถึงเชื่อ (1 ขึ้นไป)
THRESH_DB = 4.0  # เป้าต้องแรงกว่าฉากนิ่งกี่ dB (0.1-60)
MAX_CM = 300  # ระยะไกลสุดบนกราฟ (cm)
BTN_NAMES = ("SW5", "SW6")  # SW5 = ล่าง = pressed(1), SW6 = บน = pressed(0)
VOLUME = 25
SPEAKER = 40
RUN_MS = 600000  # เวลาเล่นต่อรอบ ms (10 นาที)
SAMPLE_MS = 250  # อ่านเรดาร์ทุกกี่ ms
TICK_MS = 500  # อัปเดตจอทุกกี่ ms

# สีคืนเดือนหงาย
COL_TEXT, COL_DIM = 0xE9E7FF, 0x8C8FB8
COL_CARD, COL_NIGHT = 0x161A3A, 0x0E1030
COL_MOON, COL_SAFE = 0xF4E3A1, 0x6EE7B7
COL_BAD, COL_WARN = 0xFF4D6D, 0xFFB86B
COL_ALERT_BG = 0x4A1230


# ---- 2) ฮาร์ดแวร์ ----
def beep(*notes):
    for n in notes:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 120)
        time.sleep_ms(120)


def zone_cm():
    return MIN_CM + pots.read(2) * SPAN_CM // 4095  # VR3


def read_radar():
    # เรดาร์ไม่ตอบ = None ข้ามรอบ
    try:
        r, rr = sensors.radar(), sensors.radar_range()
    except OSError:
        return None
    cm = int(rr["distance_m"] * 100) if rr["target"] else None
    return cm, r["presence"], r["energy"]


def calibrate_radar():
    # radar_config(0) = จำฉากนิ่งใหม่ แล้วตั้งเกณฑ์ THRESH_DB
    try:
        sensors.radar_config(0)
        time.sleep_ms(500)
        sensors.radar_config(THRESH_DB)
        return True
    except OSError:
        return False


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
    while True:
        for b in btns:
            b.sample()
        left = ms - time.ticks_diff(time.ticks_ms(), t0)
        if left <= 0:
            return
        time.sleep_ms(min(20, left))


def show_matrix(armed, inside, count):
    # เรียกเฉพาะตอนสถานะเปลี่ยน
    try:
        rgbmatrix.scroll("")
        rgbmatrix.clear()
        if armed and inside:
            rgbmatrix.scroll("PEN ALERT", rgbmatrix.RED, 60)
        elif armed:
            rgbmatrix.score(count, rgbmatrix.CYAN)
    except OSError:
        pass


def siren():
    for note in (81, 74, 81, 74):
        ui.tone(note, ui.WAVE_SQUARE, VOLUME, 140)
        time.sleep_ms(150)


# ---- 3) สมอง (ตัดสินใจ) ----
def smooth(hist, cm):
    # median 5 ค่า กันเรดาร์กระโดด
    hist.append(cm)
    if len(hist) > 5:
        hist.pop(0)
    return sorted(hist)[len(hist) // 2]


def in_zone(armed, cm, alert_cm):
    return armed and cm is not None and cm < alert_cm


def confirm(inside, streak, near):
    # สถานะใหม่ต้องค้าง CONFIRM_N รอบ
    if near == inside:
        return inside, 0
    if streak + 1 >= CONFIRM_N:
        return near, 0
    return inside, streak + 1


def hhmm(sec):
    return "%02d:%02d" % (sec // 3600, sec // 60 % 60)


def mmss(sec):
    return "%d:%02d" % (sec // 60, sec % 60)


# ---- 3b) เครือข่าย ----
def connect_broker(w):
    for n in (1, 2, 3):
        try:
            if mqtt.connect(BROKER, port=1883, keepalive=60,
                            client_id=CLIENT_ID + "-%04x" % (time.ticks_ms() & 0xFFFF)):
                return True
        except OSError:
            pass
    return False


def go_online(w):
    # "" = ต่อแล้ว · ไม่ได้ = เฝ้าออฟไลน์ต่อ
    if len(TEAM) != 6 or TEAM[:4] != "team" or not TEAM[4:].isdigit() or TEAM == "team00":
        return "แก้ TEAM ก่อน"
    say(w, "ต่อ WiFi... จอนิ่งได้", COL_WARN)
    ui.poll()
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "WiFi ไม่ติด"
    if not connect_broker(w) or not mqtt.subscribe(TOPIC_CMD):
        return "broker ไม่ตอบ"
    return ""


def send(w, g, obj):
    # สายหลุด = ออฟไลน์ แต่ยังเฝ้าต่อ
    if not g.online:
        return
    try:
        mqtt.publish(TOPIC_EVENT, json.dumps(obj))
    except OSError:
        g.online = False
        show_link(w, "สายหลุด")


def is_ack(raw):
    try:
        return json.loads(raw.decode()).get("cmd") == "ack"
    except Exception:
        return False


# ---- 4) หน้าจอ ----
def card(x, y, w, h, title):
    box = ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=10, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_MOON, value=16)
    return box


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("เวรยามคอกสัตว์กลางคืน", x=12, y=6, color=COL_MOON, value=24)
    w = {"mq_t": ui.Label("MQTT: ออฟไลน์", x=500, y=10, color=COL_DIM, value=16)}
    w["pen"] = card(12, 44, 520, 190, "รั้วคอก (เรดาร์)")
    w["state"] = ui.Label("สัตว์ปลอดภัย", x=24, y=70, color=COL_SAFE, value=28)
    w["cm"] = ui.Label("ระยะ --- cm", x=330, y=78, color=COL_TEXT, value=16)
    # Chart กว้างไม่เกิน 400 จุด >= ความกว้าง บอร์ดจึงไม่วาดจุดกลม (แบบ s3/sf3_01)
    w["chart"] = ui.Chart(x=24, y=112, w=400, h=112, color=COL_MOON, min=0, max=MAX_CM)
    w["chart"].prop(ui.PROP_CHART_POINTS, 400)
    w["s_zone"] = w["chart"].add_series(COL_BAD)
    ui.Label("เหลือง = ระยะ\nแดง = รั้ว", x=432, y=114, color=COL_DIM, value=14)
    w["led"] = ui.Led(x=434, y=184, w=22, h=22, color=COL_WARN, value=0)
    ui.Label("ขยับ", x=464, y=186, color=COL_DIM, value=14)
    card(544, 44, 236, 190, "เฝ้ามาแล้ว (ชม:นาที)")
    w["shift"] = ui.Seg7(text="00:00", x=560, y=76, w=200, h=60, color=COL_MOON)
    w["quiet"] = ui.Label("สงบมา 0:00", x=560, y=150, color=COL_SAFE, value=16)
    w["n"] = ui.Label("คืนนี้ 0 ครั้ง", x=560, y=182, color=COL_TEXT, value=16)
    # มุมขวาล่างสุดเว้นไว้ให้ปุ่ม Console
    card(12, 244, 400, 96, "บันทึกคืนนี้ (3 ครั้งล่าสุด)")
    w["log"] = ui.Label("ยังเงียบ", x=24, y=272, color=COL_TEXT, value=16)
    card(424, 244, 260, 96, "ระบบเฝ้า")
    w["sw"] = ui.Switch(x=436, y=276, w=64, h=32, value=1)
    w["arm"] = ui.Label(" ", x=510, y=272, color=COL_SAFE, value=14)
    w["zone"] = ui.Label("รั้ว - cm (VR3)", x=510, y=300, color=COL_DIM, value=14)
    w["status"] = ui.Label(" ", x=12, y=352, color=COL_WARN, value=16)
    show_arm(w, True)
    ui.poll()
    return w


def show_link(w, why):
    w["mq_t"].color(COL_DIM if why else COL_SAFE)
    w["mq_t"].text("MQTT: ออฟไลน์ (" + why + ")" if why else "MQTT: เชื่อมต่อแล้ว")


def say(w, text, col):
    w["status"].color(col)
    w["status"].text(text)


def show_arm(w, armed):
    w["arm"].text("เปิดเฝ้า (" + BTN_NAMES[0] + ")" if armed else "ปิดเฝ้า (" + BTN_NAMES[0] + ")")
    w["arm"].color(COL_SAFE if armed else COL_DIM)


def show_pen(w, g, cm, alert_cm, now, moving):
    w["pen"].color(COL_ALERT_BG if g.inside else COL_CARD)
    w["led"].value(1 if moving else 0)
    w["state"].color(COL_BAD if g.inside else (COL_SAFE if g.armed else COL_DIM))
    w["state"].text("มีผู้บุกรุก!" if g.inside else ("สัตว์ปลอดภัย" if g.armed else "ปิดระบบเฝ้า"))
    w["cm"].text("ระยะ --- cm" if cm is None else "ระยะ %d cm" % cm)
    w["chart"].set_next(0, MAX_CM if cm is None else min(cm, MAX_CM))
    w["chart"].set_next(w["s_zone"], alert_cm)
    w["zone"].text("รั้ว %d cm (VR3)" % alert_cm)
    w["shift"].text(hhmm(g.shift_s(now)))
    w["quiet"].text("กำลังเตือน" if g.inside else "สงบมา " + mmss(g.quiet_s(now)))


# ---- 5) โปรแกรมหลัก ----
class Guard:
    def __init__(self):
        self.armed, self.inside, self.streak, self.count = True, False, 0, 0
        self.hist, self.log = [], []
        self.radar_ok = True
        self.acked, self.online, self.t_beep = True, False, 0
        self.shown = None
        self.t_start = self.t_quiet = time.ticks_ms()

    def shift_s(self, now):
        return time.ticks_diff(now, self.t_start) // 1000

    def quiet_s(self, now):
        return time.ticks_diff(now, self.t_quiet) // 1000


def start_guard(w, g):
    for i in (3, 2, 1):
        say(w, "ถอยห่างบอร์ด! จำฉากนิ่งใน %d วินาที" % i, COL_WARN)
        time.sleep_ms(1000)
    if calibrate_radar():
        say(w, "เฝ้าคอกแล้ว เดินเข้าหาบอร์ดได้เลย", COL_SAFE)
    else:
        say(w, "จำฉากไม่สำเร็จ ใช้ฉากเดิม", COL_BAD)
    g.t_start = g.t_quiet = time.ticks_ms()
    beep(72, 79)


def on_arm(w, g, sw5):
    # ui.poll() อยู่ที่นี่ที่เดียว
    armed = g.armed
    for ev in ui.poll():
        if ev["handle"] == w["sw"].id() and ev["type"] == "toggled":
            armed = ev["value"] == 1
    if sw5.pressed_now():
        armed = not armed
        w["sw"].value(1 if armed else 0)
    if armed != g.armed:
        g.armed, g.inside, g.streak, g.acked = armed, False, 0, True
        show_arm(w, armed)
        beep(76)
        send(w, g, {"id": TEAM, "event": "armed", "on": 1 if armed else 0})


def on_ack(w, g, by):
    # หยุดไซเรนซ้ำ (จอยังแดง)
    if g.inside and not g.acked:
        g.acked = True
        say(w, "รับทราบแล้ว (" + by + ")", COL_SAFE)
        beep(76)
        send(w, g, {"id": TEAM, "event": "ack", "by": by, "count": g.count})


def on_radar(w, g, ok):
    if ok != g.radar_ok:
        g.radar_ok = ok
        say(w, "เรดาร์กลับมาแล้ว" if ok else "เรดาร์ไม่ตอบ กำลังลองใหม่",
            COL_SAFE if ok else COL_BAD)


def decide(w, g, cm, alert_cm):
    # เฉพาะตอนสถานะเปลี่ยน
    was = g.inside
    g.inside, g.streak = confirm(g.inside, g.streak, in_zone(g.armed, cm, alert_cm))
    now = time.ticks_ms()
    if g.inside and not was:
        g.count += 1
        quiet, shift = g.quiet_s(now), g.shift_s(now)
        g.log = (g.log + ["%s น. %d cm" % (hhmm(shift), cm)])[-3:]
        w["log"].text("  ".join(g.log))
        w["n"].text("คืนนี้ %d ครั้ง" % g.count)
        g.acked, g.t_beep = False, now
        send(w, g, {"id": TEAM, "event": "intruder", "count": g.count, "cm": cm,
                    "shift_s": shift, "quiet_s": quiet})
        siren()
    elif was and not g.inside:
        g.acked, g.t_quiet = True, now
        send(w, g, {"id": TEAM, "event": "clear", "count": g.count})
        beep(79, 84)


def matrix_tick(g):
    look = (g.armed, g.inside, g.count)
    if look != g.shown:
        show_matrix(g.armed, g.inside, g.count)
        g.shown = look


def finish(w, g):
    show_matrix(False, False, g.count)
    say(w, "จบรอบแล้ว - กด Program to Device เพื่อเล่นใหม่", COL_WARN)
    ui.poll()
    print("เวรคืนนี้จบ พบผู้บุกรุก", g.count, "ครั้ง")
    try:
        mqtt.disconnect()
    except Exception:
        pass


def main():
    if hasattr(ui, "volume"):
        ui.volume(SPEAKER)
    w = build_screen()
    g, sw5, sw6 = Guard(), Button(1), Button(0)
    why = go_online(w)
    g.online = not why
    show_link(w, why)
    start_guard(w, g)
    every = TICK_MS // SAMPLE_MS
    tick, t0 = 0, time.ticks_ms()
    try:
        while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
            on_arm(w, g, sw5)  # 0) ปุ่ม + สวิตช์
            if g.online and not mqtt.is_connected():
                g.online = False
                show_link(w, "สายหลุด")
            msg = mqtt.get_message() if g.online else None
            if msg and is_ack(msg[1]):
                on_ack(w, g, "app")
            if sw6.pressed_now():
                on_ack(w, g, "sw6")
            got = read_radar()  # 1) อ่าน
            on_radar(w, g, got is not None)
            if got is not None:
                raw = got[0]
                cm = None if raw is None else smooth(g.hist, raw)
                alert_cm = zone_cm()
                decide(w, g, cm, alert_cm)  # 2) ตัดสิน + 3) ทำ
                if tick % every == 0:  # 4) โชว์
                    show_pen(w, g, cm, alert_cm, time.ticks_ms(), got[1])
            if g.inside and not g.acked and time.ticks_diff(time.ticks_ms(), g.t_beep) >= BEEP_MS:
                g.t_beep = time.ticks_ms()
                siren()  # ร้องซ้ำจนมีคนรับทราบ
            matrix_tick(g)
            tick += 1
            wait_ms(SAMPLE_MS, (sw5, sw6))
    finally:
        finish(w, g)


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) CONFIRM_N = 1 เดินผ่านเร็ว ๆ เทียบกับ 3: แบบไหนเตือนผิดน้อย
# 2) นับเฉพาะตอนมีการเคลื่อนไหว (got[1] = moving เข้า in_zone())
# 3) รับ {"cmd":"arm","on":0} จากแอปให้เปิด/ปิดระบบเฝ้าได้
