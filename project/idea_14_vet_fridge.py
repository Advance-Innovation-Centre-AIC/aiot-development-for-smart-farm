# idea_14_vet_fridge.py - ตู้เก็บยาและวัคซีนสัตว์: อุณหภูมิหลุดช่วง ยาเสื่อม มือถือรู้ทันที
# แม่แบบ A (idea_3_silo.py) โครงเน็ตเดิม หน้าจอใหม่ ธีมสว่าง
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# หน้าจอ  : เลขใหญ่ Seg7 + แถบช่วงกับไม้บรรทัด (Bar + Scale) + ต่ำสุด/สูงสุด
# คิดบนบอร์ด: 1) ต่ำสุด/สูงสุด (SW6 = ล้าง) 2) เวลาหลุดช่วงรวม + จำนวนครั้ง
# MQTT: หัวข้อ/คีย์เหมือน sf2_04 (สัญญา 3.4, 4) + คีย์เพิ่ม 2 ตัวใน event:
#       "out_s" = วินาทีที่หลุดช่วงรวม  "exc_n" = จำนวนครั้งที่หลุดช่วง
#       crop: vaccine injection tablet  คำสั่ง: ack, set t_hi (เหมือนเดิม)
# เกณฑ์ = ค่าสาธิตในห้อง 25-30 C  วัคซีนจริงต้องเก็บ 2-8 C ตามฉลาก ห้ามใช้ค่าในไฟล์นี้กับตู้จริง
#
# ลองเล่น  : หมุน VR3 จนขึ้นเตือนร้อน (ห้องแอร์เย็นจัดจะขึ้นเตือนเย็นเอง) แล้วส่ง {"cmd":"ack"} หรือกด SW5
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
CROP = "วัคซีนสัตว์"
TEMP_OFFSET = 0.0    # บอร์ดอ่านสูงกว่าห้อง: ใส่ค่าชดเชย เช่น -7.0

BROKER = "broker.hivemq.com"
ROOT = "bento-aiot"
CLIENT_ID = "bento-farm-" + TEAM
TOPIC_EVENT = ROOT + "/" + TEAM + "/event"
TOPIC_CMD = ROOT + "/" + TEAM + "/cmd"
BTN_NAMES = ("SW6", "SW5")

# (ชื่อไทย, รหัสอังกฤษ, T ต่ำ, T สูง, RH ต่ำ, RH สูง)
# ค่าสาธิต (ห้องเรียนไม่มีตู้เย็น): ห้อง 27 C = ปกติ  VR3 ครึ่งรอบ (+5) = เริ่มหลุด  วัคซีนจริง 2-8 C
CROPS = (
    ("วัคซีนสัตว์", "vaccine", 20, 30, 20, 95),
    ("ยาฉีดสัตว์", "injection", 20, 30, 20, 95),
    ("ยาเม็ดสัตว์", "tablet", 18, 32, 20, 80),
)
T_HI_MAX = 45
READ_MS, BEEP_MS, POLL_MS, RUN_MS = 1000, 5000, 20, 600000
BAND_LO, BAND_HI = 10, 40    # ช่วงของแถบและไม้บรรทัด (C)
MX = (("OK", rgbmatrix.GREEN), ("CHECK", rgbmatrix.YELLOW), ("ALERT", rgbmatrix.RED))
BOX = (212, 90, 368, 150)

SPEAKER = 40
VOLUME = 25
COL_PAGE, COL_CARD, COL_EDGE = 0xE6EFF1, 0xFFFFFF, 0xA9C4CA
COL_TEXT, COL_DIM, COL_TEAL = 0x10262E, 0x52707A, 0x0F7C8C
COL_OK, COL_WARN, COL_BAD = 0x0E9F6E, 0xC77700, 0xD12E3A
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
        p.append(("เย็นจัด", "cold"))
    if t > t_hi:
        p.append(("ตู้อุ่น", "hot"))
    if h < h_lo:
        p.append(("แห้งไป", "dry"))
    if h > h_hi:
        p.append(("ตู้ชื้น", "wet"))
    if not p:
        return 0, "อยู่ในช่วง", "ok"
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
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_EDGE, max=8, value=1)
    ui.Label(title, x=x + 10, y=y + 6, color=COL_TEAL, value=16)


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: ต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Panel(x=4, y=0, w=784, h=342, color=COL_PAGE, min=COL_EDGE, max=12, value=0)
    ui.Label("ตู้ยาและวัคซีนสัตว์  ใบบันทึกอุณหภูมิ", x=18, y=8, color=COL_TEXT, value=24)
    w = {"box": None, "leds": []}
    card(16, 44, 400, 250, "อุณหภูมิในตู้ตอนนี้")
    w["seg"] = ui.Seg7(text="--.-", x=30, y=72, w=150, h=64, color=COL_TEAL)
    ui.Label("C", x=114, y=96, color=COL_DIM, value=24)
    w["mood"] = ui.Label("...", x=226, y=78, color=COL_WARN, value=16)
    w["rh"] = ui.Label("ชื้น -- %", x=226, y=126, color=COL_DIM, value=14)
    w["band"] = ui.Bar(x=30, y=158, w=372, h=26, min=BAND_LO, max=BAND_HI, value=BAND_LO)
    w["band"].color(COL_EDGE)
    ruler = ui.Scale(x=30, y=188, w=372, h=34, color=COL_DIM, min=BAND_LO, max=BAND_HI)
    ruler.ticks(7, 1)
    w["range"] = ui.Label("-", x=30, y=232, color=COL_TEXT, value=16)
    w["crop"] = ui.Label("-", x=30, y=258, color=COL_DIM, value=14)
    card(426, 44, 352, 120, "ต่ำสุด / สูงสุด ตั้งแต่ล้าง")
    w["mn"] = ui.Label("ต่ำสุด --", x=438, y=72, color=COL_TEAL, value=24)
    w["mx"] = ui.Label("สูงสุด --", x=608, y=72, color=COL_BAD, value=24)
    w["since"] = ui.Label(BTN_NAMES[0] + " = ล้างค่า (ทำทุกเช้า)", x=438, y=120, color=COL_DIM, value=14)
    card(426, 172, 352, 122, "หลุดช่วง")
    w["out"] = ui.Label("หลุดช่วงรวม 00:00  (0 ครั้ง)", x=438, y=198, color=COL_TEXT, value=16)
    for i in range(3):
        x = 438 + i * 92
        w["leds"].append(ui.Led(x=x, y=228, w=16, h=16, color=LEVEL_COLORS[i]))
        ui.Label(("ในช่วง", "ใกล้หลุด", "หลุดไกล")[i], x=x + 20, y=226, color=COL_DIM, value=14)
    w["ack"] = ui.Label("-", x=438, y=254, color=COL_DIM, value=14)
    ui.Label("VR3 = ร้อนจากนอกตู้  " + BTN_NAMES[1] + " = รับทราบ", x=18, y=298, color=COL_DIM, value=14)
    w["status"] = ui.Label("กำลังเริ่ม", x=18, y=316, color=COL_DIM, value=16)
    w["mq"] = ui.Led(x=600, y=14, w=16, h=16, color=COL_OK, value=0)
    w["mq_t"] = ui.Label("MQTT: ออฟไลน์", x=624, y=12, color=COL_DIM, value=16)
    ui.poll()
    return w


def show_status(w, msg, col):
    w["status"].color(col)
    w["status"].text(msg)


def show_ack(w, s, msg, col):
    w["ack"].color(col)
    w["ack"].text("แจ้งแล้ว %d ครั้ง  %s" % (s.alerts, msg))


def show_crop(w, s):
    w["range"].text("ช่วงที่ยอมได้ %d - %d C" % (s.lim[0], s.lim[1]))
    w["crop"].text(s.crop[0])


def show_minmax(w, s):
    w["mn"].text("ต่ำสุด " + ("--" if s.t_min is None else "%.1f" % s.t_min))
    w["mx"].text("สูงสุด " + ("--" if s.t_max is None else "%.1f" % s.t_max))


def show_reading(w, s, t, h, level, why_th):
    w["seg"].text("%.1f" % t)
    w["band"].value(int(max(BAND_LO, min(BAND_HI, t))))
    w["mood"].color(LEVEL_COLORS[level])
    w["mood"].text(("อยู่ในช่วง ใช้ยาได้", "ใกล้หลุด: ", "หลุดช่วง!\n")[level] + ("" if level == 0 else why_th))
    w["rh"].text("ชื้น %d %%" % h)
    w["out"].text("หลุดช่วงรวม %s  (%d ครั้ง)" % (mmss(s.out_s), s.exc_n))
    show_minmax(w, s)
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
        self.t_min = self.t_max = None
        self.out_s, self.exc_n, self.out = 0, 0, False


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
                 "out_s": s.out_s, "exc_n": s.exc_n}):
        stop(w, "สายหลุดตอนส่ง")
    rgbmatrix.scroll(MX[level][0], MX[level][1], 80)
    beep(("good", "tap", "empty")[level])
    show_ack(w, s, "รอสัตวแพทย์รับทราบ" if level == 2 else "ส่งแล้ว", COL_BAD if level == 2 else COL_DIM)
    close_box(w)
    if level == 2:  # MsgBox รับไม่เกิน 126 ไบต์
        w["box"] = ui.MsgBox("ตู้ยาหลุดช่วง!\n" + why_th + "\n" + BTN_NAMES[1] + " / ack = รับทราบ",
                             x=BOX[0], y=BOX[1], w=BOX[2], h=BOX[3], value=0)


def read_and_judge(w, s):
    # อ่าน -> ตัดสิน -> โชว์ -> แจ้งถ้าเปลี่ยน  คืน True ถ้าเพิ่งแจ้ง
    sun = pots.read(2) * 10 // 4095    # VR3 = ร้อนจากนอกตู้ +0..10 C
    t, h = read_climate()
    if t is None or h is None:
        s.t = None                     # อ่านไม่ได้ = ลองใหม่รอบหน้าเลย
        return False
    s.t = t = t + sun
    s.t_min = t if s.t_min is None else min(s.t_min, t)
    s.t_max = t if s.t_max is None else max(s.t_max, t)
    level, why_th, why_en = judge(t, h, s.lim)
    if level > 0:
        s.out_s += READ_MS // 1000
        if not s.out:
            s.exc_n += 1               # เพิ่งออกจากช่วง = นับหนึ่งครั้ง
    s.out = level > 0
    show_reading(w, s, t, h, level, why_th)
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
    show_status(w, "เฝ้าตู้ยาอยู่", COL_OK)
    was_down = was_reset = False
    t_read = t_beep = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        msg = mqtt.get_message()       # ฟังทุก 20 ms อ่านเซนเซอร์ทุกวินาที
        ack = on_command(w, s, msg[1]) if msg else False
        down = buttons.pressed(1)      # SW5 = รับทราบหน้าตู้ ไม่ต้องพึ่งเน็ต
        if down and not was_down:      # กดค้างไม่นับซ้ำ
            ack = True
            if s.acked:  # ไม่มีเหตุเตือน: บอกว่าปุ่มทำงาน
                show_ack(w, s, "ยังไม่มีเหตุเตือน", COL_DIM)
                beep("tap")
        was_down = down
        reset = buttons.pressed(0)     # SW6 = ล้างต่ำสุด/สูงสุด (ไม่ส่งเน็ต)
        if reset and not was_reset:
            s.t_min = s.t_max = s.t
            show_minmax(w, s)
            beep("tap")
        was_reset = reset
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

    stop(w, "จบรอบ หลุดช่วงรวม " + mmss(s.out_s), COL_DIM)


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
# 1) แจ้งเฉพาะเมื่อหลุดช่วงติดกัน 5 วินาที (นับใน Watch) - เปิดตู้หยิบยาแป๊บเดียวไม่ควรเตือน
# 2) หลุดช่วงรวมเกิน 30 นาที: ขึ้นจอ "กักยาไว้ก่อน ถามสัตวแพทย์" และส่งขึ้นเว็บ
# 3) ระดับ 2: ให้ {"cmd":"set"} รับ "t_lo" ด้วย แล้วบอกฝั่งเว็บ
