# sf3_01_pen_guard.py - ยามเฝ้าคอก: เรดาร์นับผู้บุกรุก
#
# ภารกิจ   : ใช้เรดาร์บนบอร์ดเฝ้าประตูคอกสัตว์ ใครเข้ามาในเขตเตือนถือว่า "บุกรุก"
#            ไซเรนดัง RGB matrix วิ่งคำว่า INTRUDER สีแดง และตัวนับผู้บุกรุกเพิ่มขึ้น
# ลองเล่น  : ตอนเริ่มให้ถอยห่างบอร์ด (มันกำลังจำฉากนิ่ง) แล้วค่อย ๆ เดินเข้าหาบอร์ด
#            หมุน VR3 = ขยาย/หดเขตเตือน · กด SW4 = เปิด/ปิดระบบเฝ้า (ตอนเจ้าของเข้าคอกเอง)
# แนวคิด AIoT: เรดาร์เห็นได้ในที่มืดและไม่ต้องใช้กล้อง (ไม่ละเมิดความเป็นส่วนตัว)
#            ความละเอียดราว 0.33 เมตร/ช่อง จึงบอก "โซน" ได้ ไม่ใช่ไม้บรรทัด
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.1 ขึ้นไป มีเรดาร์ BGT60TR13C) และ BENTO Emulator
#            (ใน Emulator: VR1 = ระยะของเป้า 0.15-1.8 ม. · ปุ่ม Shake = มีการเคลื่อนไหว)

import buttons
import pots
import rgbmatrix
import sensors
import time
import ui

MIN_CM, SPAN_CM = 50, 200   # VR3 ตั้งเขตเตือนได้ 50-250 cm
CONFIRM_N = 3        # ต้องเห็นติดกันกี่รอบถึงจะเชื่อ กันคลื่นสะท้อนวูบเดียว
THRESH_DB = 4.0      # เป้าต้องแรงกว่าฉากนิ่งกี่ dB (ค่าที่ตัวอย่าง IDE สอบเทียบไว้)
MAX_CM = 300
RUN_MS = 180000
TICK_MS = 250

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF
COL_SAFE_BG, COL_ALERT_BG = 0x123322, 0x4A1216

hist = []


def smooth(cm):
    """median 5 ค่า กันเรดาร์กระโดดข้ามเฟรมจากคลื่นสะท้อนหลายทาง"""
    hist.append(cm)
    if len(hist) > 5:
        hist.pop(0)
    return sorted(hist)[len(hist) // 2]


def siren():
    """สลับสองโน้ตสี่ครั้ง (เลขโน้ต MIDI ไม่ใช่เฮิรตซ์) เล่นตอนเริ่มบุกรุกเท่านั้น"""
    for note in (81, 74, 81, 74):
        ui.tone(note, ui.WAVE_SQUARE, 100, 140)
        time.sleep_ms(150)


def show_matrix(armed, inside, count):
    """วาด RGB matrix เฉพาะตอนสถานะเปลี่ยน ทุกคำสั่งคือการเขียนบัสจอหนึ่งครั้ง"""
    try:
        rgbmatrix.scroll("")                     # หยุดตัวหนังสือวิ่งเดิมก่อน
        if not armed:
            rgbmatrix.clear()
        elif inside:
            rgbmatrix.scroll("INTRUDER", rgbmatrix.RED, 60)
        else:
            rgbmatrix.score(count, rgbmatrix.GREEN)
    except OSError:
        pass


ui.screen()
time.sleep_ms(200)
ui.Label("ยามเฝ้าคอก (เรดาร์)", x=20, y=12, color=COL_TEXT, value=24)
status = ui.Label("", x=20, y=48, color=COL_WARN, value=18)

gate = ui.Panel(x=20, y=84, w=440, h=220, color=COL_SAFE_BG, min=COL_DIM, max=12, value=1)
ui.Label("ประตูคอก", x=36, y=92, color=COL_DIM, value=16)
state = ui.Label("ปลอดภัย", x=36, y=120, color=COL_OK, value=32)
seg_cm = ui.Seg7(text="---", x=36, y=176, w=200, h=60, color=COL_INFO)
ui.Label("cm", x=246, y=200, color=COL_DIM, value=18)
bar = ui.Bar(x=36, y=256, w=408, h=28, min=0, max=MAX_CM, value=0)
bar.color(COL_OK)

ui.Panel(x=480, y=84, w=300, h=220, color=COL_CARD, min=COL_DIM, max=12, value=1)
ui.Label("ผู้บุกรุก (ครั้ง)", x=496, y=92, color=COL_DIM, value=16)
seg_n = ui.Seg7(text="0", x=496, y=116, w=150, h=60, color=COL_BAD)
moving = ui.Label("การเคลื่อนไหว: -", x=496, y=190, color=COL_DIM, value=16)
zone = ui.Label("เขตเตือน - cm (VR3)", x=496, y=220, color=COL_INFO, value=16)
arm_lbl = ui.Label("ระบบเฝ้า: เปิด (SW4)", x=496, y=250, color=COL_OK, value=16)
energy = ui.Label("energy: -", x=496, y=278, color=COL_DIM, value=14)

ch = ui.Chart(x=20, y=316, w=660, h=72, min=0, max=MAX_CM)   # ขวาล่างเว้นให้ปุ่ม Console
s_line = ch.add_series(COL_BAD)     # เส้นแดง = เขตเตือน, ซีรีส์ 0 = ระยะเป้า
ui.poll()

# นับถอยหลังให้คนออกจากหน้าบอร์ดก่อน แล้วสั่งเรดาร์จำ "ฉากนิ่ง" ใหม่
for i in (3, 2, 1):
    status.text("ถอยห่างบอร์ด! จำฉากนิ่งใน %d วินาที" % i)
    ui.poll()
    time.sleep_ms(1000)
sensors.radar_config(0)
time.sleep_ms(500)
sensors.radar_config(THRESH_DB)
status.color(COL_OK)
status.text("เฝ้าคอกแล้ว เดินเข้าหาบอร์ดได้เลย")
ui.sfx(ui.SFX_UI_START)

armed, inside, sw_was = True, False, False
streak = count = 0
shown = None
t0 = time.ticks_ms()
while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
    sw = buttons.pressed(0)                      # SW4: จับ "ขอบกด" เอง ไม่งั้นกดค้างจะสลับรัว ๆ
    if sw and not sw_was:
        armed = not armed
        inside, streak = False, 0
        ui.sfx(ui.SFX_UI_SELECT if armed else ui.SFX_UI_BACK)
    sw_was = sw
    alert_cm = MIN_CM + pots.read(2) * SPAN_CM // 4095

    r, rr = sensors.radar(), sensors.radar_range()
    cm = smooth(int(rr["distance_m"] * 100)) if rr["target"] else None
    near = armed and cm is not None and cm < alert_cm

    if near != inside:                           # สถานะใหม่ต้องยืนครบ CONFIRM_N รอบ
        streak += 1
        if streak >= CONFIRM_N:
            inside, streak = near, 0
            if inside:
                count += 1
                seg_n.text(str(count))
                print("ผู้บุกรุกคนที่", count, "ระยะ", cm, "cm")
                siren()
            else:
                ui.sfx(ui.SFX_PONG_WIN)          # ออกไปแล้ว = ปลอดภัย
    else:
        streak = 0

    seg_cm.text("---" if cm is None else str(cm))
    bar.value(MAX_CM if cm is None else min(cm, MAX_CM))
    bar.color(COL_BAD if near else COL_OK)
    gate.color(COL_ALERT_BG if inside else COL_SAFE_BG)
    state.color(COL_BAD if inside else (COL_OK if armed else COL_DIM))
    state.text("มีผู้บุกรุก!" if inside else ("ปลอดภัย" if armed else "ปิดระบบเฝ้า"))
    moving.text("การเคลื่อนไหว: " + ("มี" if r["presence"] else "ไม่มี"))
    moving.color(COL_WARN if r["presence"] else COL_DIM)
    zone.text("เขตเตือน %d cm (VR3)" % alert_cm)
    arm_lbl.text("ระบบเฝ้า: " + ("เปิด" if armed else "ปิด") + " (SW4)")
    arm_lbl.color(COL_OK if armed else COL_DIM)
    energy.text("energy: %.0f" % r["energy"])
    ch.set_next(0, MAX_CM if cm is None else min(cm, MAX_CM))
    ch.set_next(s_line, alert_cm)
    if (armed, inside, count) != shown:
        shown = (armed, inside, count)
        show_matrix(armed, inside, count)
    ui.poll()
    time.sleep_ms(TICK_MS)

show_matrix(False, False, count)
print("เฝ้าคอกครบเวลา พบผู้บุกรุก", count, "ครั้ง")

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) ตั้ง CONFIRM_N = 1 แล้วเดินผ่านหน้าบอร์ดเร็ว ๆ 5 รอบ นับว่าตัวนับขึ้นกี่ครั้ง
#    เทียบกับ CONFIRM_N = 3 แบบไหน "เตือนผิด" น้อยกว่า แบบไหน "เตือนช้า" กว่า
# 2) นับผู้บุกรุกเฉพาะตอนที่เรดาร์บอกว่า "มีการเคลื่อนไหว" ด้วย (ใช้ r["presence"])
#    แล้วลองยืนนิ่งหน้าบอร์ด - ระบบแบบไหนเหมาะกับคอกวัวตอนกลางคืน
