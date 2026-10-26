# idea_9_mushroom_house.py - โรงเพาะเห็ด: อากาศแห้งเกิน ก้อนเชื้อไม่ออกดอก จับเวลาแห้งติดกัน
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# ต่อยอดจาก s2/sf2_04_crop_alert.py (WiFi MQTT ack set เหมือนเดิม) ack = พ่นหมอกแล้ว
# จอ: แถบความชื้นยาว (Bar) มีขีดต่ำสุด + นาฬิกาแห้งติดกัน (Seg7) ธีมป่าชื้น
# คิดบนบอร์ด: 1) แห้งติดกันมากี่วินาที (กลับมาชื้นพอ = เริ่มนับใหม่)
#             2) ขอพ่นหมอกไปแล้วกี่ครั้ง (นับทุกครั้งที่เพิ่งแห้งลงต่ำกว่าเกณฑ์)
# MQTT: คีย์ sf2_04 ครบ + "dry_s" (วินาทีแห้งติดกันตอนนี้) "mist_n" (ครั้งที่ขอพ่นหมอก)
# crop: oyster straw ear  alert หลักคือ "dry"
# เกณฑ์ = ค่าสาธิตในห้องเรียน ไม่ใช่คำแนะนำทางเกษตร (ห้องแห้งกว่า 55 % ให้ลด RH ต่ำลง)
# ลองเล่น: ห้อง 60 % = เริ่มแห้ง  เป่าลมหายใจใส่บอร์ดจนหายเตือน แล้วกด SW5 หรือส่ง {"cmd":"ack"}
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
CROP = "เห็ดนางฟ้า"
TEMP_OFFSET = 0.0    # บอร์ดอ่านสูงกว่าห้อง: ใส่ค่าชดเชย เช่น -7.0

BROKER = "broker.hivemq.com"
ROOT = "bento-aiot"
CLIENT_ID = "bento-farm-" + TEAM  # + เลขสุ่มตอนต่อ: ไม่ชน id เก่า
TOPIC_EVENT = ROOT + "/" + TEAM + "/event"
TOPIC_CMD = ROOT + "/" + TEAM + "/cmd"
BTN_NAMES = ("SW6", "SW5")

# (ชื่อไทย, รหัสอังกฤษ, T ต่ำ, T สูง, RH ต่ำ, RH สูง)  เป่าลมหายใจ = ชื้นดี  ห้อง 60 % = เริ่มแห้ง
CROPS = (
    ("เห็ดนางฟ้า", "oyster", 18, 32, 65, 100),
    ("เห็ดฟาง", "straw", 22, 35, 65, 100),
    ("เห็ดหูหนู", "ear", 18, 32, 60, 100),
)
T_HI_MAX = 45
READ_MS, BEEP_MS, POLL_MS, RUN_MS = 1000, 5000, 20, 600000
MX = (("MOIST", rgbmatrix.GREEN), ("DRYING", rgbmatrix.YELLOW), ("DRY", rgbmatrix.RED))
BOX = (200, 100, 380, 140)
RH_BAR = (150, 88, 560, 28)            # x, y, w, h ของแถบความชื้น 0-100 %

SPEAKER = 40
VOLUME = 25
# ธีมป่าชื้น: เขียวมอส ครีมดอกเห็ด
COL_TEXT, COL_DIM = 0xEAF7E6, 0x9DB8A0
COL_CARD, COL_EDGE = 0x14231A, 0x2F5A3B
COL_MOSS, COL_WARN, COL_BAD = 0x8BD17C, 0xF2CC8F, 0xE07A5F
COL_OK = 0x5FD3A0
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
        p.append(("โรงหนาว", "cold"))
    if t > t_hi:
        p.append(("โรงร้อน", "hot"))
    if h < h_lo:
        p.append(("อากาศแห้ง", "dry"))
    if h > h_hi:
        p.append(("ชื้นเกิน", "wet"))
    if not p:
        return 0, "ชื้นดี", "ok"
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
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_EDGE, max=20, value=2)
    ui.Label(title, x=x + 14, y=y + 6, color=COL_MOSS, value=16)


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: ต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("โรงเพาะเห็ด: อากาศชื้นพอไหม", x=12, y=6, color=COL_TEXT, value=24)
    w = {"box": None, "leds": []}
    card(12, 44, 768, 120, "ความชื้นในโรงเพาะ (%RH)")
    w["rh"] = ui.Seg7(text="--", x=28, y=76, w=100, h=56, color=COL_MOSS)
    x, y, bw, bh = RH_BAR
    w["bar"] = ui.Bar(x=x, y=y, w=bw, h=bh, min=0, max=100, value=0)
    w["bar"].color(COL_MOSS)  # Bar: color() = สีส่วนที่เติม (แบบ sf1_03) ใน Emulator จะเห็นเป็นสีราง
    ui.Label("0", x=x, y=y + bh + 6, color=COL_DIM, value=14)
    ui.Label("100", x=x + bw - 28, y=y + bh + 6, color=COL_DIM, value=14)
    w["mark"] = ui.Label("^ ต่ำสุด", x=x, y=y + bh + 6, color=COL_BAD, value=14)
    card(12, 174, 300, 170, "แห้งติดกันมา (นาที.วินาที)")
    w["dry"] = ui.Seg7(text="00.00", x=28, y=206, w=220, h=70, color=COL_MOSS)
    w["mist"] = ui.Label("ขอพ่นหมอก 0 ครั้ง", x=28, y=296, color=COL_DIM, value=16)
    card(322, 174, 380, 170, "ก้อนเชื้อ")
    w["crop"] = ui.Label("-", x=336, y=200, color=COL_DIM, value=14)
    w["mood"] = ui.Label("...", x=336, y=222, color=COL_WARN)
    w["temp"] = ui.Label("-- C  แดด +0", x=336, y=252, color=COL_TEXT, value=16)
    for i in range(3):
        lx = 336 + i * 100
        w["leds"].append(ui.Led(x=lx, y=282, w=16, h=16, color=LEVEL_COLORS[i]))
        ui.Label(("ชื้นดี", "เริ่มแห้ง", "แห้งเกิน")[i], x=lx + 22, y=280, color=COL_DIM, value=14)
    w["ack"] = ui.Label("-", x=336, y=310, color=COL_DIM, value=14)
    ui.Label("VR3 = แดดบนหลังคา  " + BTN_NAMES[1] + " = พ่นหมอกแล้ว", x=470, y=52, color=COL_DIM, value=14)
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
    w["ack"].text("แจ้ง %d ครั้ง: %s" % (s.alerts, msg))


def show_crop(w, s):
    t_lo, t_hi, h_lo, h_hi = s.lim
    w["crop"].text("%s ออกดอกดีที่ %d-%d C, RH %d %% ขึ้นไป" % (s.crop[0], t_lo, t_hi, h_lo))
    x, y, bw, bh = RH_BAR
    w["mark"].pos(x + h_lo * bw // 100 - 6, y + bh + 6)
    w["mark"].text("^ ต่ำสุด %d" % h_lo)


def show_reading(w, s, t, h, sun, level, why_th):
    w["rh"].text("%d" % int(h))
    w["rh"].color(LEVEL_COLORS[level])
    w["bar"].value(int(max(0, min(100, h))))
    w["bar"].color(LEVEL_COLORS[level])
    w["mood"].color(LEVEL_COLORS[level])
    w["mood"].text(("ชื้นดี", "เริ่มแห้ง: ", "แห้งเกิน!: ")[level] + ("" if level == 0 else why_th))
    w["temp"].text("%.1f C  แดด +%d" % (t, sun))
    w["dry"].text("%02d.%02d" % (s.dry_s // 60 % 100, s.dry_s % 60))
    w["dry"].color(COL_MOSS if s.dry_s == 0 else LEVEL_COLORS[level])
    w["mist"].text("ขอพ่นหมอก %d ครั้ง" % s.mist_n)
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
        self.crop = CROPS[idx]
        self.lim = list(self.crop[2:])
        self.dry_s = 0     # วินาทีที่แห้งติดกันตอนนี้
        self.mist_n = 0    # ครั้งที่เพิ่งแห้งลงต่ำกว่าเกณฑ์


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
    s.level, s.acked = level, level < 2
    s.alerts += 1
    if not send({"id": TEAM, "crop": s.crop[1], "level": level, "alert": why_en,
                 "temp_c": round(t, 1), "rh": round(h, 1), "sun_c": sun,
                 "dry_s": s.dry_s, "mist_n": s.mist_n}):
        stop(w, "สายหลุดตอนส่ง")
    rgbmatrix.scroll(MX[level][0], MX[level][1], 80)
    beep(("good", "tap", "empty")[level])
    show_ack(w, s, "รอพ่นหมอก" if level == 2 else "ส่งแล้ว", COL_BAD if level == 2 else COL_DIM)
    close_box(w)
    if level == 2:  # MsgBox รับไม่เกิน 126 ไบต์
        w["box"] = ui.MsgBox("แห้งเกิน!\n" + why_th + "\nพ่นหมอก กด " + BTN_NAMES[1],
                             x=BOX[0], y=BOX[1], w=BOX[2], h=BOX[3], value=0)


def read_and_judge(w, s):
    # คืน True ถ้าเพิ่งแจ้ง
    sun = pots.read(2) * 10 // 4095  # VR3 = แดดจำลอง 0-10 C ที่บวกเข้าอุณหภูมิ
    t, h = read_climate()
    if t is None or h is None:
        s.t = None  # อ่านไม่ได้ = ลองใหม่รอบหน้าเลย
        return False
    s.t = t = t + sun
    if h < s.lim[2]:
        if s.dry_s == 0:
            s.mist_n += 1  # เพิ่งแห้งลง = ขอพ่นหมอกครั้งใหม่
        s.dry_s += READ_MS // 1000  # อ่านทุก 1 วิ
    else:
        s.dry_s = 0
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
    show_status(w, "เฝ้าโรงเพาะอยู่", COL_OK)
    was_down = False
    t_read = t_beep = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        msg = mqtt.get_message()  # ฟังทุก 20 ms อ่านเซนเซอร์ทุกวินาที
        ack = on_command(w, s, msg[1]) if msg else False
        down = buttons.pressed(1)  # SW5 = คนดูแลพ่นหมอกแล้ว ไม่พึ่งเน็ต
        if down and not was_down:  # นับตอนเพิ่งกดลง
            ack = True
            if s.acked:  # ไม่มีเหตุเตือน: บอกว่าปุ่มทำงาน
                show_ack(w, s, "ยังไม่มีเหตุเตือน", COL_DIM)
                beep("tap")
        was_down = down
        if ack and not s.acked:  # รับทราบ: หยุดร้อง ปิดกล่อง RGB ขึ้น ACK
            s.acked = True
            close_box(w)
            show_ack(w, s, "พ่นหมอกแล้ว", COL_OK)
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
# 1) แจ้งเฉพาะเมื่อแห้งติดกัน 5 วินาที (ใช้ s.dry_s) เปิดประตูโรงแป๊บเดียวไม่ควรเตือน
# 2) ให้ {"cmd":"set"} รับ "rh_lo" ด้วย (ตอนนี้รับแค่ t_hi) แล้วขยับขีดต่ำสุดบนจอ
# 3) สั่งพ่นหมอกจากแอป: วาง elif "pump" จาก sf2_03 ใน on_command
