// The page has to work with no network at all: at camp there is none, and the
// whole point is a console that lives in a pocket. Cache-first, one version
// bumped by hand -- there is no build step here and there should not be one.
// BUMP THIS whenever control.html changes. The strategy is cache-first, so a
// phone that has already saved the page will otherwise never fetch a new one --
// and this page is meant to be added to a home screen and used offline for a
// weekend, which is exactly the case where a stale copy hurts most.
const CACHE = 'chorus-control-v2';
const FILES = ['control.html', 'control.webmanifest'];

self.addEventListener('install', e => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(FILES)).then(() => self.skipWaiting()));
});
self.addEventListener('activate', e => {
  e.waitUntil(caches.keys().then(ks =>
    Promise.all(ks.filter(k => k !== CACHE).map(k => caches.delete(k)))).then(() => self.clients.claim()));
});
self.addEventListener('fetch', e => {
  if (e.request.method !== 'GET') return;
  e.respondWith(caches.match(e.request, { ignoreSearch: true }).then(hit =>
    hit || fetch(e.request).then(res => {
      const copy = res.clone();
      caches.open(CACHE).then(c => c.put(e.request, copy)).catch(() => {});
      return res;
    }).catch(() => hit)));
});
