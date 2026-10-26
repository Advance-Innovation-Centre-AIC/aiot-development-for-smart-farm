# idea_12_seed_store.py - โกดังเก็บเมล็ดพันธุ์: ความชื้นสูงเกิน ความงอกลด มือถือรู้ทันที
# แม่แบบ A (idea_3_silo.py) โครงเน็ตเดิม หน้าจอใหม่
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# หน้าจอ  : บัตรล็อตเมล็ด (แถบคะแนนความงอก) ทางซ้าย + หน้าปัดเข็มความชื้น (Scale) ทางขวา
# คิดบนบอร์ด: 1) คะแนนความงอก ลดทุกวินาทีที่หลุดเกณฑ์ 2) เวลาชื้นเกินสะสม
# MQTT: หัวข้อ/คีย์เหมือน sf2_04 (สัญญา 3.4, 4) + คีย์เพิ่ม 2 ตัวใน event:
#       "germ_pct" = คะแนนความงอก 0-100 (ค่าสาธิต ไม่ใช่ผลเพาะ)  "wet_min" = นาทีที่ชื้นเกิน
#       crop: rice corn bean  คำสั่ง: ack, set t_hi (เหมือนเดิม)  ไม่มี alert "dry"
# เกณฑ์ = ค่าสาธิตในห้องเรียน โกดังเมล็ดจริงต้องแห้งกว่านี้ (ห้องชื้นเกิน 65 % ให้ขยับ RH สูงขึ้น)
#
# ลองเล่น  : เป่าลมหายใจใส่บอร์ด (ความชื้นขึ้น) จนขึ้นเตือน แล้วส่ง {"cmd":"ack"} หรือกด SW5
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
CROP = "เมล็ดข้าว"
TEMP_OFFSET = 0.0    # บอร์ดอ่านสูงกว่าห้อง: ใส่ค่าชดเชย เช่น -7.0

BROKER = "broker.hivemq.com"
ROOT = "bento-aiot"
CLIENT_ID = "bento-farm-" + TEAM
TOPIC_EVENT = ROOT + "/" + TEAM + "/event"
TOPIC_CMD = ROOT + "/" + TEAM + "/cmd"
BTN_NAMES = ("SW6", "SW5")

# (ชื่อไทย, รหัสอังกฤษ, T ต่ำ, T สูง, RH ต่ำ, RH สูง)
# ค่าสาธิต: ห้อง 60 % = ปกติ  เป่าลมหายใจ = เริ่มชื้น  เกิน RH สูง + 10 = ชื้นเกิน
CROPS = (
    ("เมล็ดข้าว", "rice", 15, 35, 0, 65),
    ("เมล็ดข้าวโพด", "corn", 15, 35, 0, 65),
    ("เมล็ดถั่ว", "bean", 15, 35, 0, 65),
)
T_HI_MAX = 45
READ_MS, BEEP_MS, POLL_MS, RUN_MS = 1000, 5000, 20, 600000
GERM_LOSS = (0, 0.1, 0.3)   # แต้มที่หายต่อวินาทีตามระดับ (เร่งให้เห็นในคาบเรียน)
NEEDLE_LEN = 80
MX = (("SAFE", rgbmatrix.GREEN), ("WATCH", rgbmatrix.YELLOW), ("RISK", rgbmatrix.RED))
BOX = (212, 90, 368, 150)

SPEAKER = 40
VOLUME = 25
COL_TEXT, COL_DIM = 0xF3EEDC, 0xA8A68C
COL_CARD, COL_EDGE = 0x18231A, 0x3F5A3A
COL_OK, COL_WARN, COL_BAD, COL_SEED = 0x6CCB5F, 0xE9B949, 0xE5604D, 0xE9D8A6
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
    # คืน (ระดับ 0-2, ไทย, อังกฤษ)  2 = หลุดไกล (เกิน 3 C หรือ 10 %RH)
    t_lo, t_hi, h_lo, h_hi = lim
    p = []
    if t < t_lo:
        p.append(("หนาวไป", "cold"))
    if t > t_hi:
        p.append(("ร้อนไป", "hot"))
    if h < h_lo:
        p.append(("แห้งไป", "dry"))
    if h > h_hi:
        p.append(("ชื้นเกิน", "wet"))
    if not p:
        return 0, "แห้งดี", "ok"
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
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_EDGE, max=6, value=2)
    ui.Label(title, x=x + 10, y=y + 6, color=COL_SEED, value=16)


def set_needle(scale, value):
    # ความยาวเข็ม 16 บิตบน ค่าที่ชี้ 16 บิตล่าง (เหมือน sf1_01)
    scale.prop(ui.PROP_SCALE_NEEDLE, (NEEDLE_LEN << 16) | (int(value) & 0xFFFF))


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: ต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("โกดังเมล็ดพันธุ์  ความงอกต้องไม่หาย", x=12, y=4, color=COL_SEED, value=24)
    w = {"box": None, "leds": []}
    card(490, 40, 290, 300, "ความชื้นในโกดัง (%RH)")
    dial = ui.Scale(x=534, y=70, w=200, h=200, color=COL_TEXT, min=0, max=100)
    dial.prop(ui.PROP_SCALE_MODE, ui.SCALE_ROUND_IN)
    dial.ticks(11, 2)
    dial.prop(ui.PROP_SCALE_NEEDLE_COLOR, COL_OK)
    w["dial"] = dial
    w["seg"] = ui.Seg7(text="--", x=512, y=282, w=100, h=44, color=COL_SEED)
    w["temp"] = ui.Label("-- C", x=632, y=280, color=COL_DIM, value=16)
    w["limit"] = ui.Label("-", x=632, y=304, color=COL_BAD, value=14)
    card(12, 40, 468, 160, "ล็อตเมล็ดในโกดัง")
    w["crop"] = ui.Label("-", x=24, y=68, color=COL_TEXT, value=16)
    w["germ"] = ui.Bar(x=24, y=100, w=330, h=24, min=0, max=100, value=100)
    pass  # Bar.color() ตั้งสีส่วนที่เติมบนบอร์ดจริง ใช้สีเดิมของ Bar ให้เห็นชัด
    w["germ_t"] = ui.Label("100", x=366, y=100, color=COL_OK, value=24)
    ui.Label("คะแนนความงอก (ค่าสาธิต)", x=24, y=130, color=COL_DIM, value=14)
    w["wet"] = ui.Label("ชื้นเกินสะสม 00:00", x=24, y=160, color=COL_TEXT, value=16)
    card(12, 210, 300, 110, "แจ้งเจ้าของโกดัง")
    for i in range(3):
        y = 240 + i * 24
        w["leds"].append(ui.Led(x=24, y=y, w=16, h=16, color=LEVEL_COLORS[i]))
        ui.Label(("ปลอดภัย", "เริ่มเสี่ยง", "อันตราย")[i], x=46, y=y - 2, color=COL_DIM, value=14)
    w["ack"] = ui.Label("-", x=140, y=240, color=COL_DIM, value=16)
    w["mood"] = ui.Label("...", x=12, y=328, color=COL_WARN, value=16)
    ui.Label("เป่าลม = ชื้นขึ้น\nVR3 = แดดบนหลังคา\n" + BTN_NAMES[1] + " = รับทราบ", x=324, y=214, color=COL_DIM, value=14)
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
    w["crop"].text("%s เก็บที่ไม่เกิน %d %%RH, %d C" % (s.crop[0], s.lim[3], s.lim[1]))
    w["limit"].text("ชื้นเกิน %d %%" % s.lim[3])


def show_reading(w, s, t, h, sun, level, why_th):
    col = LEVEL_COLORS[level]
    set_needle(w["dial"], max(0, min(100, h)))
    w["dial"].prop(ui.PROP_SCALE_NEEDLE_COLOR, col)
    w["seg"].text("%d" % h)
    w["temp"].text("%.1f C (แดด +%d)" % (t, sun))
    g = int(s.germ)
    w["germ"].value(g)
    w["germ_t"].color(COL_OK if g >= 80 else (COL_WARN if g >= 50 else COL_BAD))
    w["germ_t"].text(str(g))
    w["wet"].text("ชื้นเกินสะสม " + mmss(s.wet_s))
    w["mood"].color(col)
    w["mood"].text(("แห้งดี เมล็ดปลอดภัย", "เริ่มเสี่ยง: ", "ความงอกลด: ")[level] + ("" if level == 0 else why_th))
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
        self.germ = 100.0              # เริ่ม 100 ทุกครั้งที่เปิดบอร์ด
        self.wet_s = 0                 # วินาทีที่ RH เกินเกณฑ์


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
    try:
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
    # >>> ภารกิจกลุ่ม: วาง elif "led"/"pump" จาก sf2_03 ตรงนี้ <<<
    return False


def on_level(w, s, level, why_th, why_en, t, h, sun):
    # ระดับเปลี่ยน: ส่งก่อน (สัญญาข้อ 3.4) แล้วเสียง จอไฟ กล่องเตือน
    s.level, s.acked = level, level < 2
    s.alerts += 1
    if not send({"id": TEAM, "crop": s.crop[1], "level": level, "alert": why_en,
                 "temp_c": round(t, 1), "rh": round(h, 1), "sun_c": sun,
                 "germ_pct": int(s.germ), "wet_min": s.wet_s // 60}):
        stop(w, "สายหลุดตอนส่ง")
    rgbmatrix.scroll(MX[level][0], MX[level][1], 80)
    beep(("good", "tap", "empty")[level])
    show_ack(w, s, "รอเจ้าของรับทราบ" if level == 2 else "ส่งแล้ว", COL_BAD if level == 2 else COL_DIM)
    close_box(w)
    if level == 2:  # MsgBox รับไม่เกิน 126 ไบต์
        w["box"] = ui.MsgBox("เมล็ดเสี่ยง!\n" + why_th + "\n" + BTN_NAMES[1] + " / ack = รับทราบ",
                             x=BOX[0], y=BOX[1], w=BOX[2], h=BOX[3], value=0)


def read_and_judge(w, s):
    # อ่าน -> ตัดสิน -> โชว์ -> แจ้งถ้าเปลี่ยน  คืน True ถ้าเพิ่งแจ้ง
    sun = pots.read(2) * 10 // 4095    # VR3 = แดดบนหลังคาโกดัง +0..10 C
    t, h = read_climate()
    if t is None or h is None:
        s.t = None                     # อ่านไม่ได้ = ลองใหม่รอบหน้าเลย
        return False
    s.t = t = t + sun
    level, why_th, why_en = judge(t, h, s.lim)
    if h > s.lim[3]:
        s.wet_s += READ_MS // 1000
    s.germ = max(0.0, s.germ - GERM_LOSS[level])
    show_reading(w, s, t, h, sun, level, why_th)
    if level == s.level:               # ส่ง/เสียง/จอไฟ RGB เฉพาะตอนเปลี่ยน
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
    show_status(w, "เฝ้าโกดังอยู่", COL_OK)
    was_down = False
    t_read = t_beep = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        msg = mqtt.get_message()       # ฟังทุก 20 ms อ่านเซนเซอร์ทุกวินาที
        ack = on_command(w, s, msg[1]) if msg else False
        down = buttons.pressed(1)      # SW5 = รับทราบหน้าโกดัง ไม่ต้องพึ่งเน็ต
        if down and not was_down:      # กดค้างไม่นับซ้ำ
            ack = True
            if s.acked:  # ไม่มีเหตุเตือน: บอกว่าปุ่มทำงาน
                show_ack(w, s, "ยังไม่มีเหตุเตือน", COL_DIM)
                beep("tap")
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

    stop(w, "จบรอบ คะแนนความงอกเหลือ %d" % int(s.germ), COL_DIM)


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
# 1) แจ้งเฉพาะเมื่อชื้นติดกัน 5 วินาที (นับใน Watch) - เปิดประตูโกดังแป๊บเดียวไม่ควรเตือน
# 2) คะแนนต่ำกว่า 80: ขึ้นจอว่า "ล็อตนี้ควรสุ่มเพาะทดสอบ" และส่งขึ้นเว็บครั้งเดียว
# 3) ระดับ 2: ให้ {"cmd":"set"} รับ "rh_hi" ด้วย แล้วบอกฝั่งเว็บ
