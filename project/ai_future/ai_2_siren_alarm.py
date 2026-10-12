# ai_2_siren_alarm.py - ฟังเสียงไซเรนหน้าฟาร์มด้วย AI บนบอร์ด แล้วแจ้งแอปของกลุ่มทาง MQTT
#
# ตัวอย่างแนวทางอนาคต ไม่อยู่ในโจทย์โปรเจกต์รุ่นนี้
# ผล AI ใน BENTO Emulator เป็นผลจำลอง ต้องทดสอบบนบอร์ดจริง
#
# ภารกิจ   : รถฉุกเฉินมาหน้าฟาร์ม (ไซเรน) = เตือนคนในฟาร์มให้ไปเปิดประตู
#            โมเดลฟังไมโครโฟน ป้าย sirens ชนะ CONF_MIN % อย่างน้อย 3 ใน 5 ผลล่าสุด = เตือน (แบบ sf3_04)
# ลองเล่น  : เปิดคลิปเสียงไซเรนจากมือถือห่างบอร์ดราว 1 เมตร แล้วลองเสียงพูด เพลง ตบมือ ดูว่าเตือนมั่วไหม
# โมเดล    : SirenDetection (Deploy จาก Edge AI Store การ์ดชื่อ "Siren") ถ้าไม่มีจึงใช้ Siren Detection ในตัว
#            ไม่พบทั้งสองตัว = จอบอกให้ Deploy ก่อน
# เสียงบอร์ดเข้าไมค์: หลังบอร์ดส่งเสียงเอง ไม่เชื่อผล MUTE_MS (แบบ sf3_04)
# สัญญา MQTT: bento-aiot/<TEAM>/ai ตามข้อ 3.10 (มีคีย์ alarm) และ event ai_alarm_start / ai_alarm_end
# ต้องแก้ก่อนรัน: WIFI_SSID, WIFI_PASS และ TEAM (ยังไม่แก้ = ทำงานออฟไลน์ พิมพ์ผลลง Console แทน)

import edge_ai
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
TEAM = "teamXX"                        # เลขกลุ่มที่ผู้สอนแจก เช่น team05 (ต้องตรงกับแอป)
BROKER = "broker.hivemq.com"
CLIENT_ID = "bento-ai-" + TEAM
BASE = "bento-aiot/" + TEAM + "/"       # หัวข้อ: BASE + "ai" และ BASE + "event"
MODEL_KEYS = ("SirenDetection", "Siren Detection")   # ชื่อบนบอร์ด: Store ก่อน แล้วตัวในตัว
DANGER = ("sirens",)
CONF_MIN = 60
CONFIRM_N, WINDOW_N = 3, 5
MUTE_MS = 800
STALL_MS = 8000
HEARTBEAT_MS, GAP_MS = 2000, 200
RETRY_MS = 10000
TICK_MS = 100
RUN_MS = 300000
VOLUME, SPEAKER = 25, 40

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


# ---- 3) สมอง ----
def alert_rule(hist, alerting, danger):
    hist = (hist + [int(danger)])[-WINDOW_N:]
    hits = sum(hist)
    return hist, hits >= CONFIRM_N or (alerting and hits > 0)


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
    label("ฟังเสียงไซเรนหน้าฟาร์ม (Edge AI)", 12, 6, COL_TEXT, 24)
    w = {"model": label("หาโมเดล...", 12, 44, COL_INFO, 20)}
    w["arc"] = ui.Arc(x=12, y=84, w=180, h=180, min=0, max=100, value=0)
    w["conf"] = label("0 %", 70, 162, COL_TEXT, 24)
    label("AI ได้ยิน", 220, 90)
    w["label"] = label("-", 220, 116, COL_TEXT, 28)
    w["led"] = ui.Led(x=220, y=170, w=36, h=36, color=COL_BAD, value=0)
    w["state"] = label("ปกติ", 268, 178, COL_OK, 20)
    w["hist"] = label(" ", 220, 230)
    w["mqtt"] = ui.Led(x=12, y=300, w=24, h=24, color=COL_OK, value=0)
    w["sent"] = label("ยังไม่ได้ส่ง", 44, 302)
    w["note"] = label(" ", 12, 352, COL_WARN, 20)
    ui.poll()
    return w


def show(w, lab, conf, hist, alarm, online, n, n_ok):
    w["label"].text(lab)
    w["conf"].text("%d %%" % conf)
    w["arc"].value(conf)
    w["arc"].color(COL_BAD if lab in DANGER else COL_OK)
    w["led"].value(1 if alarm else 0)
    w["state"].text("ไซเรน! ไปเปิดประตูฟาร์ม" if alarm else "ปกติ")
    w["state"].color(COL_BAD if alarm else COL_OK)
    w["hist"].text("ไซเรน %d ใน %d ผลล่าสุด" % (sum(hist), len(hist)))
    w["mqtt"].value(1 if online else 0)
    w["sent"].text("ส่งได้ %d จาก %d ข้อความ" % (n_ok, n) + ("" if online else " (ออฟไลน์)"))


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
        w["model"].text("ไม่พบโมเดล SirenDetection")
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
    seq, lab, conf, hist, alarm, stalled = None, "-", 0, [], False, False
    n = n_ok = 0
    sent_key = None
    t0 = t_sent = t_retry = last_new = quiet_at = time.ticks_ms()
    try:
        while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
            now = time.ticks_ms()
            r = read_result()
            if r and r["seq"] != seq:
                seq, last_new = r["seq"], now
                if stalled:
                    stalled = False
                    w["note"].text(" ")
                if time.ticks_diff(now, quiet_at) >= 0:        # ไม่ใช่ช่วงที่บอร์ดเพิ่งส่งเสียงเอง
                    lab, conf = r["label"] or "-", int(r["conf"] * 100)
                    was = alarm
                    hist, alarm = alert_rule(hist, alarm, lab in DANGER and conf >= CONF_MIN)
                    if alarm != was:
                        set_led(alarm_led, alarm)
                        if alarm:
                            mx(rgbmatrix.scroll, "SIREN OPEN GATE", rgbmatrix.RED, 80)
                        else:
                            mx(rgbmatrix.scroll, "")           # หยุดตัววิ่ง (แบบ sf2_03)
                            mx(rgbmatrix.clear)
                        beep(84, 76) if alarm else beep(79, 84)
                        quiet_at = time.ticks_add(time.ticks_ms(), MUTE_MS)
                        ev = {"id": TEAM, "event": "ai_alarm_start" if alarm else "ai_alarm_end",
                              "model": name, "label": lab, "conf": conf}
                        send(online, "event", ev)
                    show(w, lab, conf, hist, alarm, online, n, n_ok)
            elif not stalled and time.ticks_diff(now, last_new) > STALL_MS:
                stalled = True
                w["note"].text("ผลไม่ขยับ ลองรีเซ็ตบอร์ด")
            if should_send((lab, alarm) != sent_key, time.ticks_diff(time.ticks_ms(), t_sent)):
                n += 1
                ok = send(online, "ai", {"id": TEAM, "n": n, "model": name, "label": lab,
                                         "conf": conf, "alarm": int(alarm)})
                n_ok += 1 if ok else 0
                t_sent, sent_key = time.ticks_ms(), (lab, alarm)
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
# 1) เพิ่มกฎคู่: mic.level() ดังต่อเนื่อง 2 วินาที แบบภารกิจ siren ใน sf3_08 แล้วเตือนเมื่อทั้งสองเห็นตรงกัน
# 2) ลองโมเดลเสียงอื่นที่คอร์สใช้: ("Environment Sounds", "Alarm") ป้าย chainsaw crackling_fire
#    หรือ ("HomeSounds", "Cough") ป้าย cough (แก้ DANGER ให้ตรงป้าย)
# 3) เก็บเสียงจริงหน้าฟาร์ม (รถ เครื่องจักร สัตว์) มาทดสอบว่าเตือนมั่วกี่ครั้งต่อวัน
