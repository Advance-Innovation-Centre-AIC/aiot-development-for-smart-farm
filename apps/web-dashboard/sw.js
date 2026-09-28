// sw.js - service worker เล็ก ๆ: เก็บ "เปลือกแอป" (HTML CSS JS ไอคอน) ไว้ในเครื่อง ให้เปิดแอปได้แม้เน็ตหลุด
//
// ไม่แตะ MQTT เลย: ข้อความ MQTT วิ่งผ่าน WebSocket ซึ่ง service worker มองไม่เห็นและไม่เก็บ
// ไฟล์ของเราเอง = ลองเน็ตก่อน ไม่ได้ค่อยใช้ของในเครื่อง (แก้โค้ดแล้วเห็นผลทันที ไม่ติดของเก่า)
// ไลบรารีจาก CDN ล็อกรุ่นแล้ว เนื้อไฟล์ไม่เปลี่ยน = ใช้ของในเครื่องก่อน (เบราว์เซอร์ยังตรวจ SRI ทุกครั้ง)
// แก้รายการไฟล์ด้านล่างเมื่อไร ให้เปลี่ยนเลขใน CACHE ด้วย (v1 -> v2) ของเก่าจะถูกลบเอง

const CACHE = "farm-dash-v1";
const SHELL = [
  "./",
  "./index.html",
  "./manifest.webmanifest",
  "./css/style.css",
  "./js/app.js",
  "./js/config.js",
  "./js/mqtt-client.js",
  "./js/ui.js",
  "./js/map.js",
  "./js/charts.js",
  "./js/alerts.js",
  "./js/storage.js",
  "./icons/icon.svg",
  "./icons/icon-192.png",
  "./icons/icon-512.png",
];
const CDN = [
  "https://cdn.jsdelivr.net/npm/mqtt@5.16.0/dist/mqtt.min.js",
  "https://cdn.jsdelivr.net/npm/chart.js@4.5.1/dist/chart.umd.min.js",
];

self.addEventListener("install", (event) => {
  event.waitUntil(caches.open(CACHE).then((c) => c.addAll(SHELL)).then(() => self.skipWaiting()));
});

self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches.keys()
      .then((keys) => Promise.all(keys.filter((k) => k.startsWith("farm-dash-") && k !== CACHE).map((k) => caches.delete(k))))
      .then(() => self.clients.claim()));
});

self.addEventListener("fetch", (event) => {
  const req = event.request;
  if (req.method !== "GET") return;
  const url = new URL(req.url);
  if (CDN.includes(url.href)) {                       // ไลบรารีล็อกรุ่น: ในเครื่องก่อน
    event.respondWith(caches.match(req).then((hit) => hit || fetch(req).then((res) => {
      if (res.ok && res.type === "cors") { const copy = res.clone(); caches.open(CACHE).then((c) => c.put(req, copy)); }
      return res;
    })));
    return;
  }
  if (url.origin !== self.location.origin) return;    // อย่างอื่นนอกเว็บเรา ปล่อยผ่าน ไม่เก็บ
  event.respondWith(fetch(req).then((res) => {         // ไฟล์ของเรา: เน็ตก่อน
    if (res.ok && url.search === "") { const copy = res.clone(); caches.open(CACHE).then((c) => c.put(req, copy)); }
    return res;
  }).catch(() => caches.match(req, { ignoreSearch: true }).then((hit) => hit || caches.match("./index.html"))));
});

// แตะที่แจ้งเตือน = เปิดแอป (หรือสลับไปหน้าที่เปิดอยู่)
self.addEventListener("notificationclick", (event) => {
  event.notification.close();
  event.waitUntil(self.clients.matchAll({ type: "window", includeUncontrolled: true }).then((list) => {
    for (const c of list) if ("focus" in c) return c.focus();
    return self.clients.openWindow ? self.clients.openWindow("./") : undefined;
  }));
});
