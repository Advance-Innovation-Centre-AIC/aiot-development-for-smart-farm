# cp3_02_matrix_toolkit.py - หลักการ 3.2: จอไฟ 16x8 เป็นจอเล็ก ๆ ได้ 4 แบบ (กราฟ หลอด ตัวเลข ตัววิ่ง)
#
# หลักการ  : จอไฟ RGB 16x8 มีแค่ 128 จุด 8 สีตายตัว ไม่มีความสว่างให้ปรับ จึงต้องเลือก "แบบ" ให้เหมาะกับข่าว
#            กราฟเส้น (sparkline) = เห็นแนวโน้ม · หลอด (bar) = เห็นว่าเต็มแค่ไหน · ตัวเลข (score) = อ่านค่าตรง ๆ
#            ตัววิ่ง (scroll) = ข้อความสั้น ๆ · และเขียนจอไฟเฉพาะตอน "ภาพเปลี่ยน" เพราะทุกครั้งคือข้อความหนึ่งก้อน
#            ที่ต้องส่งข้ามไปแกนที่คุมจอ (จอนับให้ดูว่าเขียนไปกี่ครั้ง เทียบกับรอบที่ตัดสิน)
#            กราฟเส้นใช้ blit(): ภาพทั้งจอในก้อนเดียว 64 ไบต์ จุดละ 4 บิต (สี 0-7) แถวละ 8 ไบต์
#            ไบต์ที่ y * 8 + x // 2 เก็บสองจุด: x คู่ = 4 บิตล่าง, x คี่ = 4 บิตบน
# ลองเล่น  : หมุน VR1 ช้า ๆ ดูแต่ละแบบ กด SW6 (ปุ่มบน) เปลี่ยนแบบ กด SW5 (ปุ่มล่าง) ส่งตัววิ่งใหม่ด้วยค่าล่าสุด
#            ดูตัวนับ "เขียนจอไฟ" บนจอภาพ: ค่านิ่ง = ตัวนับไม่ขยับ (ยกเว้นกราฟเส้นที่เลื่อนทุกรอบ)
# ของบนบอร์ด: จอไฟ RGB 16x8 (rgbmatrix) - VR1 = ค่า 0-100 % - SW5 = pressed(0) - SW6 = pressed(1) - ลำโพง
# กติกาจอไฟ : ใส่อาร์กิวเมนต์ตามตำแหน่งเท่านั้น (ไม่ใช้ชื่อ=ค่า) - ทุกคำสั่งห่อ try/except OSError
#            fill() รับสี 1-7 เท่านั้น ดับจอใช้ clear() - คำสั่งวาดทุกตัวหยุดตัววิ่งที่ค้างอยู่ให้เอง
#            bar() วาดเฉพาะแถวล่างสุด (แถวอื่นคงเดิม) จึง clear() ก่อนเข้าแบบหลอด
#            scroll() รู้จักแค่ 0-9 A-Z a-z (ภาษาไทยวิ่งเป็นช่องว่าง) ส่งครั้งเดียวแล้ววิ่งเองบนแกนจอ
# ในฟาร์ม  : ป้ายไฟหน้าโรงเรือน อ่านได้จากไกล ๆ: ความชื้นดินเป็นหลอด ระดับน้ำเป็นตัวเลข คำเตือนเป็นตัววิ่ง
# บอร์ด    : TESAIoT Dev Kit และ BENTO Emulator (ตัววิ่งใน Emulator ขึ้นแค่เฟรมแรก บนบอร์ดวิ่งจริง)

import buttons
import pots
import rgbmatrix
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
MODES = ("กราฟเส้น (blit)", "หลอด (bar)", "ตัวเลข (score)", "ตัววิ่ง (scroll)")
WARN, ALARM = 60, 85     # ค่าตั้งแต่เท่านี้เปลี่ยนสี: เขียว -> เหลือง -> แดง
SAMPLE_MS = 20       # อ่านปุ่มทุกกี่ ms (เฟิร์มแวร์กรองสั่นทุกครั้งที่อ่าน จึงต้องอ่านถี่)
TICK_MS = 500        # ตัดสินภาพใหม่ทุกกี่ ms (กราฟเส้นเลื่อนหนึ่งช่องต่อรอบ)
RUN_MS = 180000      # เล่นนาน 3 นาทีแล้วจบเอง
SPEAKER = 40             # ความดังลำโพงรวม 0-100% (ใช้ได้กับ firmware 2.4.2 ขึ้นไป)
VOLUME = 25              # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้
BTN_NAMES = ("SW5", "SW6")   # ชื่อบนแผง: SW5 = ปุ่มล่าง = pressed(0), SW6 = ปุ่มบน = pressed(1)

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF
ON_SCREEN = (COL_DIM, COL_BAD, COL_OK, COL_WARN)   # สีบนจอภาพ ตามเลขสีจอไฟ: 1 แดง 2 เขียว 3 เหลือง


# ---- 2) ฮาร์ดแวร์ ----
# เสียงเตือนใช้ ui.tone เพราะปรับความดังได้ (ui.sfx ในเฟิร์มแวร์นี้ปรับความดังไม่ได้)
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)


def draw(fn, *args):
    # เรียกคำสั่งจอไฟหนึ่งคำสั่ง (อาร์กิวเมนต์ตามตำแหน่ง) คืน True ถ้าส่งออก
    # จอไฟอยู่บนบัสของแกนที่คุมจอ ส่งไม่ผ่าน = OSError ไม่ให้โปรแกรมล้ม
    try:
        fn(*args)
        return True
    except OSError:
        return False


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ----
def level_color(v):
    # ค่า 0-100 -> สีบนจอไฟ (ค่าคงที่ 1-7 ของ rgbmatrix)
    return rgbmatrix.RED if v >= ALARM else rgbmatrix.YELLOW if v >= WARN else rgbmatrix.GREEN


def spark_frame(hist):
    # ค่าล่าสุด 16 ค่า -> ภาพ 64 ไบต์สำหรับ blit(): คอลัมน์ละหนึ่งจุด ค่าสูง = จุดอยู่แถวบน (y = 0)
    buf = bytearray(64)
    x0 = 16 - len(hist)                               # ข้อมูลยังไม่ครบ 16 = ชิดขวา
    for k in range(len(hist)):
        x, v = x0 + k, hist[k]
        y = 7 - v * 7 // 100                          # 0 % = แถวล่าง (7), 100 % = แถวบน (0)
        c = level_color(v)
        buf[y * 8 + x // 2] |= c << 4 if x & 1 else c   # x คี่ = 4 บิตบน, x คู่ = 4 บิตล่าง
    return buf


def picture(mode, v, hist):
    # ภาพที่ควรอยู่บนจอไฟตอนนี้ ในรูปที่เอามาเทียบได้ (ภาพเดิม = ไม่ต้องเขียนซ้ำ)
    if mode == 0:
        return bytes(spark_frame(hist))
    if mode == 1:
        return (v * 16 // 100, level_color(v))        # หลอด: จำนวนจุดที่ติดบนแถวล่าง + สี
    if mode == 2:
        return (v, level_color(v))
    return None                                       # ตัววิ่ง: ส่งตอนเข้าแบบ และตอนกด SW5 เท่านั้น


# ---- 4) เครือข่าย (ไฟล์นี้ไม่ใช้) ----


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("จอไฟ 16x8 เป็นจอเล็ก 4 แบบ", x=12, y=6, color=COL_TEXT, value=24)
    ui.Label("VR1 = ค่า  SW6 = เปลี่ยนแบบ  SW5 = ส่งตัววิ่งใหม่", x=12, y=38, color=COL_DIM, value=16)
    card(12, 64, 300, 274, "แบบ")
    w = {"modes": [ui.Label(MODES[i], x=28, y=100 + i * 52, color=COL_DIM) for i in range(4)]}
    card(322, 64, 458, 274, "ค่า VR1 และคำสั่งล่าสุด")
    w["seg"] = ui.Seg7(text="--", x=338, y=96, w=150, h=56, color=COL_OK)
    w["call"] = ui.Label(" ", x=338, y=180, color=COL_TEXT, value=16)
    w["count"] = ui.Label(" ", x=338, y=220, color=COL_DIM, value=16)
    w["help"] = ui.Label("เขียนจอไฟเฉพาะตอนภาพเปลี่ยน", x=12, y=352, color=COL_DIM, value=16)
    ui.poll()
    return w


def show_mode(w, mode):
    for i in range(4):
        w["modes"][i].color(COL_WARN if i == mode else COL_DIM)


# ---- 6) โปรแกรมหลัก ----
def main():
    if hasattr(ui, "volume"):    # บอร์ดที่ยังเป็น 2.4.1 ข้ามบรรทัดนี้
        ui.volume(SPEAKER)
    w = build_screen()
    draw(rgbmatrix.clear)
    mode, hist, last = 0, [], None
    writes = ticks = 0
    down = [False, False]
    show_mode(w, mode)
    beep("start")
    t0 = t_tick = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        send_scroll = False
        for i in (0, 1):                                   # ปุ่มกรองสั่นทุกครั้งที่อ่าน จึงอ่านทุก 20 ms
            d = buttons.pressed(i)
            if d and not down[i]:
                if i:                                      # SW6 = แบบถัดไป
                    mode, last = (mode + 1) % 4, None      # last = None บังคับวาดใหม่
                    show_mode(w, mode)
                    draw(rgbmatrix.clear)                  # หลอดวาดแค่แถวล่าง ภาพเก่าต้องหายก่อน
                    send_scroll = mode == 3
                else:                                      # SW5 = ส่งตัววิ่งใหม่ (เฉพาะแบบตัววิ่ง)
                    send_scroll = mode == 3
                beep("tap")
            down[i] = d
        v = pots.read(0) * 100 // 4095
        if send_scroll:                                    # ตัววิ่ง: ส่งข้อความครั้งเดียว แกนจอวิ่งต่อเอง
            text = "VR1 %d" % v
            if draw(rgbmatrix.scroll, text, level_color(v), 80):
                writes += 1
            w["call"].text('scroll("%s", %d, 80)' % (text, level_color(v)))
        if time.ticks_diff(now, t_tick) >= TICK_MS:        # รอบตัดสิน: ทุกครึ่งวินาที
            t_tick = now
            ticks += 1
            hist.append(v)
            if len(hist) > 16:
                hist.pop(0)                                # เก็บแค่ 16 ค่า = กว้างเท่าจอไฟ
            pic = picture(mode, v, hist)
            if pic is not None and pic != last:            # ภาพเปลี่ยน = เขียน · ภาพเดิม = ไม่แตะจอไฟ
                last = pic
                if mode == 0:
                    ok, call = draw(rgbmatrix.blit, pic), "blit(64 ไบต์)"
                elif mode == 1:
                    ok, call = draw(rgbmatrix.bar, v, 100, pic[1]), "bar(%d, 100, %d)" % (v, pic[1])
                else:
                    ok, call = draw(rgbmatrix.score, v, pic[1]), "score(%d, %d)" % (v, pic[1])
                writes += ok
                w["call"].text(call)
            w["seg"].text(str(v))
            w["seg"].color(ON_SCREEN[level_color(v)])      # ตัวเลขบนจอภาพสีเดียวกับจอไฟ
            w["count"].text("เขียนจอไฟ %d ครั้ง / ตัดสิน %d รอบ" % (writes, ticks))
            ui.poll()
        time.sleep_ms(SAMPLE_MS)
    draw(rgbmatrix.clear)
    w["help"].color(COL_WARN)
    w["help"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    beep("good")
    ui.poll()


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เดาก่อนรัน: ในแบบหลอด หมุน VR1 จาก 50 ไป 53 % จอไฟถูกเขียนใหม่ไหม (ใบ้: 16 จุดต่อ 100 % = จุดละกี่ %)
# 2) ตั้ง WARN = 30 แล้วดูกราฟเส้น สีของจุดเปลี่ยนตรงไหน ใครที่ยืนไกล ๆ อ่านอะไรได้บ้าง
# 3) เปลี่ยนข้อความตัววิ่งเป็นภาษาไทย แล้วดูจอไฟ ได้อะไร เพราะอะไร (ดูกติกาจอไฟในหัวไฟล์)
