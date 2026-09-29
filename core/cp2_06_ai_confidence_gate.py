# cp2_06_ai_confidence_gate.py - หลักการ 2.6: ให้ AI มีสิทธิ์ตอบว่า "ไม่แน่ใจ" (ประตูความมั่นใจ)
#
# หลักการ  : โมเดล AI ไม่ได้ตอบแค่ชื่อคลาส มันให้ "คะแนน" ทุกคลาส (0-1) คลาสที่คะแนนสูงสุด = คำตอบ
#            คะแนนของคำตอบนั้นคือ "ความมั่นใจ" (conf) ตัวเลขที่เราใช้ตัดสินว่าจะเชื่อคำตอบไหม
#            ประตู: ลงมือเมื่อได้คำตอบเดียวกัน N_IN_ROW ครั้งติดกัน และทุกครั้งมั่นใจ >= เกณฑ์
#            ไม่ผ่านประตู = "ไม่แน่ใจ" = ยังไม่ทำอะไร ซึ่งเป็นคำตอบที่ถูกเมื่อข้อมูลกำกวม
#            นับเฉพาะคำตอบ "ใหม่" (เลข seq เปลี่ยน) เพราะ result() คืนคำตอบล่าสุดซ้ำทุกครั้งที่ถาม
# ลองเล่น  : วางบอร์ดนิ่ง -> ถือบอร์ดวาดวงกลมในอากาศ -> เขย่า ดู "AI ตอบดิบ" (บน) เทียบ "ตัดสิน" (ล่าง)
#            หมุน VR1 = เกณฑ์ความมั่นใจ 0.50-0.99 สด ๆ แล้วดูตัวนับ "ลงมือ" กับ "ไม่แน่ใจ"
#            เกณฑ์สูง = ไม่แน่ใจบ่อยขึ้น แต่ลงมือผิดน้อยลง · เกณฑ์ต่ำ = ตอบไว แต่เชื่อคำตอบกำกวมด้วย
# ของบนบอร์ด: แกน AI บนชิป (edge_ai) + IMU · ลูกบิด VR1 · ลำโพง (ดังเฉพาะตอนผลตัดสินเปลี่ยน)
# ในฟาร์ม  : AI ฟังเสียงไอในโรงเรือนสุกร ถ้าสั่งพ่นยาทุกครั้งที่ AI ตอบว่า "ไอ" จะเปลืองและอันตราย
#            ประตูความมั่นใจ + ต้องตอบซ้ำหลายครั้ง = สั่งงานเฉพาะตอนแน่ใจ ที่เหลือแจ้งคนให้มาดู
# โมเดล    : ใช้เฉพาะโมเดลที่ติดมากับบอร์ด หาด้วย "ชื่อ" (MODEL_KEY) เพราะลำดับบนแต่ละบอร์ดไม่เหมือนกัน
#            แล้วเริ่มด้วยเลข index จากแถวของโมเดลนั้น (start/select รับเลข ไม่รับชื่อ)
#            โมเดล Motion ฝึกจากท่ามือคน (นิ่ง / วงกลม / เขย่า) ใช้เป็นตัวแทนเพื่อเรียนแนวคิดเท่านั้น
#            ถ้าเปลี่ยนเป็นโมเดลฟังไมค์ (Cough, Alarm, Siren) เสียงบี๊บของบอร์ดเองอาจเข้าไมค์ด้วย
# บอร์ด    : TESAIoT Dev Kit และ BENTO Emulator
#            ใน Emulator คำตอบของ AI เป็นผลจำลองจากเซนเซอร์จำลอง (ปุ่ม Shake = เขย่า) ไม่ได้รันโมเดลจริง

import edge_ai
import pots
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
MODEL_KEY = "Motion"     # ชื่อ (บางส่วน) ของโมเดลในตัว ลอง "Cough" "Alarm" "Siren"
N_IN_ROW = 3             # ต้องได้คำตอบเดียวกันที่มั่นใจพอ ติดกันกี่ครั้ง จึงลงมือ
CONF_MIN = None          # None = ใช้ VR1 ตั้งเกณฑ์สด ๆ · ใส่เลข เช่น 0.95 = ตรึงเกณฑ์ (VR1 ไม่มีผล)
CONF_LO, CONF_HI = 0.50, 0.99   # ช่วงเกณฑ์ที่ VR1 หมุนได้
MAX_CLASSES = 4          # แสดงคะแนนได้กี่คลาส (โมเดลในตัวมี 2-3 คลาส)
ASK_MS = 100             # ถามคำตอบจาก AI ทุกกี่ ms (ถี่พอจะไม่พลาดคำตอบใหม่)
TICK_MS = 500            # อัปเดตจอทุกกี่ ms
RUN_MS = 180000          # เล่นนาน 3 นาทีแล้วจบเอง
VOLUME = 25              # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def find_model(key):
    # หาโมเดลในตัวจาก "ชื่อ" คืน (index, ชื่อ, ชื่อคลาส) หรือ None
    # ใช้ m["index"] ของแถวนั้น ไม่ใช่ลำดับในรายการ (models() ข้ามแถวที่อ่านไม่สำเร็จ)
    try:
        for m in edge_ai.models():
            if m.get("builtin") is not False and key.lower() in m["name"].lower():
                return m["index"], m["name"], m["labels"]
    except Exception:
        pass                    # แกน AI ไม่ตอบ = ถือว่าไม่พบ
    return None


def conf_min():
    # เกณฑ์ความมั่นใจตอนนี้: ค่าคงที่ หรือจาก VR1 (0-4095 -> CONF_LO..CONF_HI)
    if CONF_MIN:
        return CONF_MIN
    return CONF_LO + pots.read(0) * (CONF_HI - CONF_LO) / 4095


# เสียงเตือนใช้ ui.tone เพราะปรับความดังได้ (ui.sfx ในเฟิร์มแวร์นี้ปรับความดังไม่ได้)
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ----
def gate(cand, n, label, conf, cmin):
    # ป้อนคำตอบใหม่หนึ่งครั้ง คืน (ผู้สมัคร, นับติดกัน, ผลตัดสิน)
    # ผลตัดสิน = ชื่อคลาส เมื่อคลาสเดียวกันมั่นใจ >= cmin ติดกันครบ N_IN_ROW ครั้ง ไม่งั้น None (ไม่แน่ใจ)
    if label is None or conf < cmin:
        cand, n = None, 0                  # ไม่มั่นใจพอ = เริ่มนับใหม่
    elif label == cand:
        n = min(n + 1, N_IN_ROW)           # คำตอบเดิม มั่นใจอีกครั้ง (นับถึง N_IN_ROW พอ)
    else:
        cand, n = label, 1                 # เปลี่ยนคำตอบ = เริ่มนับคลาสใหม่
    return cand, n, (cand if n >= N_IN_ROW else None)


def pct(x):
    # 0-1 -> 0-100 เป็นจำนวนเต็ม (widget บนจอรับเฉพาะ int)
    return int(round(x * 100))


# ---- 4) เครือข่าย (ไฟล์นี้ไม่ใช้) ----


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def line_chart(x, y, w, h, lo, hi, color, parent=None):
    # กราฟเส้นเรียบ ไม่มีจุดกลม: LVGL ไม่วาดจุดเมื่อจำนวนจุด >= ความกว้างกราฟ
    # เราจึงให้กว้างไม่เกิน 400 และตั้ง 400 จุด (เฟิร์มแวร์รับได้ 10-400)
    ch = ui.Chart(x=x, y=y, w=min(w, 400), h=h, color=color, min=lo, max=hi, parent=parent)
    ch.prop(ui.PROP_CHART_POINTS, 400)
    return ch


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("AI แน่ใจก่อนทำ", x=12, y=6, color=COL_TEXT, value=24)
    w = {"model": ui.Label("โหลดโมเดล...", x=12, y=38, color=COL_DIM, value=16)}
    card(12, 64, 380, 150, "AI ตอบดิบ")
    w["raw"] = ui.Label("-", x=24, y=92, color=COL_TEXT, value=28)
    w["bar"] = ui.Bar(x=24, y=138, w=356, h=20, min=0, max=100, value=0)
    w["conf"] = ui.Label(" ", x=24, y=172, color=COL_DIM, value=16)
    card(402, 64, 378, 150, "คะแนน")
    w["names"], w["bars"] = [], []
    for i in range(MAX_CLASSES):
        w["names"].append(ui.Label(" ", x=414, y=92 + i * 28, color=COL_TEXT, value=16))
        w["bars"].append(ui.Bar(x=530, y=94 + i * 28, w=238, h=16, min=0, max=100, value=0))
    card(12, 224, 380, 114, "ประตูตัดสิน")
    w["led"] = ui.Led(x=24, y=254, w=40, h=40, color=COL_OK, value=0)
    w["dec"] = ui.Label("ไม่แน่ใจ", x=78, y=258, color=COL_WARN, value=28)
    w["cnt"] = ui.Label(" ", x=24, y=306, color=COL_TEXT, value=16)
    ui.Label("ฟ้า มั่นใจ แดง เกณฑ์", x=422, y=224, color=COL_DIM, value=14)
    w["chart"] = line_chart(422, 246, 358, 92, 0, 100, COL_INFO)
    w["s_min"] = w["chart"].add_series(COL_BAD)
    w["help"] = ui.Label("นิ่ง วงกลม เขย่า  VR1 = เกณฑ์", x=12, y=352, color=COL_DIM, value=16)
    ui.poll()
    return w


def show(w, r, cmin, dec, n, acted, unsure):
    # บน: คำตอบดิบล่าสุด + คะแนนทุกคลาส · ล่าง: ผลตัดสินของประตู + ตัวนับ
    c, m = pct(r["conf"]), pct(cmin)
    w["raw"].text(r["label"] or "-")
    w["bar"].value(c)
    w["conf"].text("มั่นใจ %d %%  เกณฑ์ %d %%" % (c, m))
    for i, s in enumerate(r["scores"][:MAX_CLASSES]):
        w["bars"][i].value(pct(s))
    w["chart"].set_next(0, c)
    w["chart"].set_next(w["s_min"], m)
    w["led"].value(1 if dec else 0)
    w["dec"].text(dec or "ไม่แน่ใจ")
    w["dec"].color(COL_OK if dec else COL_WARN)
    w["cnt"].text("ติด %d/%d  ลงมือ %d  ไม่แน่ใจ %d" % (n, N_IN_ROW, acted, unsure))


# ---- 6) โปรแกรมหลัก ----
def watch(w):
    # วนถามคำตอบจนครบ RUN_MS ประมวลผลเฉพาะคำตอบใหม่ อัปเดตจอทุก TICK_MS
    r = last_seq = cand = dec = None
    n = acted = unsure = 0
    cmin = conf_min()
    t0 = t_show = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        try:
            got = edge_ai.result()                            # 1) วัด: คำตอบล่าสุด (None = ยังไม่มี)
        except OSError:
            got = None                                        # ลิงก์ไปแกน AI ตอบไม่ทัน: ข้ามรอบนี้
        if got and got["seq"] != last_seq:                    # คำตอบใหม่จริง ๆ เท่านั้น
            r, last_seq = got, got["seq"]
            was = dec
            cand, n, dec = gate(cand, n, r["label"], r["conf"], cmin)   # 2) ตัดสิน
            if dec:
                acted += 1
            else:
                unsure += 1
            if dec and dec != was:                            # 3) ทำ: เสียงเฉพาะตอนผลเปลี่ยนเป็นลงมือ
                beep("start")
        now = time.ticks_ms()
        if time.ticks_diff(now, t_show) >= TICK_MS:           # 4) โชว์ ทุกครึ่งวินาที
            t_show = now
            cmin = conf_min()
            if r:
                show(w, r, cmin, dec, n, acted, unsure)
            ui.poll()
        time.sleep_ms(ASK_MS)


def main():
    w = build_screen()
    found = find_model(MODEL_KEY)
    if found is None:
        w["model"].color(COL_BAD)
        w["model"].text("ไม่พบโมเดล " + MODEL_KEY)       # ไม่มีโมเดลในตัวชื่อนี้ = จบอย่างสงบ ไม่ crash
        ui.poll()
        return
    index, name, labels = found
    for i in range(min(len(labels), MAX_CLASSES)):
        w["names"][i].text(labels[i])
    ui.poll()
    try:
        edge_ai.start(index)                  # start(เลข) = select(เลข): เลือกแล้วเริ่ม รอโหลดได้ถึง 15 วิ
        w["model"].text("โมเดล: " + name)
        watch(w)
        beep("good")                         # done
        w["help"].color(COL_WARN)
        w["help"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    except OSError:                           # เลือก/เริ่มไม่สำเร็จ (เช่นแกน AI ยังไม่พร้อม)
        w["model"].color(COL_BAD)
        w["model"].text("เริ่มโมเดลไม่ได้ กด RESET")
    finally:
        try:
            edge_ai.stop()                    # ปล่อยแกน AI เสมอ แม้หยุดกลางทาง
        except OSError:
            pass
    ui.poll()


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เดาก่อนรัน: หมุน VR1 ไปสุด (0.99) แล้วเขย่า ตัวนับ "ไม่แน่ใจ" จะวิ่งเร็วขึ้นหรือช้าลง เพราะอะไร
# 2) ตั้ง CONF_MIN = 0.95 แล้วเล่น 1 นาที จดสัดส่วน ลงมือ : ไม่แน่ใจ เทียบกับ CONF_MIN = 0.60
# 3) ตั้ง N_IN_ROW = 1 แล้ววาดวงกลมช้า ๆ ผลตัดสินกระพริบไปมากี่ครั้ง เทียบกับ N_IN_ROW = 3
