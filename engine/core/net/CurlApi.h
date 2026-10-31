// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPERENGINE_CURLAPI_H
#define JASPERENGINE_CURLAPI_H

// The part of libcurl's C API the Linux HTTP backend uses, declared here so the engine needs
// no curl development headers: the library is the system's (dlopen of libcurl.so.4), linked
// against the distribution's TLS stack and certificate store and updated with the OS. Every
// value below is part of libcurl's stable ABI (curl.h / multi.h, unchanged since 7.x).

#include <cstddef>
#include <cstdint>

namespace jasperengine::curlapi {

    typedef void CURL;
    typedef void CURLM;
    typedef int CURLcode;
    typedef int CURLMcode;
    typedef int CURLoption;
    typedef int CURLINFO;
    typedef int64_t curl_off_t;

    struct curl_slist {
        char* data;
        curl_slist* next;
    };

    enum : int { CURLMSG_DONE = 1 };

    struct CURLMsg {
        int msg;
        CURL* easy_handle;
        union {
            void* whatever;
            CURLcode result;
        } data;
    };

    struct curl_waitfd {
        int fd;
        short events;
        short revents;
    };

    // curl_global_init flags
    constexpr long CURL_GLOBAL_ALL = 3;

    // CURLoption = type base + number
    constexpr int CURLOPTTYPE_LONG = 0;
    constexpr int CURLOPTTYPE_OBJECTPOINT = 10000;
    constexpr int CURLOPTTYPE_FUNCTIONPOINT = 20000;
    constexpr int CURLOPTTYPE_OFF_T = 30000;

    constexpr CURLoption CURLOPT_WRITEDATA = CURLOPTTYPE_OBJECTPOINT + 1;
    constexpr CURLoption CURLOPT_URL = CURLOPTTYPE_OBJECTPOINT + 2;
    constexpr CURLoption CURLOPT_ERRORBUFFER = CURLOPTTYPE_OBJECTPOINT + 10;
    constexpr CURLoption CURLOPT_WRITEFUNCTION = CURLOPTTYPE_FUNCTIONPOINT + 11;
    constexpr CURLoption CURLOPT_POSTFIELDS = CURLOPTTYPE_OBJECTPOINT + 15;
    constexpr CURLoption CURLOPT_USERAGENT = CURLOPTTYPE_OBJECTPOINT + 18;
    constexpr CURLoption CURLOPT_HTTPHEADER = CURLOPTTYPE_OBJECTPOINT + 23;
    constexpr CURLoption CURLOPT_HEADERDATA = CURLOPTTYPE_OBJECTPOINT + 29;
    constexpr CURLoption CURLOPT_CUSTOMREQUEST = CURLOPTTYPE_OBJECTPOINT + 36;
    constexpr CURLoption CURLOPT_NOPROGRESS = CURLOPTTYPE_LONG + 43;
    constexpr CURLoption CURLOPT_NOBODY = CURLOPTTYPE_LONG + 44;
    constexpr CURLoption CURLOPT_POST = CURLOPTTYPE_LONG + 47;
    constexpr CURLoption CURLOPT_FOLLOWLOCATION = CURLOPTTYPE_LONG + 52;
    constexpr CURLoption CURLOPT_SSL_VERIFYPEER = CURLOPTTYPE_LONG + 64;
    constexpr CURLoption CURLOPT_MAXREDIRS = CURLOPTTYPE_LONG + 68;
    constexpr CURLoption CURLOPT_HEADERFUNCTION = CURLOPTTYPE_FUNCTIONPOINT + 79;
    constexpr CURLoption CURLOPT_HTTPGET = CURLOPTTYPE_LONG + 80;
    constexpr CURLoption CURLOPT_SSL_VERIFYHOST = CURLOPTTYPE_LONG + 81;
    constexpr CURLoption CURLOPT_NOSIGNAL = CURLOPTTYPE_LONG + 99;
    constexpr CURLoption CURLOPT_ACCEPT_ENCODING = CURLOPTTYPE_OBJECTPOINT + 102;
    constexpr CURLoption CURLOPT_PRIVATE = CURLOPTTYPE_OBJECTPOINT + 103;
    constexpr CURLoption CURLOPT_POSTFIELDSIZE_LARGE = CURLOPTTYPE_OFF_T + 120;
    constexpr CURLoption CURLOPT_TIMEOUT_MS = CURLOPTTYPE_LONG + 155;
    constexpr CURLoption CURLOPT_CONNECTTIMEOUT_MS = CURLOPTTYPE_LONG + 156;

    // CURLINFO = type base + number
    constexpr int CURLINFO_STRING = 0x100000;
    constexpr int CURLINFO_LONG = 0x200000;
    constexpr CURLINFO CURLINFO_EFFECTIVE_URL = CURLINFO_STRING + 1;
    constexpr CURLINFO CURLINFO_RESPONSE_CODE = CURLINFO_LONG + 2;
    constexpr CURLINFO CURLINFO_PRIVATE = CURLINFO_STRING + 21;
    constexpr CURLINFO CURLINFO_REDIRECT_URL = CURLINFO_STRING + 31;

    constexpr size_t CURL_ERROR_SIZE = 256;
    // a write callback's "abort"
    constexpr size_t CURL_WRITEFUNC_ERROR = 0;

    // CURLcode values
    constexpr CURLcode CURLE_OK = 0;
    constexpr CURLcode CURLE_UNSUPPORTED_PROTOCOL = 1;
    constexpr CURLcode CURLE_URL_MALFORMAT = 3;
    constexpr CURLcode CURLE_COULDNT_RESOLVE_PROXY = 5;
    constexpr CURLcode CURLE_COULDNT_RESOLVE_HOST = 6;
    constexpr CURLcode CURLE_COULDNT_CONNECT = 7;
    constexpr CURLcode CURLE_WRITE_ERROR = 23;
    constexpr CURLcode CURLE_OUT_OF_MEMORY = 27;
    constexpr CURLcode CURLE_OPERATION_TIMEDOUT = 28;
    constexpr CURLcode CURLE_SSL_CONNECT_ERROR = 35;
    constexpr CURLcode CURLE_ABORTED_BY_CALLBACK = 42;
    constexpr CURLcode CURLE_TOO_MANY_REDIRECTS = 47;
    constexpr CURLcode CURLE_GOT_NOTHING = 52;
    constexpr CURLcode CURLE_SSL_ENGINE_NOTFOUND = 53;
    constexpr CURLcode CURLE_SSL_ENGINE_SETFAILED = 54;
    constexpr CURLcode CURLE_SEND_ERROR = 55;
    constexpr CURLcode CURLE_RECV_ERROR = 56;
    constexpr CURLcode CURLE_SSL_CERTPROBLEM = 58;
    constexpr CURLcode CURLE_SSL_CIPHER = 59;
    constexpr CURLcode CURLE_PEER_FAILED_VERIFICATION = 60;
    constexpr CURLcode CURLE_BAD_CONTENT_ENCODING = 61;
    constexpr CURLcode CURLE_SSL_ENGINE_INITFAILED = 66;
    constexpr CURLcode CURLE_SSL_CACERT_BADFILE = 77;
    constexpr CURLcode CURLE_SSL_SHUTDOWN_FAILED = 80;
    constexpr CURLcode CURLE_SSL_CRL_BADFILE = 82;
    constexpr CURLcode CURLE_SSL_ISSUER_ERROR = 83;
    constexpr CURLcode CURLE_SSL_PINNEDPUBKEYNOTMATCH = 90;
    constexpr CURLcode CURLE_SSL_INVALIDCERTSTATUS = 91;
    constexpr CURLcode CURLE_SSL_CLIENTCERT = 98;

    constexpr CURLMcode CURLM_OK = 0;

    typedef size_t (*curl_write_callback)(char* data, size_t size, size_t count, void* user);

    // the functions, resolved with dlsym (the optional ones may be null on old libraries)
    struct Api {
        CURLcode (*global_init)(long flags) = nullptr;
        const char* (*version)() = nullptr;
        CURL* (*easy_init)() = nullptr;
        void (*easy_cleanup)(CURL*) = nullptr;
        CURLcode (*easy_setopt)(CURL*, CURLoption, ...) = nullptr;
        CURLcode (*easy_getinfo)(CURL*, CURLINFO, ...) = nullptr;
        const char* (*easy_strerror)(CURLcode) = nullptr;
        CURLM* (*multi_init)() = nullptr;
        CURLMcode (*multi_cleanup)(CURLM*) = nullptr;
        CURLMcode (*multi_add_handle)(CURLM*, CURL*) = nullptr;
        CURLMcode (*multi_remove_handle)(CURLM*, CURL*) = nullptr;
        CURLMcode (*multi_perform)(CURLM*, int* running) = nullptr;
        CURLMsg* (*multi_info_read)(CURLM*, int* queued) = nullptr;
        CURLMcode (*multi_wait)(CURLM*, curl_waitfd*, unsigned int, int timeoutMs, int* numfds) = nullptr;
        // 7.66 / 7.68: a wait that a wakeup from another thread ends
        CURLMcode (*multi_poll)(CURLM*, curl_waitfd*, unsigned int, int timeoutMs, int* numfds) = nullptr;
        CURLMcode (*multi_wakeup)(CURLM*) = nullptr;
        curl_slist* (*slist_append)(curl_slist*, const char*) = nullptr;
        void (*slist_free_all)(curl_slist*) = nullptr;
    };

}

#endif // JASPERENGINE_CURLAPI_H
