# sf3_05_farm_all_in_one.py - ฟาร์มครบวงจรในไฟล์เดียว: บอร์ดของเราคือ Smart IoT Gateway ของฟาร์ม
#
# ภาพฟาร์มจริง : เซนเซอร์ (อากาศ ดิน เรดาร์ที่คอก) -> Dev Kit ตัดสินใจ -> สั่งรีเลย์ PLC WiFi ผ่าน MQTT
#               -> PLC เปิดพัดลม/ปั๊ม/ไซเรนจริง · แอปของกลุ่มดูทุกอย่าง และสั่งผ่าน Gateway (ไม่สั่ง PLC ตรง)
#               ในไฟล์นี้ "รีเลย์" คือไฟ Led บนจอ + จอไฟ RGB + ลำโพง (ต่อ PLC จริงได้ ดูข้อ 3 ท้ายไฟล์)
# ภารกิจ   : บอร์ดเดียวดูแลสามจุดของฟาร์ม และครบ "ห้าเสา" ของโปรเจกต์ในไฟล์เดียว
#            1) HMI      จอสัมผัสสามการ์ด + ปุ่มบนจอ "ส่งรายงานเลย"
#            2) เซนเซอร์ + ค่าตั้ง  SHT40/DPS368/เรดาร์ · VR2 = ดิน(จำลอง) · VR3 = เกณฑ์พัดลม 25-45 C
#                               VR4 = เขตคอก 50-250 cm · SW5 ค้าง = รดน้ำเอง · SW6 = รับทราบผู้บุกรุก
#            3) เสียง + จอไฟ RGB  เขียว = ปกติ · เหลือง = มีเครื่องทำงาน · แดงวิ่ง INTRUDER
#            4) MQTT ฝั่งบอร์ด  ส่ง .../telemetry ทุก 5 วิ · ส่ง .../event ตอนคอกเปลี่ยน · ฟัง .../cmd
#            5) แอปของกลุ่ม    s2/app/farm_monitor.py หรือ s2/app/farm_web.html ใส่ TEAM เดียวกัน
#               สั่งกลับมาได้: {"cmd":"pump","on":1,"sec":10} {"cmd":"pump","on":0} {"cmd":"beep"}
#               {"cmd":"ack"} (หรือ {"cmd":"silence"}) = รับทราบผู้บุกรุก
# ลองเล่น  : หมุน VR2 ลง (ดินแห้ง) · หมุน VR3 ลง (ให้พัดลมเปิด) · เดินเข้าหาบอร์ด แล้วกด SW6
#            กดปุ่ม "รดน้ำ 10 วินาที" ในหน้าเว็บ แล้วดูไฟปั๊มบนจอ · แตะ "ส่งรายงานเลย" แล้วดูบรรทัดล่างสุด
# ของบนบอร์ดที่ใช้ : SHT40 (อากาศ) DPS368 (ความกดอากาศ) เรดาร์ (ระยะคนที่คอก) ลูกบิด VR2-VR4
#            SW5 (ปุ่มล่าง) กดค้าง = รดน้ำเอง · SW6 (ปุ่มบน) = รับทราบผู้บุกรุก ปิดไซเรน
#            ลำโพง (ดังเฉพาะตอนมีเหตุ) · จอไฟ RGB 16x8 (ส่งซ้ำทุก 3 วิ เผื่อเฟรมหล่นตอนบอร์ดยุ่ง)
# บนจอ     : การ์ดโรงเรือน (อุณหภูมิ ความชื้น + ไฟพัดลม Led), การ์ดแปลงผัก (วงแหวนดิน Arc + ไฟปั๊ม Led),
#            การ์ดคอกสัตว์ (ระยะ + ไฟไซเรน Led), แถบสถานะเน็ต + คำสั่งจากแอป + ปุ่ม "ส่งรายงานเลย",
#            บรรทัดล่างสุด = JSON ใบล่าสุดที่บอร์ดส่ง (ความกดอากาศ hpa อยู่ในนี้และใน telemetry)
# แนวคิด AIoT: เน็ตหลุดฟาร์มต้องไม่หยุด - ต่อ WiFi/broker ไม่ได้ ไฟล์นี้ทำงานต่อแบบออฟไลน์
#            กฎทุกข้ออยู่บนบอร์ด เน็ตมีไว้รายงานและรับคำสั่ง ไม่ได้มีไว้ตัดสินใจแทน
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.2 ขึ้นไป · 2.4.1 ก็รันได้) และ BENTO Emulator
#            (ใน Emulator VR1 คือระยะเรดาร์ จึงใช้ VR2 แทนดิน สองอย่างจะได้ไม่ชนกัน)
# ต้องแก้ก่อนรัน: WIFI_SSID, WIFI_PASS และ TEAM · ยังไม่แก้ TEAM = ทำงานออฟไลน์ (บอกบนจอ)
#            broker ไม่เข้ารหัส ห้ามส่งของลับ
# สัญญา MQTT: s2/app/MQTT_CONTRACT_th.md (หัวข้อ bento-aiot/<ทีม>/... คีย์ temp_c rh hpa soil pump)
#            คีย์ที่ไฟล์นี้เพิ่ม: fan intruder intrusions (0/1 และจำนวนครั้ง) · event: intruder / clear

import buttons
import json
import math
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
TEAM = "teamXX"                        # เลขกลุ่มที่ผู้สอนแจก เช่น team05 (ต้องตรงกับแอป)

BROKER = "broker.hivemq.com"           # สำรอง: "test.mosquitto.org" ถ้าผู้สอนประกาศ
BASE = "bento-aiot/" + TEAM + "/"      # ชื่อนำหน้าเดียวกับคาบ 2 แอปของกลุ่มฟังชื่อนี้
BTN_NAMES = ("SW5", "SW6")             # ปุ่มล่าง = pressed(0), ปุ่มบน = pressed(1) ตามตัวอักษรบนแผง
TEMP_OFFSET = 0.0   # บอร์ดอุ่นจากชิปของตัวเอง: เทียบกับเทอร์โมมิเตอร์ในห้อง แล้วใส่ค่าชดเชย เช่น -7.0

FAN_LO, FAN_SPAN = 25, 20       # VR3 ตั้งเกณฑ์เปิดพัดลม 25-45 C · ปิดเมื่อเย็นกว่าเกณฑ์ 1 C
PUMP_ON, PUMP_OFF = 35, 45      # ดินต่ำกว่า 35 % เปิดปั๊ม เกิน 45 % ปิด (hysteresis)
GUARD_LO, GUARD_SPAN = 50, 200  # VR4 ตั้งเขตคอก 50-250 cm
TICK_MS, REPORT_MS, RUN_MS = 500, 5000, 600000   # วัด+วาดจอ / รายงาน / เวลารันทั้งหมด
VOLUME = 25                     # ความดังเสียง 0-127 (≈20%)
SPEAKER = 40             # ความดังลำโพงรวม 0-100% (ใช้ได้กับ firmware 2.4.2 ขึ้นไป)

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def beep(*notes):
    # เสียงเบา ๆ แทน ui.sfx (ui.sfx ดังคงที่ ปรับเบาไม่ได้) · เล่นโน้ต MIDI ทีละตัว ห่างกัน 120 ms
    for n in notes:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 120)
        time.sleep_ms(120)


class Button:
    # ปุ่มบนฐานบอร์ด (0 = SW5 ปุ่มล่าง, 1 = SW6 ปุ่มบน) ที่ไม่พลาดการกดสั้น ๆ
    # เฟิร์มแวร์กรองสัญญาณสั่น 50 ms ถ้าอ่านรอบละครั้งการกดแบบแตะจะหายไป จึงอ่านบ่อย ๆ ใน wait_ms

    def __init__(self, index):
        self.index, self.down, self.clicked = index, False, False

    def sample(self):
        now_down = buttons.pressed(self.index)
        if now_down and not self.down:
            self.clicked = True
        self.down = now_down

    def pressed_now(self):
        # True ครั้งเดียวต่อการกดหนึ่งครั้ง · อยากรู้ว่ายังกดค้างอยู่ไหม ดู .down
        fired, self.clicked = self.clicked, False
        return fired


def wait_ms(ms, btns):
    # รอ ms มิลลิวินาที แต่ระหว่างรอก็อ่านปุ่มทุก 20 ms เพื่อไม่พลาดการกดสั้น ๆ
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < ms:
        for b in btns:
            b.sample()
        time.sleep_ms(20)


def read_air():
    # คืน (อุณหภูมิ, ความชื้น, ความกดอากาศ) ของห้อง ปัดทศนิยม 1 ตำแหน่ง (จอกับ JSON เห็นเลขเดียวกัน)
    # ตัวไหนอ่านไม่ได้รอบนี้เป็น None รอบหน้าอ่านใหม่
    t = h = None
    for _ in range(3):                  # บัสไม่ว่างบางจังหวะ จึงลองอ่านได้ถึง 3 ครั้ง
        try:
            t_raw, h = sensors.sht40.temperature(), sensors.sht40.humidity()
            t = t_raw + TEMP_OFFSET
            if TEMP_OFFSET:             # ชดเชยอุณหภูมิแล้ว ความชื้นต้องแปลงเป็นของห้องด้วย
                h = room_humidity(h, t_raw, t)
            t, h = round(t, 1), round(h, 1)
            break
        except Exception:               # พังที่บรรทัดอ่าน t กับ h จึงยังเป็น None อยู่
            time.sleep_ms(20)
    try:
        p = round(sensors.dps368.pressure(), 1)
    except Exception:
        p = None
    return t, h, p


def room_humidity(h_raw, t_raw, t_room):
    # อากาศอุ่นขึ้นรอบเซนเซอร์ ความชื้นสัมพัทธ์จึงอ่านได้ต่ำกว่าห้อง
    # ไอน้ำเท่าเดิมแต่ห้องเย็นกว่า จึงคูณด้วยอัตราส่วนความดันไออิ่มตัว (สูตร Magnus)
    # es(t) = 6.112 * exp(17.62 t / (243.12 + t)) · หารกันแล้ว 6.112 ตัดกัน เหลือ exp ของผลต่าง
    return min(100.0, h_raw * math.exp(17.62 * t_raw / (243.12 + t_raw) - 17.62 * t_room / (243.12 + t_room)))


def read_cm():
    # ระยะเป้าหมายจากเรดาร์ (cm) ไม่มีเป้าหรือเรดาร์ไม่ตอบ = None
    try:
        rr = sensors.radar_range()
        return int(rr["distance_m"] * 100) if rr["target"] else None
    except Exception:
        return None


def radar_start():
    # จำฉากนิ่งก่อน (เกณฑ์ 0) แล้วค่อยตั้งเกณฑ์จริง (เหมือน sf3_01) · เรดาร์ไม่ตอบ = คืน False
    try:
        sensors.radar_config(0)
        time.sleep_ms(500)
        sensors.radar_config(4.0)       # เกณฑ์ความแรง 4 dB (ต่ำลง = ไวขึ้น แต่ใบไม้ไหวก็เตือน)
        return True
    except OSError:
        return False


def matrix(mode):
    # จอไฟ RGB: 0 เขียว = ปกติ · 1 เหลือง = มีเครื่องทำงาน · 2 แดงวิ่ง INTRUDER · -1 ดับ
    # ห่อ try ไว้ จอไฟพัง (OSError) ก็ไม่ลากฟาร์มหยุดตาม · รอบไหนหล่น อีก 3 วิ tick() ส่งซ้ำเอง
    try:
        rgbmatrix.scroll("")                     # หยุดตัวหนังสือวิ่งเดิมก่อน
        if mode == 2:
            rgbmatrix.scroll("INTRUDER", rgbmatrix.RED, 45)   # 45 ms/ช่อง วิ่งครบรอบก่อนส่งซ้ำ
        elif mode < 0:
            rgbmatrix.clear()
        else:
            rgbmatrix.fill(rgbmatrix.YELLOW if mode else rgbmatrix.GREEN)
    except OSError:
        pass


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ไม่แตะเน็ต ----
def decide(f, near):
    # กฎทั้งฟาร์มในที่เดียว คืน True ตอนคอกเปลี่ยน (ผู้บุกรุกเข้า/ออก)
    if f.t is not None:                 # อ่านอุณหภูมิไม่ได้ = พัดลมคงเดิม
        f.fan = f.t > f.fan_at or (f.fan and f.t > f.fan_at - 1)   # เย็นกว่าเกณฑ์ 1 C ถึงปิด
    f.auto = f.soil < PUMP_ON or (f.auto and f.soil < PUMP_OFF)    # ช่องตรงกลางกันปั๊มเปิดปิดรัว
    f.streak = f.streak + 1 if near != f.inside else 0            # เรดาร์ต้องเห็นเหมือนเดิม 3 รอบติด
    if f.streak < 3:                                              # ถึงเชื่อ (กันใบไม้ไหว)
        return False
    f.inside, f.streak, f.silenced = near, 0, False
    f.count += near                     # True นับเป็น 1 = นับเฉพาะตอนเข้า
    return True


def app_request(raw):
    # ข้อความจาก .../cmd (bytes) -> (คำสั่ง, วินาทีรดน้ำ) ตามสัญญาข้อ 4 · ไม่รู้จัก = (None, 0)
    try:
        cmd = json.loads(raw.decode())
        act = cmd.get("cmd")            # 5, null, [] ก็เป็น JSON ได้ แต่ไม่มี .get -> ไม่ใช่คำสั่ง
    except (ValueError, AttributeError):
        return None, 0
    if act == "silence":                # ชื่อเดิมของ ack (ปุ่มรับทราบใน farm_web.html ส่ง ack)
        act = "ack"
    if act == "pump" and cmd.get("on", 1):
        sec = cmd.get("sec", 10)
        return act, min(sec, 30) if isinstance(sec, int) and sec > 0 else 10   # ไม่เกิน 30 วิ
    return (act, 0) if act in ("pump", "ack", "beep") else (None, 0)          # pump on:0 = ปิด


# ---- 4) เครือข่าย ----
OFFLINE = "ออฟไลน์ (ทำงานต่อ)"          # เน็ตหลุดฟาร์มไม่หยุด แค่ไม่ได้รายงาน


def go_online(w):
    # WiFi -> broker -> ฟัง cmd · ขั้นไหนพังคืน False แล้วฟาร์มทำงานต่อแบบออฟไลน์
    # client id สร้างจาก TEAM: ถ้าหลายกลุ่มลืมแก้ teamXX จะชนกันแล้ว broker เตะกันหลุด จึงไม่ต่อเลย
    if not (len(TEAM) == 6 and TEAM[:4] == "team" and TEAM[4:].isdigit() and TEAM != "team00"):
        show_note(w, "แก้ TEAM ก่อน: " + OFFLINE, COL_WARN)
        return False
    show_note(w, "ต่อ WiFi...", COL_WARN)
    ui.poll()                          # ป้ายต้องขึ้นจอก่อนบรรทัดที่บล็อก
    try:
        ok = (wifi.connect(WIFI_SSID, WIFI_PASS) and wifi.ip() != "0.0.0.0"
              and mqtt.connect(BROKER, port=1883, client_id="bento-farm-" + TEAM, keepalive=60)
              and mqtt.subscribe(BASE + "cmd"))
    except OSError:
        ok = False
    show_note(w, ("ออนไลน์ " + TEAM) if ok else OFFLINE, COL_OK if ok else COL_WARN)
    return ok


def send(f, topic, obj):
    # ส่ง JSON ขึ้น broker แล้วคืนข้อความที่ส่ง · ออฟไลน์หรือสายหลุด (publish โยน OSError) ฟาร์มไม่หยุด
    line = json.dumps(obj)
    if f.online:
        try:
            mqtt.publish(BASE + topic, line)
        except OSError:
            pass
    return line


# ---- 5) หน้าจอ ----
def label(text, x, y, col=COL_DIM, size=16):
    # ป้ายข้อความหนึ่งอัน (size = ขนาดตัวอักษร 14/16/20/24/28)
    return ui.Label(text, x=x, y=y, color=col, value=size)


def led(x, y, col):
    # ไฟ Led = "รีเลย์" หนึ่งตัว สร้างมาแบบหรี่ = ปิด · .value(1) ติดเป็นสี col
    return ui.Led(x=x, y=y, w=36, h=36, color=col)


def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า (ทุกไฟล์ใช้แบบเดียวกัน)
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    if title:                           # None = กล่องเปล่า ไม่มีหัวเรื่อง
        label(title, x + 12, y + 6, COL_INFO)


def build_screen():
    # สร้างทุกอย่างบนจอครั้งเดียว แล้วคืน dict ของ widget ที่ต้องอัปเดตภายหลัง
    ui.screen()
    time.sleep_ms(200)
    label("Smart IoT Gateway", 12, 6, COL_TEXT, 24)
    w = {}
    card(12, 44, 252, 226, "โรงเรือน")
    w["t"] = label("--", 26, 80, COL_TEXT, 28)                 # อุณหภูมิ + ความชื้น
    w["fan"] = led(26, 176, COL_OK)
    w["fan_at"] = label(" ", 72, 184)
    card(272, 44, 252, 226, "แปลงผัก")
    w["arc"] = ui.Arc(x=284, y=76, w=112, h=112)                # Arc ตั้งต้น 0-100 อยู่แล้ว
    w["soil"] = label("--", 406, 112, COL_TEXT, 28)
    w["pump"] = led(286, 204, COL_INFO)
    label("ปั๊ม <%d%% เปิด >%d%% ปิด" % (PUMP_ON, PUMP_OFF), 332, 212, COL_DIM, 14)
    card(532, 44, 248, 226, "คอกสัตว์")
    w["cm"] = label("--", 546, 80, COL_TEXT, 28)
    w["guard"] = label(" ", 546, 124)
    w["siren"] = led(546, 176, COL_BAD)
    w["pen"] = label("ถอยห่างบอร์ด", 592, 184)       # เรดาร์กำลังจำฉากนิ่ง
    card(12, 278, 668, 62, None)        # แถบล่าง · เว้นมุมขวาไว้ให้ปุ่ม Console
    w["note"] = label(" ", 24, 286)
    w["cmd"] = label("แอป: -", 24, 312, COL_INFO)
    w["send"] = ui.Button("ส่งรายงานเลย", x=520, y=286, w=150, h=46).id()
    w["json"] = label(" ", 12, 352, COL_TEXT, 14)   # JSON ใบล่าสุดที่บอร์ดส่ง (n = ลำดับใบ)
    ui.poll()
    return w


def show_note(w, text, col):
    # ไม่เรียก ui.poll() ในนี้ เพราะจะกินเหตุการณ์แตะปุ่มที่ลูปหลักรออ่าน
    w["note"].color(col)
    w["note"].text(text)


def show_farm(w, f):
    # วาดค่าทั้งหมดใหม่ทุก TICK_MS · ไฟ Led = "รีเลย์" ที่ Gateway สั่งอยู่ตอนนี้
    w["t"].text("--" if f.t is None else "%.1f C  %.0f%%" % (f.t, f.h))   # อ่านไม่ได้ = --
    w["fan"].value(int(f.fan))
    w["fan_at"].text("พัดลม > %.1f C (VR3)" % f.fan_at)
    w["arc"].value(f.soil)
    w["arc"].color(COL_BAD if f.soil < PUMP_ON else COL_OK)    # แดง = แห้งกว่าเกณฑ์เปิดปั๊ม
    w["soil"].text("%d %%" % f.soil)
    w["pump"].value(int(f.pump))
    w["cm"].text("--" if f.cm is None else "%d cm" % f.cm)
    w["guard"].text("เขต %d cm (VR4)" % f.guard)
    siren = f.inside and not f.silenced
    w["siren"].value(int(siren))
    w["pen"].text(("ไซเรน! กด " + BTN_NAMES[1]) if siren else "บุกรุก %d ครั้ง" % f.count)


# ---- 6) โปรแกรมหลัก ----
class Farm:
    # ทุกอย่างที่ Gateway รู้และตัดสินไว้ตอนนี้ (ค่าที่วัด + สถานะเครื่อง + สถานะคอก)

    def __init__(self):
        self.t = self.h = self.p = self.cm = self.soil = self.shown = None
        self.fan = self.auto = self.pump = self.inside = self.silenced = self.online = False
        self.streak = self.count = self.n = self.remote = self.t_mx = 0


def tick(w, f, down, now):
    # ทุก TICK_MS: วัด -> ตัดสิน -> สั่ง (ไฟ Led จอไฟ เสียง event) -> วาดจอ · down = SW5 ค้างอยู่ไหม
    f.t, f.h, f.p = read_air()
    f.soil = pots.read(1) * 100 // 4095
    f.fan_at = FAN_LO + pots.read(2) * FAN_SPAN / 4095
    f.guard = GUARD_LO + pots.read(3) * GUARD_SPAN // 4095
    f.cm = read_cm()
    # f.remote > 0 = แอปสั่งรดอีกกี่ ms · < 0 = แอปสั่งปิด งดรดอัตโนมัติอีกกี่ ms
    f.remote -= max(-TICK_MS, min(TICK_MS, f.remote))   # ขยับเข้าหา 0 ทีละไม่เกิน TICK_MS ทั้งสองทาง
    if decide(f, f.cm is not None and f.cm < f.guard):     # เสียง + event เฉพาะตอนคอกเปลี่ยน
        beep(84, 76) if f.inside else beep(79, 84)
        send(f, "event", {"id": TEAM, "event": "intruder" if f.inside else "clear", "cm": f.cm})
    f.pump = (f.auto and f.remote >= 0) or down or f.remote > 0   # กด SW5 ค้างยังรดได้เสมอ
    show_farm(w, f)
    mode = 2 if f.inside and not f.silenced else int(f.fan or f.pump)
    if mode != f.shown or time.ticks_diff(now, f.t_mx) >= 3000:     # ส่งซ้ำทุก 3 วิ เผื่อเฟรมหล่น
        f.shown, f.t_mx = mode, now
        matrix(mode)


def check_net(w, f):
    # สายหลุด = ออฟไลน์ (ฟาร์มไม่หยุด) · ออนไลน์ = หยิบคำสั่งจากกล่อง (มีช่องเดียว จึงถามทุก 0.1 วิ)
    if f.online and not mqtt.is_connected():
        f.online = False
        show_note(w, OFFLINE, COL_WARN)
    msg = mqtt.get_message() if f.online else None      # None หรือ (topic, bytes)
    if msg:
        act, sec = app_request(msg[1])
        if act == "pump":
            f.remote = sec * 1000 if sec else -30000   # แอปสั่งปิด = ปิดทันที และงดรดอัตโนมัติ 30 วิ
        f.silenced = f.silenced or act == "ack"
        if act == "beep":
            beep(69)                    # โน้ต MIDI ไม่ใช่เฮิรตซ์ = เรียกเจ้าของ
        else:
            beep(76) if act else beep(84, 76)
        w["cmd"].text("แอป: %s %d วิ" % (act, sec) if act == "pump" else "แอป: " + str(act or "?"))


def report(w, f, by):
    # รายงานค่าฟาร์มเข้า telemetry · ออฟไลน์ก็ยังพิมพ์ลง Console และขึ้นบรรทัดล่างสุดของจอ
    f.n += 1
    line = send(f, "telemetry", {"id": TEAM, "n": f.n, "temp_c": f.t, "rh": f.h, "hpa": f.p,
                                 "soil": f.soil, "fan": int(f.fan), "pump": int(f.pump),
                                 "intruder": int(f.inside), "intrusions": f.count, "sim": "soil", "by": by})
    print(line)
    w["json"].text(line[:76])


def main():
    if hasattr(ui, "volume"):    # บอร์ดที่ยังเป็น 2.4.1 ข้ามบรรทัดนี้
        ui.volume(SPEAKER)
    w = build_screen()
    f = Farm()
    f.online = go_online(w)
    if not radar_start():
        w["cmd"].text("เรดาร์ไม่ตอบ")        # คอกไม่ได้เฝ้า แต่ส่วนอื่นของฟาร์มทำงานต่อ
    beep(72, 79)
    water, ack = Button(0), Button(1)
    t0 = t_tick = t_rep = time.ticks_ms()
    try:
        while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
            wait_ms(100, (water, ack))          # ถามกล่องคำสั่งทุก 0.1 วิ (กล่องมีช่องเดียว)
            now = time.ticks_ms()
            force = False
            for ev in ui.poll():                # ปุ่มบนจอสัมผัส (อ่านที่นี่ที่เดียว)
                force = force or ev["handle"] == w["send"]
            check_net(w, f)
            if ack.pressed_now() and f.inside:
                f.silenced = True               # SW6 = รับทราบ ไซเรนเงียบจนคอกเปลี่ยนอีกครั้ง
                beep(76)
            if time.ticks_diff(now, t_tick) >= TICK_MS:
                t_tick = now
                tick(w, f, water.down, now)
            if force or time.ticks_diff(now, t_rep) >= REPORT_MS:
                t_rep = now
                report(w, f, "touch" if force else "timer")
    finally:                                    # หยุดกลางทางก็ดับจอไฟ RGB และตัดสาย broker ให้เรียบร้อย
        matrix(-1)
        if f.online:
            mqtt.disconnect()
    show_note(w, "จบรอบ", COL_DIM)


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เพิ่มกฎข้ามระบบใน decide(): "มีผู้บุกรุก ห้ามเปิดปั๊ม" แล้วส่ง event บอกแอปว่าปั๊มถูกล็อกเพราะอะไร
# 2) ในแอปของกลุ่ม (s2/app/) เพิ่มกฎ: รายงานหายเกิน 15 วิ = ขึ้นเตือน "บอร์ดเงียบ" (ฟาร์มดับหรือเน็ตหลุด)
# 3) ต่อ PLC จริง: รัน s2/app/field_sim.py (TEAM เดียวกัน) แล้วให้ tick() ส่ง {"pump":1,"sec":10} เข้า
#    "plc/cmd" ตอนปั๊มเปลี่ยนเป็นเปิด และ {"pump":0} ตอนปิด (สัญญาข้อ 4.2) ดูว่า PLC ตอบอะไรใน plc/state
