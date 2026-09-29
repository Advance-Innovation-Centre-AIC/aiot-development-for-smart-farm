# sf3_02_practise_solution.py - เฉลย: Code Quest ระดับ 3: เติมช่อง A B C ใน is_event()
# รัน: ตรวจ is_event() 5 กรณีก่อนเปิดจอ ผ่าน = Console ขึ้น "ผ่าน!" · ไฟล์นี้คือเฉลย (ช่อง A B C มีป้าย "เฉลย")
# เฉลย: practise/solutions/sf3_02_practise_solution.py (ลองเองก่อน)
# (ทำจาก sf3_02_coop_ears.py 42fedcc028fc)

import gc
import mic
import pots
import rgbmatrix
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
VOLUME = 25
SPEAKER = 40
SENS = 3             # ความไวไมค์ 1-5
PEAK_MIN, PEAK_SPAN = 3000, 27000   # เกณฑ์ VR3 3000-30000
PEAK_FULL = 32768
EVENT_GAP_MS = 400   # ms ห้ามนับซ้ำ
MUTE_MS = 800        # ms ปิดหูหลังลำโพงดัง
WINDOW_MS = 30000    # ms ช่วงนับย้อนหลัง
ALARM_EVENTS = 5     # ครั้ง = ตื่นตกใจ
QUIET_LV = 20
RUN_MS = 90000
TICK_MS = 500
MIC_MS = 20
MATRIX_MS = 3000
SCROLL_MS = 80

COL_TEXT, COL_DIM, COL_CARD = 0xE8EAED, 0x9AA3AF, 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def beep(*notes):
    for n in notes:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 120)
        time.sleep_ms(120)


def read_threshold():
    return PEAK_MIN + pots.read(2) * PEAK_SPAN // 4095


def mx(cmd, *args):
    try:
        cmd(*args)
    except OSError:
        pass


def draw_matrix(frame):
    # -1 = PANIC, 0-8 = ระดับความดัง (แถวล่าง)
    if frame < 0:
        mx(rgbmatrix.scroll, "PANIC", rgbmatrix.RED, SCROLL_MS)
        return
    buf = bytearray(64)
    c = rgbmatrix.YELLOW if frame >= 4 else rgbmatrix.GREEN
    for x in range(frame * 2):
        buf[56 + x // 2] |= c << (4 * (x % 2))
    mx(rgbmatrix.blit, buf)


class Ears:
    def __init__(self):
        now = time.ticks_ms()
        self.events = []
        self.total = 0
        self.last = time.ticks_add(now, -EVENT_GAP_MS)
        self.mute_until = now
        self.top = 0

    def listen(self, ms, th):
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
    # 5 กรณี (เกณฑ์ 5000) · กรณีแรกอยู่ตรงขอบทุกอย่าง
    t, g = 100000, EVENT_GAP_MS
    for p, last, mute, ok in ((5000, t - g, t, 1), (4999, t - g, t - 1, 0), (6000, t - g + 1, t - 1, 0),
                              (6000, t - 1000, t + 1, 0), (6000, t - 1000, t - 1, 1)):
        if is_event(p, 5000, t, last, mute) != ok:
            print("ยังไม่ถูก: ยอด", p, "ห่าง", t - last, "ปิดหูอีก", mute - t, "ควรได้", bool(ok))
            return False
    print("ผ่าน! is_event() ถูกทั้ง 5 กรณี")
    return True


def forget_old(events, now):
    # ลืมเหตุการณ์ที่เก่ากว่า 30 วินาที เหลือเฉพาะที่อยู่ในหน้าต่างเวลา
    while events and time.ticks_diff(now, events[0]) > WINDOW_MS:
        events.pop(0)


def mood(panic, avg):
    if panic:
        return "PANIC! Check the coop", COL_BAD
    if avg < QUIET_LV:
        return "Quiet - hens resting", COL_DIM
    return "Normal coop sound", COL_OK


# ---- 4) หน้าจอ ----
def card(x, y, w, h, title):
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("Coop Ears", x=12, y=6, color=COL_TEXT, value=24)
    w = {"mood": ui.Label("Starting mic...", x=200, y=10, color=COL_WARN, value=20)}
    card(12, 44, 250, 150, "Loudness 0-100")
    w["lv"] = ui.Seg7(text="0", x=24, y=72, w=120, h=56, color=COL_OK)
    w["bar"] = ui.Bar(x=24, y=138, w=226, h=18, min=0, max=100, value=0)
    w["avg"] = ui.Label("avg 0", x=24, y=164, color=COL_DIM, value=16)
    card(272, 44, 300, 150, "Raw peak vs threshold")
    w["pk"] = ui.Seg7(text="0", x=284, y=72, w=150, h=56, color=COL_INFO)
    w["th"] = ui.Label("th -", x=444, y=76, color=COL_TEXT, value=16)
    ui.Label("(VR3)", x=444, y=100, color=COL_DIM, value=16)
    ui.Label("red = counted", x=284, y=164, color=COL_DIM, value=16)
    card(582, 44, 198, 150, "Loud in 30 s")
    w["win"] = ui.Seg7(text="0", x=594, y=72, w=90, h=56, color=COL_WARN)
    ui.Label("/ %d" % ALARM_EVENTS, x=694, y=90, color=COL_DIM, value=20)
    w["total"] = ui.Label("total 0", x=594, y=136, color=COL_TEXT, value=16)
    w["alarms"] = ui.Label("alarms 0", x=594, y=162, color=COL_DIM, value=16)
    w["led"] = ui.Led(x=12, y=216, w=24, h=24, color=COL_OK, value=0)
    w["ear"] = ui.Label("mic starting", x=44, y=218, color=COL_DIM, value=16)
    w["help"] = ui.Label("VR3 = threshold   clap 5-6 times, ~0.5 s apart",
                         x=12, y=352, color=COL_DIM, value=16)
    ui.poll()
    return w


def show(w, lv, avg, top, th, ears, alarms, panic):
    text, col = mood(panic, avg)
    w["mood"].text(text)
    w["mood"].color(col)
    w["lv"].text(str(lv))
    w["bar"].value(lv)
    w["avg"].text("avg %d" % avg)
    w["pk"].text(str(top))
    w["pk"].color(COL_BAD if top >= th else COL_INFO)
    w["th"].text("th %d" % th)
    w["win"].text(str(len(ears.events)))
    w["win"].color(COL_BAD if panic else COL_WARN)
    w["total"].text("total %d" % ears.total)
    w["alarms"].text("alarms %d" % alarms)
    on = ears.listening()
    w["led"].value(1 if on else 0)
    w["ear"].text("listening" if on else "muted (own beep)")


# ---- 5) โปรแกรมหลัก ----
def main():
    if hasattr(ui, "volume"):
        ui.volume(SPEAKER)
    if not self_test():
        return
    gc.collect()
    mic.start(sens=SENS)          # เปิดไมค์ก่อนสร้างจอ
    w = build_screen()
    mx(rgbmatrix.clear)
    ears = Ears()
    avg, alarms, panic = 0.0, 0, False
    drawn, sent = None, time.ticks_ms()
    t0 = time.ticks_ms()
    try:
        while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
            th = read_threshold()
            ears.listen(TICK_MS, th)                 # 1) ฟัง
            lv = mic.level()
            avg = avg * 0.7 + lv * 0.3
            now = time.ticks_ms()
            forget_old(ears.events, now)             # 2) ตัดสิน
            was, panic = panic, len(ears.events) >= ALARM_EVENTS
            if panic != was:                         # 3) ทำ: เสียงเฉพาะตอนสถานะเปลี่ยน
                beep(84, 76) if panic else beep(79, 84)
                ears.mute()                          # ทุกครั้งที่ลำโพงดัง = ปิดหู MUTE_MS
                if panic:
                    alarms += 1
                    print("ฝูงตื่นตกใจ! เสียงดัง", len(ears.events), "ครั้งใน 30 วินาที")
            frame = -1 if panic else lv * 8 // 100
            if frame != drawn or time.ticks_diff(now, sent) >= MATRIX_MS:
                draw_matrix(frame)
                drawn, sent = frame, now
            show(w, lv, int(avg), ears.top, th, ears, alarms, panic)   # 4) โชว์
            ui.poll()
    finally:
        mic.stop()
        mx(rgbmatrix.clear)
    w["help"].text("Done - press Program to Device to run again")
    ui.poll()
    print("ครบเวลา: เสียงดัง", ears.total, "ครั้ง เตือน", alarms, "รอบ")


main()
