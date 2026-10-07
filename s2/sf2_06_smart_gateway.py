# sf2_06_smart_gateway.py - Smart IoT Gateway: ฟังแปลง ตัดสินใจ สั่ง PLC
#
# ภารกิจ   : ฟัง field/+ (โหนด) plc/state (PLC) cmd (แอป) · ดิน < 30 % และน้ำพอ -> ส่ง plc/cmd รดน้ำ 10 วิ
#            แอปเรียกคนที่ฟาร์ม (beep) และส่งข้อความขึ้นจอไฟ RGB (say) ได้เหมือน sf2_03
# ลองเล่น  : รัน app/field_sim.py บนโน้ตบุ๊ก หรืออีกกลุ่มรัน sf2_07_field_station.py (TEAM เดียวกัน)
# บนจอ     : ไฟ PLC กับ RGB_BLUE ตาม plc/state ที่ PLC รายงาน ไม่ใช่ที่เราสั่ง

import buttons
import gpio
import json
import mqtt
import rgbmatrix
import time
import ui
import wifi

# ---- 1) ตั้งค่า (แก้ได้) ----
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"   # ตั้งเอง: อังกฤษ/ตัวเลขสั้น ๆ ไม่มีเว้นวรรค
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"   # อย่างน้อย 8 ตัว · อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร
TEAM = "teamXX"                        # เลขกลุ่มที่ผู้สอนแจก

BROKER = "broker.hivemq.com"
CLIENT_ID = "bento-gw-" + TEAM         # + เลขจากนาฬิกาบอร์ดทุกครั้งที่ต่อ: ไม่ชน id เก่า
BASE = "bento-aiot/" + TEAM + "/"
BTN_NAMES = ("SW5", "SW6")
SOIL_MIN = 30
PUMP_SEC, PUMP_MAX_S = 10, 30          # รดกี่วิ / เพดานที่ส่งให้ PLC
TANK_MIN = 10                          # ถังต่ำกว่านี้ไม่สั่ง (PLC กันซ้ำอีกชั้น)
COOLDOWN_MS = 60000
STALE_MS = 15000
SEND_MS, POLL_MS, DRAW_MS, RUN_MS = 5000, 100, 500, 1800000

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

def led_named(name):
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
def parse_json(raw):
    try:
        body = json.loads(raw.decode())
    except ValueError:
        return None
    return body if isinstance(body, dict) else None


def field_now(farm, name, now):
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


def ascii_only(text):
    return "".join(c for c in str(text) if " " <= c <= "~")[:20]


def farm_request(cmd):
    # คำสั่งถึงคนที่ฟาร์ม ไม่ผ่าน PLC (สัญญาข้อ 4 แบบเดียวกับ sf2_03)
    # -> (ทำอะไร, ข้อความวิ่ง, ข้อความขึ้นจอ) หรือ None ถ้าไม่ใช่คำสั่งกลุ่มนี้
    act = cmd.get("cmd") if cmd else None
    if act == "beep":
        return "beep", "", "เจ้าของฟาร์มเรียก!"
    if act == "say":
        text = ascii_only(cmd.get("text", ""))
        return "say", text, "ข้อความ: " + (text or "ไม่มีอักษรอังกฤษ")
    if act == "ack":                   # ack เป็นของ sf2_04
        return "", "", "ไม่มีแจ้งเตือน (sf2_04)"
    return None


# ---- 4) เครือข่าย ----
def connect_broker(w):
    # broker สาธารณะบางเครื่องไม่ตอบเป็นพัก ๆ: ลอง 3 ครั้ง ใช้ client_id ใหม่ทุกครั้ง
    for n in (1, 2, 3):
        if n > 1:
            show_note(w, "ลองต่อ broker ใหม่ %d/3" % n, COL_WARN)
            ui.poll()
        try:
            if mqtt.connect(BROKER, port=1883, keepalive=60,
                            client_id=CLIENT_ID + "-%04x" % (time.ticks_ms() & 0xFFFF)):
                return True
        except OSError:
            pass
    return False


def connect_gateway(w):
    # WiFi -> IP -> broker -> subscribe 3 หัวข้อ ขั้นไหนพังคืนข้อความบอกว่าพังตรงไหน
    show_note(w, "ต่อ WiFi...", COL_WARN)
    ui.poll()                          # ป้ายต้องขึ้นจอก่อนบรรทัดที่บล็อก
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "ต่อ WiFi ไม่ได้"
    ok = connect_broker(w)
    for topic in ("field/+", "plc/state", "cmd"):
        ok = ok and mqtt.subscribe(BASE + topic)
    return "" if ok else "broker ไม่ตอบ: รอ 1 นาทีแล้วรันใหม่"


def send(topic, obj):
    # สายหลุด publish โยน OSError จึงคืน False แทน
    try:
        mqtt.publish(BASE + topic, json.dumps(obj))
        return True
    except OSError:
        return False


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def line_chart(x, y, w, h, lo, hi, color, parent=None):
    ch = ui.Chart(x=x, y=y, w=min(w, 400), h=h, color=color, min=lo, max=hi, parent=parent)
    ch.prop(ui.PROP_CHART_POINTS, 400)
    return ch


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: เชื่อมต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("Smart IoT Gateway", x=12, y=6, color=COL_TEXT, value=24)
    w = {}
    card(12, 44, 240, 200, "ดิน % (field/soil)")
    w["arc"] = ui.Arc(x=24, y=80, w=120, h=120)
    w["soil"] = ui.Label("--", x=160, y=120, color=COL_TEXT, value=28)
    card(262, 44, 518, 200, "ถังน้ำ + PLC")
    w["tank_bar"] = ui.Bar(x=276, y=80, w=380, h=24)
    w["tank"] = ui.Label("--", x=670, y=76, color=COL_TEXT)
    w["led"] = ui.Led(x=276, y=124, w=40, h=40, color=COL_INFO)
    w["plc"] = ui.Label("PLC: ?", x=330, y=132, color=COL_DIM)
    w["auto"] = ui.Switch(x=276, y=190, w=64, h=32, value=1)
    ui.Label("ออโต้: ดิน < %d%%" % SOIL_MIN, x=352, y=194, color=COL_TEXT, value=16)
    w["chart"] = line_chart(12, 252, 400, 86, 0, 100, COL_OK)
    w["s_pump"] = w["chart"].add_series(COL_WARN)
    ui.Label(BTN_NAMES[0] + "=รดน้ำ " + BTN_NAMES[1] + "=ออโต้", x=424, y=290, color=COL_DIM, value=14)
    w["note"] = ui.Label("กำลังเริ่ม", x=12, y=352, color=COL_DIM)
    w["mq"] = ui.Led(x=606, y=12, w=18, h=18, color=COL_OK, value=0)
    w["mq_t"] = ui.Label("MQTT: ออฟไลน์", x=632, y=10, color=COL_DIM, value=16)
    ui.poll()
    return w


def show_note(w, text, col):
    # ไม่เรียก ui.poll() ในนี้ เพราะจะกินเหตุการณ์แตะสวิตช์ที่ลูปหลักรออ่าน
    w["note"].color(col)
    w["note"].text(text)


def show_farm(w, soil, tank, farm):
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
    def __init__(self):
        self.field, self.pump, self.plc_ms = {}, None, None
        self.left, self.auto, self.lost = 0, True, False


def on_plc(w, farm, body, now):
    pump, why = body.get("pump"), str(body.get("why", ""))[:16]
    if pump not in (0, 1):
        return
    was = farm.pump
    farm.pump, farm.left, farm.plc_ms, farm.lost = pump, body.get("left_s", 0), now, False
    if why == "blocked_tank":
        beep("bad")
        show_note(w, "PLC: ถังต่ำ", COL_BAD)
    if pump != was and not (was is None and pump == 0):
        beep("start" if pump else "stop")
        show_note(w, "ปั๊มเดิน" if pump else "ปั๊มหยุด", COL_OK)
        send("event", {"id": TEAM, "event": "pump", "pump": pump, "why": why})


def on_message(w, farm, msg, now):
    topic, body = msg[0][len(BASE):], parse_json(msg[1])
    if topic[:6] == "field/" and body:
        v = body.get("value")
        if isinstance(v, (int, float)) and 0 <= v <= 100:
            farm.field[topic[6:]] = (int(v), now)
    elif topic == "plc/state" and body:
        on_plc(w, farm, body, now)
    elif topic == "cmd":
        local = farm_request(body)
        if local:
            do, msg, text = local
            if do == "beep":
                beep("hit")
            elif do == "say" and msg:
                rgbmatrix.scroll(msg, rgbmatrix.PURPLE, 80)   # วิ่งเองบนบอร์ด ไม่ต้องวนในลูป
                beep("tap")
            show_note(w, text, COL_INFO)
            return None
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
        beep("bad")
        show_note(w, "PLC หลุด!", COL_BAD)
        send("event", {"id": TEAM, "event": "plc_lost"})


def buttons_and_touch(w, farm, water, toggle):
    for ev in ui.poll():
        if ev["handle"] == w["auto"].id() and ev["type"] == "toggled":
            farm.auto = bool(ev["value"])
    if toggle.pressed_now():
        farm.auto = not farm.auto
        w["auto"].value(1 if farm.auto else 0)
        beep("tap")
    return water.pressed_now()


class Stop(Exception):
    # SystemExit ทำให้บอร์ดเริ่มใหม่และอาจค้าง จึงใช้ Stop
    pass


def stop(w, led, msg):
    show_link(w, False)
    set_led(led, False)
    show_note(w, msg, COL_BAD)
    ui.poll()
    raise Stop


def main():
    if hasattr(ui, "volume"):
        ui.volume(SPEAKER)
    w = build_screen()
    led = led_named("RGB_BLUE")
    rgbmatrix.clear()
    if len(TEAM) != 6 or not TEAM[4:].isdigit() or TEAM == "team00":
        stop(w, led, "แก้ TEAM ก่อน")
    problem = connect_gateway(w)
    if problem:
        stop(w, led, problem)
    show_link(w, True)
    show_note(w, "ออนไลน์ " + TEAM, COL_OK)
    farm, water, toggle = Farm(), Button(1), Button(0)
    n = 0
    t0 = t_send = t_draw = time.ticks_ms()
    t_cmd = time.ticks_add(t0, -COOLDOWN_MS)
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
            if not send("telemetry", {"id": TEAM, "n": n, "soil": soil, "tank": tank, "pump": farm.pump,
                                      "auto": 1 if farm.auto else 0, "by": "gateway"}):
                stop(w, led, "สายหลุด")
            w["chart"].set_next(0, soil or 0)
            w["chart"].set_next(w["s_pump"], 90 if farm.pump == 1 else 5)
        if not mqtt.is_connected():
            stop(w, led, "สายหลุด")
        wait_ms(POLL_MS, (water, toggle))
    stop(w, led, "จบรอบ")


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
# 1) ปิด field_sim.py แล้ว Gateway รู้ตัวในกี่วิ ถ้า PLC หลุดตอนปั๊มเดิน อะไรกันน้ำท่วม
# 2) เพิ่มโหนด field/light ใน field_sim.py แล้วแก้ should_water ไม่ให้รดตอนแดดจัด
# 3) ตั้ง COOLDOWN_MS = 5000 แล้ว 1 นาทีสั่ง PLC กี่ครั้ง ทำไมต้องรอ
