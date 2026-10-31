// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The HTTP backend of macOS and iOS (exports, the Xcode workspace's engine targets and the macOS
// editor): one NSURLSession whose delegate runs on a private serial NSOperationQueue, so App
// Transport Security, the system's certificates and proxies apply. Each redirect asks
// HttpTransfer::redirect (willPerformHTTPRedirection); the whole-request timeout is a
// dispatch_after that cancels the task. No cookie storage and no URL cache: the same behaviour
// as the other platforms.

#if defined(__APPLE__)

#import <Foundation/Foundation.h>

#include "net/HttpBackend.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#if !__has_feature(objc_arc)
#error "HttpBackendApple.mm is compiled with ARC (-fobjc-arc)"
#endif

using namespace jasperengine;

namespace {

    struct AppleJob {
        std::shared_ptr<HttpTransfer> transfer;
        NSURLSessionDataTask* task = nil;
        bool timedOut = false;
    };

    std::string utf8(NSString* text){
        return text ? std::string(text.UTF8String ? text.UTF8String : "") : std::string();
    }

    NSString* nsString(const std::string& text){
        NSString* s = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
        return s ? s : @"";
    }

    std::string mapError(NSError* error){
        if ([error.domain isEqualToString:NSURLErrorDomain]){
            switch (error.code){
                case NSURLErrorTimedOut: return "timeout";
                case NSURLErrorCannotFindHost:
                case NSURLErrorDNSLookupFailed: return "dns";
                case NSURLErrorCannotConnectToHost: return "connect";
                case NSURLErrorServerCertificateHasBadDate:
                case NSURLErrorServerCertificateUntrusted:
                case NSURLErrorServerCertificateHasUnknownRoot:
                case NSURLErrorServerCertificateNotYetValid:
                case NSURLErrorClientCertificateRejected:
                case NSURLErrorClientCertificateRequired:
                case NSURLErrorSecureConnectionFailed: return "tls";
                case NSURLErrorAppTransportSecurityRequiresSecureConnection: return "insecure_http";
                case NSURLErrorNotConnectedToInternet:
                case NSURLErrorInternationalRoamingOff:
                case NSURLErrorDataNotAllowed: return "offline";
                case NSURLErrorBadURL:
                case NSURLErrorUnsupportedURL: return "invalid_url";
                case NSURLErrorCancelled: return "cancelled";
                default: return "network";
            }
        }
        return "network";
    }

    class AppleBackend;

}

@interface JasperHttpSessionDelegate : NSObject <NSURLSessionDataDelegate>
@end

namespace {

    class AppleBackend : public HttpBackend {
    public:
        AppleBackend();
        ~AppleBackend() override{ shutdown(); }

        bool available(std::string&) override{ return true; }
        void start(const std::shared_ptr<HttpTransfer>& transfer) override;
        void cancel(uint64_t id) override;
        void shutdown() override;

        // the delegate's queue
        std::shared_ptr<AppleJob> jobOf(NSURLSessionTask* task){
            std::lock_guard<std::mutex> lock(mutex);
            auto it = byTask.find(task.taskIdentifier);
            return it == byTask.end() ? nullptr : it->second;
        }
        std::shared_ptr<AppleJob> takeJob(NSURLSessionTask* task){
            std::lock_guard<std::mutex> lock(mutex);
            auto it = byTask.find(task.taskIdentifier);
            if (it == byTask.end()) return nullptr;
            std::shared_ptr<AppleJob> job = std::move(it->second);
            byTask.erase(it);
            byId.erase(job->transfer->id);
            return job;
        }
        bool isTimedOut(const std::shared_ptr<AppleJob>& job){
            std::lock_guard<std::mutex> lock(mutex);
            return job->timedOut;
        }

        // the request of the transfer's current hop
        static NSMutableURLRequest* makeRequest(HttpTransfer& t){
            NSURL* url = [NSURL URLWithString:nsString(t.url)];
            if (!url) return nil;
            NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url];
            request.HTTPMethod = nsString(t.method);
            request.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
            request.HTTPShouldHandleCookies = NO;
            // NSURLSession's only per-request timeout is the idle time between packets (it also
            // bounds connecting): the time left of the whole request, which a timer enforces
            // (start); connectTimeout has no separate effect here
            request.timeoutInterval = t.totalTimeout() > 0.0 ? std::max(1.0, t.remainingTimeout()) : 1.0e6;
            NSMutableSet<NSString*>* named = [NSMutableSet set];
            for (const auto& [name, value] : t.headers){
                NSString* key = nsString(name);
                NSString* lower = key.lowercaseString;
                if ([named containsObject:lower]){
                    [request addValue:nsString(value) forHTTPHeaderField:key];
                } else {
                    [named addObject:lower];
                    [request setValue:nsString(value) forHTTPHeaderField:key];
                }
            }
            if (!t.body.empty() && t.method != "GET" && t.method != "HEAD"){
                request.HTTPBody = [NSData dataWithBytes:t.body.data() length:t.body.size()];
            }
            return request;
        }

        NSURLSession* session = nil;
        JasperHttpSessionDelegate* delegate = nil;
        NSOperationQueue* queue = nil;
        std::mutex mutex;
        std::unordered_map<NSUInteger, std::shared_ptr<AppleJob>> byTask;
        std::unordered_map<uint64_t, std::shared_ptr<AppleJob>> byId;
    };

    std::atomic<AppleBackend*> activeBackend{nullptr};

}

@implementation JasperHttpSessionDelegate

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task willPerformHTTPRedirection:(NSHTTPURLResponse*)response
        newRequest:(NSURLRequest*)request completionHandler:(void (^)(NSURLRequest*))completionHandler {
    (void)session;
    AppleBackend* backend = activeBackend;
    std::shared_ptr<AppleJob> job = backend ? backend->jobOf(task) : nullptr;
    if (!job){
        completionHandler(nil);
        return;
    }
    HttpTransfer& t = *job->transfer;
    std::string location = utf8([response valueForHTTPHeaderField:@"Location"]);
    if (location.empty()) location = utf8(request.URL.absoluteString);
    switch (t.redirect((int)response.statusCode, location)){
        case HttpRedirect::Action::Follow: {
            NSMutableURLRequest* next = AppleBackend::makeRequest(t);
            completionHandler(next);
            return;
        }
        case HttpRedirect::Action::Fail:
            completionHandler(nil);
            [task cancel];
            return;
        case HttpRedirect::Action::Stop:
            // the 3xx is the response
            completionHandler(nil);
            return;
    }
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)dataTask didReceiveResponse:(NSURLResponse*)response
        completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    (void)session;
    AppleBackend* backend = activeBackend;
    std::shared_ptr<AppleJob> job = backend ? backend->jobOf(dataTask) : nullptr;
    if (!job){
        completionHandler(NSURLSessionResponseCancel);
        return;
    }
    int status = 200;
    __block HttpHeaders headers;
    if ([response isKindOfClass:[NSHTTPURLResponse class]]){
        NSHTTPURLResponse* http = (NSHTTPURLResponse*)response;
        status = (int)http.statusCode;
        [http.allHeaderFields enumerateKeysAndObjectsUsingBlock:^(id key, id value, BOOL* stop) {
            (void)stop;
            if ([key isKindOfClass:[NSString class]] && [value isKindOfClass:[NSString class]]){
                headers.emplace_back(utf8((NSString*)key), utf8((NSString*)value));
            }
        }];
    }
    const bool keep = job->transfer->begin(status, std::move(headers), utf8(response.URL.absoluteString));
    completionHandler(keep ? NSURLSessionResponseAllow : NSURLSessionResponseCancel);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)dataTask didReceiveData:(NSData*)data {
    (void)session;
    AppleBackend* backend = activeBackend;
    std::shared_ptr<AppleJob> job = backend ? backend->jobOf(dataTask) : nullptr;
    if (!job) return;
    __block bool accepted = true;
    [data enumerateByteRangesUsingBlock:^(const void* bytes, NSRange range, BOOL* stop) {
        if (!job->transfer->write(bytes, range.length)){
            accepted = false;
            *stop = YES;
        }
    }];
    if (!accepted) [dataTask cancel];
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didReceiveChallenge:(NSURLAuthenticationChallenge*)challenge
        completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential*))completionHandler {
    (void)session;
    AppleBackend* backend = activeBackend;
    std::shared_ptr<AppleJob> job = backend ? backend->jobOf(task) : nullptr;
    if (job && !job->transfer->verifyTls &&
            [challenge.protectionSpace.authenticationMethod isEqualToString:NSURLAuthenticationMethodServerTrust] &&
            challenge.protectionSpace.serverTrust){
        completionHandler(NSURLSessionAuthChallengeUseCredential, [NSURLCredential credentialForTrust:challenge.protectionSpace.serverTrust]);
        return;
    }
    completionHandler(NSURLSessionAuthChallengePerformDefaultHandling, nil);
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    (void)session;
    AppleBackend* backend = activeBackend;
    std::shared_ptr<AppleJob> job = backend ? backend->takeJob(task) : nullptr;
    if (!job) return;
    HttpTransfer& t = *job->transfer;
    if (!error){
        t.finish("");
        return;
    }
    if (backend->isTimedOut(job)){
        t.finish("timeout");
        return;
    }
    // a cancel of ours (begin/write/redirect refused, or Http's cancel): the transfer knows why
    t.finish(mapError(error), utf8(error.localizedDescription));
}

@end

AppleBackend::AppleBackend(){
    NSURLSessionConfiguration* configuration = [NSURLSessionConfiguration defaultSessionConfiguration];
    configuration.URLCache = nil;
    configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    configuration.HTTPCookieStorage = nil;
    configuration.HTTPShouldSetCookies = NO;
    configuration.HTTPCookieAcceptPolicy = NSHTTPCookieAcceptPolicyNever;
    queue = [[NSOperationQueue alloc] init];
    queue.maxConcurrentOperationCount = 1;
    queue.name = @"JasperEngine HTTP";
    delegate = [[JasperHttpSessionDelegate alloc] init];
    // the session keeps its delegate until invalidateAndCancel (shutdown)
    session = [NSURLSession sessionWithConfiguration:configuration delegate:delegate delegateQueue:queue];
    activeBackend = this;
}

void AppleBackend::start(const std::shared_ptr<HttpTransfer>& transfer){
    NSMutableURLRequest* request = makeRequest(*transfer);
    if (!request){
        transfer->finish("invalid_url");
        return;
    }
    auto job = std::make_shared<AppleJob>();
    job->transfer = transfer;
    NSURLSessionDataTask* task = [session dataTaskWithRequest:request];
    job->task = task;
    {
        std::lock_guard<std::mutex> lock(mutex);
        byTask[task.taskIdentifier] = job;
        byId[transfer->id] = job;
    }
    if (transfer->totalTimeout() > 0.0){
        std::weak_ptr<AppleJob> weak = job;
        const double seconds = std::max(0.001, transfer->remainingTimeout());
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            std::shared_ptr<AppleJob> expired = weak.lock();
            if (!expired) return;
            NSURLSessionDataTask* running = nil;
            {
                std::lock_guard<std::mutex> lock(mutex);
                expired->timedOut = true;
                running = expired->task;
            }
            [running cancel];
        });
    }
    [task resume];
}

void AppleBackend::cancel(uint64_t id){
    NSURLSessionDataTask* task = nil;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = byId.find(id);
        if (it != byId.end()) task = it->second->task;
    }
    [task cancel];
}

void AppleBackend::shutdown(){
    if (activeBackend == this) activeBackend = nullptr;
    [session invalidateAndCancel];
    session = nil;
    std::unordered_map<uint64_t, std::shared_ptr<AppleJob>> left;
    {
        std::lock_guard<std::mutex> lock(mutex);
        left.swap(byId);
        byTask.clear();
    }
    for (auto& [id, job] : left) job->transfer->finish("cancelled");
}

HttpBackend* jasperengine::createPlatformHttpBackend(){
    return new AppleBackend();
}

#endif
