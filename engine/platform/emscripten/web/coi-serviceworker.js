// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Cross-origin isolation for the multithreaded web player (JasperEngine export).
// Adds Cross-Origin-Opener-Policy and Cross-Origin-Embedder-Policy to the responses of this
// origin, so browsers allow SharedArrayBuffer on hosts that cannot send those headers.
// player-loader.js registers it once per session and reloads when it takes control.
self.addEventListener("install", () => self.skipWaiting());
self.addEventListener("activate", (event) => event.waitUntil(self.clients.claim()));
self.addEventListener("fetch", (event) => {
    const request = event.request;
    if (request.cache === "only-if-cached" && request.mode !== "same-origin") return;
    event.respondWith(fetch(request).then((response) => {
        if (response.status === 0) return response;
        const headers = new Headers(response.headers);
        headers.set("Cross-Origin-Embedder-Policy", "require-corp");
        headers.set("Cross-Origin-Opener-Policy", "same-origin");
        headers.set("Cross-Origin-Resource-Policy", "same-origin");
        return new Response(response.body, {status: response.status, statusText: response.statusText, headers});
    }).catch((error) => { console.error(error); throw error; }));
});
