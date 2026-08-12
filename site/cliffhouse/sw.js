const CACHE = 'cliff-house-luminary-v2';
const SHELL = [
  './', './index.html', './cliff-house.css', './cliff-house.js', './solar.js',
  './manifest.webmanifest', './assets/registration.json',
  './assets/cliff-house-foreground.png', './assets/cliff-house-ocean-texture.jpg'
];
self.addEventListener('install', (event) => {
  event.waitUntil(caches.open(CACHE).then((cache) => cache.addAll(SHELL)));
  self.skipWaiting();
});
self.addEventListener('activate', (event) => {
  event.waitUntil(caches.keys().then((keys) => Promise.all(
    keys.filter((key) => key.startsWith('cliff-house-luminary-') && key !== CACHE)
      .map((key) => caches.delete(key)),
  )).then(() => self.clients.claim()));
});
self.addEventListener('fetch', (event) => {
  if (event.request.method !== 'GET') return;
  const url = new URL(event.request.url);
  if (url.origin !== self.location.origin) return;
  const live = url.pathname.includes('/runtime/');
  if (live) {
    event.respondWith(fetch(event.request, { cache: 'no-store' }).catch(() => caches.match(event.request)));
    return;
  }
  const navigation = event.request.mode === 'navigate';
  const network = fetch(event.request).then((response) => {
    if (response.ok) caches.open(CACHE).then((cache) => cache.put(event.request, response.clone()));
    return response;
  });
  event.respondWith(navigation
    ? network.catch(() => caches.match('./index.html'))
    : caches.match(event.request).then((cached) => cached || network));
});
