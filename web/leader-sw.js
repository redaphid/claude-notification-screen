// Offline-first service worker for the leader page.
//
// Everything the page needs is cached at install, so once the page has been
// opened (or installed) it keeps working with no network at all: the leader
// is configured over USB serial, the network was only ever for delivery.
// Cache-first with a background refresh: a new deploy shows up on the next
// open after the first one that had connectivity.
const CACHE = 'chorus-leader-v1';
const ASSETS = [
  './leader.html',
  './leader.webmanifest',
  './leader-sw.js',
  './icons/leader-192.png',
  './icons/leader-512.png',
];

self.addEventListener('install', event => {
  event.waitUntil(caches.open(CACHE).then(c => c.addAll(ASSETS)).then(() => self.skipWaiting()));
});

self.addEventListener('activate', event => {
  event.waitUntil(
    caches.keys().then(keys => Promise.all(keys.filter(k => k !== CACHE).map(k => caches.delete(k))))
      .then(() => self.clients.claim())
  );
});

self.addEventListener('fetch', event => {
  if (event.request.method !== 'GET') return;
  event.respondWith(
    caches.match(event.request, { ignoreSearch: true }).then(cached => {
      const refresh = fetch(event.request).then(res => {
        if (res && res.ok) caches.open(CACHE).then(c => c.put(event.request, res.clone()));
        return res;
      }).catch(() => cached);
      return cached || refresh;
    })
  );
});
