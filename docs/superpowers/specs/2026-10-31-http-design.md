# `http`: HTTP(S) client for Lua and C++ (design)

Status: binding spec for branch `gaps/http` (ledger #1). Read with
[AGENT-BRIEF.md](../../engine-gaps/AGENT-BRIEF.md) (its shared conventions are binding),
[GAP-REPORT.md §1](../../engine-gaps/GAP-REPORT.md). Merges **after** `script-data` (uses its `Json`).

## 1. Goals

1. One HTTP(S) client usable from Lua (`Http.*`) and C++ (`jasperengine::Http`), identical on Linux,
   Windows, macOS, iOS, Android, Web and in editor Play mode.
2. Methods GET, POST, PUT, PATCH, DELETE, HEAD (any token method allowed); request headers, query
   parameters, text / JSON / binary / form bodies; response status, headers, final URL, body as
   binary-safe string, JSON (through `Json.decode`), decoded texture, or a file.
3. Timeouts (connect and total), redirect control, TLS verification on by default, gzip.
4. Download to a file under the user data directory with progress and cancel (the game's mp4/PNG
   ad downloads, `RequestMyAds.cs:104`, `RequestMyIcons.cs:75`).
5. Texture from a URL in one call (`Texture2D.LoadImage` + `Sprite.Create` replacement).
6. Many concurrent requests; every result delivered **on the main thread** at a defined frame point.
7. Errors are values (a response with `ok=false`, `errorCode`, `error`), never Lua errors, except
   for programming errors (wrong argument types).
8. Android INTERNET permission added automatically when the project uses HTTP (project setting
   `android.internet_access`: auto / require); cleartext HTTP policy shared by every platform.
9. A shared main-thread dispatch queue and owner tracking that `native-bridge` (and `coroutines`)
   reuse.

## 2. Non-goals

- WebSockets, server-sent events, HTTP/3 control, raw TCP/UDP sockets.
- A persistent cookie jar (cookies set by a response are not resent automatically; read
  `headers["set-cookie"]` and send `Cookie` yourself). Browsers keep their own cookies on Web.
- Multipart/form-data file uploads (`form` is `application/x-www-form-urlencoded` only); a raw body
  with a caller-built multipart payload and its `Content-Type` works.
- Background downloads that continue after the app is killed (iOS background sessions, Android
  WorkManager).
- HTTP caching (`Cache-Control`) beyond what the platform stack does by itself; no engine cache.
- Changing the editor's existing curl-executable importer (`editor/htmlui/HttpClient.*`): untouched.
- HTML UI `fetch()`/`XMLHttpRequest` inside documents (still "no network in documents";
  documents call Lua, Lua calls `Http`).

## 3. Users and stories

- **Porting dev (human).** "My Unity `RequestServerInfo` GETs a CSV with a 10 s timeout": 
  `Http.get(url, {timeout = 10, owner = self}, function(res) if res.ok then parse(res.body) end end)`.
- **Analytics.** `Http.post("https://backend.arakuma.io/Interact/Welcome", {playerID = id}, cb)` sends
  JSON (table body = JSON, `Content-Type: application/json`).
- **Connectivity check** (`InternetChecker.cs`): `Http.head("https://www.google.com", {timeout = 5}, cb)`
  every 10 s from an unscaled timer.
- **Ad assets.** `Http.download(url, "data://ads/" .. id .. ".mp4", {onProgress = f}, cb)`, later
  `image:setTexture("data://ads/" .. id .. ".png")`; or `Http.getTexture(iconUrl, cb)` straight to a
  `TextureData`.
- **Server time** (`GShopManager:41`): `Http.get("https://backend.arakuma.io/Now", cb)`;
  `local t = res:json()`.
- **Geo-IP chain** (`RequestUserCountry:57-135`): try four endpoints in sequence, each with a 5 s
  timeout; any failure is a value, so the chain is plain `if not res.ok then next() end`.
- **AI agent (MCP).** Writes the Lua above, plays, reads `get_play_state` (`http` block: active,
  finished, failed counts and the last failures), toggles the Game view network to `offline`
  (`configure_game_view {network: "offline"}`) to test the offline path, and tests against a local
  server (localhost is always allowed over plain HTTP).
- **C++ script author.** `Http::send(HttpRequestDesc{...}, [](const HttpResponse& r){...})`.

## 4. Research summary

| Engine | API | Taken / left |
|---|---|---|
| Unity `UnityWebRequest` ([docs](https://docs.unity3d.com/ScriptReference/Networking.UnityWebRequest.html)) | `Get/Post/Put/Delete/Head`, `SetRequestHeader`, `timeout` (s), `redirectLimit` (32), `certificateHandler`, `DownloadHandlerBuffer/File/Texture`, `downloadProgress`, `Abort()`, `result` enum (`ConnectionError`, `ProtocolError`, `DataProcessingError`), yield in coroutine. Player setting *Internet Access: Auto/Require* and *Allow downloads over HTTP* ([Player settings](https://docs.unity3d.com/Manual/class-PlayerSettingsAndroid.html)). | Take: verb shortcuts, seconds-based timeout, redirect limit, file/texture download handlers, progress, abort, errors as a result category, Internet Access Auto/Require, one HTTP-allowed setting. Leave: object-per-request builder + `Dispose`, certificate callbacks with raw DER (replaced by a bool). |
| Godot `HTTPRequest` node / `HTTPClient` ([docs](https://docs.godotengine.org/en/stable/classes/class_httprequest.html)) | node signal `request_completed(result, code, headers, body)`, `download_file`, `timeout`, `max_redirects`, `use_threads`, `TLSOptions.client_unsafe()`. | Take: one completion callback with result + code + headers + body, download_file, unsafe TLS option. Leave: a node per request (scene objects for transport). |
| Defold `http.request(url, method, cb, headers, post_data, options)` ([docs](https://defold.com/ref/stable/http/)) | callback `(self, id, response)` with `status`, `response`, `headers`, `error`, `path` (download to file), `timeout`, `ignore_cache`, `chunked_transfer`; status 0 on transport failure. | Take: table response, `status = 0` when no HTTP response, `path` download. |
| Unreal `FHttpModule` ([docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/http-requests-in-unreal-engine)) | backends: libcurl (Linux, Android), WinHttp (Windows), NSURLSession (Apple), XHR/fetch (HTML5 era); `OnProcessRequestComplete`, `OnRequestProgress64`. | Take: per-platform system backends and completion + progress delegates on the game thread. |
| Cocos `XMLHttpRequest` / `HttpClient` | JSB `XMLHttpRequest`, libcurl/okhttp backends. | Confirms system-stack choice; leave XHR API shape. |
| Emscripten `emscripten_fetch` ([docs](https://emscripten.org/docs/api_reference/fetch.html)) | XHR-based, `-sFETCH`, progress, abort. | Leave: JS `fetch()` + `AbortController` via `EM_JS` gives redirect mode, streaming progress and timeouts without `-sFETCH`. |

Backend decision (product decision 3) follows Unreal: each platform's own stack, so TLS, CA
updates, proxies and IPv6 come from the OS.

## 5. Product decisions

1. **Namespace `Http`** (static class, like `Physics`): `Http.get/post/put/patch/delete/head/request/download/getTexture`.
   Short, Unity users map `UnityWebRequest.Get` -> `Http.get` (glossary row).
2. **Callback style + handle.** Every call returns an `HttpRequest` handle (cancel, progress,
   `isDone()`, `response`) and takes an optional completion callback `function(response)`. Works
   without coroutines; with the `coroutines` feature, `Coroutine.waitFor(req)` / `coroutine.yield(req)`
   (its awaitable protocol: any object with a callable `isDone`, coroutines spec 6.5) gives
   `yield return www.SendWebRequest()` semantics; `req.response` is then set.
3. **Backends per platform** (all asynchronous, results posted to the main thread):
   - **Linux (exports and Linux editor): `dlopen` of the system libcurl** (`libcurl.so.4`, then
     `libcurl-gnutls.so.4`, then `libcurl.so`), driven by one `curl_multi` worker thread. Why not a
     vendored curl + mbedTLS: +1.5-3 MB per binary, a TLS stack the project must patch, and CA
     bundles that live at different paths per distro (`/etc/ssl/certs/ca-certificates.crt`,
     `/etc/pki/tls/certs/ca-bundle.crt`, ...) and go stale; the system libcurl is linked against
     the distro's OpenSSL/GnuTLS and its CA store and is updated by the OS. libcurl 7.x/8.x
     ships on every desktop distro and in the Steam Runtime; no development headers are needed (the
     few `CURLOPT_*`/`CURLINFO_*` constants used are ABI-stable and declared in
     `CurlApi.h`). If no libcurl loads, `Http.isAvailable()` is false and every request completes
     with `errorCode = "unsupported"` and the message `libcurl not found: install libcurl4 (e.g. apt
     install libcurl4)`; logged once as a warning.
   - **Windows: WinHTTP** (Schannel, the Windows certificate store, automatic proxy
     `WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY` on 8.1+ falling back to `DEFAULT_PROXY`), synchronous
     WinHTTP calls on a pool of worker threads (size = `Http.maxConcurrentRequests`), cancel by
     closing the request handle. Linked `winhttp`.
   - **macOS and iOS: `NSURLSession`** (one session, `NSURLSessionDataDelegate` on a private
     `NSOperationQueue`), ATS applies (decision 8).
   - **Android: `HttpURLConnection`** in a new Java class `JasperHttp` (thread pool executor),
     calls and callbacks through JNI. On API < 25 the trust store adds the bundled ISRG Root X1 and
     X2 certificates to the system ones (Let's Encrypt sites fail on Android 7.0 and older without
     them: the reason the game installed an accept-all handler on Android <= 7).
   - **Web: JS `fetch()`** with `AbortController`, from `EM_JS` functions in
     `engine/platform/emscripten/HttpBackendWeb.cpp`; CORS applies (documented).
   - **Editor Play:** the host desktop backend (Linux libcurl, Windows WinHTTP, macOS NSURLSession).
4. **Errors as values.** The callback always runs exactly once per request (unless the request's
   owner was destroyed, decision 6). `response.ok` is true only for a received response with status
   200-299. A received 404 is `ok=false, errorCode="http", status=404` with its body.
5. **Main-thread delivery point:** in `Engine::runFrame`, inside the update block (not while the
   app is paused, the editor Play is paused, or the MCP clock holds the simulation), after the fixed
   steps and **immediately before `Engine::onUpdate`**. Progress callbacks are coalesced to at most
   one call per request per frame and always precede the completion callback.
6. **Owners and scene loads.** `owner` option: a script instance table (`self`), a `GameObject`/any
   `EntityHandle`, or a `Scene`. With an owner, the request is cancelled when the instance is released
   (the coroutines ownership rule: destroy, scene unload, Play stop), the entity is destroyed or the
   scene is destroyed, and **no callback runs**. Without an owner the request is global: it survives scene loads (analytics, downloads of a
   persistent manager) and ends only on completion, `cancel()`, `Http.cancelAll()`, Play stop or
   engine shutdown. Docs and examples always pass `owner = self` in per-scene scripts.
7. **Time.** Timeouts are wall-clock seconds, independent of `Engine.timeScale` (like Unity).
   Defaults: `timeout = 30` (total), `connectTimeout = 15`.
8. **Plain HTTP policy, same everywhere.** Project setting `player.allow_insecure_http`
   (default `false`): when false, an `http://` URL whose host is not `localhost`, `127.0.0.1` or
   `::1` fails with `errorCode = "insecure_http"` on every platform including the editor (no
   editor-only behaviour). Export writes the matching Android `network_security_config.xml`
   and iOS `NSAppTransportSecurity` (`NSAllowsLocalNetworking` always, `NSAllowsArbitraryLoads` = the
   setting). Unity's equivalent: *Allow downloads over HTTP*.
9. **Android INTERNET permission:** setting `android.internet_access` = `auto` (default) | `require`.
   `require` always adds `android.permission.INTERNET`; `auto` adds it when the export finds HTTP use
   (section 7.10) or the permission is ticked in `android.permissions`. Plus
   `ACCESS_NETWORK_STATE` when `System.getInternetReachability` is used (that API belongs to
   `platform-services`; the same scanner serves both).
10. **TLS verification** is on; per-request `verifyTls = false` disables it (logs one warning per
    host); ignored with a warning on Web (browsers do not allow it).
11. **Simulated offline** in the editor: Game view toolbar Network menu (Online / Offline) and MCP
    `configure_game_view {network}`; offline fails every non-localhost request with
    `errorCode = "offline"` after one frame. Editor-only *simulation* (like Simulate Touch), never
    saved into builds.

## 6. Design

### 6.1 Runtime C++

New directory `engine/core/net/` (add every `.cpp` to `JASPERENGINE_SRCS` in
`engine/CMakeLists.txt:853+`, then `python3 tools/xcode_sync_engine.py` for the Xcode project).

**`engine/core/thread/MainThreadQueue.{h,cpp}`** (shared with `native-bridge`):

```cpp
namespace jasperengine {
class JASPERENGINE_API MainThreadQueue {
public:
    // any thread; never blocks on Lua; FIFO
    static void post(std::function<void()> task);
    // Engine::runFrame only (main thread): runs the tasks posted before this call, in order;
    // a task posting another task runs it next frame. Exceptions are caught and logged.
    static void drain();
    static size_t pending();
    // Play stop / shutdown: drops queued tasks without running them
    static void clear();
};
}
```
Implementation: `std::mutex` + `std::deque`; `drain()` swaps the deque out under the lock.
Called in `Engine::runFrame` (`engine/core/Engine.cpp:1428`) right before `Engine::onUpdate.call()`,
inside the `if (!paused && !simulationHeld)` block (`:1385`), and only when `Engine::onUpdate`
is enabled (`Engine::pauseGameEvents(false)`; expose `FunctionSubscribe::isEnabled()` if missing).
On Web (no threads) backends call `post` from JS callbacks; same path.

**`engine/core/script/AsyncOwner.{h,cpp}`** (shared with `native-bridge`; aligned with the
`coroutines` spec, whose `ScriptScheduler::cancelOwner(instance)` is called from
`LuaBinding::releaseLuaScriptInstance` (`LuaBinding.cpp:1054`)):

```cpp
struct AsyncOwner { uint64_t token = 0; };              // 0 = none
class JASPERENGINE_API AsyncOwners {
public:
    // a script instance table (the `self` of a Lua script: owner = that instance), an EntityHandle /
    // GameObject (owner = that entity), a Scene; nil -> none; anything else -> Lua error
    static AsyncOwner fromLua(lua_State* L, int index);
    static AsyncOwner fromEntity(Scene* scene, Entity entity);   // C++ scripts
    static AsyncOwner fromScriptInstance(const void* instance);  // Lua ref as void*, or ScriptBase*
    static bool alive(const AsyncOwner& owner);                  // none -> true
    static void scriptInstanceReleased(const void* instance);    // LuaBinding::releaseLuaScriptInstance, ~ScriptBase
    static void sceneDestroyed(Scene* scene);                    // Scene::~Scene (Scene.cpp:60)
    static void entityDestroyed(Scene* scene, Entity e);         // EntityRegistry::destroyEntity (registry/EntityRegistry.cpp:261)
    static FunctionSubscribe<void(uint64_t token)>& onOwnerGone();
};
```
A script instance owner ends exactly when coroutines' owned work ends (instance released: destroy,
scene unload, Play stop): put the `AsyncOwners::scriptInstanceReleased` call next to coroutines'
`cancelOwner` call (whichever feature merges second adds its call beside the other). Entity ids are
reused, so liveness is the token's registry entry, removed (firing `onOwnerGone`) by the hooks.
`fromLua` identifies a script instance by the registry ref the engine keeps for it (the table is
looked up in the instance map `LuaBinding` maintains; a plain table that is not a script instance
-> Lua error "owner must be a script instance (self), a GameObject or a Scene").

**`engine/core/net/Http.h`** (public C++ API):

```cpp
namespace jasperengine {
enum class HttpResponseType { Text, Binary, Json, Texture, File };   // Text/Binary/Json: body in memory
enum class HttpState { Queued, Running, Done, Cancelled };
struct HttpRequestDesc {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;   // order kept, duplicates allowed
    std::vector<std::pair<std::string, std::string>> query;     // appended, percent-encoded (RFC 3986 unreserved kept)
    std::vector<uint8_t> body;                                  // empty = no body
    double timeout = 30.0;          // s, whole request; <= 0 = none
    double connectTimeout = 15.0;   // s
    int maxRedirects = 10;          // 0 = do not follow (3xx returned as is)
    bool verifyTls = true;
    HttpResponseType responseType = HttpResponseType::Binary;
    std::string filePath;           // File: engine path (data://...) resolved by FileData::getSystemPath
    size_t maxBodySize = 64u << 20; // in-memory bodies; past it: errorCode "too_large"
    bool reportProgress = false;
};
struct HttpResponse {
    bool ok = false;
    int status = 0;                   // 0 = no HTTP response
    std::string errorCode;            // "" when ok; see 6.3
    std::string error;                // human message, "" when ok
    std::vector<std::pair<std::string, std::string>> headers;   // names lowercased, in order
    std::string url;                  // final URL after redirects
    bool redirected = false;
    std::vector<uint8_t> body;        // empty for File/Texture
    std::string filePath;             // File: where it was written (engine path as given)
    std::shared_ptr<TextureData> texture;  // Texture: decoded on the worker
    double elapsed = 0.0;             // s, start to completion
    uint64_t bytesReceived = 0;
};
class JASPERENGINE_API HttpRequestHandle {
public:
    uint64_t id() const; HttpState state() const; bool isDone() const;
    uint64_t bytesReceived() const; int64_t bytesTotal() const;     // -1 unknown
    float progress() const;                                         // 0..1, -1 unknown
    void cancel();                                                  // idempotent
    const HttpResponse* response() const;                           // null until Done/Cancelled
};
using HttpCallback = std::function<void(const HttpResponse&)>;
using HttpProgress = std::function<void(uint64_t received, int64_t total)>;
class JASPERENGINE_API Http {
public:
    static std::shared_ptr<HttpRequestHandle> send(HttpRequestDesc desc, HttpCallback onComplete,
                                                   HttpProgress onProgress = {}, AsyncOwner owner = {});
    static bool isAvailable();
    static void cancelAll();                       // callbacks of cancelled requests run with "cancelled"
    static void shutdown();                        // Play stop / systemShutdown: cancel, drop callbacks
    static int getMaxConcurrentRequests(); static void setMaxConcurrentRequests(int n); // 1..64, default 8
    static std::string getUserAgent(); static void setUserAgent(const std::string&);  // default "JasperEngine/<version> (<platform>)"
    static double getDefaultTimeout(); static void setDefaultTimeout(double s);
    static bool isOfflineSimulated(); static void setOfflineSimulated(bool);           // editor only sets it
    static bool isInsecureHttpAllowed(); static void setInsecureHttpAllowed(bool);     // from player.allow_insecure_http
    struct Stats { uint32_t active, queued, finished, failed; std::vector<HttpResponse> lastFailures; }; // last 5, bodies dropped
    static Stats getStats();
};
}
```

**Backend interface** `engine/core/net/HttpBackend.h` (internal):

```cpp
struct HttpBackendCallbacks {             // backends call these from any thread
    std::function<void(uint64_t id, uint64_t received, int64_t total)> progress;
    std::function<void(uint64_t id, HttpResponse&& response)> complete;   // exactly once per started id
};
class HttpBackend {
public:
    virtual ~HttpBackend() = default;
    virtual bool available(std::string& whyNot) = 0;
    virtual void start(uint64_t id, const HttpRequestDesc& desc) = 0;  // main thread
    virtual void cancel(uint64_t id) = 0;                               // main thread; completes with "cancelled"
    virtual void shutdown() = 0;                                        // joins workers
};
HttpBackend* createPlatformHttpBackend(const HttpBackendCallbacks&);   // one definition per platform
```
`Http` (in `Http.cpp`) owns the queue (FIFO, `maxConcurrentRequests` running), validation,
the insecure-HTTP / offline checks (these fail synchronously into the main-thread queue, never
reaching the backend), redirect-count enforcement where the backend reports hops, body size
limits, texture decode (`TextureData::loadTextureFromMemory`, `engine/core/texture/TextureData.h:59`,
run on the backend's worker thread before `complete`), file writing for backends that stream into
memory, progress coalescing, owner cancellation (subscribes to `AsyncOwners::onOwnerGone`), and
posts completions with `MainThreadQueue::post`.

Files per platform (selected in `engine/CMakeLists.txt` platform blocks and the editor build):

| Platform | File | Notes |
|---|---|---|
| Linux | `engine/core/net/HttpBackendCurl.cpp`, `CurlApi.h` | `dlopen` + `dlsym` of `curl_global_init`, `curl_easy_*`, `curl_multi_*`, `curl_slist_*`; options `URL, CUSTOMREQUEST, NOBODY (HEAD), HTTPHEADER, POSTFIELDS(SIZE_LARGE), FOLLOWLOCATION, MAXREDIRS, TIMEOUT_MS, CONNECTTIMEOUT_MS, SSL_VERIFYPEER/HOST, ACCEPT_ENCODING "", USERAGENT, WRITEFUNCTION, HEADERFUNCTION, XFERINFOFUNCTION, NOPROGRESS, NOSIGNAL 1, PRIVATE`; `CURLINFO_RESPONSE_CODE, EFFECTIVE_URL, REDIRECT_COUNT`. Map `CURLE_OPERATION_TIMEDOUT`->timeout, `COULDNT_RESOLVE_HOST`->dns, `COULDNT_CONNECT`->connect, `PEER_FAILED_VERIFICATION/SSL_*`->tls, `TOO_MANY_REDIRECTS`->too_many_redirects, others->network. Links `${CMAKE_DL_LIBS}`. Also compiled for FreeBSD. |
| Windows | `engine/platform/win/HttpBackendWin.cpp` | `WinHttpOpen` (UA, automatic proxy), `WinHttpConnect`, `WinHttpOpenRequest` (`WINHTTP_FLAG_SECURE` for https), `WinHttpSetTimeouts`, `WINHTTP_OPTION_REDIRECT_POLICY` (never when maxRedirects=0, else ALWAYS with manual hop counting via `WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS`), `WINHTTP_OPTION_DECOMPRESSION` (8.1+), `SECURITY_FLAG_IGNORE_*` for verifyTls=false, `WinHttpQueryHeaders(WINHTTP_QUERY_RAW_HEADERS_CRLF)`, `WINHTTP_OPTION_URL` for the final URL. Errors: `ERROR_WINHTTP_TIMEOUT`->timeout, `NAME_NOT_RESOLVED`->dns, `CANNOT_CONNECT`->connect, `SECURE_FAILURE`->tls. The editor on Windows links it too. |
| macOS + iOS | `engine/platform/apple/HttpBackendApple.mm` | Shared by `JasperEngineApple` (Xcode iOS/macOS targets), the CMake macOS backend and the macOS editor; add to the Xcode engine targets by hand (the sync script covers `core/` only). `NSURLSessionConfiguration.defaultSessionConfiguration`, `timeoutIntervalForRequest = connectTimeout`, overall timer via `dispatch_after`; `willPerformHTTPRedirection` counts hops and returns nil past `maxRedirects`; `didReceiveChallenge` accepts the server trust when `verifyTls=false`. Errors: `NSURLErrorTimedOut`->timeout, `CannotFindHost/DNSLookupFailed`->dns, `CannotConnectToHost`->connect, `ServerCertificate*/SecureConnectionFailed`->tls, `AppTransportSecurityRequiresSecureConnection`->insecure_http, `NotConnectedToInternet`->offline. |
| Android | `engine/platform/android/HttpBackendAndroid.cpp`, `engine/platform/android/java/com/jaspergameengine/jasperengine/JasperHttp.java` | Java: `static void start(long id, String method, String url, String[] headers, byte[] body, int timeoutMs, int connectTimeoutMs, int maxRedirects, boolean verifyTls, String filePath, boolean progress)`, `static void cancel(long id)`; redirects followed manually (`setInstanceFollowRedirects(false)`, also across http->https which `HttpURLConnection` refuses to follow); bodies accumulate in a `ByteArrayOutputStream`, files are streamed by Java to `filePath + ".part"` then renamed; callbacks are `native` methods registered with `RegisterNatives` in `NativeEngine::setupJNI` (`NativeEngine.cpp:817`): `onProgress(long id, long received, long total)`, `onComplete(long id, int status, String errorCode, String message, String finalUrl, String[] headers, byte[] body)`. The `jclass` is a global ref cached at setup (JNI `FindClass` fails on non-Java threads). Errors: `SocketTimeoutException`->timeout, `UnknownHostException`->dns, `ConnectException`->connect, `SSLException`->tls. API < 25: `TrustManagerFactory` over a `KeyStore` holding the system CAs (`AndroidCAStore`) plus ISRG Root X1/X2 PEMs embedded in `JasperHttp.java`. |
| Web | `engine/platform/emscripten/HttpBackendWeb.cpp` | `EM_JS(void, jasper_http_start, (double id, const char* method, const char* url, const char* headersJson, const uint8_t* body, int bodyLen, int timeoutMs, int maxRedirects, int progress))`; JS: `fetch(url, {method, headers, body, redirect: maxRedirects == 0 ? 'manual' : 'follow', signal})`, `AbortController` + `setTimeout` for the timeout, streamed read of `response.body` for progress, completion through `EMSCRIPTEN_KEEPALIVE` C functions `jasper_http_on_progress(double id, double received, double total)` and `jasper_http_on_complete(double id, int status, const char* errorCode, const char* message, const char* finalUrl, const char* headersJson, uint8_t* body, int len)` (JS allocates `body` with `_malloc`, C frees it). Add `'_malloc','_free'` to `EXPORTED_FUNCTIONS` (`engine/CMakeLists.txt` Web link flags and the exported CMake config the Generator writes). Files: C writes the body to the FS path, then `System::syncFileSystem()`. `TypeError` from fetch -> errorCode `network` with message "network error or CORS refused (the server must send Access-Control-Allow-Origin)"; `opaqueredirect` -> `redirect_blocked`. |

The `HttpBackend` for a headless/test `System` (editor unit tests) is selected by the build
platform as above; tests use a local server (section 9).

### 6.2 Lua API (`engine/core/script/binding/NetClassesLua.cpp`, registered from `LuaBinding::registerClasses`, `LuaBinding.cpp:635`)

All functions are static on `Http`. `options` is a table; unknown keys log a warning once per key
name (catches typos like `timout`). `callback` may be nil.

| Function | Meaning |
|---|---|
| `Http.request(options) -> HttpRequest` | Full form. `options.url` (string, required), `method` (default `"GET"`, uppercased), plus every option below and `onComplete` (= callback). |
| `Http.get(url [, options] [, callback])` | GET. |
| `Http.head(url [, options] [, callback])` | HEAD (no body). |
| `Http.delete(url [, options] [, callback])` | DELETE. |
| `Http.post(url, body [, options] [, callback])` | POST with `body` (see body rules). |
| `Http.put(url, body [, options] [, callback])` / `Http.patch(...)` | PUT / PATCH. |
| `Http.download(url, path [, options] [, callback])` | GET into a file; `path` is an engine path (`data://ads/x.mp4`) or an absolute path inside `System.getUserDataPath()`; anything else -> `errorCode = "file"`. Parent folders are created. Written to `<path>.part`, renamed on success, removed on failure/cancel; an existing file is replaced only on success. |
| `Http.getTexture(url [, options] [, callback])` | GET + decode (PNG, JPEG, BMP, TGA, GIF first frame, the formats `TextureData` loads); `response.textureData` is a `TextureData`; set it with `image:setTexture(response.url, response.textureData)` (existing overload `Image::setTexture(id, TextureData)`). Decode failure -> `errorCode = "decode"`. |
| `Http.cancelAll()` | Cancels every request (their callbacks run with `"cancelled"`). |
| `Http.isAvailable() -> bool` | False only when the platform backend cannot work (Linux without libcurl). |
| `Http.maxConcurrentRequests` | rw property, 1..64, default 8; extra requests wait in FIFO order (`state = HttpState.Queued`). |
| `Http.userAgent` | rw string. Ignored on Web (browser's). |
| `Http.timeout` | rw default total timeout in seconds (30). |
| `Http.logFailures` | rw bool, default false (section 6.5). |
| `Http.encodeUrl(s) -> string`, `Http.decodeUrl(s) -> string` | Percent-encoding helpers (RFC 3986 unreserved kept; `decodeUrl` turns `+` into space). |

**Argument forms.** `options` and `callback` are positional but either may be omitted: a function
in the options position is the callback. A non-string `url` or `path`, a non-table `options`, or a
non-function `callback` raises a Lua error (`Http.get: url must be a string`), the only Lua errors.

**Options** (all optional):

| Key | Type / default | Meaning |
|---|---|---|
| `headers` | table name -> string, or array of `{name, value}` | `Content-Type` etc. Names are sent as given. |
| `query` | table name -> string/number/bool | appended to the URL (`?` or `&`), sorted by name for determinism. |
| `body` | string | raw body (binary-safe) for `request`. |
| `json` | any JSON value | encoded with `Json.encode` (script-data), `Content-Type: application/json` unless set. |
| `form` | table name -> scalar | `application/x-www-form-urlencoded`. |
| `timeout` | number, `Http.timeout` | seconds, whole request; `0` = none. |
| `connectTimeout` | number, 15 | seconds. |
| `redirects` | bool or integer, true | `false`/`0`: do not follow; `true` = 10; n = at most n hops; past it `errorCode = "too_many_redirects"`. |
| `verifyTls` | bool, true | |
| `responseType` | `"text"` (default), `"binary"`, `"json"` | `text`/`binary` both fill `response.body` (Lua strings are binary-safe; the two differ only in docs intent). `json` also fills `response.data` with `Json.decode(body)`; a parse failure -> `ok = false, errorCode = "decode"`. |
| `maxBodySize` | integer bytes, 64 MiB | in-memory limit. |
| `onProgress` | `function(request, received, total)` | `total = -1` when unknown. Once per frame at most. |
| `owner` | `self` / GameObject / EntityHandle / Scene | decision 6. |
| `file` | string | for `request`: same as `download`'s path. |

**Body rules for `post/put/patch`**: string -> raw body (no default `Content-Type`); table ->
JSON (as `json`); `nil` -> empty body; anything else -> Lua error.

**`HttpRequest`** (userdata wrapping `std::shared_ptr<HttpRequestHandle>`; keeping it is optional,
the request runs without a reference): read-only properties `id` (integer), `url`, `method`,
`state` (`HttpState.Queued|Running|Done|Cancelled`), `progress`
(0..1, -1 unknown), `bytesReceived`, `bytesTotal` (-1 unknown), `response` (`HttpResponse` or nil);
methods `isDone()` (Done or Cancelled; a method, the coroutines awaitable protocol) and `cancel()`
(idempotent; a cancelled request's callback runs next frame with `errorCode = "cancelled"` unless the
request is already Done).

**`HttpResponse`** (userdata, read-only): `ok`, `status`, `errorCode` (string, `""` when ok),
`error` (string, `""` when ok), `headers` (fresh Lua table: lowercase name -> value; repeated
headers joined with `", "` except `set-cookie`, joined with `"\n"`), `url`, `redirected`, `body`
(string; `""` for HEAD, file and texture), `data` (decoded JSON for `responseType = "json"`, else
nil), `file` (path for downloads), `textureData` (`TextureData` or nil), `elapsed` (seconds),
`bytesReceived`; methods `json() -> value, err` (`Json.decode(body)`; on failure `nil, message`),
`header(name) -> string|nil` (case-insensitive). `tostring(response)` = `HttpResponse(200 GET https://...)`.

**Enum** `HttpState` (`Queued`, `Running`, `Done`, `Cancelled`).

### 6.3 Error codes (strings, stable, documented)

`http` (got a non-2xx status), `timeout`, `dns`, `connect`, `tls`, `network` (other transport
failure, CORS on Web), `cancelled`, `offline` (simulated or the OS reports no connection),
`insecure_http`, `too_many_redirects`, `redirect_blocked` (Web with redirects off), `invalid_url`
(unparseable or a scheme other than http/https), `file` (bad path or write failure), `decode`,
`too_large`, `unsupported` (no backend). `error` holds the platform's message prefixed with what
failed: `"timeout after 10 s: https://x/y"`.

### 6.4 C++ scripting

`#include "net/Http.h"` from C++ scripts (the engine include root is already on the script
include path). Callbacks run on the main thread at the same point. A C++ script passes
`AsyncOwners::fromScriptInstance(this)` (or `fromEntity(getScene(), getEntity())`) to get the
destroy rule.

### 6.5 Editor UX

- **Project Settings > Player** (shared section, `editor/window/dialog/ProjectSettingsWindow.cpp`):
  checkbox **Allow Insecure HTTP** (tooltip: "Plain http:// URLs other than localhost. Off: they
  fail with insecure_http on every platform; exports set Android cleartext and iOS ATS to match.").
- **Project Settings > Android**: combo **Internet Access** (`Auto` / `Require`) next to
  Permissions; help: "Auto adds INTERNET when Lua or C++ code uses Http (checked at export)".
- **Game view toolbar**: an icon button **Network** (`ICON_FA_WIFI`, highlighted when offline)
  with a menu `Online` / `Offline (simulated)`; tooltip; resets to Online when the editor
  restarts (not saved). `GameViewSettings` gains no saved field; the state lives on `Http`.
- **Console**: `Http.logFailures` (rw bool, default `false` everywhere, editor included): when true,
  each failed request logs one warning line `HTTP GET https://... failed: timeout after 10 s`.
  Default off because games retry and probe connectivity on purpose; agents read
  `get_play_state.http` instead. Add `logFailures` to the Lua table in 6.2.
- Script templates/snippets: the Lua code editor's completion picks up `Http` from the generated
  stubs automatically.

### 6.6 MCP / agent surfaces

- `get_project_settings`/`set_project_settings` keys (`editor/mcp/tools/ProjectTools.cpp:1006+`):
  `player.allow_insecure_http` (bool), `android.internet_access` (`"auto"|"require"`).
- `configure_game_view` (`editor/mcp/tools/ViewTools.cpp:1355`): new arg `network`
  (`"online"|"offline"`), returned in its read output. Works while playing.
- `get_play_state` (`editor/mcp/tools/PlayTools.cpp`): adds
  `"http": {"active": n, "queued": n, "finished": n, "failed": n, "offline": bool,
  "last_failures": [{"method","url","status","error_code","error"}]}` (max 5, only while playing).
- Agent guide (`editor/mcp/McpInstructions.cpp`): one line: "HTTP: `Http.get/post/download/getTexture`
  with a callback, results on the main thread; see jasper://docs/networking". Details in a new
  `docs/mcp/guide/networking.md`.
- `docs/mcp/TOOLS.md` regenerated (`tools/mcp/gen_tool_docs.py`); `tools/mcp/tool_budget.py` must pass.

### 6.7 Exported-build path

- **Generator** (`editor/Generator.cpp:2301` area, generated `main.cpp`): emit
  `Http::setInsecureHttpAllowed(true|false);` from `player.allow_insecure_http`. Editor Play:
  `Project` sets it in `finalizeStart` (`editor/Project.cpp:5636`) from the same setting.
- **Android** (`Exporter::writeAndroidProjectSettings`, `editor/Exporter.cpp:2940`):
  - INTERNET per decision 9 (written before the `android.permissions` loop; never twice).
  - Always write `res/xml/network_security_config.xml`:
    `<network-security-config><base-config cleartextTrafficPermitted="{setting}"/><domain-config cleartextTrafficPermitted="true"><domain includeSubdomains="false">localhost</domain><domain includeSubdomains="false">127.0.0.1</domain></domain-config></network-security-config>`
    and `android:networkSecurityConfig="@xml/network_security_config"` on `<application>`.
  - `JasperHttp.java` lives in `platform/android/java` (always compiled; tiny); add
    `-keep class com.jaspergameengine.jasperengine.JasperHttp { *; }` is already covered by the
    existing `proguard-rules.pro` wildcard (`com.jaspergameengine.jasperengine.*`).
- **iOS/macOS** (`Exporter::writeAppleProjectSettings`, `editor/Exporter.cpp:3155`): write
  `NSAppTransportSecurity` dict into both Info.plists: `NSAllowsLocalNetworking = true`,
  `NSAllowsArbitraryLoads = {setting}` (add a `replacePlistDictValue` helper next to
  `replacePlistBoolValue`, `Exporter.cpp:446`). Add `HttpBackendApple.mm` to the template
  `project.pbxproj` engine targets (`jasperengine-ios`, `jasperengine-macos`) and link
  `CFNetwork` is implicit with Foundation.
- **Windows**: `winhttp` added to `PLATFORM_LIBS` (`engine/CMakeLists.txt` Windows block) and to the
  Windows platform libs the Generator writes (`editor/Generator.cpp:582`).
- **Linux**: `${CMAKE_DL_LIBS}` (already linked if present; verify) in the Linux block and in the
  Generator's Linux config.
- **Web**: `_malloc`, `_free` exported (both `engine/CMakeLists.txt` Web link flags and the
  Generator's Web flags).

### 6.8 Docs to update

`docs/mcp/guide/networking.md` (new: API, options, error codes, CORS, ATS/cleartext, offline
simulation, examples for the six game use cases), `docs/mcp/guide/lua-api.md` (new section 2.13 Http,
summary table + link), `docs/mcp/guide/html-ui.md:1533` (row: "`fetch` -> call Lua `Http`"),
`docs/unity-glossary.md` (rows: `UnityWebRequest.Get/Post/Put/Delete/Head` -> `Http.get/...`;
`DownloadHandlerFile` -> `Http.download`; `DownloadHandlerTexture`/`UnityWebRequestTexture.GetTexture`
-> `Http.getTexture`; `UnityWebRequest.Abort` -> `request:cancel()`; `result`/`error` -> `ok`,
`errorCode`, `error`; *Internet Access* -> `android.internet_access`; *Allow downloads over HTTP*
-> `player.allow_insecure_http`), README feature list ("HTTP(S) client").

## 7. Behaviour rules and edge cases

1. **Threading.** No Lua is touched off the main thread. Backends only fill C++ structs; Lua
   objects (`HttpResponse`, tables, `TextureData` userdata) are created in `drain()`.
   `LuaBinding::isLuaThread()` asserted in the Lua callback trampoline (debug builds).
2. **Callback errors.** A Lua error in a callback is logged with traceback (like event handlers)
   and does not affect other callbacks.
3. **Frame order.** Requests finished during frame N are delivered in frame N+1 at the delivery
   point; within a frame: progress of all requests, then completions in completion order.
   A request started inside a callback starts at once (backend start is synchronous on the main
   thread) and delivers no earlier than the next frame.
4. **Pause.** App background (`systemPause`), editor Pause and MCP clock hold: nothing is
   delivered; transfers continue in the background where the OS allows (iOS may suspend sockets: the
   result is then a `network`/`timeout` error, delivered after resume). Resume delivers in the first
   update.
5. **Time scale 0** does not stop delivery (the update block still runs).
6. **Owner destroyed / scene unloaded** -> cancel, no callback, `.part` file removed.
7. **Play stop** (`Project::stop`, next to `AnimEngine::instance().reset()` at `editor/Project.cpp:9237`):
   `Http::shutdown()` (cancel all, drop callbacks, join nothing on the main thread: backends finish
   their cancellations on their own threads), then `MainThreadQueue::clear()`. The next Play starts
   with an empty queue. **Engine shutdown** (`Engine::systemShutdown`, `Engine.cpp:1574`): same,
   before `LuaBinding::cleanup()`.
8. **Lua refs.** Callback and owner refs are `luaL_ref` in the main state (`LUA_RIDX_MAINTHREAD`
   state, never a coroutine's), released after the callback runs or on cancel/owner loss; a pattern
   like `LuaRefHolder` (`HtmlUIAnimeLua.cpp:44-87`) that refuses to unref into a different/closed state.
9. **Redirects.** 301/302/303 change a POST to GET without body (browser behaviour); 307/308 keep
   method and body. `response.url` is the final URL, `redirected` true after a hop. Cross-scheme
   https->http hops are refused unless insecure HTTP is allowed (`insecure_http`).
10. **INTERNET scan** (`Exporter`, new helper `ExportFeatureScan` in `editor/util/`): case-sensitive
    regex `\bHttp\s*[.:\[]` over every exported `.lua` (and Lua inside `.html` `<script type="text/lua">`),
    `\bjasperengine::Http\b|\bHttp::|"net/Http.h"` over exported C++ scripts. The export log prints
    `Android: added INTERNET (Http used in lua/net/analytics.lua)`. The scan result is also returned
    by MCP `build_project` under `warnings` when `auto` added nothing but a native plugin exists
    (hint to set `require`).
11. **Headers.** `Host`, `Content-Length`, `Transfer-Encoding`, `Connection` set by the caller are
    ignored with a one-time warning (the stacks set them). `Accept-Encoding` is managed (gzip
    accepted and decoded transparently on every platform). Web: browser-forbidden headers
    (`User-Agent`, `Cookie`, `Referer`...) are dropped by the browser; documented.
12. **HEAD** responses have empty `body`; `bytesTotal` reflects `Content-Length` when sent.
13. **Large downloads** stream to disk (no memory growth) on every backend; Web buffers per chunk
    and appends with `FS.write`; `maxBodySize` does not apply to files.
14. **User data on Web**: a file download is persisted after completion with `syncFileSystem()`;
    `isUserDataLoaded()` false (IndexedDB restore pending) -> `download` waits for the restore before
    writing (queued, not failed).
15. **Concurrency limit** counts running requests; cancelled queued requests never start.
16. **IDs** are unique per process, increasing, starting at 1.
17. **URL validation**: must parse as `http[s]://host[:port]/...`; spaces are not auto-escaped
    (`invalid_url`), IDNs are passed to the platform as given.
18. **Android main-thread rule** satisfied (Java executor); game thread never blocks on I/O.

## 8. Performance and memory

- One worker thread on Linux (curl multi), N on Windows (pool, created lazily, reaped after 30 s
  idle), OS-managed on Apple/Android/Web. Zero threads when `Http` is never used.
- Main-thread cost per completed request: one Lua userdata + copies of headers; bodies are moved
  (never copied twice) into the Lua string at `response.body` first access (lazy: keep the
  `std::vector` until the property is read).
- Progress coalescing bounds callbacks to one per request per frame.
- Texture decode never on the main thread.

## 9. Tests

**Unit (gtest, `editor/tests/HttpTest.cpp`, add to `EditorTests.cmake`)**, against an in-process
test server (`editor/tests/util/TestHttpServer.{h,cpp}`: a small POSIX/Winsock HTTP/1.1 server on
127.0.0.1:0 with routes `/text`, `/json`, `/status/{code}`, `/redirect/{n}`, `/slow/{ms}`,
`/bytes/{n}` (chunked and with length), `/echo` (returns method, headers, body as JSON), `/gzip`,
`/png` (4x2 PNG)):
1. GET text: ok, status 200, body, lowercase headers, final URL, elapsed > 0.
2. POST table -> JSON body and `content-type: application/json` echoed; string body keeps bytes
   (including `\0`).
3. `query` encoding (`a b&c` -> `a%20b%26c`), sorted order.
4. 404 -> `ok=false, errorCode="http", status=404`, body present.
5. Timeout: `/slow/2000` with `timeout = 0.3` -> `timeout` in < 1 s.
6. Redirects: `/redirect/3` follows (redirected true); `redirects = 2` -> `too_many_redirects`;
   `redirects = false` -> status 302 ok=false errorCode `http`.
7. Cancel before start (queued), while running, after done (no-op); callback exactly once.
8. Concurrency: 20 requests with `maxConcurrentRequests = 4` -> never more than 4 running
   (server counts), all complete.
9. Download to `data://t/x.bin`: file content equals `/bytes/1048576`; `.part` absent; cancel ->
   no file; existing file kept on failure.
10. getTexture: `/png` -> 4x2 TextureData; `/text` -> `decode`.
11. Insecure policy: `http://example.invalid/` with allow=false -> `insecure_http` without network;
    `http://127.0.0.1:port/` works.
12. Offline simulation -> `offline` for remote, localhost unaffected.
13. Owner: request with owner entity, destroy entity before completion -> no callback, request
    cancelled; scene destroyed -> same.
14. Delivery point: completion runs inside `runFrame` before `onUpdate` handlers of the same frame
    (a Lua script records order); not while `Engine::setSimulationHeld(true)`; no Lua allocation off
    the Lua thread (`LuaBinding::getOffThreadUseCount()` stays 0).
15. Progress coalescing: `/bytes/10MB` with progress -> monotonic, <= frames count, last before complete.
16. gzip transparently decoded.
17. `Http.isAvailable()` true on the Linux host (libcurl.so.4 present); a test-only seam forcing the
    dlopen name list to a missing library -> `unsupported`.
18. Lua argument errors raise (`Http.get(42)`), unknown option key warns once.

**MCP e2e (`tools/mcp/tests/test_http.py`)**: start Python `http.server`-based fixture on
127.0.0.1 in the test; create a project with a Lua script that GETs `/json`, POSTs, downloads a
file, loads a texture into an Image; `play`, `wait until="_G.httpDone"`, assert values via
`eval_lua`, screenshot shows the downloaded image; `configure_game_view {network:"offline"}` -> the
script sees `offline`; `get_play_state` has the `http` block; stop while a `/slow` request runs ->
no error in console, next play clean. `set_project_settings` round-trips the two keys.

**Export checks (`tools/mcp/tests/test_build.py`, source export)**: project with `Http.get` in Lua
-> manifest has INTERNET once; without -> absent (auto) / present (require); manifest references
`network_security_config`; xml has the setting; iOS/macOS plist has `NSAppTransportSecurity`
(`tools/mcp/pbxproj.py` confirms `HttpBackendApple.mm` in both engine targets).

**Platform compile checks**: `cd engine/workspaces/androidstudio && ./gradlew assembleDebug`
(Java + JNI compile); `emcmake cmake -S engine -B build/web ... && cmake --build` (EM_JS +
exported functions); Windows/Apple: code review checklist in the PR (WinHTTP flags, ARC in
`.mm`, delegate retain cycles broken in `shutdown`).

## 10. Acceptance criteria

- [ ] `Http.get/head/delete/post/put/patch/request/download/getTexture/cancelAll/isAvailable`, the
      properties and `HttpRequest`/`HttpResponse` exactly as section 6.2, stubs generated with an empty
      `-- unresolved:` list.
- [ ] All 18 unit cases and the MCP e2e pass on Linux; Android `assembleDebug` and the Web build compile.
- [ ] Callbacks only at the delivery point; zero off-thread Lua uses in the tests.
- [ ] Owner/scene/Play-stop rules hold (tests 13, e2e stop case).
- [ ] Export: INTERNET auto/require, cleartext config, ATS dict, Apple file in the pbxproj.
- [ ] `player.allow_insecure_http` enforced identically in editor and builds.
- [ ] Game view Network menu + `configure_game_view {network}` + `get_play_state.http`.
- [ ] Docs: networking guide, lua-api, glossary, README, TOOLS.md regenerated, tool budget passes.

## 11. Integration points

| File | Change |
|---|---|
| `engine/core/Engine.cpp:1385-1428` | `MainThreadQueue::drain()` before `Engine::onUpdate.call()`; `Engine::systemShutdown` (`:1570`) calls `Http::shutdown()` + `MainThreadQueue::clear()` before `LuaBinding::cleanup()`. |
| `engine/core/thread/MainThreadQueue.{h,cpp}` | new. |
| `engine/core/script/AsyncOwner.{h,cpp}` | new (unless coroutines provides it). Hooks in `engine/core/Scene.cpp:60` and `engine/core/registry/EntityRegistry.cpp:261`. |
| `engine/core/net/*` | new (Http, HttpBackend, HttpBackendCurl, CurlApi.h). |
| `engine/core/script/binding/NetClassesLua.cpp` + `LuaBinding.h/.cpp:640-651` | new `registerNetClasses`. |
| `engine/CMakeLists.txt` | sources, `winhttp`, dl libs, Web exports. |
| `engine/platform/{win,apple,android,emscripten}/HttpBackend*` | new. |
| `engine/platform/android/NativeEngine.cpp:817` | register `JasperHttp` natives, cache class. |
| `engine/workspaces/xcode/JasperEngine.xcodeproj/project.pbxproj` | `HttpBackendApple.mm` in both engine targets. |
| `editor/Project.cpp:9237`, `:5636` | Play stop shutdown; Play start sets policy. |
| `editor/Project.h` / `editor/Stream.cpp:2617+,2963+` | `allowInsecureHttp` (player), `AndroidProjectSettings::internetAccess` enum serialization (`internetAccess: auto|require`, default omitted). |
| `editor/Exporter.cpp:2940-3150`, `:3155-3500` | permission, network config, ATS. |
| `editor/Generator.cpp:2301`, `:482-600` | generated setter; platform libs. |
| `editor/window/dialog/ProjectSettingsWindow.cpp` | two settings. |
| `editor/window/GameWindow.cpp:350-420` | Network toggle. |
| `editor/mcp/tools/ProjectTools.cpp`, `ViewTools.cpp`, `PlayTools.cpp` | keys, `network`, `http` block. |

Reuse: `FileData::getSystemPath` (`engine/core/io/FileData.cpp:125`) for `data://`,
`TextureData::loadTextureFromMemory`, `newJavaString`/`javaStringToUtf8` (`JasperEngineAndroid.cpp`),
`System::syncFileSystem` (Web), `LuaRefHolder` pattern.

## 12. Cross-feature dependencies

- **script-data** (required, merges first): `Json.encode/decode` in Lua and its C++ API
  `LuaJson::toJson(L, idx, out, opts, err)` / `LuaJson::push(L, json, opts)` (script-data spec 6.1,
  `engine/core/script/LuaJson.h`). The `json` option encodes with `LuaJson::toJson` (encode error ->
  Lua error naming the path, it is a programming error); `HttpResponse:json()` and
  `responseType = "json"` parse with nlohmann and push with `LuaJson::push`.
- **native-bridge**: reuses `MainThreadQueue` and `AsyncOwners`.
- **coroutines**: may make `HttpRequest` awaitable; must reuse/replace `AsyncOwners` (one owner
  mechanism in the engine) and keep the delivery point compatible (its resumptions run in the same
  update phase).
- **platform-services**: shares the export feature scanner (ACCESS_NETWORK_STATE for
  `System.getInternetReachability`) and `System.openURL`.
- **video-mp4**: plays files downloaded by `Http.download` (and may stream URLs itself).
- **persistence**: requests without an owner already survive scene loads; with a DontDestroyOnLoad
  owner they live as long as that object.

## Implementation notes

What was built differs from the text above in these points (decisions taken while
implementing; everything else is as specified).

### Backend interface: a transfer object instead of id callbacks

`HttpBackend::start` takes a `std::shared_ptr<HttpTransfer>` (engine/core/net/HttpBackend.h)
instead of `(id, desc)` plus `HttpBackendCallbacks`. A backend only moves bytes for one hop at a
time and reports to the transfer: `redirect(status, location)` for a 3xx with a Location,
`begin(status, headers, finalUrl)` for the final response, `write(bytes)` per chunk and
`finish(errorCode, detail)` exactly once. The transfer (shared C++, every platform) applies the
redirect rules (method changes, hop limit, the plain-HTTP policy of every hop, credentials
dropped on a cross-origin hop), body limits, files (`.part`, rename, resume, keepPartial),
texture and JSON decoding (on the backend's thread), the error texts and progress. So the
platforms behave the same by construction:

- **Redirects are followed by the engine** on Linux (curl with FOLLOWLOCATION off), Windows
  (WINHTTP_OPTION_REDIRECT_POLICY_NEVER) and Android (as specified), and through
  `willPerformHTTPRedirection` on Apple. Only the browser follows them itself: on the web a hop
  limit other than 0 is the browser's (20), and `redirected` is fetch's flag.
- **Android streams the body to C++** (`nativeData` per 64 KiB chunk) instead of writing files in
  Java; Java only runs the connection (redirect decisions come from `nativeRedirect`). The native
  methods are `nativeResponse(id, status, finalUrl, headers)`, `nativeData(id, bytes, length)`,
  `nativeComplete(id, errorCode, message)` and `nativeRedirect(id, status, location)`;
  `JasperHttp.start(id, method, url, headers, body, connectTimeoutMs, timeoutMs, verifyTls)`.
  The class is found through the activity's class loader in `NativeEngine::setupJNI`
  (`httpAndroidSetup`, engine/platform/android/HttpBackendAndroid.h).
- **Web** reads `response.body` as a stream and hands each chunk to `jasper_http_on_data`
  (instead of one `jasper_http_on_complete` with the whole body), so downloads and progress need
  no buffering in JavaScript; the C side writes files through the same transfer, and
  `System::syncFileSystem()` runs at delivery. Headers go both ways as JSON text. The
  User-Agent header is never set on the web (it would make every cross-origin request need a
  CORS preflight).

### Cancel semantics

`cancel()` on a queued or running request turns the handle `Cancelled` at once (with a
cancelled `response`), and the callback runs with `"cancelled"` once the backend has stopped the
transfer (normally the next frame). A request whose response has already arrived (the backend
finished it, the callback not run yet) is not changed by `cancel()`: its file is already renamed,
and reporting it as cancelled would lie. A cancel and the response's arrival are serialized by
the transfer (`requestCancel` vs. the commit in `finish`), so a cancelled download never leaves
its file.

### Additions

- `HttpRequestDesc::resume` and `keepPartial` (requested by video-mp4); also Lua options
  `resume` and `keepPartial`. With `resume` the byte counts include the part already on disk; a
  416 removes the `.part` (the next try starts over).
- `HttpResponse::method`, `json` (the parsed body of `HttpResponseType::Json`, an opaque
  `HttpJson` in net/HttpJson.h, parsed on the worker with `LuaJson::parse`), `header(name)` and
  `takeTexture()`; `Http::unavailableReason()`, `getLogFailures/setLogFailures`, and
  `Stats::cancelled` (cancelled requests are counted apart, not as failures; MCP
  `get_play_state.http.cancelled`).
- `MainThreadQueue::epoch()` (a poster knows its queued task was dropped by `clear()`),
  `isMainThread()` / `setMainThread()`. `AsyncOwners::fromLua` returns `bool` with an error text
  (Lua is built as C: a raising C++ function would skip destructors), plus `fromScene`,
  `fromScript(const ScriptBase*)`, `describe` and `count`. Owners are keyed by registry
  (`EntityRegistry*`); a script instance owner is also ended with its GameObject and scene.
- `ExportFeatureScan` (editor/util) is generic (`scan(root, features)`), with `http()` as the
  first feature; platform-services adds its own.
- An HTTP test seam `HttpTesting` (curl library names, running transfers, deliver).

### Decisions

- **App Transport Security**: Apple ignores `NSAllowsArbitraryLoads` when
  `NSAllowsLocalNetworking` is present too, so the export writes only one of them:
  `NSAllowsArbitraryLoads = true` when insecure HTTP is allowed (localhost included), else
  `NSAllowsLocalNetworking = true`. The template Info.plists carry the default.
- **Error bodies of downloads and textures** stay in memory (at most 1 MiB) so `response.body`
  shows what the server said; `maxBodySize = 0` means no limit.
- `responseType = "json"` with an empty 2xx body (not 204, not HEAD) is `decode`; a non-2xx JSON
  body is still parsed into `data` when it is valid (APIs send JSON errors).
- Unknown option keys log a warning once per key name; wrong option *types* raise (they are
  programming errors), as do `body`/`json`/`form` on GET/HEAD/getTexture/download and a body
  given twice.
- `Accept-Encoding` set by the caller is ignored with a warning like the other managed headers
  (otherwise Android and curl would not decode gzip).
- The offline simulation also fails running non-localhost requests (`"offline"`), and survives
  Play stop (it is the editor's toggle; the editor restart resets it).
- The default User-Agent version comes from `JASPERENGINE_EDITOR_VERSION`, set on Http.cpp only
  (so a version change recompiles one file); a build without it says `JasperEngine (<platform>)`.
- `TextureData` follows the engine's ownership rule: the `textureData` pushed to Lua does not own
  its pixels (the texture pool that receives it does); a response whose texture was never read
  frees them.
- Apple: NSURLSession's per-request timeout is an idle timeout, so it is set to the time left of
  the whole request (a timer enforces the total); `connectTimeout` has no separate effect there.
  Repeated `Set-Cookie` headers arrive joined with ", " (allHeaderFields).
- C++ scripts include `"net/Http.h"` (also in JasperEngine.h); `core/net` is not a flat include
  directory.
- `build_project` reports the "native plugins but no INTERNET" hint as a warning on
  `project.yaml:1` when `<project>/plugins/native` exists and nothing uses Http.

### Review fixes (second pass)

- **Windows** runs WinHTTP in asynchronous mode (`WINHTTP_FLAG_ASYNC`, one status callback, a
  heap "hop" per request handle as its context, freed at `WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING`):
  closing a request handle is then the documented cancel, also for the whole-request timeout
  (a watchdog thread that ends after 30 s without timed requests; deadlines under one mutex).
  TLS 1.2 and 1.3 are enabled (`WINHTTP_OPTION_SECURE_PROTOCOLS`, TLS 1.2 alone where 1.3 is
  unknown); a header WinHTTP refuses fails the request (never sent without it). `iphlpapi`
  is linked for the offline check.
- **verifyTls = false** runs on a session of its own (Apple: an ephemeral NSURLSession;
  Windows: a second WinHttpOpen session), so a connection that skipped the certificate check is
  never reused by a verified request. Curl checks per transfer (no shared unverified pool:
  curl does not reuse a connection across different SSL_VERIFY settings).
- **One default Content-Type**: a non-empty body without a Content-Type header gets
  `application/octet-stream`, set in the core for every backend (tables stay
  `application/json`, `form` `application/x-www-form-urlencoded`). The stacks' own defaults
  (a form on Android and Apple, curl's form for POST) never apply.
- **Resume** sends `If-Range` with the validator the part was written with (a strong ETag, else
  Last-Modified, stored in `<path>.part.meta`) and `Accept-Encoding: identity`; a part without a
  validator is not continued (a 200 starts over). A 200 to a resume rewrites the part from the
  start. The meta file goes with the part.
- **One download per file**: a download to a path another live request is writing fails at
  once with `errorCode = "file"` ("data://x is being downloaded already (request N)"), rather
  than joining it (two callbacks, two owners, two option sets would have to agree).
- **Private Network Access** (lead decision): a redirect from a host that is not loopback /
  private / link-local to one that is (literal addresses and `localhost` names; DNS names are
  not resolved) fails with `redirect_blocked`. The same check, and the plain-HTTP policy, run on
  the final URL a stack reached by itself (the browser).
- **Editor Play** sets the plain-HTTP policy before any scene's scripts initialise
  (`runPlayStartup`), as the exported main does. The macOS editor's Info.plist allows arbitrary
  loads (the engine enforces the project policy itself); a macOS export built without Xcode
  embeds an `__info_plist` section with the project's ATS dictionary
  (`JASPERENGINE_ALLOW_INSECURE_HTTP`, written by the export). Builds of the project outside the
  editor that link the editor's engine library (Generator's non-plugin path) get no ATS section.
- **Android**: custom methods over HTTPS fail with `unsupported` (HttpsURLConnection would send
  a GET); a refused redirect disconnects; response headers keep their wire order without
  Android's `X-Android-*`; `offline` when ConnectivityManager says there is no network (the
  export adds `ACCESS_NETWORK_STATE`, a normal permission, with INTERNET). The cleartext config
  also lists `::1`. Curl (`getifaddrs`) and Windows (`GetAdaptersAddresses`) report `offline` for
  a DNS/connect failure when no interface but loopback is up.
- **Apple**: no total timeout means `connectTimeout` (else 60 s) as the idle timeout; a redirect
  target NSURL cannot parse fails with `invalid_url`.
- **Settings**: `Http::shutdown` (Play stop) also resets `maxConcurrentRequests`, `userAgent`,
  `timeout` and `logFailures`; engine shutdown calls the new `Http::terminate`, which also
  joins the platform stack's threads (the next request starts them again).
- **Lua**: `options.file` with `responseType` raises.
- **Export scan**: any use of the name `Http` in shipped Lua (aliases such as `local H = Http`),
  HTML UI documents' Lua, and C++ `Http::`, `jasperengine::Http`, `HttpRequestDesc`,
  `"net/Http.h"`; it reads the export's `lua/`, `assets/` and `scripts/` as they are on disk
  (what the build packs and compiles), never the generated sources.
- **HTML UI documents** have no `self`: their scripts pass `owner = document` (the document's
  GameObject).

### Verification

- Unit tests (editor/tests/HttpTest.cpp, 42 cases: the 18 of section 9 plus TLS, redirect method
  rules and credentials, resume / keepPartial / 416, body limits, owners by scene and script,
  shutdown, stats and logFailures, URL helpers, the Lua API, coroutine callers, the export scan)
  against editor/tests/util/TestHttpServer and a Python HTTPS server with an openssl self-signed
  certificate: all pass; the whole suite 1014 tests, 1006 passed, 8 skipped (the known ones).
- MCP: tools/mcp/tests/test_http.py (2), test_build.py `test_http_export` and the rest of
  test_build.py, test_stubs.py with lua-language-server (the Http stubs check clean and a wrong
  `Http.maxConcurrentRequests` assignment is reported), test_docs.py, test_protocol.py (the
  agent guide stays under 16 KB) pass.
- Android: `assembleDebug` of engine/workspaces/androidstudio for arm64-v8a (JasperHttp.java,
  HttpBackendAndroid.cpp, NativeEngine.cpp) builds; the APK has the class and the natives.
- Web: the full emsdk build of the engine and its sample project links (EM_JS, exports); the web
  backend with the client core ran in Node 24 against a local server (GET, POST with a binary
  body, 404, timeout, redirect followed / not followed, cancel, a file download).
- Windows (WinHTTP) and Apple (NSURLSession) code could not be compiled on this host; they were
  written against the current SDK APIs and re-read.
