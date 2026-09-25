// FilterTrack offline shell. Caches the app's own files so the page opens
// with no network; session data already queues in localStorage and is sent
// by the page once it's back online. Cross-origin requests (the API, fonts)
// and the /log diagnostics endpoint always go straight to the network.

const CACHE = "filtertrack-shell-v7";
const SHELL = [
  "./",
  "index.html",
  "csv-analysis.html",
  "bi-config.js?v=7",
  "bridge.js?v=7",
  "manifest.webmanifest",
  "icons/icon-192.png",
  "icons/icon-512.png",
  "vendor/react.production.min.js",
  "vendor/react-dom.production.min.js",
  "vendor/babel.min.js",
];

self.addEventListener("install", (event) => {
  event.waitUntil(
    caches.open(CACHE).then((cache) => cache.addAll(SHELL)).then(() => self.skipWaiting())
  );
});

self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches.keys()
      .then((keys) => Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k))))
      .then(() => self.clients.claim())
  );
});

// Network first, falling back to the cache: online the page always gets the
// latest deploy (and refreshes the cache); offline it gets the last copy.
self.addEventListener("fetch", (event) => {
  const req = event.request;
  const url = new URL(req.url);
  if (req.method !== "GET" || url.origin !== self.location.origin || url.pathname === "/log") return;

  event.respondWith(
    fetch(req)
      .then((res) => {
        if (res.ok) {
          const copy = res.clone();
          caches.open(CACHE).then((cache) => cache.put(req, copy));
        }
        return res;
      })
      .catch(() =>
        caches.match(req, { ignoreSearch: url.pathname === "/" || url.pathname.endsWith(".html") })
          .then((hit) => hit || caches.match("index.html"))
      )
  );
});
