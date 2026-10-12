# idea_3_silo.py - ไซโลข้าวเปลือก: วงความชื้นใหญ่ นับเวลาเสี่ยงรา มือถือรู้ทันที
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# ต่อยอดจาก s2/sf2_04_crop_alert.py (WiFi MQTT ack set เหมือนเดิม)
# จอ: วงความชื้น (Arc) ตัวใหญ่ + กราฟความชื้น (Chart) เส้นแดง = RH สูง ธีมรวงข้าว
# คิดบนบอร์ด: 1) เวลาเสี่ยงรา = วินาทีที่ RH เกินเกณฑ์สะสม
#             2) RH ต่ำสุด/สูงสุดตั้งแต่เริ่ม
# MQTT: คีย์ sf2_04 ครบ + "wet_s" (วินาทีที่ชื้นเกินสะสม) "rh_max" (RH สูงสุด)
# crop: paddy seed corn (RH ต่ำ = 0 ไม่เตือนแห้ง)
# เกณฑ์ = ค่าสาธิตในห้องเรียน ไม่ใช่คำแนะนำทางเกษตร (ห้องชื้นเกิน 70 % ให้ขยับ RH สูงขึ้น)
# ลองเล่น: เป่าลมหายใจใส่บอร์ด จนวงเป็นสีแดง แล้วกด SW5 หรือส่ง {"cmd":"ack"}
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
CROP = "ข้าวเปลือก"
TEMP_OFFSET = 0.0    # บอร์ดอ่านสูงกว่าห้อง: ใส่ค่าชดเชย เช่น -7.0

BROKER = "broker.hivemq.com"
ROOT = "bento-aiot"
CLIENT_ID = "bento-farm-" + TEAM  # + เลขสุ่มตอนต่อ: ไม่ชน id เก่า
TOPIC_EVENT = ROOT + "/" + TEAM + "/event"
TOPIC_CMD = ROOT + "/" + TEAM + "/cmd"
BTN_NAMES = ("SW6", "SW5")

# (ชื่อไทย, รหัสอังกฤษ, T ต่ำ, T สูง, RH ต่ำ, RH สูง)  ห้อง 60 % = ปกติ  เป่าลมหายใจ = เริ่มชื้น
CROPS = (
    ("ข้าวเปลือก", "paddy", 15, 35, 0, 70),
    ("เมล็ดพันธุ์", "seed", 15, 35, 0, 65),
    ("ข้าวโพด", "corn", 15, 35, 0, 70),
)
T_HI_MAX = 45
READ_MS, BEEP_MS, POLL_MS, RUN_MS = 1000, 5000, 20, 600000
MX = (("DRY", rgbmatrix.GREEN), ("DAMP", rgbmatrix.YELLOW), ("MOLD", rgbmatrix.RED))
BOX = (220, 100, 360, 140)

SPEAKER = 40
VOLUME = 25
# ธีมรวงข้าว: น้ำตาลยุ้งฉาง เหลืองข้าวสุก
COL_TEXT, COL_DIM = 0xFFF3DC, 0xBFA88A
COL_CARD, COL_EDGE = 0x2A2118, 0x5C4A33
COL_GRAIN, COL_WARN, COL_BAD = 0xE9C46A, 0xF4A261, 0xE76F51
COL_OK = 0x8AB17D
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
        p.append(("ไซโลเย็น", "cold"))
    if t > t_hi:
        p.append(("ไซโลร้อน", "hot"))
    if h < h_lo:
        p.append(("แห้งไป", "dry"))
    if h > h_hi:
        p.append(("ชื้นเกิน", "wet"))
    if not p:
        return 0, "แห้งดี", "ok"
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
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_EDGE, max=16, value=1)
    ui.Label(title, x=x + 14, y=y + 6, color=COL_GRAIN, value=16)


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: ต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("ไซโลข้าวเปลือก: ชื้นจนขึ้นราไหม", x=12, y=6, color=COL_TEXT, value=24)
    w = {"box": None}
    card(12, 44, 300, 300, "ความชื้นในไซโล")
    w["arc"] = ui.Arc(x=36, y=78, w=220, h=220, min=0, max=100, value=0)
    w["arc"].color(COL_OK)
    w["rh"] = ui.Seg7(text="--", x=106, y=150, w=80, h=56, color=COL_OK)
    ui.Label("%RH", x=128, y=214, color=COL_DIM, value=16)
    w["crop"] = ui.Label("-", x=26, y=310, color=COL_DIM, value=14)
    card(322, 44, 458, 136, "เวลาเสี่ยงรา (นาที.วินาที)")
    w["wet"] = ui.Seg7(text="00.00", x=336, y=74, w=180, h=60, color=COL_GRAIN)
    w["mood"] = ui.Label("...", x=530, y=78, color=COL_WARN, value=16)
    w["mm"] = ui.Label("ต่ำสุด -- สูงสุด -- %", x=530, y=108, color=COL_DIM, value=14)
    w["temp"] = ui.Label("-- C  แดด +0", x=336, y=146, color=COL_DIM, value=16)
    w["ack"] = ui.Label("-", x=530, y=146, color=COL_DIM, value=14)
    w["chart"] = ui.Chart(x=322, y=190, w=400, h=110, color=COL_GRAIN, min=0, max=100)
    w["chart"].prop(ui.PROP_CHART_POINTS, 400)
    w["s_hi"] = w["chart"].add_series(COL_BAD)
    ui.Label("เหลือง = ความชื้น  แดง = เกณฑ์ราขึ้น", x=322, y=306, color=COL_DIM, value=14)
    ui.Label("VR3 = แดดบนหลังคา  " + BTN_NAMES[1] + " = รับทราบ", x=322, y=326, color=COL_DIM, value=14)
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
    w["crop"].text("%s เก็บดีเมื่อ RH ไม่เกิน %d %%" % (s.crop[0], h_hi))


def show_reading(w, s, t, h, sun, level, why_th):
    w["arc"].value(int(max(0, min(100, h))))
    w["arc"].color(LEVEL_COLORS[level])
    w["rh"].text("%d" % int(h))
    w["rh"].color(LEVEL_COLORS[level])
    w["mood"].color(LEVEL_COLORS[level])
    w["mood"].text(("แห้งดี", "เริ่มชื้น: ", "เสี่ยงรา!: ")[level] + ("" if level == 0 else why_th))
    w["mm"].text("ต่ำสุด %d  สูงสุด %d %%" % (int(s.rh_min), int(s.rh_max)))
    w["temp"].text("%.1f C  แดด +%d" % (t, sun))
    w["wet"].text("%02d.%02d" % (s.wet_s // 60 % 100, s.wet_s % 60))
    w["wet"].color(COL_GRAIN if s.wet_s == 0 else COL_BAD)
    w["chart"].set_next(0, int(max(0, min(100, h))))
    w["chart"].set_next(w["s_hi"], s.lim[3])  # เส้นแดง = RH สูง


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
        self.wet_s = 0                       # วินาทีที่ RH เกินเกณฑ์สะสม
        self.rh_min, self.rh_max = 100.0, 0.0


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
                 "wet_s": s.wet_s, "rh_max": round(s.rh_max, 1)}):
        stop(w, "สายหลุดตอนส่ง")
    rgbmatrix.scroll(MX[level][0], MX[level][1], 80)
    beep(("good", "tap", "empty")[level])
    show_ack(w, s, "รอคนงาน" if level == 2 else "ส่งแล้ว", COL_BAD if level == 2 else COL_DIM)
    close_box(w)
    if level == 2:  # MsgBox รับไม่เกิน 126 ไบต์
        w["box"] = ui.MsgBox("เสี่ยงรา!\n" + why_th + "\nเปิดพัดลม กด " + BTN_NAMES[1],
                             x=BOX[0], y=BOX[1], w=BOX[2], h=BOX[3], value=0)


def read_and_judge(w, s):
    # คืน True ถ้าเพิ่งแจ้ง
    sun = pots.read(2) * 10 // 4095  # VR3 = แดดจำลอง 0-10 C ที่บวกเข้าอุณหภูมิ
    t, h = read_climate()
    if t is None or h is None:
        s.t = None  # อ่านไม่ได้ = ลองใหม่รอบหน้าเลย
        return False
    s.t = t = t + sun
    s.rh_min, s.rh_max = min(s.rh_min, h), max(s.rh_max, h)
    if h > s.lim[3]:
        s.wet_s += READ_MS // 1000  # อ่านทุก 1 วิ
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
    show_status(w, "เฝ้าไซโลอยู่", COL_OK)
    was_down = False
    t_read = t_beep = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        msg = mqtt.get_message()  # ฟังทุก 20 ms อ่านเซนเซอร์ทุกวินาที
        ack = on_command(w, s, msg[1]) if msg else False
        down = buttons.pressed(1)  # SW5 = คนงานรับทราบ ไม่พึ่งเน็ต
        if down and not was_down:  # นับตอนเพิ่งกดลง
            ack = True
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
# 1) แจ้งเฉพาะเมื่อชื้นติดกัน 5 วินาที (นับใน Watch) เปิดประตูไซโลแป๊บเดียวไม่ควรเตือน
# 2) ให้ {"cmd":"set"} รับ "rh_hi" ด้วย (ตอนนี้รับแค่ t_hi) แล้วบอกฝั่งเว็บ
# 3) ฝั่งเว็บ: วาดแนวโน้ม rh จาก event หลายใบ ว่าชื้นขึ้นเรื่อย ๆ ไหม
