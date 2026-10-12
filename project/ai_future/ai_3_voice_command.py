# ai_3_voice_command.py - สั่งปั๊ม (จำลอง) ด้วยเสียง "go" / "stop" แล้วกดปุ่มยืนยันก่อนทำจริง
#
# ตัวอย่างแนวทางอนาคต ไม่อยู่ในโจทย์โปรเจกต์รุ่นนี้
# ผล AI ใน BENTO Emulator เป็นผลจำลอง ต้องทดสอบบนบอร์ดจริง
#
# ภารกิจ   : พูด "go" = ขอเปิดปั๊ม · "stop" = ขอปิดปั๊ม · บอร์ดไม่ทำทันที แต่ถามก่อน
#            กด SW6 (ปุ่มบน) ภายใน CONFIRM_MS = ยืนยัน · SW5 (ปุ่มล่าง) = ยกเลิก · ไม่กด = ไม่ทำ
#            ปั๊มในไฟล์นี้เป็นปั๊มจำลอง (ไฟบนจอ + จอไฟ RGB) ไม่สั่งรีเลย์หรือ PLC จริง
# โมเดล    : ชื่อบนบอร์ด "Voice Commands" (Edge AI Store การ์ด VoiceCommands, Keyword spotting)
#            คลาสของโมเดล: unlabeled left right up down stop go ไม่มี forward / backward
#            ไฟล์นี้ใช้แค่ go กับ stop · คำอื่นแสดงบนจอเฉย ๆ ไม่ทำอะไร
# ผลขึ้นกับ firmware และความไวไมค์ (mic gain): ทดสอบ 2026-10-10 ได้ 14/20 บน Dev Kit fw 2.5.1
#            และ 0/20 บน firmware รุ่นเก่ากว่า · ห้องเสียงดัง ระยะ มุมไมค์ ก็มีผล
# สัญญา MQTT: bento-aiot/<TEAM>/ai ตามข้อ 3.9 และ event voice_cmd ตามข้อ 3.11 (m = "voice")
# ต้องแก้ก่อนรัน: WIFI_SSID, WIFI_PASS และ TEAM (ยังไม่แก้ = ทำงานออฟไลน์ พิมพ์ผลลง Console แทน)

try:
    import buttons
except ImportError:      # บอร์ดที่ไม่มีปุ่ม: ยืนยันไม่ได้ ปั๊มจึงไม่ขยับ
    buttons = None
import edge_ai
import json
import mqtt
import rgbmatrix
import time
import ui
import wifi

# ---- 1) ตั้งค่า (แก้ได้) ----
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"   # ตั้งเอง: อังกฤษ/ตัวเลขสั้น ๆ ไม่มีเว้นวรรค
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"   # อย่างน้อย 8 ตัว · อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร
TEAM = "teamXX"                        # เลขกลุ่มที่ผู้สอนแจก เช่น team05 (ต้องตรงกับแอป)
BROKER = "broker.hivemq.com"
CLIENT_ID = "bento-ai-" + TEAM
BASE = "bento-aiot/" + TEAM + "/"
MODEL_KEYS = ("Voice Commands",)       # ชื่อที่บอร์ดรายงาน ไม่ใช่ชื่อการ์ดใน Store
WORDS = ("go", "stop")
CONF_MIN = 60            # มั่นใจไม่ถึงกี่ % ไม่นับ
K, N = 2, 4              # คำนั้นต้องชนะอย่างน้อย K ใน N ผลล่าสุด (แบบ s3/models/voice_commands.py)
REFRACT_MS = 1000        # ได้คำแล้ว ไม่รับคำใหม่ช่วงนี้
CONFIRM_MS = 5000        # รอกดยืนยันได้นานเท่านี้
MUTE_MS = 800            # หลังบอร์ดส่งเสียงเอง ไม่เชื่อผลช่วงนี้ (เสียงเข้าไมค์)
SW6, SW5 = 0, 1          # ลำดับปุ่มใน buttons: 0 = SW6 (บน) · 1 = SW5 (ล่าง)
HEARTBEAT_MS, GAP_MS = 2000, 200
RETRY_MS = 10000
TICK_MS = 50
RUN_MS = 300000
VOLUME, SPEAKER = 25, 40

ASK = {"go": "เปิดปั๊ม", "stop": "ปิดปั๊ม"}
COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def beep(*notes):
    for n in notes:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 120)
        time.sleep_ms(120)


def find_models(keys):
    try:
        ms = edge_ai.models()
    except Exception:
        return []
    return [(m["index"], m["name"]) for key in keys for m in ms if key.lower() in m["name"].lower()]


def read_result():
    try:
        return edge_ai.result()
    except OSError:
        return None


def mx(cmd, *args):
    try:
        cmd(*args)
    except OSError:
        pass


class Button:
    # pressed_now() = True ครั้งเดียวต่อการกด (แบบ sf3_08)
    def __init__(self, index):
        self.index, self.down, self.clicked = index, False, False

    def sample(self):
        now_down = buttons.pressed(self.index) if buttons else False
        if now_down and not self.down:
            self.clicked = True
        self.down = now_down

    def pressed_now(self):
        fired, self.clicked = self.clicked, False
        return fired


# ---- 3) สมอง ----
def heard(hist):
    # คำใน WORDS ที่ชนะอย่างน้อย K ครั้งใน N ผลล่าสุด หรือ None
    for wd in WORDS:
        if hist.count(wd) >= K:
            return wd
    return None


def should_send(changed, since_ms):
    return since_ms >= GAP_MS and (changed or since_ms >= HEARTBEAT_MS)


# ---- 4) เครือข่าย (เหมือน sf3_06 แบบย่อ) ----
def team_ok():
    return len(TEAM) == 6 and TEAM[:4] == "team" and TEAM[4:].isdigit() and TEAM != "team00"


def wifi_up():
    if not wifi.is_connected():
        if WIFI_SSID[:1] in ("<", ""):
            return False
        if not wifi.connect(WIFI_SSID, WIFI_PASS):
            return False
    return wifi.ip() != "0.0.0.0"


def go_online():
    try:
        return wifi_up() and bool(mqtt.connect(BROKER, port=1883, keepalive=30,
                                               client_id=CLIENT_ID + "-%04x" % (time.ticks_ms() & 0xFFFF)))
    except OSError:
        return False


def send(online, topic, obj):
    line = json.dumps(obj)
    print(line)
    if online:
        try:
            return bool(mqtt.publish(BASE + topic, line))
        except OSError:
            pass
    return False


# ---- 5) หน้าจอ ----
def label(text, x, y, col=COL_DIM, size=16):
    return ui.Label(text, x=x, y=y, color=col, value=size)


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    label("สั่งปั๊มด้วยเสียง (Edge AI)", 12, 6, COL_TEXT, 24)
    w = {"model": label("หาโมเดล...", 12, 44, COL_INFO, 20)}
    label("พูด go = เปิดปั๊ม   stop = ปิดปั๊ม", 12, 76)
    w["arc"] = ui.Arc(x=12, y=110, w=150, h=150, min=0, max=100, value=0)
    w["word"] = label("-", 180, 120, COL_TEXT, 28)
    w["conf"] = label("มั่นใจ - %", 180, 164)
    w["ask"] = label(" ", 180, 200, COL_WARN, 20)
    w["pump"] = ui.Led(x=12, y=290, w=40, h=40, color=COL_OK, value=0)
    w["pump_txt"] = label("ปั๊ม (จำลอง): ปิด", 64, 298, COL_TEXT, 20)
    w["mqtt"] = ui.Led(x=12, y=350, w=24, h=24, color=COL_OK, value=0)
    w["sent"] = label("ยังไม่ได้ส่ง", 44, 352)
    w["note"] = label(" ", 12, 400, COL_WARN, 16)
    ui.poll()
    return w


def show_pump(w, pump):
    w["pump"].value(1 if pump else 0)
    w["pump_txt"].text("ปั๊ม (จำลอง): เปิด" if pump else "ปั๊ม (จำลอง): ปิด")
    mx(rgbmatrix.scroll, "PUMP ON" if pump else "PUMP OFF",
       rgbmatrix.GREEN if pump else rgbmatrix.RED, 80)


# ---- 6) โปรแกรมหลัก ----
def main():
    if hasattr(ui, "volume"):
        ui.volume(SPEAKER)
    w = build_screen()
    name = None
    for idx, nm in find_models(MODEL_KEYS):
        try:
            edge_ai.select(idx)
            name = nm
            break
        except OSError:
            pass
    if name is None:
        w["model"].text("ไม่พบโมเดล Voice Commands")
        w["note"].color(COL_BAD)
        w["note"].text("ต้อง Deploy โมเดลจาก Edge AI Store ก่อน")
        ui.poll()
        print("ไม่พบโมเดล", MODEL_KEYS)
        return
    w["model"].text("โมเดล: " + name)
    if buttons is None:
        w["note"].text("ไม่มีปุ่ม SW6: ปั๊มจะไม่ขยับ")
    can_net = team_ok() and not (WIFI_SSID[:1] in ("<", "") and not wifi.is_connected())
    online = go_online() if can_net else False
    w["mqtt"].value(1 if online else 0)
    sw6, sw5 = Button(SW6), Button(SW5)
    seq, lab, conf, hist, pump, want = None, "-", 0, [], False, None
    n = n_ok = 0
    sent_key = None
    t0 = t_sent = t_retry = t_word = t_ask = quiet_at = time.ticks_ms()
    show_pump(w, pump)
    try:
        while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
            now = time.ticks_ms()
            sw6.sample()
            sw5.sample()
            r = read_result()                                       # 1) ฟัง
            if r and r["seq"] != seq and time.ticks_diff(now, quiet_at) >= 0:
                seq, lab, conf = r["seq"], r["label"] or "-", int(r["conf"] * 100)
                hist = (hist + [lab if conf >= CONF_MIN else None])[-N:]
                w["word"].text(lab)
                w["conf"].text("มั่นใจ %d %%" % conf)
                w["arc"].value(conf)
                wd = heard(hist)
                if wd and want is None and time.ticks_diff(now, t_word) >= REFRACT_MS:
                    want, t_ask, t_word, hist = wd, now, now, []   # 2) ได้คำ: ถามก่อน ยังไม่ทำ
                    w["ask"].text("ได้ยิน %s: กด SW6 เพื่อ%s (SW5 ยกเลิก)" % (wd, ASK[wd]))
                    mx(rgbmatrix.scroll, wd.upper() + "?", rgbmatrix.BLUE, 80)
            if want:                                                # 3) รอคนยืนยัน
                why = None
                if sw6.pressed_now():
                    pump, why = want == "go", ""
                    show_pump(w, pump)
                    beep(79, 84)
                elif sw5.pressed_now():
                    why = "cancel"
                elif time.ticks_diff(now, t_ask) >= CONFIRM_MS:
                    why = "unconfirmed"
                if why is not None:
                    quiet_at = time.ticks_add(time.ticks_ms(), MUTE_MS)
                    w["ask"].text("ทำแล้ว: " + ASK[want] if why == "" else "ไม่ได้ทำ: " + why)
                    if why:
                        show_pump(w, pump)
                    send(online, "event", {"id": TEAM, "event": "voice_cmd", "m": "voice",
                                           "cmd": want, "ok": int(why == ""), "why": why})
                    want = None
            else:
                sw6.pressed_now()                                   # กดตอนไม่มีคำถาม = ไม่นับ
                sw5.pressed_now()
            if should_send(lab != sent_key, time.ticks_diff(time.ticks_ms(), t_sent)):
                n += 1                                              # 4) ผล AI ขึ้น .../ai (ข้อ 3.9)
                ok = send(online, "ai", {"id": TEAM, "n": n, "model": name, "label": lab, "conf": conf})
                n_ok += 1 if ok else 0
                t_sent, sent_key = time.ticks_ms(), lab
                w["sent"].text("ส่งได้ %d จาก %d ข้อความ" % (n_ok, n) + ("" if online else " (ออฟไลน์)"))
                if online and not ok:
                    online, t_retry = False, now
            if online and not mqtt.is_connected():
                online, t_retry = False, now
            if can_net and not online and time.ticks_diff(now, t_retry) >= RETRY_MS:
                online, t_retry = go_online(), time.ticks_ms()
            w["mqtt"].value(1 if online else 0)
            ui.poll()
            time.sleep_ms(TICK_MS)
    finally:
        try:
            edge_ai.stop()
        except OSError:
            pass
        mx(rgbmatrix.clear)
        try:
            mqtt.disconnect()
        except OSError:
            pass
    w["note"].text("ครบเวลา - กด Program to Device เพื่อเล่นใหม่")
    ui.poll()


main()

# ----- แนวทางต่อยอด -----
# 1) ปั๊มจริงในฟาร์ม: คำสั่งผ่าน Gateway ไป plc/cmd (ข้อ 4.2) ไม่ใช่สั่งจากไฟล์นี้ตรง
# 2) ถกกันในกลุ่ม: "stop" ควรทำทันทีโดยไม่ต้องยืนยันไหม (ปิดปั๊มปลอดภัยกว่าเปิด)
# 3) นับผลเอง 20 ครั้งต่อคำ ในห้องเงียบ กับห้องที่มีเสียงพัดลม เทียบกับ 14/20 ข้างบน
