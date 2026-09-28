# sf3_02_coop_ears.py - หูฟังเล้าไก่: ฝูงสงบหรือตื่นตกใจ
#
# ภารกิจ   : ใช้ไมค์บนบอร์ดฟังเล้าไก่ นับ "เสียงดังฉับพลัน" (ไก่ตื่น หมาเห่า ประตูกระแทก)
#            ถ้าใน 30 วินาทีมีเสียงดังถึง ALARM_EVENTS ครั้ง = ฝูงตื่นตกใจ -> RGB matrix วิ่ง PANIC
# ลองเล่น  : ตบมือห่าง ๆ หนึ่งครั้ง แล้วลองตบถี่ ๆ ห้าครั้ง ดูว่าเมื่อไรจอเปลี่ยนเป็นสีแดง
#            หมุน VR3 = ปรับเกณฑ์ "เสียงดัง" สด ๆ ไม่ต้องแก้โค้ด · แถวล่างของ matrix = ความดัง
# แนวคิด AIoT: เสียงหนึ่งวินาทีมีตัวเลขหมื่นหกพันตัว บอร์ดยุบเหลือ "ความดัง" ตัวเดียว
#            แล้วเราตัดสินจาก "จำนวนเหตุการณ์ในช่วงเวลา" ไม่ใช่จากเสียงครั้งเดียว
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.1 ขึ้นไป) และ BENTO Emulator
#            (ใน Emulator เสียงเป็นสัญญาณสังเคราะห์ ตบมือใส่คอมพิวเตอร์แล้วไม่มีผล)

import mic
import pots
import rgbmatrix
import time
import ui

SENS = 3             # ความไวไมค์ 1-5
PEAK_MIN, PEAK_SPAN = 3000, 27000   # VR3 ตั้งเกณฑ์ยอดเสียงได้ 3000-30000 (เต็มสเกล 32768)
EVENT_GAP_MS = 400   # ห้ามนับซ้ำภายในเวลานี้ กันเสียงก้องถูกนับสองครั้ง
MUTE_MS = 800        # หลังลำโพงบอร์ดส่งเสียง ไม่ฟังช่วงนี้ ไม่งั้นไมค์ได้ยินลำโพงตัวเองแล้วนับเป็นเหตุการณ์
WINDOW_MS = 30000    # นับเหตุการณ์ย้อนหลัง 30 วินาที
ALARM_EVENTS = 5     # ถึงเท่านี้ใน 30 วินาที = ฝูงตื่นตกใจ
QUIET_LV = 20        # ความดังเฉลี่ยต่ำกว่านี้ = เล้าเงียบ
RUN_MS = 90000
TICK_MS = 80         # ต้องอ่านถี่ ไม่งั้นเสียงค้างคิวแล้วจอช้ากว่าความจริง

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


def meter_bytes(level):
    """ความดัง 0-100 -> มิเตอร์จุด 8x8 ไต่จากแถวล่างขึ้นบน (หนึ่งแถว = หนึ่งไบต์)"""
    lit = min(8, level * 8 // 100)
    out = bytearray(8)
    for row in range(8):
        if row >= 8 - lit:
            out[row] = 0xFF
    return out


def matrix(cmd, *args):
    """ส่งคำสั่งไป RGB matrix ถ้าบัสจอไม่ว่างก็ข้ามรอบนี้ไป ไม่ให้โปรแกรมตาย"""
    try:
        cmd(*args)
    except OSError:
        pass


ui.screen()
time.sleep_ms(200)
ui.Label("หูฟังเล้าไก่", x=20, y=12, color=COL_TEXT, value=24)
mood = ui.Label("กำลังเปิดไมค์...", x=20, y=48, color=COL_WARN, value=20)

ui.Panel(x=20, y=84, w=300, h=210, color=COL_CARD, min=COL_DIM, max=12, value=1)
ui.Label("ความดังตอนนี้", x=36, y=92, color=COL_DIM, value=16)
seg_lv = ui.Seg7(text="0", x=36, y=116, w=150, h=60, color=COL_OK)
bar = ui.Bar(x=36, y=190, w=268, h=24, min=0, max=100, value=0)
bar.color(COL_OK)
lbl_avg = ui.Label("เฉลี่ย 0", x=36, y=224, color=COL_DIM, value=16)
lbl_th = ui.Label("เกณฑ์เสียงดัง - (VR3)", x=36, y=250, color=COL_INFO, value=16)

dots = ui.DotMatrix(x=340, y=90, w=150, h=150, cols=8, rows=8)
lbl_lag = ui.Label("คิวค้าง 0 ms", x=352, y=252, color=COL_DIM, value=14)

ui.Panel(x=510, y=84, w=270, h=210, color=COL_CARD, min=COL_DIM, max=12, value=1)
ui.Label("เสียงดังใน 30 วิ", x=526, y=92, color=COL_DIM, value=16)
seg_win = ui.Seg7(text="0", x=526, y=116, w=120, h=60, color=COL_WARN)
ui.Label("/ " + str(ALARM_EVENTS), x=656, y=136, color=COL_DIM, value=20)
lbl_total = ui.Label("รวมทั้งหมด 0 ครั้ง", x=526, y=200, color=COL_TEXT, value=18)
lbl_alarm = ui.Label("เตือนแล้ว 0 รอบ", x=526, y=236, color=COL_DIM, value=16)

ch = ui.Chart(x=20, y=306, w=660, h=82, min=0, max=100)      # ขวาล่างเว้นให้ปุ่ม Console
ui.poll()

matrix(rgbmatrix.clear)
mic.start(sens=SENS)          # start() ทิ้งเสียงสองชุดแรกให้เอง เพราะยังไม่นิ่ง

events = []                   # เวลา (ms) ของเสียงดังแต่ละครั้ง
total = alarms = n = 0
avg = 0.0
panic = False
last_ev = mute_until = shown = -1
t0 = time.ticks_ms()
while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
    t_work = time.ticks_ms()
    lv = mic.level()                    # อ่านเสียงหนึ่งชุด -> 0-100 (สเกลหู)
    rms_v, peak_v, dc_v = mic.stats()   # อ่านอีกชุด -> ค่าดิบ ใช้ peak จับเสียงสั้น ๆ
    avg = avg * 0.9 + lv * 0.1
    peak_th = PEAK_MIN + pots.read(2) * PEAK_SPAN // 4095

    now = time.ticks_ms()
    listening = time.ticks_diff(now, mute_until) >= 0     # ยังอยู่ช่วงปิดหูหลังลำโพงดังไหม
    if listening and peak_v >= peak_th and time.ticks_diff(now, last_ev) >= EVENT_GAP_MS:
        last_ev = now
        events.append(now)
        total += 1
        lbl_total.text("รวมทั้งหมด " + str(total) + " ครั้ง")
    while events and time.ticks_diff(now, events[0]) > WINDOW_MS:
        events.pop(0)                   # ลืมเหตุการณ์ที่เก่ากว่า 30 วินาที

    was = panic
    panic = len(events) >= ALARM_EVENTS
    if panic != was:
        ui.sfx(ui.SFX_UI_DENY if panic else ui.SFX_PONG_WIN)
        mute_until = time.ticks_add(time.ticks_ms(), MUTE_MS)   # ลำโพงเพิ่งดัง ไมค์ต้องหูหนวกชั่วคราว
        matrix(rgbmatrix.scroll, "PANIC" if panic else "", rgbmatrix.RED, 60)
        shown = -1                                      # ให้แถบความดังวาดใหม่หลังจบ PANIC
        if panic:
            alarms += 1
            lbl_alarm.text("เตือนแล้ว " + str(alarms) + " รอบ")
            print("ฝูงตื่นตกใจ! เสียงดัง", len(events), "ครั้งใน 30 วินาที")

    if panic:
        mood.color(COL_BAD)
        mood.text("ฝูงตื่นตกใจ! ไปดูเล้าด่วน")
    elif avg < QUIET_LV:
        mood.color(COL_DIM)
        mood.text("เล้าเงียบ ไก่พักผ่อน")
    else:
        mood.color(COL_OK)
        mood.text("เสียงปกติของเล้า")

    seg_lv.text(str(lv))
    bar.value(lv)
    bar.color(COL_BAD if lv >= 85 else (COL_WARN if lv >= 45 else COL_OK))
    dots.set_pixels(meter_bytes(lv))
    seg_win.text(str(len(events)))
    seg_win.color(COL_BAD if panic else COL_WARN)
    n += 1
    if n % 4 == 0:                      # ของช้า ๆ อัปเดตทุก 4 รอบ (~0.3 วินาที) ก็พอ
        lbl_avg.text("เฉลี่ย %d" % int(avg))
        lbl_th.text("เกณฑ์เสียงดัง %d (VR3)" % peak_th)
        lbl_lag.text("คิวค้าง %d ms" % mic.lag())
        ch.set_next(0, int(avg))
        step = lv * 8 // 100            # matrix วาดเฉพาะตอนระดับเปลี่ยน (ช่วงละ 2 ดวง)
        if not panic and step != shown:
            shown = step
            matrix(rgbmatrix.bar, step, 8, rgbmatrix.YELLOW if step >= 4 else rgbmatrix.GREEN)
    ui.poll()

    left = TICK_MS - time.ticks_diff(time.ticks_ms(), t_work)
    if left > 0:
        time.sleep_ms(left)

mic.stop()
matrix(rgbmatrix.scroll, "")
matrix(rgbmatrix.clear)
print("ฟังเล้าครบเวลา เสียงดัง", total, "ครั้ง เตือน", alarms, "รอบ")

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) ห้องเรียนมีเสียงคุยตลอด หมุน VR3 หาเกณฑ์ที่ "ไม่นับเสียงคุย แต่นับเสียงตบมือ"
#    จดเลขเกณฑ์นั้นลงใบงาน แล้วตั้ง MUTE_MS = 0 ดูว่าเสียงเตือนของบอร์ดนับตัวเองไหม
# 2) เพิ่มกฎ "กลางคืนต้องเงียบ": ถ้า avg เกิน 40 ติดกันนาน 10 วินาที ให้เตือนอีกแบบ
