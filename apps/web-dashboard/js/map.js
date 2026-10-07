// map.js - ผังฟาร์มสด: โหนดเซนเซอร์ในแปลง -> Dev Kit (Smart IoT Gateway) -> PLC WiFi -> ปั๊ม
//
// อ่านจาก 3 แหล่งตามสัญญา: field/<เซนเซอร์> (3.5) · plc/state (3.6) · สรุปของ Gateway ใน telemetry (3.7)
// แต่ละกล่องมีจุดสถานะ: เขียว = ได้ยินภายใน 15 วิ · เหลือง = เงียบเกิน 15 วิ · เทา = ยังไม่เคยได้ยิน
// เส้นเชื่อมกะพริบทุกครั้งที่มีข้อความวิ่งผ่านจริง ไม่ใช่ภาพประกอบเฉย ๆ

import { STALE_S, WHY_TH } from "./config.js";
import { el, icon, ago, num, short } from "./ui.js";

const LIVE_MS = STALE_S * 1000;

function stateOf(t, now) {
  if (!t) return "none";
  return now - t < LIVE_MS ? "live" : "stale";
}

function row(label) {
  const v = el("b", { text: "—" });
  return { node: el("div", { class: "gm-row" }, el("span", { text: label }), v), v };
}

export class GatewayMap {
  constructor(box) {
    this.box = box;
    this.fields = {};          // ชื่อเซนเซอร์ -> { data, t, dom }
    this.plc = null;           // { data, t }
    this.plcCmd = null;        // { data, t } คำสั่งล่าสุดที่ Gateway ส่งให้ PLC
    this.gw = null;            // { data, t } telemetry ที่ by = "gateway"
    this.board = null;         // { t, format } telemetry ใบล่าสุด (ไฟล์ไหนก็ได้)
    this.appCmd = null;        // { text, t } คำสั่งล่าสุดบนหัวข้อ cmd
    this.note = null;          // { text, t, level } เหตุการณ์ล่าสุดจาก Gateway
    this.build();
  }

  build() {
    const fieldCol = el("div", { class: "gm-field" });
    this.fieldCol = fieldCol;
    this.fieldEmpty = el("div", { class: "gm-node gm-ghost", "data-state": "none" },
      el("div", { class: "gm-title" }, icon("node"), el("span", { text: "โหนดเซนเซอร์" })),
      el("p", { class: "gm-hint", text: "ยังไม่ได้ยิน field/* (เปิด PLC Simulator ใน farm_web.html หรือบอร์ดแปลง sf2_07)" }));
    fieldCol.append(this.fieldEmpty);

    const gwRows = [row("ออโต้"), row("ดิน / ถัง"), row("ปั๊มตาม PLC"), row("คำสั่งจากแอป")];
    [this.gwAuto, this.gwView, this.gwPump, this.gwCmd] = gwRows.map((r) => r.v);
    this.gwFile = el("span", { class: "chip", text: "—" });
    this.gwNote = el("p", { class: "gm-note" });
    this.gwNode = el("div", { class: "gm-node gm-gw", "data-state": "none" },
      el("div", { class: "gm-title" }, icon("chip"), el("span", { text: "Dev Kit · Smart IoT Gateway" }), el("i", { class: "dot" })),
      el("div", { class: "gm-sub" }, el("span", { text: "ไฟล์บนบอร์ด (เดาจากข้อความ)" }), this.gwFile),
      gwRows.map((r) => r.node), this.gwNote);

    const plcRows = [row("ปั๊ม"), row("เหลืออีก"), row("เหตุผล (why)"), row("คำสั่งล่าสุด")];
    [this.plcPump, this.plcLeft, this.plcWhy, this.plcLast] = plcRows.map((r) => r.v);
    this.plcAge = el("span", { class: "gm-age", text: "ยังไม่เคยได้ยิน" });
    this.plcNode = el("div", { class: "gm-node gm-plc", "data-state": "none" },
      el("div", { class: "gm-title" }, icon("plc"), el("span", { text: "PLC WiFi (โรงสูบ)" }), el("i", { class: "dot" })),
      plcRows.map((r) => r.node), this.plcAge);

    this.pumpText = el("b", { class: "gm-pump-text", text: "ไม่ทราบ" });
    this.pumpSub = el("span", { class: "gm-age", text: "" });
    this.pumpNode = el("div", { class: "gm-node gm-pump", "data-on": "unknown" },
      el("div", { class: "gm-title" }, el("span", { text: "ปั๊ม / วาล์ว" })),
      icon("pump", "icon gm-impeller"), this.pumpText, this.pumpSub);

    this.links = {
      field: this.link("field/+", "ค่าจากแปลง"),
      plc: this.link("plc/cmd · plc/state", "สั่งปั๊ม · สถานะจริง"),
      relay: this.link("รีเลย์", ""),
    };
    this.box.replaceChildren(
      el("div", { class: "gmap" }, fieldCol, this.links.field.node, this.gwNode,
        this.links.plc.node, this.plcNode, this.links.relay.node, this.pumpNode));
  }

  link(label, sub) {
    const node = el("div", { class: "gm-link", "aria-hidden": "true" },
      el("span", { class: "gm-wire" }, el("span", { class: "gm-pulse" })),
      el("small", { text: label }), sub ? el("small", { class: "gm-link-sub", text: sub }) : null);
    return { node, timer: null };
  }

  pulse(name, reverse = false) {
    const l = this.links[name];
    l.node.classList.remove("go", "rev");
    void l.node.offsetWidth;                           // เริ่มแอนิเมชันใหม่ทุกครั้ง
    l.node.classList.add("go");
    if (reverse) l.node.classList.add("rev");
    clearTimeout(l.timer);
    l.timer = setTimeout(() => l.node.classList.remove("go", "rev"), 900);
  }

  // ---- ข้อความเข้า ----
  onField(sensor, data, t) {
    let f = this.fields[sensor];
    if (!f) {
      const value = el("b", { class: "gm-value", text: "—" });
      const fill = el("span");
      const age = el("span", { class: "gm-age" });
      const name = el("span", { text: sensor });
      const dom = el("div", { class: "gm-node gm-sensor", "data-state": "none" },
        el("div", { class: "gm-title" }, icon(sensor.startsWith("tank") ? "tank" : sensor.startsWith("soil") ? "soil" : "node"), name, el("i", { class: "dot" })),
        value, el("div", { class: "bar" }, fill), age);
      f = this.fields[sensor] = { dom, value, fill, age, name };
      this.fieldEmpty.remove();
      this.fieldCol.append(dom);
    }
    f.data = data;
    f.t = t;
    const v = num(data.value, 0, 100);
    f.value.textContent = v === null ? "—" : v + " " + short(data.unit || "", 4);
    f.fill.style.width = (v === null ? 0 : v) + "%";
    f.name.textContent = typeof data.node === "string" ? short(data.node, 16) : sensor;
    this.pulse("field");
  }

  onPlcState(data, t) {
    this.plc = { data, t, until: t + (num(data.left_s, 0, 3600) || 0) * 1000 };
    this.pulse("plc", true);
  }

  onPlcCmd(data, t) {
    this.plcCmd = { data, t };
    this.pulse("plc");
  }

  onTelemetry(data, t, format) {
    this.board = { t, format };
    if (data.by === "gateway") this.gw = { data, t };
    if ("pump" in data) this.lastTelPump = data.pump === 1 ? 1 : data.pump === 0 ? 0 : undefined;
  }

  onAppCmd(text, t) {
    this.appCmd = { text, t };
  }

  onEvent(data, t) {
    const e = data.event;
    if (e === "plc_lost") this.note = { text: "Gateway แจ้ง: PLC เงียบเกิน 15 วิ", t, level: "crit" };
    else if (e === "auto_water") this.note = { text: "Gateway รดน้ำเองเพราะดินแห้ง (" + (num(data.soil, 0, 100) ?? "?") + " %)", t, level: "warn" };
    else if (e === "pump") this.note = { text: "ปั๊ม" + (data.pump ? "เดิน" : "หยุด") + " · " + (WHY_TH[data.why] || short(data.why || "", 16)), t, level: "info" };
  }

  // ---- วาดใหม่ทุก 1 วิ (อายุข้อความ นับถอยหลัง สถานะเงียบ) ----
  render(now = Date.now()) {
    for (const f of Object.values(this.fields)) {
      f.dom.dataset.state = stateOf(f.t, now);
      f.age.textContent = "n " + (num(f.data.n, 0, 1e9) ?? "?") + " · " + ago(f.t, now);
    }

    // Gateway : ถ้าได้ยินสรุป by=gateway ใช้ค่านั้น ไม่งั้นบอกว่าบอร์ดรันไฟล์อื่นอยู่
    const b = this.board;
    this.gwNode.dataset.state = stateOf(b && b.t, now);
    this.gwFile.textContent = b ? b.format : "ยังไม่ได้ยินบอร์ด";
    if (this.gw && now - this.gw.t < LIVE_MS * 4) {
      const d = this.gw.data;
      this.gwAuto.textContent = d.auto === 1 ? "เปิด" : d.auto === 0 ? "ปิด" : "—";
      this.gwView.textContent = (num(d.soil, 0, 100) ?? "—") + " / " + (num(d.tank, 0, 100) ?? "—") + " %";
      this.gwPump.textContent = d.pump === 1 ? "เดิน" : d.pump === 0 ? "หยุด" : "ไม่ทราบ (PLC เงียบ)";
    } else {
      this.gwAuto.textContent = "—";
      this.gwView.textContent = "—";
      this.gwPump.textContent = b ? "ไม่มีสรุปจาก sf2_06" : "—";
    }
    this.gwCmd.textContent = this.appCmd ? short(this.appCmd.text, 30) + " · " + ago(this.appCmd.t, now) : "—";
    const note = this.note && now - this.note.t < 120000 ? this.note : null;
    this.gwNote.textContent = note ? note.text + " · " + ago(note.t, now) : "";
    this.gwNote.dataset.level = note ? note.level : "";

    // PLC
    const p = this.plc;
    const pState = stateOf(p && p.t, now);
    this.plcNode.dataset.state = pState === "stale" ? "lost" : pState;
    let pumpOn = null;
    if (p) {
      const left = Math.max(0, Math.ceil((p.until - now) / 1000));
      if (pState === "live") pumpOn = p.data.pump === 1 ? 1 : 0;
      this.plcPump.textContent = pumpOn === null ? "ไม่ทราบ" : pumpOn ? "เดิน" : "หยุด";
      this.plcLeft.textContent = p.data.pump === 1 && pState === "live" ? left + " วิ" : "—";
      this.plcWhy.textContent = WHY_TH[p.data.why] || short(p.data.why || "—", 16);
      this.plcAge.textContent = pState === "stale" ? "PLC หลุด! เงียบ " + ago(p.t, now).replace("ที่แล้ว", "") : "n " + (num(p.data.n, 0, 1e9) ?? "?") + " · " + ago(p.t, now);
    }
    const c = this.plcCmd;
    this.plcLast.textContent = c ? (c.data.pump ? "เปิด " + (num(c.data.sec, 0, 999) ?? 10) + " วิ" : "ปิด") + " · " + ago(c.t, now) : "—";

    // ปั๊ม : เชื่อ PLC ก่อน (ความจริงอยู่ที่ PLC) ถ้าไม่มี PLC ใช้ pump จาก telemetry
    if (pumpOn === null && b && this.lastTelPump !== undefined && now - b.t < LIVE_MS) pumpOn = this.lastTelPump;
    this.pumpNode.dataset.on = pumpOn === null ? "unknown" : pumpOn ? "1" : "0";
    this.pumpText.textContent = pumpOn === null ? "ไม่ทราบ" : pumpOn ? "กำลังสูบน้ำ" : "หยุด";
    this.pumpSub.textContent = pumpOn && p && pState === "live" ? "เหลือ " + Math.max(0, Math.ceil((p.until - now) / 1000)) + " วิ" :
      pumpOn === null ? "ยังไม่รู้สถานะจริง" : p ? "ตาม PLC" : "ตาม telemetry";
  }
}
