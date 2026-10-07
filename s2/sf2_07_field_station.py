# sf2_07_field_station.py - บอร์ดนี้เล่นเป็น "แปลงผัก": โหนดเซนเซอร์ + PLC คุมปั๊ม
#
# ภารกิจ   : ส่งดิน (VR1) เข้า field/soil ถัง (VR4) เข้า field/tank ทุก 5 วิ · ฟัง plc/cmd แล้วเปิด/ปิดปั๊ม
#            ตามกฎ PLC (เปิดไม่เกิน 30 วิ · ถังต่ำกว่า 10 % ไม่เปิด) · ส่ง plc/state ตอนเปลี่ยนและทุก 5 วิ
# ลองเล่น  : กลุ่มเพื่อนรัน sf2_06_smart_gateway.py · หมุน VR1 ให้ดินแห้ง ดูว่า Gateway สั่งปั๊มไหม
# ของบนบอร์ด: ไฟ RGB_BLUE = รีเลย์ปั๊ม · SW5 (ปุ่มล่าง) = หยุดฉุกเฉิน
# บนจอ     : หลอดดินกับถัง (Bar), ไฟรีเลย์ (Led), วงแหวนนับถอยหลัง (Arc)
# แนวคิด AIoT: PLC ตรวจทุกคำสั่งด้วยกฎของตัวเอง และบอกความจริงกลับเสมอ
# สัญญา MQTT: app/MQTT_CONTRACT_th.md

import buttons
import gpio
import json
import mqtt
import pots
import time
import ui
import wifi

# ---- 1) ตั้งค่า (แก้ได้) ----
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"   # ตั้งเอง: อังกฤษ/ตัวเลขสั้น ๆ ไม่มีเว้นวรรค
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"   # อย่างน้อย 8 ตัว · อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร
TEAM = "teamXX"                        # เลขกลุ่มที่เป็น Gateway (ต้องตรงกันทั้งสองบอร์ด)

BROKER = "broker.hivemq.com"
ROOT = "bento-aiot"
CLIENT_ID = "bento-field-" + TEAM      # + เลขจากนาฬิกาบอร์ดทุกครั้งที่ต่อ: ไม่ชน id เก่า
T_BASE = ROOT + "/" + TEAM + "/"
PLC_MAX_S, PLC_TANK_MIN = 30, 10       # กฎความปลอดภัยของ PLC
PLC_DEFAULT_S = 10
SEND_MS, POLL_MS, RUN_MS = 5000, 100, 1800000
BTN_NAMES = ("SW5", "SW6")

SPEAKER = 40
VOLUME = 25
COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)

def knob_percent(i):
    # ลูกบิด VR1-VR4 (i = 0-3) เป็น 0-100 %
    return pots.read(i) * 100 // 4095


def led_named(name):
    try:
        names = gpio.board_info()["led_names"]
        led = gpio.led(names.index(name) if name in names else 0)
        led.off()
        return led
    except Exception:
        return None


def set_relay(relay, on):
    if relay is None:
        return
    if on:
        relay.on()
    else:
        relay.off()


class Button:
    # เฟิร์มแวร์กรองสัญญาณสั่น 50 ms: อ่านบ่อย ๆ ใน wait_ms ไม่งั้นการกดแบบแตะหายไป
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


# ---- 3) สมอง (ตัดสินใจ) ----
def valid_team(team):
    return len(team) == 6 and team[:4] == "team" and team[4:].isdigit() and team != "team00"


def plc_decide(raw, tank):
    # คำสั่งจาก plc/cmd (สัญญาข้อ 4.2) -> (วินาทีที่จะเปิด / 0 = ปิด / None = ไม่ทำ, เหตุผล)
    try:
        cmd = json.loads(raw.decode())
    except ValueError:
        cmd = None
    if not isinstance(cmd, dict) or "pump" not in cmd:
        return None, "bad_cmd"
    if not cmd["pump"]:
        return 0, "off"
    if tank < PLC_TANK_MIN:
        return None, "blocked_tank"
    sec = cmd.get("sec", PLC_DEFAULT_S)
    if isinstance(sec, bool) or not isinstance(sec, int) or sec <= 0:  # true/false ใน JSON ไม่ใช่จำนวนเต็ม (สัญญาข้อ 4) แม้ Python นับ bool เป็น int
        sec = PLC_DEFAULT_S
    return min(sec, PLC_MAX_S), "on"


def node_message(sensor, value, n):
    # ข้อความโหนดเซนเซอร์ (สัญญาข้อ 3.5)
    return {"node": sensor + "-1", "value": value, "unit": "%", "n": n}


def plc_message(left_ms, why, n):
    # plc/state (สัญญาข้อ 3.6): left_s ปัดขึ้น ไม่ให้ขึ้น 0 ตอนปั๊มยังเดิน
    left = (left_ms + 999) // 1000 if left_ms > 0 else 0
    return {"pump": 1 if left else 0, "left_s": left, "why": why, "n": n}


# ---- 4) เครือข่าย ----
def connect_broker(w):
    # broker สาธารณะบางเครื่องไม่ตอบเป็นพัก ๆ: ลอง 3 ครั้ง ใช้ client_id ใหม่ทุกครั้ง
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


def connect_station(w):
    show_status(w, "กำลังต่อ WiFi จอจะนิ่งสักครู่", COL_WARN)
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "ต่อ WiFi ไม่ได้ ตรวจชื่อวงกับรหัส"
    linked = connect_broker(w)         # ลองได้ 3 ครั้ง (ดู connect_broker)
    if not linked or not mqtt.subscribe(T_BASE + "plc/cmd"):
        return "broker ไม่ตอบ: รอ 1 นาทีแล้วรันใหม่"
    return ""


def send(topic, obj):
    try:
        mqtt.publish(topic, json.dumps(obj))
        return True
    except OSError:
        return False


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    return ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def build_node_card(w):
    card(12, 44, 380, 130, "โหนดเซนเซอร์ (ส่งทุก 5 วิ)")
    ui.Label("ดิน VR1", x=24, y=78, color=COL_TEXT, value=16)
    w["soil_bar"] = ui.Bar(x=110, y=80, w=200, h=20)
    w["soil"] = ui.Label("-- %", x=320, y=76, color=COL_TEXT, value=16)
    ui.Label("ถัง VR4", x=24, y=116, color=COL_TEXT, value=16)
    w["tank_bar"] = ui.Bar(x=110, y=118, w=200, h=20)
    w["tank"] = ui.Label("-- %", x=320, y=114, color=COL_TEXT, value=16)
    w["sent"] = ui.Label("ส่งแล้ว 0 ใบ", x=24, y=148, color=COL_DIM, value=14)


def build_plc_card(w):
    card(12, 182, 380, 152, "PLC (รีเลย์ = ไฟสีฟ้า)")
    w["relay"] = ui.Led(x=28, y=216, w=44, h=44, color=COL_INFO)
    w["pump"] = ui.Label("ปั๊มหยุด", x=84, y=224, color=COL_DIM, value=20)
    w["why"] = ui.Label("why = start", x=28, y=272, color=COL_DIM, value=16)
    w["arc"] = ui.Arc(x=270, y=208, w=110, h=110, min=0, max=PLC_MAX_S, value=0)
    w["arc"].color(COL_OK)
    ui.Label(BTN_NAMES[0] + " = หยุดฉุกเฉิน", x=28, y=304, color=COL_WARN, value=14)


def build_rules_card(w):
    card(402, 44, 378, 290, "กฎของ PLC (ตรวจก่อนทุกคำสั่ง)")
    ui.Label("เปิดสูงสุด %d วิ / ถัง < %d%% ไม่เปิด" % (PLC_MAX_S, PLC_TANK_MIN), x=414, y=80, color=COL_TEXT, value=16)
    w["got"] = ui.Label("รับคำสั่งแล้ว 0", x=414, y=120, color=COL_DIM, value=16)


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: เชื่อมต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("แปลงผัก: โหนด + PLC", x=12, y=6, color=COL_TEXT, value=24)
    w = {"status": ui.Label("กำลังเริ่ม", x=300, y=12, color=COL_DIM, value=16)}
    build_node_card(w)
    build_plc_card(w)
    build_rules_card(w)
    w["note"] = ui.Label("ยังไม่มีคำสั่ง", x=12, y=352, color=COL_DIM)
    w["mq"] = ui.Led(x=606, y=12, w=18, h=18, color=COL_OK, value=0)
    w["mq_t"] = ui.Label("MQTT: ออฟไลน์", x=632, y=10, color=COL_DIM, value=16)
    ui.poll()
    return w


def show_status(w, msg, col):
    w["status"].color(col)
    w["status"].text(msg)
    ui.poll()


def show_knobs(w, soil, tank):
    w["soil_bar"].value(soil)
    w["soil"].text(str(soil) + " %")
    w["tank_bar"].value(tank)
    w["tank_bar"].color(COL_INFO if tank >= PLC_TANK_MIN else COL_BAD)
    w["tank"].text(str(tank) + " %")


def show_relay(w, sec_left, why):
    w["relay"].value(1 if sec_left else 0)
    w["pump"].color(COL_OK if sec_left else COL_DIM)
    w["pump"].text("ปั๊มเดิน อีก " + str(sec_left) + " วิ" if sec_left else "ปั๊มหยุด")
    w["arc"].value(sec_left)
    w["why"].text("why = " + why)


# ---- 6) โปรแกรมหลัก ----
class Plc:

    def __init__(self):
        self.run_ms, self.t_on, self.why, self.n = 0, 0, "start", 0

    def left(self, now):
        return self.run_ms - time.ticks_diff(now, self.t_on) if self.run_ms else 0


def report_plc(plc, now, why=None):
    plc.n += 1
    return send(T_BASE + "plc/state", plc_message(plc.left(now), why or plc.why, plc.n))


def on_command(w, plc, raw, tank, now, got):
    # คำสั่งจาก Gateway: ตรวจ -> ทำ -> เสียง -> ตอบสถานะทันที
    run_s, why = plc_decide(raw, tank)
    plc.why = why
    if run_s is not None:
        plc.run_ms, plc.t_on = run_s * 1000, now
    beep("start" if why == "on" else ("stop" if why == "off" else "bad"))
    w["got"].text("รับคำสั่งแล้ว %d" % got)
    w["note"].text("คำสั่งล่าสุด -> " + why)
    return report_plc(plc, now)


class Stop(Exception):
    # SystemExit ทำให้บอร์ดเริ่มใหม่และอาจค้าง จึงใช้ Stop
    pass


def stop(w, relay, msg, col=COL_BAD):
    show_link(w, False)
    set_relay(relay, False)
    show_status(w, msg, col)
    raise Stop


def main():
    if hasattr(ui, "volume"):
        ui.volume(SPEAKER)
    w = build_screen()
    relay = led_named("RGB_BLUE")
    if not valid_team(TEAM):
        stop(w, relay, "แก้ TEAM เป็นเลขกลุ่มที่เป็น Gateway")
    problem = connect_station(w)
    if problem:
        stop(w, relay, problem)
    show_link(w, True)
    show_status(w, "แปลงของ " + TEAM + " ออนไลน์", COL_OK)
    plc, stop_btn, got = Plc(), Button(1), 0
    counts, shown, was_on = {"soil": 0, "tank": 0}, -1, False
    t0 = time.ticks_ms()
    due = {"soil": 0, "state": SEND_MS // 4, "tank": SEND_MS // 2}   # เหลื่อมกัน กล่องรับของ Gateway มีช่องเดียว
    report_plc(plc, t0)
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        el = time.ticks_diff(now, t0)
        soil, tank = knob_percent(0), knob_percent(3)
        if stop_btn.pressed_now() and plc.run_ms:
            plc.run_ms, plc.why = 0, "stop"
            beep("stop")
            report_plc(plc, now)
        msg = mqtt.get_message()
        if msg is not None:
            got += 1
            if not on_command(w, plc, msg[1], tank, now, got):
                stop(w, relay, "สายหลุด")
        left = plc.left(now)
        if plc.run_ms and (left <= 0 or tank < PLC_TANK_MIN):   # ครบเวลาหรือถังแห้งระหว่างเดิน: ดับเอง
            plc.run_ms, plc.why, left = 0, "timeout" if left <= 0 else "blocked_tank", 0
            beep("stop")
            report_plc(plc, now)
        set_relay(relay, plc.run_ms > 0)
        sec_left = (left + 999) // 1000 if plc.run_ms else 0
        if sec_left != shown or was_on != (plc.run_ms > 0):
            shown, was_on = sec_left, plc.run_ms > 0
            show_relay(w, sec_left, plc.why)
        show_knobs(w, soil, tank)
        for key in ("soil", "tank"):                 # โหนดเซนเซอร์: ทีละตัว ทุก 5 วิ
            if el >= due[key]:
                due[key] += SEND_MS
                counts[key] += 1
                if not send(T_BASE + "field/" + key, node_message(key, soil if key == "soil" else tank, counts[key])):
                    stop(w, relay, "สายหลุด")
        w["sent"].text("ส่งแล้ว " + str(counts["soil"] + counts["tank"]) + " ใบ")
        if el >= due["state"]:                       # ชีพจร PLC ทุก 5 วิ
            due["state"] += SEND_MS
            report_plc(plc, now, "tick")
        if not mqtt.is_connected():
            stop(w, relay, "สายหลุด")
        ui.poll()
        wait_ms(POLL_MS, (stop_btn,))

    stop(w, relay, "จบรอบแปลง", COL_DIM)


try:
    main()
except Stop:
    pass
finally:
    try:
        mqtt.disconnect()
    except Exception:
        pass

# ----- ตาคุณ -----
# 1) ให้ Gateway ของเพื่อนสั่ง {"pump":1,"sec":999} ปั๊มเปิดกี่วิ ใครตัดเหลือ 30
# 2) หมุน VR4 ต่ำกว่า 10 % ตอนปั๊มเดิน PLC ทำอะไร แล้ว Gateway รู้ได้อย่างไร
# 3) เพิ่มโหนดตัวที่ 3: VR3 = แสงแดด ส่งเข้า field/light (ใช้ node_message ตัวเดิม)
