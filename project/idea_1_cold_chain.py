# idea_1_cold_chain.py - กล่องผลไม้ห้องเย็น: นับเวลาที่กล่องอุ่นเกิน เทียบกับงบที่ผลไม้ทนได้
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# ต่อยอดจาก s2/sf2_04_crop_alert.py (WiFi MQTT ack set เหมือนเดิม)
# จอ: ปรอทแนวตั้ง (Bar) ตัวเลขใหญ่ (Seg7) วงงบความร้อน (Arc)
# คิดบนบอร์ด: 1) งบความร้อน = วินาทีที่ผลไม้ทนอุ่นเกินได้ (คอลัมน์สุดท้ายของ CROPS)
#             2) อุณหภูมิสูงสุดตั้งแต่ออกรถ
# MQTT: คีย์ sf2_04 ครบ + "warm_s" (วินาทีอุ่นเกินสะสม) "t_max" (สูงสุด C)
# crop: mango strawberry durian  เกณฑ์ = ค่าสาธิตในห้อง ไม่ใช่คำแนะนำทางเกษตร
# ลองเล่น: หมุน VR3 จนเกินเกณฑ์ ดูวงงบลดลง แล้วกด SW5 หรือส่ง {"cmd":"ack"}
# ต้องแก้ก่อนรัน: WIFI_SSID, WIFI_PASS, TEAM, CROP, TEMP_OFFSET

import buttons
import json
import mqtt
import pots
import rgbmatrix
import sensors
import time
import ui
import wifi

# ---- 1) ตั้งค่า ----
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"   # ตั้งเอง: อังกฤษ/ตัวเลขสั้น ๆ ไม่มีเว้นวรรค
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"   # อย่างน้อย 8 ตัว · อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร
TEAM = "teamXX"
CROP = "มะม่วง"
TEMP_OFFSET = 0.0  # บอร์ดอ่านสูงกว่าห้อง: ใส่ค่าชดเชย เช่น -7.0

BROKER = "broker.hivemq.com"
ROOT = "bento-aiot"
CLIENT_ID = "bento-farm-" + TEAM       # + เลขสุ่มตอนต่อ: ไม่ชน id เก่า
TOPIC_EVENT = ROOT + "/" + TEAM + "/event"
TOPIC_CMD = ROOT + "/" + TEAM + "/cmd"
BTN_NAMES = ("SW6", "SW5")

# (ชื่อไทย, รหัสอังกฤษ, T ต่ำ, T สูง, RH ต่ำ, RH สูง, งบความร้อน วินาที)
# ห้อง 27 C = ปกติ  VR3 ครึ่งรอบ (+5) = เริ่มอุ่น
CROPS = (
    ("มะม่วง", "mango", 15, 30, 20, 95, 180),
    ("สตรอว์เบอร์รี", "strawberry", 15, 28, 20, 95, 90),
    ("ทุเรียน", "durian", 15, 32, 20, 95, 240),
)
T_HI_MAX = 45
READ_MS, BEEP_MS, POLL_MS, RUN_MS = 1000, 5000, 20, 600000
THERMO_MAX = 40  # ปรอทแนวตั้ง 0-40 C
MX = (("COOL", rgbmatrix.CYAN), ("WARM", rgbmatrix.YELLOW), ("HOT", rgbmatrix.RED))
BOX = (196, 96, 380, 150)

SPEAKER = 40
VOLUME = 25
# ธีมน้ำแข็ง: น้ำเงินเข้ม ฟ้าใส
COL_TEXT, COL_DIM = 0xEAF6FF, 0x8FA9BF
COL_CARD, COL_EDGE = 0x0E1A2B, 0x2C4A66
COL_ICE, COL_WARN, COL_BAD = 0x7FD3FF, 0xFFB547, 0xFF5A5F
COL_OK = 0x3DD6C6
LEVEL_COLORS = (COL_ICE, COL_WARN, COL_BAD)
THERMO = (44, 84, 34, 236)  # x, y, w, h ของปรอท


# ---- 2) ฮาร์ดแวร์ ----
TUNES = {"tap": (76,), "good": (72, 79, 84), "bad": (84, 76), "empty": (84, 76, 69)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)

def read_climate():
    try:
        t_raw = sensors.sht40.temperature()
        h = sensors.sht40.humidity()
    except Exception:
        return None, None
    return t_raw + TEMP_OFFSET, h


# ---- 3) สมอง ----
def valid_team(team):
    return len(team) == 6 and team[:4] == "team" and team[4:].isdigit() and team != "team00"


def judge(t, h, lim):
    # คืน (ระดับ 0-2, ไทย, อังกฤษ)  ระดับ 2 = หลุดไกล (เกิน 3 C หรือ 10 %RH)
    t_lo, t_hi, h_lo, h_hi = lim
    p = []
    if t < t_lo:
        p.append(("เย็นจัด", "cold"))
    if t > t_hi:
        p.append(("กล่องอุ่น", "hot"))
    if h < h_lo:
        p.append(("แห้งไป", "dry"))
    if h > h_hi:
        p.append(("ไอน้ำเกาะ", "wet"))
    if not p:
        return 0, "เย็นดี", "ok"
    far = (t < t_lo - 3) or (t > t_hi + 3) or (h < h_lo - 10) or (h > h_hi + 10)
    return (2 if far else 1), " + ".join(x[0] for x in p), "+".join(x[1] for x in p)


# ---- 4) เครือข่าย ----
def connect_broker(w):
    # broker สาธารณะไม่ตอบเป็นพัก ๆ: ลอง 3 ครั้ง client_id ใหม่ทุกครั้ง
    for n in (1, 2, 3):
        if n > 1:
            show_status(w, "ลองต่อ broker ใหม่ %d/3" % n, COL_WARN)
            ui.poll()
        try:
            if mqtt.connect(BROKER, port=1883, keepalive=60,
                            client_id=CLIENT_ID + "-%04x" % (time.ticks_ms() & 0xFFFF)):
                return True
        except OSError:
            pass
    return False


def connect_farm(w):
    # WiFi -> IP -> broker + subscribe คืนข้อความบอกว่าพังขั้นไหน
    show_status(w, "ต่อ WiFi...", COL_WARN)
    ui.poll()  # ป้ายขึ้นก่อนบรรทัดที่บล็อก
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "WiFi ไม่ติด: ตรวจชื่อ/รหัส"
    linked = connect_broker(w)  # ลองได้ 3 ครั้ง
    if not linked or not mqtt.subscribe(TOPIC_CMD):  # subscribe หลัง connect เสมอ
        return "broker ไม่ตอบ: รอ 1 นาทีแล้วรันใหม่"
    return ""


def send(obj):
    # สายหลุดโยน OSError
    try:
        mqtt.publish(TOPIC_EVENT, json.dumps(obj))
        return True
    except OSError:
        return False


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_EDGE, max=10, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_ICE, value=16)


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: ต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def thermo_y(c):
    x, y, bw, bh = THERMO
    c = max(0, min(THERMO_MAX, c))
    return y + bh - c * bh // THERMO_MAX


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("กล่องผลไม้เย็น: นับเวลาร้อนสะสม", x=12, y=6, color=COL_TEXT, value=24)
    w = {"box": None}
    card(12, 44, 168, 300, "ปรอทในกล่อง")
    x, y, bw, bh = THERMO
    w["thermo"] = ui.Bar(x=x, y=y, w=bw, h=bh, min=0, max=THERMO_MAX, value=0)
    w["thermo"].color(COL_ICE)  # Bar: color() = สีส่วนที่เติม (แบบ sf1_03) ใน Emulator จะเห็นเป็นสีราง
    ui.Label("%d C" % THERMO_MAX, x=x + bw + 8, y=y - 8, color=COL_DIM, value=14)
    ui.Label("0 C", x=x + bw + 8, y=y + bh - 12, color=COL_DIM, value=14)
    w["mark"] = ui.Label("< เกณฑ์", x=x + bw + 8, y=y, color=COL_BAD, value=14)
    card(190, 44, 320, 186, "ในกล่องตอนนี้")
    w["crop"] = ui.Label("-", x=202, y=70, color=COL_DIM, value=14)
    w["seg"] = ui.Seg7(text="--.-", x=202, y=94, w=180, h=64, color=COL_ICE)
    ui.Label("C", x=392, y=120, color=COL_DIM)
    w["rh"] = ui.Label("RH -- %", x=420, y=120, color=COL_DIM, value=16)
    w["mood"] = ui.Label("...", x=202, y=172, color=COL_WARN)
    w["tmax"] = ui.Label("สูงสุด -- C", x=202, y=202, color=COL_DIM, value=14)
    card(520, 44, 260, 186, "งบความร้อนผลไม้")
    w["arc"] = ui.Arc(x=540, y=72, w=120, h=120, min=0, max=100, value=100)
    w["arc"].color(COL_OK)
    w["left"] = ui.Label("เหลือ -- วิ", x=670, y=100, color=COL_TEXT, value=16)
    w["used"] = ui.Label("ใช้ไป 0 วิ", x=670, y=128, color=COL_DIM, value=14)
    w["ack"] = ui.Label("-", x=540, y=200, color=COL_DIM, value=14)
    card(190, 240, 320, 104, "บอกคนขับ")
    w["leds"] = []
    for i in range(3):
        lx = 202 + i * 100
        w["leds"].append(ui.Led(x=lx, y=274, w=18, h=18, color=LEVEL_COLORS[i]))
        ui.Label(("เย็นดี", "เริ่มอุ่น", "ร้อนเกิน")[i], x=lx + 24, y=274, color=COL_DIM, value=14)
    ui.Label("VR3 = ร้อนจากนอกกล่อง  " + BTN_NAMES[1] + " = รับทราบ", x=202, y=310, color=COL_DIM, value=14)
    w["status"] = ui.Label("กำลังเริ่ม", x=12, y=352, color=COL_DIM)
    w["mq"] = ui.Led(x=606, y=12, w=18, h=18, color=COL_OK, value=0)
    w["mq_t"] = ui.Label("MQTT: ออฟไลน์", x=632, y=10, color=COL_DIM, value=16)
    ui.poll()
    return w


def show_status(w, msg, col):
    w["status"].color(col)
    w["status"].text(msg)


def show_ack(w, s, msg, col):
    w["ack"].color(col)
    w["ack"].text("แจ้งไปแล้ว %d ครั้ง: %s" % (s.alerts, msg))


def show_crop(w, s):
    t_lo, t_hi, h_lo, h_hi = s.lim
    w["crop"].text("%s เก็บที่ %d-%d C ทนอุ่นได้ %d วิ" % (s.crop[0], t_lo, t_hi, s.budget))
    w["mark"].pos(THERMO[0] + THERMO[2] + 8, thermo_y(t_hi) - 8)


def show_budget(w, s):
    left = max(0, s.budget - s.warm_s)
    pct = left * 100 // s.budget
    w["arc"].value(pct)
    w["arc"].color(COL_OK if pct > 50 else (COL_WARN if pct > 0 else COL_BAD))
    w["left"].text("เหลือ %d วิ" % left if left else "หมดงบ!")
    w["used"].text("ใช้ไป %d วิ" % s.warm_s)


def show_reading(w, s, t, h, sun, level, why_th):
    w["seg"].text("%.1f" % t)
    w["seg"].color(LEVEL_COLORS[level])
    w["rh"].text("RH %d %%" % int(h))
    w["thermo"].value(int(max(0, min(THERMO_MAX, t))))
    w["thermo"].color(LEVEL_COLORS[level])
    w["mood"].color(LEVEL_COLORS[level])
    w["mood"].text(("เย็นดี", "เริ่มอุ่น: ", "ร้อนเกิน!: ")[level] + ("" if level == 0 else why_th)
                   + "  (นอกกล่อง +%d)" % sun)
    w["tmax"].text("สูงสุดตั้งแต่ออกรถ %.1f C" % s.t_max)
    for i in range(3):
        w["leds"][i].value(1 if i == level else 0)
    show_budget(w, s)


def close_box(w):
    if w["box"] is not None:
        w["box"].delete()
        w["box"] = None


# ---- 6) โปรแกรมหลัก ----
class Watch:
    def __init__(self, idx):
        self.level, self.acked, self.alerts, self.t = -1, True, 0, None
        self.crop = CROPS[idx]
        self.lim = list(self.crop[2:6])
        self.budget = self.crop[6]
        self.warm_s = 0  # วินาทีอุ่นเกินสะสม
        self.t_max = -99.0  # สูงสุดตั้งแต่เริ่ม


class Stop(Exception):
    # ไม่ใช้ SystemExit (บอร์ดอาจค้าง)
    pass


def stop(w, msg, col=COL_BAD):
    show_link(w, False)
    rgbmatrix.scroll("")
    rgbmatrix.clear()
    close_box(w)
    show_status(w, msg, col)
    ui.poll()
    raise Stop


def on_command(w, s, raw):
    # คำสั่งจากแอป (สัญญาข้อ 4) คืน True ถ้าเป็น ack  ไม่รู้จัก = เงียบ
    try:  # ไม่ใช่ JSON object ก็ไม่ใช้
        cmd = json.loads(raw.decode())
        act = cmd.get("cmd")
    except Exception:
        return False
    if act == "ack":
        return True
    elif act == "set":                 # เกณฑ์ใหม่ ต้องเป็น int ในช่วงที่สมเหตุผล
        v = cmd.get("t_hi")
        if isinstance(v, int) and s.lim[0] < v <= T_HI_MAX:
            s.lim[1], s.level, s.t = v, -1, None  # ตัดสินใหม่และแจ้งทันที
            show_crop(w, s)
            beep("tap")
    # >>> ภารกิจกลุ่ม: วาง elif สำหรับ "led"/"pump" จาก sf2_03 ตรงนี้ <<<
    return False


def on_level(w, s, level, why_th, why_en, t, h, sun):
    # ระดับเปลี่ยน: ส่งก่อน (สัญญาข้อ 3.4) แล้วค่อยเสียง RGB กล่องเตือน
    s.level, s.acked = level, level < 2
    s.alerts += 1
    if not send({"id": TEAM, "crop": s.crop[1], "level": level, "alert": why_en,
                 "temp_c": round(t, 1), "rh": round(h, 1), "sun_c": sun,
                 "warm_s": s.warm_s, "t_max": round(s.t_max, 1)}):
        stop(w, "สายหลุดตอนส่ง")
    rgbmatrix.scroll(MX[level][0], MX[level][1], 80)
    beep(("good", "tap", "empty")[level])
    show_ack(w, s, "รอคนขับ" if level == 2 else "ส่งแล้ว", COL_BAD if level == 2 else COL_DIM)
    close_box(w)
    if level == 2:  # MsgBox รับไม่เกิน 126 ไบต์
        w["box"] = ui.MsgBox("กล่องร้อน!\n" + why_th + "\n" + BTN_NAMES[1] + " = รับทราบ",
                             x=BOX[0], y=BOX[1], w=BOX[2], h=BOX[3], value=0)


def read_and_judge(w, s):
    # คืน True ถ้าเพิ่งแจ้ง
    sun = pots.read(2) * 10 // 4095  # VR3 = ความร้อนจากนอกกล่องจำลอง 0-10 C ที่บวกเข้าอุณหภูมิ
    t, h = read_climate()
    if t is None or h is None:
        s.t = None  # อ่านไม่ได้ = ลองใหม่รอบหน้าเลย
        return False
    s.t = t = t + sun
    s.t_max = max(s.t_max, t)
    if t > s.lim[1]:
        s.warm_s += READ_MS // 1000  # อ่านทุก 1 วิ
    level, why_th, why_en = judge(t, h, s.lim)
    show_reading(w, s, t, h, sun, level, why_th)
    if level == s.level:  # ส่ง เสียง RGB เฉพาะตอนเปลี่ยน
        return False
    on_level(w, s, level, why_th, why_en, t, h, sun)
    return True


def main():
    if hasattr(ui, "volume"):
        ui.volume(SPEAKER)
    w = build_screen()
    names = [c[0] for c in CROPS]
    if CROP not in names:
        stop(w, "แก้ CROP ให้ตรงกับ CROPS")
    s = Watch(names.index(CROP))
    show_crop(w, s)
    show_budget(w, s)
    if not valid_team(TEAM):
        stop(w, "แก้ TEAM ก่อน")
    problem = connect_farm(w)
    if problem:
        stop(w, problem)
    show_link(w, True)
    show_status(w, "เฝ้ากล่องอยู่", COL_OK)
    was_down = False
    t_read = t_beep = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        msg = mqtt.get_message()  # ฟังทุก 20 ms อ่านเซนเซอร์ทุกวินาที
        ack = on_command(w, s, msg[1]) if msg else False
        down = buttons.pressed(1)  # SW5 = คนขับรับทราบ ไม่พึ่งเน็ต
        if down and not was_down:  # นับตอนเพิ่งกดลง
            ack = True
            if s.acked:  # ไม่มีเหตุเตือน: บอกว่าปุ่มทำงาน
                show_ack(w, s, "ยังไม่มีเหตุเตือน", COL_DIM)
                beep("tap")
        was_down = down
        if ack and not s.acked:  # รับทราบ: หยุดร้อง ปิดกล่อง RGB ขึ้น ACK
            s.acked = True
            close_box(w)
            show_ack(w, s, "รับทราบแล้ว", COL_OK)
            rgbmatrix.scroll("ACK", rgbmatrix.CYAN, 80)
            beep("tap")
        ui.poll()
        if s.t is None or time.ticks_diff(now, t_read) >= READ_MS:
            t_read = now
            if read_and_judge(w, s):
                t_beep = now
        if s.level == 2 and not s.acked and time.ticks_diff(now, t_beep) >= BEEP_MS:
            t_beep = now
            beep("bad")
        time.sleep_ms(POLL_MS)

    stop(w, "จบรอบ", COL_DIM)


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
# 1) งบหมด: ส่ง {"alert":"budget"} ครั้งเดียว แล้วโชว์บนเว็บ
# 2) นับการกระแทกกล่องจาก IMU (s1/sf1_04_tank_tilt.py)
# 3) ให้ {"cmd":"set"} รับ "budget" แล้วนับงบใหม่
