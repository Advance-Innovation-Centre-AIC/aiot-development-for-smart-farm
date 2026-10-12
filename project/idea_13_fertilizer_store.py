# idea_13_fertilizer_store.py - โกดังปุ๋ย: ความชื้นสูง ปุ๋ยจับก้อน มือถือรู้ทันที
# แม่แบบ A (idea_3_silo.py) โครงเน็ตเดิม หน้าจอใหม่
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# หน้าจอ  : กระสอบปุ๋ย 3 ใบ (Bar ตั้ง) เทียบเกณฑ์ของแต่ละชนิด + กราฟ
# คิดบนบอร์ด: 1) แต้มจับก้อนสะสม (%RH ที่เกิน x นาที) 2) นับครั้งที่ความชื้นพุ่ง (ประตูเปิด)
# MQTT: หัวข้อ/คีย์เหมือน sf2_04 (สัญญา 3.4, 4) + คีย์เพิ่ม 2 ตัวใน event:
#       "cake_pts" = แต้มจับก้อนสะสม  "spikes" = ครั้งที่ RH พุ่ง 5 % ใน 10 วินาที
#       crop: urea npk organic  คำสั่ง: ack, set t_hi (เหมือนเดิม)  ไม่มี alert "dry"
# เกณฑ์ = ค่าสาธิตในห้องเรียน ไม่ใช่คำแนะนำทางเกษตร (ห้องชื้นเกิน 65 % ให้ขยับ RH สูงขึ้น)
#
# ลองเล่น  : เป่าลมหายใจใส่บอร์ด จนขึ้นเตือน แล้วส่ง {"cmd":"ack"} หรือกด SW5
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
CROP = "ปุ๋ยยูเรีย"
TEMP_OFFSET = 0.0    # บอร์ดอ่านสูงกว่าห้อง: ใส่ค่าชดเชย เช่น -7.0

BROKER = "broker.hivemq.com"
ROOT = "bento-aiot"
CLIENT_ID = "bento-farm-" + TEAM
TOPIC_EVENT = ROOT + "/" + TEAM + "/event"
TOPIC_CMD = ROOT + "/" + TEAM + "/cmd"
BTN_NAMES = ("SW6", "SW5")

# (ชื่อไทย, รหัสอังกฤษ, T ต่ำ, T สูง, RH ต่ำ, RH สูง)
# ค่าสาธิต: ห้อง 60 % = ปกติ  เป่าลมหายใจ = เริ่มชื้น  เกิน RH สูง + 10 = จับก้อน
CROPS = (
    ("ปุ๋ยยูเรีย", "urea", 15, 38, 0, 65),
    ("ปุ๋ยสูตรผสม", "npk", 15, 38, 0, 70),
    ("ปุ๋ยอินทรีย์", "organic", 15, 38, 0, 75),
)
SHORT = ("ยูเรีย", "สูตรผสม", "อินทรีย์")
T_HI_MAX = 45
READ_MS, BEEP_MS, POLL_MS, RUN_MS = 1000, 5000, 20, 600000
SACK_MAX = 130       # แท่งกระสอบ: 100 = ชื้นเท่าเกณฑ์ของปุ๋ยชนิดนั้น
SPIKE_RH, SPIKE_S = 5, 10
MX = (("DRY", rgbmatrix.GREEN), ("DAMP", rgbmatrix.YELLOW), ("CAKE", rgbmatrix.RED))
BOX = (212, 90, 368, 150)

SPEAKER = 40
VOLUME = 25
COL_TEXT, COL_DIM = 0xECEFF4, 0x8E96A3
COL_CARD, COL_EDGE = 0x1C1F24, 0xFFC300
COL_OK, COL_WARN, COL_BAD, COL_HAZ = 0x3DBE8B, 0xFF9F1C, 0xF0433A, 0xFFC300
LEVEL_COLORS = (COL_OK, COL_WARN, COL_BAD)
SACK_TRACK = (0x2B3A33, 0x4A3A1A, 0x4A1F1F)


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


def sack_state(h, h_hi):
    return 0 if h <= h_hi else (1 if h <= h_hi + 10 else 2)


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
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_EDGE, max=4, value=1)
    ui.Label(title, x=x + 10, y=y + 6, color=COL_HAZ, value=16)


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: ต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def build_screen(idx):
    ui.screen()
    time.sleep_ms(200)
    ui.Label("โกดังปุ๋ย  กระสอบไหนจะจับก้อน", x=12, y=4, color=COL_HAZ, value=24)
    w = {"box": None, "sacks": [], "dots": [], "pct": []}
    card(12, 40, 420, 250, "กระสอบ 3 ชนิด เทียบเกณฑ์ของตัวเอง")
    for i in range(3):
        x = 40 + i * 132
        w["dots"].append(ui.Panel(x=x + 27, y=68, w=16, h=16, color=COL_OK, min=COL_OK, max=8, value=0))
        w["sacks"].append(ui.Bar(x=x, y=90, w=70, h=140, min=0, max=SACK_MAX, value=0))
        pass  # Bar.color() ตั้งสีส่วนที่เติมบนบอร์ดจริง ใช้สีเดิมของ Bar ให้เห็นชัด
        ui.Label(SHORT[i], x=x, y=236, color=COL_HAZ if i == idx else COL_DIM, value=16)
        w["pct"].append(ui.Label("--", x=x, y=260, color=COL_DIM, value=14))
    w["chart"] = ui.Chart(x=12, y=298, w=420, h=54, color=COL_HAZ, min=0, max=100)
    w["chart"].prop(ui.PROP_CHART_POINTS, 400)
    w["s_hi"] = w["chart"].add_series(COL_BAD)
    card(442, 40, 338, 140, "ห้องเก็บตอนนี้")
    w["seg"] = ui.Seg7(text="--", x=456, y=70, w=100, h=50, color=COL_HAZ)
    ui.Label("%RH", x=566, y=90, color=COL_DIM, value=16)
    w["temp"] = ui.Label("-- C", x=640, y=72, color=COL_DIM, value=16)
    w["sun"] = ui.Label("แดด +0", x=640, y=96, color=COL_DIM, value=14)
    w["mood"] = ui.Label("...", x=456, y=138, color=COL_WARN, value=16)
    card(442, 188, 338, 106, "สะสมตั้งแต่เปิดบอร์ด")
    w["cake"] = ui.Label("แต้มจับก้อน 0.0", x=454, y=214, color=COL_TEXT, value=16)
    w["spike"] = ui.Label("ชื้นพุ่ง (ประตูเปิด) 0 ครั้ง", x=454, y=238, color=COL_TEXT, value=16)
    w["ack"] = ui.Label("-", x=454, y=264, color=COL_DIM, value=14)
    ui.Label("เป่าลม = ชื้นขึ้น  " + BTN_NAMES[1] + " = รับทราบ", x=442, y=302, color=COL_DIM, value=14)
    w["status"] = ui.Label("กำลังเริ่ม", x=12, y=358, color=COL_DIM)
    w["mq"] = ui.Led(x=600, y=10, w=16, h=16, color=COL_OK, value=0)
    w["mq_t"] = ui.Label("MQTT: ออฟไลน์", x=624, y=8, color=COL_DIM, value=16)
    ui.poll()
    return w


def show_status(w, msg, col):
    w["status"].color(col)
    w["status"].text(msg)


def show_ack(w, s, msg, col):
    w["ack"].color(col)
    w["ack"].text("แจ้งแล้ว %d ครั้ง  %s" % (s.alerts, msg))


def show_crop(w, s):
    w["chart"].set_next(w["s_hi"], s.lim[3])


def show_reading(w, s, t, h, sun, level, why_th):
    for i in range(3):
        h_hi = CROPS[i][5]
        st = sack_state(h, h_hi)
        w["sacks"][i].value(int(min(SACK_MAX, h * 100 / h_hi)))
        pass  # Bar.color() ตั้งสีส่วนที่เติมบนบอร์ดจริง ใช้สีเดิมของ Bar ให้เห็นชัด
        w["dots"][i].color(LEVEL_COLORS[st])
        w["pct"][i].text("%d%% ของเกณฑ์ %d" % (h * 100 / h_hi, h_hi))
    w["seg"].text("%d" % h)
    w["temp"].text("%.1f C" % t)
    w["sun"].text("แดด +%d" % sun)
    w["mood"].color(LEVEL_COLORS[level])
    w["mood"].text(("แห้งดี ปุ๋ยร่วน", "เริ่มชื้น: ", "จับก้อนแล้ว: ")[level] + ("" if level == 0 else why_th))
    w["cake"].text("แต้มจับก้อน %.1f" % s.cake)
    w["spike"].text("ชื้นพุ่ง (ประตูเปิด) %d ครั้ง" % s.spikes)
    w["chart"].set_next(0, int(max(0, min(100, h))))
    w["chart"].set_next(w["s_hi"], s.lim[3])


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
        self.cake = 0.0                # ผลรวม (RH - เกณฑ์) / 60 ทุกวินาที
        self.spikes = 0
        self.hist = []                 # RH ย้อนหลัง SPIKE_S วินาที
        self.in_spike = False


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
    elif act == "set":                 # จำนวนเต็มในช่วงที่สมเหตุผลเท่านั้น
        v = cmd.get("t_hi")
        if isinstance(v, int) and s.lim[0] < v <= T_HI_MAX:
            s.lim[1], s.level, s.t = v, -1, None     # ตัดสินใหม่และแจ้งทันที
            show_crop(w, s)
            beep("tap")
    # >>> ภารกิจกลุ่ม: วาง elif "led"/"pump" จาก sf2_03 ตรงนี้ <<<
    return False


def on_level(w, s, level, why_th, why_en, t, h, sun):
    # ระดับเปลี่ยน: ส่งก่อน (สัญญาข้อ 3.4)
    s.level, s.acked = level, level < 2
    s.alerts += 1
    if not send({"id": TEAM, "crop": s.crop[1], "level": level, "alert": why_en,
                 "temp_c": round(t, 1), "rh": round(h, 1), "sun_c": sun,
                 "cake_pts": round(s.cake, 1), "spikes": s.spikes}):
        stop(w, "สายหลุดตอนส่ง")
    rgbmatrix.scroll(MX[level][0], MX[level][1], 80)
    beep(("good", "tap", "empty")[level])
    show_ack(w, s, "รอคนเฝ้าโกดัง" if level == 2 else "ส่งแล้ว", COL_BAD if level == 2 else COL_DIM)
    close_box(w)
    if level == 2:  # MsgBox รับไม่เกิน 126 ไบต์
        w["box"] = ui.MsgBox("ปุ๋ยจับก้อน!\n" + why_th + "\n" + BTN_NAMES[1] + " / ack = รับทราบ",
                             x=BOX[0], y=BOX[1], w=BOX[2], h=BOX[3], value=0)


def track(s, h):
    # แต้มจับก้อน + ความชื้นพุ่งเทียบ SPIKE_S วินาทีก่อน
    s.cake += max(0.0, h - s.lim[3]) / 60
    s.hist.append(h)
    if len(s.hist) > SPIKE_S:
        s.hist.pop(0)
    rise = h - s.hist[0]
    if rise >= SPIKE_RH and not s.in_spike:
        s.spikes += 1
        s.in_spike = True
    elif rise < 2:
        s.in_spike = False


def read_and_judge(w, s):
    # อ่าน -> ตัดสิน -> โชว์ -> แจ้งถ้าเปลี่ยน  คืน True ถ้าเพิ่งแจ้ง
    sun = pots.read(2) * 10 // 4095    # VR3 = แดดบนหลังคาโกดัง +0..10 C
    t, h = read_climate()
    if t is None or h is None:
        s.t = None                     # อ่านไม่ได้ = ลองใหม่รอบหน้าเลย
        return False
    s.t = t = t + sun
    track(s, h)
    level, why_th, why_en = judge(t, h, s.lim)
    show_reading(w, s, t, h, sun, level, why_th)
    if level == s.level:               # ส่ง/เสียง/จอไฟ RGB เฉพาะตอนเปลี่ยน
        return False
    on_level(w, s, level, why_th, why_en, t, h, sun)
    return True


def main():
    if hasattr(ui, "volume"):
        ui.volume(SPEAKER)
    names = [c[0] for c in CROPS]
    idx = names.index(CROP) if CROP in names else -1
    w = build_screen(idx)
    if idx < 0:
        stop(w, "แก้ CROP ให้ตรงกับ CROPS")
    s = Watch(idx)
    show_crop(w, s)
    if not valid_team(TEAM):
        stop(w, "แก้ TEAM ก่อน")
    problem = connect_farm(w)
    if problem:
        stop(w, problem)
    show_link(w, True)
    show_status(w, "เฝ้า" + CROP + "อยู่", COL_OK)
    was_down = False
    t_read = t_beep = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        msg = mqtt.get_message()       # ฟังทุก 20 ms อ่านเซนเซอร์ทุกวินาที
        ack = on_command(w, s, msg[1]) if msg else False
        down = buttons.pressed(1)      # SW5 = รับทราบหน้าโกดัง ไม่ต้องพึ่งเน็ต
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

    stop(w, "จบรอบ แต้มจับก้อน %.1f" % s.cake, COL_DIM)


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
# 2) แต้มจับก้อนเกิน 10: ขึ้นจอว่า "ย้ายกระสอบขึ้นแคร่" และส่งขึ้นเว็บครั้งเดียว
# 3) ฝั่งเว็บ: วาดแนวโน้ม rh จาก event หลายใบ ว่าชื้นขึ้นเรื่อย ๆ ไหม
