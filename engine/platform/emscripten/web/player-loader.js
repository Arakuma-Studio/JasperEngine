// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Starts a JasperEngine web player (docs/specs/S30-shipping.md R15, S06 R19). The page loads it
// with its configuration in data- attributes:
//   <script src="player-loader.js" data-st="Game.js" data-mt="threads/Game.js"
//           data-worker="coi-serviceworker.js" data-pack="content.jpak"
//           data-manifest="content-manifest.json"></script>
// data-st: the single-thread player (required). data-mt: the pthreads one (only when the build
// has it). data-pack / data-manifest: a prebuilt player's content, fetched before it starts.
//
// It decides once which player runs:
//  - the page is cross-origin isolated (and has SharedArrayBuffer): mt;
//  - it is not, the build has mt, a service worker can be installed and none was tried in this
//    session: register coi-serviceworker.js, wait for it to control the page and reload, without
//    starting a player (the reload must not cut a load short);
//  - otherwise (no mt, no worker, the worker did not isolate the page): st.
(function () {
    "use strict";
    var script = document.currentScript;
    var data = (script && script.dataset) || {};
    var TRIED = "jasperengine-coi-tried";
    var WAIT_MS = 5000;
    var started = false;

    function status(text, failed) {
        var element = document.getElementById("status");
        if (element) {
            element.textContent = text;
            if (failed) element.setAttribute("data-failed", "1");
        }
        if (failed) console.error(text);
    }

    function directory(path) {
        var slash = path.lastIndexOf("/");
        return slash < 0 ? "" : path.substring(0, slash + 1);
    }

    function fetchOrFail(url, kind) {
        return fetch(url).then(function (response) {
            if (!response.ok) throw new Error(url + ": HTTP " + response.status);
            return kind === "text" ? response.text() : response.arrayBuffer();
        });
    }

    function start(threads, isolated) {
        if (started) return;
        started = true;
        var player = threads ? data.mt : data.st;
        console.log("crossOriginIsolated=" + isolated + " threads=" + threads);
        var Module = window.Module = window.Module || {};
        // the player's .wasm (and worker) sit next to its .js
        var base = directory(player);
        var locate = Module.locateFile;
        Module.locateFile = function (path, prefix) {
            return locate ? locate(base + path, prefix) : base + path;
        };
        var content = [];
        if (data.pack) {
            status("Downloading...");
            content.push(fetchOrFail(data.pack, "binary").then(function (bytes) { Module.jasperContentPack = bytes; }));
        }
        if (data.manifest) {
            content.push(fetchOrFail(data.manifest, "text").then(function (text) { Module.jasperContentManifest = text; }));
        }
        Promise.all(content).then(function () {
            var element = document.createElement("script");
            element.async = true;
            element.src = player;
            element.onerror = function () { status("Cannot load " + player, true); };
            document.body.appendChild(element);
        }).catch(function (error) {
            status("Cannot load the game's content: " + error.message, true);
        });
    }

    var isolated = self.crossOriginIsolated === true && typeof SharedArrayBuffer !== "undefined";
    if (!data.st) {
        status("player-loader.js: the page gives no data-st player", true);
        return;
    }
    if (!data.mt || isolated) {
        start(!!data.mt && isolated, isolated);
        return;
    }
    var canRegister = window.isSecureContext && !!navigator.serviceWorker && !!data.worker;
    var tried = false;
    try { tried = sessionStorage.getItem(TRIED) === "1"; } catch (e) { tried = true; }
    // a worker that already controls this page and did not isolate it never will
    if (!canRegister || tried || navigator.serviceWorker.controller) {
        start(false, false);
        return;
    }
    try { sessionStorage.setItem(TRIED, "1"); } catch (e) { start(false, false); return; }
    status("Enabling threads...");
    navigator.serviceWorker.addEventListener("controllerchange", function () {
        if (!started) window.location.reload();
    });
    navigator.serviceWorker.register(data.worker).then(function () {
        // the worker claims the page when it activates (controllerchange); a browser that
        // never lets it gets the single-thread player
        setTimeout(function () {
            if (!navigator.serviceWorker.controller) start(false, false);
        }, WAIT_MS);
    }).catch(function (error) {
        console.warn("coi-serviceworker: " + error);
        start(false, false);
    });
})();
