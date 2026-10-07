# sf3_05_farm_all_in_one.py - ฟาร์มครบวงจรในไฟล์เดียว: บอร์ดคือ Smart IoT Gateway
# ภารกิจ: ครบห้าเสา · ไฟ Led บนจอ + จอไฟ RGB + ลำโพง = "รีเลย์" (ต่อ PLC ดูข้อ 3 ท้ายไฟล์)
# ลองเล่น: VR2 ลง (ดินแห้ง) · VR3 ลง (พัดลม) · เดินเข้าหาบอร์ดแล้วกด SW6 · แตะ "ส่งรายงานเลย"
# ของบนบอร์ดที่ใช้: SHT40 DPS368 เรดาร์ · VR2 ดิน(จำลอง) VR3 เกณฑ์พัดลม VR4 เขตคอก
#   SW5 (ล่าง) ค้าง = รดน้ำ · SW6 (บน) = รับทราบผู้บุกรุก
# บนจอ: การ์ดโรงเรือน แปลงผัก คอกสัตว์ · บรรทัดล่างสุด = JSON ใบล่าสุด
# แนวคิด AIoT: กฎอยู่บนบอร์ด เน็ตหลุดฟาร์มไม่หยุด
# บอร์ด: TESAIoT Dev Kit (firmware 2.4.1 ขึ้นไป) และ BENTO Emulator
# ต้องแก้ก่อนรัน: WIFI_SSID WIFI_PASS TEAM · broker ไม่เข้ารหัส ห้ามส่งของลับ
# สัญญา MQTT: s2/app/MQTT_CONTRACT_th.md · แอปสั่ง {"cmd":"pump","on":1,"sec":10}
#   {"cmd":"pump","on":0} {"cmd":"beep"} {"cmd":"ack"}

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
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"  # ชื่อ ไม่มีเว้นวรรค
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"  # รหัส 8 ตัวขึ้นไป
TEAM = "teamXX"  # team01-team99 (ตรงกับแอป)

BROKER = "broker.hivemq.com"  # สำรอง test.mosquitto.org
CLIENT_ID = "bento-farm-" + TEAM + "-%04x" % (time.ticks_ms() & 0xFFFF)  # ท้ายสุ่ม ไม่ชน id เก่า
BASE = "bento-aiot/" + TEAM + "/"  # หัวข้อ MQTT
BTN_NAMES = ("SW5", "SW6")  # ชื่อปุ่มล่าง/บน
TEMP_OFFSET = 0.0  # ชดเชยอุณหภูมิ C เช่น -7.0

FAN_LO, FAN_SPAN = 25, 20  # เกณฑ์พัดลม 25-45 C (VR3)
PUMP_ON, PUMP_OFF = 35, 45  # ดิน % เปิด/ปิดปั๊ม
GUARD_LO, GUARD_SPAN = 50, 200  # เขตคอก 50-250 cm (VR4)
TICK_MS, REPORT_MS, RUN_MS = 500, 5000, 600000  # ms วัด/รายงาน/รันทั้งหมด
VOLUME = 25  # เสียง 0-127 (≈20%)
SPEAKER = 40  # ลำโพง 0-100% (2.4.2+)

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF  # สีตัวอักษร
COL_CARD = 0x171B22  # สีการ์ด
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF  # สีสถานะ


# ---- 2) ฮาร์ดแวร์ ----
def beep(*notes):
    # โน้ต MIDI เบา ๆ (ui.sfx ปรับเบาไม่ได้)
    for n in notes:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 120)
        time.sleep_ms(120)


class Button:
    # อ่านบ่อยใน wait_ms จึงไม่พลาดการกดสั้น

    def __init__(self, index):
        self.index, self.down, self.clicked = index, False, False

    def sample(self):
        now_down = buttons.pressed(self.index)
        if now_down and not self.down:
            self.clicked = True
        self.down = now_down

    def pressed_now(self):
        fired, self.clicked = self.clicked, False
        return fired


def wait_ms(ms, btns):
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < ms:
        for b in btns:
            b.sample()
        time.sleep_ms(20)


def read_air():
    # อ่านไม่ได้ = None
    t = h = None
    for _ in range(3):
        try:
            t_raw, h = sensors.sht40.temperature(), sensors.sht40.humidity()
            t = t_raw + TEMP_OFFSET
            if TEMP_OFFSET:
                h = room_humidity(h, t_raw, t)
            t, h = round(t, 1), round(h, 1)
            break
        except Exception:
            time.sleep_ms(20)
    try:
        p = round(sensors.dps368.pressure(), 1)
    except Exception:
        p = None
    return t, h, p


def room_humidity(h_raw, t_raw, t_room):
    # ความชื้นของห้อง (สูตร Magnus)
    return min(100.0, h_raw * math.exp(17.62 * t_raw / (243.12 + t_raw) - 17.62 * t_room / (243.12 + t_room)))


def read_cm():
    # ไม่มีเป้า = None
    try:
        rr = sensors.radar_range()
        return int(rr["distance_m"] * 100) if rr["target"] else None
    except Exception:
        return None


def radar_start():
    # จำฉากนิ่ง แล้วตั้งเกณฑ์ 4 dB
    try:
        sensors.radar_config(0)
        time.sleep_ms(500)
        sensors.radar_config(4.0)
        return True
    except OSError:
        return False


def matrix(mode):
    # 0 เขียว · 1 เหลือง · 2 INTRUDER · -1 ดับ
    try:
        rgbmatrix.scroll("")
        if mode == 2:
            rgbmatrix.scroll("INTRUDER", rgbmatrix.RED, 45)
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
    # -> (คำสั่ง, วินาที) · ไม่รู้จัก = (None, 0)
    try:
        cmd = json.loads(raw.decode())
        act = cmd.get("cmd")
    except (ValueError, AttributeError):
        return None, 0
    if act == "pump" and cmd.get("on", 1):
        sec = cmd.get("sec", 10)
        return act, min(sec, 30) if isinstance(sec, int) and not isinstance(sec, bool) and sec > 0 else 10
    return (act, 0) if act in ("pump", "ack", "beep") else (None, 0)


# ---- 4) เครือข่าย ----
OFFLINE = "ออฟไลน์ (ทำงานต่อ)"


def go_online(w):
    # ไม่แก้ TEAM = ไม่ต่อ (id ชนกัน)
    if not (len(TEAM) == 6 and TEAM[:4] == "team" and TEAM[4:].isdigit() and TEAM != "team00"):
        show_note(w, "แก้ TEAM ก่อน: " + OFFLINE, COL_WARN)
        return False
    show_note(w, "ต่อ WiFi...", COL_WARN)
    ui.poll()
    try:
        ok = (wifi.connect(WIFI_SSID, WIFI_PASS) and wifi.ip() != "0.0.0.0"
              and mqtt.connect(BROKER, port=1883, client_id=CLIENT_ID, keepalive=60)
              and mqtt.subscribe(BASE + "cmd"))
    except OSError:
        ok = False
    show_note(w, ("ออนไลน์ " + TEAM) if ok else OFFLINE, COL_OK if ok else COL_WARN)
    return ok


def send(f, topic, obj):
    line = json.dumps(obj)
    if f.online:
        try:
            mqtt.publish(BASE + topic, line)
        except OSError:
            pass
    return line


# ---- 5) หน้าจอ ----
def label(text, x, y, col=COL_DIM, size=16):
    return ui.Label(text, x=x, y=y, color=col, value=size)


def led(x, y, col):
    return ui.Led(x=x, y=y, w=36, h=36, color=col)


def card(x, y, w, h, title):
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    if title:
        label(title, x + 12, y + 6, COL_INFO)


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    label("Smart IoT Gateway", 12, 6, COL_TEXT, 24)
    w = {}
    card(12, 44, 252, 226, "โรงเรือน")
    w["t"] = label("--", 26, 80, COL_TEXT, 28)
    w["fan"] = led(26, 176, COL_OK)
    w["fan_at"] = label(" ", 72, 184)
    card(272, 44, 252, 226, "แปลงผัก")
    w["arc"] = ui.Arc(x=284, y=76, w=112, h=112)
    w["soil"] = label("--", 406, 112, COL_TEXT, 28)
    w["pump"] = led(286, 204, COL_INFO)
    label("ปั๊ม <%d%% เปิด >%d%% ปิด" % (PUMP_ON, PUMP_OFF), 332, 212, COL_DIM, 14)
    card(532, 44, 248, 226, "คอกสัตว์")
    w["cm"] = label("--", 546, 80, COL_TEXT, 28)
    w["guard"] = label(" ", 546, 124)
    w["siren"] = led(546, 176, COL_BAD)
    w["pen"] = label("ถอยห่างบอร์ด", 592, 184)
    card(12, 278, 668, 62, None)  # เว้นมุมขวาให้ปุ่ม Console
    w["note"] = label(" ", 24, 286)
    w["cmd"] = label("แอป: -", 24, 312, COL_INFO)
    w["send"] = ui.Button("ส่งรายงานเลย", x=520, y=286, w=150, h=46).id()
    w["json"] = label(" ", 12, 352, COL_TEXT, 14)
    ui.poll()
    return w


def show_note(w, text, col):
    # ห้าม ui.poll() ในนี้ (กินการแตะของลูปหลัก)
    w["note"].color(col)
    w["note"].text(text)


def show_farm(w, f):
    w["t"].text("--" if f.t is None else "%.1f C  %.0f%%" % (f.t, f.h))
    w["fan"].value(int(f.fan))
    w["fan_at"].text("พัดลม > %.1f C (VR3)" % f.fan_at)
    w["arc"].value(f.soil)
    w["arc"].color(COL_BAD if f.soil < PUMP_ON else COL_OK)
    w["soil"].text("%d %%" % f.soil)
    w["pump"].value(int(f.pump))
    w["cm"].text("--" if f.cm is None else "%d cm" % f.cm)
    w["guard"].text("เขต %d cm (VR4)" % f.guard)
    siren = f.inside and not f.silenced
    w["siren"].value(int(siren))
    w["pen"].text(("ไซเรน! กด " + BTN_NAMES[1]) if siren else "บุกรุก %d ครั้ง" % f.count)


# ---- 6) โปรแกรมหลัก ----
class Farm:
    def __init__(self):
        self.t = self.h = self.p = self.cm = self.soil = self.shown = None
        self.fan = self.auto = self.pump = self.inside = self.silenced = self.online = False
        self.streak = self.count = self.n = self.remote = self.t_mx = 0


def tick(w, f, down, now):
    # down = SW5 ค้าง
    f.t, f.h, f.p = read_air()
    f.soil = pots.read(1) * 100 // 4095
    f.fan_at = FAN_LO + pots.read(2) * FAN_SPAN / 4095
    f.guard = GUARD_LO + pots.read(3) * GUARD_SPAN // 4095
    f.cm = read_cm()
    # remote >0 = แอปสั่งรดอีกกี่ ms · <0 = งดรดอัตโนมัติอีกกี่ ms
    f.remote -= max(-TICK_MS, min(TICK_MS, f.remote))
    if decide(f, f.cm is not None and f.cm < f.guard):
        beep(84, 76) if f.inside else beep(79, 84)
        send(f, "event", {"id": TEAM, "event": "intruder" if f.inside else "clear", "cm": f.cm})
    f.pump = (f.auto and f.remote >= 0) or down or f.remote > 0
    show_farm(w, f)
    mode = 2 if f.inside and not f.silenced else int(f.fan or f.pump)
    if mode != f.shown or time.ticks_diff(now, f.t_mx) >= 3000:  # ส่งซ้ำ เผื่อเฟรมหล่น
        f.shown, f.t_mx = mode, now
        matrix(mode)


def check_net(w, f):
    if f.online and not mqtt.is_connected():
        f.online = False
        show_note(w, OFFLINE, COL_WARN)
    msg = mqtt.get_message() if f.online else None
    if msg:
        act, sec = app_request(msg[1])
        if act == "pump":
            f.remote = sec * 1000 if sec else -30000
        f.silenced = f.silenced or act == "ack"
        if act == "beep":
            beep(69)
        else:
            beep(76) if act else beep(84, 76)
        w["cmd"].text("แอป: %s %d วิ" % (act, sec) if act == "pump" else "แอป: " + str(act or "?"))


def report(w, f, by):
    f.n += 1
    line = send(f, "telemetry", {"id": TEAM, "n": f.n, "temp_c": f.t, "rh": f.h, "hpa": f.p,
                                 "soil": f.soil, "fan": int(f.fan), "pump": int(f.pump),
                                 "intruder": int(f.inside), "intrusions": f.count, "sim": "soil", "by": by})
    print(line)
    w["json"].text(line[:76])


def main():
    if hasattr(ui, "volume"):  # 2.4.1 ข้าม
        ui.volume(SPEAKER)
    w = build_screen()
    f = Farm()
    f.online = go_online(w)
    if not radar_start():
        w["cmd"].text("เรดาร์ไม่ตอบ")
    beep(72, 79)
    water, ack = Button(1), Button(0)
    t0 = t_tick = t_rep = time.ticks_ms()
    try:
        while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
            wait_ms(100, (water, ack))  # กล่องคำสั่งมีช่องเดียว
            now = time.ticks_ms()
            force = False
            for ev in ui.poll():
                force = force or ev["handle"] == w["send"]
            check_net(w, f)
            if ack.pressed_now() and f.inside:
                f.silenced = True  # เงียบจนคอกเปลี่ยน
                beep(76)
            if time.ticks_diff(now, t_tick) >= TICK_MS:
                t_tick = now
                tick(w, f, water.down, now)
            if force or time.ticks_diff(now, t_rep) >= REPORT_MS:
                t_rep = now
                report(w, f, "touch" if force else "timer")
    finally:
        matrix(-1)
        if f.online:
            mqtt.disconnect()
    show_note(w, "จบรอบ", COL_DIM)


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) decide(): มีผู้บุกรุก ห้ามเปิดปั๊ม + ส่ง event บอกแอปว่าทำไม
# 2) แอป (s2/app/): รายงานหายเกิน 15 วิ = เตือน "บอร์ดเงียบ"
# 3) PLC: เปิด PLC Simulator ใน s2/app/farm_web.html ให้ tick() ส่ง {"pump":1,"sec":10} / {"pump":0}
#    เข้า "plc/cmd" ตอนปั๊มเปิด/ปิด ดูคำตอบใน plc/state
