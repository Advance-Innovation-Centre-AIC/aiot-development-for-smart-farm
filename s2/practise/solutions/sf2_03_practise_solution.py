# sf2_03_practise.py - เฉลยแบบฝึกเติมโค้ด (Code Quest ระดับ 3): ตรวจคำสั่งปั๊มก่อนแตะของจริง
#
# วิธีเล่น  : ไฟล์นี้เหมือน sf2_03_remote_pump.py ทุกอย่าง ยกเว้นส่วน "3) สมอง" ที่เว้นช่อง ____ (ขีดล่างสี่ตัว)
#            ไว้ 3 ช่อง: A, B (ใน pump_seconds) และ C (ใน handle_command)
#            เติมให้ครบแล้วรัน โปรแกรมจะตรวจกฎของคุณ 8 กรณีก่อนเปิดจอ (self_test)
#            ผ่านครบ = Console ขึ้น "ผ่าน!" แล้วเล่นต่อได้เหมือนไฟล์ตัวอย่าง
#            ยังไม่ถูก = Console บอกว่ากรณีไหนผิด แล้วหยุด (ยังไม่เปิดจอ ยังไม่ต่อเน็ต)
# ถ้าเจอ   : NameError: name '____' isn't defined = ยังมีช่องที่ไม่ได้เติม (ตั้งใจให้หยุดชัด ๆ แบบนี้)
# ติดขัด?  : ใช้บันไดช่วยเหลือในใบงาน (คำใบ้ 3 ขั้นอยู่ท้ายใบงาน) หรือรันไฟล์ตัวอย่างเต็ม
#            sf2_03_remote_pump.py เพื่อไปต่อก่อน แล้วค่อยกลับมาเทียบกับของตัวเอง
# เฉลย     : โจทย์หลัก มีเฉลยในคาบ อยู่ที่ practise/solutions/sf2_03_practise_solution.py
#            (ลองเองก่อน แล้วค่อยเปิดดูเมื่อจำเป็น)
# ต้องแก้ก่อนรันบนบอร์ด: WIFI_SSID, WIFI_PASS, TEAM (ตรวจกฎผ่านได้โดยไม่ต้องต่อเน็ต)
#
# (ทำจาก sf2_03_remote_pump.py bef636922eee)

import buttons
import gpio
import json
import mqtt
import pots
import rgbmatrix
import time
import ui
import wifi

# ---- 1) ตั้งค่า (แก้ได้) ----
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"   # ตั้งเอง: อังกฤษ/ตัวเลขสั้น ๆ ไม่มีเว้นวรรค
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"   # อย่างน้อย 8 ตัว · อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร
TEAM = "teamXX"                        # เลขกลุ่มที่ผู้สอนแจก เช่น team05 (team00 = บอร์ดผู้สอนหน้าห้อง)

BROKER = "broker.hivemq.com"           # สำรอง: "test.mosquitto.org" ถ้าผู้สอนประกาศ
CLIENT_ID = "bento-farm-" + TEAM       # ต้องไม่ซ้ำกับใครบน broker (แอปจึงต่อท้ายด้วยตัวสุ่ม)
TOPIC_CMD = "bento-aiot/" + TEAM + "/cmd"
TOPIC = "bento-aiot/" + TEAM + "/telemetry"
BTN_NAMES = ("SW5", "SW6")             # ปุ่มล่าง = pressed(0), ปุ่มบน = pressed(1) ตามตัวอักษรบนแผง
PUMP_DEFAULT_S, PUMP_MAX_S = 10, 30    # ใครสั่ง 9999 วินาที ก็ได้แค่ 30
TANK_MIN = 10                          # น้ำในถังต่ำกว่านี้ ห้ามปั๊มทำงาน (ปั๊มแห้งพัง)
SEND_MS, POLL_MS, LISTEN_MS = 5000, 100, 1800000

VOLUME = 25                            # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้
COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----

# เสียงเตือนใช้ ui.tone เพราะปรับความดังได้ (ui.sfx ในเฟิร์มแวร์นี้ปรับความดังไม่ได้)
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)

def knob_percent(i):
    # ลูกบิด VR1-VR4 (i = 0-3) เป็น 0-100 %  บอร์ดไม่มี pots.percent() จึงคิดเอง
    return pots.read(i) * 100 // 4095


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


def matrix_countdown(sec_left, shown):
    # จอไฟ RGB: นับถอยหลังวินาทีที่ปั๊มเปิด เขียนเฉพาะตอนเลขเปลี่ยน (ทุกครั้งคือการเขียนบัส I2C)
    if shown <= 0 or sec_left == 0:
        rgbmatrix.scroll("")           # ปั๊มเริ่มหรือจบ: หยุดตัววิ่งก่อน จอไฟใช้ร่วมกัน
        rgbmatrix.clear()
    if sec_left:
        rgbmatrix.score(sec_left, rgbmatrix.BLUE)
    return sec_left


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ไม่แตะเน็ต ----
def pump_seconds(sec):
    # ตรวจเวลาที่สั่ง: ไม่ใช่จำนวนเต็มบวก = ค่าตั้งต้น  เกินเพดาน = เพดาน
    if not isinstance(sec, int) or sec <= 0:
        sec = PUMP_DEFAULT_S
    return min(sec, PUMP_MAX_S)


def ascii_only(text):
    # จอไฟ RGB รับแต่อักษรอังกฤษ ตัวเลข เครื่องหมาย กรองที่เหลือทิ้งและตัดให้สั้น
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
        return "deny", 0, "ไม่มีปั๊มดวงที่ " + str(cmd.get("n")), COL_WARN
    if act in ("led", "pump") and not on:
        return "off", 0, "สั่งปิดปั๊ม", COL_OK
    if act in ("led", "pump") and tank < TANK_MIN:
        return "deny", 0, "น้ำเหลือ %d%% ไม่ยอมเปิด" % tank, COL_BAD
    if act in ("led", "pump"):
        sec = pump_seconds(cmd.get("sec", PUMP_DEFAULT_S))
        return "on", sec, "เปิดปั๊ม %d วิ" % sec, COL_OK
    if act == "beep":
        return "beep", 0, "เจ้าของฟาร์มเรียก!", COL_INFO
    if act == "say":
        text = ascii_only(cmd.get("text", ""))
        return "say", text, "ข้อความ: " + (text or "ไม่มีอักษรอังกฤษ"), COL_INFO
    if act == "ack":                   # ปุ่มรับทราบในหน้าเว็บเป็นของ sf2_04 ไฟล์นี้ไม่มีแจ้งเตือน
        return "", 0, "ไม่มีแจ้งเตือน (sf2_04)", COL_INFO
    if act != "":
        return "deny", 0, "ไม่รู้จัก " + str(act)[:12], COL_WARN
    return "", 0, "", COL_INFO


def self_test():
    # ตรวจ handle_command() 8 กรณีก่อนเปิดจอ คืน True ถ้าผ่านครบ (ไม่ต่อเน็ต ไม่แตะปั๊มจริง)
    cases = ((b'{"cmd":"pump","on":1,"sec":9999}', 80, "on", 30),
             (b'{"cmd":"pump","on":1}', 80, "on", 10),
             (b'{"cmd":"pump","on":1,"sec":-5}', 80, "on", 10),
             (b'{"cmd":"pump","on":1,"sec":"20"}', 80, "on", 10),
             (b'{"cmd":"pump","on":1,"sec":30}', 80, "on", 30),
             (b'{"cmd":"pump","on":1,"sec":5}', 5, "deny", 0),
             (b'{"cmd":"pump","on":1,"sec":5}', 10, "on", 5),
             (b'{"cmd":"pump","on":0}', 5, "off", 0))
    for raw, tank, want_act, want_sec in cases:
        act, sec, _, _ = handle_command(raw, tank)
        if (act, sec) != (want_act, want_sec):
            print("ยังไม่ถูก:", raw.decode(), "ถัง", tank, "% ควรได้", want_act, want_sec, "แต่ได้", act, sec)
            return False
    print("ผ่าน! ตรวจคำสั่งปั๊มถูกทั้ง 8 กรณี")
    return True


# ---- 4) เครือข่าย ----
def connect_farm(w):
    # บันไดสามขั้น WiFi -> IP -> broker + subscribe ขั้นไหนพังคืนข้อความบอกว่าพังตรงไหน
    show_note(w, "ต่อ WiFi... จอนิ่งได้", COL_WARN)
    ui.poll()                          # ป้ายต้องขึ้นจอก่อนบรรทัดที่บล็อก
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "ต่อ WiFi ไม่ได้"
    try:
        linked = mqtt.connect(BROKER, port=1883, client_id=CLIENT_ID, keepalive=60)
    except OSError:
        linked = False
    if not linked or not mqtt.subscribe(TOPIC_CMD):     # subscribe ต้องมาหลัง connect เสมอ
        return "broker ไม่ตอบ พอร์ต 1883?"
    return ""


def send_report(n, soil, tank, running):
    # รายงานทุก 5 วิ (สัญญาข้อ 3.2) คืน False ถ้าสายหลุด (publish ตอนสายหลุดโยน OSError)
    try:
        mqtt.publish(TOPIC, json.dumps({"id": TEAM, "n": n, "soil": soil, "tank": tank,
                                        "pump": 1 if running else 0, "sim": "soil tank"}))
        return True
    except OSError:
        return False


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า (ทุกไฟล์ใช้แบบเดียวกัน) คืนป้ายหัวเรื่อง
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    return ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def build_screen():
    # สร้างทุกอย่างบนจอครั้งเดียว แล้วคืน dict ของ widget ที่ต้องอัปเดตภายหลัง
    ui.screen()
    time.sleep_ms(200)
    ui.Label("ปั๊มน้ำสั่งจากที่ไกล", x=12, y=6, color=COL_TEXT, value=24)
    w = {}
    w["title"] = card(12, 44, 440, 290, "ปั๊มน้ำ = ไฟสีฟ้า (คำสั่ง 0)")
    w["led"] = ui.Led(x=28, y=80, w=48, h=48, color=COL_INFO)       # สร้างมาแบบหรี่ = ปั๊มปิด
    w["pump"] = ui.Label("ปิด", x=90, y=90, color=COL_DIM, value=24)
    w["arc"] = ui.Arc(x=290, y=76, w=150, h=150, max=PUMP_MAX_S)
    w["arc"].color(COL_OK)             # สีตอนสร้างใช้ไม่ได้กับ Arc ต้องตั้งหลังสร้าง
    w["soil"] = ui.Label("ดิน (VR1) -- %", x=28, y=150, color=COL_TEXT)
    ui.Label("วงแหวน = วิที่เหลือ", x=290, y=234, color=COL_DIM, value=14)
    ui.Label(BTN_NAMES[0] + " = หยุดฉุกเฉิน ไม่พึ่งเน็ต", x=28, y=270, color=COL_WARN, value=16)
    card(462, 44, 318, 150, "น้ำในถัง (VR4)")
    w["tank_bar"] = ui.Bar(x=476, y=86, w=200, h=24)
    w["tank"] = ui.Label("-- %", x=690, y=82, color=COL_TEXT)
    ui.Label("ต่ำกว่า %d%% ไม่เปิด / เปิดสูงสุด %d วิ" % (TANK_MIN, PUMP_MAX_S), x=476, y=130, color=COL_DIM, value=14)
    w["note"] = ui.Label("ยังไม่มีคำสั่ง", x=12, y=352, color=COL_DIM)
    ui.poll()
    return w


def show_note(w, text, col):
    w["note"].color(col)
    w["note"].text(text)


def show_pump(w, sec_left):
    w["led"].value(1 if sec_left else 0)
    w["pump"].color(COL_OK if sec_left else COL_DIM)
    w["pump"].text("เปิด อีก %d วิ" % sec_left if sec_left else "ปิด")
    w["arc"].value(sec_left)


def show_knobs(w, soil, tank):
    w["soil"].text("ดิน (VR1) %d %%" % soil)
    w["tank_bar"].value(tank)
    w["tank_bar"].color(COL_INFO if tank >= TANK_MIN else COL_BAD)
    w["tank"].text("%d %%" % tank)


# ---- 6) โปรแกรมหลัก ----
def stop(w, pump, msg, col=COL_BAD):
    # จบเพราะอะไรก็ตาม ปั๊มต้องดับก่อน แล้วค่อยบอกเหตุผล
    set_led(pump, False)
    rgbmatrix.scroll("")
    rgbmatrix.clear()
    show_note(w, msg, col)
    ui.poll()
    raise SystemExit


def act_on(w, raw, tank, now, pump_ms, pump_t0):
    # คำสั่งหนึ่งใบ: ตัดสิน -> ทำ -> เสียง -> บอกบนจอ แล้วคืน (pump_ms, pump_t0) ใหม่
    do, arg, text, col = handle_command(raw, tank)
    if do == "off":
        pump_ms = 0
    elif do == "on":
        pump_ms, pump_t0 = arg * 1000, now
    elif do == "beep":
        beep("hit")                    # เจ้าของฟาร์มเรียก (โน้ต MIDI ไม่ใช่ความถี่ ดู TUNES)
    elif do == "say" and arg and not pump_ms:
        rgbmatrix.scroll(arg, rgbmatrix.PURPLE, 80)
    tune = {"deny": "bad", "off": "stop", "on": "start", "say": "tap"}.get(do)
    if tune is not None:
        beep(tune)                     # เสียงดังเฉพาะตอนมีคำสั่งเข้า ไม่ใช่ทุกรอบลูป
    if text:
        show_note(w, text, col)
    return pump_ms, pump_t0


def main():
    if not self_test():                # ตรวจกฎก่อน ไม่ผ่าน = ไม่เปิดจอ ไม่ต่อเน็ต
        return
    w = build_screen()
    pump = led_named("RGB_BLUE")       # ไฟสีฟ้าบนบอร์ด = ปั๊มน้ำ
    rgbmatrix.clear()
    if len(TEAM) != 6 or TEAM[:4] != "team" or not TEAM[4:].isdigit():
        stop(w, pump, "แก้ TEAM เป็นเลขกลุ่มก่อน")
    problem = connect_farm(w)
    if problem:
        stop(w, pump, problem)
    show_note(w, "ฟัง " + TOPIC_CMD, COL_OK)
    stop_btn = Button(0)
    got = n = pump_ms = pump_t0 = 0
    shown = -1
    t_send = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < LISTEN_MS:
        now = time.ticks_ms()
        if stop_btn.pressed_now() and pump_ms:                      # SW5 ไม่ผ่านเน็ตเลย
            pump_ms = 0
            show_note(w, "หยุดฉุกเฉิน " + BTN_NAMES[0], COL_BAD)
            beep("stop")
        soil, tank = knob_percent(0), knob_percent(3)
        msg = mqtt.get_message()       # None = ยังไม่มีอะไรมา / (topic, bytes)
        if msg is not None:
            got += 1
            w["title"].text("ปั๊มน้ำ = ไฟสีฟ้า (คำสั่ง %d)" % got)
            pump_ms, pump_t0 = act_on(w, msg[1], tank, now, pump_ms, pump_t0)
        left = pump_ms - time.ticks_diff(now, pump_t0) if pump_ms else 0
        if pump_ms and (left <= 0 or tank < TANK_MIN):   # ปั๊มดับเองเมื่อครบเวลาหรือน้ำหมดถัง
            pump_ms = 0
            show_note(w, "ครบเวลา ดับเอง" if left <= 0 else "ถังแห้ง ดับเอง", COL_INFO)
            beep("stop")
        set_led(pump, pump_ms)
        sec_left = left // 1000 + 1 if pump_ms else 0
        if sec_left != shown:
            shown = matrix_countdown(sec_left, shown)
            show_pump(w, sec_left)
        show_knobs(w, soil, tank)
        if time.ticks_diff(now, t_send) >= SEND_MS:   # รายงานทุก 5 วิ ให้แอปเห็นว่าปั๊มทำงานจริง
            t_send, n = now, n + 1
            if not send_report(n, soil, tank, pump_ms):
                stop(w, pump, "สายหลุด")
        if not mqtt.is_connected():
            stop(w, pump, "สายหลุด")
        ui.poll()
        wait_ms(POLL_MS, (stop_btn,))
    stop(w, pump, "เลิกฟัง ได้รับ %d คำสั่ง" % got, COL_DIM)   # ดับปั๊มก่อนจบเสมอ


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) ส่ง {"cmd":"pump","on":1,"sec":9999} จากช่อง JSON ใน app/mqtt_dashboard.html (ใส่เลขกลุ่มในช่องทีม)
#    ปั๊มเปิดนานเท่าไร ทำไมฟาร์มจริงต้องมีเพดานนี้ (ดูฟังก์ชัน pump_seconds)
# 2) เพิ่มคำสั่ง {"cmd":"set","tank_min":20} ใน handle_command ให้แอปของกลุ่มเปลี่ยนเกณฑ์ถังน้ำได้จากที่ไกล
#    อย่าลืมกันค่าแปลก ๆ เช่น -5 หรือ 500 (handle_command ต้องไม่แตะฮาร์ดแวร์ แค่ตัดสิน)
