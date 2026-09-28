#!/usr/bin/env python3
"""Build index.html, the landing page of the AIoT Development for Smart Farm site.

    python3 tools/gen_index.py            # writes ../index.html (the site root)
    python3 tools/gen_index.py --check    # exit 1 if index.html is stale

The page is generated from the tree, so re-running it after a new session lands
(a deck in slides/, examples in sN/, an app in sN/app/ or apps/) updates it.
What it reads:
  README.md        the schedule table (date + topic per session)
  sN/              sfN_*.py examples (title = text after " - " on line 1),
                   sf-sN-th.docx / sf-sN-th.worksheet.md, app/ files
  slides/          sf-session-0N.html (a card links it only if it exists)
  PROJECT_BRIEF_th.md/.docx, apps/

Links are relative, except source files (.py, .md), which point at their GitHub
blob page so they render with syntax colouring and Thai text intact.
"""
import html
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "index.html")
REPO = "https://github.com/Advance-Innovation-Centre-AIC/aiot-development-for-smart-farm"
BLOB = REPO + "/blob/main/"
SDK = "https://tesaiot.github.io/tesaiot-pse84-devkit-sdk/"
IDE = "https://ide.tesaiot.com/"
PROGRAMMER = "https://github.com/wiroon/TESAIoT_PSE84_Programmer/releases"
COLORS = {1: "#22d3ee", 2: "#a3c93a", 3: "#f59e0b", 0: "#c084fc"}

esc = html.escape


def exists(rel):
    return os.path.exists(os.path.join(ROOT, rel))


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


DATES = {1: "28 ก.ย.", 2: "5 ต.ค.", 3: "12 ต.ค.", 0: "26 ต.ค."}   # วันเรียน (แก้ที่นี่ถ้าเลื่อน)


def schedule():
    """{key: (date, title, detail)} from the README table; key = 1..n, 0 = project showcase.
    Reads rows like  | **Session 1** | หัวข้อ | ลงมือทำ |  and  | **Project Showcase** | ... |
    (the older  | 1 | วันที่ | **หัวข้อ:** รายละเอียด | ไฟล์ |  layout is still understood)."""
    out = {}
    if not exists("README.md"):
        return out
    for line in read("README.md").splitlines():
        m = re.match(r"^\|\s*\*\*(Session\s+(\d+)|Project Showcase)\*\*\s*\|\s*([^|]+?)\s*\|\s*([^|]+?)\s*\|\s*$", line)
        if m:
            key = int(m.group(2)) if m.group(2) else 0
            out[key] = (DATES.get(key, ""), m.group(3), re.sub(r"\*\*(.+?)\*\*", r"\1", m.group(4)))
            continue
        m = re.match(r"^\|\s*(\d+|นำเสนอ)\s*\|\s*([^|]+?)\s*\|\s*(.+?)\s*\|\s*[^|]*\|\s*$", line)
        if not m:
            continue
        key = 0 if m.group(1) == "นำเสนอ" else int(m.group(1))
        t = re.match(r"\*\*(.+?):?\*\*:?\s*(.*)", m.group(3))
        title, detail = (t.group(1).rstrip(":"), t.group(2)) if t else (m.group(3), "")
        out.setdefault(key, (m.group(2), title, re.sub(r"\*\*(.+?)\*\*", r"\1", detail)))
    return out


def example_title(rel):
    first = read(rel).splitlines()[0] if read(rel) else ""
    m = re.match(r"^#\s*\S+\s+-\s+(.*)$", first)
    return m.group(1).strip() if m else ""


def app_title(rel):
    text = read(rel)
    for line in text.splitlines()[:6]:
        m = re.match(r"^\s*(?:#|<!--)?\s*[\w.]+\s+-\s+(.*)$", line.strip())
        if m:
            return m.group(1).strip()
        m = re.match(r"^#\s+(.*)$", line)
        if m and rel.endswith(".md"):
            return m.group(1).strip()
    return ""


def link(href, label, cls="lk", ext=False):
    tgt = ' target="_blank" rel="noopener"' if ext else ""
    return f'<a class="{cls}" href="{esc(href)}"{tgt}>{label}</a>'


def session_card(n, meta):
    d = f"s{n}"
    date, title, detail = meta.get(n, (DATES.get(n, ""), f"Session {n}", ""))
    c = COLORS.get(n, "#22d3ee")
    deck = f"slides/sf-session-{n:02d}.html"
    parts = [f'<article class="card" style="--c:{c}">',
             f'<div class="cn">Session {n} · {esc(date)}</div>',
             f'<h3 class="ct">{esc(title)}</h3>',
             f'<p class="ch">{esc(detail)}</p>', '<div class="btns">']
    parts.append(link(deck, "🎞️ สไลด์", "btn") if exists(deck) else '<span class="btn off">🎞️ สไลด์ · เร็ว ๆ นี้</span>')
    ws_docx, ws_md = f"{d}/sf-s{n}-th.docx", f"{d}/sf-s{n}-th.worksheet.md"
    parts.append(link(ws_docx, "📝 ใบงาน .docx", "btn") if exists(ws_docx) else '<span class="btn off">📝 ใบงาน · เร็ว ๆ นี้</span>')
    if exists(ws_md):
        parts.append(link(BLOB + ws_md, "📄 ใบงาน (อ่านออนไลน์)", "btn", ext=True))
    parts.append("</div>")
    exs = sorted(f for f in os.listdir(os.path.join(ROOT, d))
                 if re.match(rf"sf{n}_\d+_.*\.py$", f)) if exists(d) else []
    if exs:
        parts.append('<div class="exs"><div class="exh">ตัวอย่างโค้ด (เปิดใน BENTO IDE แล้วกด Program to Device)</div>')
        for f in exs:
            rel = f"{d}/{f}"
            parts.append(f'<a class="ex" href="{esc(BLOB + rel)}" target="_blank" rel="noopener">'
                         f'<code>{esc(f)}</code><span>{esc(example_title(rel))}</span></a>')
        parts.append("</div>")
    else:
        parts.append('<div class="exs"><div class="exh">ตัวอย่างโค้ด · เร็ว ๆ นี้</div></div>')
    app = f"{d}/app"
    if exists(app):
        files = sorted(os.listdir(os.path.join(ROOT, app)))
        items = []
        for f in files:
            rel = f"{app}/{f}"
            if f.startswith(".") or os.path.isdir(os.path.join(ROOT, rel)) or f.endswith(".pyc"):
                continue
            if f.endswith(".html"):
                href, tag, ext = rel, "เปิดในเบราว์เซอร์", False
            else:
                href, tag, ext = BLOB + rel, "ดูไฟล์", True
            t = app_title(rel)
            tgt = ' target="_blank" rel="noopener"' if ext else ""
            items.append(f'<a class="ex app" href="{esc(href)}"{tgt}>'
                         f'<code>{esc(f)}</code><span>{esc(t) or tag}</span></a>')
        if items:
            parts.append('<div class="exs"><div class="exh">แอปบนโน้ตบุ๊ก + สัญญา MQTT</div>' + "".join(items) + "</div>")
    parts.append("</article>")
    return "\n".join(parts)


def presentation_card(meta):
    date, title, detail = meta.get(0, (DATES[0], "นำเสนอผลงาน", ""))
    c = COLORS[0]
    btns = []
    if exists("PROJECT_BRIEF_th.md"):
        btns.append(link(BLOB + "PROJECT_BRIEF_th.md", "📄 โจทย์โปรเจกต์ (อ่านออนไลน์)", "btn", ext=True))
    if exists("PROJECT_BRIEF_th.docx"):
        btns.append(link("PROJECT_BRIEF_th.docx", "📝 โจทย์โปรเจกต์ .docx", "btn"))
    if not btns:
        btns.append('<span class="btn off">โจทย์โปรเจกต์ · เร็ว ๆ นี้</span>')
    return (f'<article class="card" style="--c:{c}"><div class="cn">Project Showcase · {esc(date)}</div>'
            f'<h3 class="ct">{esc(title)}</h3><p class="ch">{esc(detail)}</p>'
            f'<div class="btns">{"".join(btns)}</div></article>')


def apps_block():
    if exists("apps") and any(not f.startswith(".") for f in os.listdir(os.path.join(ROOT, "apps"))):
        items = []
        for f in sorted(os.listdir(os.path.join(ROOT, "apps"))):
            if f.startswith("."):
                continue
            rel = f"apps/{f}"
            if os.path.isdir(os.path.join(ROOT, rel)):
                idx = f"{rel}/index.html"
                href = idx if exists(idx) else BLOB.replace("/blob/", "/tree/") + rel
            else:
                href = rel if f.endswith(".html") else BLOB + rel
            items.append(f'<a class="ex app" href="{esc(href)}"><code>{esc(f)}</code><span></span></a>')
        return "".join(items)
    return '<p class="muted">กำลังเตรียม — แอปเว็บ/มือถือ (PWA) ตัวอย่างสำหรับดูข้อมูลฟาร์มและรับแจ้งเตือน จะขึ้นที่นี่</p>'


CSS = """
:root{--ink:#eef2fb;--muted:#94a1bf;--faint:#6f7c9c;--bg:#080d18;--card:#0f1830;--card2:#121d38;
 --line:#1b2540;--teal:#22d3ee;--lime:#a3c93a;--btn:rgba(255,255,255,.04);--code:#cbd5f5;
 --glow1:#16243f;--glow2:#13213c}
@media (prefers-color-scheme: light){:root{--ink:#14203a;--muted:#4a5878;--faint:#6b7894;--bg:#f5f8f2;
 --card:#ffffff;--card2:#fbfdf9;--line:#dbe4d5;--teal:#0e7490;--lime:#4d7c0f;--btn:#f1f6ee;--code:#1e3a5f;
 --glow1:#e3f3dc;--glow2:#e0f2fe}}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:"IBM Plex Sans Thai","Noto Sans Thai",-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;
 background:radial-gradient(1100px 620px at 85% -8%,var(--glow1) 0,transparent 55%),
  radial-gradient(900px 560px at 8% 4%,var(--glow2) 0,transparent 50%),var(--bg);
 color:var(--ink);padding:48px 16px 64px;min-height:100vh;line-height:1.55;-webkit-font-smoothing:antialiased}
.wrap{max-width:1180px;margin:0 auto}
a{color:var(--teal)}
.eyebrow{color:var(--teal);font-weight:800;letter-spacing:.16em;text-transform:uppercase;font-size:.78rem}
h1{font-size:clamp(2rem,5vw,3.3rem);font-weight:850;margin-top:10px;line-height:1.12}
h1 .g{background:linear-gradient(90deg,var(--lime),var(--teal));-webkit-background-clip:text;background-clip:text;color:transparent}
.sub{color:var(--muted);margin-top:14px;font-size:clamp(1rem,1.5vw,1.2rem);max-width:860px}
.sub b{color:var(--ink)}
.meta{display:flex;gap:10px;flex-wrap:wrap;margin-top:18px}
.pill{font-size:.8rem;font-weight:700;padding:7px 14px;border-radius:999px;background:var(--btn);border:1px solid var(--line);color:var(--muted)}
.pill b{color:var(--ink)}
.block{margin:36px 0}
.block>h2{font-size:1.2rem;font-weight:800;border-left:4px solid var(--teal);padding-left:12px;margin:0 0 16px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(min(100%,340px),1fr));gap:18px}
.card{background:linear-gradient(180deg,var(--card2),var(--card));border:1px solid var(--line);
 border-top:4px solid var(--c,var(--teal));border-radius:18px;padding:18px 20px;display:flex;flex-direction:column;gap:8px}
.cn{font-weight:800;font-size:.8rem;letter-spacing:.04em;color:var(--c,var(--teal))}
.ct{font-weight:850;font-size:1.15rem}
.ch{color:var(--muted);font-size:.92rem}
.btns{display:flex;flex-wrap:wrap;gap:8px;margin-top:4px}
.btn{display:inline-block;font-size:.85rem;font-weight:700;padding:8px 13px;border-radius:10px;text-decoration:none;
 color:var(--ink);background:var(--btn);border:1px solid var(--line)}
.btn:hover{border-color:var(--c,var(--teal))}
.btn.off{color:var(--faint);border-style:dashed}
.exs{margin-top:6px}
.exh{font-size:.78rem;color:var(--faint);font-weight:700;margin:6px 0 2px}
.ex{display:flex;flex-direction:column;gap:2px;margin-top:6px;padding:9px 12px;background:var(--btn);border:1px solid var(--line);
 border-radius:12px;text-decoration:none;color:var(--ink);font-size:.86rem}
.ex:hover{border-color:var(--lime)}
.ex code{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:.78rem;color:var(--code);word-break:break-all}
.ex span{color:var(--muted)}
.diagram{background:#fbfdf9;border:1px solid var(--line);border-radius:18px;padding:10px}
.diagram img{width:100%;height:auto;display:block}
.cap{color:var(--faint);font-size:.82rem;margin-top:8px}
.steps{counter-reset:s;list-style:none;display:grid;gap:10px}
.steps li{counter-increment:s;display:flex;gap:12px;align-items:flex-start}
.steps li::before{content:counter(s);flex:0 0 34px;height:34px;border-radius:50%;background:#5e35b1;color:#fff;
 font-weight:800;display:flex;align-items:center;justify-content:center}
.note{background:var(--btn);border:1px solid var(--line);border-left:4px solid #f5a623;border-radius:12px;padding:10px 14px;
 color:var(--muted);font-size:.9rem;margin-top:12px}
.note b{color:var(--ink)}
.muted{color:var(--muted)}
.two{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,420px),1fr));gap:18px}
.foot{margin-top:44px;padding-top:18px;border-top:1px solid var(--line);color:var(--faint);font-size:.84rem}
@media (min-width:1600px){body{font-size:18px}.wrap{max-width:1400px}}
"""


def build():
    meta = schedule()
    sessions = sorted(int(m.group(1)) for m in (re.match(r"^s(\d+)$", d) for d in os.listdir(ROOT)) if m)
    for k in meta:
        if k and k not in sessions:
            sessions.append(k)
    sessions = sorted(set(sessions))
    cards = [session_card(n, meta) for n in sessions] + [presentation_card(meta)]
    diagram = ""
    if exists("slides/img/gateway_architecture.svg"):
        diagram = ('<section class="block"><h2>ภาพรวมระบบ: บอร์ดของเราคือ Smart IoT Gateway</h2>'
                   '<div class="diagram"><img src="slides/img/gateway_architecture.svg" '
                   'alt="เซนเซอร์ไร้สายในแปลง ส่งค่าผ่าน MQTT ไปยัง broker; TESAIoT Dev Kit ตัดสิน แสดงผล และสั่ง PLC/รีเลย์ Wi-Fi คุมปั๊ม วาล์ว พัดลม; แอปของทีมดูทุกอย่างบนมือถือ"></div>'
                   '<p class="cap">Session 1 ใช้ลูกบิดบนบอร์ดแทนเซนเซอร์ไร้สาย และไฟสีฟ้าแทน PLC คุมปั๊ม · Session 2 ต่อจริงผ่าน MQTT</p></section>')
    setup = f"""<section class="block"><h2>เตรียมบอร์ดครั้งแรก (ราว 20 นาที)</h2>
<div class="two"><div class="card" style="--c:#5e35b1"><ol class="steps">
<li><div><b>ติดตั้ง TESAIoT PSE84 Programmer</b> — {link(PROGRAMMER, "หน้า Releases", ext=True)} · macOS: <code>…_universal.dmg</code> · Windows: <code>…_x64-setup.exe</code> — เปิดค้างไว้ แล้วสลับเป็นโหมด <b>Remote</b></div></li>
<li><div><b>เสียบ USB-C</b> เข้าพอร์ต <b>KitProg3</b> ของบอร์ด (ใช้สายที่ส่งข้อมูลได้)</div></li>
<li><div>เปิด Chrome หรือ Edge → {link(IDE, "ide.tesaiot.com", ext=True)} → <b>Welcome</b> → <b>TESAIoT Dev Kit</b> → <b>Flash this board →</b> → จับคู่กับ Programmer → <b>Flash v2.4.1</b></div></li>
<li><div>จอบอร์ดขึ้น <b>v2.4.1</b> → กด <b>Connect</b> ใน IDE</div></li>
</ol></div>
<div class="card" style="--c:#f5a623"><div class="ct">แก้ปัญหาเร็ว</div>
<p class="ch">เครื่องเตือนตอนเปิดแอป → เรียกผู้สอน · คอมไม่เห็นบอร์ด → เปลี่ยนสาย/พอร์ต USB · จอดำหลังแฟลชหรือรันโค้ด → กด RESET หนึ่งครั้ง ยังดำให้ถอดสายแล้วเสียบใหม่ · <b>SW2 บนฐานบอร์ดคือสวิตช์ตัดไฟ ห้ามโยก</b></p>
<div class="ct" style="margin-top:8px">เรียนต่อที่บ้าน: BENTO Emulator</div>
<p class="ch">ทุกไฟล์ในคอร์สรันใน BENTO Emulator ได้ (ในเมนูของ BENTO IDE) ใช้แผง <b>TESAIoT DEV KIT</b> (ลูกบิด VR1–VR4 ปุ่มคู่ของฐานบอร์ด จอไฟ RGB) กับแผง ENVIRONMENT/TILT แทนบอร์ดจริง — ค่าเซนเซอร์ใน Emulator เป็นค่าจำลอง</p></div></div></section>"""
    apps = f'<section class="block"><h2>แอปของทีม (เว็บ/มือถือ)</h2>{apps_block()}</section>'
    learn = f"""<section class="block"><h2>รู้จักบอร์ดให้ลึกขึ้น</h2><div class="card" style="--c:var(--teal)">
<div class="ct">TESAIoT Dev Kit SDK</div><p class="ch">ฮาร์ดแวร์ทั้งบอร์ด · ซอฟต์แวร์ · ความปลอดภัย · Edge AI · เอกสาร SDK</p>
<div class="btns">{link(SDK, "เปิดเว็บ SDK ↗", "btn", ext=True)}{link(REPO, "repo ของคอร์สนี้ ↗", "btn", ext=True)}</div></div></section>"""
    page = f"""<!DOCTYPE html>
<html lang="th">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="dark light">
<title>AIoT Development for Smart Farm</title>
<meta name="description" content="AIoT Development for Smart Farm — Intensive Course: เปลี่ยน TESAIoT Dev Kit ให้เป็น Smart IoT Gateway ของฟาร์ม ด้วย BENTO IDE และ BENTO Emulator">
<style>{CSS}</style>
</head>
<body><div class="wrap">
<header>
<div class="eyebrow">Intensive Course · TESAIoT Dev Kit · MicroPython</div>
<h1>AIoT Development for <span class="g">Smart Farm</span></h1>
<p class="sub">เปลี่ยนบอร์ดเล็ก ๆ หนึ่งตัวให้เป็น <b>Smart IoT Gateway</b> ของฟาร์ม: อ่านอากาศในโรงเรือน ดูแลความชื้นดิน เฝ้าแท็งก์น้ำ ส่งเสียงเตือนเมื่อพืชเริ่มเครียด แล้วส่งข้อมูลขึ้นอินเทอร์เน็ตให้<b>แอปบนมือถือของคุณเอง</b>แสดงผลและแจ้งเตือนแบบเรียลไทม์</p>
<div class="meta"><span class="pill">บอร์ด <b>TESAIoT Dev Kit</b></span><span class="pill">firmware <b>v2.4.1</b></span><span class="pill"><b>BENTO IDE</b> + <b>BENTO Emulator</b></span><span class="pill">ลงมือทำตั้งแต่นาทีแรก</span></div>
</header>
{diagram}
{setup}
<section class="block"><h2>เส้นทางการเรียน</h2><div class="grid">
{chr(10).join(cards)}
</div></section>
{apps}
{learn}
<footer class="foot">อินโฟกราฟิกและภาพหน้าจอทำขึ้นสำหรับคอร์สนี้ · เครดิตภาพและวิดีโอทั้งหมดอยู่ที่สไลด์ท้ายของแต่ละ Session · หน้านี้สร้างด้วย <code>tools/gen_index.py</code></footer>
</div></body></html>
"""
    return page


if __name__ == "__main__":
    page = build()
    if "--check" in sys.argv[1:]:
        cur = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
        sys.exit(0 if cur == page else 1)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(page)
    print("wrote", os.path.relpath(OUT, os.getcwd()))
