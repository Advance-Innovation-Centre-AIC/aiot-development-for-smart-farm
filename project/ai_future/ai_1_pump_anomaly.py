# ai_1_pump_anomaly.py - ปั๊มสั่นผิดปกติ: กฎที่เขียนเอง กับ AI ตัดสินคู่กัน แล้วส่งผลขึ้น MQTT
#
# ตัวอย่างแนวทางอนาคต ไม่อยู่ในโจทย์โปรเจกต์รุ่นนี้
# ผล AI ใน BENTO Emulator เป็นผลจำลอง ต้องทดสอบบนบอร์ดจริง
#
# ภารกิจ   : วางบอร์ดบนตัวปั๊ม ให้สองฝ่ายตัดสินพร้อมกัน
#            กฎ = ค่าสั่นจาก IMU เกินค่าปกติ K_VIB เท่า (แบบ sf3_03)
#            AI = โมเดล AnomalousVibration ป้าย anomaly ชนะ CONF_MIN % อย่างน้อย 3 ใน 5 ผลล่าสุด (แบบ sf3_04)
#            ทั้งสองเห็นตรงกัน = เตือนจริง (ไฟแดง + เสียง) / ฝ่ายเดียว = ให้คนไปดู
# ลองเล่น  : วางนิ่ง 5 วินาทีแรก (กฎเรียนค่าปกติ) แล้วเขย่าเบา ๆ และเขย่าแรง ดูว่าใครเห็นก่อน
# โมเดล    : AnomalousVibration (Deploy จาก Edge AI Store) ถ้าไม่มีจึงใช้ Motion ที่ติดมากับบอร์ด
# สัญญา MQTT: bento-aiot/<TEAM>/ai ตามข้อ 3.11 ของ s2/app/MQTT_CONTRACT_th.md (m = "pump", rk = "vib")
#            ส่งเมื่อ label / lvl / alarm เปลี่ยน และทุก 2 วินาที ห่างกันอย่างน้อย 200 ms
# ยังไม่ได้ทดสอบ: อ่าน IMU พร้อมกับที่โมเดล IMU รันอยู่ได้ไหม (อ่านไม่ได้ จอจะบอก และกฎเป็น 0)
# ต้องแก้ก่อนรัน: WIFI_SSID, WIFI_PASS และ TEAM (ยังไม่แก้ = ทำงานออฟไลน์ พิมพ์ผลลง Console แทน)

import edge_ai
import gpio
import json
import math
import mqtt
import rgbmatrix
import sensors
import time
import ui
import wifi

# ---- 1) ตั้งค่า (แก้ได้) ----
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"   # ตั้งเอง: อังกฤษ/ตัวเลขสั้น ๆ ไม่มีเว้นวรรค
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"   # อย่างน้อย 8 ตัว · อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร
TEAM = "teamXX"                        # เลขกลุ่มที่ผู้สอนแจก เช่น team05 (ต้องตรงกับแอป)
BROKER = "broker.hivemq.com"
CLIENT_ID = "bento-ai-" + TEAM
TOPIC = "bento-aiot/" + TEAM + "/ai"
MODEL_KEYS = ("AnomalousVibration", "Motion")   # โมเดลจาก Store ก่อน ไม่มีค่อยใช้โมเดลในตัว
DANGER = ("anomaly", "shaking")       # ป้ายที่นับเป็นเหตุ (Motion ในตัวใช้ shaking)
CONF_MIN = 60            # AI มั่นใจไม่ถึงกี่ % ไม่นับ
CONFIRM_N, WINDOW_N = 3, 5
SAMPLE_MS = 50           # อ่าน IMU และถามผล AI ทุกกี่ ms
WIN = 20                 # คิดค่าสั่นจากกี่ตัวอย่างล่าสุด (เหมือน sf3_03)
LEARN_MS = 5000          # เรียนค่าสั่นปกติกี่ ms แรก (วางนิ่ง)
MIN_BASE = 0.05          # ค่าปกติต่ำสุด (m/s^2)
K_VIB = 3.0              # ค่าสั่นเกินค่าปกติกี่เท่า = กฎบอกเกิดเหตุ
HEARTBEAT_MS, GAP_MS = 2000, 200
RETRY_MS = 10000         # สายหลุด: ลองต่อใหม่ทุกเท่านี้
DRAW_MS = 300
RUN_MS = 300000
VOLUME, SPEAKER = 25, 40

AGREE_TH = {"both": "ยืนยันสองทาง: เตือน!", "ai": "AI เห็นฝ่ายเดียว: ให้คนไปดู",
            "rule": "กฎเห็นฝ่ายเดียว: ให้คนไปดู", "none": "ปกติ"}
COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def beep(*notes):
    for n in notes:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 120)
        time.sleep_ms(120)


def find_models(keys):
    # หาโมเดลจาก "ชื่อ" ตามลำดับใน keys (ลำดับบนบอร์ดเปลี่ยนได้หลังรีบูต)
    try:
        ms = edge_ai.models()
    except Exception:
        return []
    return [(m["index"], m["name"], m["labels"]) for key in keys for m in ms if key.lower() in m["name"].lower()]


def read_result():
    try:
        return edge_ai.result()
    except OSError:
        return None


def led_named(name):
    try:
        names = gpio.board_info()["led_names"]
        return gpio.led(names.index(name) if name in names else 0)
    except Exception:
        return None


def set_led(led, on):
    if led:
        led.on() if on else led.off()


def mx(cmd, *args):
    try:
        cmd(*args)
    except OSError:
        pass


def magnitude():
    # ขนาดความเร่งรวมสามแกน (m/s^2) แบบ sf3_03 · อ่านไม่ได้คืน None (ไม่เดาค่าแทน)
    try:
        ax, ay, az, _, _, _ = sensors.bmi270.motion()
    except Exception:
        return None
    return math.sqrt(ax * ax + ay * ay + az * az)


# ---- 3) สมอง (ตัดสินใจ) ----
class Vib:
    # ค่าสั่น = ส่วนเบี่ยงเบนมาตรฐานของ WIN ตัวอย่างล่าสุด · LEARN_MS แรก = เรียนค่าปกติ
    def __init__(self, now):
        self.buf, self.vib, self.fails = [], 0.0, 0
        self.base, self.total, self.n, self.t0 = None, 0.0, 0, now

    def push(self, m, now):
        if m is None:
            self.fails += 1
            return
        self.fails = 0
        self.buf = (self.buf + [m])[-WIN:]
        if len(self.buf) < WIN:
            return
        avg = sum(self.buf) / WIN
        self.vib = math.sqrt(sum((x - avg) * (x - avg) for x in self.buf) / WIN)
        if self.base is None:
            self.total, self.n = self.total + self.vib, self.n + 1
            if time.ticks_diff(now, self.t0) >= LEARN_MS:
                self.base = max(MIN_BASE, self.total / self.n)

    def limit(self):
        return self.base * K_VIB if self.base else 0.0

    def on(self):
        return self.base is not None and self.fails < WIN and self.vib >= self.limit()


def alert_rule(hist, alerting, danger):
    # เหมือน sf3_04: เหตุเมื่ออย่างน้อย CONFIRM_N ใน WINDOW_N ผลล่าสุด · หายเมื่อทั้งหน้าต่างปลอดภัย
    hist = (hist + [int(danger)])[-WINDOW_N:]
    hits = sum(hist)
    return hist, hits >= CONFIRM_N or (alerting and hits > 0)


def second_best(r, labels):
    s = r["scores"]
    if len(s) < 2:
        return "-", 0
    i = sorted(range(len(s)), key=lambda k: -s[k])[1]
    return (labels[i] if i < len(labels) else str(i)), int(s[i] * 100)


def agreement(ai_on, rule_on):
    return "both" if ai_on and rule_on else "ai" if ai_on else "rule" if rule_on else "none"


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


def send(online, obj):
    line = json.dumps(obj)
    print(line)
    if online:
        try:
            return bool(mqtt.publish(TOPIC, line))
        except OSError:
            pass
    return False


# ---- 5) หน้าจอ ----
def label(text, x, y, col=COL_DIM, size=16):
    return ui.Label(text, x=x, y=y, color=col, value=size)


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    label("ปั๊มสั่นผิดปกติ: กฎ กับ AI", 12, 6, COL_TEXT, 24)
    w = {"model": label("หาโมเดล...", 12, 44, COL_INFO, 20)}
    label("AI", 12, 90, COL_INFO, 20)
    w["arc"] = ui.Arc(x=12, y=120, w=150, h=150, min=0, max=100, value=0)
    w["ai"] = label("-", 180, 130, COL_TEXT, 24)
    w["ai2"] = label(" ", 180, 170)
    w["hist"] = label(" ", 180, 200)
    label("กฎ (IMU)", 420, 90, COL_INFO, 20)
    w["rule"] = label("กำลังเรียนค่าปกติ: วางนิ่ง", 420, 130, COL_TEXT, 20)
    w["rule2"] = label(" ", 420, 170)
    w["led"] = ui.Led(x=12, y=290, w=36, h=36, color=COL_BAD, value=0)
    w["agree"] = label("-", 60, 296, COL_TEXT, 20)
    w["mqtt"] = ui.Led(x=12, y=350, w=24, h=24, color=COL_OK, value=0)
    w["sent"] = label("ยังไม่ได้ส่ง", 44, 352)
    w["note"] = label(" ", 12, 400, COL_WARN, 16)
    ui.poll()
    return w


def draw(w, lab, conf, r2, hist, ai_on, vib, agree, online, n, n_ok):
    w["arc"].value(conf)
    w["arc"].color(COL_BAD if ai_on else COL_OK)
    w["ai"].text("%s %d %%" % (lab, conf))
    w["ai2"].text("อันดับสอง %s %d %%" % r2)
    w["hist"].text("เหตุ %d ใน %d ผลล่าสุด" % (sum(hist), len(hist)))
    if vib.fails >= WIN:
        w["rule"].text("อ่าน IMU ไม่ได้")
    elif vib.base is not None:
        w["rule"].text("สั่น %.2f / เกณฑ์ %.2f" % (vib.vib, vib.limit()))
    w["rule"].color(COL_BAD if vib.on() else COL_TEXT)
    w["rule2"].text("กฎบอก: เกิดเหตุ" if vib.on() else "กฎบอก: ปกติ")
    w["agree"].text(AGREE_TH[agree])
    w["agree"].color(COL_BAD if agree == "both" else COL_WARN if agree != "none" else COL_OK)
    w["mqtt"].value(1 if online else 0)
    w["sent"].text("ส่งได้ %d จาก %d ข้อความ" % (n_ok, n) + ("" if online else " (ออฟไลน์)"))


# ---- 6) โปรแกรมหลัก ----
def main():
    if hasattr(ui, "volume"):
        ui.volume(SPEAKER)
    w = build_screen()
    name = labels = None
    for idx, nm, lbs in find_models(MODEL_KEYS):
        try:
            edge_ai.select(idx)
            name, labels = nm, lbs
            break
        except OSError:
            pass
    if name is None:
        w["note"].color(COL_BAD)
        w["note"].text("ต้อง Deploy โมเดลจาก Edge AI Store ก่อน")
        ui.poll()
        print("ไม่พบโมเดล", MODEL_KEYS)
        return
    w["model"].text("โมเดล: " + name)
    can_net = team_ok() and not (WIFI_SSID[:1] in ("<", "") and not wifi.is_connected())
    if not can_net:
        w["note"].text("แก้ TEAM และ Hotspot ก่อน: ออฟไลน์")
    online = go_online() if can_net else False
    alarm_led = led_named("RGB_RED")
    now = time.ticks_ms()
    vib = Vib(now)
    seq, lab, conf, r2, ms, hist, ai_on, alarm = None, "-", 0, ("-", 0), 0, [], False, False
    n = n_ok = 0
    sent_key = None
    t0 = t_sent = t_retry = t_draw = now
    try:
        while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
            now = time.ticks_ms()
            vib.push(magnitude(), now)                         # 1) กฎ
            r = read_result()                                  # 2) AI
            if r and r["seq"] != seq:
                seq, lab, conf = r["seq"], r["label"] or "-", int(r["conf"] * 100)
                r2, ms = second_best(r, labels), int(r["latency_ms"])
                hist, ai_on = alert_rule(hist, ai_on, lab in DANGER and conf >= CONF_MIN)
            agree = agreement(ai_on, vib.on())                 # 3) เทียบสองฝ่าย
            lvl = 2 if agree == "both" else 0 if agree == "none" else 1
            if (lvl == 2) != alarm:
                alarm = lvl == 2
                set_led(alarm_led, alarm)
                mx(rgbmatrix.scroll, "PUMP ALARM" if alarm else "PUMP OK",
                   rgbmatrix.RED if alarm else rgbmatrix.GREEN, 80)
                beep(84, 76) if alarm else beep(79, 84)
            key = (lab, lvl, alarm)                            # 4) ส่งขึ้น MQTT (ข้อ 3.11)
            if should_send(key != sent_key, time.ticks_diff(time.ticks_ms(), t_sent)):
                n += 1
                ok = send(online, {"id": TEAM, "n": n, "m": "pump", "model": name, "label": lab,
                                   "conf": conf, "p2": r2[0], "c2": r2[1], "ms": ms, "rk": "vib",
                                   "rule": int(vib.on()), "rv": round(vib.vib, 2), "rl": round(vib.limit(), 2),
                                   "agree": agree, "lvl": lvl, "alarm": int(alarm)})
                n_ok += 1 if ok else 0
                t_sent, sent_key = time.ticks_ms(), key
                if online and not ok:
                    online = False
                    t_retry = now
            if online and not mqtt.is_connected():
                online, t_retry = False, now
            if can_net and not online and time.ticks_diff(now, t_retry) >= RETRY_MS:
                w["note"].text("กำลังต่อใหม่...")
                ui.poll()
                online, t_retry = go_online(), time.ticks_ms()
                w["note"].text("ต่อใหม่สำเร็จ" if online else "ยังต่อไม่ได้ จะลองใหม่เอง")
            if time.ticks_diff(now, t_draw) >= DRAW_MS:
                draw(w, lab, conf, r2, hist, ai_on, vib, agree, online, n, n_ok)
                t_draw = now
            ui.poll()
            time.sleep_ms(SAMPLE_MS)
    finally:                                                   # หยุดกลางทางก็ปล่อยแกน AI ดับไฟ ปิดสาย
        try:
            edge_ai.stop()
        except OSError:
            pass
        set_led(alarm_led, False)
        mx(rgbmatrix.clear)
        try:
            mqtt.disconnect()
        except OSError:
            pass
    w["note"].text("ครบเวลา - กด Program to Device เพื่อเล่นใหม่")
    ui.poll()


main()

# ----- แนวทางต่อยอด -----
# 1) ส่ง event ai_alarm_start / ai_alarm_end ขึ้น bento-aiot/<TEAM>/event ตามข้อ 3.10 ตอน alarm เปลี่ยน
# 2) เก็บค่าสั่นจากปั๊มจริงของฟาร์ม แล้วฝึกโมเดลใหม่ (โมเดลตัวอย่างไม่ได้ฝึกจากปั๊มจริง)
# 3) เทียบจำนวนครั้งที่ "ฝ่ายเดียว" ในหนึ่งวัน: ฝ่ายไหนเตือนมั่วบ่อยกว่า
