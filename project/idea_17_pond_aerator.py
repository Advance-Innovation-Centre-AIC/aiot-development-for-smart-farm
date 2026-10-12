# idea_17_pond_aerator.py - เติมอากาศบ่อปลา: ออกซิเจนตกตอนกลางคืน สั่งเครื่องตีน้ำจากมือถือ
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# ต่อยอดจาก s2/sf2_03_remote_pump.py · ปั๊ม = เครื่องตีน้ำ, tank = น้ำในบ่อ (VR4)
# soil = DO จำลองจาก VR1 (0-100 = 0.0-10.0 mg/L) · ภาพหลัก = เลข DO + บันไดไฟ 3 ขั้น
# MQTT: ของเดิม sf2_03 ครบ (สัญญา 3.2, 4) + 2 คีย์ใหม่ที่บอร์ดคิดเอง
#   dips = DO ตกต่ำกว่า DO_LOW กี่ครั้ง  do_min = DO ต่ำสุด x10
# ทีมต้องแก้ 3 อย่าง: 1) DO_LOW DO_OK PUMP_MAX_S จากค่าที่ลอง 2) หัวจอและป้าย 3) เสียง
# เกณฑ์ = ค่าสาธิตในห้องเรียน
#
# ลองเล่น  : สั่งจาก project/web/idea_17.html?team=<เลขกลุ่ม>
# ต้องแก้ก่อนรัน: WIFI_SSID, WIFI_PASS, TEAM (เลขกลุ่ม)
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
TANK_MIN = 10  # น้ำในบ่อต่ำกว่านี้ใบพัดตีลม ห้ามเปิด
DO_LOW, DO_OK = 30, 50                 # x10: ต่ำกว่า 3.0 = ปลาลอยหัว  ตั้งแต่ 5.0 = สบาย
SEND_MS, POLL_MS, LISTEN_MS = 5000, 100, 1800000

SPEAKER = 40
VOLUME = 25
# สีบ่อกลางคืน
COL_TEXT, COL_DIM = 0xE2F2F0, 0x86A3A0
COL_CARD = 0x0F262B
COL_O2, COL_NIGHT, COL_WATCH = 0x3FD6C6, 0x6C7BD9, 0xF2B636
COL_GASP, COL_CALM = 0xE5484D, 0x47C774


# ---- 2) ฮาร์ดแวร์ ----
TUNES = {"tap": (76,), "start": (67, 74), "stop": (74, 67), "good": (72, 79, 84),
         "bad": (84, 76), "gasp": (88, 84, 88, 84), "hit": (88,)}


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


def do_text(do10):
    return "%d.%d" % (do10 // 10, do10 % 10)


def do_step(do10):
    # 0 สบาย  1 เฝ้าระวัง  2 ปลาลอยหัว
    return 0 if do10 >= DO_OK else 1 if do10 >= DO_LOW else 2


def handle_command(raw, tank):
    # ตัดสินคำสั่ง -> (ทำอะไร, ค่า, ข้อความจอ, สี) ไม่แตะของจริง
    try:
        cmd = json.loads(raw.decode())
    except ValueError:
        cmd = None
    if not isinstance(cmd, dict):      # 5, null, [] ก็เป็น JSON ได้ แต่ไม่ใช่คำสั่ง
        return "deny", 0, "อ่านคำสั่งไม่ได้", COL_GASP
    act, on = cmd.get("cmd", ""), cmd.get("on", 1)
    if act == "led" and cmd.get("n", 0) != 0:
        return "deny", 0, "ไม่มีเครื่องที่ " + str(cmd.get("n"))[:6], COL_WATCH
    if act in ("led", "pump") and not on:
        return "off", 0, "สั่งหยุดเครื่องตีน้ำ", COL_CALM
    if act in ("led", "pump") and tank < TANK_MIN:
        return "deny", 0, "น้ำในบ่อเหลือ %d%% ไม่เปิด" % tank, COL_GASP
    if act in ("led", "pump"):
        sec = pump_seconds(cmd.get("sec", PUMP_DEFAULT_S))
        return "on", sec, "เปิดเครื่องตีน้ำ %d วิ" % sec, COL_O2
    if act == "beep":
        return "beep", 0, "เจ้าของบ่อเรียก!", COL_NIGHT
    if act == "say":
        text = ascii_only(cmd.get("text", ""))
        return "say", text, "ข้อความ: " + (text or "ไม่มีอักษรอังกฤษ"), COL_NIGHT
    if act == "ack":  # ack เป็นของ sf2_04
        return "", 0, "ไม่มีแจ้งเตือน (sf2_04)", COL_DIM
    if act != "":
        return "deny", 0, "ไม่รู้จัก " + str(act)[:12], COL_WATCH
    return "", 0, "", COL_DIM


# ---- 4) เครือข่าย ----
def connect_broker(w):
    # broker สาธารณะไม่ตอบเป็นพัก ๆ: ลอง 3 ครั้ง client_id ใหม่ทุกครั้ง
    for n in (1, 2, 3):
        if n > 1:
            show_note(w, "ลองต่อ broker ใหม่ %d/3" % n, COL_WATCH)
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
    show_note(w, "ต่อ WiFi... จอนิ่งได้", COL_WATCH)
    ui.poll()                          # ป้ายต้องขึ้นจอก่อนบรรทัดที่บล็อก
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "ต่อ WiFi ไม่ได้"
    linked = connect_broker(w)
    if not linked or not mqtt.subscribe(TOPIC_CMD):     # subscribe ต้องมาหลัง connect เสมอ
        return "broker ไม่ตอบ: รอ 1 นาทีแล้วรันใหม่"
    return ""


def send_report(n, do10, tank, running, dips, do_min):
    # publish ตอนสายหลุดโยน OSError
    try:
        mqtt.publish(TOPIC, json.dumps({"id": TEAM, "n": n, "soil": do10, "tank": tank,
                                        "pump": 1 if running else 0, "sim": "soil tank",
                                        "dips": dips, "do_min": do_min}))
        return True
    except OSError:
        return False


# ---- 5) หน้าจอ ----
STEP_WORDS = ("ปลาสบาย", "เฝ้าระวัง", "ปลาลอยหัว!")
STEP_COLS = (COL_CALM, COL_WATCH, COL_GASP)


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: เชื่อมต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("บ่อปลา: เฝ้าออกซิเจนทั้งคืน", x=12, y=6, color=COL_O2, value=24)
    w = {}
    ui.Panel(x=12, y=44, w=400, h=250, color=COL_CARD, min=COL_NIGHT, max=16, value=1)
    ui.Label("ออกซิเจนละลายน้ำ (VR1)", x=26, y=52, color=COL_NIGHT, value=16)
    w["do"] = ui.Label("DO --", x=26, y=88, color=COL_TEXT, value=28)
    ui.Label("mg/L", x=190, y=96, color=COL_DIM, value=16)
    w["step"] = ui.Label("--", x=26, y=132, color=COL_DIM, value=24)
    w["ladder"] = []
    for i in range(3):
        w["ladder"].append(ui.Led(x=330, y=70 + i * 52, w=36, h=36, color=STEP_COLS[i], value=0))
    w["dip"] = ui.Label("ยังไม่เคยต่ำ", x=26, y=180, color=COL_DIM, value=16)
    w["stats"] = ui.Label("ตกต่ำ 0 ครั้ง  ต่ำสุด --", x=26, y=212, color=COL_O2, value=16)
    ui.Label("ต่ำกว่า %s = ปลาลอยหัว" % do_text(DO_LOW), x=26, y=258, color=COL_DIM, value=14)
    ui.Panel(x=424, y=44, w=356, h=250, color=COL_CARD, min=COL_O2, max=16, value=1)
    w["title"] = ui.Label("เครื่องตีน้ำ (คำสั่ง 0)", x=438, y=52, color=COL_O2, value=16)
    w["led"] = ui.Led(x=440, y=86, w=40, h=40, color=COL_O2)
    w["pump"] = ui.Label("หยุด", x=494, y=92, color=COL_DIM, value=24)
    w["arc"] = ui.Arc(x=620, y=130, w=120, h=120, max=PUMP_MAX_S)
    w["arc"].color(COL_O2)
    w["tank"] = ui.Label("น้ำในบ่อ (VR4) -- %", x=438, y=150, color=COL_TEXT, value=16)
    w["tank_bar"] = ui.Bar(x=440, y=180, w=160, h=14)
    ui.Label(BTN_NAMES[0] + " = หยุดเครื่องทันที", x=438, y=262, color=COL_WATCH, value=14)
    w["note"] = ui.Label("ยังไม่มีคำสั่ง", x=12, y=352, color=COL_DIM)
    w["mq"] = ui.Led(x=606, y=12, w=18, h=18, color=COL_CALM, value=0)
    w["mq_t"] = ui.Label("MQTT: ออฟไลน์", x=632, y=10, color=COL_DIM, value=16)
    ui.poll()
    return w


def show_note(w, text, col):
    w["note"].color(col)
    w["note"].text(text)


def show_pump(w, sec_left):
    w["led"].value(1 if sec_left else 0)
    w["pump"].color(COL_O2 if sec_left else COL_DIM)
    w["pump"].text("ตีน้ำ อีก %d วิ" % sec_left if sec_left else "หยุด")
    w["arc"].value(sec_left)


def show_step(w, step):
    for i in range(3):
        w["ladder"][i].value(1 if i == step else 0)
    w["step"].color(STEP_COLS[step])
    w["step"].text(STEP_WORDS[step])


def show_pond(w, do10, tank):
    w["do"].color(STEP_COLS[do_step(do10)])
    w["do"].text("DO " + do_text(do10))
    w["tank_bar"].value(tank)
    w["tank_bar"].color(COL_O2 if tank >= TANK_MIN else COL_GASP)
    w["tank"].text("น้ำในบ่อ (VR4) %d %%" % tank)


# ---- 6) โปรแกรมหลัก ----
class Stop(Exception):
    # ไม่ใช้ SystemExit: บอร์ดอาจค้าง
    pass


def stop(w, pump, msg, col=COL_GASP):
    # หยุดเครื่องก่อนเสมอ
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
    show_note(w, "ฟัง " + TOPIC_CMD, COL_CALM)
    stop_btn = Button(1)
    got = n = pump_ms = pump_t0 = dips = 0
    do_min, step_shown = 100, -1
    dip_t0 = None                      # None = ตอนนี้ไม่ต่ำ
    shown = -1
    t_send = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < LISTEN_MS:
        now = time.ticks_ms()
        if stop_btn.pressed_now() and pump_ms:                      # SW5 ไม่ผ่านเน็ตเลย
            pump_ms = 0
            show_note(w, "หยุดเครื่องตีน้ำฉุกเฉิน " + BTN_NAMES[0], COL_GASP)
            beep("stop")
        do10, tank = knob_percent(0), knob_percent(3)
        do_min = min(do_min, do10)
        step = do_step(do10)
        if step != step_shown:         # ขั้นเปลี่ยน = วาดครั้งเดียว
            if step == 2 and dip_t0 is None:
                dips, dip_t0 = dips + 1, now
                beep("gasp")
                if not pump_ms:
                    rgbmatrix.scroll("O2 LOW", rgbmatrix.RED, 80)
            elif step < 2 and dip_t0 is not None:
                dip_t0 = None
                if not pump_ms:
                    rgbmatrix.scroll("")
                    rgbmatrix.clear()
            step_shown = step
            show_step(w, step)
        if dip_t0 is not None:
            dip_s = time.ticks_diff(now, dip_t0) // 1000
            w["dip"].color(COL_GASP)
            w["dip"].text("ต่ำมาแล้ว %d:%02d นาที" % (dip_s // 60, dip_s % 60))
        else:
            w["dip"].color(COL_DIM)
            w["dip"].text("ตอนนี้ไม่ต่ำ")
        w["stats"].text("ตกต่ำ %d ครั้ง ต่ำสุด %s" % (dips, do_text(do_min)))
        msg = mqtt.get_message()       # None = ยังไม่มีอะไรมา / (topic, bytes)
        if msg is not None:
            got += 1
            w["title"].text("เครื่องตีน้ำ (คำสั่ง %d)" % got)
            pump_ms, pump_t0 = act_on(w, msg[1], tank, now, pump_ms, pump_t0)
        left = pump_ms - time.ticks_diff(now, pump_t0) if pump_ms else 0
        if pump_ms and (left <= 0 or tank < TANK_MIN):   # หยุดเองเมื่อครบเวลาหรือน้ำต่ำ
            pump_ms = 0
            show_note(w, "ครบเวลา หยุดเครื่องเอง" if left <= 0 else "น้ำต่ำกว่าใบพัด หยุดเอง", COL_O2)
            beep("stop")
        set_led(pump, pump_ms)
        sec_left = (left + 999) // 1000 if pump_ms else 0     # ปัดขึ้น: สั่ง 10 วิ เริ่มนับที่ 10 ไม่ใช่ 11
        if sec_left != shown:
            shown = matrix_countdown(sec_left, shown)
            show_pump(w, sec_left)
        show_pond(w, do10, tank)
        if time.ticks_diff(now, t_send) >= SEND_MS:
            t_send, n = now, n + 1
            if not send_report(n, do10, tank, pump_ms, dips, do_min):
                stop(w, pump, "สายหลุด")
        if not mqtt.is_connected():
            stop(w, pump, "สายหลุด")
        ui.poll()
        wait_ms(POLL_MS, (stop_btn,))
    stop(w, pump, "เลิกเฝ้า DO ตกต่ำ %d ครั้ง" % dips, COL_DIM)


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
# 1) หมุน VR1 กลับไปมาตรง 3.0 dips ขึ้นรัว ๆ ไหม ลองเพิ่มช่วงกันแกว่ง
# 2) ระดับ 2: DO ต่ำ = เปิดเครื่องเอง เว้น 1 นาทีก่อนเปิดซ้ำ
