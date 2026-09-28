// charts.js - กราฟแนวโน้มสดด้วย Chart.js (window.Chart จาก CDN ล็อกรุ่น + SRI ใน index.html)
//
// แต่ละกราฟมาจาก CHARTS ใน config.js · แกน x เป็นเวลา (ms) แบบ linear จึงไม่ต้องโหลดตัวแปลงวันที่เพิ่ม
// หนึ่งกราฟ = หนึ่งหน่วย (ห้ามแกน y สองแกน) · สีเส้นอ่านจากตัวแปร CSS จึงเปลี่ยนตามธีมสว่าง/มืดเอง

import { CHARTS, CHART_WINDOWS_MIN } from "./config.js";
import { el, hhmmss, num } from "./ui.js";

const MAX_POINTS = 2000;
const STEP_MIN = { 5: 1, 15: 3, 60: 10 };   // ช่วงดูกี่นาที -> ขีดเวลาทุกกี่นาที

function css(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

export class TrendCharts {
  constructor(box) {
    this.box = box;
    this.windowMin = CHART_WINDOWS_MIN[1];
    this.charts = [];
    this.ok = typeof window.Chart !== "undefined";
    if (!this.ok) {
      box.append(el("p", { class: "empty", text: "โหลด Chart.js ไม่ได้ (เช็กเน็ต แล้วโหลดหน้าใหม่) ค่ายังเก็บในประวัติตามปกติ" }));
      return;
    }
    const Chart = window.Chart;
    Chart.defaults.font.family = css("--font") || "system-ui, sans-serif";
    Chart.defaults.font.size = 12;
    for (const cfg of CHARTS) this.make(cfg);
    this.applyTheme();
  }

  make(cfg) {
    const canvas = el("canvas", { role: "img", "aria-label": "กราฟ" + cfg.title });
    const last = el("span", { class: "chart-last" });
    const card = el("figure", { class: "panel chart-card" },
      el("figcaption", {}, el("span", { class: "chart-title", text: cfg.title + " (" + cfg.unit + ")" }), last),
      el("div", { class: "chart-wrap" }, canvas));
    this.box.append(card);
    const sameSource = cfg.series.every((s) => s.src === cfg.series[0].src);
    const chart = new window.Chart(canvas, {
      type: "line",
      data: {
        datasets: cfg.series.map((s) => ({
          label: s.label, data: [], borderWidth: 2, pointRadius: 0, pointHoverRadius: 4,
          pointHitRadius: 8, tension: 0.25, spanGaps: false, colorKey: s.color,
        })),
      },
      options: {
        animation: false,
        parsing: false,
        normalized: true,
        responsive: true,
        maintainAspectRatio: false,
        // ทุกเส้นมาจากข้อความใบเดียวกัน = ชี้แล้วเห็นทุกเส้นพร้อมกัน · มาคนละหัวข้อ = เห็นจุดที่ใกล้ที่สุด
        interaction: sameSource ? { mode: "index", intersect: false } : { mode: "nearest", axis: "x", intersect: false },
        scales: {
          x: { type: "linear", ticks: { includeBounds: false, maxRotation: 0, callback: (v) => hhmmss(v).slice(0, 5) } },
          y: { min: cfg.min, max: cfg.max, ticks: { maxTicksLimit: 5 } },
        },
        plugins: {
          legend: { display: cfg.series.length > 1, labels: { usePointStyle: true, pointStyle: "line", boxWidth: 18 } },
          tooltip: { callbacks: {
            title: (items) => items.length ? hhmmss(items[0].parsed.x) : "",
            label: (it) => it.dataset.label + ": " + (it.parsed.y === null ? "—" : it.parsed.y) + " " + cfg.unit,
          } },
        },
      },
    });
    this.charts.push({ cfg, chart, last });
  }

  // จุดใหม่หนึ่งจุดจากข้อความหนึ่งใบ (src เช่น "telemetry" หรือ "field/soil")
  add(src, data, t, redraw = true) {
    for (const c of this.charts) {
      let touched = false;
      c.cfg.series.forEach((s, i) => {
        if (s.src !== src) return;
        // เส้นที่มาจากข้อความใบเดียวกันต้องได้จุดทุกใบ (null = เว้นช่อง) เส้นจึงเรียงตรงกันเวลาชี้ดู
        if (!(s.key in data) && c.cfg.series.length === 1) return;
        const v = num(data[s.key], c.cfg.min ?? -1e6, c.cfg.max ?? 1e6);
        const arr = c.chart.data.datasets[i].data;
        arr.push({ x: t, y: v });
        if (arr.length > MAX_POINTS) arr.splice(0, arr.length - MAX_POINTS);
        touched = true;
      });
      if (touched && redraw) this.refresh(c);
    }
  }

  // โหลดประวัติจาก storage.js ตอนเปิดหน้า กราฟจะได้ไม่ว่าง
  loadHistory(items) {
    for (const it of items) {
      if (it.k === "telemetry" || it.k === "field" || it.k === "plc") this.add(it.s, it.d || {}, it.t, false);
    }
    this.refreshAll();
  }

  clear() {
    for (const c of this.charts) c.chart.data.datasets.forEach((d) => (d.data = []));
    this.refreshAll();
  }

  setWindow(min) {
    this.windowMin = min;
    this.refreshAll();
  }

  refresh(c, now = Date.now()) {
    const x = c.chart.options.scales.x;
    x.min = now - this.windowMin * 60000;
    x.max = now;
    x.ticks.stepSize = STEP_MIN[this.windowMin] * 60000 || undefined;   // ขีดเวลาตรงนาทีเต็ม
    c.chart.update("none");
    // ค่าล่าสุดของแต่ละเส้นบนหัวกราฟ (ข้อความ ไม่ใช่สี เพื่อให้คนตาบอดสีอ่านได้)
    const parts = c.chart.data.datasets.map((d) => {
      const p = d.data[d.data.length - 1];
      return p && p.y !== null ? (c.cfg.series.length > 1 ? d.label + " " : "") + p.y + " " + c.cfg.unit : null;
    }).filter(Boolean);
    c.last.textContent = parts.join(" · ");
  }

  refreshAll() {
    const now = Date.now();
    for (const c of this.charts) this.refresh(c, now);
  }

  applyTheme() {
    if (!this.ok) return;
    const grid = css("--grid"), ink = css("--ink-2");
    for (const c of this.charts) {
      for (const d of c.chart.data.datasets) {
        const color = css("--series-" + d.colorKey) || ink;
        d.borderColor = color;
        d.backgroundColor = color;
        d.pointHoverBackgroundColor = color;
        d.pointHoverBorderColor = css("--surface");
      }
      for (const axis of ["x", "y"]) {
        const sc = c.chart.options.scales[axis];
        sc.grid = { color: grid, drawTicks: false };
        sc.border = { display: false };
        sc.ticks.color = ink;
      }
      c.chart.options.plugins.legend.labels.color = ink;
      c.chart.update("none");
    }
  }
}
