# sf3_04_ai_pump_doctor.py - ให้ AI บนบอร์ดตัดสินแทนกฎ (Edge AI)
#
# ภารกิจ   : ใช้โมเดล AI บนบอร์ด (จาก Local Edge AI Store หรือที่ติดมากับบอร์ด) อ่านการเคลื่อนไหวจาก IMU
#            แล้วตอบว่าเครื่อง "นิ่ง / หมุนเป็นจังหวะ / สั่นผิดปกติ" พร้อมคะแนนทุกคลาส
#            คลาสอันตรายชนะด้วยความมั่นใจสูงติดกันหลายครั้ง -> เตือน (เสียง + ไฟแดง)
# ลองเล่น  : วางบอร์ดนิ่ง -> ถือบอร์ดวาดวงกลมในอากาศช้า ๆ -> เขย่าแรง ๆ
#            ดูวงแหวนความมั่นใจ แถบคะแนนแต่ละคลาส เวลาที่ AI ใช้คิด กราฟ และจอไฟ RGB
#            (ใน Emulator เป็นผลจำลอง: ปุ่ม Shake = shaking, เอียงบอร์ด = circle)
# ของบนบอร์ดที่ใช้ : แกน AI บนชิป (edge_ai) + IMU, ไฟ RGB_RED บนบอร์ด = ไฟเตือน,
#            ลำโพง (ดังตอนเริ่มเตือนและตอนหายเตือนเท่านั้น)
#            จอไฟ RGB: แท่งคะแนนของแต่ละคลาส แท่งที่ชนะเป็นสีเขียว (แดงตอนเตือน)
# บนจอ     : วงแหวนความมั่นใจ (Arc), ไฟเตือน (Led), แถบคะแนนทุกคลาส (Bar),
#            กราฟความมั่นใจเทียบเกณฑ์ (Chart)
# แนวคิด AIoT: Edge AI = โมเดลรันบนชิปในบอร์ดเอง ไม่ต้องส่งข้อมูลดิบขึ้นคลาวด์
#            เร็ว ประหยัดเน็ต และข้อมูลฟาร์มไม่ออกนอกฟาร์ม - เทียบกับ sf3_03 ที่ใช้กฎเขียนเอง
#            โมเดลตัวอย่างไม่ได้ฝึกจากปั๊มในฟาร์มจริง เราใช้เป็นตัวแทนเพื่อเรียนแนวคิดเท่านั้น
# โมเดล    : หาด้วย "ชื่อ" ตามลำดับใน MODEL_KEYS: AnomalousVibration (ส่งลงบอร์ดจาก Local Edge AI Store)
#            ก่อน ถ้าบอร์ดไม่มีหรือเลือกไม่สำเร็จจึงใช้ Motion ที่ติดมากับบอร์ด · จอบอกว่าใช้ตัวไหนอยู่
#            ถ้าผลไม่ขยับนาน จอจะบอกให้ลองรีเซ็ตบอร์ด
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.2 ขึ้นไป · 2.4.1 ก็รันได้) และ BENTO Emulator

import edge_ai
import gpio
import rgbmatrix
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
VOLUME = 25          # ความดังเสียง 0-127 (≈20%)
SPEAKER = 40             # ความดังลำโพงรวม 0-100% (ใช้ได้กับ firmware 2.4.2 ขึ้นไป)
MODEL_KEYS = ("AnomalousVibration", "Motion")   # ลองตามลำดับ: โมเดลจาก Store ก่อน ไม่มีหรือเลือกไม่ได้ค่อยใช้โมเดลในตัว
                         # ลองเปลี่ยนเป็น ("Cough",) ("Alarm",) ("Siren",) (ฟังเสียง) หรือ ("Push",)
CONF_MIN = 60            # มั่นใจไม่ถึงกี่ % ไม่นับเป็นเหตุการณ์
CONFIRM_N = 3            # คลาสอันตรายต้องชนะติดกันกี่ครั้งถึงจะเตือน
STALL_MS = 8000          # ไม่มีคำตอบใหม่นานเท่านี้ = บอกบนจอ
MUTE_MS = 800            # โมเดลที่ฟังไมค์: ไม่เชื่อผลช่วงนี้หลังบอร์ดส่งเสียงเอง
MATRIX_MS = 3000         # ส่งภาพจอไฟ RGB ซ้ำทุกกี่ ms (กันภาพหล่นหาย)
MAX_CLASSES = 4          # โมเดลในตัวมีไม่เกิน 3 คลาส เผื่อไว้ 4 แถว
SENSOR_MIC = 2           # ค่าช่อง "sensor" ของโมเดลที่ฟังไมโครโฟน
RUN_MS = 120000
TICK_MS = 500

# แปลชื่อคลาสของโมเดลเป็นภาษาฟาร์ม (ชื่อคลาสต้องตรงกับที่บอร์ดรายงาน)
MEANING = {
    "idle": "เครื่องหยุด / นิ่ง", "circle": "หมุนเป็นจังหวะ (ปกติ)",
    "shaking": "สั่นผิดปกติ!", "unlabelled": "ไม่มีเหตุการณ์",
    "anomaly": "สั่นผิดปกติ!", "unlabeled": "ปกติ",
    "cough": "ได้ยินเสียงไอ", "alarm": "เสียงสัญญาณเตือน",
    "sirens": "เสียงไซเรน", "Push": "มีคนผลักเข้ามา",
}
DANGER = ("shaking", "anomaly", "cough", "alarm", "sirens", "Push")

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def beep(*notes):
    # เสียงเบา ๆ แทน ui.sfx (ui.sfx ดังคงที่ ปรับเบาไม่ได้) · เล่นโน้ต MIDI ทีละตัว ห่างกัน 120 ms
    for n in notes:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 120)
        time.sleep_ms(120)


def find_models(keys):
    # หาโมเดลจาก "ชื่อ" ตามลำดับใน keys เพราะลำดับบนแต่ละบอร์ดไม่เหมือนกัน (เปลี่ยนได้หลังรีบูต)
    # คืนรายการ (ลำดับ, ชื่อ, ชื่อคลาส) ทุกตัวที่ชื่อตรง ตัวแรกเลือกไม่สำเร็จ main() จะลองตัวถัดไป
    try:
        ms = edge_ai.models()
    except Exception:
        return []               # แกน AI ไม่ตอบ = ถือว่าไม่พบ
    return [(m["index"], m["name"], m["labels"]) for key in keys for m in ms if key.lower() in m["name"].lower()]


def uses_mic(index):
    # โมเดลนี้ฟังไมโครโฟนไหม: อ่านช่อง "sensor" ช่องเดียว แล้วเก็บแค่ True/False
    try:
        return edge_ai.model(index)["sensor"] == SENSOR_MIC
    except Exception:
        return False


def read_result():
    # ขอคำตอบล่าสุด ถ้าลิงก์ไปแกน AI ตอบไม่ทัน (OSError) รอบนี้ข้ามไป ไม่ให้โปรแกรมหยุด
    try:
        return edge_ai.result()
    except OSError:
        return None


def stop_ai():
    try:
        edge_ai.stop()
    except OSError:
        pass


def led_named(name):
    # หา LED ด้วยชื่อ ไม่ใช่เลข: บน Dev Kit ดวง LED1/LED2 อยู่บน SoM มองไม่เห็น
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


def put(buf, x, y, c):
    # ตั้งสีจุด (x, y) ในเฟรม 64 ไบต์ของจอไฟ RGB (จุดละ 4 บิต)
    i = y * 8 + (x >> 1)
    if x & 1:
        buf[i] = (buf[i] & 0x0F) | (c << 4)
    else:
        buf[i] = (buf[i] & 0xF0) | c


def draw_scores(frame):
    # วาดแท่งคะแนนของแต่ละคลาสบนจอไฟ RGB 16x8 (ภาพนิ่ง ส่งซ้ำได้ไม่กระตุก)
    heights, top, alerting = frame
    buf = bytearray(64)
    cw = 16 // max(1, len(heights))
    for i, h in enumerate(heights):
        c = rgbmatrix.BLUE
        if i == top:
            c = rgbmatrix.RED if alerting else rgbmatrix.GREEN
        for x in range(i * cw, i * cw + cw - 1):   # เว้น 1 คอลัมน์ระหว่างแท่ง
            for y in range(8 - h, 8):
                put(buf, x, y, c)
    try:
        rgbmatrix.blit(buf)
    except OSError:
        pass                    # จอไฟ RGB ตอบไม่ทัน: ข้ามภาพนี้ไป


def matrix_update(frame, drawn, sent_at):
    # ส่งภาพเมื่อภาพเปลี่ยน และส่งซ้ำทุก MATRIX_MS เพราะบางภาพอาจหล่นหายตอนบอร์ดยุ่ง
    now = time.ticks_ms()
    if frame and (frame != drawn or time.ticks_diff(now, sent_at) >= MATRIX_MS):
        draw_scores(frame)
        return frame, now
    return drawn, sent_at


# ---- 3) สมอง (ตัดสินใจ) ----
def is_danger(label, conf):
    return label in DANGER and conf >= CONF_MIN


def alert_rule(streak, alerting, danger):
    # เตือนเมื่ออันตรายชนะติดกัน CONFIRM_N ครั้ง (กันเตือนมั่วจากคำตอบเดียว)
    # หายเตือนเมื่อคำตอบกลับมาปลอดภัย
    streak = streak + 1 if danger else 0
    if streak >= CONFIRM_N:
        return streak, True
    return streak, alerting and streak > 0


def score_frame(scores, top, alerting):
    # ภาพของจอไฟ RGB: ความสูงแท่ง 0-8 แถวต่อคลาส + คลาสที่ชนะ + กำลังเตือนไหม
    return tuple(min(8, round(s * 8)) for s in scores[:MAX_CLASSES]), top, alerting


# ---- 4) หน้าจอ ----
def card(x, y, w, h, title):
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def line_chart(x, y, w, h, lo, hi, color):
    # กราฟเส้นเรียบ ไม่มีจุดกลม: กว้างไม่เกิน 400 และตั้ง 400 จุด
    ch = ui.Chart(x=x, y=y, w=min(w, 400), h=h, color=color, min=lo, max=hi)
    ch.prop(ui.PROP_CHART_POINTS, 400)
    return ch


def build_answer(w):
    card(12, 64, 380, 172, "AI ตอบว่า")
    w["arc"] = ui.Arc(x=24, y=92, w=132, h=132, min=0, max=100, value=0)
    w["arc"].color(COL_DIM)
    w["verdict"] = ui.Label("-", x=170, y=94, color=COL_TEXT, value=28)
    w["led"] = ui.Led(x=340, y=96, w=36, h=36, color=COL_BAD, value=0)
    w["meaning"] = ui.Label("-", x=170, y=140, color=COL_INFO, value=16)
    w["conf"] = ui.Label("มั่นใจ - %", x=170, y=176, color=COL_DIM, value=16)


def build_scores(w):
    card(402, 64, 378, 172, "คะแนนทุกคลาส (%)")
    w["names"], w["bars"] = [], []
    for i in range(MAX_CLASSES):
        w["names"].append(ui.Label(" ", x=414, y=96 + i * 34, color=COL_TEXT, value=16))
        bar = ui.Bar(x=512, y=98 + i * 34, w=256, h=18, min=0, max=100, value=0)
        bar.color(COL_DIM)
        w["bars"].append(bar)


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("AI หมอเครื่องจักร (Edge AI)", x=12, y=6, color=COL_TEXT, value=24)
    w = {"model": ui.Label("โมเดล: -", x=12, y=38, color=COL_DIM, value=16)}
    build_answer(w)
    build_scores(w)
    ui.Label("ฟ้า = มั่นใจ %   แดง = เกณฑ์", x=12, y=242, color=COL_DIM, value=14)
    w["chart"] = line_chart(12, 262, 400, 76, 0, 100, COL_INFO)
    w["s_min"] = w["chart"].add_series(COL_BAD)
    card(422, 244, 358, 94, "สมองของ AI")
    w["lat"] = ui.Label("ใช้เวลาคิด - ms", x=434, y=274, color=COL_DIM, value=16)
    w["streak"] = ui.Label("อันตรายติดกัน 0", x=434, y=304, color=COL_DIM, value=16)
    w["status"] = ui.Label("กำลังหาโมเดล...", x=12, y=352, color=COL_WARN, value=16)
    ui.poll()
    return w


def say(w, text, color):
    w["status"].color(color)
    w["status"].text(text)


def show_model(w, name, labels, mic):
    w["model"].text("โมเดล: " + name + ("  (ฟังไมค์)" if mic else ""))
    for i, lb in enumerate(w["names"]):
        lb.text(labels[i] if i < len(labels) else " ")


def show_result(w, r, conf, alerting, streak, alerts):
    lab = r["label"] or "-"
    w["verdict"].text(lab)
    w["meaning"].text(MEANING.get(lab, lab))
    w["meaning"].color(COL_BAD if alerting else COL_INFO)
    w["arc"].value(conf)
    w["arc"].color(COL_OK if conf >= CONF_MIN else COL_WARN)
    w["conf"].text("มั่นใจ %d %%" % conf)
    w["led"].value(1 if alerting else 0)
    hot = COL_BAD if alerting else COL_OK
    for i, bar in enumerate(w["bars"]):
        if i < len(r["scores"]):
            bar.value(int(r["scores"][i] * 100))
            bar.color(hot if i == r["top"] else COL_DIM)
    w["lat"].text("ใช้เวลาคิด %.1f ms" % r["latency_ms"])
    w["streak"].text("อันตรายติดกัน %d/%d  เตือนแล้ว %d ครั้ง" % (min(streak, CONFIRM_N), CONFIRM_N, alerts))
    w["chart"].set_next(0, conf)
    w["chart"].set_next(w["s_min"], CONF_MIN)


def announce(alerting, mic):
    # เสียงเฉพาะตอนเริ่ม/หายเตือน แล้วคืนเวลาที่จะกลับมาเชื่อผล:
    # ถ้าโมเดลฟังไมค์ เสียงจากลำโพงบอร์ดเองจะเข้าไมค์ จึงไม่เชื่อผล MUTE_MS หลังเสียง
    beep(84, 76) if alerting else beep(79, 84)
    return time.ticks_add(time.ticks_ms(), MUTE_MS if mic else 0)


# ---- 5) โปรแกรมหลัก ----
def watch(w, mic, alarm):
    # วนอ่านคำตอบของ AI จนครบ RUN_MS แล้วคืนจำนวนครั้งที่เตือน
    last_seq, streak, alerts, alerting, stalled = None, 0, 0, False, False
    frame = drawn = None
    t0 = last_new = quiet_at = sent_at = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        r = read_result()                                         # 1) อ่าน
        if r and r["seq"] != last_seq:                            # คำตอบใหม่จริง ๆ เท่านั้น
            last_seq, last_new = r["seq"], now
            if stalled:
                stalled = False
                say(w, "AI กำลังคิด...", COL_OK)
            if time.ticks_diff(now, quiet_at) >= 0:               # ไม่ใช่ช่วงที่บอร์ดเพิ่งส่งเสียง
                conf = int(r["conf"] * 100)
                was = alerting                                    # 2) ตัดสิน
                streak, alerting = alert_rule(streak, alerting, is_danger(r["label"], conf))
                if alerting != was:                               # 3) ทำ
                    quiet_at = announce(alerting, mic)
                    set_led(alarm, alerting)
                    if alerting:
                        alerts += 1
                        print("AI เตือน:", r["label"], "มั่นใจ", conf, "%")
                show_result(w, r, conf, alerting, streak, alerts)  # 4) โชว์
                frame = score_frame(r["scores"], r["top"], alerting)
        elif not stalled and time.ticks_diff(now, last_new) > STALL_MS:
            stalled = True
            say(w, "ผลไม่ขยับ ลองรีเซ็ตบอร์ด", COL_BAD)
        drawn, sent_at = matrix_update(frame, drawn, sent_at)
        ui.poll()
        time.sleep_ms(TICK_MS)
    return alerts


def start(w, index):
    try:
        edge_ai.select(index)
        say(w, "AI กำลังคิด...", COL_OK)
        return True
    except OSError as e:
        say(w, "โหลดโมเดลไม่สำเร็จ", COL_BAD)
        print("select:", e)
        return False


def main():
    if hasattr(ui, "volume"):    # บอร์ดที่ยังเป็น 2.4.1 ข้ามบรรทัดนี้
        ui.volume(SPEAKER)
    w = build_screen()
    found = find_models(MODEL_KEYS)
    if not found:
        msg = "ไม่พบโมเดล " + " / ".join(MODEL_KEYS) + " บนบอร์ดนี้"
        say(w, msg, COL_BAD)
        ui.poll()
        print(msg, "- ใช้ sf3_03 แทน")      # sf3_03 = กฎที่เขียนเอง ไม่ต้องใช้โมเดล
        return
    alarm = led_named("RGB_RED")
    alerts = None
    try:
        for index, name, labels in found:      # ตัวนี้เลือกไม่สำเร็จ ลองตัวถัดไป (เช่น Motion ในตัว)
            mic = uses_mic(index)
            show_model(w, name, labels, mic)
            if start(w, index):
                alerts = watch(w, mic, alarm)
                break
    finally:                                   # หยุดกลางทางก็ปล่อยแกน AI และดับไฟเสมอ
        stop_ai()
        set_led(alarm, False)
        try:
            rgbmatrix.clear()
        except OSError:
            pass
    if alerts is not None:
        say(w, "ครบเวลา - กด Program to Device เพื่อเล่นใหม่", COL_WARN)
        print("AI เฝ้าเครื่องครบเวลา เตือน", alerts, "ครั้ง")
    ui.poll()


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) รัน sf3_03 (กฎ) กับไฟล์นี้ (AI) แล้วทำท่าเดียวกัน 5 ท่า จดลงใบงานว่าใครตอบถูกกี่ท่า
# 2) เปลี่ยน MODEL_KEYS เป็น ("Cough",) แล้วลองไอใส่บอร์ด (แนวคิด: ฟาร์มสุกรใช้เสียงไอจับโรคทางเดินหายใจ
#    แต่โมเดลนี้ฝึกจากเสียงไอคน - งานจริงต้องฝึกโมเดลใหม่จากเสียงในฟาร์ม)
# 3) ลด CONFIRM_N เหลือ 1 แล้วเขย่าเบา ๆ นับว่าเตือนมั่วกี่ครั้ง เทียบกับ CONFIRM_N = 3
