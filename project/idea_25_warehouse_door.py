# idea_25_warehouse_door.py - ประตูโกดังกันขโมย: เรดาร์จับคนที่ประตู แจ้งมือถือ (ขั้นสูง)
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# ต่อยอดจาก s3/sf3_01_pen_guard.py + WiFi/MQTT, send(), ack แบบ s2/sf2_04_crop_alert.py
# จอ: บานประตู + สองเขต (หน้าโกดัง/ประตู) + นับถอยหลังก่อนเฝ้า
# ทีมต้องแก้ 3 อย่าง: 1) MIN_CM SPAN_CM CONFIRM_N EXIT_S 2) หัวจอและป้าย 3) คำบนจอไฟ RGB และเสียง
# MQTT (ใหม่ ไม่มีในสัญญา) ทาง event เฉพาะตอนเกิด · รับ cmd {"cmd":"ack"} อย่างเดียว:
#   ทุกข้อความมี "id":"team05","event":ชื่อ  intruder {"count":3,"cm":60,"loiter":5}  clear {"count":3,"loiter":5}
#   armed {"on":1,"delay_s":15}  ack {"by":"app","count":3} (by app/sw6)
#   คีย์เพิ่ม: loiter = เข้าเขตหน้าโกดังกี่ครั้ง · delay_s = รอกี่วิก่อนเฝ้า
# เกณฑ์ = ค่าสาธิต ไม่ใช่ระบบรักษาความปลอดภัยจริง
#
# ลองเล่น : ถอยห่าง รอนับถอยหลัง แล้วเดินเข้าหา · VR3 = เขตประตู · SW5 เฝ้า · SW6 รับทราบ
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
CLIENT_ID = "bento-farm-" + TEAM
TOPIC_EVENT = "bento-aiot/" + TEAM + "/event"
TOPIC_CMD = "bento-aiot/" + TEAM + "/cmd"
BEEP_MS = 5000  # ไซเรนซ้ำทุกกี่ ms จนมีคนรับทราบ
MIN_CM, SPAN_CM = 30, 120  # VR3 เขตประตู 30-150 cm · หน้าโกดัง = 2 เท่า
CONFIRM_N = 3  # ต้องเห็นติดกันกี่รอบถึงเชื่อ (1 ขึ้นไป)
THRESH_DB = 4.0  # dB เหนือฉากนิ่ง (0.1-60)
MAX_CM = 300  # ระยะไกลสุดบนแถบ (cm)
EXIT_S = 15  # รอกี่วิให้คนงานออกก่อนเฝ้า
BTN_NAMES = ("SW5", "SW6")  # pressed(1), pressed(0)
VOLUME = 25
SPEAKER = 40
RUN_MS = 600000  # 10 นาทีต่อรอบ
SAMPLE_MS = 250  # อ่านเรดาร์ทุกกี่ ms
TICK_MS = 500  # อัปเดตจอทุกกี่ ms

# สีโรงงาน
COL_TEXT, COL_DIM = 0xF2F2F2, 0x9AA0A8
COL_CARD, COL_STEEL = 0x1C2026, 0x3A4048
COL_HAZ, COL_OK = 0xFFC300, 0x34C759
COL_BAD, COL_INFO = 0xFF3B30, 0x5AC8FA


# ---- 2) ฮาร์ดแวร์ ----
def beep(*notes):
    for n in notes:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 100)
        time.sleep_ms(110)


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
    # 0 = จำฉากนิ่งใหม่ แล้วตั้งเกณฑ์
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


def show_matrix(look):
    # เรียกเฉพาะตอนเปลี่ยน
    try:
        rgbmatrix.scroll("")
        rgbmatrix.clear()
        if look[0] == "thief":
            rgbmatrix.scroll("THIEF", rgbmatrix.RED, 60)
        elif look[0] == "wait":
            rgbmatrix.scroll("EXIT NOW", rgbmatrix.YELLOW, 60)
        elif look[0] == "watch":
            rgbmatrix.score(look[1], rgbmatrix.GREEN)
    except OSError:
        pass


def siren():
    for note in (88, 76, 88, 76, 88):
        ui.tone(note, ui.WAVE_SQUARE, VOLUME, 110)
        time.sleep_ms(120)


# ---- 3) สมอง (ตัดสินใจ) ----
def smooth(hist, cm):
    # median 5 ค่า
    hist.append(cm)
    if len(hist) > 5:
        hist.pop(0)
    return sorted(hist)[len(hist) // 2]


def in_zone(watching, cm, alert_cm):
    return watching and cm is not None and cm < alert_cm


def confirm(inside, streak, near):
    # สถานะใหม่ต้องค้าง CONFIRM_N รอบ
    if near == inside:
        return inside, 0
    if streak + 1 >= CONFIRM_N:
        return near, 0
    return inside, streak + 1


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
    say(w, "ต่อ WiFi... จอนิ่งได้", COL_HAZ)
    ui.poll()
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "WiFi ไม่ติด"
    if not connect_broker(w) or not mqtt.subscribe(TOPIC_CMD):
        return "broker ไม่ตอบ"
    return ""


def send(w, g, obj):
    # สายหลุด = ยังเฝ้าต่อ
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
def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("ประตูโกดังกันขโมย", x=12, y=6, color=COL_HAZ, value=24)
    w = {"mq_t": ui.Label("MQTT: ออฟไลน์", x=500, y=10, color=COL_DIM, value=16)}
    # บานประตู เปลี่ยนสีตามเขต
    w["door"] = ui.Panel(x=12, y=44, w=170, h=290, color=COL_STEEL, min=COL_HAZ, max=6, value=3)
    w["door_t"] = ui.Label("ปิดล็อก", x=28, y=70, color=COL_TEXT, value=28)
    w["cm"] = ui.Label("--- cm", x=28, y=290, color=COL_TEXT, value=16)
    ui.Panel(x=194, y=44, w=338, h=190, color=COL_CARD, min=COL_STEEL, max=4, value=1)
    ui.Label("ระยะจากประตู (เรดาร์)", x=206, y=50, color=COL_HAZ, value=16)
    w["bar"] = ui.Bar(x=206, y=84, w=314, h=24, min=0, max=MAX_CM, value=0)
    ruler = ui.Scale(x=206, y=110, w=314, h=34, color=COL_DIM, min=0, max=MAX_CM)
    ruler.ticks(7, 2)
    w["zones"] = ui.Label("ประตู < - cm  หน้าโกดัง < - cm", x=206, y=154, color=COL_INFO, value=14)
    w["state"] = ui.Label("รอเริ่มเฝ้า", x=206, y=184, color=COL_DIM, value=24)
    # มุมขวาล่างสุดเว้นไว้ให้ปุ่ม Console
    ui.Panel(x=544, y=44, w=236, h=190, color=COL_CARD, min=COL_STEEL, max=4, value=1)
    w["big_t"] = ui.Label("ออกจากโกดังใน (วิ)", x=556, y=50, color=COL_HAZ, value=16)
    w["big"] = ui.Seg7(text="--", x=556, y=80, w=150, h=64, color=COL_HAZ)
    w["sw"] = ui.Switch(x=556, y=166, w=64, h=32, value=1)
    w["arm"] = ui.Label(" ", x=630, y=172, color=COL_OK, value=14)
    ui.Panel(x=194, y=244, w=490, h=90, color=COL_CARD, min=COL_STEEL, max=4, value=1)
    w["count"] = ui.Label("เดินผ่านหน้าโกดัง 0  ถึงประตู 0 ครั้ง", x=206, y=256, color=COL_TEXT, value=16)
    w["last"] = ui.Label("ยังไม่มีใครถึงประตู", x=206, y=290, color=COL_DIM, value=16)
    w["status"] = ui.Label(" ", x=12, y=352, color=COL_HAZ, value=16)
    show_arm(w, True)
    ui.poll()
    return w


def show_link(w, why):
    w["mq_t"].color(COL_DIM if why else COL_OK)
    w["mq_t"].text("MQTT: ออฟไลน์ (" + why + ")" if why else "MQTT: เชื่อมต่อแล้ว")


def say(w, text, col):
    w["status"].color(col)
    w["status"].text(text)


def show_arm(w, armed):
    w["arm"].text(("เฝ้าอยู่" if armed else "ปิดเฝ้า") + " (" + BTN_NAMES[0] + ")")
    w["arm"].color(COL_OK if armed else COL_DIM)


# โหมด -> (สีบาน, คำบนบาน, สีป้าย, ป้าย)
LOOK = {"thief": (COL_BAD, "มีคน!", COL_BAD, "มีคนที่ประตู!"),
        "near": (COL_HAZ, "มีคนเดิน", COL_HAZ, "มีคนหน้าโกดัง"),
        "wait": (COL_STEEL, "รอปิด", COL_HAZ, "ยังไม่เฝ้า: รอคนออก"),
        "watch": (COL_STEEL, "ปิดล็อก", COL_OK, "เฝ้าอยู่ ปลอดภัย"),
        "off": (COL_CARD, "เปิดทำงาน", COL_DIM, "ปิดระบบเฝ้า")}


def show_door(w, g, cm, door, now):
    left, mode = g.exit_left(now), g.mode(now)
    door_col, door_t, col, text = LOOK[mode]
    w["door"].color(door_col)
    w["door_t"].text(door_t)
    w["cm"].text("--- cm" if cm is None else "%d cm" % cm)
    w["bar"].value(MAX_CM if cm is None else min(cm, MAX_CM))
    pass  # Bar.color() ตั้งสีส่วนที่เติมบนบอร์ดจริง ใช้สีเดิมของ Bar ให้เห็นชัด
    w["zones"].text("ประตู < %d cm  หน้าโกดัง < %d cm" % (door, door * 2))
    w["state"].color(col)
    w["state"].text(text)
    w["big_t"].text("ออกจากโกดังใน (วิ)" if left else "ถึงประตูคืนนี้ (ครั้ง)")
    w["big"].text(str(left if left else g.count))


# ---- 5) โปรแกรมหลัก ----
class Guard:
    def __init__(self):
        self.armed, self.inside, self.streak, self.count = True, False, 0, 0
        self.near, self.nstreak, self.loiter = False, 0, 0
        self.hist = []
        self.radar_ok = True
        self.acked, self.online, self.t_beep = True, False, 0
        self.shown = None
        self.t_arm = time.ticks_ms()

    def exit_left(self, now):
        # 0 = เฝ้าแล้ว/ปิดเฝ้า · ครบแล้วล็อกไว้ (None) จนกดเปิดเฝ้าใหม่
        if not self.armed or self.t_arm is None:
            return 0
        left = max(0, EXIT_S - time.ticks_diff(now, self.t_arm) // 1000)
        if not left:
            self.t_arm = None
        return left

    def mode(self, now):
        if not self.armed:
            return "off"
        if self.inside:
            return "thief"
        if self.exit_left(now):
            return "wait"
        return "near" if self.near else "watch"


def start_guard(w, g):
    for i in (3, 2, 1):
        say(w, "ถอยห่างบอร์ด! จำฉากนิ่งใน %d วินาที" % i, COL_HAZ)
        time.sleep_ms(1000)
    if calibrate_radar():
        say(w, "จำฉากแล้ว ออกจากโกดังภายใน %d วินาที" % EXIT_S, COL_OK)
    else:
        say(w, "จำฉากไม่สำเร็จ ใช้ฉากเดิม", COL_BAD)
    g.t_arm = time.ticks_ms()
    beep(72, 79)
    send(w, g, {"id": TEAM, "event": "armed", "on": 1, "delay_s": EXIT_S})


def on_arm(w, g, sw5):
    # ui.poll() ที่นี่ที่เดียว
    armed = g.armed
    for ev in ui.poll():
        if ev["handle"] == w["sw"].id() and ev["type"] == "toggled":
            armed = ev["value"] == 1
    if sw5.pressed_now():
        armed = not armed
        w["sw"].value(1 if armed else 0)
    if armed != g.armed:
        g.armed, g.inside, g.streak, g.acked = armed, False, 0, True
        g.near, g.nstreak, g.t_arm = False, 0, time.ticks_ms()
        show_arm(w, armed)
        beep(76)
        send(w, g, {"id": TEAM, "event": "armed", "on": 1 if armed else 0,
                    "delay_s": EXIT_S if armed else 0})


def on_ack(w, g, by):
    # หยุดไซเรนซ้ำ
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


def decide(w, g, cm, door, now):
    # หน้าโกดัง = นับ · ประตู = เตือน + ไซเรน
    on = g.armed and not g.exit_left(now)
    was_near = g.near
    g.near, g.nstreak = confirm(g.near, g.nstreak, in_zone(on, cm, door * 2))
    if g.near and not was_near:
        g.loiter += 1
        beep(84)
    was = g.inside
    g.inside, g.streak = confirm(g.inside, g.streak, in_zone(on, cm, door))
    if g.inside and not was:
        g.count += 1
        w["last"].text("ครั้งที่ %d ระยะ %d cm" % (g.count, cm))
        g.acked, g.t_beep = False, now
        send(w, g, {"id": TEAM, "event": "intruder", "count": g.count, "cm": cm, "loiter": g.loiter})
        siren()
    elif was and not g.inside:
        g.acked = True
        send(w, g, {"id": TEAM, "event": "clear", "count": g.count, "loiter": g.loiter})
        beep(79, 84)
    if g.inside != was or g.near != was_near:
        w["count"].text("เดินผ่านหน้าโกดัง %d  ถึงประตู %d ครั้ง" % (g.loiter, g.count))


def matrix_tick(g, now):
    m = g.mode(now)
    look = (m, g.count if m == "watch" else 0)
    if look != g.shown:
        show_matrix(look)
        g.shown = look


def finish(w, g):
    show_matrix(("off", 0))
    say(w, "จบรอบแล้ว - กด Program to Device เพื่อเล่นใหม่", COL_HAZ)
    ui.poll()
    print("จบรอบ ถึงประตู", g.count, "เดินผ่าน", g.loiter)
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
            on_arm(w, g, sw5)
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
            now = time.ticks_ms()
            if got is not None:
                cm = None if got[0] is None else smooth(g.hist, got[0])
                door = zone_cm()
                decide(w, g, cm, door, now)  # 2) ตัดสิน + 3) ทำ
                if tick % every == 0:  # 4) โชว์
                    show_door(w, g, cm, door, now)
            if g.inside and not g.acked and time.ticks_diff(now, g.t_beep) >= BEEP_MS:
                g.t_beep = now
                siren()
            matrix_tick(g, now)
            tick += 1
            wait_ms(SAMPLE_MS, (sw5, sw6))
    finally:
        finish(w, g)


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) CONFIRM_N = 1 เดินผ่านเร็ว ๆ เทียบกับ 3: แบบไหนเตือนผิดน้อย
# 2) เปิดเฝ้าเฉพาะกลางคืน: ปิดเฝ้าเองตอนกลางวัน (นับจาก time.ticks_ms)
# 3) รับ {"cmd":"arm","on":0} จากแอปให้เปิด/ปิดระบบเฝ้าได้ ตอนคนงานเข้าโกดัง
