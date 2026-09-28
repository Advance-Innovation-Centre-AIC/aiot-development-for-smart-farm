// mqtt-client.js - ต่อ broker ผ่าน WebSocket, subscribe หัวข้อของกลุ่ม, ส่งคำสั่ง, ต่อใหม่เองเมื่อหลุด
//
// ใช้ mqtt.js (window.mqtt) ที่ index.html โหลดจาก CDN แบบล็อกรุ่นและตรวจ SRI แล้ว
// กติกาจากสัญญาข้อ 5: client id ห้ามซ้ำ (สุ่มท้ายเสมอ) · QoS 0 · ไม่ retain · ส่งคำสั่งห่างกันอย่างน้อย 1 วิ

import { CMD_GAP_MS, MAX_PAYLOAD_B, topics } from "./config.js";

// สถานะที่ป้ายบนหัวจอแสดง
export const STATUS = {
  idle: "ยังไม่ได้ต่อ",
  connecting: "กำลังต่อ...",
  online: "ต่อแล้ว",
  reconnecting: "หลุด กำลังต่อใหม่...",
  offline: "ไม่มีเน็ต",
  nolib: "โหลด mqtt.js ไม่ได้",
};

export class FarmMqtt {
  // onMessage(topic, text) · onStatus(code, detail) · onSent(obj, ok, why)
  constructor({ onMessage, onStatus, onSent }) {
    this.onMessage = onMessage;
    this.onStatus = onStatus;
    this.onSent = onSent;
    this.client = null;
    this.team = null;
    this.queue = [];
    this.lastSend = 0;
    this.timer = null;
    this.recent = [];            // ข้อความที่เราส่งเอง ไว้แยก "คำสั่งของเรา" ออกจากของคนอื่นบนหัวข้อ cmd
    this.counts = { rx: 0, tx: 0 };
  }

  get connected() {
    return !!(this.client && this.client.connected);
  }

  connect(url, team) {
    this.disconnect();
    if (typeof window.mqtt === "undefined") {
      this.onStatus("nolib", "เช็กเน็ต แล้วโหลดหน้าใหม่");
      return;
    }
    this.team = team;
    this.t = topics(team);
    const rand = Math.random().toString(16).slice(2, 8).padEnd(6, "0");
    this.clientId = "farm-dash-" + team + "-" + rand;          // ไม่ชนกับบอร์ด (bento-farm-/bento-gw-)
    this.onStatus("connecting", url.replace(/^wss:\/\//, "").replace(/\/mqtt$/, ""));
    const client = window.mqtt.connect(url, {
      clientId: this.clientId,
      clean: true,
      keepalive: 30,
      connectTimeout: 10000,
      reconnectPeriod: 3000,       // หลุดแล้วลองใหม่ทุก 3 วิ ไม่ต้องกดอะไร
      resubscribe: false,          // เรา subscribe เองทุกครั้งที่ต่อได้ (ด้านล่าง)
      protocolVersion: 4,
    });
    this.client = client;
    client.on("connect", () => {
      if (client !== this.client) return;
      client.subscribe(this.t.all, { qos: 0 }, (err) => {
        this.onStatus(err ? "reconnecting" : "online", err ? "subscribe ไม่ได้" : this.t.all);
      });
    });
    client.on("reconnect", () => client === this.client && this.onStatus("reconnecting", ""));
    client.on("offline", () => client === this.client &&
      this.onStatus(navigator.onLine === false ? "offline" : "reconnecting", ""));
    client.on("error", (e) => client === this.client && this.onStatus("reconnecting", String(e && e.message || e).slice(0, 60)));
    client.on("message", (topic, payload) => {
      if (client !== this.client) return;
      this.counts.rx += 1;
      if (payload.length > 4096) return;                       // ขยะก้อนใหญ่ ไม่อ่าน
      this.onMessage(topic, new TextDecoder("utf-8").decode(payload));
    });
  }

  disconnect() {
    if (this.client) {
      const old = this.client;
      this.client = null;
      old.end(true);
    }
    this.queue = [];
    clearTimeout(this.timer);
    this.timer = null;
  }

  // ส่งคำสั่งเข้าคิว คิวปล่อยทีละใบห่างกัน CMD_GAP_MS (กล่องรับของบอร์ดมีช่องเดียว ใบที่สองจะทับใบแรก)
  sendCommand(obj) {
    const text = JSON.stringify(obj);
    if (new TextEncoder().encode(text).length > MAX_PAYLOAD_B) {
      this.onSent(obj, false, "ยาวเกิน " + MAX_PAYLOAD_B + " ไบต์");
      return false;
    }
    if (!this.connected) {
      this.onSent(obj, false, "ยังไม่ได้ต่อ broker");
      return false;
    }
    this.queue.push(obj);
    this.pump();
    return true;
  }

  get queued() {
    return this.queue.length;
  }

  pump() {
    if (this.timer || !this.queue.length) return;
    const wait = Math.max(0, this.lastSend + CMD_GAP_MS - Date.now());
    this.timer = setTimeout(() => {
      this.timer = null;
      const obj = this.queue.shift();
      if (obj && this.connected) {
        const text = JSON.stringify(obj);
        this.client.publish(this.t.cmd, text, { qos: 0, retain: false });
        this.lastSend = Date.now();
        this.counts.tx += 1;
        this.recent.push({ text, t: this.lastSend });
        this.recent = this.recent.filter((r) => this.lastSend - r.t < 10000);
        this.onSent(obj, true, "");
      } else if (obj) {
        this.onSent(obj, false, "หลุดก่อนส่ง");
      }
      this.pump();
    }, wait);
  }

  // ข้อความบนหัวข้อ cmd เป็นของเราไหม (เทียบกับที่เราเพิ่งส่งใน 10 วิ)
  isOurs(text) {
    const i = this.recent.findIndex((r) => r.text === text);
    if (i < 0) return false;
    this.recent.splice(i, 1);
    return true;
  }
}
