# sf3_04_ai_pump_doctor.py - ให้ AI บนบอร์ดตัดสินแทนกฎ (Edge AI)
#
# ภารกิจ   : ใช้โมเดล AI ที่ติดมากับบอร์ด (DEEPCRAFT Ready Model) อ่านการเคลื่อนไหวจาก IMU
#            แล้วตอบว่าเครื่อง "นิ่ง / หมุนเป็นจังหวะ / สั่นผิดปกติ" พร้อมความมั่นใจทุกคลาส
# ลองเล่น  : วางบอร์ดนิ่ง -> ถือบอร์ดวาดวงกลมในอากาศช้า ๆ -> เขย่าแรง ๆ
#            ดูแถบคะแนนแต่ละคลาส เวลาที่ AI ใช้คิด และ RGB matrix ที่วิ่งคำตอบของ AI
# แนวคิด AIoT: Edge AI = โมเดลรันบนชิปในบอร์ดเอง ไม่ต้องส่งข้อมูลดิบขึ้นคลาวด์
#            เร็ว ประหยัดเน็ต และข้อมูลฟาร์มไม่ออกนอกฟาร์ม - เทียบกับ sf3_03 ที่ใช้กฎเขียนเอง
#            โมเดลนี้ฝึกจากท่ามือคน (ไม่ใช่ปั๊มจริง) เราใช้เป็นตัวแทนเพื่อเรียนแนวคิดเท่านั้น
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.1 ขึ้นไป) และ BENTO Emulator
#            (ใน Emulator เป็นผลจำลอง: ปุ่ม Shake = shaking, เอียงบอร์ด = circle)
# หมายเหตุ  : Ready Model รุ่นทดลองจำกัดจำนวนครั้งที่คิดต่อการเปิดเครื่องหนึ่งครั้ง
#            ถ้าผลหยุดขยับนาน ๆ จอจะบอก ให้กดรีเซ็ตบอร์ดแล้วรันใหม่

import edge_ai
import rgbmatrix
import time
import ui

MODEL_KEY = "Motion"     # ลองเปลี่ยนเป็น "Cough" "Alarm" "Siren" หรือ "Push" (โมเดลเสียง/เรดาร์)
CONF_MIN = 0.60          # มั่นใจไม่ถึงเท่านี้ ไม่นับเป็นเหตุการณ์
CONFIRM_N = 3            # คลาสอันตรายต้องชนะติดกันกี่ครั้งถึงจะเตือน
STALL_MS = 8000          # ผลไม่ขยับนานเท่านี้ = น่าจะหมดโควตาการคิด
RUN_MS = 120000
TICK_MS = 180

# แปลชื่อคลาสของโมเดลเป็นภาษาฟาร์ม (ชื่อคลาสต้องตรงกับที่ edge_ai.models() รายงาน)
MEANING = {
    "idle": "เครื่องหยุด / นิ่ง", "circle": "หมุนเป็นจังหวะ (ปกติ)",
    "shaking": "สั่นผิดปกติ!", "unlabelled": "ไม่มีเหตุการณ์",
    "cough": "ได้ยินเสียงไอ", "alarm": "เสียงสัญญาณเตือน",
    "sirens": "เสียงไซเรน", "Push": "มีคนผลักเข้ามา",
}
DANGER = ("shaking", "cough", "alarm", "sirens", "Push")

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


def show_matrix(text, color):
    """วิ่งคำตอบของ AI บน RGB matrix (ชื่อคลาสเป็นภาษาอังกฤษ matrix วาดไทยไม่ได้)"""
    try:
        rgbmatrix.scroll(text, color, 70)
    except OSError:
        pass


def find_model(keyword):
    """หาโมเดลจากชื่อ ห้ามจำเป็นเลขลำดับ เพราะแต่ละบอร์ดเรียงไม่เหมือนกัน"""
    try:
        ms = edge_ai.models()
    except Exception:
        return None
    for m in ms:
        if keyword.lower() in m["name"].lower():
            return m
    return None


ui.screen()
time.sleep_ms(200)
ui.Label("AI หมอเครื่องจักร (Edge AI)", x=20, y=12, color=COL_TEXT, value=24)
status = ui.Label("กำลังหาโมเดล...", x=20, y=48, color=COL_WARN, value=18)

ui.Panel(x=20, y=84, w=360, h=230, color=COL_CARD, min=COL_DIM, max=12, value=1)
ui.Label("AI ตอบว่า", x=36, y=92, color=COL_DIM, value=16)
verdict = ui.Label("-", x=36, y=120, color=COL_TEXT, value=28)
meaning = ui.Label("-", x=36, y=166, color=COL_INFO, value=22)
lbl_conf = ui.Label("มั่นใจ - %", x=36, y=210, color=COL_DIM, value=18)
lbl_lat = ui.Label("ใช้เวลาคิด - ms", x=36, y=240, color=COL_DIM, value=18)
lbl_alert = ui.Label("เตือนแล้ว 0 ครั้ง", x=36, y=276, color=COL_DIM, value=18)

ui.Panel(x=400, y=84, w=380, h=230, color=COL_CARD, min=COL_DIM, max=12, value=1)
ui.Label("คะแนนทุกคลาส (%)", x=416, y=92, color=COL_DIM, value=16)
rows = []
for i in range(4):                       # โมเดลในตัวมีไม่เกิน 3 คลาส เผื่อไว้ 4 แถว
    lb = ui.Label("", x=416, y=122 + i * 46, color=COL_TEXT, value=16)
    br = ui.Bar(x=416, y=142 + i * 46, w=340, h=14, min=0, max=100, value=0)
    br.color(COL_DIM)
    rows.append((lb, br))
ui.poll()

model = find_model(MODEL_KEY)
if model is None:
    status.color(COL_BAD)
    status.text("ไม่พบโมเดล " + MODEL_KEY + " บนบอร์ดนี้")
    ui.poll()
    print("ไม่พบโมเดล", MODEL_KEY, "- ข้ามไปทำ sf3_03 (กฎเขียนเอง) แทน")
else:
    labels = model["labels"]
    for i, (lb, br) in enumerate(rows):
        lb.text(labels[i] if i < len(labels) else "")
    try:
        edge_ai.select(model["index"])
        status.color(COL_OK)
        status.text("โมเดล: " + model["name"])
    except OSError as e:
        model = None
        status.color(COL_BAD)
        status.text("โหลดโมเดลไม่สำเร็จ")
        print("edge_ai.select ล้มเหลว:", e)
    ui.poll()

last_seq = -1
shown = None
streak = alerts = 0
alerting = stalled = False
t0 = last_new = time.ticks_ms()
while model is not None and time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
    now = time.ticks_ms()
    r = edge_ai.result()
    if r and r["seq"] != last_seq:            # มีคำตอบใหม่จริง ๆ เท่านั้นถึงวาดจอ
        last_seq, last_new = r["seq"], now
        if stalled:                           # กลับมาคิดได้แล้ว คืนป้ายสถานะ
            stalled = False
            status.color(COL_OK)
            status.text("โมเดล: " + model["name"])
        lab = r["label"] or "-"
        verdict.text(lab)
        meaning.text(MEANING.get(lab, lab))
        lbl_conf.text("มั่นใจ %d %%" % int(r["conf"] * 100))
        lbl_lat.text("ใช้เวลาคิด %.1f ms" % r["latency_ms"])
        for i, (lb, br) in enumerate(rows):
            if i < len(r["scores"]):
                br.value(int(r["scores"][i] * 100))
                br.color(COL_OK if i == r["top"] else COL_DIM)

        danger = lab in DANGER and r["conf"] >= CONF_MIN
        streak = streak + 1 if danger else 0
        if streak >= CONFIRM_N and not alerting:
            alerting = True
            alerts += 1
            lbl_alert.text("เตือนแล้ว " + str(alerts) + " ครั้ง")
            ui.sfx(ui.SFX_UI_DENY)
            print("AI เตือน:", lab, "มั่นใจ %d%%" % int(r["conf"] * 100))
        elif streak == 0 and alerting:
            alerting = False
            ui.sfx(ui.SFX_PONG_WIN)            # อันตรายผ่านไปแล้ว
        meaning.color(COL_BAD if alerting else COL_INFO)
        if (lab, alerting) != shown:           # matrix เขียนเฉพาะตอนคำตอบเปลี่ยน
            shown = (lab, alerting)
            show_matrix(lab.upper(), rgbmatrix.RED if alerting else rgbmatrix.GREEN)
    elif time.ticks_diff(now, last_new) > STALL_MS:
        stalled = True
        status.color(COL_BAD)
        status.text("ผลไม่ขยับ %d วิ - รีเซ็ตบอร์ด" % (time.ticks_diff(now, last_new) // 1000))
    ui.poll()
    time.sleep_ms(TICK_MS)

if model is not None:
    edge_ai.stop()
show_matrix("", rgbmatrix.WHITE)                # scroll("") = หยุดตัวหนังสือวิ่ง
print("AI เฝ้าเครื่องครบเวลา เตือน", alerts, "ครั้ง")

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) รัน sf3_03 (กฎ) กับไฟล์นี้ (AI) แล้วทำท่าเดียวกัน 5 ท่า จดลงใบงานว่าใครตอบถูกกี่ท่า
# 2) เปลี่ยน MODEL_KEY เป็น "Cough" แล้วลองไอใส่บอร์ด (แนวคิด: ฟาร์มสุกรใช้เสียงไอจับโรคทางเดินหายใจ
#    แต่โมเดลนี้ฝึกจากเสียงไอคน - งานจริงต้องฝึกโมเดลใหม่จากเสียงในฟาร์ม)
