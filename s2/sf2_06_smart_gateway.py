# sf2_06_smart_gateway.py - บอร์ดของเราเป็น Smart IoT Gateway ของฟาร์ม: ฟังแปลง ตัดสินใจ สั่ง PLC
#
# ภาพฟาร์มจริง : กลางแปลงมีโหนดเซนเซอร์ไร้สายส่งความชื้นดินกับน้ำในถังขึ้น MQTT ที่โรงสูบมี PLC WiFi คุมปั๊ม
#               บอร์ดของเราอยู่ตรงกลาง อ่านค่าทุกโหนด ตัดสินว่าต้องรดน้ำไหม แล้วสั่ง PLC พร้อมโชว์ทุกอย่างบนจอ
# ภารกิจ   : ฟัง field/soil field/tank และ plc/state · ดินแห้งกว่า 30 % และน้ำพอ -> สั่ง PLC รดน้ำ 10 วิ
#            (สั่งซ้ำได้ทุก 60 วิ) · ส่งสรุปเข้า telemetry ทุก 5 วิ และแจ้งเหตุการณ์เข้า event
# ลองเล่น  : รัน app/field_sim.py บนโน้ตบุ๊ก (TEAM เดียวกัน) แล้วดูดินแห้งลงจน Gateway สั่งรดน้ำเอง
#            ปิดสวิตช์ "อัตโนมัติ" บนจอ แล้วสั่งเองด้วยปุ่ม SW5 หรือปุ่มรดน้ำใน farm_web.html
#            หรือจับคู่กับอีกกลุ่ม: บอร์ดเขารัน sf2_07_field_station.py เป็นแปลงให้ (ตั้ง TEAM เดียวกัน)
# ของบนบอร์ดที่ใช้ : ไฟ RGB_BLUE = ปั๊มที่ PLC เดินอยู่จริง · ไฟ RGB_RED = PLC เงียบ (หลุด)
#            SW5 (ปุ่มล่าง) = รดน้ำเดี๋ยวนี้ · SW6 (ปุ่มบน) = สลับโหมดอัตโนมัติ
#            จอไฟ RGB 16x8 = ความชื้นดิน (แดง = แห้ง) · ลำโพงดังเฉพาะตอนปั๊มเปลี่ยน / PLC ปฏิเสธ / PLC หลุด
# บนจอ     : วงแหวนความชื้นดิน (Arc), หลอดถังน้ำ (Bar), ไฟ PLC (Led), สวิตช์อัตโนมัติ (Switch),
#            กราฟดินกับปั๊ม (Chart), วงหมุนตอนต่อเน็ต (Spinner)
# แนวคิด AIoT: Sense (โหนด) -> Decide (Gateway) -> Act (PLC) -> Confirm (PLC บอกสถานะจริงกลับมา)
#            คนสั่งไม่ใช่ความจริง ความจริงคือสิ่งที่ PLC รายงานกลับมา จอจึงโชว์ plc/state ไม่ใช่คำสั่งที่ส่งไป
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.1 ขึ้นไป) · ใน BENTO Emulator รันได้แต่ MQTT เป็นแบบจำลอง จะไม่มีแปลงส่งค่ามา
# สัญญา MQTT: app/MQTT_CONTRACT_th.md ข้อ 3.5-3.8 และ 4.1-4.2

import buttons
import gpio
import json
import mqtt
import rgbmatrix
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
COOLDOWN_MS = 60000                    # สั่งแล้วรอกี่ ms ก่อนสั่งอัตโนมัติซ้ำ (ดินเพิ่งรดยังไม่ทันชื้น)
STALE_MS = 15000                       # ไม่ได้ยินเกินนี้ = ค่านั้นเชื่อไม่ได้แล้ว
SEND_MS, POLL_MS, DRAW_MS, RUN_MS = 5000, 100, 500, 1800000

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def led_named(name):
    """หา LED ด้วยชื่อ ไม่ใช่เลข: ดวง LED1/LED2 (เลข 0, 1) อยู่บน SoM มองไม่เห็น ดวงที่เห็นคือ RGB_*"""
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


class Edge:
    """จับจังหวะ "เพิ่งกด" ของปุ่ม: กดหนึ่งครั้ง = ทำงานหนึ่งครั้ง แม้จะกดค้างไว้"""

    def __init__(self):
        self.was_down = False

    def pressed_now(self, is_down):
        fired = is_down and not self.was_down
        self.was_down = is_down
        return fired


def matrix_soil(soil, shown):
    """จอไฟ RGB: ความชื้นดินเป็นตัวเลข แดง = แห้ง เขียนเฉพาะตอนค่าเปลี่ยน"""
    if soil != shown:
        if soil is None:
            rgbmatrix.clear()
        else:
            rgbmatrix.score(soil, rgbmatrix.RED if soil < SOIL_MIN else rgbmatrix.GREEN)
    return soil


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ไม่แตะเน็ต ----
def parse_json(raw):
    """bytes -> dict หรือ None  (ใครส่งอะไรมาก็ได้ ไม่ใช่ JSON object ก็ไม่ใช้)"""
    try:
        body = json.loads(raw.decode())
    except ValueError:
        return None
    return body if isinstance(body, dict) else None


def field_value(body):
    """ค่าจากโหนด (สัญญาข้อ 3.5) ต้องเป็นตัวเลข 0-100 ไม่งั้นทิ้ง"""
    v = body.get("value") if body else None
    return int(v) if isinstance(v, (int, float)) and 0 <= v <= 100 else None


def fresh(value, seen_ms, now):
    """ค่าที่ไม่ได้ยินเกิน STALE_MS ถือว่าเชื่อไม่ได้ คืน None แทน"""
    return value if seen_ms is not None and time.ticks_diff(now, seen_ms) < STALE_MS else None


def should_water(auto, soil, tank, pump, since_cmd_ms):
    """กฎอัตโนมัติ: เปิดโหมด + ดินแห้ง + น้ำพอ + PLC บอกว่าปั๊มหยุด + พ้นช่วงรอ"""
    return (auto and soil is not None and soil < SOIL_MIN and tank is not None
            and tank >= TANK_MIN and pump == 0 and since_cmd_ms >= COOLDOWN_MS)


def app_request(cmd, tank):
    """คำสั่งจากแอป (สัญญาข้อ 4.1) -> (คำสั่งถึง PLC หรือ None, โหมดใหม่หรือ None, ข้อความ)"""
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
        return None, 1 if cmd.get("on", 1) else 0, "ตั้งโหมดออโต้"
    return None, None, "ไม่รู้จักคำสั่ง"


# ---- 4) เครือข่าย ----
def connect_gateway(w):
    """WiFi -> IP -> broker -> subscribe 3 หัวข้อ ขั้นไหนพังคืนข้อความบอกว่าพังตรงไหน"""
    show_note(w, "ต่อ WiFi... จอนิ่งได้", COL_WARN)
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
    """ส่ง JSON คืน False ถ้าสายหลุด (publish ตอนสายหลุดโยน OSError ไม่ใช่คืน False)"""
    try:
        mqtt.publish(BASE + topic, json.dumps(obj))
        return True
    except OSError:
        return False


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    """การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า (ทุกไฟล์ใช้แบบเดียวกัน)"""
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def build_screen():
    """สร้างทุกอย่างบนจอครั้งเดียว แล้วคืน dict ของ widget ที่ต้องอัปเดตภายหลัง"""
    ui.screen()
    time.sleep_ms(200)
    ui.Label("Smart IoT Gateway", x=12, y=6, color=COL_TEXT, value=24)
    w = {"spin": ui.Spinner(x=744, y=4, w=36, h=36)}
    card(12, 44, 240, 200, "ดิน % (field/soil)")
    w["arc"] = ui.Arc(x=56, y=76, w=150, h=150, min=0, max=100, value=0)
    w["soil"] = ui.Label("--", x=110, y=132, color=COL_TEXT, value=28)
    card(262, 44, 518, 200, "ถังน้ำ + PLC")
    w["tank_bar"] = ui.Bar(x=276, y=80, w=380, h=24, min=0, max=100, value=0)
    w["tank"] = ui.Label("--", x=670, y=76, color=COL_TEXT, value=20)
    w["led"] = ui.Led(x=276, y=124, w=40, h=40, color=COL_INFO, value=0)
    w["plc"] = ui.Label("PLC: รอสถานะ", x=330, y=132, color=COL_DIM, value=20)
    w["auto"] = ui.Switch(x=276, y=190, w=64, h=32, value=1)
    ui.Label("ออโต้: ดิน < %d%%" % SOIL_MIN, x=352, y=194, color=COL_TEXT, value=16)
    # กราฟกว้างไม่เกิน 400 และ 400 จุด เส้นจึงเรียบไม่มีวงกลมทุกจุด (จุดละ 5 วิ = ย้อนหลัง 33 นาที)
    w["chart"] = ui.Chart(x=12, y=252, w=400, h=86, color=COL_OK, min=0, max=100)
    w["chart"].prop(ui.PROP_CHART_POINTS, 400)
    w["s_pump"] = w["chart"].add_series(COL_WARN)
    ui.Label("เขียว=ดิน ส้ม=ปั๊ม", x=424, y=256, color=COL_DIM, value=14)
    ui.Label(BTN_NAMES[0] + "=รดน้ำ " + BTN_NAMES[1] + "=ออโต้", x=424, y=286, color=COL_DIM, value=14)
    w["note"] = ui.Label("กำลังเริ่ม", x=12, y=352, color=COL_DIM, value=20)
    ui.poll()
    return w


def show_note(w, text, col):
    """ไม่เรียก ui.poll() ในนี้ เพราะจะกินเหตุการณ์แตะสวิตช์ที่ลูปหลักรออ่าน"""
    w["note"].color(col)
    w["note"].text(text)


def show_farm(w, soil, tank, farm):
    """วาดค่าทั้งหมดใหม่ (ทุกครึ่งวินาที) ไฟ PLC ตามความจริงจาก plc/state เท่านั้น"""
    w["soil"].text("--" if soil is None else str(soil))
    w["arc"].value(soil or 0)
    w["arc"].color(COL_DIM if soil is None else (COL_BAD if soil < SOIL_MIN else COL_OK))
    w["tank"].text("--" if tank is None else "%d %%" % tank)
    w["tank_bar"].value(tank or 0)
    w["led"].value(1 if farm.pump == 1 else 0)
    if farm.pump is None:
        w["plc"].color(COL_BAD)
        w["plc"].text("PLC เงียบ!")
    else:
        w["plc"].color(COL_OK if farm.pump else COL_DIM)
        w["plc"].text("PLC: ปั๊มเดิน อีก %d วิ" % farm.left if farm.pump else "PLC: ปั๊มหยุด")


# ---- 6) โปรแกรมหลัก ----
class Farm:
    """ทุกอย่างที่ Gateway รู้เกี่ยวกับแปลงตอนนี้ (ค่า + เวลาที่ได้ยินล่าสุด)"""

    def __init__(self):
        self.soil = self.tank = self.pump = None
        self.soil_ms = self.tank_ms = self.plc_ms = None
        self.left, self.auto, self.lost = 0, True, False


def on_plc(w, farm, body, now):
    """สถานะใหม่จาก PLC (สัญญาข้อ 3.6): เสียง + event เฉพาะตอนปั๊มเปลี่ยน หรือ PLC ปฏิเสธ"""
    pump, why = body.get("pump"), str(body.get("why", ""))[:16]
    if pump not in (0, 1):
        return
    was = farm.pump
    farm.pump, farm.left, farm.plc_ms, farm.lost = pump, body.get("left_s", 0), now, False
    if why == "blocked_tank":
        ui.sfx(ui.SFX_UI_DENY)
        show_note(w, "PLC ไม่เปิด: ถังต่ำ", COL_BAD)
    if pump != was and not (was is None and pump == 0):   # รู้ครั้งแรกว่า "หยุด" ไม่ใช่การเปลี่ยน
        ui.sfx(ui.SFX_UI_START if pump else ui.SFX_UI_BACK)
        show_note(w, "PLC: ปั๊ม" + ("เดิน" if pump else "หยุด") + " (" + why + ")", COL_OK)
        send("event", {"id": TEAM, "event": "pump", "pump": pump, "why": why})


def on_message(w, farm, msg, now):
    """แยกข้อความตามหัวข้อ แล้วส่งให้ตัวจัดการที่ถูกเรื่อง คืนคำสั่งที่ต้องส่ง PLC (หรือ None)"""
    topic, body = msg[0][len(BASE):], parse_json(msg[1])
    value = field_value(body)
    if topic == "field/soil" and value is not None:
        farm.soil, farm.soil_ms = value, now
    elif topic == "field/tank" and value is not None:
        farm.tank, farm.tank_ms = value, now
    elif topic == "plc/state" and body:
        on_plc(w, farm, body, now)
    elif topic == "cmd":
        plc_cmd, auto, text = app_request(body, fresh(farm.tank, farm.tank_ms, now))
        if auto is not None:
            farm.auto = bool(auto)
            w["auto"].value(auto)
        show_note(w, "แอป: " + text, COL_INFO)
        return plc_cmd
    return None


def check_plc_alive(w, farm, now):
    """PLC เงียบเกิน STALE_MS: ไม่รู้แล้วว่าปั๊มเดินไหม ต้องบอกคนทันที (เสียง + ไฟแดง + event)"""
    if farm.plc_ms is not None and not farm.lost and fresh(1, farm.plc_ms, now) is None:
        farm.lost, farm.pump = True, None
        ui.sfx(ui.SFX_UI_DENY)
        show_note(w, "PLC หลุด!", COL_BAD)
        send("event", {"id": TEAM, "event": "plc_lost"})


def buttons_and_touch(w, farm, water, toggle):
    """ปุ่มล่าง = อยากรดน้ำ (คืน True) · ปุ่มบนหรือแตะสวิตช์บนจอ = สลับโหมดอัตโนมัติ"""
    for ev in ui.poll():
        if ev["handle"] == w["auto"].id() and ev["type"] == "toggled":
            farm.auto = bool(ev["value"])
    if toggle.pressed_now(buttons.pressed(1)):
        farm.auto = not farm.auto
        w["auto"].value(1 if farm.auto else 0)
        ui.sfx(ui.SFX_UI_MOVE)
    return water.pressed_now(buttons.pressed(0))


def stop(w, leds, msg):
    for led in leds:
        set_led(led, False)
    rgbmatrix.clear()
    w["spin"].hide()
    show_note(w, msg, COL_BAD)
    ui.poll()
    raise SystemExit


def main():
    w = build_screen()
    leds = (led_named("RGB_BLUE"), led_named("RGB_RED"))   # ฟ้า = ปั๊มเดิน, แดง = PLC หลุด
    if len(TEAM) != 6 or not TEAM[4:].isdigit() or TEAM == "team00":
        stop(w, leds, "แก้ TEAM ก่อน")
    problem = connect_gateway(w)
    if problem:
        stop(w, leds, problem)
    w["spin"].hide()
    show_note(w, "ออนไลน์ " + TEAM, COL_OK)
    farm, water, toggle = Farm(), Edge(), Edge()
    shown, n = None, 0
    t0 = t_send = t_draw = time.ticks_ms()
    t_cmd = time.ticks_add(t0, -COOLDOWN_MS)            # เริ่มมาสั่งได้เลย ไม่ต้องรอ 60 วิ
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        soil, tank = fresh(farm.soil, farm.soil_ms, now), fresh(farm.tank, farm.tank_ms, now)
        msg = mqtt.get_message()
        want = on_message(w, farm, msg, now) if msg else None
        if buttons_and_touch(w, farm, water, toggle):  # ปุ่มผ่านกฎเดียวกับคำสั่งจากแอป
            want, _, text = app_request({"cmd": "pump"}, tank)
            show_note(w, BTN_NAMES[0] + ": " + text, COL_INFO)
        elif want is None and should_water(farm.auto, soil, tank, farm.pump, time.ticks_diff(now, t_cmd)):
            want = {"pump": 1, "sec": PUMP_SEC}
            show_note(w, "ดินแห้ง รดน้ำ", COL_WARN)
            send("event", {"id": TEAM, "event": "auto_water", "soil": soil})
        if want is not None:
            t_cmd = now
            if not send("plc/cmd", want):
                stop(w, leds, "สายหลุด")
        check_plc_alive(w, farm, now)
        if time.ticks_diff(now, t_draw) >= DRAW_MS:
            t_draw = now
            set_led(leds[0], farm.pump == 1)
            set_led(leds[1], farm.lost)
            shown = matrix_soil(soil, shown)
            show_farm(w, soil, tank, farm)
        if time.ticks_diff(now, t_send) >= SEND_MS:
            t_send, n = now, n + 1
            # สรุปของ Gateway (สัญญาข้อ 3.7)
            if not send("telemetry", {"id": TEAM, "n": n, "soil": soil, "tank": tank, "pump": farm.pump,
                                      "auto": 1 if farm.auto else 0, "by": "gateway"}):
                stop(w, leds, "สายหลุด")
            w["chart"].set_next(0, soil or 0)
            w["chart"].set_next(w["s_pump"], 90 if farm.pump == 1 else 5)
        if not mqtt.is_connected():
            stop(w, leds, "สายหลุด")
        time.sleep_ms(POLL_MS)
    stop(w, leds, "จบรอบ")


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) ปิด field_sim.py กลางคัน แล้วดูว่า Gateway รู้ตัวภายในกี่วินาที (ดู STALE_MS กับ check_plc_alive)
#    ถ้าฟาร์มจริง PLC หลุดตอนปั๊มกำลังเดิน อะไรจะช่วยไม่ให้น้ำท่วมแปลง (ใบ้: PLC ดับปั๊มเองตาม sec)
# 2) เพิ่มโหนดเซนเซอร์ตัวที่ 3 เช่น field/light ใน field_sim.py แล้วให้ Gateway ไม่รดน้ำตอนแดดจัด
#    (แก้ on_message ให้เก็บ light และแก้ should_water เท่านั้น ที่เหลือไม่ต้องแตะ)
# 3) เปลี่ยน COOLDOWN_MS เป็น 5000 แล้วนับว่าใน 1 นาที Gateway สั่ง PLC กี่ครั้ง ทำไมฟาร์มจริงต้องรอ
