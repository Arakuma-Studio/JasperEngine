// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The HTTP backend of Linux (exports and the Linux editor) and FreeBSD: the system libcurl,
// loaded with dlopen (no build dependency, the distribution's TLS stack and certificates),
// driven by one curl_multi worker thread made at the first request. Redirects are followed
// here one hop at a time (HttpTransfer::redirect decides), so the method and policy rules are
// the client's on every platform.

#if (defined(__linux__) || defined(__FreeBSD__)) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)

#include "net/HttpBackend.h"
#include "net/CurlApi.h"

#include <dlfcn.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::curlapi;

void jasperengineHttpResetBackend();

namespace {

    std::mutex& namesMutex(){
        static std::mutex* m = new std::mutex();
        return *m;
    }

    std::vector<std::string>& libraryNamesOverride(){
        static std::vector<std::string>* names = new std::vector<std::string>();
        return *names;
    }

    std::vector<std::string> defaultLibraryNames(){
        return {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"};
    }

    struct Job {
        std::shared_ptr<HttpTransfer> transfer;
        CURL* easy = nullptr;
        curl_slist* headers = nullptr;
        char errorBuffer[CURL_ERROR_SIZE] = {0};
        std::string rawHeaders;      // of the current hop
        bool headDone = false;       // begin() or redirect() decided for this hop
        bool following = false;      // the current hop is a redirect we follow
        bool failed = false;         // begin/write/redirect refused: finish() with the transfer's reason
        bool cancelled = false;
    };

    class CurlBackend : public HttpBackend {
    public:
        CurlBackend(){
            std::vector<std::string> names;
            {
                std::lock_guard<std::mutex> lock(namesMutex());
                names = libraryNamesOverride().empty() ? defaultLibraryNames() : libraryNamesOverride();
            }
            for (const std::string& name : names){
                library = dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
                if (library){
                    libraryName = name;
                    break;
                }
            }
            if (!library){
                whyNot = "libcurl not found: install libcurl4 (e.g. apt install libcurl4)";
                return;
            }
            bool ok = true;
            auto load = [&](auto& fn, const char* symbol, bool required){
                fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(library, symbol));
                if (!fn && required) ok = false;
            };
            load(api.global_init, "curl_global_init", true);
            load(api.version, "curl_version", false);
            load(api.easy_init, "curl_easy_init", true);
            load(api.easy_cleanup, "curl_easy_cleanup", true);
            load(api.easy_setopt, "curl_easy_setopt", true);
            load(api.easy_getinfo, "curl_easy_getinfo", true);
            load(api.easy_strerror, "curl_easy_strerror", true);
            load(api.multi_init, "curl_multi_init", true);
            load(api.multi_cleanup, "curl_multi_cleanup", true);
            load(api.multi_add_handle, "curl_multi_add_handle", true);
            load(api.multi_remove_handle, "curl_multi_remove_handle", true);
            load(api.multi_perform, "curl_multi_perform", true);
            load(api.multi_info_read, "curl_multi_info_read", true);
            load(api.multi_wait, "curl_multi_wait", true);
            load(api.multi_poll, "curl_multi_poll", false);
            load(api.multi_wakeup, "curl_multi_wakeup", false);
            load(api.slist_append, "curl_slist_append", true);
            load(api.slist_free_all, "curl_slist_free_all", true);
            if (!ok){
                whyNot = libraryName + " is too old (libcurl 7.28 or newer is needed): install libcurl4";
                dlclose(library);
                library = nullptr;
                return;
            }
            if (!api.multi_poll || !api.multi_wakeup){
                api.multi_poll = nullptr;
                api.multi_wakeup = nullptr;
            }
            // not thread-safe in old versions: here, on the main thread, before any other call
            if (api.global_init(CURL_GLOBAL_ALL) != CURLE_OK){
                whyNot = "curl_global_init failed (" + libraryName + ")";
                return;
            }
            usable = true;
        }

        ~CurlBackend() override{
            shutdown();
            // the library stays loaded: its TLS stack keeps process-wide state
        }

        bool available(std::string& reason) override{
            if (!usable) reason = whyNot;
            return usable;
        }

        void start(const std::shared_ptr<HttpTransfer>& transfer) override{
            if (!usable){
                transfer->finish("unsupported", whyNot);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                incoming.push_back(transfer);
                if (!worker.joinable()){
                    stopping = false;
                    worker = std::thread([this]() { run(); });
                }
            }
            wake();
        }

        void cancel(uint64_t id) override{
            {
                std::lock_guard<std::mutex> lock(mutex);
                cancels.push_back(id);
            }
            wake();
        }

        void shutdown() override{
            {
                std::lock_guard<std::mutex> lock(mutex);
                stopping = true;
            }
            wake();
            if (worker.joinable()) worker.join();
        }

    private:
        void* library = nullptr;
        std::string libraryName;
        std::string whyNot;
        bool usable = false;
        Api api;

        std::mutex mutex;
        std::condition_variable condition;
        std::deque<std::shared_ptr<HttpTransfer>> incoming;
        std::vector<uint64_t> cancels;
        bool stopping = false;
        std::thread worker;
        CURLM* multi = nullptr;   // the worker's

        void wake(){
            std::lock_guard<std::mutex> lock(mutex);
            condition.notify_all();
            if (multi && api.multi_wakeup) api.multi_wakeup(multi);
        }

        // ---- curl callbacks (the worker thread)

        static size_t onHeader(char* data, size_t size, size_t count, void* user){
            Job* job = static_cast<Job*>(user);
            const size_t n = size * count;
            const std::string line(data, n);
            if (line.compare(0, 5, "HTTP/") == 0){
                // a new response (an interim 100 Continue, then the real one)
                job->rawHeaders.clear();
            }
            job->rawHeaders += line;
            return n;
        }

        static size_t onWrite(char* data, size_t size, size_t count, void* user){
            Job* job = static_cast<Job*>(user);
            CurlBackend* self = job->transfer ? currentBackend : nullptr;
            const size_t n = size * count;
            if (!self) return CURL_WRITEFUNC_ERROR;
            if (!job->headDone && !self->decideHead(*job)) return CURL_WRITEFUNC_ERROR;
            if (job->following) return n;   // a redirect's body: dropped
            if (!job->transfer->write(data, n)){
                job->failed = !job->transfer->isCancelled();
                return CURL_WRITEFUNC_ERROR;
            }
            return n;
        }

        // the worker's backend, for the static callbacks (one worker per backend)
        static thread_local CurlBackend* currentBackend;

        // The current hop's head arrived (at its first body byte or at its end): a redirect to
        // follow, or the response
        bool decideHead(Job& job){
            job.headDone = true;
            long status = 0;
            api.easy_getinfo(job.easy, CURLINFO_RESPONSE_CODE, &status);
            int parsedStatus = 0;
            HttpHeaders headers = HttpUtil::parseHeaderBlock(job.rawHeaders, &parsedStatus);
            if (status == 0) status = parsedStatus;
            if (HttpTransfer::isRedirectStatus((int)status)){
                std::string location;
                for (const auto& [name, value] : headers){
                    if (name == "location") location = value;
                }
                if (!location.empty()){
                    char* redirectUrl = nullptr;
                    if (api.easy_getinfo(job.easy, CURLINFO_REDIRECT_URL, &redirectUrl) == CURLE_OK && redirectUrl && *redirectUrl){
                        location = redirectUrl;
                    }
                    switch (job.transfer->redirect((int)status, location)){
                        case HttpRedirect::Action::Follow:
                            job.following = true;
                            return true;
                        case HttpRedirect::Action::Fail:
                            job.failed = true;
                            return false;
                        case HttpRedirect::Action::Stop:
                            break;
                    }
                }
            }
            char* effective = nullptr;
            api.easy_getinfo(job.easy, CURLINFO_EFFECTIVE_URL, &effective);
            if (!job.transfer->begin((int)status, std::move(headers), effective ? std::string(effective) : std::string())){
                job.failed = !job.transfer->isCancelled();
                return false;
            }
            return true;
        }

        void freeEasy(Job& job){
            if (job.easy){
                api.multi_remove_handle(multi, job.easy);
                api.easy_cleanup(job.easy);
                job.easy = nullptr;
            }
            if (job.headers){
                api.slist_free_all(job.headers);
                job.headers = nullptr;
            }
        }

        // a fresh easy handle for the transfer's current hop; false: finish() was called
        bool setupHop(Job& job){
            HttpTransfer& t = *job.transfer;
            freeEasy(job);
            job.rawHeaders.clear();
            job.headDone = false;
            job.following = false;
            job.errorBuffer[0] = 0;
            const double remaining = t.remainingTimeout();
            if (remaining <= 0.0){
                t.finish("timeout");
                return false;
            }
            job.easy = api.easy_init();
            if (!job.easy){
                t.finish("network", "curl_easy_init failed");
                return false;
            }
            CURL* e = job.easy;
            api.easy_setopt(e, CURLOPT_URL, t.url.c_str());
            api.easy_setopt(e, CURLOPT_PRIVATE, &job);
            api.easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
            api.easy_setopt(e, CURLOPT_ERRORBUFFER, job.errorBuffer);
            api.easy_setopt(e, CURLOPT_FOLLOWLOCATION, 0L);
            api.easy_setopt(e, CURLOPT_ACCEPT_ENCODING, "");
            api.easy_setopt(e, CURLOPT_NOPROGRESS, 1L);
            api.easy_setopt(e, CURLOPT_HEADERFUNCTION, &CurlBackend::onHeader);
            api.easy_setopt(e, CURLOPT_HEADERDATA, &job);
            api.easy_setopt(e, CURLOPT_WRITEFUNCTION, &CurlBackend::onWrite);
            api.easy_setopt(e, CURLOPT_WRITEDATA, &job);
            if (t.totalTimeout() > 0.0){
                api.easy_setopt(e, CURLOPT_TIMEOUT_MS, (long)std::max(1.0, remaining * 1000.0));
            }
            if (t.connectTimeout > 0.0){
                api.easy_setopt(e, CURLOPT_CONNECTTIMEOUT_MS, (long)std::max(1.0, t.connectTimeout * 1000.0));
            }
            if (!t.verifyTls){
                api.easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 0L);
                api.easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 0L);
            }

            bool hasContentType = false;
            for (const auto& [name, value] : t.headers){
                const std::string lower = HttpUtil::lower(name);
                if (lower == "content-type") hasContentType = true;
                // "Name;" sends an empty header, "Name:" would remove it
                const std::string line = value.empty() ? name + ";" : name + ": " + value;
                job.headers = api.slist_append(job.headers, line.c_str());
            }
            const std::string& m = t.method;
            const bool sendsBody = !t.body.empty() || m == "POST" || m == "PUT" || m == "PATCH";
            if (sendsBody){
                // curl's own defaults for a body: no form Content-Type, no 100-continue wait
                if (!hasContentType) job.headers = api.slist_append(job.headers, "Content-Type:");
                job.headers = api.slist_append(job.headers, "Expect:");
            }
            if (job.headers) api.easy_setopt(e, CURLOPT_HTTPHEADER, job.headers);

            if (m == "GET"){
                api.easy_setopt(e, CURLOPT_HTTPGET, 1L);
            } else if (m == "HEAD"){
                api.easy_setopt(e, CURLOPT_NOBODY, 1L);
            } else if (m == "POST"){
                api.easy_setopt(e, CURLOPT_POST, 1L);
            } else {
                api.easy_setopt(e, CURLOPT_CUSTOMREQUEST, m.c_str());
            }
            if (sendsBody && m != "GET" && m != "HEAD"){
                api.easy_setopt(e, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)t.body.size());
                api.easy_setopt(e, CURLOPT_POSTFIELDS, t.body.empty() ? "" : reinterpret_cast<const char*>(t.body.data()));
            }
            if (api.multi_add_handle(multi, e) != CURLM_OK){
                freeEasy(job);
                t.finish("network", "curl_multi_add_handle failed");
                return false;
            }
            return true;
        }

        static std::string mapError(CURLcode code){
            switch (code){
                case CURLE_OPERATION_TIMEDOUT: return "timeout";
                case CURLE_COULDNT_RESOLVE_HOST:
                case CURLE_COULDNT_RESOLVE_PROXY: return "dns";
                case CURLE_COULDNT_CONNECT: return "connect";
                case CURLE_PEER_FAILED_VERIFICATION:
                case CURLE_SSL_CONNECT_ERROR:
                case CURLE_SSL_ENGINE_NOTFOUND:
                case CURLE_SSL_ENGINE_SETFAILED:
                case CURLE_SSL_CERTPROBLEM:
                case CURLE_SSL_CIPHER:
                case CURLE_SSL_ENGINE_INITFAILED:
                case CURLE_SSL_CACERT_BADFILE:
                case CURLE_SSL_SHUTDOWN_FAILED:
                case CURLE_SSL_CRL_BADFILE:
                case CURLE_SSL_ISSUER_ERROR:
                case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
                case CURLE_SSL_INVALIDCERTSTATUS:
                case CURLE_SSL_CLIENTCERT: return "tls";
                case CURLE_TOO_MANY_REDIRECTS: return "too_many_redirects";
                case CURLE_URL_MALFORMAT:
                case CURLE_UNSUPPORTED_PROTOCOL: return "invalid_url";
                default: return "network";
            }
        }

        void complete(Job& job, CURLcode result){
            HttpTransfer& t = *job.transfer;
            if (job.cancelled || t.isCancelled()){
                t.finish("cancelled");
                return;
            }
            if (job.failed){
                t.finish("network");
                return;
            }
            if (result != CURLE_OK){
                std::string detail = job.errorBuffer[0] ? job.errorBuffer : api.easy_strerror(result);
                t.finish(mapError(result), detail);
                return;
            }
            if (!job.headDone && !decideHead(job)){
                t.finish("network");
                return;
            }
            if (job.following){
                // the next hop on the same job
                setupHop(job);
                return;
            }
            t.finish("");
        }

        void run(){
            currentBackend = this;
            CURLM* m = api.multi_init();
            {
                std::lock_guard<std::mutex> lock(mutex);
                multi = m;
            }
            std::unordered_map<uint64_t, std::unique_ptr<Job>> jobs;
            while (true){
                std::deque<std::shared_ptr<HttpTransfer>> starting;
                std::vector<uint64_t> cancelling;
                bool stop = false;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    if (jobs.empty() && incoming.empty() && cancels.empty() && !stopping){
                        // idle: no CPU until a request comes
                        condition.wait(lock, [&]{ return !incoming.empty() || !cancels.empty() || stopping; });
                    }
                    starting.swap(incoming);
                    cancelling.swap(cancels);
                    stop = stopping;
                }
                if (stop){
                    for (auto& [id, job] : jobs){
                        freeEasy(*job);
                        job->transfer->finish("cancelled");
                    }
                    jobs.clear();
                    for (auto& transfer : starting) transfer->finish("cancelled");
                    break;
                }
                for (auto& transfer : starting){
                    auto job = std::make_unique<Job>();
                    job->transfer = transfer;
                    if (transfer->isCancelled()){
                        transfer->finish("cancelled");
                        continue;
                    }
                    if (setupHop(*job)) jobs.emplace(transfer->id, std::move(job));
                }
                for (uint64_t id : cancelling){
                    auto it = jobs.find(id);
                    if (it == jobs.end()) continue;
                    it->second->cancelled = true;
                    freeEasy(*it->second);
                    it->second->transfer->finish("cancelled");
                    jobs.erase(it);
                }
                if (jobs.empty()) continue;

                int stillRunning = 0;
                api.multi_perform(m, &stillRunning);
                int queued = 0;
                while (CURLMsg* msg = api.multi_info_read(m, &queued)){
                    if (msg->msg != CURLMSG_DONE) continue;
                    Job* job = nullptr;
                    api.easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &job);
                    if (!job) continue;
                    const CURLcode result = msg->data.result;
                    const uint64_t id = job->transfer->id;
                    complete(*job, result);
                    // finished, or the next hop is on its way
                    if (job->transfer->isFinished()){
                        freeEasy(*job);
                        jobs.erase(id);
                    }
                }
                // a hop that ran out of time before it started is finished too
                for (auto it = jobs.begin(); it != jobs.end();){
                    if (it->second->transfer->isFinished()){
                        freeEasy(*it->second);
                        it = jobs.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (jobs.empty()) continue;
                int fds = 0;
                if (api.multi_poll){
                    api.multi_poll(m, nullptr, 0, 1000, &fds);
                } else {
                    // without curl_multi_wakeup a new request waits at most this long
                    api.multi_wait(m, nullptr, 0, 50, &fds);
                }
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                multi = nullptr;
            }
            api.multi_cleanup(m);
            currentBackend = nullptr;
        }
    };

    thread_local CurlBackend* CurlBackend::currentBackend = nullptr;

}

HttpBackend* jasperengine::createPlatformHttpBackend(){
    return new CurlBackend();
}

void HttpTesting::setCurlLibraryNames(const std::vector<std::string>& names){
    {
        std::lock_guard<std::mutex> lock(namesMutex());
        libraryNamesOverride() = names;
    }
    jasperengineHttpResetBackend();
}

std::vector<std::string> HttpTesting::curlLibraryNames(){
    std::lock_guard<std::mutex> lock(namesMutex());
    return libraryNamesOverride().empty() ? defaultLibraryNames() : libraryNamesOverride();
}

#endif
