# idea_11_longan_dryer.py - โรงอบลำไย: ห้องอบร้อนเกิน ลำไยไหม้ มือถือรู้ทันที
# แม่แบบ A (idea_3_silo.py) โครงเน็ตเดิม หน้าจอใหม่
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# หน้าจอ  : เสาปรอทตั้ง (Bar) ชิดขวา + แถบ "เตาตอนนี้" เลขใหญ่ Seg7 + กราฟบันทึกเตาทั้งคืนกว้างเต็มล่าง
# คิดบนบอร์ด: 1) เวลาอบพอดีสะสม 2) ร้อนสุดตั้งแต่เริ่ม
# MQTT: หัวข้อ/คีย์เหมือน sf2_04 (สัญญา 3.4, 4) + คีย์เพิ่ม 2 ตัวใน event:
#       "peak_c" = ร้อนสุดตั้งแต่เปิดบอร์ด (C)  "bake_min" = นาทีที่อบพอดีสะสม
#       crop: whole golden dried  คำสั่ง: ack, set t_hi (เหมือนเดิม)
# เกณฑ์ = ค่าสาธิตในห้อง 25-30 C  เตาอบจริงร้อนกว่านี้มาก ห้ามใช้ค่าในไฟล์นี้กับเตาจริง
#
# ลองเล่น  : หมุน VR3 (ไฟจากเตา) จนขึ้นเตือน แล้วส่ง {"cmd":"ack"} หรือกด SW5
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
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"   # อังกฤษ/ตัวเลข ไม่มีเว้นวรรค
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"   # อย่างน้อย 8 ตัว
TEAM = "teamXX"
CROP = "ลำไยทั้งลูก"
TEMP_OFFSET = 0.0    # บอร์ดอ่านสูงกว่าห้อง: ใส่ค่าชดเชย เช่น -7.0

BROKER = "broker.hivemq.com"
ROOT = "bento-aiot"
CLIENT_ID = "bento-farm-" + TEAM
TOPIC_EVENT = ROOT + "/" + TEAM + "/event"
TOPIC_CMD = ROOT + "/" + TEAM + "/cmd"
BTN_NAMES = ("SW6", "SW5")

# (ชื่อไทย, รหัสอังกฤษ, T ต่ำ, T สูง, RH ต่ำ, RH สูง)
# ค่าสาธิต: ห้อง 27 C = ปกติ  VR3 ครึ่งรอบ (+5) = เริ่มร้อน  เกิน T สูง + 3 = ร้อนเกิน
CROPS = (
    ("ลำไยทั้งลูก", "whole", 15, 32, 0, 85),
    ("ลำไยสีทอง", "golden", 15, 31, 0, 85),
    ("ลำไยแห้ง", "dried", 15, 30, 0, 80),
)
T_HI_MAX = 45
READ_MS, BEEP_MS, POLL_MS, RUN_MS = 1000, 5000, 20, 600000
THERMO_LO, THERMO_HI = 20, 50    # ช่วงของแท่งเทอร์โมมิเตอร์ (C)
MX = (("BAKE", rgbmatrix.GREEN), ("WARN", rgbmatrix.YELLOW), ("ALARM", rgbmatrix.RED))
BOX = (212, 90, 368, 150)

SPEAKER = 40
VOLUME = 25
COL_TEXT, COL_DIM = 0xFFF1E0, 0xB89F86
COL_CARD, COL_EDGE = 0x24170F, 0x6B4A2E
COL_OK, COL_WARN, COL_BAD, COL_GOLD = 0x9BC53D, 0xF08A24, 0xD7263D, 0xF4C95D
LEVEL_COLORS = (COL_OK, COL_WARN, COL_BAD)
TRACK = (0x3A4A1C, 0x5A3410, 0x5A1018)   # สีรางปรอทตามระดับ


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
    # คืน (ระดับ 0-2, ไทย, อังกฤษ)  2 = หลุดไกล (เกิน 3 C หรือ 10 %RH)
    t_lo, t_hi, h_lo, h_hi = lim
    p = []
    if t < t_lo:
        p.append(("เตาเย็น", "cold"))
    if t > t_hi:
        p.append(("เตาร้อน", "hot"))
    if h < h_lo:
        p.append(("แห้งไป", "dry"))
    if h > h_hi:
        p.append(("ชื้นค้าง", "wet"))
    if not p:
        return 0, "อบพอดี", "ok"
    far = (t < t_lo - 3) or (t > t_hi + 3) or (h < h_lo - 10) or (h > h_hi + 10)
    return (2 if far else 1), " + ".join(x[0] for x in p), "+".join(x[1] for x in p)


def mmss(sec):
    return "%02d:%02d" % (sec // 60, sec % 60)


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
    # WiFi -> IP -> broker + subscribe  พังขั้นไหนบอกขั้นนั้น
    show_status(w, "ต่อ WiFi...", COL_WARN)
    ui.poll()
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "WiFi ไม่ติด: ตรวจชื่อ/รหัส"
    linked = connect_broker(w)
    if not linked or not mqtt.subscribe(TOPIC_CMD):     # subscribe หลัง connect เสมอ
        return "broker ไม่ตอบ: รอ 1 นาทีแล้วรันใหม่"
    return ""


def send(obj):
    try:
        mqtt.publish(TOPIC_EVENT, json.dumps(obj))
        return True
    except OSError:
        return False


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_EDGE, max=10, value=1)
    ui.Label(title, x=x + 10, y=y + 6, color=COL_GOLD, value=16)


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: ต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("โรงอบลำไย  เฝ้าเตาแทนคนทั้งคืน", x=12, y=4, color=COL_GOLD, value=24)
    w = {"box": None, "leds": []}
    # แถบบน: เตาตอนนี้ (เลขใหญ่ + ไฟ 3 ดวง + รับทราบ)
    card(12, 40, 568, 118, "เตาตอนนี้")
    w["seg"] = ui.Seg7(text="--.-", x=24, y=66, w=150, h=64, color=COL_GOLD)
    ui.Label("C", x=128, y=84, color=COL_DIM, value=24)
    w["mood"] = ui.Label("...", x=222, y=64, color=COL_WARN, value=16)
    w["rh"] = ui.Label("ชื้น -- %", x=222, y=92, color=COL_DIM, value=16)
    w["fire"] = ui.Label("ไฟเตา +0", x=340, y=92, color=COL_DIM, value=16)
    for i in range(3):
        x = 222 + i * 76
        w["leds"].append(ui.Led(x=x, y=126, w=18, h=18, color=LEVEL_COLORS[i]))
        ui.Label(("พอดี", "ระวัง", "อันตราย")[i], x=x + 22, y=126, color=COL_DIM, value=14)
    w["ack"] = ui.Label("-", x=456, y=116, color=COL_DIM, value=14)
    # แถบล่าง: บันทึกเตาทั้งคืน (กราฟกว้าง) + ตัวเลขที่บอร์ดคิดเอง
    card(12, 166, 568, 174, "บันทึกเตาทั้งคืน")
    w["chart"] = ui.Chart(x=24, y=192, w=544, h=100, color=COL_WARN, min=THERMO_LO, max=THERMO_HI)
    w["chart"].prop(ui.PROP_CHART_POINTS, 400)
    w["s_hi"] = w["chart"].add_series(COL_BAD)
    w["bake"] = ui.Label("อบพอดีสะสม 00:00", x=24, y=298, color=COL_TEXT, value=16)
    w["peak"] = ui.Label("ร้อนสุด -- C", x=220, y=298, color=COL_TEXT, value=16)
    ui.Label("ส้ม = ห้องอบ  แดง = เกณฑ์ไหม้", x=376, y=300, color=COL_DIM, value=14)
    ui.Label("VR3 = ไฟจากเตา  " + BTN_NAMES[1] + " = รับทราบ", x=24, y=320, color=COL_DIM, value=14)
    # เสาขวา: ปรอทห้องอบ ตั้งสูงข้างเตา
    card(590, 40, 190, 300, "ปรอทห้องอบ")
    w["thermo"] = ui.Bar(x=610, y=70, w=40, h=210, min=THERMO_LO, max=THERMO_HI, value=THERMO_LO)
    pass  # Bar.color() ตั้งสีส่วนที่เติมบนบอร์ดจริง ใช้สีเดิมของ Bar ให้เห็นชัด
    for i in range(4):                 # ขีดบอกค่าข้างแท่ง 50 40 30 20
        ui.Label("%d C" % (THERMO_HI - i * 10), x=662, y=62 + i * 70, color=COL_DIM, value=14)
    w["limit"] = ui.Label("-", x=602, y=290, color=COL_BAD, value=14)
    w["crop"] = ui.Label("-", x=602, y=312, color=COL_DIM, value=14)
    w["status"] = ui.Label("กำลังเริ่ม", x=12, y=356, color=COL_DIM)
    w["mq"] = ui.Led(x=600, y=10, w=16, h=16, color=COL_OK, value=0)
    w["mq_t"] = ui.Label("MQTT: ออฟไลน์", x=624, y=8, color=COL_DIM, value=16)
    ui.poll()
    return w


def show_status(w, msg, col):
    w["status"].color(col)
    w["status"].text(msg)


def show_ack(w, s, msg, col):
    w["ack"].color(col)
    w["ack"].text("แจ้งแล้ว %d ครั้ง\n%s" % (s.alerts, msg))


def show_crop(w, s):
    w["crop"].text(s.crop[0])
    w["limit"].text("ไหม้เมื่อเกิน %d C" % s.lim[1])


def show_reading(w, s, t, h, fire, level, why_th):
    col = LEVEL_COLORS[level]
    w["thermo"].value(int(max(THERMO_LO, min(THERMO_HI, t))))
    pass  # Bar.color() ตั้งสีส่วนที่เติมบนบอร์ดจริง ใช้สีเดิมของ Bar ให้เห็นชัด
    w["seg"].text("%.1f" % t)
    w["rh"].text("ชื้น %d %%" % h)
    w["fire"].text("ไฟเตา +%d" % fire)
    w["mood"].color(col)
    w["mood"].text(("อบพอดี ลำไยสีสวย", "ระวัง: ", "อันตราย: ")[level] + ("" if level == 0 else why_th))
    for i in range(3):
        w["leds"][i].value(1 if i == level else 0)
    w["bake"].text("อบพอดีสะสม " + mmss(s.bake_s))
    w["peak"].text("ร้อนสุด %.1f C" % s.peak)
    w["chart"].set_next(0, int(max(THERMO_LO, min(THERMO_HI, t))))
    w["chart"].set_next(w["s_hi"], s.lim[1])


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
        self.bake_s = 0                # วินาทีที่อบพอดี (ระดับ 0)
        self.peak = -99.0


class Stop(Exception):
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
    # คำสั่งจากแอป (สัญญาข้อ 4) คืน True ถ้ารับทราบ  คำสั่งที่ไม่รู้จัก = เงียบ
    try:                               # ไม่ใช่ JSON object ก็ไม่ใช้
        cmd = json.loads(raw.decode())
        act = cmd.get("cmd")
    except Exception:
        return False
    if act == "ack":
        return True
    elif act == "set":                 # เกณฑ์ใหม่ ต้องเป็นจำนวนเต็มในช่วงที่สมเหตุผล
        v = cmd.get("t_hi")
        if isinstance(v, int) and s.lim[0] < v <= T_HI_MAX:
            s.lim[1], s.level, s.t = v, -1, None     # ตัดสินใหม่และแจ้งทันที
            show_crop(w, s)
            beep("tap")
    # >>> ภารกิจกลุ่ม: วาง elif "led"/"pump" จาก sf2_03 ตรงนี้ (เช่น พัดลมระบาย) <<<
    return False


def on_level(w, s, level, why_th, why_en, t, h, fire):
    # ระดับเปลี่ยน: ส่งออกเน็ตก่อน (สัญญาข้อ 3.4) แล้วเสียง จอไฟ RGB กล่องเตือน
    s.level, s.acked = level, level < 2
    s.alerts += 1
    if not send({"id": TEAM, "crop": s.crop[1], "level": level, "alert": why_en,
                 "temp_c": round(t, 1), "rh": round(h, 1), "sun_c": fire,
                 "peak_c": round(s.peak, 1), "bake_min": s.bake_s // 60}):
        stop(w, "สายหลุดตอนส่ง")
    rgbmatrix.scroll(MX[level][0], MX[level][1], 80)
    beep(("good", "tap", "empty")[level])
    show_ack(w, s, "รอคนคุมเตารับทราบ" if level == 2 else "ส่งแล้ว", COL_BAD if level == 2 else COL_DIM)
    close_box(w)
    if level == 2:
        head = "ลดไฟเตาด่วน!" if "hot" in why_en else "ตรวจเตาด่วน!"   # เตาเย็นไม่ใช่ลดไฟ
        w["box"] = ui.MsgBox(head + "\n" + why_th + "\n" + BTN_NAMES[1] + " / ack = รับทราบ",
                             x=BOX[0], y=BOX[1], w=BOX[2], h=BOX[3], value=0)


def read_and_judge(w, s):
    # อ่าน -> ตัดสิน -> โชว์ -> แจ้งถ้าเปลี่ยน  คืน True ถ้าเพิ่งแจ้ง
    fire = pots.read(2) * 10 // 4095   # VR3 = ไฟเตาจำลอง +0..10 C
    t, h = read_climate()
    if t is None or h is None:
        s.t = None                     # อ่านไม่ได้ = ลองใหม่รอบหน้าเลย
        return False
    s.t = t = t + fire
    s.peak = max(s.peak, t)
    level, why_th, why_en = judge(t, h, s.lim)
    if level == 0:
        s.bake_s += READ_MS // 1000
    show_reading(w, s, t, h, fire, level, why_th)
    if level == s.level:               # ส่ง/เสียง/จอไฟ RGB เฉพาะตอนเปลี่ยน
        return False
    on_level(w, s, level, why_th, why_en, t, h, fire)
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
    show_status(w, "เฝ้าเตาอยู่", COL_OK)
    was_down = False
    t_read = t_beep = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        msg = mqtt.get_message()       # ฟังทุก 20 ms อ่านเซนเซอร์ทุกวินาที
        ack = on_command(w, s, msg[1]) if msg else False
        down = buttons.pressed(1)      # SW5 = รับทราบหน้าเตา ไม่ต้องพึ่งเน็ต
        if down and not was_down:      # กดค้างไม่นับซ้ำ
            ack = True
        was_down = down
        if ack and not s.acked:        # รับทราบ: หยุดร้อง ปิดกล่อง จอไฟ ACK
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

    stop(w, "จบรอบ อบพอดีสะสม " + mmss(s.bake_s), COL_DIM)


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
# 1) แจ้งเฉพาะเมื่อร้อนติดกัน 5 วินาที (นับใน Watch) - เติมฟืนแป๊บเดียวไม่ควรเตือน
# 2) ตั้งเป้า "อบพอดีครบ 30 นาที" แล้วนับถอยหลังบนจอ ครบแล้วแจ้งเว็บว่ากลับลำไยได้
# 3) ส่ง {"cmd":"set","t_hi":29} จากเว็บ ดูเส้นแดงขยับ แล้วให้ set รับ rh_hi ด้วย
