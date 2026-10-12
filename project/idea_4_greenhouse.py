# idea_4_greenhouse.py - โรงเรือนอัจฉริยะ: กราฟดินแห้ง สั่งเปิดวาล์วรดน้ำจากมือถือ
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# ต่อยอดจาก s2/sf2_03_remote_pump.py (เครือข่ายและกฎความปลอดภัยเดิม)
# ปั๊ม = วาล์วรดน้ำ, VR1 = ดิน (จำลอง), VR4 = ถังพัก · ภาพหลัก = กราฟดิน 2 นาที
# MQTT: ของเดิม sf2_03 ครบ (สัญญา 3.2, 4) + 2 คีย์ใหม่ที่บอร์ดคิดเอง
#   dry_min = นาทีที่ดินต่ำกว่า SOIL_DRY รวม  water_s = วินาทีที่วาล์วเปิดรวม
# ทีมต้องแก้ 3 อย่าง: 1) SOIL_DRY PUMP_MAX_S จากค่าที่ลองเอง 2) หัวจอและป้าย 3) เสียงใน TUNES
# เกณฑ์ = ค่าสาธิตในห้องเรียน ไม่ใช่คำแนะนำทางเกษตร
#
# ลองเล่น  : สั่งจาก project/web/idea_4.html?team=<เลขกลุ่ม>
# ต้องแก้ก่อนรัน: WIFI_SSID, WIFI_PASS, TEAM (เลขกลุ่ม)
# บนจอ     : กราฟดิน ไฟวาล์ว วงแหวนนับถอยหลัง หลอดถังพัก ตัวนับ
# กับดัก    : get_message() ไม่บล็อก ต้องถามทุก 100 ms

import buttons
import gpio
import json
import mqtt
import pots
import rgbmatrix
import time
import ui
import wifi

# ---- 1) ตั้งค่า ----
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"   # ตั้งเอง: อังกฤษ/ตัวเลขสั้น ๆ ไม่มีเว้นวรรค
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"   # อย่างน้อย 8 ตัว · อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร
TEAM = "teamXX"

BROKER = "broker.hivemq.com"
CLIENT_ID = "bento-farm-" + TEAM       # + เลขจากนาฬิกาบอร์ดทุกครั้งที่ต่อ: ไม่ชน id เก่า
TOPIC_CMD = "bento-aiot/" + TEAM + "/cmd"
TOPIC = "bento-aiot/" + TEAM + "/telemetry"
BTN_NAMES = ("SW5", "SW6")
PUMP_DEFAULT_S, PUMP_MAX_S = 10, 30    # ใครสั่ง 9999 วินาที ก็ได้แค่ 30
TANK_MIN = 10  # ถังพักต่ำกว่านี้ห้ามเปิดวาล์ว (ปั๊มดูดลมพัง)
SOIL_DRY = 30  # ค่าสาธิต: ดิน (VR1) ต่ำกว่านี้นับว่าแห้ง
SEND_MS, POLL_MS, LISTEN_MS = 5000, 100, 1800000
CHART_MS, CHART_PTS = 2000, 60         # จุดละ 2 วิ x 60 จุด = ย้อนหลัง 2 นาที

SPEAKER = 40
VOLUME = 25
# สีโรงเรือน: เขียวใบไม้ น้ำตาลดิน ฟ้าน้ำ
COL_TEXT, COL_DIM = 0xE6F0E4, 0x8FA394
COL_CARD = 0x14221A
COL_LEAF, COL_SOIL, COL_RAIN = 0x5CC27E, 0xC28A4E, 0x4FB3E8
COL_DRY, COL_BAD = 0xF0A030, 0xE5484D


# ---- 2) ฮาร์ดแวร์ ----
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)

def knob_percent(i):
    return pots.read(i) * 100 // 4095


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


def matrix_countdown(sec_left, shown):
    if shown <= 0 or sec_left == 0:
        rgbmatrix.scroll("")
        rgbmatrix.clear()
    if sec_left:
        rgbmatrix.score(sec_left, rgbmatrix.GREEN)
    return sec_left


# ---- 3) สมอง ----
def pump_seconds(sec):
    # ตรวจเวลาที่สั่ง: ไม่ใช่จำนวนเต็มบวก = ค่าตั้งต้น  เกินเพดาน = เพดาน
    if isinstance(sec, bool) or not isinstance(sec, int) or sec <= 0:  # bool ไม่ใช่จำนวนเต็ม (สัญญาข้อ 4)
        sec = PUMP_DEFAULT_S
    return min(sec, PUMP_MAX_S)


def ascii_only(text):
    return "".join(c for c in str(text) if " " <= c <= "~")[:20]


def handle_command(raw, tank):
    # ตัดสินคำสั่งหนึ่งใบ -> (ทำอะไร, วินาทีหรือข้อความ, ข้อความขึ้นจอ, สี)  ไม่แตะของจริงในนี้
    try:
        cmd = json.loads(raw.decode())
    except ValueError:
        cmd = None
    if not isinstance(cmd, dict):      # 5, null, [] ก็เป็น JSON ได้ แต่ไม่ใช่คำสั่ง
        return "deny", 0, "อ่านคำสั่งไม่ได้", COL_BAD
    act, on = cmd.get("cmd", ""), cmd.get("on", 1)
    if act == "led" and cmd.get("n", 0) != 0:
        return "deny", 0, "ไม่มีวาล์วที่ " + str(cmd.get("n"))[:6], COL_DRY
    if act in ("led", "pump") and not on:
        return "off", 0, "สั่งปิดวาล์วแล้ว", COL_LEAF
    if act in ("led", "pump") and tank < TANK_MIN:
        return "deny", 0, "ถังพักเหลือ %d%% ไม่เปิดวาล์ว" % tank, COL_BAD
    if act in ("led", "pump"):
        sec = pump_seconds(cmd.get("sec", PUMP_DEFAULT_S))
        return "on", sec, "เปิดวาล์วรดแปลง %d วิ" % sec, COL_RAIN
    if act == "beep":
        return "beep", 0, "เจ้าของโรงเรือนเรียก!", COL_RAIN
    if act == "say":
        text = ascii_only(cmd.get("text", ""))
        return "say", text, "ข้อความ: " + (text or "ไม่มีอักษรอังกฤษ"), COL_RAIN
    if act == "ack":  # ack เป็นของ sf2_04
        return "", 0, "ไม่มีแจ้งเตือน (sf2_04)", COL_DIM
    if act != "":
        return "deny", 0, "ไม่รู้จัก " + str(act)[:12], COL_DRY
    return "", 0, "", COL_DIM


# ---- 4) เครือข่าย ----
def connect_broker(w):
    # broker สาธารณะไม่ตอบเป็นพัก ๆ: ลอง 3 ครั้ง client_id ใหม่ทุกครั้ง
    for n in (1, 2, 3):
        if n > 1:
            show_note(w, "ลองต่อ broker ใหม่ %d/3" % n, COL_DRY)
            ui.poll()
        try:
            if mqtt.connect(BROKER, port=1883, keepalive=60,
                            client_id=CLIENT_ID + "-%04x" % (time.ticks_ms() & 0xFFFF)):
                return True
        except OSError:
            pass
    return False


def connect_farm(w):
    # WiFi -> IP -> broker + subscribe ขั้นไหนพังบอกตรงนั้น
    show_note(w, "ต่อ WiFi... จอนิ่งได้", COL_DRY)
    ui.poll()                          # ป้ายต้องขึ้นจอก่อนบรรทัดที่บล็อก
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "ต่อ WiFi ไม่ได้"
    linked = connect_broker(w)         # ลองได้ 3 ครั้ง (ดู connect_broker)
    if not linked or not mqtt.subscribe(TOPIC_CMD):     # subscribe ต้องมาหลัง connect เสมอ
        return "broker ไม่ตอบ: รอ 1 นาทีแล้วรันใหม่"
    return ""


def send_report(n, soil, tank, running, dry_min, water_s):
    # publish ตอนสายหลุดโยน OSError
    try:
        mqtt.publish(TOPIC, json.dumps({"id": TEAM, "n": n, "soil": soil, "tank": tank,
                                        "pump": 1 if running else 0, "sim": "soil tank",
                                        "dry_min": dry_min, "water_s": water_s}))
        return True
    except OSError:
        return False


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title, col):
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=col, max=12, value=1)
    return ui.Label(title, x=x + 12, y=y + 6, color=col, value=16)


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: เชื่อมต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("โรงเรือน: ดินแห้งตรงไหน รดจากมือถือ", x=12, y=6, color=COL_LEAF, value=24)
    w = {}
    card(12, 44, 470, 252, "ความชื้นดิน (VR1) ย้อนหลัง 2 นาที", COL_SOIL)
    w["chart"] = ui.Chart(x=24, y=74, w=446, h=150, color=COL_SOIL, min=0, max=100)
    w["chart"].prop(ui.PROP_CHART_POINTS, CHART_PTS)
    w["s_dry"] = w["chart"].add_series(COL_BAD)
    w["soil"] = ui.Label("ดิน -- %", x=24, y=234, color=COL_TEXT, value=28)
    w["soil_t"] = ui.Label("เส้นแดง = แห้งที่ %d %%" % SOIL_DRY, x=220, y=244, color=COL_BAD, value=16)
    w["title"] = card(494, 44, 286, 252, "วาล์วแปลง 0 (คำสั่ง 0)", COL_RAIN)
    w["led"] = ui.Led(x=510, y=82, w=40, h=40, color=COL_RAIN)
    w["pump"] = ui.Label("ปิด", x=562, y=88, color=COL_DIM, value=24)
    w["arc"] = ui.Arc(x=576, y=134, w=120, h=120, max=PUMP_MAX_S)
    w["arc"].color(COL_RAIN)
    ui.Label(BTN_NAMES[0] + " = ปิดวาล์วทันที", x=508, y=266, color=COL_DRY, value=14)
    ui.Label("ถังพัก (VR4)", x=12, y=306, color=COL_DIM, value=16)
    w["tank_bar"] = ui.Bar(x=112, y=310, w=170, h=16)
    w["tank"] = ui.Label("-- %", x=292, y=304, color=COL_TEXT, value=16)
    w["stats"] = ui.Label("ดินแห้งสะสม 0 นาที  รดไป 0 วิ", x=370, y=304, color=COL_LEAF, value=16)
    w["note"] = ui.Label("ยังไม่มีคำสั่ง", x=12, y=352, color=COL_DIM)
    w["mq"] = ui.Led(x=606, y=12, w=18, h=18, color=COL_LEAF, value=0)
    w["mq_t"] = ui.Label("MQTT: ออฟไลน์", x=632, y=10, color=COL_DIM, value=16)
    ui.poll()
    return w


def show_note(w, text, col):
    w["note"].color(col)
    w["note"].text(text)


def show_pump(w, sec_left):
    w["led"].value(1 if sec_left else 0)
    w["pump"].color(COL_RAIN if sec_left else COL_DIM)
    w["pump"].text("รดอยู่ อีก %d วิ" % sec_left if sec_left else "ปิด")
    w["arc"].value(sec_left)


def show_knobs(w, soil, tank):
    dry = soil < SOIL_DRY
    w["soil"].color(COL_DRY if dry else COL_TEXT)
    w["soil"].text("ดิน %d %%%s" % (soil, " แห้ง" if dry else ""))
    w["tank_bar"].value(tank)
    w["tank_bar"].color(COL_RAIN if tank >= TANK_MIN else COL_BAD)
    w["tank"].text("%d %%" % tank)


def show_stats(w, dry_min, water_s):
    w["stats"].text("ดินแห้งสะสม %d นาที  รดไป %d วิ" % (dry_min, water_s))


# ---- 6) โปรแกรมหลัก ----
class Stop(Exception):
    # ไม่ใช้ SystemExit: บอร์ดอาจค้าง
    pass


def stop(w, pump, msg, col=COL_BAD):
    # ปิดวาล์วก่อนเสมอ
    show_link(w, False)
    set_led(pump, False)
    rgbmatrix.scroll("")
    rgbmatrix.clear()
    show_note(w, msg, col)
    ui.poll()
    raise Stop


def act_on(w, raw, tank, now, pump_ms, pump_t0):
    do, arg, text, col = handle_command(raw, tank)
    if do == "off":
        pump_ms = 0
    elif do == "on":
        pump_ms, pump_t0 = arg * 1000, now
    elif do == "beep":
        beep("hit")
    elif do == "say" and arg and not pump_ms:
        rgbmatrix.scroll(arg, rgbmatrix.PURPLE, 80)
    tune = {"deny": "bad", "off": "stop", "on": "start", "say": "tap"}.get(do)
    if tune is not None:
        beep(tune)
    if text:
        show_note(w, text, col)
    return pump_ms, pump_t0


def main():
    if hasattr(ui, "volume"):
        ui.volume(SPEAKER)
    w = build_screen()
    pump = led_named("RGB_BLUE")
    rgbmatrix.clear()
    if len(TEAM) != 6 or TEAM[:4] != "team" or not TEAM[4:].isdigit():
        stop(w, pump, "แก้ TEAM เป็นเลขกลุ่มก่อน")
    problem = connect_farm(w)
    if problem:
        stop(w, pump, problem)
    show_link(w, True)
    show_note(w, "ฟัง " + TOPIC_CMD, COL_LEAF)
    stop_btn = Button(1)
    got = n = pump_ms = pump_t0 = 0
    dry_ms = water_ms = 0              # ตัวนับของเรื่องนี้ (มิลลิวินาที)
    shown, stats_shown = -1, None
    t_send = t_chart = t_last = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < LISTEN_MS:
        now = time.ticks_ms()
        dt, t_last = time.ticks_diff(now, t_last), now
        if stop_btn.pressed_now() and pump_ms:                      # SW5 ไม่ผ่านเน็ตเลย
            pump_ms = 0
            show_note(w, "ปิดวาล์วฉุกเฉิน " + BTN_NAMES[0], COL_BAD)
            beep("stop")
        soil, tank = knob_percent(0), knob_percent(3)
        msg = mqtt.get_message()       # None = ยังไม่มีอะไรมา / (topic, bytes)
        if msg is not None:
            got += 1
            w["title"].text("วาล์วแปลง 0 (คำสั่ง %d)" % got)
            pump_ms, pump_t0 = act_on(w, msg[1], tank, now, pump_ms, pump_t0)
        left = pump_ms - time.ticks_diff(now, pump_t0) if pump_ms else 0
        if pump_ms and (left <= 0 or tank < TANK_MIN):   # วาล์วปิดเองเมื่อครบเวลาหรือถังพักแห้ง
            pump_ms = 0
            show_note(w, "ครบเวลา ปิดวาล์วเอง" if left <= 0 else "ถังพักแห้ง ปิดวาล์วเอง", COL_RAIN)
            beep("stop")
        set_led(pump, pump_ms)
        if soil < SOIL_DRY:
            dry_ms += dt
        if pump_ms:
            water_ms += dt
        sec_left = (left + 999) // 1000 if pump_ms else 0     # ปัดขึ้น: สั่ง 10 วิ เริ่มนับที่ 10 ไม่ใช่ 11
        if sec_left != shown:
            shown = matrix_countdown(sec_left, shown)
            show_pump(w, sec_left)
        show_knobs(w, soil, tank)
        if time.ticks_diff(now, t_chart) >= CHART_MS:
            t_chart = now
            w["chart"].set_next(0, soil)
            w["chart"].set_next(w["s_dry"], SOIL_DRY)
        stats = (dry_ms // 60000, water_ms // 1000)
        if stats != stats_shown:       # เขียนจอเฉพาะตอนตัวเลขเปลี่ยน
            stats_shown = stats
            show_stats(w, stats[0], stats[1])
        if time.ticks_diff(now, t_send) >= SEND_MS:
            t_send, n = now, n + 1
            if not send_report(n, soil, tank, pump_ms, stats[0], stats[1]):
                stop(w, pump, "สายหลุด")
        if not mqtt.is_connected():
            stop(w, pump, "สายหลุด")
        ui.poll()
        wait_ms(POLL_MS, (stop_btn,))
    stop(w, pump, "เลิกฟัง ได้รับ %d คำสั่ง" % got, COL_DIM)


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
# 1) ส่ง {"cmd":"pump","on":1,"sec":9999} วาล์วเปิดกี่วิ ทำไมต้องมีเพดาน
# 2) ระดับ 2: ดินต่ำกว่า SOIL_DRY = เปิดวาล์วเองอัตโนมัติ เว้น 60 วิก่อนรดซ้ำ (ดู s1/sf1_03_auto_irrigation.py)
