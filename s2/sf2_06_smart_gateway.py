# sf2_06_smart_gateway.py - บอร์ดของเราเป็น Smart IoT Gateway ของฟาร์ม: ฟังแปลง ตัดสินใจ สั่ง PLC
#
# ภาพฟาร์มจริง : กลางแปลงมีโหนดเซนเซอร์ไร้สายส่งความชื้นดินกับน้ำในถังขึ้น MQTT ที่โรงสูบมี PLC WiFi คุมปั๊ม
#               บอร์ดของเราอยู่ตรงกลาง อ่านค่าทุกโหนด ตัดสินว่าต้องรดน้ำไหม แล้วสั่ง PLC พร้อมโชว์ทุกอย่างบนจอ
# ภารกิจ   : ฟัง field/soil field/tank และ plc/state · ดินแห้งกว่า 30 % และน้ำพอ -> สั่ง PLC รดน้ำ 10 วิ
#            (สั่งซ้ำได้ทุก 60 วิ) · ส่งสรุปเข้า telemetry ทุก 5 วิ และแจ้งเหตุการณ์เข้า event
# ลองเล่น  : รัน app/field_sim.py บนโน้ตบุ๊ก (TEAM เดียวกัน) แล้วดูดินแห้งลงจน Gateway สั่งรดน้ำเอง
#            แตะสวิตช์ "ออโต้" บนจอให้ปิด แล้วสั่งเองด้วยปุ่ม SW5 หรือปุ่มรดน้ำใน farm_web.html
#            หรือจับคู่กับอีกกลุ่ม: บอร์ดเขารัน sf2_07_field_station.py เป็นแปลงให้ (ตั้ง TEAM เดียวกัน)
# ของบนบอร์ดที่ใช้ : ไฟ RGB_BLUE = ปั๊มที่ PLC เดินอยู่จริง (ไม่ใช่ที่เราสั่ง)
#            SW5 (ปุ่มล่าง) = รดน้ำเดี๋ยวนี้ · SW6 (ปุ่มบน) = สลับโหมดออโต้
#            ลำโพงดังเฉพาะตอนปั๊มเปลี่ยน / PLC ปฏิเสธ / PLC หลุด
# บนจอ     : วงแหวนความชื้นดิน (Arc), หลอดถังน้ำ (Bar), ไฟ PLC (Led), สวิตช์ออโต้ (Switch แตะได้),
#            กราฟ (Chart): เส้นเขียว = ดิน, เส้นส้ม = ปั๊ม (สูง = เดิน)
# แนวคิด AIoT: Sense (โหนด) -> Decide (Gateway) -> Act (PLC) -> Confirm (PLC บอกสถานะจริงกลับมา)
#            คนสั่งไม่ใช่ความจริง ความจริงคือสิ่งที่ PLC รายงานกลับมา จอจึงโชว์ plc/state ไม่ใช่คำสั่งที่ส่งไป
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.1 ขึ้นไป) · ใน BENTO Emulator รันได้แต่ MQTT เป็นแบบจำลอง จะไม่มีแปลงส่งค่ามา
# สัญญา MQTT: app/MQTT_CONTRACT_th.md ข้อ 3.5-3.8 และ 4.1-4.2

import buttons
import gpio
import json
import mqtt
import time
import ui
import wifi

# ---- 1) ตั้งค่า (แก้ได้) ----
WIFI_SSID = "bento-teamXX"
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"
TEAM = "teamXX"                        # team01 ถึง team20 (ต้องตรงกับ field_sim.py หรือบอร์ดแปลงของอีกกลุ่ม)

BROKER = "broker.hivemq.com"           # สำรอง: "test.mosquitto.org" ถ้าผู้สอนประกาศ
CLIENT_ID = "bento-gw-" + TEAM         # ไม่ซ้ำกับบอร์ดแปลง (bento-field-...) หรือแอป
BASE = "bento-aiot/" + TEAM + "/"
BTN_NAMES = ("SW5", "SW6")             # ปุ่มล่าง = pressed(0), ปุ่มบน = pressed(1) ตามตัวอักษรบนแผง
SOIL_MIN = 30                          # ดินแห้งกว่านี้ (%) = ถึงเวลารดน้ำ
PUMP_SEC, PUMP_MAX_S = 10, 30          # รดครั้งละกี่วินาที / เพดานที่ยอมส่งต่อให้ PLC
TANK_MIN = 10                          # น้ำในถังต่ำกว่านี้ Gateway ไม่สั่ง (PLC ก็กันซ้ำอีกชั้น)
COOLDOWN_MS = 60000                    # สั่งแล้วรอกี่ ms ก่อนสั่งออโต้ซ้ำ (ดินเพิ่งรดยังไม่ทันชื้น)
STALE_MS = 15000                       # ไม่ได้ยินเกินนี้ = ค่านั้นเชื่อไม่ได้แล้ว
SEND_MS, POLL_MS, DRAW_MS, RUN_MS = 5000, 100, 500, 1800000

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def led_named(name):
    # หา LED ด้วยชื่อ ไม่ใช่เลข: ดวง LED1/LED2 (เลข 0, 1) อยู่บน SoM มองไม่เห็น ดวงที่เห็นคือ RGB_*
    try:
        names = gpio.board_info()["led_names"]
        led = gpio.led(names.index(name) if name in names else 0)
        led.off()
        return led
    except Exception:
        return None


def set_led(led, on):
    if led:
        led.on() if on else led.off()


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
        # True ครั้งเดียวต่อการกดหนึ่งครั้ง (กดค้างไว้ก็ไม่นับซ้ำ)
        fired, self.clicked = self.clicked, False
        return fired


def wait_ms(ms, btns):
    # รอ ms มิลลิวินาที แต่ระหว่างรอก็อ่านปุ่มทุก 20 ms เพื่อไม่พลาดการกดสั้น ๆ
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < ms:
        for b in btns:
            b.sample()
        time.sleep_ms(20)


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ไม่แตะเน็ต ----
def parse_json(raw):
    # bytes -> dict หรือ None  (ใครส่งอะไรมาก็ได้ ไม่ใช่ JSON object ก็ไม่ใช้)
    try:
        body = json.loads(raw.decode())
    except ValueError:
        return None
    return body if isinstance(body, dict) else None


def field_now(farm, name, now):
    # ค่าล่าสุดของโหนด name (soil, tank, ...) ถ้าเงียบเกิน STALE_MS ถือว่าเชื่อไม่ได้ คืน None
    got = farm.field.get(name)
    return got[0] if got and time.ticks_diff(now, got[1]) < STALE_MS else None


def should_water(farm, soil, tank, since_cmd_ms):
    # กฎออโต้: เปิดโหมด + ดินแห้ง + น้ำพอ + PLC บอกว่าปั๊มหยุด + พ้นช่วงรอ
    return (farm.auto and soil is not None and soil < SOIL_MIN and tank is not None
            and tank >= TANK_MIN and farm.pump == 0 and since_cmd_ms >= COOLDOWN_MS)


def app_request(cmd, tank):
    # คำสั่งจากแอป (สัญญาข้อ 4.1) -> (คำสั่งถึง PLC หรือ None, โหมดออโต้ใหม่หรือ None, ข้อความ)
    act = cmd.get("cmd") if cmd else None
    if act == "pump" and not cmd.get("on", 1):
        return {"pump": 0}, None, "หยุดปั๊ม"
    if act == "pump":
        if tank is None or tank < TANK_MIN:
            return None, None, "ถังน้ำไม่พอ"
        sec = cmd.get("sec", PUMP_SEC)
        sec = min(sec, PUMP_MAX_S) if isinstance(sec, int) and sec > 0 else PUMP_SEC
        return {"pump": 1, "sec": sec}, None, "รดน้ำ %d วิ" % sec
    if act == "auto":
        return None, 1 if cmd.get("on", 1) else 0, "ตั้งออโต้"
    return None, None, "ไม่รู้จักคำสั่ง"


# ---- 4) เครือข่าย ----
def connect_gateway(w):
    # WiFi -> IP -> broker -> subscribe 3 หัวข้อ ขั้นไหนพังคืนข้อความบอกว่าพังตรงไหน
    show_note(w, "ต่อ WiFi...", COL_WARN)
    ui.poll()                          # ป้ายต้องขึ้นจอก่อนบรรทัดที่บล็อก
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "ต่อ WiFi ไม่ได้"
    try:
        ok = mqtt.connect(BROKER, port=1883, client_id=CLIENT_ID, keepalive=60)
    except OSError:
        ok = False
    for topic in ("field/+", "plc/state", "cmd"):
        ok = ok and mqtt.subscribe(BASE + topic)
    return "" if ok else "broker ไม่ตอบ"


def send(topic, obj):
    # ส่ง JSON คืน False ถ้าสายหลุด (publish ตอนสายหลุดโยน OSError ไม่ใช่คืน False)
    try:
        mqtt.publish(BASE + topic, json.dumps(obj))
        return True
    except OSError:
        return False


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า (ทุกไฟล์ใช้แบบเดียวกัน)
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def line_chart(x, y, w, h, lo, hi, color, parent=None):
    # กราฟเส้นเรียบ ไม่มีจุดกลม: LVGL ไม่วาดจุดเมื่อจำนวนจุด >= ความกว้างกราฟ
    # เราจึงให้กว้างไม่เกิน 400 และตั้ง 400 จุด (เฟิร์มแวร์รับได้ 10-400)
    ch = ui.Chart(x=x, y=y, w=min(w, 400), h=h, color=color, min=lo, max=hi, parent=parent)
    ch.prop(ui.PROP_CHART_POINTS, 400)
    return ch


def build_screen():
    # สร้างทุกอย่างบนจอครั้งเดียว แล้วคืน dict ของ widget ที่ต้องอัปเดตภายหลัง
    ui.screen()
    time.sleep_ms(200)
    ui.Label("Smart IoT Gateway", x=12, y=6, color=COL_TEXT, value=24)
    w = {}
    card(12, 44, 240, 200, "ดิน % (field/soil)")
    w["arc"] = ui.Arc(x=24, y=80, w=120, h=120)                # ค่าตั้งต้นของ Arc คือ 0-100 อยู่แล้ว
    w["soil"] = ui.Label("--", x=160, y=120, color=COL_TEXT, value=28)
    card(262, 44, 518, 200, "ถังน้ำ + PLC")
    w["tank_bar"] = ui.Bar(x=276, y=80, w=380, h=24)
    w["tank"] = ui.Label("--", x=670, y=76, color=COL_TEXT)
    w["led"] = ui.Led(x=276, y=124, w=40, h=40, color=COL_INFO)  # Led สร้างมาแบบหรี่ = ปั๊มหยุด
    w["plc"] = ui.Label("PLC: ?", x=330, y=132, color=COL_DIM)
    w["auto"] = ui.Switch(x=276, y=190, w=64, h=32, value=1)
    ui.Label("ออโต้: ดิน < %d%%" % SOIL_MIN, x=352, y=194, color=COL_TEXT, value=16)
    w["chart"] = line_chart(12, 252, 400, 86, 0, 100, COL_OK)   # จุดละ 5 วิ 400 จุด = ย้อนหลัง 33 นาที
    w["s_pump"] = w["chart"].add_series(COL_WARN)
    ui.Label(BTN_NAMES[0] + "=รดน้ำ " + BTN_NAMES[1] + "=ออโต้", x=424, y=290, color=COL_DIM, value=14)
    w["note"] = ui.Label("กำลังเริ่ม", x=12, y=352, color=COL_DIM)
    ui.poll()
    return w


def show_note(w, text, col):
    # ไม่เรียก ui.poll() ในนี้ เพราะจะกินเหตุการณ์แตะสวิตช์ที่ลูปหลักรออ่าน
    w["note"].color(col)
    w["note"].text(text)


def show_farm(w, soil, tank, farm):
    # วาดค่าทั้งหมดใหม่ (ทุกครึ่งวินาที) ไฟ PLC ตามความจริงจาก plc/state เท่านั้น
    w["soil"].text("--" if soil is None else str(soil))
    w["arc"].value(soil or 0)
    w["arc"].color(COL_DIM if soil is None else (COL_BAD if soil < SOIL_MIN else COL_OK))
    w["tank"].text("--" if tank is None else "%d %%" % tank)
    w["tank_bar"].value(tank or 0)
    w["led"].value(1 if farm.pump == 1 else 0)
    w["plc"].color(COL_BAD if farm.lost else (COL_OK if farm.pump else COL_DIM))
    w["plc"].text("PLC หลุด!" if farm.lost else ("PLC: เดิน อีก %d วิ" % farm.left if farm.pump else "PLC: หยุด"))


# ---- 6) โปรแกรมหลัก ----
class Farm:
    # ทุกอย่างที่ Gateway รู้ตอนนี้: field = {ชื่อโหนด: (ค่า, เวลาที่ได้ยิน)} และสถานะ PLC ล่าสุด

    def __init__(self):
        self.field, self.pump, self.plc_ms = {}, None, None
        self.left, self.auto, self.lost = 0, True, False


def on_plc(w, farm, body, now):
    # สถานะใหม่จาก PLC (สัญญาข้อ 3.6): เสียง + event เฉพาะตอนปั๊มเปลี่ยน หรือ PLC ปฏิเสธ
    pump, why = body.get("pump"), str(body.get("why", ""))[:16]
    if pump not in (0, 1):
        return
    was = farm.pump
    farm.pump, farm.left, farm.plc_ms, farm.lost = pump, body.get("left_s", 0), now, False
    if why == "blocked_tank":
        ui.sfx(ui.SFX_UI_DENY)
        show_note(w, "PLC: ถังต่ำ", COL_BAD)
    if pump != was and not (was is None and pump == 0):   # รู้ครั้งแรกว่า "หยุด" ไม่ใช่การเปลี่ยน
        ui.sfx(ui.SFX_UI_START if pump else ui.SFX_UI_BACK)
        show_note(w, "ปั๊มเดิน" if pump else "ปั๊มหยุด", COL_OK)
        send("event", {"id": TEAM, "event": "pump", "pump": pump, "why": why})


def on_message(w, farm, msg, now):
    # แยกข้อความตามหัวข้อ แล้วส่งให้ตัวจัดการที่ถูกเรื่อง คืนคำสั่งที่ต้องส่ง PLC (หรือ None)
    topic, body = msg[0][len(BASE):], parse_json(msg[1])
    if topic[:6] == "field/" and body:                  # โหนดไหนก็ได้ (สัญญาข้อ 3.5) รับค่า 0-100
        v = body.get("value")
        if isinstance(v, (int, float)) and 0 <= v <= 100:
            farm.field[topic[6:]] = (int(v), now)
    elif topic == "plc/state" and body:
        on_plc(w, farm, body, now)
    elif topic == "cmd":
        plc_cmd, auto, text = app_request(body, field_now(farm, "tank", now))
        if auto is not None:
            farm.auto = bool(auto)
            w["auto"].value(auto)
        show_note(w, text, COL_INFO)
        return plc_cmd
    return None


def check_plc_alive(w, farm, now):
    # PLC เงียบเกิน STALE_MS: ไม่รู้แล้วว่าปั๊มเดินไหม ต้องบอกคนทันที (เสียง + ข้อความแดง + event)
    if farm.plc_ms is not None and not farm.lost and time.ticks_diff(now, farm.plc_ms) >= STALE_MS:
        farm.lost, farm.pump = True, None
        ui.sfx(ui.SFX_UI_DENY)
        show_note(w, "PLC หลุด!", COL_BAD)
        send("event", {"id": TEAM, "event": "plc_lost"})


def buttons_and_touch(w, farm, water, toggle):
    # ปุ่มล่าง = อยากรดน้ำ (คืน True) · ปุ่มบนหรือแตะสวิตช์บนจอ = สลับโหมดออโต้
    for ev in ui.poll():
        if ev["handle"] == w["auto"].id() and ev["type"] == "toggled":
            farm.auto = bool(ev["value"])
    if toggle.pressed_now():
        farm.auto = not farm.auto
        w["auto"].value(1 if farm.auto else 0)
        ui.sfx(ui.SFX_UI_MOVE)
    return water.pressed_now()


def stop(w, led, msg):
    set_led(led, False)
    show_note(w, msg, COL_BAD)
    ui.poll()
    raise SystemExit


def main():
    w = build_screen()
    led = led_named("RGB_BLUE")        # ไฟฟ้าบนบอร์ด = ปั๊มที่ PLC บอกว่าเดินอยู่
    if len(TEAM) != 6 or not TEAM[4:].isdigit() or TEAM == "team00":
        stop(w, led, "แก้ TEAM ก่อน")
    problem = connect_gateway(w)
    if problem:
        stop(w, led, problem)
    show_note(w, "ออนไลน์ " + TEAM, COL_OK)
    farm, water, toggle = Farm(), Button(0), Button(1)
    n = 0
    t0 = t_send = t_draw = time.ticks_ms()
    t_cmd = time.ticks_add(t0, -COOLDOWN_MS)            # เริ่มมาสั่งได้เลย ไม่ต้องรอ 60 วิ
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        soil, tank = field_now(farm, "soil", now), field_now(farm, "tank", now)
        msg = mqtt.get_message()
        want = on_message(w, farm, msg, now) if msg else None
        if buttons_and_touch(w, farm, water, toggle):  # ปุ่มผ่านกฎเดียวกับคำสั่งจากแอป
            want, _, text = app_request({"cmd": "pump"}, tank)
            show_note(w, BTN_NAMES[0] + ": " + text, COL_INFO)
        elif want is None and should_water(farm, soil, tank, time.ticks_diff(now, t_cmd)):
            want = {"pump": 1, "sec": PUMP_SEC}
            show_note(w, "ดินแห้ง รดน้ำ", COL_WARN)
            send("event", {"id": TEAM, "event": "auto_water", "soil": soil})
        if want is not None:
            t_cmd = now
            if not send("plc/cmd", want):
                stop(w, led, "สายหลุด")
        check_plc_alive(w, farm, now)
        if time.ticks_diff(now, t_draw) >= DRAW_MS:
            t_draw = now
            set_led(led, farm.pump == 1)
            show_farm(w, soil, tank, farm)
        if time.ticks_diff(now, t_send) >= SEND_MS:
            t_send, n = now, n + 1
            # สรุปของ Gateway (สัญญาข้อ 3.7)
            if not send("telemetry", {"id": TEAM, "n": n, "soil": soil, "tank": tank, "pump": farm.pump,
                                      "auto": 1 if farm.auto else 0, "by": "gateway"}):
                stop(w, led, "สายหลุด")
            w["chart"].set_next(0, soil or 0)
            w["chart"].set_next(w["s_pump"], 90 if farm.pump == 1 else 5)
        if not mqtt.is_connected():
            stop(w, led, "สายหลุด")
        wait_ms(POLL_MS, (water, toggle))
    stop(w, led, "จบรอบ")


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) ปิด field_sim.py กลางคัน แล้วดูว่า Gateway รู้ตัวภายในกี่วินาที (ดู STALE_MS กับ check_plc_alive)
#    ถ้าฟาร์มจริง PLC หลุดตอนปั๊มกำลังเดิน อะไรจะช่วยไม่ให้น้ำท่วมแปลง (ใบ้: PLC ดับปั๊มเองตาม sec)
# 2) เพิ่มโหนดเซนเซอร์ตัวที่ 3 เช่น field/light ใน field_sim.py แล้วให้ Gateway ไม่รดน้ำตอนแดดจัด
#    (on_message เก็บทุกโหนดให้อยู่แล้ว แค่อ่าน field_now(farm, "light", now) แล้วแก้ should_water)
# 3) เปลี่ยน COOLDOWN_MS เป็น 5000 แล้วนับว่าใน 1 นาที Gateway สั่ง PLC กี่ครั้ง ทำไมฟาร์มจริงต้องรอ
