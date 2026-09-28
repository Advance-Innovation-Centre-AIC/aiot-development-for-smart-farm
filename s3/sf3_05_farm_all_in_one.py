# sf3_05_farm_all_in_one.py - Smart HMI ฟาร์มครบวงจร: แม่แบบโปรเจกต์ของกลุ่ม
#
# ภารกิจ   : บอร์ดเดียวดูแลสามจุดของฟาร์ม และครบ "ห้าเสา" ของโปรเจกต์ในไฟล์เดียว
#            1) HMI     จอสัมผัสสามการ์ด + ปุ่มบนจอ "ส่งรายงานเลย"
#            2) เซนเซอร์ + ค่าตั้ง  SHT40/DPS368/เรดาร์ · VR2 = ดิน(จำลอง) VR3 = เกณฑ์พัดลม VR4 = เขตคอก
#                               SW4 ค้าง = รดน้ำเอง · SW5 = รับทราบผู้บุกรุก ปิดไซเรน
#            3) เสียง + RGB matrix  เขียว = ปกติ · เหลือง = มีเครื่องทำงาน · แดงวิ่ง INTRUDER
#            4) MQTT ฝั่งบอร์ด  ส่ง .../telemetry ทุก 5 วิ · ส่ง .../event ตอนเกิดเหตุ · ฟัง .../cmd
#            5) แอปของกลุ่ม    เปิด s2/app/farm_monitor.py หรือ s2/app/farm_web.html ใส่ TEAM เดียวกัน
#               แล้วสั่งกลับมาได้: {"cmd":"pump","on":1,"sec":10} {"cmd":"beep"} {"cmd":"silence"}
# ลองเล่น  : หมุน VR2 ลง (ดินแห้ง) · หมุน VR3 ลง (ให้พัดลมเปิด) · เดินเข้าหาบอร์ด · สั่งปั๊มจากแอป
# แนวคิด AIoT: เน็ตหลุดฟาร์มต้องไม่หยุด - ถ้าต่อ WiFi/broker ไม่ได้ ไฟล์นี้ทำงานต่อแบบออฟไลน์
#            (กฎทุกข้ออยู่บนบอร์ด เน็ตมีไว้รายงานและรับคำสั่ง ไม่ได้มีไว้ตัดสินใจแทน)
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.1 ขึ้นไป) และ BENTO Emulator
#            (ใน Emulator VR1 คือระยะเรดาร์ จึงใช้ VR2 แทนดิน สองอย่างจะได้ไม่ชนกัน)
# ต้องแก้ก่อนรัน: WIFI_SSID, WIFI_PASS และ TEAM (เหมือนคาบ 2) · broker ไม่เข้ารหัส ห้ามส่งของลับ

import buttons
import json
import mqtt
import pots
import rgbmatrix
import sensors
import time
import ui
import wifi

# ----- แก้สามบรรทัดนี้ -----
WIFI_SSID = "bento-teamXX"
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"
TEAM = "teamXX"                        # team01 ถึง team20

BROKER = "broker.hivemq.com"
ROOT = "bento-aiot"                    # ชื่อนำหน้าเดียวกับคาบ 2 แอปของกลุ่มฟังชื่อนี้
TOPIC_TELE = ROOT + "/" + TEAM + "/telemetry"
TOPIC_EVENT = ROOT + "/" + TEAM + "/event"
TOPIC_CMD = ROOT + "/" + TEAM + "/cmd"

FAN_LO, FAN_SPAN = 25.0, 10.0   # VR3 ตั้งเกณฑ์เปิดพัดลม 25-35 C · ปิดเมื่อเย็นกว่าเกณฑ์ 1 C
PUMP_ON, PUMP_OFF = 35, 45      # ดินต่ำกว่า 35 % เปิดปั๊ม เกิน 45 % ปิด (hysteresis)
GUARD_LO, GUARD_SPAN = 50, 200  # VR4 ตั้งเขตคอก 50-250 cm
REMOTE_MAX_S = 30               # แอปสั่งรดน้ำนานแค่ไหนก็ได้ไม่เกินนี้
CONFIRM_N = 3
THRESH_DB = 4.0
REPORT_MS, SENSE_MS, TICK_MS = 5000, 500, 100   # ถามกล่องคำสั่งทุก 100 ms (กล่องมีช่องเดียว)
RUN_MS = 600000

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF

online = False


def r1(v):
    return None if v is None else round(v, 1)


def read_air():
    try:
        t, h = sensors.sht40.temperature(), sensors.sht40.humidity()
    except Exception:
        t = h = None
    try:
        p = sensors.dps368.pressure()
    except Exception:
        p = None
    return t, h, p


def read_cm():
    try:
        rr = sensors.radar_range()
        return int(rr["distance_m"] * 100) if rr["target"] else None
    except Exception:
        return None


def status_light(mode):
    """ไฟสถานะบน RGB matrix วาดเฉพาะตอนสถานะเปลี่ยน (เต็มจอ 128 ดวง = คำสั่งเดียว)"""
    try:
        rgbmatrix.scroll("")                     # หยุดตัวหนังสือวิ่งเดิมก่อน
        if mode == "intruder":
            rgbmatrix.scroll("INTRUDER", rgbmatrix.RED, 60)
        elif mode == "off":
            rgbmatrix.clear()
        else:
            rgbmatrix.fill(rgbmatrix.YELLOW if mode == "busy" else rgbmatrix.GREEN)
    except OSError:
        pass


def send(topic, obj):
    """ส่งขึ้น broker ถ้าสายหลุดก็แค่เปลี่ยนเป็นออฟไลน์ ฟาร์มไม่หยุด"""
    global online
    if online:
        try:
            return mqtt.publish(topic, json.dumps(obj))
        except OSError:
            online = False
    return False


ui.screen()
time.sleep_ms(200)
ui.Label("ฟาร์มอัจฉริยะของกลุ่ม", x=20, y=8, color=COL_TEXT, value=24)
net = ui.Label("กำลังต่อ WiFi จอจะนิ่งสักครู่", x=330, y=14, color=COL_WARN, value=16)


def card(x, title):
    ui.Panel(x=x, y=48, w=246, h=232, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 14, y=56, color=COL_INFO, value=18)
    a = ui.Label("-", x=x + 14, y=90, color=COL_TEXT, value=24)
    b = ui.Label("-", x=x + 14, y=128, color=COL_DIM, value=16)
    c = ui.Label("-", x=x + 14, y=156, color=COL_DIM, value=16)
    act = ui.Label("-", x=x + 14, y=206, color=COL_DIM, value=28)
    return a, b, c, act


g_val, g_sub, g_set, g_act = card(20, "โรงเรือน")
s_val, s_sub, s_set, s_act = card(277, "แปลงผัก")
p_val, p_sub, p_set, p_act = card(534, "คอกสัตว์")
ui.Panel(x=20, y=290, w=660, h=98, color=COL_CARD, min=COL_DIM, max=12, value=1)  # เว้นมุมปุ่ม Console
rep_n = ui.Label("รายงาน 0 ครั้ง", x=36, y=298, color=COL_DIM, value=16)
cmd_lbl = ui.Label("ยังไม่มีคำสั่งจากแอป", x=36, y=324, color=COL_DIM, value=16)
rep_line = ui.Label("-", x=36, y=354, color=COL_TEXT, value=14)
send_id = ui.Button("ส่งรายงานเลย", x=520, y=300, w=150, h=40).id()
s_set.text("เปิด<%d%% ปิด>%d%%" % (PUMP_ON, PUMP_OFF))
p_sub.text("จำฉากนิ่ง... ถอยห่างบอร์ด")
ui.poll()

try:                                           # เสา 4: ต่อเน็ต ถ้าไม่ได้ก็ทำงานต่อแบบออฟไลน์
    if wifi.connect(WIFI_SSID, WIFI_PASS) and wifi.ip() != "0.0.0.0":
        online = bool(mqtt.connect(BROKER, port=1883, client_id="bento-farm-" + TEAM,
                                   keepalive=60)) and bool(mqtt.subscribe(TOPIC_CMD))
except OSError:
    online = False
net.color(COL_OK if online else COL_WARN)
net.text(("ออนไลน์ " + TEAM) if online else "ออฟไลน์ (ฟาร์มยังทำงานต่อ)")
sensors.radar_config(0)                        # จำฉากนิ่ง แล้วตั้งเกณฑ์ (เหมือน sf3_01)
time.sleep_ms(500)
sensors.radar_config(THRESH_DB)
ui.sfx(ui.SFX_UI_START)


def handle(raw):
    """เสา 5: คำสั่งจากแอปของกลุ่ม -> ข้อความสั้น ๆ สำหรับขึ้นจอ"""
    global remote_ms, remote_t0, silenced
    try:
        cmd = json.loads(raw.decode())
    except ValueError:
        cmd = None
    if not isinstance(cmd, dict):              # 5, null, [] ก็เป็น JSON ได้ ต้องกันไว้
        ui.sfx(ui.SFX_UI_DENY)
        return "อ่านคำสั่งไม่ออก"
    act = cmd.get("cmd", "")
    if act == "pump" and not cmd.get("on", 1):
        remote_ms = 0
        return "แอปสั่งปิดปั๊ม"
    if act == "pump":
        sec = cmd.get("sec", 10)
        sec = min(sec, REMOTE_MAX_S) if isinstance(sec, int) and sec > 0 else 10
        remote_ms, remote_t0 = sec * 1000, time.ticks_ms()
        ui.sfx(ui.SFX_UI_SELECT)
        return "แอปสั่งรดน้ำ %d วิ" % sec
    if act == "beep":
        ui.tone(69, ui.WAVE_SQUARE, 90, 150)   # โน้ต MIDI ไม่ใช่เฮิรตซ์
        return "แอปเรียกหาเจ้าของ!"
    if act == "silence":
        silenced = True
        return "แอปรับทราบผู้บุกรุก"
    ui.sfx(ui.SFX_UI_DENY)
    return "ไม่รู้จักคำสั่ง " + str(act)[:10]


fan_on = pump_on = inside = silenced = force = False
t = h = p = cm = None
soil = streak = count = sent = remote_ms = remote_t0 = 0
shown = None
prev = buttons.read()
t0 = last_sense = last_rep = time.ticks_ms()
while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
    now = time.ticks_ms()
    b = buttons.read()                          # เสา 1-2: ปุ่มจริง (SW4 ค้างไหม, SW5 ค้างไหม)
    if b[1] and not prev[1] and inside:         # ขอบกด SW5 = รับทราบแล้ว
        silenced = True
        ui.sfx(ui.SFX_UI_BACK)
    prev = b
    for ev in ui.poll():                        # ปุ่มบนจอสัมผัส
        if ev.get("handle") == send_id:
            force = True
    if online:
        msg = mqtt.get_message()                # None หรือ (topic, bytes)
        if msg is not None:
            cmd_lbl.color(COL_INFO)
            cmd_lbl.text(handle(msg[1])[:24])
        if not mqtt.is_connected():
            online = False
            net.color(COL_WARN)
            net.text("เน็ตหลุด ทำงานต่อแบบออฟไลน์")
    if remote_ms and time.ticks_diff(now, remote_t0) >= remote_ms:
        remote_ms = 0                           # ครบเวลาที่แอปสั่ง ปั๊มหยุดเอง

    if time.ticks_diff(now, last_sense) >= SENSE_MS:
        last_sense = now
        t, h, p = read_air()                    # วัด
        soil = pots.read(1) * 100 // 4095
        fan_at = FAN_LO + pots.read(2) * FAN_SPAN / 4095
        guard_cm = GUARD_LO + pots.read(3) * GUARD_SPAN // 4095
        cm = read_cm()
        if t is not None:                       # ตัดสิน
            fan_on = t > fan_at or (fan_on and t > fan_at - 1.0)
        pump_on = soil < PUMP_ON or (pump_on and soil < PUMP_OFF)
        near = cm is not None and cm < guard_cm
        streak = streak + 1 if near != inside else 0
        if streak >= CONFIRM_N:
            inside, streak, silenced = near, 0, False
            count += 1 if inside else 0
            ui.sfx(ui.SFX_SHOOT_EXPLODE if inside else ui.SFX_PONG_WIN)
            send(TOPIC_EVENT, {"id": TEAM, "event": "intruder" if inside else "clear",
                               "n": count, "cm": cm})
        pump_cmd = pump_on or b[0] or remote_ms > 0

        mode = "intruder" if (inside and not silenced) else ("busy" if (fan_on or pump_cmd) else "ok")
        if mode != shown:                       # สั่งงาน: matrix เฉพาะตอนเปลี่ยน
            shown = mode
            status_light(mode)
        g_val.text("--" if t is None else "%.1f C" % t)
        g_sub.text(("" if h is None else "%.0f %%RH  " % h) + ("" if p is None else "%.0f hPa" % p))
        g_set.text("พัดลมเปิด > %.1f C (VR3)" % fan_at)
        g_act.text("พัดลม: " + ("เปิด" if fan_on else "ปิด"))
        g_act.color(COL_OK if fan_on else COL_DIM)
        s_val.text("ดินชื้น %d %%" % soil)
        s_sub.text("SW4 รดเอง" if b[0] else ("แอปสั่งรด" if remote_ms else "อัตโนมัติ"))
        s_act.text("ปั๊ม: " + ("เปิด" if pump_cmd else "ปิด"))
        s_act.color(COL_INFO if pump_cmd else COL_DIM)
        p_val.text("ระยะ " + ("---" if cm is None else str(cm)) + " cm")
        p_sub.text("ผู้บุกรุก %d ครั้ง" % count)
        p_set.text("เขตคอก %d cm (VR4)" % guard_cm)
        p_act.text(("รับทราบแล้ว" if silenced else "ไซเรน!") if inside else "เงียบ")
        p_act.color((COL_WARN if silenced else COL_BAD) if inside else COL_DIM)

    if force or time.ticks_diff(now, last_rep) >= REPORT_MS:   # รายงาน
        body = {"id": TEAM, "n": sent + 1, "temp_c": r1(t), "rh": r1(h), "hpa": r1(p),
                "soil": soil, "fan": int(fan_on), "pump": int(pump_on or b[0] or remote_ms > 0),
                "intruder": int(inside), "intrusions": count, "by": "touch" if force else "timer"}
        force, last_rep = False, now
        if send(TOPIC_TELE, body):
            sent += 1
        print(json.dumps(body))                 # ออฟไลน์ก็ยังเห็นในช่อง Console
        rep_n.text(("ส่งขึ้น broker %d ครั้ง" % sent) if online else "ออฟไลน์: พิมพ์ลง Console")
        rep_line.text(json.dumps(body)[:76])
    time.sleep_ms(TICK_MS)

status_light("off")
if online:
    mqtt.disconnect()
print("ฟาร์มจบรอบ ส่งรายงาน", sent, "ครั้ง ผู้บุกรุก", count, "ครั้ง")

# ----- ตาคุณ แก้แล้วรันใหม่ (แล้วต่อยอดเป็นโปรเจกต์) -----
# 1) เพิ่มกฎข้ามระบบ: "มีผู้บุกรุก ห้ามเปิดปั๊ม" และส่ง event บอกแอปว่าปั๊มถูกล็อกเพราะอะไร
# 2) ในแอปของกลุ่ม (s2/app/) เพิ่มกฎ: รายงานหายเกิน 15 วิ = ขึ้นเตือน "บอร์ดเงียบ" (ฟาร์มดับ/เน็ตหลุด)
