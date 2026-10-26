# idea_10_banana_ripening.py - ห้องบ่มกล้วย: สีเปลือก 5 ขั้น บ่มไปแล้วกี่ % พร้อมขายเมื่อไร
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# ต่อยอดจาก s2/sf2_04_crop_alert.py (WiFi MQTT ack set เหมือนเดิม)
# จอ: แถบสีเปลือก 5 ขั้น (Panel) + แถบบ่ม (Bar) + ไฟ เย็นไป/พอดี/ร้อนไป ธีมกล้วยหอม
# คิดบนบอร์ด: 1) บ่มคืบหน้าเฉพาะเวลาที่ห้องอยู่ในช่วง (คอลัมน์สุดท้ายของ CROPS)
#             2) หลุดช่วงรวมกี่วินาที (เวลาที่เสียไป)
# MQTT: คีย์ sf2_04 ครบ + "ripe_pct" (บ่มไปแล้ว 0-100) "out_s" (วินาทีที่หลุดช่วงสะสม)
# crop: hom namwa khai  เกณฑ์ = ค่าสาธิตในห้อง 25-30 C ไม่ใช่ค่าบ่มกล้วยจริง
# ลองเล่น: หมุน VR3 จนร้อนไป ดูแถบบ่มหยุด กด SW5 หรือส่ง {"cmd":"ack"}
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
CROP = "กล้วยหอม"
TEMP_OFFSET = 0.0    # บอร์ดอ่านสูงกว่าห้อง: ใส่ค่าชดเชย เช่น -7.0

BROKER = "broker.hivemq.com"
ROOT = "bento-aiot"
CLIENT_ID = "bento-farm-" + TEAM  # + เลขสุ่มตอนต่อ: ไม่ชน id เก่า
TOPIC_EVENT = ROOT + "/" + TEAM + "/event"
TOPIC_CMD = ROOT + "/" + TEAM + "/cmd"
BTN_NAMES = ("SW6", "SW5")

# (ชื่อไทย, รหัสอังกฤษ, T ต่ำ, T สูง, RH ต่ำ, RH สูง, วินาทีที่ต้องบ่ม)  ห้อง 27 C = พอดี
CROPS = (
    ("กล้วยหอม", "hom", 22, 30, 20, 95, 300),
    ("กล้วยน้ำว้า", "namwa", 22, 31, 20, 95, 360),
    ("กล้วยไข่", "khai", 22, 30, 20, 95, 240),
)
T_HI_MAX = 45
READ_MS, BEEP_MS, POLL_MS, RUN_MS = 1000, 5000, 20, 600000
MX = (("RIPENING", rgbmatrix.GREEN), ("CHECK", rgbmatrix.YELLOW), ("ALERT", rgbmatrix.RED))
BOX = (200, 110, 380, 140)
# สีเปลือก 5 ขั้น: เขียวจัด -> เหลืองจุดน้ำตาล
PEEL = ((0x3E8E41, "เขียว"), (0x8DBF3A, "เขียวอม"), (0xD7D83A, "เริ่มเหลือง"),
        (0xFFE135, "เหลือง"), (0xE0B12B, "จุดน้ำตาล"))

SPEAKER = 40
VOLUME = 25
# ธีมกล้วยหอม: น้ำตาลเข้ม เหลืองเปลือกกล้วย
COL_TEXT, COL_DIM = 0xFFF8DC, 0xC2B280
COL_CARD, COL_EDGE = 0x2B2410, 0x5A4A1C
COL_BANANA, COL_WARN, COL_BAD = 0xFFE135, 0xFFA94D, 0xFF6B6B
COL_OK, COL_COLD = 0x9ACD32, 0x6CB4EE
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
        p.append(("ห้องเย็นไป", "cold"))
    if t > t_hi:
        p.append(("ห้องร้อนไป", "hot"))
    if h < h_lo:
        p.append(("เปลือกแห้ง", "dry"))
    if h > h_hi:
        p.append(("ชื้นเกิน", "wet"))
    if not p:
        return 0, "พอดี", "ok"
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
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_EDGE, max=14, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_BANANA, value=16)


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: ต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("ห้องบ่มกล้วย: พร้อมขายเมื่อไร", x=12, y=6, color=COL_TEXT, value=24)
    w = {"box": None, "leds": []}
    card(12, 44, 768, 150, "สีเปลือก (ขั้นการบ่ม)")
    for i in range(5):
        px = 24 + i * 150
        ui.Panel(x=px, y=72, w=140, h=30, color=PEEL[i][0], min=COL_EDGE, max=15, value=0)
        ui.Label(PEEL[i][1], x=px + 6, y=106, color=COL_DIM, value=14)
    w["here"] = ui.Label("^ ตอนนี้", x=24, y=124, color=COL_BANANA, value=14)
    w["ripe"] = ui.Bar(x=24, y=150, w=560, h=18, min=0, max=100, value=0)
    w["ripe_t"] = ui.Label("บ่มไปแล้ว 0 %", x=600, y=148, color=COL_TEXT, value=16)
    card(12, 204, 380, 140, "อุณหภูมิห้องบ่ม")
    w["seg"] = ui.Seg7(text="--.-", x=24, y=234, w=150, h=56, color=COL_BANANA)
    w["rh"] = ui.Label("C  RH -- %", x=184, y=256, color=COL_DIM, value=16)
    for i in range(3):
        lx = 24 + i * 120
        w["leds"].append(ui.Led(x=lx, y=306, w=16, h=16, color=(COL_COLD, COL_OK, COL_BAD)[i]))
        ui.Label(("เย็นไป", "พอดี", "ร้อนไป")[i], x=lx + 22, y=304, color=COL_DIM, value=14)
    card(402, 204, 300, 140, "แจ้งคนบ่ม")
    w["mood"] = ui.Label("...", x=414, y=232, color=COL_WARN, value=16)
    w["left"] = ui.Label("-", x=414, y=258, color=COL_TEXT, value=16)
    w["out"] = ui.Label("หลุดช่วง 0 วิ", x=414, y=284, color=COL_DIM, value=14)
    w["ack"] = ui.Label("-", x=414, y=306, color=COL_DIM, value=14)
    ui.Label("VR3 = ความร้อนจากหลังคา  " + BTN_NAMES[1] + " = รับทราบ", x=430, y=50, color=COL_DIM, value=14)
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
    w["left"].text("%s บ่มที่ %d-%d C" % (s.crop[0], t_lo, t_hi))


def ripe_pct(s):
    return min(100, s.ok_s * 100 // s.need_s)


def show_reading(w, s, t, h, sun, level, why_th):
    w["seg"].text("%.1f" % t)
    w["seg"].color(LEVEL_COLORS[level])
    w["rh"].text("C  RH %d %%  แดด +%d" % (int(h), sun))
    side = 0 if t < s.lim[0] else (2 if t > s.lim[1] else 1)
    for i in range(3):
        w["leds"][i].value(1 if i == side else 0)
    pct = ripe_pct(s)
    w["ripe"].value(pct)
    w["ripe_t"].text("บ่มไปแล้ว %d %%" % pct)
    w["here"].pos(24 + min(4, pct // 20) * 150, 124)
    w["mood"].color(LEVEL_COLORS[level])
    w["mood"].text(("บ่มอยู่ พอดี", "เริ่มหลุด: ", "หลุดมาก!: ")[level] + ("" if level == 0 else why_th))
    left = s.need_s - s.ok_s
    w["left"].text("พร้อมขายอีก %d นาที %d วิ" % (left // 60, left % 60) if left > 0 else "พร้อมขายแล้ว")
    w["out"].text("หลุดช่วง %d วิ (บ่มช้าลง)" % s.out_s)


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
        self.need_s = self.crop[6]
        self.ok_s = 0      # วินาทีที่อยู่ในช่วง = บ่มเดินหน้า
        self.out_s = 0     # วินาทีที่หลุดช่วง
        self.ready = False


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
                 "ripe_pct": ripe_pct(s), "out_s": s.out_s}):
        stop(w, "สายหลุดตอนส่ง")
    rgbmatrix.scroll(MX[level][0], MX[level][1], 80)
    beep(("good", "tap", "empty")[level])
    show_ack(w, s, "รอคนบ่ม" if level == 2 else "ส่งแล้ว", COL_BAD if level == 2 else COL_DIM)
    close_box(w)
    if level == 2:  # MsgBox รับไม่เกิน 126 ไบต์
        w["box"] = ui.MsgBox("หลุดช่วง!\n" + why_th + "\n" + BTN_NAMES[1] + " = รับทราบ",
                             x=BOX[0], y=BOX[1], w=BOX[2], h=BOX[3], value=0)


def read_and_judge(w, s):
    # คืน True ถ้าเพิ่งแจ้ง
    sun = pots.read(2) * 10 // 4095  # VR3 = ความร้อนจำลอง 0-10 C ที่บวกเข้าอุณหภูมิ
    t, h = read_climate()
    if t is None or h is None:
        s.t = None  # อ่านไม่ได้ = ลองใหม่รอบหน้าเลย
        return False
    s.t = t = t + sun
    level, why_th, why_en = judge(t, h, s.lim)
    if level == 0:
        s.ok_s = min(s.need_s, s.ok_s + READ_MS // 1000)  # อ่านทุก 1 วิ
    else:
        s.out_s += READ_MS // 1000
    show_reading(w, s, t, h, sun, level, why_th)
    if not s.ready and s.ok_s >= s.need_s:  # สุกพอดี: บอกครั้งเดียว
        s.ready = True
        w["ripe"].color(COL_OK)  # Bar: color() = สีส่วนที่เติม (แบบ sf1_03) ใน Emulator จะเห็นเป็นสีราง
        rgbmatrix.scroll("READY", rgbmatrix.YELLOW, 80)
        beep("good")
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
    show_status(w, "เฝ้าห้องบ่ม", COL_OK)
    was_down = False
    t_read = t_beep = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        msg = mqtt.get_message()  # ฟังทุก 20 ms อ่านเซนเซอร์ทุกวินาที
        ack = on_command(w, s, msg[1]) if msg else False
        down = buttons.pressed(1)  # SW5 = คนบ่มรับทราบ ไม่พึ่งเน็ต
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
# 1) ตอนสุกพอดี ส่ง event {"alert":"ready"} ครั้งเดียว แล้วให้เว็บขึ้นป้ายพร้อมขาย
# 2) ให้ {"cmd":"set"} รับ "t_lo" ด้วย (ตอนนี้รับแค่ t_hi) แล้วบอกฝั่งเว็บ
# 3) ร้อนไป = สุกเร็วแต่เปลือกด่าง: นับ "ด่าง" แยก
