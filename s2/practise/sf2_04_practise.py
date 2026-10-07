# sf2_04_practise.py - แบบฝึกเติมโค้ด (Code Quest ระดับ 3 โจทย์เพิ่ม): ตัดสินว่าพืชสบายไหม
# วิธีเล่น : โค้ดเหมือน sf2_04_crop_alert.py ทุกบรรทัด ยกเว้นช่อง ____ 3 ช่อง: A, B, C ในฟังก์ชัน judge()
#           คำอธิบายเต็มอยู่ใน sf2_04_crop_alert.py ไฟล์นี้เก็บไว้เฉพาะส่วนที่ต้องเติม
#           รันแล้ว self_test ตรวจ 7 กรณีก่อนเปิดจอ: ผ่าน = "ผ่าน!" · ไม่ผ่าน = บอกกรณีที่ผิดแล้วหยุด
# ถ้าเจอ  : NameError: name '____' = ยังเติมไม่ครบ · ติดขัด = คำใบ้ท้ายใบงาน หรือรัน sf2_04_crop_alert.py ไปก่อน
# เฉลย    : โจทย์เพิ่ม (โบนัส) เฉลยต้นคาบหน้า
# ต้องแก้ก่อนรัน: WIFI_SSID, WIFI_PASS, TEAM, CROP (ตรวจกฎได้โดยไม่ต้องต่อเน็ต)
# (ทำจาก sf2_04_crop_alert.py e091cf15205b)

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
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"
TEAM = "teamXX"
CROP = "มะเขือเทศ"
TEMP_OFFSET = 0.0

BROKER = "broker.hivemq.com"
ROOT = "bento-aiot"
CLIENT_ID = "bento-farm-" + TEAM
TOPIC_EVENT = ROOT + "/" + TEAM + "/event"
TOPIC_CMD = ROOT + "/" + TEAM + "/cmd"
BTN_NAMES = ("SW6", "SW5")

CROPS = (
    ("มะเขือเทศ", "tomato", 20, 30, 60, 80),
    ("ผักสลัด", "lettuce", 15, 25, 50, 70),
    ("เห็ดนางฟ้า", "mushroom", 22, 28, 80, 95),
    ("กล้วยไม้", "orchid", 22, 32, 60, 80),
)
T_HI_MAX = 45
READ_MS, BEEP_MS, POLL_MS, RUN_MS = 1000, 5000, 20, 600000
CHART_MAX_C = 50
MX = (("OK", rgbmatrix.GREEN), ("WARN", rgbmatrix.YELLOW), ("ALERT", rgbmatrix.RED))
BOX = (212, 90, 368, 150)

SPEAKER = 40
VOLUME = 25
COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF
LEVEL_COLORS = (COL_OK, COL_WARN, COL_BAD)


# ---- 2) ฮาร์ดแวร์ ----
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


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
    # คืน (ระดับ 0-2, ข้อความไทย, รหัสอังกฤษ)
    # ระดับ 2 = หลุดไกล (เกิน 3 C หรือ 10 %RH)
    t_lo, t_hi, h_lo, h_hi = lim
    p = []
    if t < t_lo:
        p.append(("หนาวไป", "cold"))
    if t > ____:                       # ช่อง A: ร้อนเกิน "อะไร" ถึงนับว่าร้อนไป?
        p.append(("ร้อนไป", "hot"))
    if h < h_lo:
        p.append(("แห้งไป", "dry"))
    if h > h_hi:
        p.append(("ชื้นไป", "wet"))
    if not p:
        return 0, "สบายดี", "ok"
    # ช่อง B: หลุดเกิน "กี่องศา" จึงนับว่าหลุดไกล (ระดับ 2)?  ช่อง C: หลุดนิดเดียว = ระดับอะไร?
    far = (t < t_lo - 3) or (t > t_hi + ____) or (h < h_lo - 10) or (h > h_hi + 10)
    return (2 if far else ____), " + ".join(x[0] for x in p), "+".join(x[1] for x in p)


def self_test():
    # ตรวจ judge() 7 กรณี ด้วยเกณฑ์มะเขือเทศ 20-30 C, 60-80 %RH ก่อนเปิดจอ คืน True ถ้าผ่านครบ
    lim = [20, 30, 60, 80]
    cases = ((25, 70, 0, "ok"), (31, 70, 1, "hot"), (33, 70, 1, "hot"), (33.5, 70, 2, "hot"),
             (25, 55, 1, "dry"), (35, 50, 2, "hot+dry"), (19, 85, 1, "cold+wet"))
    for t, h, want_level, want_code in cases:
        level, _, code = judge(t, h, lim)
        if (level, code) != (want_level, want_code):
            print("ยังไม่ถูก:", t, "C", h, "% ควรได้ระดับ", want_level, want_code, "แต่ได้", level, code)
            return False
    print("ผ่าน! ตัดสินพืชถูกทั้ง 7 กรณี")
    return True


# ---- 4) เครือข่าย ----
def connect_broker(w):
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
    show_status(w, "ต่อ WiFi...", COL_WARN)
    ui.poll()
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "WiFi ไม่ติด: ตรวจชื่อ/รหัส"
    linked = connect_broker(w)
    if not linked or not mqtt.subscribe(TOPIC_CMD):
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
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: เชื่อมต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("พืชไม่สบาย มือถือรู้ทันที", x=12, y=6, color=COL_TEXT, value=24)
    w = {"box": None, "leds": []}
    card(12, 44, 500, 200, "ตอนนี้")
    w["crop"] = ui.Label("-", x=24, y=74, color=COL_INFO, value=16)
    w["th"] = ui.Label("-- C  -- %", x=24, y=100, color=COL_TEXT, value=24)
    w["mood"] = ui.Label("...", x=24, y=140, color=COL_WARN)
    for i in range(3):
        x = 24 + i * 130
        w["leds"].append(ui.Led(x=x, y=198, w=20, h=20, color=LEVEL_COLORS[i]))
        ui.Label(("สบาย", "เครียด", "แย่")[i], x=x + 28, y=198, color=COL_DIM, value=14)
    card(522, 44, 258, 200, "แจ้งสถานะ")
    w["ack"] = ui.Label("-", x=534, y=80, color=COL_DIM)
    w["chart"] = ui.Chart(x=12, y=252, w=400, h=86, color=COL_WARN, min=0, max=CHART_MAX_C)
    w["chart"].prop(ui.PROP_CHART_POINTS, 400)
    w["s_hi"] = w["chart"].add_series(COL_BAD)
    ui.Label("ส้ม = อุณหภูมิ  แดง = เกณฑ์ร้อน\nVR3 = แดด  " + BTN_NAMES[1] + " = รับทราบ",
             x=424, y=256, color=COL_DIM, value=14)
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
    w["ack"].text("แจ้งไปแล้ว %d ครั้ง\n%s" % (s.alerts, msg))


def show_crop(w, s):
    t_lo, t_hi, h_lo, h_hi = s.lim
    w["crop"].text("%s ชอบ %d-%d C, %d-%d %%" % (s.crop[0], t_lo, t_hi, h_lo, h_hi))


def show_reading(w, s, t, h, sun, level, why_th):
    w["th"].text("%.1f C  %.1f %%  (แดด +%d)" % (t, h, sun))
    w["mood"].color(LEVEL_COLORS[level])
    w["mood"].text(("สบายดี :)", "เริ่มเครียด: ", "แย่แล้ว!: ")[level] + ("" if level == 0 else why_th))
    for i in range(3):
        w["leds"][i].value(1 if i == level else 0)
    w["chart"].set_next(0, int(max(0, min(CHART_MAX_C, t))))
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
    try:
        cmd = json.loads(raw.decode())
        act = cmd.get("cmd")
    except Exception:
        return False
    if act == "ack":
        return True
    elif act == "set":
        v = cmd.get("t_hi")
        if isinstance(v, int) and s.lim[0] < v <= T_HI_MAX:
            s.lim[1], s.level, s.t = v, -1, None
            show_crop(w, s)
            beep("tap")
    return False


def on_level(w, s, level, why_th, why_en, t, h, sun):
    s.level, s.acked = level, level < 2
    s.alerts += 1
    if not send({"id": TEAM, "crop": s.crop[1], "level": level, "alert": why_en,
                 "temp_c": round(t, 1), "rh": round(h, 1), "sun_c": sun}):
        stop(w, "สายหลุดตอนส่ง")
    rgbmatrix.scroll(MX[level][0], MX[level][1], 80)
    beep(("good", "tap", "empty")[level])
    show_ack(w, s, "รอคนรับทราบ" if level == 2 else "ส่งแล้ว", COL_BAD if level == 2 else COL_DIM)
    close_box(w)
    if level == 2:
        w["box"] = ui.MsgBox("แย่แล้ว!\n" + why_th + "\n" + BTN_NAMES[1] + " / ack = รับทราบ",
                             x=BOX[0], y=BOX[1], w=BOX[2], h=BOX[3], value=0)


def read_and_judge(w, s):
    sun = pots.read(2) * 10 // 4095
    t, h = read_climate()
    if t is None or h is None:
        s.t = None
        return False
    s.t = t = t + sun
    level, why_th, why_en = judge(t, h, s.lim)
    show_reading(w, s, t, h, sun, level, why_th)
    if level == s.level:
        return False
    on_level(w, s, level, why_th, why_en, t, h, sun)
    return True


def main():
    if hasattr(ui, "volume"):
        ui.volume(SPEAKER)
    if not self_test():                # ตรวจกฎก่อน ไม่ผ่าน = ไม่เปิดจอ ไม่ต่อเน็ต
        return
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
    show_status(w, "เฝ้าอยู่", COL_OK)
    was_down = False
    t_read = t_beep = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        msg = mqtt.get_message()
        ack = on_command(w, s, msg[1]) if msg else False
        down = buttons.pressed(1)
        if down and not was_down:
            ack = True
        was_down = down
        if ack and not s.acked:
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
