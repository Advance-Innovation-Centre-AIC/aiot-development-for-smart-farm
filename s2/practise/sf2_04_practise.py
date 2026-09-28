# sf2_04_practise.py - แบบฝึกเติมโค้ด (Code Quest ระดับ 3 โจทย์เพิ่ม): ตัดสินว่าพืชสบายไหม
#
# วิธีเล่น  : ไฟล์นี้เหมือน sf2_04_crop_alert.py ทุกอย่าง ยกเว้นส่วน "3) สมอง" ที่เว้นช่อง ____ (ขีดล่างสี่ตัว)
#            ไว้ 3 ช่อง: A, B, C ในฟังก์ชัน judge()
#            เติมให้ครบแล้วรัน โปรแกรมจะตรวจกฎของคุณ 7 กรณีก่อนเปิดจอ (self_test)
#            ผ่านครบ = Console ขึ้น "ผ่าน!" แล้วเล่นต่อได้เหมือนไฟล์ตัวอย่าง
#            ยังไม่ถูก = Console บอกว่ากรณีไหนผิด แล้วหยุด (ยังไม่เปิดจอ ยังไม่ต่อเน็ต)
# ถ้าเจอ   : NameError: name '____' isn't defined = ยังมีช่องที่ไม่ได้เติม (ตั้งใจให้หยุดชัด ๆ แบบนี้)
# ติดขัด?  : ใช้บันไดช่วยเหลือในใบงาน (คำใบ้ 3 ขั้นอยู่ท้ายใบงาน) หรือรันไฟล์ตัวอย่างเต็ม
#            sf2_04_crop_alert.py เพื่อไปต่อก่อน แล้วค่อยกลับมาเทียบกับของตัวเอง
# เฉลย     : โจทย์เพิ่ม (โบนัส) เฉลยต้นคาบหน้า
# ต้องแก้ก่อนรันบนบอร์ด: WIFI_SSID, WIFI_PASS, TEAM, CROP (ตรวจกฎผ่านได้โดยไม่ต้องต่อเน็ต)
#
# (ทำจาก sf2_04_crop_alert.py a4073c0bfb23)

import buttons
import json
import mqtt
import pots
import rgbmatrix
import sensors
import time
import ui
import wifi

# ---- 1) ตั้งค่า (แก้ได้) ----
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"   # ตั้งเอง: อังกฤษ/ตัวเลขสั้น ๆ ไม่มีเว้นวรรค
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"   # อย่างน้อย 8 ตัว · อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร
TEAM = "teamXX"                        # team01 ถึง team20
CROP = "มะเขือเทศ"                     # พืชของกลุ่ม ต้องมีใน CROPS
TEMP_OFFSET = 0.0    # บอร์ดอุ่นจากชิปของตัวเอง: เทียบกับเทอร์โมมิเตอร์ในห้อง แล้วใส่ค่าชดเชย เช่น -7.0
                     # (ใช้ค่าเดียวกับที่กลุ่มหาได้ในคาบ 1) ไม่ตั้ง = พืชทุกชนิดจะ "ร้อนไป" เร็วกว่าจริง
                     # ความชื้นที่บอร์ดอ่านจะต่ำกว่าห้องเล็กน้อยเพราะบอร์ดอุ่น (sf2_02 มีตัวแปลงให้ดู)

BROKER = "broker.hivemq.com"
ROOT = "bento-aiot"
CLIENT_ID = "bento-farm-" + TEAM
TOPIC_EVENT = ROOT + "/" + TEAM + "/event"   # หน้าเว็บโชว์หัวข้อนี้ในกล่อง event
TOPIC_CMD = ROOT + "/" + TEAM + "/cmd"
BTN_NAMES = ("SW5", "SW6")             # ปุ่มล่าง = pressed(0), ปุ่มบน = pressed(1) ตามตัวอักษรบนแผง

# (ชื่อไทย, รหัสอังกฤษที่ส่งในแจ้งเตือน, T ต่ำ, T สูง, RH ต่ำ, RH สูง) ตัวเลขเพื่อการเรียน ไม่ใช่คำแนะนำเกษตร
# เพิ่มพืชของกลุ่มต่อท้ายได้เลย
CROPS = (
    ("มะเขือเทศ", "tomato", 20, 30, 60, 80),
    ("ผักสลัด", "lettuce", 15, 25, 50, 70),
    ("เห็ดนางฟ้า", "mushroom", 22, 28, 80, 95),
    ("กล้วยไม้", "orchid", 22, 32, 60, 80),
)
T_HI_MAX = 45                          # {"cmd":"set"} ตั้งเกณฑ์ร้อนได้ไม่เกินนี้
READ_MS, BEEP_MS, POLL_MS, RUN_MS = 1000, 5000, 20, 600000   # POLL 20 ms: ปุ่มต้องอ่านบ่อยกว่า 50 ms
CHART_MAX_C = 50                       # กราฟอุณหภูมิ 0-50 C
# จอไฟ RGB 16x8 รับแต่อักษรอังกฤษกับตัวเลข สีมี OFF RED GREEN YELLOW BLUE PURPLE CYAN WHITE
MX = (("OK", rgbmatrix.GREEN), ("WARN", rgbmatrix.YELLOW), ("ALERT", rgbmatrix.RED))
BOX = (212, 90, 368, 150)              # กล่องเตือน (x, y, w, h) ลอยทับกลางจอโดยตั้งใจ จนกว่าจะมีคนรับทราบ

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF
LEVEL_COLORS = (COL_OK, COL_WARN, COL_BAD)


# ---- 2) ฮาร์ดแวร์ ----
def read_climate():
    # คืน (อุณหภูมิที่ชดเชยแล้ว, ความชื้น) ตัวที่อ่านไม่ได้เป็น None
    # ชดเชยตรงนี้ที่เดียว ส่วนอื่นของโปรแกรมจึงได้ค่าที่แก้แล้วเสมอ
    try:
        t_raw = sensors.sht40.temperature()
        h = sensors.sht40.humidity()
    except Exception:
        return None, None
    return t_raw + TEMP_OFFSET, h


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ไม่แตะเน็ต ----
def valid_team(team):
    return len(team) == 6 and team[:4] == "team" and team[4:].isdigit() and team != "team00"


def judge(t, h, lim):
    # คืน (ระดับ 0-2, ปัญหาภาษาไทยไว้ขึ้นจอ, รหัสอังกฤษไว้ส่งให้เครื่องอ่าน)
    # lim = [T ต่ำ, T สูง, RH ต่ำ, RH สูง] ที่ใช้อยู่ · ระดับ 2 = หลุดไกล (เกิน 3 C หรือ 10 %RH)
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
def connect_farm(w):
    # WiFi -> IP -> broker + subscribe ขั้นไหนพังคืนข้อความบอกว่าพังตรงไหน
    show_status(w, "ต่อ WiFi...", COL_WARN)
    ui.poll()                          # ป้ายต้องขึ้นจอก่อนบรรทัดที่บล็อก
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "WiFi ไม่ติด: ตรวจชื่อ/รหัส"
    try:
        linked = mqtt.connect(BROKER, port=1883, client_id=CLIENT_ID, keepalive=60)
    except OSError:
        linked = False
    if not linked or not mqtt.subscribe(TOPIC_CMD):     # subscribe ต้องมาหลัง connect เสมอ
        return "broker ไม่ตอบ (1883)"
    return ""


def send(obj):
    # ส่ง JSON เข้า event คืน False ถ้าสายหลุด (publish ตอนสายหลุดโยน OSError ไม่ใช่คืน False)
    try:
        mqtt.publish(TOPIC_EVENT, json.dumps(obj))
        return True
    except OSError:
        return False


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า (ทุกไฟล์ใช้แบบเดียวกัน)
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def build_screen():
    # สร้างทุกอย่างบนจอครั้งเดียว แล้วคืน dict ของ widget ที่ต้องอัปเดตภายหลัง
    ui.screen()
    time.sleep_ms(200)
    ui.Label("พืชไม่สบาย มือถือรู้ทันที", x=12, y=6, color=COL_TEXT, value=24)
    w = {"box": None, "leds": []}
    card(12, 44, 500, 200, "ตอนนี้")
    w["crop"] = ui.Label("-", x=24, y=74, color=COL_INFO, value=16)
    w["th"] = ui.Label("-- C  -- %", x=24, y=100, color=COL_TEXT, value=24)
    w["mood"] = ui.Label("...", x=24, y=140, color=COL_WARN)
    for i in range(3):                 # ไฟสถานะ 3 ดวง ติดทีละดวงเหมือนแผงควบคุมจริง
        x = 24 + i * 130
        w["leds"].append(ui.Led(x=x, y=198, w=20, h=20, color=LEVEL_COLORS[i]))
        ui.Label(("สบาย", "เครียด", "แย่")[i], x=x + 28, y=198, color=COL_DIM, value=14)
    card(522, 44, 258, 200, "แจ้งสถานะ")
    w["ack"] = ui.Label("-", x=534, y=80, color=COL_DIM)
    # กราฟกว้าง 400 และ 400 จุด = เส้นเรียบไม่มีจุดกลม (จุดละวินาที = ย้อนหลังราว 6 นาที)
    w["chart"] = ui.Chart(x=12, y=252, w=400, h=86, color=COL_WARN, min=0, max=CHART_MAX_C)
    w["chart"].prop(ui.PROP_CHART_POINTS, 400)
    w["s_hi"] = w["chart"].add_series(COL_BAD)
    ui.Label("ส้ม = อุณหภูมิ  แดง = เกณฑ์ร้อน\nVR3 = แดด  " + BTN_NAMES[0] + " = รับทราบ",
             x=424, y=256, color=COL_DIM, value=14)
    w["status"] = ui.Label("กำลังเริ่ม", x=12, y=352, color=COL_DIM)
    ui.poll()
    return w


def show_status(w, msg, col):
    # ไม่เรียก ui.poll() ในนี้ เพราะจะกินเหตุการณ์ปัดวงล้อที่ลูปหลักรออ่าน
    w["status"].color(col)
    w["status"].text(msg)


def show_ack(w, s, msg, col):
    w["ack"].color(col)
    w["ack"].text("แจ้งไปแล้ว %d ครั้ง\n%s" % (s.alerts, msg))


def show_crop(w, s):
    # ชื่อพืช + เกณฑ์ที่ใช้อยู่ (เปลี่ยนเมื่อแอปส่ง set มา)
    t_lo, t_hi, h_lo, h_hi = s.lim
    w["crop"].text("%s ชอบ %d-%d C, %d-%d %%" % (s.crop[0], t_lo, t_hi, h_lo, h_hi))


def show_reading(w, s, t, h, sun, level, why_th):
    # ค่าที่อ่านได้ + อารมณ์พืช + ไฟ 3 ดวง + กราฟ (ทุกวินาที)
    w["th"].text("%.1f C  %.1f %%  (แดด +%d)" % (t, h, sun))
    w["mood"].color(LEVEL_COLORS[level])
    w["mood"].text(("สบายดี :)", "เริ่มเครียด: ", "แย่แล้ว!: ")[level] + ("" if level == 0 else why_th))
    for i in range(3):
        w["leds"][i].value(1 if i == level else 0)    # 0 = หรี่ (ไม่ดับมืด)
    w["chart"].set_next(0, int(max(0, min(CHART_MAX_C, t))))   # กราฟรับเฉพาะจำนวนเต็ม
    w["chart"].set_next(w["s_hi"], s.lim[1])


def close_box(w):
    # กล่องเตือนสร้างแบบไม่มีปุ่ม X (value=0) เราจึงปิดเองด้วย .delete() และมีได้ทีละกล่อง
    if w["box"] is not None:
        w["box"].delete()
        w["box"] = None


# ---- 6) โปรแกรมหลัก ----
class Watch:
    # สิ่งที่บอร์ดจำระหว่างเฝ้า: พืช เกณฑ์ที่ใช้อยู่ ระดับล่าสุด รับทราบหรือยัง แจ้งไปกี่ครั้ง
    # level -1 = ยังไม่รู้ รอบแรกจึงแจ้งเสมอ (บอกว่าฟาร์มออนไลน์) · t None = ต้องอ่านเดี๋ยวนี้

    def __init__(self, idx):
        self.level, self.acked, self.alerts, self.t = -1, True, 0, None
        self.crop = CROPS[idx]
        self.lim = list(self.crop[2:])   # [T ต่ำ, T สูง, RH ต่ำ, RH สูง]  set แก้ T สูงได้


def stop(w, msg, col=COL_BAD):
    # จบเพราะอะไรก็ตาม: หยุดตัววิ่ง ปิดกล่องเตือน แล้วบอกเหตุผล
    rgbmatrix.scroll("")               # หยุดตัววิ่งก่อน ไม่งั้นมันวิ่งต่อหลังโปรแกรมจบ
    rgbmatrix.clear()
    close_box(w)
    show_status(w, msg, col)
    ui.poll()
    raise SystemExit


def on_command(w, s, raw):
    # คำสั่งจากแอป (สัญญาข้อ 4) คืน True ถ้าเป็นการรับทราบ  คำสั่งที่ไม่รู้จัก = เงียบ ไม่ทำอะไร
    try:                               # ใครส่งอะไรมาก็ได้ ไม่ใช่ JSON object ก็ไม่ใช้
        cmd = json.loads(raw.decode())
        act = cmd.get("cmd")
    except Exception:
        return False
    if act == "ack":
        return True
    elif act == "set":                 # เกณฑ์ใหม่จากที่ไกล ต้องเป็นเลขจำนวนเต็มในช่วงที่สมเหตุผล
        v = cmd.get("t_hi")
        if isinstance(v, int) and s.lim[0] < v <= T_HI_MAX:
            s.lim[1], s.level, s.t = v, -1, None     # level -1 + t None = ตัดสินใหม่และแจ้งทันที
            show_crop(w, s)
            ui.sfx(ui.SFX_UI_SELECT)
    # >>> ภารกิจกลุ่ม: วาง elif สำหรับ "led"/"pump" จาก sf2_03 ตรงนี้ <<<
    return False


def on_level(w, s, level, why_th, why_en, t, h, sun):
    # ระดับเปลี่ยน: แจ้งออกเน็ต (สัญญาข้อ 3.4) แล้วค่อยเสียง จอไฟ RGB และกล่องเตือน
    s.level, s.acked = level, level < 2
    s.alerts += 1
    if not send({"id": TEAM, "crop": s.crop[1], "level": level, "alert": why_en,
                 "temp_c": round(t, 1), "rh": round(h, 1), "sun_c": sun}):
        stop(w, "สายหลุดตอนส่ง")
    rgbmatrix.scroll(MX[level][0], MX[level][1], 80)
    ui.sfx((ui.SFX_FLAPPY_SCORE, ui.SFX_UI_MOVE, ui.SFX_GAME_OVER)[level])
    show_ack(w, s, "รอคนรับทราบ" if level == 2 else "ส่งแล้ว", COL_BAD if level == 2 else COL_DIM)
    close_box(w)
    if level == 2:                     # บรรทัดแรก = หัวกล่อง ที่เหลือ = เนื้อความ (รวมไม่เกิน 126 ไบต์)
        w["box"] = ui.MsgBox("แย่แล้ว!\n" + why_th + "\n" + BTN_NAMES[0] + " / ack = รับทราบ",
                             x=BOX[0], y=BOX[1], w=BOX[2], h=BOX[3], value=0)


def read_and_judge(w, s):
    # อ่าน -> ตัดสิน -> โชว์ -> แจ้งถ้าเปลี่ยน  คืน True ถ้าเพิ่งแจ้ง (เริ่มนับรอบร้องซ้ำใหม่)
    sun = pots.read(2) * 10 // 4095    # VR3 = แดดจำลอง 0-10 C ที่บวกเข้าอุณหภูมิ
    t, h = read_climate()
    if t is None or h is None:
        s.t = None                     # อ่านไม่ได้ = ลองใหม่รอบหน้าเลย ไม่ต้องรอครบวินาที
        return False
    s.t = t = t + sun
    level, why_th, why_en = judge(t, h, s.lim)
    show_reading(w, s, t, h, sun, level, why_th)
    if level == s.level:               # ส่ง เล่นเสียง และเขียนจอไฟ RGB เฉพาะตอนเปลี่ยน
        return False
    on_level(w, s, level, why_th, why_en, t, h, sun)
    return True


def main():
    if not self_test():                # ตรวจกฎก่อน ไม่ผ่าน = ไม่เปิดจอ ไม่ต่อเน็ต
        return
    w = build_screen()
    names = [c[0] for c in CROPS]
    if CROP not in names:              # สะกดไม่ตรง = หยุด ไม่เดาให้ เพราะรหัสพืชจะถูกส่งออกไปผิดตัว
        stop(w, "แก้ CROP ให้ตรงกับ CROPS")
    s = Watch(names.index(CROP))
    show_crop(w, s)
    if not valid_team(TEAM):
        stop(w, "แก้ TEAM ก่อน")
    problem = connect_farm(w)
    if problem:
        stop(w, problem)
    show_status(w, "เฝ้าอยู่", COL_OK)
    was_down = False
    t_read = t_beep = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        msg = mqtt.get_message()       # ฟังทุก 20 ms แม้จะอ่านเซนเซอร์แค่ทุกวินาที
        ack = on_command(w, s, msg[1]) if msg else False
        down = buttons.pressed(0)      # SW5 (ปุ่มล่าง) = คนหน้าฟาร์มกดรับทราบเอง ไม่ต้องพึ่งเน็ต
        if down and not was_down:      # นับตอนเพิ่งกดลง กดค้างไม่นับซ้ำ
            ack = True
        was_down = down
        if ack and not s.acked:        # มีคนรับทราบ: หยุดร้อง ปิดกล่องเตือน จอไฟ RGB ขึ้น ACK
            s.acked = True
            close_box(w)
            show_ack(w, s, "รับทราบแล้ว", COL_OK)
            rgbmatrix.scroll("ACK", rgbmatrix.CYAN, 80)
            ui.sfx(ui.SFX_UI_SELECT)
        ui.poll()                      # ให้จอตอบสนอง (ไฟล์นี้ไม่มี widget ที่ต้องแตะ)
        if s.t is None or time.ticks_diff(now, t_read) >= READ_MS:
            t_read = now
            if read_and_judge(w, s):
                t_beep = now
        if s.level == 2 and not s.acked and time.ticks_diff(now, t_beep) >= BEEP_MS:
            t_beep = now               # ร้องซ้ำทุก 5 วิ ไม่ใช่ทุกรอบลูป ห้องมี 20 บอร์ด
            ui.tone(69, ui.WAVE_SQUARE, 90, 150)
        time.sleep_ms(POLL_MS)

    stop(w, "จบรอบ", COL_DIM)


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) หมุนแดดขึ้นลงเร็ว ๆ ข้ามเส้นไปมา นับว่าแจ้งเตือนรัวแค่ไหน (ดูการ์ดแจ้งสถานะ) แล้วแก้ให้ต้อง "แย่ติดกัน 5 วินาที"
#    ก่อนแจ้ง (เก็บตัวนับไว้ใน Watch แล้วเช็กใน read_and_judge ก่อนเรียก on_level)
# 2) แจ้งเตือนมี sun_c บอกว่าบวกแดดจำลองไปเท่าไร ถ้าไม่บอก ข้อมูลฟาร์มที่เก็บไว้จะเสียอย่างไร
# 3) เพิ่มพืชของกลุ่มลงใน CROPS (ตั้งรหัสอังกฤษให้แอปอ่านได้) แล้วตั้ง CROP เป็นพืชนั้น
#    ส่ง {"cmd":"set","t_hi":28} ระหว่างรัน แล้วดูเส้นแดงในกราฟขยับตาม
