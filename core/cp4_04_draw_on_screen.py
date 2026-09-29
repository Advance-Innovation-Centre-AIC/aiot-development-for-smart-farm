# cp4_04_draw_on_screen.py - หลักการ 4.4: วาดด้วยของที่มี เส้น (Line) กับ จุด (DotMatrix)
#
# หลักการ  : เฟิร์มแวร์นี้ไม่มี Canvas (ผืนที่สั่งวาดเส้น สี่เหลี่ยม วงกลม ลงไปเองได้) และไม่มี Gauge
#            ภาพที่วาดเองได้จึงมาจากของสองอย่าง:
#            ui.Line = เส้นหักที่ลากผ่านจุดที่เราเติมด้วย add_point(x, y) ได้ไม่เกิน 16 จุดต่อเส้น
#              จะวาดใหม่ต้อง clear_items() แล้วเติมจุดใหม่ทั้งชุด ความหนาเปลี่ยนตอนรันด้วย PROP_LINE_WIDTH
#              สีเส้นตั้งได้ตอนสร้างเท่านั้น (color() ของ Line จะทาสีพื้นทั้งกล่อง ไม่ใช่สีเส้น)
#            ui.DotMatrix = ตารางจุด 16 x 8 ส่งทั้งภาพทีเดียวด้วย set_pixels(16 ไบต์):
#              1 บิตต่อจุด เรียงแถวบนลงล่าง ซ้ายไปขวา บิตสูงสุดของแต่ละไบต์ = จุดซ้ายสุด มีสีเดียว
#            จอไฟ RGB บนบอร์ด (16 x 8 เท่ากัน) วาดภาพเดียวกันด้วย rgbmatrix.blit(64 ไบต์):
#              4 บิตต่อจุด = มีสี ไบต์ที่ y*8 + x//2 จุด x คู่อยู่ 4 บิตล่าง จุด x คี่อยู่ 4 บิตบน
#            ภาพเดียวกัน สองรูปแบบไบต์ และส่งไปเฉพาะตอนภาพเปลี่ยน
# ลองเล่น  : หมุน VR1 (ความชื้นดิน) ขึ้นลงช้า ๆ เส้นกราฟบนจอ จุดบนจอ และจอไฟ RGB เลื่อนไปพร้อมกัน
#            หมุนลงต่ำกว่าเส้นแดง (25 %) เส้นกราฟหนาขึ้น และแท่งบนจอไฟ RGB เป็นสีแดง
# ของบนบอร์ด: ลูกบิด VR1 · จอไฟ RGB 16x8 · ลำโพง (เสียงตอนดินเริ่มแห้งและตอนกลับมาชื้น)
# ในฟาร์ม  : กราฟย้อนหลังสั้น ๆ บนจอหน้าตู้ และไฟแท่งบนตู้ที่มองเห็นจากไกล ๆ ว่าช่วงไหนดินแห้ง
# บอร์ด    : TESAIoT Dev Kit และ BENTO Emulator (Emulator วาดจอไฟ RGB ไว้บนแผงจำลอง)
#            ui.Image.set_image (ภาพพิกเซลสี) ยังไม่ใช้ในไฟล์นี้ เพราะรูปแบบไบต์และความเร็วบนบอร์ดยังไม่ได้วัด

import pots
import rgbmatrix
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
DRY_BELOW = 25       # ความชื้นดินต่ำกว่านี้ (%) = แห้ง (เส้นหนา แท่งแดง)
POINTS = 16          # จำนวนจุดย้อนหลัง = ความกว้างของจอจุด และเพดานจุดต่อเส้นของเฟิร์มแวร์
SAMPLE_MS = 1000     # อ่าน VR1 และวาดใหม่ทุกกี่ ms (16 จุด = ย้อนหลัง 16 วินาที)
LINE_THIN, LINE_BOLD = 3, 7   # ความหนาเส้นกราฟตอนปกติ / ตอนแห้ง (พิกเซล)
CH_X, CH_Y, CH_W, CH_H = 24, 76, 400, 160   # กรอบกราฟบนจอ (เส้นสองเส้นวางซ้อนในกรอบนี้โดยตั้งใจ)
RUN_MS = 180000      # เล่นนาน 3 นาทีแล้วจบเอง
SPEAKER = 40             # ความดังลำโพงรวม 0-100% (ใช้ได้กับ firmware 2.4.2 ขึ้นไป)
VOLUME = 25              # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้
BTN_NAMES = ("SW5", "SW6")   # ชื่อบนแผง: SW5 = ปุ่มล่าง, SW6 = ปุ่มบน (ไฟล์นี้ไม่ใช้ปุ่ม)

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD, COL_BAD, COL_INFO = 0x171B22, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def soil_percent():
    return pots.read(0) * 100 // 4095      # VR1: 0 = แห้งสนิท, 100 = แฉะ


# เสียงเตือนใช้ ui.tone เพราะปรับความดังได้ (ui.sfx ในเฟิร์มแวร์นี้ปรับความดังไม่ได้)
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)


def draw_matrix(frame):
    # จอไฟ RGB: ส่งทั้งภาพ 64 ไบต์ในข้อความเดียว (เรียกเฉพาะตอนภาพเปลี่ยน)
    try:
        rgbmatrix.blit(frame)
    except OSError:
        pass                    # จอไฟ RGB ตอบไม่ทัน: ข้ามภาพนี้ไป ไม่ให้โปรแกรมหยุด


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ----
def height(v):
    # ความชื้น 0-100 % -> ความสูงแท่ง 0-8 จุด (ปัดเศษ)
    return (v * 8 + 50) // 100


def dots(hist):
    # ภาพสำหรับ DotMatrix: 16 ไบต์ แถวละ 2 ไบต์ (จุด 0-7 กับ 8-15) บิตสูงสุด = จุดซ้ายสุด
    b = bytearray(16)
    for x in range(len(hist)):
        for y in range(8 - height(hist[x]), 8):          # แท่งโตจากแถวล่างขึ้นบน
            b[y * 2 + x // 8] |= 0x80 >> (x % 8)
    return b


def frame(hist):
    # ภาพเดียวกันสำหรับจอไฟ RGB: 64 ไบต์ จุดละ 4 บิต = เลขสี (ดินแห้ง = แดง ปกติ = เขียว)
    f = bytearray(64)
    for x in range(len(hist)):
        c = rgbmatrix.RED if hist[x] < DRY_BELOW else rgbmatrix.GREEN
        for y in range(8 - height(hist[x]), 8):
            f[y * 8 + x // 2] |= c << (4 * (x % 2))      # x คู่ = 4 บิตล่าง, x คี่ = 4 บิตบน
    return f


def line_y(v):
    # ความชื้น -> พิกัด y ในกรอบกราฟ (100 % อยู่บน, 0 % อยู่ล่าง เว้นขอบ 4 พิกเซล)
    return CH_H - 4 - v * (CH_H - 8) // 100


# ---- 4) เครือข่าย (ไฟล์นี้ไม่ใช้) ----


# ---- 5) หน้าจอ ----
def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("วาดด้วยของที่มี: เส้น กับ จุด", x=12, y=6, color=COL_TEXT, value=24)
    ui.Label("ไม่มี Canvas: ใช้ ui.Line กับ ui.DotMatrix", x=12, y=38, color=COL_DIM, value=16)
    w = {}
    ui.Panel(x=12, y=66, w=424, h=180, color=COL_CARD, min=COL_DIM, max=12, value=1)
    # เส้นเกณฑ์ (วาดครั้งเดียว 2 จุด) แล้วเส้นกราฟ (วาดใหม่ทุกครั้งที่อ่าน) value= คือความหนาเส้นตอนสร้าง
    yd = line_y(DRY_BELOW)
    edge = ui.Line(x=CH_X, y=CH_Y, w=CH_W, h=CH_H, color=COL_BAD, value=2)
    edge.add_point(0, yd)
    edge.add_point(CH_W, yd)
    w["trend"] = ui.Line(x=CH_X, y=CH_Y, w=CH_W, h=CH_H, color=COL_INFO, value=LINE_THIN)
    ui.Label("เส้นแดง = เกณฑ์ดินแห้ง", x=24, y=254, color=COL_DIM, value=16)
    w["val"] = ui.Label(" ", x=24, y=284, color=COL_TEXT, value=24)
    # DotMatrix: cols/rows บอกจำนวนจุด w/h บอกขนาดรวม (เฟิร์มแวร์คิดขนาดจุดให้เอง)
    w["dots"] = ui.DotMatrix(x=466, y=76, w=304, h=152, cols=16, rows=8)
    ui.Label("จอไฟ RGB บนบอร์ด = ภาพเดียวกัน", x=466, y=254, color=COL_DIM, value=16)
    w["help"] = ui.Label("หมุน VR1 ช้า ๆ", x=12, y=352, color=COL_DIM, value=16)
    ui.poll()
    return w


def draw_trend(line, hist):
    # วาดเส้นใหม่ทั้งเส้น: ล้างจุดเก่า แล้วเติมจุดใหม่ทีละจุด (ไม่เกิน 16 จุด)
    line.clear_items()
    for i in range(len(hist)):
        line.add_point(i * CH_W // (POINTS - 1), line_y(hist[i]))


# ---- 6) โปรแกรมหลัก ----
def main():
    if hasattr(ui, "volume"):    # บอร์ดที่ยังเป็น 2.4.1 ข้ามบรรทัดนี้
        ui.volume(SPEAKER)
    w = build_screen()
    hist = []                                          # ความชื้นย้อนหลัง (list ธรรมดา เก่าสุดอยู่หน้า)
    dry = pic = None
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        v = soil_percent()                             # 1) อ่าน
        hist.append(v)
        if len(hist) > POINTS:
            hist.pop(0)
        d = v < DRY_BELOW                              # 2) ตัดสิน
        if d != dry:                                   # 3) โชว์: ความหนาเส้นเปลี่ยนเฉพาะตอนสถานะเปลี่ยน
            if dry is not None:
                beep("bad" if d else "good")
            w["trend"].prop(ui.PROP_LINE_WIDTH, LINE_BOLD if d else LINE_THIN)
            dry = d
        draw_trend(w["trend"], hist)
        b = dots(hist)
        if b != pic:                                   # จอจุดกับจอไฟ RGB: ส่งเฉพาะตอนภาพเปลี่ยน
            w["dots"].set_pixels(b)
            draw_matrix(frame(hist))
            pic = b
        w["val"].text("ดิน %d %%" % v)
        while ui.poll():                               # ระบายคิวเหตุการณ์ให้ว่าง (ไฟล์นี้ไม่ได้ใช้ แต่คิวเต็มแล้วจะทิ้งของใหม่)
            pass
        time.sleep_ms(SAMPLE_MS)
    try:
        rgbmatrix.clear()                              # จบรอบ: ดับจอไฟ RGB
    except OSError:
        pass
    w["help"].color(COL_BAD)
    w["help"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    ui.poll()


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เดาก่อนรัน: ตั้ง POINTS = 20 เส้นกราฟจะมีกี่จุดจริง ๆ (เฟิร์มแวร์เก็บได้ 16 จุดต่อเส้น จุดเกินถูกทิ้งเงียบ ๆ)
#    แล้วจอจุดกับจอไฟ RGB ที่กว้าง 16 จะเป็นอย่างไร
# 2) เปลี่ยน LINE_BOLD เป็น 12 แล้วหมุนลงให้ดินแห้ง เส้นหนาช่วยให้เห็นจากไกลขึ้นไหม
#    ลองเพิ่มเส้นเกณฑ์ "แฉะเกินไป" ที่ 80 % อีกเส้น (Line ใหม่ 2 จุด สีส้ม)
# 3) ในฟังก์ชัน dots() ลองเปลี่ยน 0x80 >> (x % 8) เป็น 0x01 << (x % 8) ภาพบนจอจุดจะกลับด้านอย่างไร
#    นี่คือเหตุผลที่ต้องอ่านรูปแบบไบต์ของเฟิร์มแวร์ก่อนวาด
