// JasperEngine web crash hooks (docs/crash-diagnostics.md), linked with --pre-js so custom shells
// keep them. The first fatal event (Module.onAbort, an uncaught error, an unhandled rejection)
// logs "JASPERENGINE CRASH: <message>", the stack and the last 200 console lines with
// console.error (then the Gameplay Journal, read from the heap), calls
// window.jasperengine.onCrash(report) when the page defines it, and shows
// "The game stopped working." unless the overlay is off. Nothing is sent anywhere.
(function () {
  var M = typeof Module != "undefined" ? Module : {};
  var tail = [], fired = false, cfg = { overlay: M.jasperengineCrashOverlay !== false, buildHash: "", product: "" };
  var con = typeof console != "undefined" ? console : null, errorOut = con ? con.error.bind(con) : function () {};
  function keep(args) {
    var line = Array.prototype.map.call(args, String).join(" ");
    tail.push(line.length > 2000 ? line.slice(0, 2000) : line);
    if (tail.length > 200) tail.shift();
  }
  ["log", "info", "warn", "error", "debug"].forEach(function (name) {
    if (!con || !con[name]) return;
    var original = con[name].bind(con);
    con[name] = function () { keep(arguments); original.apply(null, arguments); };
  });
  function overlay() {
    if (!cfg.overlay || typeof document == "undefined" || !document.body) return;
    var box = document.createElement("div");
    box.id = "jasperengine-crash";
    box.style.cssText = "position:fixed;inset:0;z-index:2147483647;display:flex;flex-direction:column;" +
      "align-items:center;justify-content:center;gap:12px;background:rgba(0,0,0,.8);color:#fff;font:16px sans-serif";
    box.textContent = "The game stopped working.";
    var button = document.createElement("button");
    button.textContent = "Reload";
    button.onclick = function () { location.reload(); };
    box.appendChild(button);
    document.body.appendChild(box);
  }
  // journal.txt from the Gameplay Journal's ring in the heap (diag/Journal.h): the header, then
  // the whole entries oldest first (a torn oldest one dropped, as the native handlers do)
  function journalText() {
    var j = cfg.journal;
    if (!j || !j.ring || !j.capacity || typeof HEAPU8 == "undefined" || typeof HEAPU32 == "undefined") return "";
    var cursor = HEAPU32[j.cursor >>> 2] + HEAPU32[(j.cursor >>> 2) + 1] * 4294967296;
    var written = HEAPU32[j.written >>> 2] + HEAPU32[(j.written >>> 2) + 1] * 4294967296;
    if (!cursor) return "";
    var begin = cursor > j.capacity ? cursor - j.capacity : 0, bytes = [], i;
    for (i = begin; i < cursor; i++) bytes.push(HEAPU8[j.ring + (i % j.capacity)]);
    var text = new TextDecoder().decode(new Uint8Array(bytes));
    if (cursor > j.capacity) text = text.slice(text.indexOf("\n") + 1);
    var lines = text.split("\n").filter(function (line, index, all) { return index < all.length - 1 && line.charAt(0) == "+"; });
    if (!lines.length) return "";
    return j.header + "dropped: " + Math.max(0, written - lines.length) + "\n---\n" + lines.join("\n") + "\n";
  }
  function crash(message, stack) {
    if (fired) return;
    fired = true;
    message = String(message || "unknown error");
    stack = String(stack || new Error().stack || "");
    var journal = "";
    try { journal = journalText(); } catch (e) { journal = ""; }
    var report = { message: message, stack: stack, logTail: tail.join("\n"), buildHash: cfg.buildHash, product: cfg.product,
      userAgent: typeof navigator != "undefined" ? navigator.userAgent : "", time: new Date().toISOString(), journal: journal };
    errorOut("JASPERENGINE CRASH: " + message + "\n" + stack + "\n--- log tail ---\n" + report.logTail +
      (journal ? "\n--- journal ---\n" + journal : ""));
    try {
      var api = typeof window != "undefined" && window.jasperengine;
      if (api && typeof api.onCrash == "function") api.onCrash(report);
    } catch (e) {
      errorOut("jasperengine.onCrash failed: " + e);
    }
    overlay();
  }
  var previousAbort = M.onAbort;
  M.onAbort = function (what) {
    crash("abort: " + what);
    if (typeof previousAbort == "function") previousAbort(what);
  };
  M.jasperengineCrash = {
    configure: function (options) { for (var key in options) cfg[key] = options[key]; },
    report: crash
  };
  if (typeof window != "undefined" && window.addEventListener) {
    window.addEventListener("error", function (event) {
      crash(event.message || (event.error && event.error.message), event.error && event.error.stack);
    });
    window.addEventListener("unhandledrejection", function (event) {
      var reason = event.reason;
      crash("unhandled rejection: " + (reason && reason.message ? reason.message : reason), reason && reason.stack);
    });
  }
})();
