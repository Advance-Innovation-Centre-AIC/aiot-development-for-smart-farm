# cp3_06_practise.py - แบบฝึกเติมโค้ด (Code Quest ระดับ 3): สร้างกรอบ Modbus TCP FC06 เอง
#
# วิธีเล่น  : ไฟล์นี้คือ cp3_06_modbus_frame.py ในโหมดดูกรอบ (ไม่มีส่วนเครือข่าย ให้ไฟล์เล็กพอคอมไพล์บนบอร์ด)
#            ฟังก์ชัน fc06_write() ในส่วน "3) สมอง" เว้นช่อง ____ (ขีดล่างสี่ตัว) ไว้ 2 ช่อง: A, B
#            ครั้งนี้สร้างกรอบเป็นสองท่อน: PDU 5 ไบต์ (fc, addr, value) แล้วต่อท้าย MBAP 7 ไบต์
#            ช่อง A = รูปแบบ struct ของ PDU (ดู fc03_read ข้างบนเป็นตัวอย่าง: ">" = big-endian, B = 1 ไบต์, H = 2 ไบต์)
#            ช่อง B = ค่าช่อง len ใน MBAP (นับเฉพาะไบต์ที่ตามหลังช่อง len)
#            เติมให้ครบแล้วรัน โปรแกรมตรวจกรอบ 3 กรอบก่อนเปิดจอ (self_test) เทียบกับกรอบที่รู้คำตอบแน่นอน
#            ผ่านครบ = Console ขึ้น "ผ่าน!" แล้วเล่นต่อได้: SW5 = กรอบ FC03, SW6 = กรอบ FC06 (สลับ 1/0)
#            ยังไม่ถูก = Console บอกว่ากรอบไหนผิด ได้ไบต์อะไร แล้วหยุด (ยังไม่เปิดจอ)
# ถ้าเจอ   : NameError: name '____' isn't defined = ยังมีช่องที่ไม่ได้เติม (ตั้งใจให้หยุดชัด ๆ แบบนี้)
# ทบทวน    : ADU = MBAP 7 ไบต์ (tid 2, protocol 2 = 0 เสมอ, len 2, unit 1) + PDU 5 ไบต์ (fc 1, addr 2, value 2)
#            ตัวอย่างที่ต้องได้เป๊ะ: fc06_write(1, 1, 0, 1) = 00 01 00 00 00 06 01 06 00 00 00 01
#            ลืม ">" = struct ใช้ลำดับไบต์และการเว้นช่องของเครื่อง ได้กรอบยาวผิดหรือไบต์กลับด้าน
# ติดขัด?  : รันไฟล์ตัวอย่างเต็ม cp3_06_modbus_frame.py เพื่อไปต่อก่อน แล้วค่อยกลับมาเทียบกับของตัวเอง
#            อยากส่งกรอบไปหา PLC จำลองจริง ใช้ไฟล์ตัวอย่างเต็ม (มี WiFi + MQTT + gateway บนโน้ตบุ๊ก)
# เฉลย     : โจทย์หลัก มีเฉลยในคาบ อยู่ที่ practise/solutions/cp3_06_practise_solution.py
#            (ลองเองก่อน แล้วค่อยเปิดดูเมื่อจำเป็น)
# บอร์ด    : TESAIoT Dev Kit และ BENTO Emulator
#
# (ทำจาก cp3_06_modbus_frame.py 63d04321697d)

import buttons
import struct
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
UNIT_ID = 1          # เลขเครื่องของ PLC (unit id) ใน MBAP
SAMPLE_MS = 20       # อ่านปุ่มทุกกี่ ms (เฟิร์มแวร์กรองสั่นทุกครั้งที่อ่าน จึงต้องอ่านถี่)
RUN_MS = 180000      # เล่นนาน 3 นาทีแล้วจบเอง
SPEAKER = 40             # ความดังลำโพงรวม 0-100% (ใช้ได้กับ firmware 2.4.2 ขึ้นไป)
VOLUME = 25              # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้
BTN_NAMES = ("SW5", "SW6")   # ชื่อบนแผง: SW5 = ปุ่มล่าง = pressed(0), SW6 = ปุ่มบน = pressed(1)

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
# เสียงเตือนใช้ ui.tone เพราะปรับความดังได้ (ui.sfx ในเฟิร์มแวร์นี้ปรับความดังไม่ได้)
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ----
def fc03_read(tid, unit, addr, qty):
    # FC03 อ่าน holding register qty ช่อง เริ่มที่ช่อง addr (ทำเสร็จแล้ว ใช้ดูเป็นตัวอย่าง)
    # ">HHHBBHH" = big-endian: tid(H) protocol(H) len(H) unit(B) | fc(B) addr(H) qty(H) รวม 12 ไบต์
    return struct.pack(">HHHBBHH", tid & 0xFFFF, 0, 6, unit, 3, addr, qty)


def fc06_write(tid, unit, addr, value):
    # FC06 เขียนค่า value ลง holding register ช่องเดียว (ช่อง addr) สร้างเป็นสองท่อนแล้วต่อกัน
    pdu = struct.pack(____, 6, addr, value)                    # ช่อง A: รูปแบบของ fc(1) addr(2) value(2)
    mbap = struct.pack(">HHHB", tid & 0xFFFF, 0, ____, unit)   # ช่อง B: len = นับไบต์ที่ตามหลังช่องนี้
    return mbap + pdu


def self_test():
    # ตรวจกรอบ 3 กรอบก่อนเปิดจอ: (ชื่อ, กรอบที่สร้าง, กรอบที่ต้องได้ - ตัวแรกมาจากสไลด์)
    for c in (("fc06_write(1, 1, 0, 1)", fc06_write(1, 1, 0, 1), "000100000006010600000001"),
              ("fc06_write(258, 2, 0, 0)", fc06_write(258, 2, 0, 0), "010200000006020600000000"),
              ("fc03_read(7, 1, 0, 3)", fc03_read(7, 1, 0, 3), "000700000006010300000003")):
        got = "%02x" * len(c[1]) % tuple(c[1])        # ไบต์เป็นเลขฐานสิบหก ตัวละ 2 หลัก
        if got != c[2]:
            print("ยังไม่ถูก:", c[0], "ควรได้", c[2], "(12 ไบต์) แต่ได้", got, "(%d ไบต์)" % len(c[1]))
            return False
    print("ผ่าน! fc06_write สร้างกรอบ Modbus TCP ถูกทุกไบต์")
    return True


# ---- 4) เครือข่าย (ไฟล์นี้ไม่ใช้) ----
# ไฟล์ตัวอย่างเต็ม cp3_06_modbus_frame.py ส่งกรอบขึ้น MQTT ให้ gateway บนโน้ตบุ๊กพูด Modbus TCP แทน
# (เฟิร์มแวร์ไม่มีโมดูล Modbus และไม่มี socket) แบบฝึกนี้ตัดส่วนนั้นออกให้ไฟล์เล็กพอคอมไพล์บนบอร์ด


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("Modbus TCP 12 ไบต์", x=12, y=6, color=COL_TEXT, value=24)
    ui.Label("แบบฝึก: โหมดดูกรอบ ไม่ส่งไปไหน", x=12, y=38, color=COL_WARN, value=16)
    card(12, 62, 768, 112, "ฟ้า = MBAP  เขียว = PDU")
    w = {"b": []}
    for i in range(12):                                   # ไบต์ที่ i อยู่ที่ x = 28 + i * 62
        w["b"].append(ui.Label("--", x=28 + i * 62, y=96, color=COL_INFO if i < 7 else COL_OK, value=28))
    # ชื่อช่องใต้ไบต์แรกของแต่ละช่อง: tid 2 ไบต์ proto 2 len 2 unit 1 | fc 1 addr 2 qty/value 2
    for i, name in zip((0, 2, 4, 6, 7, 8, 10), "tid proto len unit fc addr qty/value".split()):
        ui.Label(name, x=28 + i * 62, y=136, color=COL_DIM, value=14)
    card(12, 186, 768, 152, "กรอบนี้ขออะไร")
    w["stat"] = ui.Label(" ", x=28, y=224, color=COL_TEXT)          # ไม่ใส่ value = ตัวอักษร 20
    w["note"] = ui.Label("SW5 = FC03 อ่าน  SW6 = FC06 เขียน", x=12, y=352, color=COL_DIM, value=16)
    ui.poll()
    return w


# ---- 6) โปรแกรมหลัก ----
def main():
    if hasattr(ui, "volume"):    # บอร์ดที่ยังเป็น 2.4.1 ข้ามบรรทัดนี้
        ui.volume(SPEAKER)
    if not self_test():                    # ตรวจ fc06_write() ก่อน ไม่ผ่าน = หยุดตรงนี้
        return
    w = build_screen()
    beep("good")
    want, n = 1, 0                        # ค่าที่ SW6 จะเขียนครั้งหน้า (สลับ 1/0), n = tid
    down = [False, False]
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        for i in (0, 1):                  # i = 0 คือ SW5, i = 1 คือ SW6
            d = buttons.pressed(i)
            if d and not down[i]:         # ขอบกด
                n += 1
                arg = want if i else 3
                frame = (fc06_write if i else fc03_read)(n, UNIT_ID, 0, arg)
                for k in range(12):
                    w["b"][k].text("%02X" % frame[k])
                w["stat"].text(("#%d FC03 อ่าน HR0 %d ช่อง", "#%d FC06 เขียน HR0 = %d")[i] % (n, arg))
                ui.poll()
                beep("tap")
                if i:
                    want = 1 - want
            down[i] = d
        time.sleep_ms(SAMPLE_MS)
    w["note"].color(COL_WARN)
    w["note"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    ui.poll()


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) ผ่านแล้ว ลองลบ ">" ออกจากช่อง A แล้วรันใหม่ self_test บอกว่าได้กี่ไบต์ ทำไมไม่เท่ากับ 5 + 7
# 2) ถ้าอยากเขียนสองช่องในคำขอเดียว (FC16) PDU ต้องมีอะไรเพิ่ม len ใน MBAP จะเป็นเท่าไร
