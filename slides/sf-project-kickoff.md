---
marp: true
theme: default
paginate: true
title: "เปิดโจทย์โปรเจกต์ Smart Farm (1 สัปดาห์) · AIoT Development for Smart Farm"
---


<!-- fit-css -->
<style>
section { font-size: 24px; padding: 40px 52px; justify-content: flex-start; }
section h1 { font-size: 1.45em; line-height: 1.12; margin: 0 0 .22em; }
section h2 { font-size: 1.12em; margin: .15em 0; }
section h3 { font-size: 1.0em; margin: .12em 0; }
section p, section li { margin: .16em 0; line-height: 1.32; }
section img { max-width: 100%; height: auto; }
section svg { max-height: 330px; width: 100%; }
section table { font-size: .78em; }
section pre { font-size: .70em; line-height: 1.32; background:#0d1117; color:#e6edf3; border:1px solid #30363d; border-radius:8px; padding:12px 16px; box-shadow:0 2px 8px rgba(0,0,0,.25); }
section pre code { white-space: pre-wrap; background:transparent; color:inherit; } section pre .hljs-comment{color:#8b949e;font-style:italic} section pre .hljs-keyword,section pre .hljs-built_in,section pre .hljs-literal{color:#ff7b72} section pre .hljs-string{color:#a5d6ff} section pre .hljs-number{color:#79c0ff} section pre .hljs-title,section pre .hljs-title.function_,section pre .hljs-section{color:#d2a8ff} section pre .hljs-meta{color:#ffa657} section pre .hljs-attr,section pre .hljs-attribute,section pre .hljs-name{color:#7ee787}
section blockquote { margin: .25em 0; font-size: .92em; }
section p > img + img { margin-left: 10px; }
section { overflow-y: auto; overflow-x: hidden; }
section::-webkit-scrollbar { width: 11px; }
section::-webkit-scrollbar-thumb { background:#4a90d9; border-radius:6px; }
section::-webkit-scrollbar-track { background:rgba(0,0,0,.06); }
section img{filter:drop-shadow(0 3px 12px rgba(0,0,0,.5))}
section.cover{background-color:#0b1426}
section.cover *{color:#fff !important}
section.cover h1,section.cover h2,section.cover h3,section.cover p,section.cover li,section.cover strong,section.cover em,section.cover blockquote,section.cover div{text-shadow:0 2px 9px rgba(0,0,0,.92),0 0 3px rgba(0,0,0,.8)}
section.cover blockquote{border-left:4px solid rgba(120,200,255,.6) !important;background:rgba(13,17,23,.55) !important;border-radius:6px;padding:.3em .6em}
section.cover h1,section.cover h2,section.cover h3,section.cover p,section.cover blockquote{max-width:60%}
section.cover img{filter:none}

/* ---- ส่วนของคอร์ส Smart Farm ---- */
section.sec { background: linear-gradient(135deg,#0f3d2e 0%,#1f6b3a 55%,#7cb342 100%); color:#fff; justify-content:center; }
section.sec h1 { font-size: 2.1em; color:#fff; }
section.sec h2, section.sec p, section.sec li { color:#e8f5e9; }
section.sec .when { display:inline-block; background:rgba(0,0,0,.28); border-radius:999px; padding:.1em .8em; font-size:.9em; margin-bottom:.4em; }
section.sec a { color:#fff; text-decoration:underline; }
section.sec code { background:rgba(0,0,0,.25); color:#fff; }
.chal { margin-top:.7em; background:rgba(255,255,255,.95); color:#bf360c !important; border-radius:14px; padding:.45em .9em; font-size:.95em; box-shadow:0 4px 18px rgba(0,0,0,.25); }
.chal b { color:#bf360c; }
section.brk { background: linear-gradient(135deg,#ff9f1c 0%,#ffbf69 60%,#ffe8c2 100%); justify-content:center; }
.cols { display:flex; gap:20px; align-items:flex-start; }
.cols > div { flex:1; min-width:0; }
.c40 { flex:0 0 40% !important; } .c45 { flex:0 0 45% !important; } .c55 { flex:0 0 55% !important; } .c60 { flex:0 0 60% !important; }
.cap { font-size:.5em; color:#78909c; line-height:1.3; margin-top:.15em; }
.shot img { border-radius:8px; filter:drop-shadow(0 3px 10px rgba(0,0,0,.35)); }
.goal { background:#e8f5e9; border-left:6px solid #2e7d32; border-radius:8px; padding:.25em .7em; margin:.2em 0; font-size:.86em; }
.think { background:#fff8e1; border-left:6px solid #f5a623; border-radius:8px; padding:.25em .7em; margin:.25em 0; font-size:.84em; }
.warn { background:#ffebee; border-left:6px solid #e53935; border-radius:8px; padding:.25em .7em; margin:.25em 0; font-size:.84em; }
.try { font-size:.84em; }
.tiles { display:grid; grid-template-columns:repeat(4,1fr); gap:12px; }
.tile { background:#f6faf5; border:2px solid #cfe3c9; border-radius:12px; padding:8px 10px; font-size:.6em; line-height:1.3; }
.tile img { width:100%; height:128px; object-fit:cover; border-radius:8px; filter:none; }
.tile b.t { display:block; font-size:1.25em; color:#1b5e20; margin:.2em 0; }
.chip { display:inline-block; border-radius:999px; padding:0 8px; margin:2px 2px 0 0; color:#fff; font-size:.92em; }
.chip.s { background:#1e88e5; } .chip.d { background:#8e24aa; } .chip.a { background:#2e7d32; } .chip.r { background:#607d8b; }
.vid { display:flex; gap:14px; align-items:flex-start; background:#f3f6fb; border:1px solid #d6deea; border-radius:10px; padding:8px 10px; margin:.25em 0; }
.vid iframe { flex:0 0 auto; border-radius:6px; }
.vid div { font-size:.64em; line-height:1.35; }
.vid b { font-size:1.08em; }
.timeline { display:flex; gap:4px; margin:.3em 0 .5em; }
.timeline div { border-radius:8px; padding:6px 6px; color:#fff; font-size:.52em; line-height:1.25; text-align:center; }
.timeline div b { display:block; font-size:1.25em; }
.pill { display:inline-block; background:#263238; color:#fff; border-radius:6px; padding:0 7px; font-family:monospace; font-size:.9em; }
.kbd { display:inline-block; border:2px solid #455a64; border-bottom-width:4px; border-radius:6px; padding:0 7px; font-weight:700; }
.big { font-size:1.35em; font-weight:700; }
.src { font-size:.48em; color:#78909c; }

/* ---- Special Session: การ์ดหลักการ ---- */
.rule { background:#fffdf5; border:3px solid #f5a623; border-radius:18px; padding:.5em 1em; margin:.4em 0; font-size:1.25em; font-weight:700; line-height:1.4; color:#5d4037; box-shadow:0 4px 18px rgba(0,0,0,.12); }
.p1 { color:#1565c0; } .p2 { color:#6a1b9a; } .p3 { color:#1b5e20; } .p4 { color:#e65100; }
.cards { display:grid; grid-template-columns:repeat(4,1fr); gap:10px; }
.card { border-radius:12px; padding:10px 12px; font-size:.72em; line-height:1.35; color:#fff; }
.card b { display:block; font-size:1.3em; margin-bottom:.2em; }
.card a { color:#fff; text-decoration:underline; }
.no { background:#eceff1; border-left:6px solid #78909c; border-radius:8px; padding:.25em .7em; margin:.25em 0; font-size:.82em; color:#455a64; }
/* ---- หน้าเปิดส่วน/กิจกรรม: หลักการ · ขั้นตอน · เวลา · ไฟล์ (รูปแบบเดียวกันทุกคาบ) ---- */
section.sec .when { font-weight:700; }
.lead { background:rgba(255,255,255,.15); border-left:6px solid #ffd54f; border-radius:10px; padding:.22em .8em; margin:.15em 0 .5em; font-size:1.02em; font-weight:700; line-height:1.35; color:#fff; }
.flow { display:flex; align-items:center; gap:8px; margin:.15em 0 .5em; }
.flow b { flex:1 1 0; min-width:0; background:#fff; color:#1b5e20; border-radius:10px; padding:.32em .45em; text-align:center; font-size:.68em; line-height:1.25; box-shadow:0 2px 8px rgba(0,0,0,.18); }
.flow i { flex:0 0 auto; font-style:normal; font-weight:700; color:#ffd54f; font-size:.95em; }
.budget { display:flex; flex-wrap:wrap; gap:8px; margin:.1em 0 .45em; }
.budget span { background:rgba(0,0,0,.28); color:#fff; border-radius:999px; padding:.08em .8em; font-size:.66em; white-space:nowrap; }
.budget b { color:#ffd54f; }
.files { display:grid; grid-template-columns:1fr 1fr; gap:4px 28px; background:#fff; color:#263238; border:2px solid #cfe3c9; border-radius:12px; padding:.45em .95em .55em; margin:.3em 0 .2em; font-size:.66em; line-height:1.5; box-shadow:0 3px 12px rgba(0,0,0,.18); }
.files.one { grid-template-columns:1fr; }
.files.page { font-size:.7em; line-height:1.6; padding:.6em 1.1em .7em; }
.files .fh { font-weight:700; color:#1b5e20; border-bottom:2px solid #cfe3c9; margin-bottom:.2em; }
.files .fh.hw { color:#8d6e63; border-color:#eadfd6; }
.files .fh.lap { color:#37474f; border-color:#dfe5e8; margin-top:.45em; }
.files .f span { color:#607d8b; }
section .files a, section.sec .files a { color:#1b5e20; font-family:ui-monospace,SFMono-Regular,Menlo,monospace; font-weight:700; text-decoration:none; }
section .files .hw ~ .f a { color:#6d4c41; }
.tip { background:#f1f8e9; border-left:6px solid #7cb342; border-radius:8px; padding:.25em .7em; margin:.3em 0; font-size:.8em; color:#33691e; }

/* ---- Project Kickoff: ตัวใหญ่ บรรทัดสั้น ---- */
section { font-size: 30px; }
section table { font-size: .74em; }
section table th { background:#1b5e20; color:#fff; }
.one { background:#fffdf5; border:3px solid #f5a623; border-radius:18px; padding:.5em 1em; margin:.4em 0; font-size:1.15em; font-weight:700; line-height:1.45; color:#5d4037; box-shadow:0 4px 18px rgba(0,0,0,.12); }
.phones { display:flex; gap:16px; justify-content:center; align-items:flex-start; }
.phones figure { margin:0; text-align:center; flex:0 0 auto; }
.phones img { height:390px; width:auto; border-radius:14px; border:3px solid #263238; filter:drop-shadow(0 3px 10px rgba(0,0,0,.35)); }
.phones figcaption { font-size:.5em; color:#455a64; margin-top:.2em; }
.ph { border:3px dashed #90a4ae; border-radius:14px; height:390px; width:180px; display:flex; align-items:center; justify-content:center; text-align:center; padding:10px; font-size:.52em; color:#546e7a; background:#f5f7f8; }
.steps { counter-reset:s; list-style:none; padding:0; }
.steps li { counter-increment:s; margin:.28em 0; }
.steps li::before { content:counter(s); display:inline-block; width:1.5em; height:1.5em; line-height:1.5em; text-align:center; border-radius:50%; background:#2e7d32; color:#fff; font-weight:700; margin-right:.5em; font-size:.85em; }
.pairs { display:grid; grid-template-columns:1fr 1fr; gap:10px 18px; }
.pair { background:#f6faf5; border:2px solid #cfe3c9; border-radius:12px; padding:4px 10px 4px; }
.pair .pt { display:block; color:#1b5e20; font-size:.62em; margin-bottom:2px; }
.pi { display:flex; gap:10px; align-items:flex-end; }
.pi figure { margin:0; text-align:center; }
.pi figcaption { font-size:.36em; color:#546e7a; line-height:1.2; }
.pi img.bd { height:196px; width:auto; border-radius:6px; filter:drop-shadow(0 2px 6px rgba(0,0,0,.3)); }
.pi img.wb { height:212px; width:auto; border-radius:8px; border:2px solid #263238; filter:none; }
</style>

<!-- _class: sec -->
<!-- _paginate: false -->

<span class="when">โปรเจกต์ 1 สัปดาห์ · นำเสนอ 18 ต.ค. 2569</span>

# โปรเจกต์ Smart Farm: เปิดโจทย์

<div class="lead">เลือก 1 ใน 25 เรื่อง → รับโค้ดเริ่มต้น (บอร์ด + เว็บ) → ต่อยอดเองอย่างน้อย 3 อย่าง</div>

กลุ่มละ 2 คน · ฝั่งบอร์ด (Smart HMI) + ฝั่งเว็บ (Web UI)

AIoT Development for Smart Farm · TESAIoT Dev Kit

---

## โจทย์ในประโยคเดียว

<div class="one">

เลือก **1 ใน 25 เรื่อง** → นำ **โค้ดเริ่มต้น** ไป **ต่อยอดเองอย่างน้อย 3 อย่าง**<br>
ให้เป็นอุปกรณ์ Smart HMI ของทีม

</div>

- **วัดค่า**ได้ · **ตัดสินใจ**เองได้
- **เตือน**ด้วยเสียงและไฟ
- **ดูและสั่งงานจากมือถือ** ผ่าน MQTT

<div class="warn">

**ไม่มี AI ในโปรเจกต์นี้** · ใช้ของคาบ 1–2 และ MQTT เป็นหลัก<br>
Edge AI (คาบ 3) เรียนเพื่อเข้าใจหลักการเท่านั้น

</div>

---

## กติกา: เรื่องละ 1 ทีม

- 22 กลุ่ม เลือกจาก **25 เรื่อง** · **เรื่องละ 1 ทีม**
- เลือกเรื่องจาก **ลิงก์ / QR Code ที่อาจารย์แจ้งในคาบ** · **ใครกดก่อนได้ก่อน** เห็นผลทันทีทุกเครื่อง
- ทุกเรื่องมี **โค้ดเริ่มต้นครบ** (บอร์ด + หน้าเว็บ) ใน `project/`

<div class="warn">

**ห้ามส่งโค้ดเริ่มต้นตามที่ได้ไป** · ต้องต่อยอดเองอย่างน้อย 3 อย่าง

</div>

- บอกให้ชัดว่า **ส่วนไหนมาจากโค้ดเริ่มต้น** และ **ส่วนไหนทีมทำเอง** (เขียนในหัวไฟล์ด้วย)

---

## 25 เรื่อง · 4 ชุดเริ่มต้น

| ชุดเริ่มต้น | เรื่อง (เลขไฟล์ `project/idea_N_*.py`) |
|---|---|
| **A** เฝ้าอากาศ + เตือน + รับทราบ | 1 ห้องเย็นผลไม้ · 2 รถส่งผลผลิต · 3 ไซโลข้าวเปลือก · 8 เล้าไก่ · 9 โรงเพาะเห็ด · 10 ห้องบ่มกล้วย · 11 โรงอบลำไย · 12 โกดังเมล็ดพันธุ์ · 13 โกดังปุ๋ย · 14 ตู้ยาและวัคซีนสัตว์ · 15 ไฮโดรโปนิกส์ |
| **B** สั่งเครื่องจากมือถือ + กฎปลอดภัย | 4 โรงเรือนรดน้ำ · 6 หอถังน้ำ · 16 พ่นหมอกกล้วยไม้ · 17 เติมอากาศบ่อปลา · 18 พัดลมเล้าไก่ · 19 ประตูน้ำแปลงนา · 20 ให้อาหารปลา |
| **C** การสั่นผิดปกติ + หยุดเครื่อง (กฎธรรมดา ไม่ใช้ AI) | 7 หมอฟังปั๊ม · 21 พัดลมโรงเรือน · 22 เครื่องสีข้าว · 23 ถาดไข่ขนส่ง · 24 ลังผลไม้บนกระบะ |
| **D** เรดาร์จับผู้บุกรุก (กฎธรรมดา ไม่ใช้ AI) | 5 คอกกลางคืน · 25 ประตูโกดังกันขโมย |

<div class="cap">หน้าเว็บของแต่ละเรื่อง: project/web/idea_N.html · เกณฑ์ในโค้ดเริ่มต้นเป็นค่าสาธิต ทีมต้องตั้งใหม่ · ตัวอย่าง Edge AI 3 ตัวใน project/ai_future/ เป็นแนวทางอนาคต ไม่อยู่ในโจทย์</div>

---

## ตัวอย่างชุด A–B: จอบอร์ด + หน้าเว็บ (เรื่อง 1–4)

<div class="pairs">
<div class="pair"><b class="pt">1 · กล่องขนส่งผลไม้ห้องเย็น</b><div class="pi"><figure><img class="bd" src="img/project/board_idea_1.png" alt="จอบอร์ด ไอเดีย 1"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_1.png" alt="หน้าเว็บ ไอเดีย 1"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
<div class="pair"><b class="pt">2 · รถส่งผลผลิตถึงตลาด</b><div class="pi"><figure><img class="bd" src="img/project/board_idea_2.png" alt="จอบอร์ด ไอเดีย 2"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_2.png" alt="หน้าเว็บ ไอเดีย 2"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
<div class="pair"><b class="pt">3 · ไซโลข้าวเปลือก</b><div class="pi"><figure><img class="bd" src="img/project/board_idea_3.png" alt="จอบอร์ด ไอเดีย 3"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_3.png" alt="หน้าเว็บ ไอเดีย 3"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
<div class="pair"><b class="pt">4 · โรงเรือนอัจฉริยะ</b><div class="pi"><figure><img class="bd" src="img/project/board_idea_4.png" alt="จอบอร์ด ไอเดีย 4"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_4.png" alt="หน้าเว็บ ไอเดีย 4"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
</div>

---

## ตัวอย่างชุด B–D: จอบอร์ด + หน้าเว็บ (เรื่อง 5–8)

<div class="pairs">
<div class="pair"><b class="pt">5 · คอกปศุสัตว์กลางคืน</b><div class="pi"><figure><img class="bd" src="img/project/board_idea_5.png" alt="จอบอร์ด ไอเดีย 5"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_5.png" alt="หน้าเว็บ ไอเดีย 5"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
<div class="pair"><b class="pt">6 · หอถังน้ำของฟาร์ม</b><div class="pi"><figure><img class="bd" src="img/project/board_idea_6.png" alt="จอบอร์ด ไอเดีย 6"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_6.png" alt="หน้าเว็บ ไอเดีย 6"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
<div class="pair"><b class="pt">7 · หมอฟังปั๊ม/พัดลม</b><div class="pi"><figure><img class="bd" src="img/project/board_idea_7.png" alt="จอบอร์ด ไอเดีย 7"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_7.png" alt="หน้าเว็บ ไอเดีย 7"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
<div class="pair"><b class="pt">8 · เล้าไก่กันความเครียดฝูง</b><div class="pi"><figure><img class="bd" src="img/project/board_idea_8.png" alt="จอบอร์ด ไอเดีย 8"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_8.png" alt="หน้าเว็บ ไอเดีย 8"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
</div>


---

## ทุกเรื่องมีจอและหน้าเว็บของตัวเอง (เรื่อง 9–12)

<div class="pairs">
<div class="pair"><b class="pt">9 · โรงเพาะเห็ด <span style="color:#667">(ชุด A)</span></b><div class="pi"><figure><img class="bd" src="img/project/board_idea_9.png" alt="จอบอร์ด เรื่อง 9"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_9.png" alt="หน้าเว็บ เรื่อง 9"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
<div class="pair"><b class="pt">10 · ห้องบ่มกล้วย <span style="color:#667">(ชุด A)</span></b><div class="pi"><figure><img class="bd" src="img/project/board_idea_10.png" alt="จอบอร์ด เรื่อง 10"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_10.png" alt="หน้าเว็บ เรื่อง 10"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
<div class="pair"><b class="pt">11 · โรงอบลำไย <span style="color:#667">(ชุด A)</span></b><div class="pi"><figure><img class="bd" src="img/project/board_idea_11.png" alt="จอบอร์ด เรื่อง 11"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_11.png" alt="หน้าเว็บ เรื่อง 11"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
<div class="pair"><b class="pt">12 · โกดังเก็บเมล็ดพันธุ์ <span style="color:#667">(ชุด A)</span></b><div class="pi"><figure><img class="bd" src="img/project/board_idea_12.png" alt="จอบอร์ด เรื่อง 12"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_12.png" alt="หน้าเว็บ เรื่อง 12"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
</div>

---

## ทุกเรื่องมีจอและหน้าเว็บของตัวเอง (เรื่อง 13–16)

<div class="pairs">
<div class="pair"><b class="pt">13 · โกดังปุ๋ย <span style="color:#667">(ชุด A)</span></b><div class="pi"><figure><img class="bd" src="img/project/board_idea_13.png" alt="จอบอร์ด เรื่อง 13"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_13.png" alt="หน้าเว็บ เรื่อง 13"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
<div class="pair"><b class="pt">14 · ตู้เก็บยาและวัคซีนสัตว์ <span style="color:#667">(ชุด A)</span></b><div class="pi"><figure><img class="bd" src="img/project/board_idea_14.png" alt="จอบอร์ด เรื่อง 14"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_14.png" alt="หน้าเว็บ เรื่อง 14"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
<div class="pair"><b class="pt">15 · โรงเรือนไฮโดรโปนิกส์ <span style="color:#667">(ชุด A)</span></b><div class="pi"><figure><img class="bd" src="img/project/board_idea_15.png" alt="จอบอร์ด เรื่อง 15"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_15.png" alt="หน้าเว็บ เรื่อง 15"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
<div class="pair"><b class="pt">16 · พ่นหมอกโรงกล้วยไม้ <span style="color:#667">(ชุด B)</span></b><div class="pi"><figure><img class="bd" src="img/project/board_idea_16.png" alt="จอบอร์ด เรื่อง 16"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_16.png" alt="หน้าเว็บ เรื่อง 16"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
</div>

---

## ทุกเรื่องมีจอและหน้าเว็บของตัวเอง (เรื่อง 17–20)

<div class="pairs">
<div class="pair"><b class="pt">17 · เติมอากาศบ่อปลา <span style="color:#667">(ชุด B)</span></b><div class="pi"><figure><img class="bd" src="img/project/board_idea_17.png" alt="จอบอร์ด เรื่อง 17"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_17.png" alt="หน้าเว็บ เรื่อง 17"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
<div class="pair"><b class="pt">18 · พัดลมเล้าไก่ <span style="color:#667">(ชุด B)</span></b><div class="pi"><figure><img class="bd" src="img/project/board_idea_18.png" alt="จอบอร์ด เรื่อง 18"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_18.png" alt="หน้าเว็บ เรื่อง 18"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
<div class="pair"><b class="pt">19 · ประตูน้ำแปลงนา <span style="color:#667">(ชุด B)</span></b><div class="pi"><figure><img class="bd" src="img/project/board_idea_19.png" alt="จอบอร์ด เรื่อง 19"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_19.png" alt="หน้าเว็บ เรื่อง 19"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
<div class="pair"><b class="pt">20 · เครื่องให้อาหารปลา <span style="color:#667">(ชุด B)</span></b><div class="pi"><figure><img class="bd" src="img/project/board_idea_20.png" alt="จอบอร์ด เรื่อง 20"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_20.png" alt="หน้าเว็บ เรื่อง 20"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
</div>

---

## ทุกเรื่องมีจอและหน้าเว็บของตัวเอง (เรื่อง 21–24)

<div class="pairs">
<div class="pair"><b class="pt">21 · พัดลมโรงเรือนสั่นผิดปกติ <span style="color:#667">(ชุด C)</span></b><div class="pi"><figure><img class="bd" src="img/project/board_idea_21.png" alt="จอบอร์ด เรื่อง 21"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_21.png" alt="หน้าเว็บ เรื่อง 21"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
<div class="pair"><b class="pt">22 · เครื่องสีข้าวชุมชน <span style="color:#667">(ชุด C)</span></b><div class="pi"><figure><img class="bd" src="img/project/board_idea_22.png" alt="จอบอร์ด เรื่อง 22"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_22.png" alt="หน้าเว็บ เรื่อง 22"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
<div class="pair"><b class="pt">23 · ถาดไข่ระหว่างขนส่ง <span style="color:#667">(ชุด C)</span></b><div class="pi"><figure><img class="bd" src="img/project/board_idea_23.png" alt="จอบอร์ด เรื่อง 23"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_23.png" alt="หน้าเว็บ เรื่อง 23"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
<div class="pair"><b class="pt">24 · ลังผลไม้บนรถกระบะ <span style="color:#667">(ชุด C)</span></b><div class="pi"><figure><img class="bd" src="img/project/board_idea_24.png" alt="จอบอร์ด เรื่อง 24"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_24.png" alt="หน้าเว็บ เรื่อง 24"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
</div>

---

## ทุกเรื่องมีจอและหน้าเว็บของตัวเอง (เรื่อง 25)

<div class="pairs">
<div class="pair"><b class="pt">25 · ประตูโกดังกันขโมย <span style="color:#667">(ชุด D)</span></b><div class="pi"><figure><img class="bd" src="img/project/board_idea_25.png" alt="จอบอร์ด เรื่อง 25"><figcaption>จอบอร์ดใน BENTO Emulator</figcaption></figure><figure><img class="wb" src="img/project/web_idea_25.png" alt="หน้าเว็บ เรื่อง 25"><figcaption>หน้าเว็บ รับค่าจริงจากบอร์ดใน Emulator</figcaption></figure></div></div>
</div>

<div class="cap">หน้าเว็บของทุกเรื่องรวมไว้ที่ <a href="https://advance-innovation-centre-aic.github.io/aiot-development-for-smart-farm/project/web/index.html" target="_blank" rel="noopener">project/web/index.html</a> · โค้ดเริ่มต้นเท่านั้น ต้องต่อยอดเอง 3 อย่าง</div>

---

## ต่อยอดอย่างน้อย 3 อย่าง = หัวใจของคะแนน

| ข้อ | ทำอะไร | ตัวอย่าง |
|---|---|---|
| **1 · เกณฑ์ของเราเอง** (บังคับ) | วัดค่าจริง แล้วตั้งเกณฑ์ใหม่ พร้อมเหตุผล | วัดความชื้นห้อง 3 ครั้ง → เกณฑ์ไซโล 70 % |
| **2 · จอเป็นของผู้ใช้เรา** (บังคับ) | ชื่อ ป้าย ข้อความเตือน คำบน RGB matrix | จอคนขับรถส่งของ อ่านได้ใน 1 วินาที |
| **3 · ความสามารถเพิ่ม 1 อย่าง** | ฝั่งบอร์ด หรือฝั่งเว็บ | บอร์ด: นับครั้งที่เตือน · เว็บ: ปุ่มคำสั่งใหม่ · "บอร์ดเงียบเกิน 15 วินาที" · กราฟ · สรุป CSV |

<div class="warn">ใช้ชุดเริ่มต้นโดยไม่ต่อยอดเลย → หมวดต่อยอด (30 คะแนน) = 0</div>

---

## แบ่งหน้าที่ 2 คน

| ช่วงงาน | คนที่ 1 · ฝั่งบอร์ด | คนที่ 2 · ฝั่งเว็บ |
|---|---|---|
| คาบเสนอหัวข้อ | รันไฟล์ของเรื่องบนบอร์ดจริง | เปิด `web/idea_N.html?team=teamXX` เห็นค่าของกลุ่ม |
| ระหว่างสัปดาห์ | ต่อยอดข้อ 1–2 (+ ข้อ 3 ถ้าเลือกฝั่งบอร์ด) | ต่อยอดหน้าเว็บ (ข้อ 3 ถ้าเลือกฝั่งเว็บ) |
| วันนำเสนอ | สาธิตบอร์ด · ตอบเรื่องโค้ดบอร์ด | สาธิตเว็บ · ตอบเรื่องโค้ดเว็บ |

<div class="tip">

ฝั่งบอร์ด: **ไม่ใช้ vibe coding เขียนใหม่ทั้งไฟล์** — AI ทั่วไปไม่รู้จัก API ของบอร์ด<br>
ฝั่งเว็บ: vibe coding ได้ · วาง `MQTT_CONTRACT_th.md` ให้ก่อน · ทดสอบกับ `fake_board.html`

</div>

---

## เริ่มให้ได้ใน 10 นาที

<ol class="steps">
<li>คัดลอกไฟล์ของเรื่องเป็น <code>g&lt;เลขทีม&gt;_&lt;ชื่อโปรเจกต์&gt;.py</code></li>
<li>แก้ <code>WIFI_SSID</code> · <code>WIFI_PASS</code> · <code>TEAM</code></li>
<li>กด <b>Program to Device</b> → รันได้ก่อน แล้วค่อยแก้</li>
<li>มือถือเปิด <code>web/idea_N.html?team=teamXX</code></li>
</ol>

<div class="cols"><div class="c40">

<div class="phones"><figure><img src="img/project/web_chooser.png" alt="หน้าเลือกเรื่อง" style="height:300px"><figcaption>ไม่ใส่ ?idea= → หน้าเลือกไอเดีย</figcaption></figure></div>

</div><div>

<div class="think">

แก้ **ทีละ 1–2 จุด** แล้วรันดูทุกครั้ง<br>
เซฟสำเนาไฟล์ที่รันได้ก่อนแก้ทุกครั้ง

</div>

</div></div>

---

## ถ้าทำไม่ได้ จะไปต่ออย่างไร

| ถ้า… | ให้ทำแบบนี้ | ผลต่อคะแนน |
|---|---|---|
| แก้แล้วบอร์ด error | กลับไปไฟล์ที่รันได้ล่าสุด · แก้ทีละจุด | ไม่เสีย ถ้าวันนำเสนอรันได้ |
| ต่อยอดไม่ครบ 3 อย่าง | นำเสนอเท่าที่ทำได้ + แผนทำต่อ | หักเฉพาะหมวดต่อยอด |
| เว็บที่แก้เองพัง | เปิด `web/idea_N.html` ตัวเดิมของคอร์ส | หักเฉพาะส่วนเว็บที่ต่อยอด |
| เน็ต/broker ล่มวันนำเสนอ | สาธิตบอร์ดสด + คลิปสำรองฝั่งมือถือ | ไม่เสียส่วนเว็บ ถ้ามีคลิป |
| ติดจนไปต่อไม่ได้ | ส่งภาพจอบอร์ด + error ในกลุ่มแชท | ไม่เสีย |

<div class="goal">

**จุดตรวจวันที่ 3:** ส่งภาพจอบอร์ด + ภาพหน้าเว็บที่เห็นค่าของกลุ่ม ในกลุ่มแชทของคอร์ส

</div>

---

## ปฏิทิน 1 สัปดาห์

| เมื่อไร | คนที่ 1 · ฝั่งบอร์ด | คนที่ 2 · ฝั่งเว็บ |
|---|---|---|
| คาบเสนอหัวข้อ | เลือกเรื่อง · ลงชื่อ · รันไฟล์ของเรื่อง | เปิดเว็บเห็นค่า · กรอกใบเสนอโจทย์ |
| วันที่ 1–2 | ข้อ 1: วัดค่า ตั้งเกณฑ์ | ปรับ `IDEAS` ในหน้าเว็บ |
| วันที่ 3–4 | ข้อ 2: จอของผู้ใช้เรา · ส่งภาพจุดตรวจ | ข้อ 3 (ถ้าฝั่งเว็บ) · ส่งภาพจุดตรวจ |
| วันที่ 5–6 | ทดสอบ 3 กรณี · อัดคลิปสำรอง | สไลด์ 3–5 หน้า · ซ้อม |
| **18 ต.ค. 2569** | **สาธิตบอร์ด** | **สาธิตเว็บ** |

<div class="cap">นำเสนอ 5–6 นาที + ถาม-ตอบ 2 นาที · ถามทั้งสองคน</div>

---

## เกณฑ์ให้คะแนน (100)

| หมวด | คะแนน | ดีมาก |
|---|---|---|
| ปัญหา ผู้ใช้ และคุณค่า | 15 | ผู้ใช้ชัด ไม่ซ้ำ · ลดความเสียหายอะไร |
| **สิ่งที่ทีมต่อยอดเอง (3 อย่าง)** | **30** | เกณฑ์จากค่าที่วัดเอง · จอตรงผู้ใช้ · ฟีเจอร์เพิ่มทำงานจริง |
| บอร์ดทำงานครบในการสาธิต | 20 | ครบทุกส่วน + พิสูจน์กรณี "ไม่ควรเตือน" |
| MQTT + หน้าเว็บ | 15 | มือถือเห็นค่าจริง · สั่งกลับแล้วบอร์ดตอบ |
| สาธิต + ทดสอบ + ตอบคำถาม | 20 | ตรงเวลา · ทุกคนพูด · ผลทดสอบ 3 กรณี |

<div class="warn">broker.hivemq.com ไม่เข้ารหัส — <b>ห้ามส่งรหัสผ่านหรือข้อมูลส่วนตัว</b> · บอกให้ชัดว่าค่าไหนวัดจริง ค่าไหนจำลอง</div>
