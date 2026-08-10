// Luminary PWA service worker. The app shell + scene assets + wasm are cached
// for offline (cache-first); the live runtime bundle is always network-first so
// conditions stay current, falling back to whatever was last seen.
const CACHE = 'luminary-app-v1';
const SHELL = [
  './', './index.html', './luminary.js', './solar.js',
  './luminary_core.js', './luminary_core.wasm', './manifest.webmanifest',
  './icon-192.png', './icon-512.png',
  './assets/nubble_runtime_base.jpg',
  './assets/nubble_runtime_water_mask.bin',
  './assets/nubble_runtime_land_mask.bin',
  './assets/nubble_runtime_shore_distance.bin',
  './assets/nubble_runtime_ocean_map.bin',
  './assets/nubble_runtime_ocean_depth.bin',
  './assets/nubble_runtime_cloud_low.bin',
  './assets/nubble_runtime_cloud_mid.bin',
  './assets/nubble_runtime_cloud_high.bin',
];

self.addEventListener('install', (e) => {
  e.waitUntil(caches.open(CACHE).then((c) => c.addAll(SHELL)).then(() => self.skipWaiting()));
});

self.addEventListener('activate', (e) => {
  e.waitUntil(
    caches.keys().then((keys) =>
      Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k))))
      .then(() => self.clients.claim()),
  );
});

self.addEventListener('fetch', (e) => {
  const url = new URL(e.request.url);
  if (url.pathname.startsWith('/runtime/')) {
    // Live data: prefer network, fall back to last cached copy.
    e.respondWith(
      fetch(e.request).then((r) => {
        const copy = r.clone();
        caches.open(CACHE).then((c) => c.put(e.request, copy));
        return r;
      }).catch(() => caches.match(e.request)),
    );
    return;
  }
  e.respondWith(caches.match(e.request).then((r) => r || fetch(e.request)));
});
