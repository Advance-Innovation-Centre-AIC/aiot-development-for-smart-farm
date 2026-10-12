# idea_25_warehouse_door.py - ประตูโกดังกันขโมย: เรดาร์จับคนเข้าใกล้ประตูตอนปิดโกดัง แจ้งมือถือ (ขั้นสูง)
# แม่แบบ D = idea_5_night_pen.py · สร้างจาก project/stories.json ด้วย tools/gen_stories.py (คัดลอกไปแก้ ไม่แก้ไฟล์นี้ตรง ๆ)
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# ต่อยอดจาก s3/sf3_01_pen_guard.py + WiFi/MQTT, send(), ack แบบ s2/sf2_04_crop_alert.py
# เปลี่ยน: ไซเรนซ้ำทุก BEEP_MS จนรับทราบ, SW6 = รับทราบ, เน็ตหลุด = เฝ้าต่อแบบออฟไลน์
# ทีมต้องแก้ 3 อย่าง: 1) MIN_CM SPAN_CM CONFIRM_N จากการลองเดินเอง 2) หัวจอและป้าย 3) คำบนจอไฟ RGB และเสียง
# MQTT (ใหม่ ไม่มีในสัญญา) ทาง event เฉพาะตอนเกิด · รับ cmd {"cmd":"ack"} อย่างเดียว:
#   {"id":"team05","event":"intruder","count":3,"cm":120}  {"id":"team05","event":"clear","count":3}
#   {"id":"team05","event":"ack","by":"app","count":3} (by app/sw6)  {"id":"team05","event":"armed","on":0}
# เกณฑ์ = ค่าสาธิต ไม่ใช่ระบบรักษาความปลอดภัยจริง
#
# ลองเล่น : ถอยห่างตอนเริ่ม แล้วเดินเข้าหา · VR3 = เขตเตือน · SW5 = เปิด/ปิดเฝ้า · SW6 = รับทราบ
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
MIN_CM, SPAN_CM = 50, 200  # VR3 ตั้งเขตเตือนได้ 50-250 cm
CONFIRM_N = 3  # ต้องเห็นติดกันกี่รอบถึงเชื่อ (1 ขึ้นไป)
THRESH_DB = 4.0  # เป้าต้องแรงกว่าฉากนิ่งกี่ dB (0.1-60)
MAX_CM = 300  # ระยะไกลสุดบนแถบ ไม้บรรทัด กราฟ (cm)
BTN_NAMES = ("SW5", "SW6")  # SW5 = ล่าง = pressed(1), SW6 = บน = pressed(0)
VOLUME = 25  # ความดังเสียง 0-127
SPEAKER = 40  # ลำโพงรวม 0-100%
RUN_MS = 600000  # เวลาเล่นต่อรอบ ms (10 นาที)
SAMPLE_MS = 250  # อ่านเรดาร์ทุกกี่ ms
TICK_MS = 500  # อัปเดตจอทุกกี่ ms
MATRIX_MS = 3000  # ส่งภาพจอไฟ RGB ซ้ำทุกกี่ ms

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF  # สี 0xRRGGBB
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF
COL_SAFE_BG, COL_ALERT_BG = 0x123322, 0x4A1216


# ---- 2) ฮาร์ดแวร์ ----
def beep(*notes):
    # เบากว่า ui.sfx
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
    # 1 = SW5 (ล่าง), 0 = SW6 (บน)

    def __init__(self, index):
        self.index = index
        self.down = False
        self.clicked = False

    def sample(self):
        now_down = buttons.pressed(self.index)
        if now_down and not self.down:
            self.clicked = True
        self.down = now_down

    def pressed_now(self):
        fired = self.clicked
        self.clicked = False
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
    try:
        if not armed:
            rgbmatrix.clear()
        elif inside:
            rgbmatrix.scroll("THIEF", rgbmatrix.RED, 60)
        else:
            rgbmatrix.score(count, rgbmatrix.GREEN)
    except OSError:
        pass


def siren():
    # เลขโน้ต MIDI
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
    # สถานะใหม่ต้องค้าง CONFIRM_N รอบ คืน (สถานะ, รอบที่เห็นติดกัน)
    if near == inside:
        return inside, 0
    if streak + 1 >= CONFIRM_N:
        return near, 0
    return inside, streak + 1


# ---- 3b) เครือข่าย ----
def connect_broker(w):
    # ลอง 3 ครั้ง client_id ใหม่ทุกครั้ง
    for n in (1, 2, 3):
        try:
            if mqtt.connect(BROKER, port=1883, keepalive=60,
                            client_id=CLIENT_ID + "-%04x" % (time.ticks_ms() & 0xFFFF)):
                return True
        except OSError:
            pass
    return False


def go_online(w):
    # คืน "" = ต่อแล้ว · ต่อไม่ได้ = เฝ้าแบบออฟไลน์ต่อ
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
    box = ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)
    return box


def line_chart(x, y, w, h, lo, hi, color, parent=None):
    ch = ui.Chart(x=x, y=y, w=min(w, 400), h=h, color=color, min=lo, max=hi, parent=parent)
    ch.prop(ui.PROP_CHART_POINTS, 400)
    return ch


def build_gate(w):
    w["gate"] = card(12, 44, 470, 196, "ประตูโกดัง (เรดาร์)")
    w["state"] = ui.Label("โกดังปลอดภัย", x=24, y=72, color=COL_OK, value=28)
    w["cm"] = ui.Seg7(text="---", x=24, y=112, w=150, h=56, color=COL_INFO)
    ui.Label("cm", x=182, y=140, color=COL_DIM, value=16)
    w["led"] = ui.Led(x=300, y=116, w=28, h=28, color=COL_WARN, value=0)
    ui.Label("การเคลื่อนไหว", x=338, y=118, color=COL_DIM, value=16)
    w["bar"] = ui.Bar(x=24, y=176, w=446, h=20, min=0, max=MAX_CM, value=0)
    ruler = ui.Scale(x=24, y=198, w=446, h=34, color=COL_DIM, min=0, max=MAX_CM)
    ruler.ticks(13, 2)


def build_counter(w):
    card(492, 44, 288, 196, "คนเข้าใกล้ประตู (ครั้ง)")
    w["n"] = ui.Seg7(text="0", x=504, y=72, w=150, h=60, color=COL_BAD)
    w["sw"] = ui.Switch(x=504, y=146, w=64, h=32, value=1)
    w["arm"] = ui.Label(" ", x=578, y=150, color=COL_OK, value=16)
    w["zone"] = ui.Label("เขตเตือน - cm (VR3)", x=504, y=196, color=COL_INFO, value=16)
    show_arm(w, True)


def build_log(w):
    ui.Label("ฟ้า = ระยะเป้า   แดง = เขตเตือน", x=12, y=244, color=COL_DIM, value=14)
    w["chart"] = line_chart(12, 264, 400, 74, 0, MAX_CM, COL_INFO)
    w["s_zone"] = w["chart"].add_series(COL_BAD)
    card(422, 250, 358, 88, "บันทึกเรดาร์")
    w["last"] = ui.Label("ยังไม่มี", x=434, y=280, color=COL_TEXT, value=16)
    w["energy"] = ui.Label("energy: -", x=434, y=308, color=COL_DIM, value=14)


def show_link(w, why):
    w["mq_t"].color(COL_DIM if why else COL_OK)
    w["mq_t"].text("MQTT: ออฟไลน์ (" + why + ")" if why else "MQTT: เชื่อมต่อแล้ว")


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("ประตูโกดังกันขโมย", x=12, y=6, color=COL_TEXT, value=24)
    w = {"mq_t": ui.Label("MQTT: ออฟไลน์", x=500, y=10, color=COL_DIM, value=16)}
    build_gate(w)
    build_counter(w)
    build_log(w)
    w["status"] = ui.Label(" ", x=12, y=352, color=COL_WARN, value=16)
    ui.poll()
    return w


def say(w, text, col):
    w["status"].color(col)
    w["status"].text(text)


def show_arm(w, armed):
    w["arm"].text("ระบบเฝ้า: " + ("เปิด" if armed else "ปิด") + " (" + BTN_NAMES[0] + ")")
    w["arm"].color(COL_OK if armed else COL_DIM)


def show_gate(w, g, near, cm, moving):
    w["gate"].color(COL_ALERT_BG if g.inside else COL_SAFE_BG)
    w["state"].color(COL_BAD if g.inside else (COL_OK if g.armed else COL_DIM))
    w["state"].text("มีผู้บุกรุก!" if g.inside else ("โกดังปลอดภัย" if g.armed else "ปิดระบบเฝ้า"))
    w["cm"].text("---" if cm is None else str(cm))
    w["bar"].value(MAX_CM if cm is None else min(cm, MAX_CM))
    w["bar"].color(COL_BAD if near else COL_OK)
    w["led"].value(1 if moving else 0)


def show_zone(w, alert_cm, cm, energy):
    w["zone"].text("เขตเตือน %d cm (VR3)" % alert_cm)
    w["energy"].text("energy: %.0f" % energy)
    w["chart"].set_next(0, MAX_CM if cm is None else min(cm, MAX_CM))
    w["chart"].set_next(w["s_zone"], alert_cm)


# ---- 5) โปรแกรมหลัก ----
class Guard:
    def __init__(self):
        self.armed, self.inside, self.streak, self.count = True, False, 0, 0
        self.hist = []
        self.radar_ok = True
        self.acked, self.online, self.t_beep = True, False, 0
        self.shown, self.sent = None, 0


def start_guard(w):
    for i in (3, 2, 1):
        say(w, "ถอยห่างบอร์ด! จำฉากนิ่งใน %d วินาที" % i, COL_WARN)
        time.sleep_ms(1000)
    if calibrate_radar():
        say(w, "เฝ้าประตูแล้ว เดินเข้าหาบอร์ดได้เลย", COL_OK)
    else:
        say(w, "จำฉากไม่สำเร็จ ใช้ฉากเดิม", COL_BAD)
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
        say(w, "รับทราบแล้ว (" + by + ")", COL_OK)
        beep(76)
        send(w, g, {"id": TEAM, "event": "ack", "by": by, "count": g.count})


def on_radar(w, g, ok):
    if ok != g.radar_ok:
        g.radar_ok = ok
        say(w, "เรดาร์กลับมาแล้ว" if ok else "เรดาร์ไม่ตอบ กำลังลองใหม่",
            COL_OK if ok else COL_BAD)


def decide(w, g, cm, alert_cm):
    # เฉพาะตอนสถานะเปลี่ยน
    was = g.inside
    near = in_zone(g.armed, cm, alert_cm)
    g.inside, g.streak = confirm(g.inside, g.streak, near)
    if g.inside and not was:
        g.count += 1
        w["n"].text(str(g.count))
        msg = "คนที่ %d  ระยะ %d cm" % (g.count, cm)
        w["last"].text(msg)
        print("ผู้บุกรุก" + msg)
        g.acked, g.t_beep = False, time.ticks_ms()
        send(w, g, {"id": TEAM, "event": "intruder", "count": g.count, "cm": cm})
        siren()
    elif was and not g.inside:
        g.acked = True
        send(w, g, {"id": TEAM, "event": "clear", "count": g.count})
        beep(79, 84)  # ออกไปแล้ว
    return near


def matrix_tick(g):
    look = (g.armed, g.inside, g.count)
    now = time.ticks_ms()
    if look != g.shown or time.ticks_diff(now, g.sent) >= MATRIX_MS:
        show_matrix(g.armed, g.inside, g.count)
        g.shown, g.sent = look, now


def finish(w, count):
    show_matrix(False, False, count)
    say(w, "จบรอบแล้ว - กด Program to Device เพื่อเล่นใหม่", COL_WARN)
    ui.poll()
    print("เฝ้าโกดังครบเวลา พบคนเข้าใกล้ประตู", count, "ครั้ง")
    try:
        mqtt.disconnect()
    except Exception:
        pass


def main():
    if hasattr(ui, "volume"):  # fw 2.4.1 ข้าม
        ui.volume(SPEAKER)
    w = build_screen()
    g, sw5, sw6 = Guard(), Button(1), Button(0)
    why = go_online(w)
    g.online = not why
    show_link(w, why)
    start_guard(w)
    every = TICK_MS // SAMPLE_MS
    tick, t0 = 0, time.ticks_ms()
    try:
        while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
            on_arm(w, g, sw5)  # 0) ปุ่ม + คำสั่งจากแอป
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
                raw, moving, energy = got
                cm = None if raw is None else smooth(g.hist, raw)
                alert_cm = zone_cm()
                near = decide(w, g, cm, alert_cm)  # 2) ตัดสิน + 3) ทำ
                if tick % every == 0:  # 4) โชว์
                    show_gate(w, g, near, cm, moving)
                    show_zone(w, alert_cm, cm, energy)
            if g.inside and not g.acked and time.ticks_diff(time.ticks_ms(), g.t_beep) >= BEEP_MS:
                g.t_beep = time.ticks_ms()
                siren()  # ร้องซ้ำจนมีคนรับทราบ
            matrix_tick(g)
            tick += 1
            wait_ms(SAMPLE_MS, (sw5, sw6))  # รอ + ฟังปุ่ม
    finally:
        finish(w, g.count)


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) CONFIRM_N = 1 เดินผ่านเร็ว ๆ เทียบกับ 3: แบบไหนเตือนผิดน้อย
# 2) เปิดเฝ้าเฉพาะกลางคืน: ปิดเฝ้าเองตอนกลางวัน (นับเวลาจาก time.ticks_ms ในการสาธิต)
# 3) รับ {"cmd":"arm","on":0} จากแอปให้เปิด/ปิดระบบเฝ้าได้ ตอนคนงานเข้าโกดัง
