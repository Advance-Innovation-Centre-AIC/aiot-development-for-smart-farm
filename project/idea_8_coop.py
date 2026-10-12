# idea_8_coop.py - เล้าไก่กันเครียดฝูง: เข็มดัชนีร้อนชื้น (THI) และเวลาเครียดสะสม
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# ต่อยอดจาก s2/sf2_04_crop_alert.py (WiFi MQTT ack set เหมือนเดิม)
# จอ: หน้าปัดเข็ม (Scale) ดัชนี THI + แถบเครียดสะสม (Bar) ธีมโรงนาไม้
# คิดบนบอร์ด: 1) THI = 0.8 T + (RH/100)(T - 14.4) + 46.4 (สูตรปศุสัตว์ทั่วไป ใช้สาธิต)
#             2) เวลาเครียดสะสม = วินาทีที่ระดับ 1 ขึ้นไป เทียบ STRESS_MAX_S
# MQTT: คีย์ sf2_04 ครบ + "thi" (ดัชนี 1 ตำแหน่ง) "stress_s" (วินาทีเครียดสะสม)
# crop: layer broiler chick  เกณฑ์ = ค่าสาธิตในห้อง 25-30 C ไม่ใช่คำแนะนำทางสัตวบาล
# ลองเล่น: หมุน VR3 ดูเข็มขยับ หรือเป่าลมหายใจ แล้วกด SW5 หรือส่ง {"cmd":"ack"}
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
CROP = "ไก่ไข่"
TEMP_OFFSET = 0.0    # บอร์ดอ่านสูงกว่าห้อง: ใส่ค่าชดเชย เช่น -7.0

BROKER = "broker.hivemq.com"
ROOT = "bento-aiot"
CLIENT_ID = "bento-farm-" + TEAM  # + เลขสุ่มตอนต่อ: ไม่ชน id เก่า
TOPIC_EVENT = ROOT + "/" + TEAM + "/event"
TOPIC_CMD = ROOT + "/" + TEAM + "/cmd"
BTN_NAMES = ("SW6", "SW5")

# (ชื่อไทย, รหัสอังกฤษ, T ต่ำ, T สูง, RH ต่ำ, RH สูง)  ห้อง 27 C = ปกติ  VR3 ครึ่งรอบ (+5) = เครียด
CROPS = (
    ("ไก่ไข่", "layer", 18, 30, 30, 75),
    ("ไก่เนื้อ", "broiler", 18, 29, 30, 75),
    ("ลูกเจี๊ยบ", "chick", 22, 32, 30, 75),
)
T_HI_MAX = 45
READ_MS, BEEP_MS, POLL_MS, RUN_MS = 1000, 5000, 20, 600000
THI_LO, THI_HI = 60, 100               # หน้าปัด THI
THI_ZONES = ((74, "สบาย"), (79, "เฝ้าระวัง"), (84, "อันตราย"), (999, "วิกฤต"))
STRESS_MAX_S = 300                     # แถบเต็ม = เครียดสะสม 5 นาที (ค่าสาธิต)
NEEDLE_LEN = 70
MX = (("OK", rgbmatrix.GREEN), ("STRESS", rgbmatrix.YELLOW), ("HEAT", rgbmatrix.RED))
BOX = (282, 96, 360, 140)

SPEAKER = 40
VOLUME = 25
# ธีมโรงนา: ไม้แดงเข้ม ฟางเหลือง
COL_TEXT, COL_DIM = 0xFFF1E0, 0xC9A98C
COL_CARD, COL_EDGE = 0x2B1A14, 0x6B3A2A
COL_STRAW, COL_WARN, COL_BAD = 0xF2C14E, 0xF28C28, 0xE4572E
COL_OK = 0x7BC47F
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
        p.append(("เล้าหนาว", "cold"))
    if t > t_hi:
        p.append(("เล้าร้อน", "hot"))
    if h < h_lo:
        p.append(("ฝุ่นแห้ง", "dry"))
    if h > h_hi:
        p.append(("เล้าชื้น", "wet"))
    if not p:
        return 0, "ไก่สบาย", "ok"
    far = (t < t_lo - 3) or (t > t_hi + 3) or (h < h_lo - 10) or (h > h_hi + 10)
    return (2 if far else 1), " + ".join(x[0] for x in p), "+".join(x[1] for x in p)


def thi_of(t, h):
    # ร้อนเท่ากัน แต่ชื้นกว่า = ไก่ระบายความร้อนยากกว่า
    return 0.8 * t + (h / 100) * (t - 14.4) + 46.4


def thi_zone(thi):
    # คืนลำดับโซน 0-3 ของ THI_ZONES
    for i in range(len(THI_ZONES)):
        if thi < THI_ZONES[i][0]:
            return i
    return len(THI_ZONES) - 1


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
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_EDGE, max=4, value=2)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_STRAW, value=16)


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: ต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def set_needle(scale, value):
    # Scale ไม่มีเข็มในตัว: ความยาวเข็ม 16 บิตบน ค่าที่ชี้ 16 บิตล่าง
    scale.prop(ui.PROP_SCALE_NEEDLE, (NEEDLE_LEN << 16) | (int(value) & 0xFFFF))


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("เล้าไก่: ร้อนชื้นจนไก่เครียดไหม", x=12, y=6, color=COL_TEXT, value=24)
    w = {"box": None, "leds": []}
    card(12, 44, 260, 300, "ดัชนีร้อนชื้น THI")
    g = ui.Scale(x=42, y=74, w=200, h=200, color=COL_TEXT, min=THI_LO, max=THI_HI)
    g.prop(ui.PROP_SCALE_MODE, ui.SCALE_ROUND_IN)
    g.ticks(9, 2)
    g.prop(ui.PROP_SCALE_NEEDLE_COLOR, COL_STRAW)
    w["gauge"] = g
    w["thi"] = ui.Seg7(text="--", x=24, y=284, w=110, h=48, color=COL_STRAW)
    w["zone"] = ui.Label("-", x=146, y=296, color=COL_DIM, value=20)
    card(282, 44, 300, 190, "ในเล้า")
    w["crop"] = ui.Label("-", x=294, y=70, color=COL_DIM, value=14)
    w["th"] = ui.Label("-- C  -- %", x=294, y=94, color=COL_TEXT, value=28)
    w["mood"] = ui.Label("...", x=294, y=136, color=COL_WARN)
    for i in range(3):
        lx = 294 + i * 96
        w["leds"].append(ui.Led(x=lx, y=196, w=18, h=18, color=LEVEL_COLORS[i]))
        ui.Label(("สบาย", "เครียด", "หนัก")[i], x=lx + 24, y=196, color=COL_DIM, value=14)
    card(592, 44, 188, 190, "แจ้งคนเลี้ยง")
    w["ack"] = ui.Label("-", x=604, y=74, color=COL_DIM, value=16)
    ui.Label("VR3 = แดดบนหลังคา\n" + BTN_NAMES[1] + " = รับทราบ", x=604, y=170, color=COL_DIM, value=14)
    card(282, 244, 420, 100, "ไก่เครียดสะสม")
    w["stress"] = ui.Bar(x=294, y=280, w=396, h=22, min=0, max=STRESS_MAX_S, value=0)
    w["stress_t"] = ui.Label("0 วิ จาก %d" % STRESS_MAX_S, x=294, y=310, color=COL_DIM, value=14)
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
    w["ack"].text("แจ้งแล้ว %d ครั้ง\n%s" % (s.alerts, msg))


def show_crop(w, s):
    t_lo, t_hi, h_lo, h_hi = s.lim
    w["crop"].text("%s สบายที่ %d-%d C, %d-%d %%" % (s.crop[0], t_lo, t_hi, h_lo, h_hi))


def show_reading(w, s, t, h, sun, level, why_th):
    thi = s.thi
    set_needle(w["gauge"], max(THI_LO, min(THI_HI, thi)))
    w["thi"].text("%d" % int(thi))
    w["thi"].color(LEVEL_COLORS[min(2, s.zone)])
    w["zone"].text(THI_ZONES[s.zone][1])
    w["zone"].color(LEVEL_COLORS[min(2, s.zone)])
    w["th"].text("%.1f C  %d %%" % (t, int(h)))
    w["mood"].color(LEVEL_COLORS[level])
    w["mood"].text(("ไก่สบายดี", "เริ่มเครียด: ", "เครียดหนัก!: ")[level] + ("" if level == 0 else why_th))
    for i in range(3):
        w["leds"][i].value(1 if i == level else 0)
    w["stress"].value(min(STRESS_MAX_S, s.stress_s))
    if s.stress_s >= STRESS_MAX_S:  # เต็มแล้ว: แถบเป็นสีแดง
        w["stress"].color(COL_BAD)  # Bar: color() = สีส่วนที่เติม (แบบ sf1_03) ใน Emulator จะเห็นเป็นสีราง
    w["stress_t"].text("%d วิ จาก %d  (แดด +%d C)" % (s.stress_s, STRESS_MAX_S, sun))


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
        self.thi, self.zone = 0.0, 0
        self.stress_s = 0              # วินาทีที่ระดับ 1 ขึ้นไป สะสม


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
                 "thi": round(s.thi, 1), "stress_s": s.stress_s}):
        stop(w, "สายหลุดตอนส่ง")
    rgbmatrix.scroll(MX[level][0], MX[level][1], 80)
    beep(("good", "tap", "empty")[level])
    show_ack(w, s, "รอคนเลี้ยง" if level == 2 else "ส่งแล้ว", COL_BAD if level == 2 else COL_DIM)
    close_box(w)
    if level == 2:  # MsgBox รับไม่เกิน 126 ไบต์
        w["box"] = ui.MsgBox("เครียดหนัก!\n" + why_th + "\nเปิดพัดลม กด " + BTN_NAMES[1],
                             x=BOX[0], y=BOX[1], w=BOX[2], h=BOX[3], value=0)


def read_and_judge(w, s):
    # คืน True ถ้าเพิ่งแจ้ง
    sun = pots.read(2) * 10 // 4095  # VR3 = แดดจำลอง 0-10 C ที่บวกเข้าอุณหภูมิ
    t, h = read_climate()
    if t is None or h is None:
        s.t = None  # อ่านไม่ได้ = ลองใหม่รอบหน้าเลย
        return False
    s.t = t = t + sun
    s.thi = thi_of(t, h)
    s.zone = thi_zone(s.thi)
    level, why_th, why_en = judge(t, h, s.lim)
    if level > 0:
        s.stress_s += READ_MS // 1000  # อ่านทุก 1 วิ
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
    show_status(w, "เฝ้าเล้าอยู่", COL_OK)
    was_down = False
    t_read = t_beep = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        msg = mqtt.get_message()  # ฟังทุก 20 ms อ่านเซนเซอร์ทุกวินาที
        ack = on_command(w, s, msg[1]) if msg else False
        down = buttons.pressed(1)  # SW5 = คนเลี้ยงรับทราบ ไม่พึ่งเน็ต
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
# 1) ตัดสินระดับจาก THI แทน T/RH แยกกัน (ดู THI_ZONES) แล้วเทียบว่าเตือนต่างกันไหม
# 2) นับเสียงดังในเล้าจากไมค์ (ดู s3/sf3_02_coop_ears.py)
# 3) เพิ่มชนิดไก่ใน CROPS
