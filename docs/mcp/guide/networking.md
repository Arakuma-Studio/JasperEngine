# Networking: HTTP(S) requests

Served as `jasper://docs/networking`. `Http` (Lua) and `jasperengine::Http` (C++,
`#include "net/Http.h"`) send HTTP(S) requests: GET / POST / PUT / PATCH / DELETE / HEAD (any
method), JSON, text, binary and form bodies, downloads to a file with progress, and textures
straight from a URL. Unity: `UnityWebRequest` (see the table at the end).

Each platform uses its own HTTP stack, so TLS, certificates, proxies and IPv6 come from the OS:
Linux the system libcurl (`libcurl.so.4`, installed on every desktop distribution; without it
`Http.isAvailable()` is false and requests fail with `unsupported`), Windows WinHTTP, macOS and
iOS NSURLSession, Android HttpURLConnection, Web the browser's `fetch()`. Editor Play uses the
desktop's.

## The rules

1. **One callback, on the main thread.** `Http.get(url, function(res) ... end)` returns at once;
   the callback runs exactly once, in a later frame, right before `onUpdate` (never inside the
   call, never on another thread). It does not run while the game or the editor's Play is
   paused (the transfer goes on), and runs at `Engine.timeScale = 0`.
2. **Errors are values.** The callback always gets an `HttpResponse`: check `res.ok`, then
   `res.errorCode` / `res.error`. Only wrong argument types raise Lua errors
   (`Http.get(42)`). A 404 is `ok = false, errorCode = "http", status = 404` with its body.
3. **Owners.** Pass `owner = self` in a script of a scene: when the script goes (GameObject
   destroyed, scene unloaded, Play stopped) the request is cancelled and **no callback runs**
   (the callback would touch destroyed objects). `owner` can also be a GameObject (any
   EntityHandle) or a Scene. Without an owner a request is global: it survives scene loads and
   ends with its response, `cancel()`, `Http.cancelAll()`, Play stop or the app quitting.
4. **Plain HTTP is off.** `http://` to any host but `localhost`, `127.0.0.1` and `::1` fails
   with `insecure_http` everywhere, editor Play included, also for a redirect to such a URL.
   Turn it on with Project Settings > Player > Other Settings > **Allow Insecure HTTP**
   (`player.allow_insecure_http`); exports then set Android's cleartext policy and iOS/macOS
   App Transport Security to match.
5. **Timeouts are wall-clock seconds** (default `timeout = Http.timeout` = 30 for the whole
   request including redirects, `connectTimeout = 15`), not scaled by `Engine.timeScale`.

## API

| Function | Meaning |
|---|---|
| `Http.get(url [, options] [, callback])` | GET. Either may be left out; a function in the options place is the callback. |
| `Http.head(url [, options] [, callback])` | HEAD: status and headers, empty body. |
| `Http.delete(url [, options] [, callback])` | DELETE. |
| `Http.post(url, body [, options] [, callback])` | POST. `body`: a string is sent as is (no Content-Type: set one in `headers`), a table as JSON (`Content-Type: application/json`), `nil` sends nothing. |
| `Http.put(...)` / `Http.patch(...)` | the same for PUT / PATCH. |
| `Http.request(options)` | the full form: `url` (required), `method` (default `"GET"`, uppercased), `onComplete` and every option. |
| `Http.download(url, path [, options] [, callback])` | GET into a file: `path` is `data://...` (a bare relative path means `data://`) or an absolute path inside `System.getUserDataPath()`; missing folders are made. The bytes go to `<path>.part`, renamed to `path` on success: an existing file is replaced only by a complete download, a failed or cancelled one leaves no `.part`. Big files stream to disk (no memory limit). |
| `Http.getTexture(url [, options] [, callback])` | GET and decode an image (PNG, JPEG, BMP, TGA, GIF first frame) off the main thread: `res.textureData` (a `TextureData`). Not an image: `errorCode = "decode"`. |
| `Http.cancelAll()` | cancels every request; their callbacks run with `"cancelled"`. |
| `Http.isAvailable()` | false only when the platform's stack cannot work (Linux without libcurl). |
| `Http.encodeUrl(s)` / `Http.decodeUrl(s)` | percent-encoding (letters, digits and `-._~` stay); `decodeUrl` turns `+` into a space. |
| `Http.maxConcurrentRequests` | rw, 1..64, default 8: more requests wait in order (`HttpState.Queued`). |
| `Http.userAgent` | rw: default `JasperEngine/<version> (<platform>)`; `""` restores it. The browser's own on the web. |
| `Http.timeout` | rw: the default total timeout in seconds (30). |
| `Http.logFailures` | rw, default false: each failed request (not a cancel) logs `HTTP GET https://... failed: <error>` as a warning. Off by default: games retry and probe connectivity on purpose (agents read `get_play_state.http`). |

### Options (all optional; an unknown key logs a warning once, e.g. `timout`)

| Key | Type, default | Meaning |
|---|---|---|
| `headers` | `{["Name"] = "value"}` or `{{"Name", "value"}, ...}` | request headers (names as given). `Host`, `Content-Length`, `Transfer-Encoding`, `Connection` and `Accept-Encoding` belong to the stack: ignored with a warning (gzip is accepted and decoded on every platform). |
| `query` | `{name = value}` (strings, numbers, booleans) | appended to the URL (`?` or `&`), percent-encoded, sorted by name. |
| `body` | string | the raw body (binary-safe), for `request` and the body-less calls' `method`. |
| `json` | any JSON value | encoded with `Json.encode` (an encode error raises: it is a bug), `Content-Type: application/json` unless set. |
| `form` | `{name = value}` | `application/x-www-form-urlencoded`. |
| `timeout` | number, `Http.timeout` | seconds for the whole request; `0` = none. |
| `connectTimeout` | number, 15 | seconds to connect. |
| `redirects` | boolean or integer, `true` | `true` = up to 10 hops, `false`/`0` = a 3xx is the response (`ok = false, errorCode = "http"`), `n` = at most n (past it `too_many_redirects`). 301/302/303 turn a POST into a GET without its body (303 any method but HEAD), 307/308 keep both; a hop to another origin drops `Authorization` and `Cookie`. |
| `verifyTls` | boolean, true | `false` accepts any certificate (logs a warning once per host; ignored on the web). Use it only for a test server. |
| `responseType` | `"text"` (default), `"binary"`, `"json"` | `text` and `binary` both fill `res.body` (Lua strings hold any bytes). `json` also fills `res.data` (decoded on a worker thread; invalid JSON in a 2xx is `errorCode = "decode"`). |
| `maxBodySize` | integer bytes, 64 MiB | in-memory bodies (not downloads); past it `too_large`. `0` = no limit. |
| `onProgress` | `function(request, received, total)` | at most once a frame, always before the callback; `total` is -1 while unknown (a compressed or chunked body). |
| `owner` | `self`, a GameObject / EntityHandle, a Scene | rule 3. |
| `file` | string | write the body to this file (as `Http.download`). |
| `resume` | boolean, false | a download continues an existing `<path>.part` (sends `Range: bytes=<size>-`; a 206 appends, a 200 starts over, a 416 removes the `.part` and fails). |
| `keepPartial` | boolean, false | a failed or cancelled download keeps its `<path>.part` (to `resume` later). |

### `HttpRequest` (what every call returns; keeping it is optional)

Read-only: `id` (unique, increasing), `url` (as sent, with the query), `method`, `state`
(`HttpState.Queued`, `Running`, `Done`, `Cancelled`), `progress` (0..1, -1 unknown),
`bytesReceived`, `bytesTotal` (-1 unknown), `response` (the `HttpResponse` once done or
cancelled, else nil). Methods: `req:isDone()` (Done or Cancelled), `req:cancel()` (idempotent:
a queued or running request ends as cancelled at once and its callback runs with
`errorCode = "cancelled"` in a following frame; once the response has arrived, even if its
callback has not run yet, cancel does nothing).

With coroutines, `Coroutine.waitFor(req)` (any object with `isDone`) waits for it; then read
`req.response`.

### `HttpResponse` (read-only)

| Field | Meaning |
|---|---|
| `ok` | true only for status 200-299, read whole (and decoded, for `json` / textures) |
| `status` | the HTTP status; 0 when no response came (timeout, dns, ...) |
| `errorCode` | `""` when ok; see the table below |
| `error` | `""` when ok; what failed and the URL: `"timeout after 10 s: https://x/y"` |
| `headers` | a new table: lowercase name -> value (repeated headers joined with `", "`, `set-cookie` with `"\n"`) |
| `body` | the body as a string (`""` for HEAD, downloads and textures; a failed download's error body is here) |
| `data` | `responseType = "json"`: the decoded body, else nil |
| `textureData` | `Http.getTexture`: the image, else nil |
| `file` | a download's path (as given) |
| `url`, `redirected` | the final URL; a redirect was followed |
| `method`, `elapsed`, `bytesReceived` | the request's method; seconds from start to end; body bytes |
| `res:json()` | `Json.decode(res.body)`: the value, or `nil, message` |
| `res:header(name)` | one header, any case, or nil |

`tostring(res)` is `HttpResponse(200 GET https://...)`.

### Error codes (stable strings)

| `errorCode` | Meaning |
|---|---|
| `http` | a response that is not 2xx (`status` says which; `body` has the server's text) |
| `timeout` | the request took longer than `timeout` (or could not connect within `connectTimeout`) |
| `dns` | the host name did not resolve (no network often looks like this) |
| `connect` | could not connect (nothing listening, refused, unreachable) |
| `tls` | the TLS handshake or the certificate failed |
| `network` | another transport failure (connection reset, the server hung up); on the web also CORS |
| `cancelled` | `req:cancel()` / `Http.cancelAll()` |
| `offline` | the Game view's Network is set to Offline, or the OS says there is no connection |
| `insecure_http` | plain `http://` refused (rule 4), or blocked by iOS App Transport Security |
| `too_many_redirects` | more hops than `redirects` allows |
| `redirect_blocked` | web only: `redirects = false` and the server redirected (browsers hide the 3xx) |
| `invalid_url` | not `http[s]://host[:port]/...` (spaces must be encoded: `Http.encodeUrl`) |
| `file` | a download path outside `data://`, or the file could not be written |
| `decode` | `getTexture` got no image, `responseType = "json"` got no JSON |
| `too_large` | an in-memory body larger than `maxBodySize` |
| `unsupported` | no HTTP stack (Linux without libcurl), or an invalid method |

## Examples

```lua
-- GET a CSV with a 10 s timeout (Unity: UnityWebRequest.Get + timeout)
Http.get("https://gamedata.example.com/AdData.csv", {timeout = 10, owner = self}, function(res)
    if res.ok then self:parseCsv(res.body) else Log.warn("config: " .. res.error) end
end)

-- POST JSON analytics (a table body is JSON)
Http.post("https://backend.example.com/Interact/Welcome", {playerID = self.playerId}, function(res)
    if not res.ok then Log.warn("analytics " .. res.errorCode) end
end)

-- server time as JSON
Http.get("https://backend.example.com/Now", {responseType = "json", owner = self}, function(res)
    if res.ok then self.serverTime = res.data.time end
end)

-- connectivity check every 10 s (from onUpdate with an unscaled timer, or a coroutine)
Http.head("https://www.google.com", {timeout = 5, owner = self}, function(res)
    self.online = res.ok
end)

-- a chain of endpoints: errors are values, so "try the next one" is a plain if
local endpoints = {"https://a.example/geo", "https://b.example/geo", "https://c.example/geo"}
local function tryGeo(self, i)
    if i > #endpoints then return end
    Http.get(endpoints[i], {timeout = 5, responseType = "json", owner = self}, function(res)
        if res.ok and res.data.country then self.country = res.data.country else tryGeo(self, i + 1) end
    end)
end
tryGeo(self, 1)

-- an ad video and its icon, with progress
local id = "promo42"
Http.download("https://cdn.example.com/ads/" .. id .. ".mp4", "data://ads/" .. id .. ".mp4", {
    owner = self,
    onProgress = function(req, got, total) if total > 0 then self.bar.value = got / total end end,
}, function(res) self.videoReady = res.ok end)

-- a texture from a URL into an Image (Unity: UnityWebRequestTexture.GetTexture + Sprite.Create)
Http.getTexture("https://cdn.example.com/icons/" .. id .. ".png", {owner = self}, function(res)
    if res.ok then Image(self.scene, self.iconEntity):setTexture(res.url, res.textureData) end
end)
-- or, once downloaded: image:setTexture("data://ads/" .. id .. ".png")
```

`TextureData` follows the engine's rule: its pixels belong to the texture that uses it
(`setTexture(id, textureData)`; the id names it in the texture pool, so use the URL or another
unique name). A `textureData` read but never used keeps its pixels until
`textureData:releaseImageData()`; one never read is freed with the response.

### C++ scripts

```cpp
#include "net/Http.h"

HttpRequestDesc desc;
desc.url = "https://backend.example.com/Now";
desc.timeout = 10;
Http::send(desc, [this](const HttpResponse& r) {
    if (r.ok) Log::print("now: %s", std::string(r.body.begin(), r.body.end()).c_str());
}, {}, AsyncOwners::fromScript(this));   // cancelled with this script (or fromEntity(getScene(), getEntity()))
```

`HttpRequestDesc` has `method`, `url`, `headers`, `query`, `body`, `timeout`, `connectTimeout`,
`maxRedirects`, `verifyTls`, `responseType` (`Text`, `Binary`, `Json`, `Texture`, `File`),
`filePath`, `maxBodySize`, `resume`, `keepPartial`; `Http::send` returns a
`std::shared_ptr<HttpRequestHandle>` (`state()`, `isDone()`, `progress()`, `cancel()`,
`response()`). Callbacks run at the same point as Lua's. Other engine work that finishes on
another thread uses the same delivery: `MainThreadQueue::post(task)` (engine/core/thread)
runs `task` on the main thread before the next `onUpdate`, and `AsyncOwners` (engine/core/script)
gives it the same owner rules.

## Platforms

- **Web**: requests are the browser's `fetch()`, so **CORS** applies: a server of another origin
  must send `Access-Control-Allow-Origin` or the request fails with `network` ("network error or
  CORS refused"). The browser sets `User-Agent`, `Cookie`, `Referer` and other forbidden
  headers itself (yours are dropped) and always checks certificates (`verifyTls` is ignored).
  `redirects = false` gives `redirect_blocked`; a numeric hop limit is the browser's (20).
  A download waits for the saved files to be restored (`System.isUserDataLoaded()`) and is
  stored in IndexedDB when it completes. The page is `https://` on most hosts: browsers block
  `http://` requests from it (mixed content) whatever the setting.
- **Android**: the export adds the INTERNET permission when the exported Lua (also inside HTML
  UI documents) or C++ uses `Http`; Project Settings > Android > **Internet Access** `Require`
  (`android.internet_access: "require"`) always adds it (needed when only a native plugin uses
  the network). Android 7.0 and older get the Let's Encrypt roots (ISRG Root X1/X2) added to
  their trust store. The cleartext policy follows Allow Insecure HTTP
  (`res/xml/network_security_config.xml`; localhost always allowed).
- **iOS / macOS**: App Transport Security follows Allow Insecure HTTP (`NSAllowsArbitraryLoads`
  when on, else `NSAllowsLocalNetworking` for localhost). iOS may suspend transfers in the
  background: they end with `network` or `timeout`, delivered after resume. Repeated
  `Set-Cookie` headers arrive joined with `", "`.
- **Linux**: needs the system libcurl (`libcurl.so.4` or `libcurl-gnutls.so.4`; Debian/Ubuntu
  `libcurl4`). Certificates are the distribution's.
- **Windows**: WinHTTP, the system proxy and certificate store.

Not provided: WebSockets, server-sent events, a cookie jar (read `res.headers["set-cookie"]`
and send `Cookie` yourself), multipart uploads (build the body yourself and set
`Content-Type`), background downloads that outlive the app, an HTTP cache.

## Testing (editor and MCP)

- **Offline**: the Game view toolbar's Network button (wifi icon) > Offline (simulated), or MCP
  `configure_game_view {network: "offline"}` (works while playing): every request to a host
  other than localhost fails with `errorCode = "offline"` after one frame, running ones too.
  Editor only, never in builds; back to Online when the editor restarts.
- **A local server**: plain `http://127.0.0.1:<port>/` and `http://localhost` always work, e.g.
  `python3 -m http.server 8000` in a folder.
- **`get_play_state`** has an `http` block while playing: `active`, `queued`, `finished`,
  `failed`, `cancelled`, `offline` and `last_failures` (the last 5: `method`, `url`, `status`,
  `error_code`, `error`). `Http.logFailures = true` logs each failure to the Console.
- Play stop ends every request without its callback.

## Unity -> JasperEngine

| Unity | JasperEngine |
|---|---|
| `UnityWebRequest.Get(url)` + `SendWebRequest()` | `Http.get(url, callback)` |
| `UnityWebRequest.Post / Put / Delete / Head` | `Http.post(url, body, cb)` / `Http.put` / `Http.delete` / `Http.head` |
| `new UnityWebRequest(url, method)` | `Http.request{url = url, method = method, onComplete = cb}` |
| `SetRequestHeader(name, value)` | `options.headers = {[name] = value}` |
| `timeout` (seconds) | `options.timeout` (seconds) |
| `redirectLimit` | `options.redirects` |
| `certificateHandler` (accept all) | `options.verifyTls = false` |
| `DownloadHandlerBuffer.text` / `.data` | `res.body` |
| `DownloadHandlerFile` | `Http.download(url, "data://...")` |
| `DownloadHandlerTexture` / `UnityWebRequestTexture.GetTexture` + `Sprite.Create` | `Http.getTexture(url, cb)`, `image:setTexture(res.url, res.textureData)` |
| `downloadProgress` | `req.progress`, `options.onProgress` |
| `Abort()` | `req:cancel()` |
| `result` / `error` / `responseCode` | `res.ok` + `res.errorCode`, `res.error`, `res.status` |
| `GetResponseHeader(name)` | `res:header(name)` |
| `yield return www.SendWebRequest()` | `Coroutine.waitFor(req)` (coroutines), then `req.response` |
| `UnityWebRequest.EscapeURL` / `UnEscapeURL` | `Http.encodeUrl` / `Http.decodeUrl` |
| Player setting *Internet Access* | `android.internet_access` (`auto` / `require`) |
| Player setting *Allow downloads over HTTP* | `player.allow_insecure_http` |
