// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The HTTP backend of the web build: the browser's fetch() with an AbortController (cancel and
// the whole-request timeout), the body read as a stream so downloads and progress need no
// buffering in JavaScript. Everything runs on the browser's main thread: the chunks reach the
// HttpTransfer through the EMSCRIPTEN_KEEPALIVE functions below, which keep the body, file and
// error rules of every platform. The browser follows redirects itself (up to 20; the transfer
// learns only that one happened), applies CORS and sets the forbidden headers (User-Agent,
// Cookie, Referer...) itself.

#if defined(__EMSCRIPTEN__)

#include "net/HttpBackend.h"
#include "json.hpp"

#include <emscripten.h>

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>

using namespace jasperengine;

EM_JS_DEPS(jasper_http, "$UTF8ToString,$stringToNewUTF8");

// redirectMode: 1 follow, 0 manual (a redirect then shows as an opaque response)
EM_JS(void, jasper_http_start, (double id, const char* method, const char* url, const char* headersJson,
                                const uint8_t* body, int bodyLength, int timeoutMs, int redirectMode), {
    if (!Module.jasperHttp) Module.jasperHttp = {};
    var state = { controller: new AbortController(), cancelled: false, timedOut: false, done: false, timer: 0 };
    Module.jasperHttp[id] = state;
    var finish = function(code, message) {
        if (state.done) return;
        state.done = true;
        if (state.timer) clearTimeout(state.timer);
        delete Module.jasperHttp[id];
        var c = stringToNewUTF8(code);
        var m = stringToNewUTF8(message);
        _jasper_http_on_complete(id, c, m);
        _free(c);
        _free(m);
    };
    var init = {
        method: UTF8ToString(method),
        headers: new Headers(),
        redirect: redirectMode ? "follow" : "manual",
        signal: state.controller.signal,
        cache: "no-store"
    };
    var headers = JSON.parse(UTF8ToString(headersJson));
    for (var i = 0; i < headers.length; i++) {
        // a header the browser forbids is dropped (it would throw in some browsers)
        try { init.headers.append(headers[i][0], headers[i][1]); } catch (e) {}
    }
    if (bodyLength > 0) init.body = HEAPU8.slice(body, body + bodyLength);
    if (timeoutMs > 0) {
        state.timer = setTimeout(function() { state.timedOut = true; state.controller.abort(); }, timeoutMs);
    }
    var failure = function(error) {
        if (state.cancelled) finish("cancelled", "");
        else if (state.timedOut) finish("timeout", "");
        else if (error && error.name === "AbortError") finish("cancelled", "");
        else finish("network", "network error or CORS refused (the server must send Access-Control-Allow-Origin)");
    };
    var urlText = UTF8ToString(url);
    var request;
    try {
        request = fetch(urlText, init);
    } catch (e) {
        failure(e);
        return;
    }
    request.then(function(response) {
        if (state.cancelled) { finish("cancelled", ""); return; }
        if (response.type === "opaqueredirect") {
            finish("redirect_blocked", "the server redirected and redirects are off (a browser does not show the redirect)");
            return;
        }
        var list = [];
        response.headers.forEach(function(value, name) { list.push([name, value]); });
        var headerText = stringToNewUTF8(JSON.stringify(list));
        var finalUrl = stringToNewUTF8(response.url || urlText);
        var keep = _jasper_http_on_response(id, response.status, finalUrl, headerText, response.redirected ? 1 : 0);
        _free(headerText);
        _free(finalUrl);
        if (!keep) { state.controller.abort(); finish("", ""); return; }
        if (!response.body) { finish("", ""); return; }
        var reader = response.body.getReader();
        var pump = function() {
            return reader.read().then(function(chunk) {
                if (state.cancelled) { reader.cancel(); finish("cancelled", ""); return; }
                if (chunk.done) { finish("", ""); return; }
                var bytes = chunk.value;
                var pointer = _malloc(bytes.length);
                HEAPU8.set(bytes, pointer);
                var accepted = _jasper_http_on_data(id, pointer, bytes.length);
                _free(pointer);
                if (!accepted) { reader.cancel(); finish("", ""); return; }
                return pump();
            });
        };
        return pump();
    }).catch(failure);
});

EM_JS(void, jasper_http_cancel, (double id), {
    var state = Module.jasperHttp && Module.jasperHttp[id];
    if (!state) return;
    state.cancelled = true;
    state.controller.abort();
});

namespace {

    std::unordered_map<uint64_t, std::shared_ptr<HttpTransfer>>& transfers(){
        static auto* map = new std::unordered_map<uint64_t, std::shared_ptr<HttpTransfer>>();
        return *map;
    }

    std::shared_ptr<HttpTransfer> findTransfer(double id){
        auto it = transfers().find((uint64_t)id);
        return it == transfers().end() ? nullptr : it->second;
    }

    class WebBackend : public HttpBackend {
    public:
        bool available(std::string&) override{
            return true;
        }

        void start(const std::shared_ptr<HttpTransfer>& transfer) override{
            transfers()[transfer->id] = transfer;
            nlohmann::json headers = nlohmann::json::array();
            for (const auto& [name, value] : transfer->headers){
                // the browser's own (a User-Agent would make every request need a CORS preflight)
                if (HttpUtil::lower(name) == "user-agent") continue;
                headers.push_back({name, value});
            }
            const std::string headersText = headers.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            const double total = transfer->totalTimeout() > 0.0 ? transfer->remainingTimeout() : 0.0;
            const int timeoutMs = total > 0.0 ? (int)std::min(2147483647.0, std::max(1.0, total * 1000.0)) : 0;
            jasper_http_start((double)transfer->id, transfer->method.c_str(), transfer->url.c_str(), headersText.c_str(),
                              transfer->body.empty() ? nullptr : transfer->body.data(), (int)transfer->body.size(), timeoutMs,
                              transfer->maxRedirects > 0 ? 1 : 0);
        }

        void cancel(uint64_t id) override{
            jasper_http_cancel((double)id);
        }

        void shutdown() override{
        }
    };

}

extern "C" {

EMSCRIPTEN_KEEPALIVE int jasper_http_on_response(double id, int status, const char* finalUrl, const char* headersJson, int redirected){
    std::shared_ptr<HttpTransfer> transfer = findTransfer(id);
    if (!transfer) return 0;
    HttpHeaders headers;
    nlohmann::json list = nlohmann::json::parse(headersJson ? headersJson : "[]", nullptr, false);
    if (list.is_array()){
        for (const auto& pair : list){
            if (pair.is_array() && pair.size() == 2 && pair[0].is_string() && pair[1].is_string()){
                headers.emplace_back(pair[0].get<std::string>(), pair[1].get<std::string>());
            }
        }
    }
    if (redirected) transfer->stackRedirected();
    return transfer->begin(status, std::move(headers), finalUrl ? finalUrl : "") ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int jasper_http_on_data(double id, const uint8_t* data, int length){
    std::shared_ptr<HttpTransfer> transfer = findTransfer(id);
    if (!transfer || length < 0) return 0;
    return transfer->write(data, (size_t)length) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void jasper_http_on_complete(double id, const char* errorCode, const char* message){
    auto it = transfers().find((uint64_t)id);
    if (it == transfers().end()) return;
    std::shared_ptr<HttpTransfer> transfer = std::move(it->second);
    transfers().erase(it);
    transfer->finish(errorCode ? errorCode : "", message ? message : "");
}

}

HttpBackend* jasperengine::createPlatformHttpBackend(){
    return new WebBackend();
}

#endif
