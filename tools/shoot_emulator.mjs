// shoot_emulator.mjs — ถ่ายภาพจอจริงจาก BENTO Emulator ของตัวอย่างคอร์ส Smart Farm
//
//   node short_courses/smart_farm/tools/shoot_emulator.mjs [options] [glob ...]
//
//   glob     พาธเทียบกับโฟลเดอร์ smart_farm/ (ค่าเริ่มต้น "s1/sf1_*.py")
//            เช่น "s2/sf2_*.py" หรือ "s*/sf*_0[1-3]_*.py"
//   --base URL   ใช้ emulator server ที่เปิดอยู่แล้ว (ค่าเริ่มต้น http://localhost:8765)
//                ถ้า URL นั้นไม่ตอบ สคริปต์จะเปิด server เอง (python3 -m http.server
//                ในโฟลเดอร์ bento-emulator/web) บนพอร์ตว่างตัวแรกตั้งแต่ 8765 ขึ้นไป
//                แล้วปิด server ตัวนั้นเองเมื่อเสร็จ — ไม่แตะ process ที่ไม่ได้เปิดเอง
//   --out DIR    โฟลเดอร์ปลายทาง (ค่าเริ่มต้น smart_farm/slides/img/emu)
//   --shots FILE ไฟล์ฉาก (ค่าเริ่มต้น tools/emu_shots.json)
//
// ต่อหนึ่งฉาก ได้ 3 ภาพ:
//   <ไฟล์>__<ฉาก>.png       จอบอร์ด 800x480 (canvas)
//   <ไฟล์>__<ฉาก>_kit.png   แผง TESAIoT DEV KIT (ลูกบิด VR1-VR4, SW4/SW5, จอไฟ RGB 16x8)
//   <ไฟล์>__<ฉาก>_full.png  จอ + แผงฮาร์ดแวร์จำลองทั้งแผ่น
// และ manifest.json หนึ่งรายการต่อฉาก: { file, shot, status, images, console }
//
// ฉาก (emu_shots.json) — key คือชื่อไฟล์ .py (หรือ "*" = ค่าเริ่มต้น):
//   [{ "name": "normal", "env": {"temp":27,"hum":65,"pressure":1009},
//      "pots": [0.3, 0.5, 0.9, 0.5],      // สัดส่วน 0..1 ของ VR1..VR4 (null = ไม่แตะ)
//      "tilt": [0, 0],                     // -1..1 เอียงซ้าย/ขวา, หน้า/หลัง
//      "press": ["SW4"],                   // กดแล้วปล่อย ก่อนรอ
//      "hold": ["SW4"],                    // กดค้างไว้ตลอดช่วงรอและตอนถ่าย
//      "shake": 700,                       // เขย่า ms
//      "tap": [[300, 150]],                // แตะจอที่พิกัด (x, y) ของจอ 800x480 เช่น แท็บของ Tabview
//      "wait": 6000 }]                     // รอกี่ ms หลังตั้งค่าแล้วค่อยถ่าย
//   ฉากในไฟล์เดียวกันเดินต่อกันใน "การรันครั้งเดียว" (โปรแกรมไม่ถูกรีสตาร์ต)
//   ฉากแรกมี "pre" ได้ = สภาพแผงก่อนกด Run (เช่น ตั้งลูกบิดไว้ก่อน)
//   ทุกไฟล์รันด้วยโปรไฟล์บอร์ด TESAIoT Dev Kit (sensors.board("devkit"))
//   ค่าเซนเซอร์ใน Emulator เป็นค่าจำลอง ไม่ใช่ค่าจากบอร์ดจริง
import { createRequire } from "module";
import { readFileSync, writeFileSync, mkdirSync, existsSync, globSync } from "fs";
import { join, dirname, basename, relative, resolve } from "path";
import { fileURLToPath } from "url";
import { spawn } from "child_process";
import net from "net";

const TOOLS = dirname(fileURLToPath(import.meta.url));
const COURSE = dirname(TOOLS);                                  // smart_farm/
const WS = resolve(COURSE, "../../..");                          // workspace root
const EMU = join(WS, "BENTO_IDE/bento-emulator");

const args = process.argv.slice(2);
const opt = { base: "http://localhost:8765", out: join(COURSE, "slides/img/emu"),
              shots: join(TOOLS, "emu_shots.json"), globs: [] };
for (let i = 0; i < args.length; i++) {
    const a = args[i];
    if (a === "--base") opt.base = args[++i];
    else if (a === "--out") opt.out = resolve(args[++i]);
    else if (a === "--shots") opt.shots = resolve(args[++i]);
    else if (a === "-h" || a === "--help") {
        console.log(readFileSync(fileURLToPath(import.meta.url), "utf8").split("\nimport ")[0]);
        process.exit(0);
    } else opt.globs.push(a);
}
if (!opt.globs.length) opt.globs.push("s1/sf1_*.py");

const files = [...new Set(opt.globs.flatMap((g) => globSync(g, { cwd: COURSE })))]
    .filter((f) => f.endsWith(".py") && !f.includes("__pycache__"))
    .sort();
if (!files.length) {
    console.error("shoot_emulator: glob ไม่เจอไฟล์ .py:", opt.globs.join(" "));
    process.exit(2);
}
const SHOTS = existsSync(opt.shots) ? JSON.parse(readFileSync(opt.shots, "utf8")) : {};
const DEFAULT_SHOT = [{ name: "run", wait: 8000 }];

// playwright มาจาก node_modules ของ bento-emulator — ไม่ติดตั้งซ้ำ
const require = createRequire(join(EMU, "package.json"));
const { chromium } = require("playwright");

// ---------- server: ใช้ของที่เปิดอยู่ หรือเปิดเองแล้วปิดเอง ----------
async function isEmulator(url) {
    try {
        const r = await fetch(url + "/index.html", { signal: AbortSignal.timeout(2500) });
        return r.ok && (await r.text()).includes("emu-canvas");
    } catch { return false; }
}
function portFree(port) {
    return new Promise((ok) => {
        const s = net.createServer();
        s.once("error", () => ok(false));
        s.once("listening", () => s.close(() => ok(true)));
        s.listen(port, "127.0.0.1");
    });
}
let server = null;
async function ensureServer() {
    if (await isEmulator(opt.base)) return opt.base;
    for (let port = 8765; port < 8800; port++) {
        if (!(await portFree(port))) continue;
        server = spawn("python3", ["-m", "http.server", String(port), "--bind", "127.0.0.1"],
                       { cwd: join(EMU, "web"), stdio: "ignore" });
        const url = `http://127.0.0.1:${port}`;
        for (let i = 0; i < 40; i++) {
            if (await isEmulator(url)) {
                console.log(`server: started pid ${server.pid} on ${url}`);
                return url;
            }
            await new Promise((r) => setTimeout(r, 250));
        }
        stopServer();
        throw new Error("เปิด emulator server ไม่ขึ้นที่ " + url);
    }
    throw new Error("ไม่มีพอร์ตว่างช่วง 8765-8799");
}
function stopServer() {
    if (server && server.exitCode === null) {
        server.kill("SIGTERM");
        console.log(`server: stopped pid ${server.pid}`);
    }
    server = null;
}
process.on("SIGINT", () => { stopServer(); process.exit(130); });

// ---------- ตัวช่วยควบคุมแผงฮาร์ดแวร์จำลอง ----------
const post = (page, msg) => page.evaluate((m) => window.postMessage({ bento: true, ...m }, "*"), msg);
const BTN = { SW4: 0, SW5: 1 };
const PROFILE_LINE = 'import sensors as _sf_p; _sf_p.board("devkit"); del _sf_p\n';

async function setPot(page, idx, frac) {
    const knob = page.locator(`.knob[data-pot="${idx}"]`);
    await knob.scrollIntoViewIfNeeded();
    const b = await knob.boundingBox();
    const a = (Math.min(1, Math.max(0, frac)) * 270 - 135) * Math.PI / 180;
    const r = Math.min(b.width, b.height) * 0.4;
    const x = b.x + b.width / 2 + r * Math.sin(a);
    const y = b.y + b.height / 2 - r * Math.cos(a);
    await page.mouse.move(x, y);
    await page.mouse.down();
    await page.mouse.up();
}
async function buttonDown(page, name) {
    const el = page.locator(`.hw-devbtn[data-devbtn="${BTN[name]}"]`);
    await el.scrollIntoViewIfNeeded();
    await el.dispatchEvent("pointerdown", { pointerId: 10 + BTN[name], bubbles: true });
}
async function buttonUp(page, name) {
    await page.locator(`.hw-devbtn[data-devbtn="${BTN[name]}"]`)
        .dispatchEvent("pointerup", { pointerId: 10 + BTN[name], bubbles: true });
}
async function applyShot(page, s) {
    if (s.env) await post(page, { cmd: "hw", patch: s.env });
    if (s.pots) for (let i = 0; i < s.pots.length; i++)
        if (s.pots[i] !== null && s.pots[i] !== undefined) await setPot(page, i, s.pots[i]);
    if (s.tilt) await post(page, { cmd: "tilt", dx: s.tilt[0], dy: s.tilt[1] });
    for (const b of s.press || []) {
        await buttonDown(page, b); await page.waitForTimeout(350); await buttonUp(page, b);
    }
    for (const b of s.hold || []) await buttonDown(page, b);
    if (s.shake) await post(page, { cmd: "shake", ms: s.shake });
    for (const [x, y] of s.tap || []) {                 // แตะจอ (พิกัดจอ 800x480)
        const c = page.locator("#emu-canvas");
        const b = await c.boundingBox();
        await c.click({ position: { x: x * b.width / 800, y: y * b.height / 480 } });
        await page.waitForTimeout(250);
    }
}

// ---------- main ----------
mkdirSync(opt.out, { recursive: true });
const manifest = [];
let browser;
try {
    const base = await ensureServer();
    browser = await chromium.launch();
    const page = await browser.newPage({ viewport: { width: 1900, height: 1500 } });
    for (const rel of files) {
        const name = basename(rel, ".py");
        const src = readFileSync(join(COURSE, rel), "utf8");
        const shots = SHOTS[name] || SHOTS["*"] || DEFAULT_SHOT;
        await page.goto(base + "/index.html");
        await page.waitForFunction(() => window.BentoEmulator && window.BentoEmulator.ready,
                                   null, { timeout: 90000 });
        await post(page, { cmd: "hwpanel", value: true });
        await page.waitForTimeout(300);
        if (shots[0].pre) await applyShot(page, shots[0].pre);   // สภาพก่อนเริ่มรัน
        // โปรไฟล์ TESAIoT Dev Kit ของ emulator คือ sensors.board("devkit") — เติมหนึ่งบรรทัด
        // หน้าโปรแกรม (เลขบรรทัดใน traceback จึงเลื่อนไป 1) ไม่แก้ไฟล์ของนิสิต
        await page.evaluate((c) => {
            document.getElementById("emu-console").textContent = "";
            window.BentoEmulator.run(c);
        }, PROFILE_LINE + src);
        for (const s of shots) {
            await applyShot(page, s);
            await page.waitForTimeout(s.wait ?? 6000);
            const pane = await page.evaluate(() => document.getElementById("emu-console").textContent);
            const err = /Traceback|Error:/.test(pane);
            const running = await page.evaluate(() => window.BentoEmulator.running);
            const stem = `${name}__${s.name}`;
            await page.locator("#emu-canvas").screenshot({ path: join(opt.out, stem + ".png") });
            const kit = page.locator("#hw-devkit");
            await kit.scrollIntoViewIfNeeded();
            await kit.screenshot({ path: join(opt.out, stem + "_kit.png") });
            await page.evaluate(() => document.querySelector(".right").scrollTo(0, 0));
            const a = await page.locator(".device-shell").boundingBox();
            const b = await page.locator("#hw-panel").boundingBox();
            const x0 = Math.max(0, Math.min(a.x, b.x) - 12), y0 = Math.max(0, a.y - 12);
            await page.screenshot({ path: join(opt.out, stem + "_full.png"), clip: {
                x: x0, y: y0,
                width: Math.max(a.x + a.width, b.x + b.width) + 40 - x0,
                height: b.y + b.height + 12 - y0 } });
            for (const b of s.hold || []) await buttonUp(page, b);
            const entry = { file: rel, shot: s.name,
                            status: err ? "ERROR" : running ? "RUNS" : "DONE",
                            images: [stem + ".png", stem + "_kit.png", stem + "_full.png"],
                            console: pane.trim().split("\n").slice(-4).join(" | ").slice(0, 400) };
            manifest.push(entry);
            console.log(`${entry.status.padEnd(5)} ${rel} :: ${s.name}`);
            if (err) { console.log("   ", entry.console); break; }
        }
        if (await page.evaluate(() => window.BentoEmulator.running)) {
            await page.evaluate(() => window.BentoEmulator.stop());
            await page.waitForTimeout(800);
        }
    }
} finally {
    if (browser) await browser.close();
    stopServer();
}
writeFileSync(join(opt.out, "manifest.json"), JSON.stringify(
    { shot_at: new Date().toISOString(), note: "BENTO Emulator — ค่าเซนเซอร์เป็นค่าจำลอง", shots: manifest },
    null, 1));
const bad = manifest.filter((m) => m.status === "ERROR");
console.log(`shots: ${manifest.length}, errors: ${bad.length} -> ${relative(process.cwd(), opt.out) || "."}`);
process.exit(bad.length ? 1 : 0);
