// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The native bridge's platform layer on iOS and macOS (core/native/NativePlatform.h):
// Objective-C (and @objc Swift) class methods, found with NSClassFromString and called through
// typed objc_msgSend casts. Sync: `+ method:(NSArray*)args`, then `+ method` without
// arguments; async: `+ method:(NSArray*)args call:(JasperNativeCall*)call` (a sync form serves
// an async call too: its return value resolves it). The engine thread is the main thread:
// async calls run later on the main queue (never inside the engine's call), or on a global
// queue for "background". Values cross as Foundation objects (JasperNative.h).

#import "JasperNativeInternal.h"

#include "Log.h"
#include "native/NativePlatform.h"

#import <objc/message.h>
#import <objc/runtime.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

using namespace jasperengine;

namespace {

    constexpr int maxDepth = 64;

    NSString* toNSString(const std::string& text){
        NSString* s = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
        return s ?: @"";
    }

    std::string toStd(NSString* text){
        if (!text) return std::string();
        const char* utf8 = text.UTF8String;
        return utf8 ? std::string(utf8) : std::string();
    }

    // JSON -> Foundation (NSNull, NSNumber, NSString, NSArray, NSDictionary)
    id toFoundation(const NativeJson& value){
        switch (value.type()){
            case NativeJson::value_t::null: return NSNull.null;
            case NativeJson::value_t::boolean: return value.get<bool>() ? @YES : @NO;
            case NativeJson::value_t::number_integer: return @(value.get<int64_t>());
            case NativeJson::value_t::number_unsigned: return @(value.get<uint64_t>());
            case NativeJson::value_t::number_float: return @(value.get<double>());
            case NativeJson::value_t::string: return toNSString(value.get_ref<const std::string&>());
            case NativeJson::value_t::array: {
                NSMutableArray* array = [NSMutableArray arrayWithCapacity:value.size()];
                for (const NativeJson& item : value) [array addObject:toFoundation(item)];
                return array;
            }
            case NativeJson::value_t::object: {
                NSMutableDictionary* dictionary = [NSMutableDictionary dictionaryWithCapacity:value.size()];
                for (auto it = value.begin(); it != value.end(); ++it) dictionary[toNSString(it.key())] = toFoundation(it.value());
                return dictionary;
            }
            default: return NSNull.null;
        }
    }

    // Foundation -> JSON; false with the reason (unsupported_type)
    bool toJson(id value, NativeJson& out, std::string& why, int depth = 0){
        if (depth > maxDepth){
            why = "a value nested deeper than 64";
            return false;
        }
        if (!value || value == NSNull.null){
            out = nullptr;
            return true;
        }
        if ([value isKindOfClass:NSNumber.class]){
            NSNumber* number = (NSNumber*)value;
            if (CFGetTypeID((__bridge CFTypeRef)number) == CFBooleanGetTypeID()){
                out = number.boolValue ? true : false;
                return true;
            }
            const char type = number.objCType[0];
            if (type == 'f' || type == 'd'){
                const double d = number.doubleValue;
                if (!std::isfinite(d)){
                    why = "NaN and infinities have no JSON form";
                    return false;
                }
                out = d;
                return true;
            }
            if (type == 'Q' || type == 'L' || type == 'I'){
                const unsigned long long u = number.unsignedLongLongValue;
                if (u > (unsigned long long)INT64_MAX) out = (double)u;
                else out = (int64_t)u;
                return true;
            }
            out = (int64_t)number.longLongValue;
            return true;
        }
        if ([value isKindOfClass:NSString.class]){
            out = toStd((NSString*)value);
            return true;
        }
        if ([value isKindOfClass:NSArray.class]){
            out = NativeJson::array();
            for (id item in (NSArray*)value){
                NativeJson converted;
                if (!toJson(item, converted, why, depth + 1)) return false;
                out.push_back(std::move(converted));
            }
            return true;
        }
        if ([value isKindOfClass:NSDictionary.class]){
            out = NativeJson::object();
            NSDictionary* dictionary = (NSDictionary*)value;
            for (id key in dictionary){
                if (![key isKindOfClass:NSString.class]){
                    why = std::string("a dictionary key that is a ") + toStd(NSStringFromClass([key class])) + " (keys must be strings)";
                    return false;
                }
                NativeJson converted;
                if (!toJson(dictionary[key], converted, why, depth + 1)) return false;
                out[toStd((NSString*)key)] = std::move(converted);
            }
            return true;
        }
        why = "a " + toStd(NSStringFromClass([value class])) + " has no JSON form (return nil, NSNull, NSNumber, NSString, NSArray or NSDictionary)";
        return false;
    }

    // A class method's return value boxed for toJson: '@' as is, scalars as NSNumber, void nil
    // (the cast matches the method's real return type: an id read of a double is undefined)
    bool invokeWith(Class cls, SEL selector, id arg, id call, int args, id __strong* result, std::string& why, bool& badArgs){
        badArgs = false;
        Method method = class_getClassMethod(cls, selector);
        if (!method){
            why = "no such selector";
            return false;
        }
        // the parameters must be objects (an int read from an NSArray pointer is garbage)
        const unsigned int parameters = method_getNumberOfArguments(method);   // self, _cmd, ...
        for (unsigned int i = 2; i < parameters; i++){
            char argument[32] = {0};
            method_getArgumentType(method, i, argument, sizeof(argument));
            const char* a = argument;
            while (*a && strchr("rnNoORV", *a)) a++;
            if (*a != '@'){
                why = std::string("must take objects: ") + (i == 2 ? "(NSArray*)args" : "(JasperNativeCall*)call") +
                      ", not the type " + argument;
                badArgs = true;
                return false;
            }
        }
        char type[32] = {0};
        method_getReturnType(method, type, sizeof(type));
        // skip type qualifiers (r, n, N, o, O, R, V)
        const char* t = type;
        while (*t && strchr("rnNoORV", *t)) t++;
        id receiver = cls;
#define JASPER_SEND(R) (args == 0 ? ((R (*)(id, SEL))objc_msgSend)(receiver, selector) \
                        : args == 1 ? ((R (*)(id, SEL, id))objc_msgSend)(receiver, selector, arg) \
                        : ((R (*)(id, SEL, id, id))objc_msgSend)(receiver, selector, arg, call))
        switch (*t){
            case '@': *result = JASPER_SEND(id); return true;
            case 'v':
                if (args == 0) ((void (*)(id, SEL))objc_msgSend)(receiver, selector);
                else if (args == 1) ((void (*)(id, SEL, id))objc_msgSend)(receiver, selector, arg);
                else ((void (*)(id, SEL, id, id))objc_msgSend)(receiver, selector, arg, call);
                *result = nil;
                return true;
            case 'B': *result = JASPER_SEND(bool) ? @YES : @NO; return true;
            case 'c': *result = JASPER_SEND(signed char) ? @YES : @NO; return true;   // BOOL on Intel macOS
            case 'C': *result = @(JASPER_SEND(unsigned char)); return true;
            case 's': *result = @(JASPER_SEND(short)); return true;
            case 'S': *result = @(JASPER_SEND(unsigned short)); return true;
            case 'i': *result = @(JASPER_SEND(int)); return true;
            case 'I': *result = @(JASPER_SEND(unsigned int)); return true;
            case 'l': *result = @(JASPER_SEND(long)); return true;
            case 'L': *result = @(JASPER_SEND(unsigned long)); return true;
            case 'q': *result = @(JASPER_SEND(long long)); return true;
            case 'Q': *result = @(JASPER_SEND(unsigned long long)); return true;
            case 'f': *result = @(JASPER_SEND(float)); return true;
            case 'd': *result = @(JASPER_SEND(double)); return true;
            default:
                why = std::string("returns a type the bridge cannot read (") + type + ")";
                return false;
        }
#undef JASPER_SEND
    }

    struct Selectors {
        SEL withArgs = nullptr;    // method:
        SEL bare = nullptr;        // method
        SEL async = nullptr;       // method:call:
    };

    Selectors selectorsOf(Class cls, const std::string& method){
        Selectors s;
        NSString* name = toNSString(method);
        SEL withArgs = NSSelectorFromString([name stringByAppendingString:@":"]);
        SEL bare = NSSelectorFromString(name);
        SEL async = NSSelectorFromString([name stringByAppendingString:@":call:"]);
        if (class_getClassMethod(cls, withArgs)) s.withArgs = withArgs;
        if (class_getClassMethod(cls, bare)) s.bare = bare;
        if (class_getClassMethod(cls, async)) s.async = async;
        return s;
    }

    // the calls native code holds, so a cancel reaches them
    std::mutex callsMutex;
    std::unordered_map<uint64_t, JasperNativeCall*> calls;

    // runs a sync form; false with err
    bool runSync(Class cls, const std::string& plugin, const std::string& method, const Selectors& s, NSArray* args,
                 NativeJson& out, NativeError& err){
        id result = nil;
        std::string why;
        bool ok = false;
        bool badArgs = false;
        @try {
            if (s.withArgs) ok = invokeWith(cls, s.withArgs, args, nil, 1, &result, why, badArgs);
            else ok = invokeWith(cls, s.bare, nil, nil, 0, &result, why, badArgs);
        } @catch (NSException* exception) {
            err = NativeError("exception", toStd(exception.name) + ": " + toStd(exception.reason));
            Log::error("Native: %s.%s threw %s: %s\n%s", plugin.c_str(), method.c_str(), toStd(exception.name).c_str(),
                       toStd(exception.reason).c_str(), toStd([exception.callStackSymbols componentsJoinedByString:@"\n"]).c_str());
            return false;
        }
        if (!ok){
            err = NativeError(badArgs ? "bad_args" : "unsupported_type", plugin + "." + method + " " + why);
            return false;
        }
        if (!toJson(result, out, why)){
            err = NativeError("unsupported_type", plugin + "." + method + " returned " + why);
            return false;
        }
        return true;
    }

    // NSClassFromString(plugin) when the class adopts JasperNativePlugin (any other class of the
    // app or the system is no plugin: a mock or "not_found" answers for it)
    Class pluginClass(const std::string& plugin){
        Class cls = NSClassFromString(toNSString(plugin));
        return (cls && jasperNativeIsPluginClass(cls)) ? cls : Nil;
    }

    class AppleNativePlatform : public NativePlatform {
    public:
        const char* name() const override{
#if TARGET_OS_IPHONE
            return "iOS";
#else
            return "macOS";
#endif
        }

        bool has(const std::string& plugin, const std::string& method) override{
            @autoreleasepool {
                Class cls = pluginClass(plugin);
                if (!cls) return false;
                if (method.empty()) return true;
                const Selectors s = selectorsOf(cls, method);
                return s.withArgs || s.bare || s.async;
            }
        }

        NativeJson call(const std::string& plugin, const std::string& method, const NativeJson& args, NativeError& err) override{
            @autoreleasepool {
                Class cls = pluginClass(plugin);
                if (!cls){
                    err = NativeError("not_found", "no Objective-C class adopting JasperNativePlugin for the native plugin \"" + plugin + "\"");
                    return nullptr;
                }
                Selectors s = selectorsOf(cls, method);
                // `+ method` only takes no arguments
                if (!args.empty()) s.bare = nullptr;
                if (!s.withArgs && !s.bare){
                    if (s.async){
                        err = NativeError("async_only", plugin + "." + method + " takes a JasperNativeCall: it is asynchronous, use Native.callAsync");
                    } else if (!args.empty() && class_getClassMethod(cls, NSSelectorFromString(toNSString(method)))){
                        err = NativeError("bad_args", plugin + "." + method + " takes no arguments (declare + (id)" + method + ":(NSArray*)args)");
                    } else {
                        err = NativeError("no_method", "the native plugin \"" + plugin + "\" has no class method " + method + ": (or " + method + ":call:)");
                    }
                    return nullptr;
                }
                NativeJson out;
                if (!runSync(cls, plugin, method, s, (NSArray*)toFoundation(args), out, err)) return nullptr;
                return out;
            }
        }

        void callAsync(uint64_t id, const std::string& plugin, const std::string& method, const NativeJson& args, bool background) override{
            NSArray* arguments = (NSArray*)toFoundation(args);
            NSString* pluginName = toNSString(plugin);
            NSString* methodName = toNSString(method);
            JasperNativeCall* call = [[JasperNativeCall alloc] initWithId:id plugin:pluginName method:methodName args:arguments];
            {
                std::lock_guard<std::mutex> lock(callsMutex);
                calls[id] = call;
            }
            const bool noArgs = args.empty();
            dispatch_block_t run = ^{
                @autoreleasepool {
                    if (call.isCancelled) return;
                    const std::string p = toStd(pluginName);
                    const std::string m = toStd(methodName);
                    Class cls = pluginClass(toStd(pluginName));
                    if (!cls){
                        [call failWithCode:@"not_found" message:[NSString stringWithFormat:@"no Objective-C class for the native plugin \"%@\"", pluginName]];
                        return;
                    }
                    Selectors s = selectorsOf(cls, m);
                    if (!noArgs) s.bare = nullptr;
                    if (s.async){
                        id ignored = nil;
                        std::string why;
                        bool badArgs = false;
                        @try {
                            if (!invokeWith(cls, s.async, arguments, call, 2, &ignored, why, badArgs)){
                                [call failWithCode:(badArgs ? @"bad_args" : @"unsupported_type") message:toNSString(p + "." + m + " " + why)];
                            }
                        } @catch (NSException* exception) {
                            Log::error("Native: %s.%s threw %s: %s", p.c_str(), m.c_str(), toStd(exception.name).c_str(), toStd(exception.reason).c_str());
                            [call failWithCode:@"exception" message:[NSString stringWithFormat:@"%@: %@", exception.name, exception.reason]];
                        }
                        return;
                    }
                    if (!s.withArgs && !s.bare){
                        [call failWithCode:@"no_method" message:toNSString("the native plugin \"" + p + "\" has no class method " + m + ":call: (or " + m + ":)")];
                        return;
                    }
                    NativeJson out;
                    NativeError err;
                    if (runSync(cls, p, m, s, arguments, out, err)){
                        // a sync form: its return value is the result (already JSON)
                        if (call.isCancelled) return;
                        jasperNativeBridgeForget(call.callId);
                        NativeBridge::completeFromPlatform(call.callId, std::move(out), nullptr);
                    } else {
                        [call failWithCode:toNSString(err.code) message:toNSString(err.message)];
                    }
                }
            };
            dispatch_async(background ? dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0) : dispatch_get_main_queue(), run);
        }

        void cancel(uint64_t id) override{
            JasperNativeCall* call = nil;
            {
                std::lock_guard<std::mutex> lock(callsMutex);
                auto it = calls.find(id);
                if (it == calls.end()) return;
                call = it->second;
                calls.erase(it);
            }
            [call markCancelled];
        }

        void initPlugins() override{
            [JasperNative dispatchInit];
        }

        void shutdown() override{
            std::unordered_map<uint64_t, JasperNativeCall*> all;
            {
                std::lock_guard<std::mutex> lock(callsMutex);
                all.swap(calls);
            }
            for (auto& entry : all) [entry.second markCancelled];
        }
    };

}

// ---------------------------------------------------------------- JasperNative.m's hooks

extern "C" void jasperNativeBridgeForget(uint64_t callId){
    std::lock_guard<std::mutex> lock(callsMutex);
    calls.erase(callId);
}

extern "C" void jasperNativeBridgeComplete(uint64_t callId, id value, NSString* code, NSString* message){
    jasperNativeBridgeForget(callId);
    if (code.length){
        const NativeError err(toStd(code), toStd(message));
        NativeBridge::completeFromPlatform(callId, nullptr, &err);
        return;
    }
    NativeJson out;
    std::string why;
    if (!toJson(value, out, why)){
        const NativeError err("unsupported_type", "resolved with " + why);
        NativeBridge::completeFromPlatform(callId, nullptr, &err);
        return;
    }
    NativeBridge::completeFromPlatform(callId, std::move(out), nullptr);
}

extern "C" void jasperNativeBridgeEmit(NSString* plugin, NSString* event, id payload){
    NativeJson out;
    std::string why;
    if (!toJson(payload, out, why)){
        Log::error("Native: [JasperNative emit:%s event:%s]: the payload is dropped: %s", toStd(plugin).c_str(), toStd(event).c_str(), why.c_str());
        return;
    }
    NativeBridge::emit(toStd(plugin), toStd(event), std::move(out));
}

extern "C" void jasperNativeBridgePause(BOOL pause){
    if (pause) NativeBridge::pauseEngine();
    else NativeBridge::resumeEngine();
}

extern "C" void jasperNativeBridgePost(dispatch_block_t block){
    dispatch_block_t copied = [block copy];
    NativeBridge::postToGameThread([copied]() { copied(); });
}

NativePlatform* jasperengine::createNativePlatform(){
    return new AppleNativePlatform();
}
