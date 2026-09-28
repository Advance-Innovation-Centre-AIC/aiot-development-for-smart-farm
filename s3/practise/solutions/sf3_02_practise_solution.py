# sf3_02_practise_solution.py - เฉลย: แบบฝึกเติมโค้ด (Code Quest ระดับ 3): is_event()
#
# วิธีเล่น  : ไฟล์นี้เหมือน sf3_02_coop_ears.py ทุกอย่าง ยกเว้นฟังก์ชัน is_event() ในส่วน "3) สมอง"
#            ที่เว้นช่อง ____ (ขีดล่างสี่ตัว) ไว้ 3 ช่อง: A, B, C
#            เติมให้ครบแล้วรัน โปรแกรมจะตรวจ is_event() 7 กรณีก่อนเปิดจอ (self_test)
#            ผ่านครบ = Console ขึ้น "ผ่าน!" แล้วเล่นต่อได้เหมือนไฟล์ตัวอย่าง
#            ยังไม่ถูก = Console บอกว่ากรณีไหนผิด แล้วหยุด (ยังไม่เปิดจอ)
# ไฟล์นี้  : เฉลยที่เติมครบแล้ว (ช่อง A B C มีป้าย "เฉลย") ใช้เทียบกับของตัวเอง
# ติดขัด?  : ใช้บันไดช่วยเหลือในใบงาน (คำใบ้ 3 ขั้นอยู่ท้ายใบงาน) หรือรันไฟล์ตัวอย่างเต็ม
#            sf3_02_coop_ears.py เพื่อไปต่อก่อน แล้วค่อยกลับมาเทียบกับของตัวเอง
# เฉลย     : โจทย์หลัก มีเฉลยในคาบ อยู่ที่ practise/solutions/sf3_02_practise_solution.py (ลองเองก่อน)
#
# (ทำจาก sf3_02_coop_ears.py 8c7ad7735690)

import mic
import pots
import rgbmatrix
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
SENS = 3             # ความไวไมค์ 1-5
PEAK_MIN, PEAK_SPAN = 3000, 27000   # VR3 ตั้งเกณฑ์ยอดเสียงได้ 3000-30000
PEAK_FULL = 32768    # ยอดเสียงดิบเต็มสเกล (ไมค์ 16 บิต) ใช้เป็นสเกลของแถบและกราฟ
EVENT_GAP_MS = 400   # ห้ามนับซ้ำภายในเวลานี้ กันเสียงก้องถูกนับสองครั้ง
MUTE_MS = 800        # หลังลำโพงบอร์ดส่งเสียง ไม่ฟังช่วงนี้ ไม่งั้นไมค์ได้ยินลำโพงตัวเองแล้วนับเป็นเหตุการณ์
WINDOW_MS = 30000    # นับเหตุการณ์ย้อนหลัง 30 วินาที
ALARM_EVENTS = 5     # ถึงเท่านี้ใน 30 วินาที = ฝูงตื่นตกใจ
QUIET_LV = 20        # ความดังเฉลี่ยต่ำกว่านี้ = เล้าเงียบ
RUN_MS = 90000
TICK_MS = 500        # อัปเดตจอทุกครึ่งวินาที
MIC_MS = 20          # ระหว่างรอ อ่านไมค์ทุก 20 ms เสียงตบมือสั้น ๆ จะได้ไม่หลุดช่วง
MATRIX_MS = 3000     # ส่งภาพจอไฟ RGB ซ้ำทุก 3 วินาที เผื่อภาพก่อนหน้าหล่นหายตอนบอร์ดยุ่ง
SCROLL_MS = 80       # PANIC วิ่งก้าวละ 80 ms: 37 ก้าว ~ 3 วินาที = หนึ่งรอบพอดีกับการส่งซ้ำ ภาพจึงไม่สะดุด

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def read_threshold():
    return PEAK_MIN + pots.read(2) * PEAK_SPAN // 4095    # VR3 -> เกณฑ์ยอดเสียงดิบ


def mx(cmd, *args):
    # ส่งคำสั่งไปจอไฟ RGB ถ้าบัสไม่ว่างก็ข้ามรอบนี้ไป (อีกไม่เกิน 3 วินาทีก็ส่งซ้ำ) ไม่ให้โปรแกรมตาย
    try:
        cmd(*args)
    except OSError:
        pass


def meter_frame(step):
    # ภาพเต็มจอไฟ RGB 16x8 (จุดละ 4 บิต): แถวล่างติด step x 2 ดวง ที่เหลือดับ
    # ส่งทั้งภาพทุกครั้ง ตัวอักษร PANIC ที่เพิ่งวิ่งจึงไม่ค้างอยู่แถวบน
    buf = bytearray(64)
    c = rgbmatrix.YELLOW if step >= 4 else rgbmatrix.GREEN
    for x in range(step * 2):
        buf[56 + x // 2] |= c << (4 * (x % 2))
    return buf


def draw_matrix(frame):
    # frame = -1 คือ PANIC, 0-8 คือระดับความดัง
    if frame < 0:
        mx(rgbmatrix.scroll, "PANIC", rgbmatrix.RED, SCROLL_MS)
    else:
        mx(rgbmatrix.blit, meter_frame(frame))


class Ears:
    # หูของเล้า: ระหว่างรอจอรอบถัดไป อ่านไมค์ถี่ ๆ แล้วจำเวลาของเสียงดังแต่ละครั้ง

    def __init__(self):
        now = time.ticks_ms()
        self.events = []      # เวลา (ms) ของเสียงดังใน 30 วินาทีล่าสุด
        self.total = 0
        self.last = time.ticks_add(now, -EVENT_GAP_MS)   # ให้เสียงแรกนับได้ทันที
        self.mute_until = now
        self.top = 0          # ยอดเสียงดิบสูงสุดในรอบจอนี้

    def listen(self, ms, th):
        # ฟังนาน ms: stats() คืน (rms, peak, dc) peak = ค่าดิบสูงสุด 0-32768 จับเสียงสั้น ๆ ได้ดี
        self.top = 0
        t0 = time.ticks_ms()
        while time.ticks_diff(time.ticks_ms(), t0) < ms:
            peak = mic.stats()[1]
            now = time.ticks_ms()
            self.top = max(self.top, peak)
            if is_event(peak, th, now, self.last, self.mute_until):
                self.last = now
                self.events.append(now)
                self.total += 1
            time.sleep_ms(MIC_MS)

    def mute(self):
        # ลำโพงเพิ่งดัง ไมค์ต้องหูหนวกชั่วคราว
        self.mute_until = time.ticks_add(time.ticks_ms(), MUTE_MS)

    def listening(self):
        return time.ticks_diff(time.ticks_ms(), self.mute_until) >= 0


# ---- 3) สมอง (ตัดสินใจ) ----
def is_event(peak, th, now, last, mute_until):
    # นับเป็น "เสียงดังฉับพลัน" เมื่อ: พ้นช่วงปิดหู, ยอดถึงเกณฑ์, และห่างครั้งก่อนพอ
    return (time.ticks_diff(now, mute_until) >= 0        # ช่อง A (เฉลย): พ้นช่วงปิดหูแล้ว ผลต่างเวลาต้องไม่ติดลบ
            and peak >= th                              # ช่อง B (เฉลย): ยอดเสียงต้องถึง "อะไร"?
            and time.ticks_diff(now, last) >= EVENT_GAP_MS)     # ช่อง C (เฉลย): ห่างครั้งก่อนอย่างน้อยกี่ ms? (ส่วน 1)


def self_test():
    # ตรวจ is_event() 7 กรณีก่อนเปิดไมค์ (เกณฑ์ยอดเสียง 5000)
    now, g = 100000, EVENT_GAP_MS
    for peak, last, mute, want in ((6000, now - 1000, now - 1, True), (4000, now - 1000, now - 1, False),
                                   (5000, now - 1000, now - 1, True), (6000, now - g + 1, now - 1, False),
                                   (6000, now - g, now - 1, True), (6000, now - 1000, now + 100, False),
                                   (6000, now - 1000, now, True)):
        got = is_event(peak, 5000, now, last, mute)
        if got != want:
            print("ยังไม่ถูก: ยอด", peak, "ห่าง", now - last, "ms ปิดหูถึง", mute - now, "ควรได้", want, "แต่ได้", got)
            return False
    print("ผ่าน! is_event() ถูกทั้ง 7 กรณี")
    return True


def forget_old(events, now):
    # ลืมเหตุการณ์ที่เก่ากว่า 30 วินาที เหลือเฉพาะที่อยู่ในหน้าต่างเวลา
    while events and time.ticks_diff(now, events[0]) > WINDOW_MS:
        events.pop(0)


def mood(panic, avg):
    if panic:
        return "ฝูงตื่นตกใจ! ไปดูเล้าด่วน", COL_BAD
    if avg < QUIET_LV:
        return "เล้าเงียบ ไก่พักผ่อน", COL_DIM
    return "เสียงปกติของเล้า", COL_OK


def level_color(lv):
    return COL_BAD if lv >= 85 else (COL_WARN if lv >= 45 else COL_OK)


# ---- 4) หน้าจอ ----
def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า (ทุกไฟล์ใช้แบบเดียวกัน)
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def line_chart(x, y, w, h, lo, hi, color, parent=None):
    # กราฟเส้นเรียบ ไม่มีจุดกลม: LVGL ไม่วาดจุดเมื่อจำนวนจุด >= ความกว้างกราฟ
    # เราจึงให้กว้างไม่เกิน 400 และตั้ง 400 จุด (เฟิร์มแวร์รับได้ 10-400)
    ch = ui.Chart(x=x, y=y, w=min(w, 400), h=h, color=color, min=lo, max=hi, parent=parent)
    ch.prop(ui.PROP_CHART_POINTS, 400)
    return ch


def meter_bytes(level):
    # ความดัง 0-100 -> จุด 8x8 บนจอ ไต่จากแถวล่างขึ้นบน (หนึ่งแถว = หนึ่งไบต์)
    lit = min(8, level * 8 // 100)
    return b"\x00" * (8 - lit) + b"\xff" * lit


def build_level_card(w):
    card(12, 44, 250, 150, "ความดัง (สเกลหู 0-100)")
    w["seg_lv"] = ui.Seg7(text="0", x=24, y=72, w=120, h=56, color=COL_OK)
    w["bar_lv"] = ui.Bar(x=24, y=138, w=226, h=18, min=0, max=100, value=0)
    w["avg"] = ui.Label("เฉลี่ย 0", x=24, y=164, color=COL_DIM, value=16)


def build_peak_card(w):
    card(272, 44, 300, 150, "ยอดเสียงดิบ เทียบเกณฑ์")
    w["seg_pk"] = ui.Seg7(text="0", x=284, y=72, w=150, h=56, color=COL_INFO)
    w["th"] = ui.Label("เกณฑ์ -", x=444, y=76, color=COL_TEXT, value=16)
    ui.Label("(VR3)", x=444, y=100, color=COL_DIM, value=14)
    w["bar_pk"] = ui.Bar(x=284, y=138, w=276, h=18, min=0, max=PEAK_FULL, value=0)
    ui.Label("ถึงเกณฑ์ = นับหนึ่งครั้ง", x=284, y=164, color=COL_DIM, value=14)


def build_count_card(w):
    card(582, 44, 198, 150, "เสียงดังใน 30 วิ")
    w["seg_win"] = ui.Seg7(text="0", x=594, y=72, w=90, h=56, color=COL_WARN)
    ui.Label("/ " + str(ALARM_EVENTS), x=694, y=90, color=COL_DIM, value=20)
    w["total"] = ui.Label("รวม 0 ครั้ง", x=594, y=136, color=COL_TEXT, value=16)
    w["alarms"] = ui.Label("เตือน 0 รอบ", x=594, y=162, color=COL_DIM, value=16)


def build_bottom(w):
    # กราฟ: ฟ้า (ชุด 0) = ยอดเสียงดิบ, แดง = เกณฑ์จาก VR3 สเกลเดียวกัน 0-32768
    ui.Label("ฟ้า = ยอดเสียงดิบ   แดง = เกณฑ์ (VR3)", x=12, y=204, color=COL_DIM, value=14)
    w["chart"] = line_chart(12, 226, 400, 112, 0, PEAK_FULL, COL_INFO)
    w["s_th"] = w["chart"].add_series(COL_BAD)
    card(422, 204, 358, 134, "ความดังเป็นจุด + ไมค์")
    w["dots"] = ui.DotMatrix(x=434, y=230, w=100, h=100, cols=8, rows=8)
    w["led"] = ui.Led(x=548, y=236, w=24, h=24, color=COL_OK, value=0)
    w["ear"] = ui.Label("กำลังเปิดไมค์", x=580, y=238, color=COL_DIM, value=16)
    w["lag"] = ui.Label("คิวค้าง 0 ms", x=548, y=276, color=COL_DIM, value=14)


def build_screen():
    # สร้างทุกอย่างบนจอครั้งเดียว แล้วคืน dict ของ widget ที่ต้องอัปเดตภายหลัง
    ui.screen()
    time.sleep_ms(200)
    ui.Label("หูฟังเล้าไก่", x=12, y=6, color=COL_TEXT, value=24)
    w = {"mood": ui.Label("กำลังเปิดไมค์...", x=220, y=10, color=COL_WARN, value=20)}
    build_level_card(w)
    build_peak_card(w)
    build_count_card(w)
    build_bottom(w)
    w["help"] = ui.Label("VR3 = เกณฑ์   ตบมือ 5-6 ครั้ง ห่างกันราว 0.5 วินาที",
                         x=12, y=352, color=COL_DIM, value=16)
    ui.poll()
    return w


def show_level(w, lv, avg, panic):
    w["seg_lv"].text(str(lv))
    w["bar_lv"].value(lv)
    w["bar_lv"].color(level_color(lv))
    w["avg"].text("เฉลี่ย %d" % avg)
    w["dots"].set_pixels(meter_bytes(lv))
    text, col = mood(panic, avg)
    w["mood"].text(text)
    w["mood"].color(col)


def show_peak(w, top, th):
    col = COL_BAD if top >= th else COL_INFO   # แดง = ยอดถึงเกณฑ์ในครึ่งวินาทีนี้
    w["seg_pk"].text(str(top))
    w["seg_pk"].color(col)
    w["bar_pk"].value(top)
    w["bar_pk"].color(col)
    w["th"].text("เกณฑ์ %d" % th)
    w["chart"].set_next(0, top)
    w["chart"].set_next(w["s_th"], th)


def show_count(w, ears, alarms, panic, lag):
    w["seg_win"].text(str(len(ears.events)))
    w["seg_win"].color(COL_BAD if panic else COL_WARN)
    w["total"].text("รวม %d ครั้ง" % ears.total)
    w["alarms"].text("เตือน %d รอบ" % alarms)
    on = ears.listening()
    w["led"].value(1 if on else 0)
    w["ear"].text("กำลังฟัง" if on else "ปิดหูชั่วคราว")
    w["lag"].text("คิวค้าง %d ms" % lag)


# ---- 5) โปรแกรมหลัก ----
def finish(w, total, alarms):
    w["help"].color(COL_WARN)
    w["help"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    ui.poll()
    print("ฟังเล้าครบเวลา เสียงดัง", total, "ครั้ง เตือน", alarms, "รอบ")


def main():
    if not self_test():
        return
    w = build_screen()
    mx(rgbmatrix.clear)
    mic.start(sens=SENS)          # start() ทิ้งเสียงสองชุดแรกให้เอง เพราะยังไม่นิ่ง
    ears = Ears()
    avg, alarms, panic = 0.0, 0, False
    drawn, sent = None, time.ticks_ms()   # ภาพบนจอไฟ RGB และเวลาที่ส่งล่าสุด
    t0 = time.ticks_ms()
    try:
        while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
            th = read_threshold()
            ears.listen(TICK_MS, th)                 # 1) ฟังครึ่งวินาที (อ่านไมค์ถี่ ๆ)
            lv = mic.level()                         # ความดังตอนนี้ 0-100 (สเกลหู)
            avg = avg * 0.7 + lv * 0.3
            now = time.ticks_ms()
            forget_old(ears.events, now)             # 2) ตัดสิน
            was, panic = panic, len(ears.events) >= ALARM_EVENTS
            if panic != was:                         # 3) ทำ: เสียงเฉพาะตอนสถานะเปลี่ยน
                ui.sfx(ui.SFX_UI_DENY if panic else ui.SFX_PONG_WIN)
                ears.mute()                          # ทุกครั้งที่ลำโพงดัง = ปิดหู MUTE_MS
                if panic:
                    alarms += 1
                    print("ฝูงตื่นตกใจ! เสียงดัง", len(ears.events), "ครั้งใน 30 วินาที")
            frame = -1 if panic else lv * 8 // 100
            if frame != drawn or time.ticks_diff(now, sent) >= MATRIX_MS:
                draw_matrix(frame)                   # วาดตอนเปลี่ยน และส่งซ้ำทุก 3 วินาที
                drawn, sent = frame, now
            show_level(w, lv, int(avg), panic)       # 4) โชว์
            show_peak(w, ears.top, th)
            show_count(w, ears, alarms, panic, mic.lag())
            ui.poll()
    finally:
        mic.stop()
        mx(rgbmatrix.clear)
    finish(w, ears.total, alarms)


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) ห้องมีเสียงคุยตลอด หมุน VR3 หาเกณฑ์ที่ "ไม่นับเสียงคุย แต่นับเสียงตบมือ"
#    (ในกราฟ: เส้นแดงต้องอยู่เหนือยอดเสียงคุย แต่ต่ำกว่ายอดเสียงตบมือ) จดเลขเกณฑ์นั้นไว้
# 2) ตั้ง MUTE_MS = 0 แล้วตบมือจนถึง PANIC ดูว่าเสียงเตือนของบอร์ดถูกนับเป็นเหตุการณ์เองไหม
# 3) เพิ่มกฎ "กลางคืนต้องเงียบ": ถ้า avg เกิน 40 ติดกันนาน 10 วินาที ให้เตือนอีกแบบ
#    (เขียนเป็นฟังก์ชันในส่วน 3) แล้วเรียกจาก main)
