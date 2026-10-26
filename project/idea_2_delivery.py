# idea_2_delivery.py - รถส่งผลผลิตถึงตลาด: นาฬิกาเที่ยวรถ กระบะร้อนกี่วินาที ลูกค้ากดรับของ
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# ต่อยอดจาก s2/sf2_04_crop_alert.py (WiFi MQTT ack set เหมือนเดิม) ack = ลูกค้ารับของ (ครั้งเดียว)
# จอ: นาฬิกาเที่ยวตัวใหญ่ (Seg7) + แถบเส้นทางฟาร์มถึงตลาด (Bar) ธีมถนนยางมะตอย
# คิดบนบอร์ด: 1) นาฬิกาเที่ยว + เวลาถึงตลาดโดยประมาณ (ETA_MIN)
#             2) กระบะร้อนเกินรวมกี่วินาทีตลอดทาง
# MQTT: คีย์ sf2_04 ครบ + "hot_s" (วินาทีที่กระบะร้อนเกินสะสม) ทั้งใน event เตือนและ delivered
#   ตอนรับของ (temp_c อาจ null):
#   {"id":"team05","event":"delivered","by":"sw5","trip_min":7,"temp_c":27.4,"hot_s":40}
# crop: veg fruit flower  เกณฑ์ = ค่าสาธิตในห้อง ไม่ใช่คำแนะนำทางเกษตร
# ลองเล่น: หมุน VR3 จนขึ้นเตือน แล้วกด SW5 หรือส่ง {"cmd":"ack"} = ลูกค้ารับของ
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
CROP = "ผักสด"
TEMP_OFFSET = 0.0    # บอร์ดอ่านสูงกว่าห้อง: ใส่ค่าชดเชย เช่น -7.0
ETA_MIN = 10         # เที่ยวสาธิต: ฟาร์มถึงตลาดราว 10 นาที

BROKER = "broker.hivemq.com"
ROOT = "bento-aiot"
CLIENT_ID = "bento-farm-" + TEAM  # + เลขสุ่มตอนต่อ: ไม่ชน id เก่า
TOPIC_EVENT = ROOT + "/" + TEAM + "/event"
TOPIC_CMD = ROOT + "/" + TEAM + "/cmd"
BTN_NAMES = ("SW6", "SW5")

# (ชื่อไทย, รหัสอังกฤษ, T ต่ำ, T สูง, RH ต่ำ, RH สูง)  ห้อง 27 C = ปกติ  VR3 ครึ่งรอบ (+5) = ร้อน
CROPS = (
    ("ผักสด", "veg", 15, 30, 20, 95),
    ("ผลไม้", "fruit", 15, 31, 20, 95),
    ("ดอกไม้", "flower", 15, 29, 20, 95),
)
T_HI_MAX = 45
READ_MS, BEEP_MS, POLL_MS, RUN_MS = 1000, 5000, 20, 600000
MX = (("ON ROAD", rgbmatrix.GREEN), ("WARM", rgbmatrix.YELLOW), ("HOT", rgbmatrix.RED))
BOX = (196, 100, 380, 140)

SPEAKER = 40
VOLUME = 25
# ธีมถนน: เทายางมะตอย เส้นจราจรเหลือง
COL_TEXT, COL_DIM = 0xF5F5F7, 0xA1A1AA
COL_CARD, COL_EDGE = 0x1C1C1E, 0x3A3A3C
COL_ROAD, COL_WARN, COL_BAD = 0xFFD23F, 0xFF9F0A, 0xFF453A
COL_OK = 0x34C759
LEVEL_COLORS = (COL_OK, COL_WARN, COL_BAD)


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
        p.append(("กระบะเย็นจัด", "cold"))
    if t > t_hi:
        p.append(("กระบะร้อน", "hot"))
    if h < h_lo:
        p.append(("ลมแห้ง", "dry"))
    if h > h_hi:
        p.append(("ชื้นอบ", "wet"))
    if not p:
        return 0, "ของสดดี", "ok"
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
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_EDGE, max=6, value=2)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_ROAD, value=16)


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: ต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("รถส่งผลผลิตถึงตลาด", x=12, y=6, color=COL_TEXT, value=24)
    w = {"box": None, "leds": []}
    card(12, 44, 372, 160, "นาฬิกาเที่ยวนี้ (นาที.วินาที)")
    w["clock"] = ui.Seg7(text="00.00", x=24, y=74, w=240, h=80, color=COL_ROAD)
    w["eta"] = ui.Label("ถึงตลาดอีกราว %d นาที" % ETA_MIN, x=24, y=166, color=COL_DIM, value=16)
    card(394, 44, 386, 160, "ท้ายกระบะ")
    w["crop"] = ui.Label("-", x=406, y=70, color=COL_DIM, value=14)
    w["th"] = ui.Label("-- C  -- %", x=406, y=94, color=COL_TEXT, value=24)
    w["mood"] = ui.Label("...", x=406, y=130, color=COL_WARN)
    w["hot"] = ui.Label("ร้อนมาแล้ว 0 วิ", x=406, y=166, color=COL_DIM, value=16)
    card(12, 214, 768, 82, "เส้นทาง")
    ui.Label("ฟาร์ม", x=24, y=252, color=COL_DIM, value=16)
    w["road"] = ui.Bar(x=80, y=254, w=470, h=20, min=0, max=100, value=0)
    w["road"].color(COL_ROAD)  # Bar: color() = สีส่วนที่เติม (แบบ sf1_03) ใน Emulator จะเห็นเป็นสีราง
    ui.Label("ตลาด", x=560, y=252, color=COL_DIM, value=16)
    for i in range(3):
        lx = 620 + i * 52
        w["leds"].append(ui.Led(x=lx, y=254, w=18, h=18, color=LEVEL_COLORS[i]))
    w["trip"] = ui.Label("กำลังขนส่ง", x=12, y=304, color=COL_ROAD, value=16)
    ui.Label("VR3 = แดดบนกระบะ  " + BTN_NAMES[1] + " = ลูกค้ารับของ", x=260, y=304, color=COL_DIM, value=14)
    w["ack"] = ui.Label("-", x=260, y=326, color=COL_DIM, value=14)
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
    w["crop"].text("%s ไม่ควรเกิน %d C" % (s.crop[0], t_hi))


def show_trip(w, s, ms):
    sec = ms // 1000
    w["clock"].text("%02d.%02d" % (sec // 60 % 100, sec % 60))
    w["road"].value(min(100, sec * 100 // (ETA_MIN * 60)))
    left = ETA_MIN - sec // 60
    w["eta"].text("ถึงตลาดอีกราว %d นาที" % left if left > 0 else "เลยเวลาที่คาดไว้แล้ว")


def show_reading(w, s, t, h, sun, level, why_th):
    w["th"].text("%.1f C  %d %%  แดด +%d" % (t, int(h), sun))
    w["mood"].color(LEVEL_COLORS[level])
    w["mood"].text(("ของสดดี", "เริ่มร้อน: ", "ร้อนเกิน!: ")[level] + ("" if level == 0 else why_th))
    w["hot"].text("ร้อนมาแล้ว %d วิ" % s.hot_s)
    w["hot"].color(COL_DIM if s.hot_s == 0 else COL_WARN)
    for i in range(3):
        w["leds"][i].value(1 if i == level else 0)


def close_box(w):
    if w["box"] is not None:
        w["box"].delete()
        w["box"] = None


# ---- 6) โปรแกรมหลัก ----
class Watch:
    def __init__(self, idx):
        self.level, self.acked, self.alerts, self.t = -1, True, 0, None
        self.done = False  # ลูกค้ารับของแล้วหรือยัง
        self.crop = CROPS[idx]
        self.lim = list(self.crop[2:])
        self.hot_s = 0     # วินาทีที่กระบะร้อนเกิน T สูง


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
    elif act == "set":  # เกณฑ์ใหม่ ต้องเป็น int ในช่วงที่สมเหตุผล
        v = cmd.get("t_hi")
        if isinstance(v, int) and s.lim[0] < v <= T_HI_MAX:
            s.lim[1], s.level, s.t = v, -1, None  # ตัดสินใหม่และแจ้งทันที
            show_crop(w, s)
            beep("tap")
    # >>> ภารกิจกลุ่ม: วาง elif สำหรับ "led"/"pump" จาก sf2_03 ตรงนี้ <<<
    return False


def on_level(w, s, level, why_th, why_en, t, h, sun):
    # ระดับเปลี่ยน: ส่งก่อน (สัญญาข้อ 3.4) แล้วค่อยเสียง RGB กล่องเตือน
    s.level, s.acked = level, level < 2 or s.done
    s.alerts += 1
    if not send({"id": TEAM, "crop": s.crop[1], "level": level, "alert": why_en,
                 "temp_c": round(t, 1), "rh": round(h, 1), "sun_c": sun, "hot_s": s.hot_s}):
        stop(w, "สายหลุดตอนส่ง")
    rgbmatrix.scroll(MX[level][0], MX[level][1], 80)
    beep(("good", "tap", "empty")[level])
    show_ack(w, s, "คนขับ: ตรวจความเย็น" if level == 2 else "แจ้งมือถือแล้ว", COL_BAD if level == 2 else COL_DIM)
    close_box(w)
    if level == 2 and not s.done:  # MsgBox รับไม่เกิน 126 ไบต์
        w["box"] = ui.MsgBox("ร้อนเกิน!\n" + why_th + "\nจอดตรวจ กด " + BTN_NAMES[1],
                             x=BOX[0], y=BOX[1], w=BOX[2], h=BOX[3], value=0)


def read_and_judge(w, s):
    # คืน True ถ้าเพิ่งแจ้ง
    sun = pots.read(2) * 10 // 4095  # VR3 = แดดจำลอง 0-10 C ที่บวกเข้าอุณหภูมิ
    t, h = read_climate()
    if t is None or h is None:
        s.t = None  # อ่านไม่ได้ = ลองใหม่รอบหน้าเลย
        return False
    s.t = t = t + sun
    if t > s.lim[1] and not s.done:
        s.hot_s += READ_MS // 1000  # อ่านทุก 1 วิ
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
    if not valid_team(TEAM):
        stop(w, "แก้ TEAM ก่อน")
    problem = connect_farm(w)
    if problem:
        stop(w, problem)
    show_link(w, True)
    show_status(w, "ออกรถแล้ว เฝ้ากระบะอยู่", COL_OK)
    was_down = False
    t_read = t_beep = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        msg = mqtt.get_message()  # ฟังทุก 20 ms อ่านเซนเซอร์ทุกวินาที
        ack = on_command(w, s, msg[1]) if msg else False
        by = "app"
        down = buttons.pressed(1)  # SW5 = ลูกค้ากดรับของที่รถ ไม่พึ่งเน็ต
        if down and not was_down:  # นับตอนเพิ่งกดลง
            ack, by = True, "sw5"
            if s.done:  # รับของไปแล้ว: บอกว่าปุ่มทำงาน
                show_ack(w, s, "รับของไปแล้ว", COL_DIM)
                beep("tap")
        was_down = down
        if ack and not s.done:  # ลูกค้ารับของ: ครั้งเดียวต่อเที่ยว หยุดร้อง ปิดกล่องเตือน
            s.done = s.acked = True
            close_box(w)
            mins = time.ticks_diff(now, t0) // 60000
            show_ack(w, s, "ลูกค้ารับของแล้ว", COL_OK)
            w["trip"].color(COL_OK)
            w["trip"].text("ส่งถึงแล้ว %d นาที" % mins)
            rgbmatrix.scroll("DELIVERED", rgbmatrix.CYAN, 80)
            beep("good")
            if not send({"id": TEAM, "event": "delivered", "by": by, "trip_min": mins,
                         "temp_c": None if s.t is None else round(s.t, 1), "hot_s": s.hot_s}):
                stop(w, "สายหลุดตอนส่ง")
        ui.poll()
        if s.t is None or time.ticks_diff(now, t_read) >= READ_MS:
            t_read = now
            if not s.done:
                show_trip(w, s, time.ticks_diff(now, t0))
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
# 1) แจ้งเมื่อร้อนติดกัน 5 วินาที
# 2) นับถนนขรุขระจาก IMU (s1/sf1_04_tank_tilt.py) แล้วส่งคีย์ "bumps"
# 3) ให้ SW6 (buttons.pressed(0)) = "ออกรถ" เริ่มจับเวลาเที่ยวใหม่
