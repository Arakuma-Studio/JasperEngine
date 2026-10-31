package com.jaspergameengine.jasperengine;

import android.os.Build;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.Field;
import java.net.ConnectException;
import java.net.HttpURLConnection;
import java.net.NoRouteToHostException;
import java.net.ProtocolException;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.net.UnknownHostException;
import java.security.KeyStore;
import java.security.cert.Certificate;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

import javax.net.ssl.HostnameVerifier;
import javax.net.ssl.HttpsURLConnection;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLException;
import javax.net.ssl.SSLSession;
import javax.net.ssl.SSLSocketFactory;
import javax.net.ssl.TrustManager;
import javax.net.ssl.TrustManagerFactory;
import javax.net.ssl.X509TrustManager;

/**
 * The Android backend of the engine's HTTP client (engine/platform/android/HttpBackendAndroid.cpp,
 * engine/core/net/Http.h). One request runs on one thread of a pool; the engine keeps the
 * concurrency limit, the redirect rules and the body handling (files, limits, decoding) in C++:
 * this class moves bytes. Redirects are followed one hop at a time after asking the engine
 * (nativeRedirect), also across http -> https, which HttpURLConnection never follows itself.
 * The native methods are registered by NativeEngine::setupJNI (RegisterNatives).
 */
public final class JasperHttp {

    private JasperHttp() {}

    // ---- the engine (C++)
    private static native boolean nativeResponse(long id, int status, String finalUrl, String[] headers);
    private static native boolean nativeData(long id, byte[] data, int length);
    private static native void nativeComplete(long id, String errorCode, String message);
    // null: deliver the 3xx; {"follow", url, method, keepBody ("1"/"0"), name, value, ...}; {"fail"}
    private static native String[] nativeRedirect(long id, int status, String location);

    private static final ExecutorService pool = Executors.newCachedThreadPool(new ThreadFactory() {
        private final AtomicInteger count = new AtomicInteger(1);
        @Override
        public Thread newThread(Runnable r) {
            Thread thread = new Thread(r, "JasperHttp-" + count.getAndIncrement());
            thread.setDaemon(true);
            return thread;
        }
    });
    // the whole-request timeouts
    private static final ScheduledExecutorService timer = Executors.newSingleThreadScheduledExecutor(new ThreadFactory() {
        @Override
        public Thread newThread(Runnable r) {
            Thread thread = new Thread(r, "JasperHttp-timer");
            thread.setDaemon(true);
            return thread;
        }
    });

    private static final class Job {
        final long id;
        volatile boolean cancelled;
        volatile boolean timedOut;
        volatile HttpURLConnection connection;
        Job(long id) { this.id = id; }

        void abort() {
            final HttpURLConnection c = connection;
            if (c == null) return;
            // disconnect() may touch the network: never on the caller's (main) thread
            pool.execute(new Runnable() {
                @Override
                public void run() {
                    try { c.disconnect(); } catch (Throwable ignored) {}
                }
            });
        }
    }

    private static final Map<Long, Job> jobs = new ConcurrentHashMap<>();

    /** The game thread: starts request `id` (headers are name, value pairs). */
    public static void start(final long id, final String method, final String url, final String[] headers, final byte[] body,
                             final int connectTimeoutMs, final int timeoutMs, final boolean verifyTls) {
        final Job job = new Job(id);
        jobs.put(id, job);
        pool.execute(new Runnable() {
            @Override
            public void run() {
                String[] result;
                try {
                    result = transfer(job, method, url, headers, body, connectTimeoutMs, timeoutMs, verifyTls);
                } catch (Throwable t) {
                    result = new String[] {"network", String.valueOf(t)};
                }
                jobs.remove(id);
                nativeComplete(id, result[0], result[1]);
            }
        });
    }

    /** The game thread: ends request `id` soon (its completion reports "cancelled"). */
    public static void cancel(long id) {
        Job job = jobs.get(id);
        if (job == null) return;
        job.cancelled = true;
        job.abort();
    }

    // {errorCode, message}: {"", ""} when the response was read whole
    @SuppressWarnings("deprecation") // new URL(String): deprecated on desktop JDKs only
    private static String[] transfer(final Job job, String method, String url, String[] headers, byte[] body,
                                int connectTimeoutMs, int timeoutMs, boolean verifyTls) {
        final long deadline = timeoutMs > 0 ? System.nanoTime() + timeoutMs * 1000000L : Long.MAX_VALUE;
        ScheduledFuture<?> watchdog = null;
        if (timeoutMs > 0) {
            watchdog = timer.schedule(new Runnable() {
                @Override
                public void run() {
                    job.timedOut = true;
                    job.abort();
                }
            }, timeoutMs, TimeUnit.MILLISECONDS);
        }
        try {
            while (true) {
                if (job.cancelled) return new String[] {"cancelled", ""};
                final long left = deadline == Long.MAX_VALUE ? Integer.MAX_VALUE : (deadline - System.nanoTime()) / 1000000L;
                if (left <= 0) return new String[] {"timeout", ""};

                HttpURLConnection c;
                try {
                    c = (HttpURLConnection) new URL(url).openConnection();
                } catch (IOException | ClassCastException e) {
                    return new String[] {"invalid_url", String.valueOf(e.getMessage())};
                }
                job.connection = c;
                try {
                    c.setInstanceFollowRedirects(false);
                    c.setUseCaches(false);
                    c.setConnectTimeout((int) Math.max(1, Math.min(connectTimeoutMs > 0 ? connectTimeoutMs : Integer.MAX_VALUE, left)));
                    c.setReadTimeout((int) Math.max(1, Math.min(Integer.MAX_VALUE, left)));
                    if (c instanceof HttpsURLConnection) configureTls((HttpsURLConnection) c, verifyTls);
                    setMethod(c, method);
                    final java.util.Set<String> named = new java.util.HashSet<>();
                    for (int i = 0; i + 1 < headers.length; i += 2) {
                        // the first one replaces the stack's default (User-Agent), the next ones add
                        if (named.add(headers[i].toLowerCase(java.util.Locale.ROOT))) c.setRequestProperty(headers[i], headers[i + 1]);
                        else c.addRequestProperty(headers[i], headers[i + 1]);
                    }
                    final boolean sendsBody = body != null && body.length > 0 && !"GET".equals(method) && !"HEAD".equals(method);
                    final boolean emptyBodyMethod = "POST".equals(method) || "PUT".equals(method) || "PATCH".equals(method);
                    if (sendsBody || emptyBodyMethod) {
                        c.setDoOutput(true);
                        c.setFixedLengthStreamingMode(sendsBody ? body.length : 0);
                        OutputStream out = c.getOutputStream();
                        if (sendsBody) out.write(body);
                        out.close();
                    }
                    if (job.cancelled) return new String[] {"cancelled", ""};
                    final int status = c.getResponseCode();
                    if (status < 0) return new String[] {"network", "not an HTTP response"};

                    final String location = c.getHeaderField("Location");
                    if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && location != null) {
                        final String[] next = nativeRedirect(job.id, status, location);
                        if (next != null && next.length > 0 && "fail".equals(next[0])) return new String[] {"", ""};
                        if (next != null && next.length >= 4 && "follow".equals(next[0])) {
                            drain(c);
                            c.disconnect();
                            url = next[1];
                            method = next[2];
                            if (!"1".equals(next[3])) body = null;
                            final String[] nextHeaders = new String[next.length - 4];
                            System.arraycopy(next, 4, nextHeaders, 0, nextHeaders.length);
                            headers = nextHeaders;
                            continue;
                        }
                    }

                    if (!nativeResponse(job.id, status, c.getURL().toString(), responseHeaders(c))) {
                        c.disconnect();
                        return new String[] {"", ""};
                    }
                    if ("HEAD".equals(method)) return new String[] {"", ""};
                    InputStream in;
                    try {
                        in = status >= 400 ? c.getErrorStream() : c.getInputStream();
                    } catch (IOException e) {
                        in = c.getErrorStream();
                    }
                    if (in == null) return new String[] {"", ""};
                    final byte[] buffer = new byte[64 * 1024];
                    try {
                        int n;
                        while ((n = in.read(buffer)) != -1) {
                            if (job.cancelled) return new String[] {"cancelled", ""};
                            if (n > 0 && !nativeData(job.id, buffer, n)) return new String[] {"", ""};
                        }
                    } finally {
                        try { in.close(); } catch (IOException ignored) {}
                    }
                    return new String[] {"", ""};
                } catch (SocketTimeoutException e) {
                    return failure(job, "timeout", e);
                } catch (UnknownHostException e) {
                    return failure(job, "dns", e);
                } catch (ConnectException | NoRouteToHostException e) {
                    return failure(job, "connect", e);
                } catch (SSLException e) {
                    return failure(job, "tls", e);
                } catch (UnsupportedMethod e) {
                    return new String[] {"unsupported", e.getMessage()};
                } catch (IOException e) {
                    return failure(job, "network", e);
                } finally {
                    job.connection = null;
                }
            }
        } finally {
            if (watchdog != null) watchdog.cancel(false);
        }
    }

    // a failure, unless the request was cancelled or timed out (its connection was closed for that)
    private static String[] failure(Job job, String code, Exception e) {
        if (job.cancelled) return new String[] {"cancelled", ""};
        if (job.timedOut) return new String[] {"timeout", ""};
        return new String[] {code, String.valueOf(e.getMessage())};
    }

    private static final class UnsupportedMethod extends IOException {
        private static final long serialVersionUID = 1L;
        UnsupportedMethod(String message) { super(message); }
    }

    // HttpURLConnection knows GET POST HEAD OPTIONS PUT DELETE TRACE (and PATCH on Android's
    // OkHttp-based one); another token goes into the field directly
    private static void setMethod(HttpURLConnection c, String method) throws IOException {
        try {
            c.setRequestMethod(method);
        } catch (ProtocolException e) {
            try {
                Field field = HttpURLConnection.class.getDeclaredField("method");
                field.setAccessible(true);
                field.set(c, method);
            } catch (Exception reflection) {
                throw new UnsupportedMethod("the method " + method + " is not supported on Android");
            }
        }
    }

    private static String[] responseHeaders(HttpURLConnection c) {
        List<String> out = new ArrayList<>();
        Map<String, List<String>> fields = c.getHeaderFields();
        if (fields != null) {
            for (Map.Entry<String, List<String>> field : fields.entrySet()) {
                // the null key is the status line
                if (field.getKey() == null || field.getValue() == null) continue;
                for (String value : field.getValue()) {
                    out.add(field.getKey());
                    out.add(value == null ? "" : value);
                }
            }
        }
        return out.toArray(new String[0]);
    }

    private static void drain(HttpURLConnection c) {
        try {
            InputStream in = c.getErrorStream();
            if (in == null) in = c.getInputStream();
            if (in == null) return;
            byte[] buffer = new byte[8192];
            while (in.read(buffer) != -1) {}
            in.close();
        } catch (IOException ignored) {}
    }

    // ---- TLS

    private static SSLSocketFactory legacyFactory;
    private static SSLSocketFactory insecureFactory;

    private static synchronized void configureTls(HttpsURLConnection c, boolean verifyTls) {
        if (!verifyTls) {
            if (insecureFactory == null) {
                try {
                    SSLContext context = SSLContext.getInstance("TLS");
                    context.init(null, new TrustManager[] {new X509TrustManager() {
                        @Override public void checkClientTrusted(X509Certificate[] chain, String authType) {}
                        @Override public void checkServerTrusted(X509Certificate[] chain, String authType) {}
                        @Override public X509Certificate[] getAcceptedIssuers() { return new X509Certificate[0]; }
                    }}, null);
                    insecureFactory = context.getSocketFactory();
                } catch (Exception e) {
                    return;
                }
            }
            c.setSSLSocketFactory(insecureFactory);
            c.setHostnameVerifier(new HostnameVerifier() {
                @Override public boolean verify(String hostname, SSLSession session) { return true; }
            });
            return;
        }
        // Android 7.1 (API 25) and newer trust Let's Encrypt; older ones get its roots added to
        // the system's (the reason games installed accept-all handlers there)
        if (Build.VERSION.SDK_INT >= 25) return;
        if (legacyFactory == null) {
            try {
                KeyStore store = KeyStore.getInstance(KeyStore.getDefaultType());
                store.load(null, null);
                KeyStore system = KeyStore.getInstance("AndroidCAStore");
                system.load(null, null);
                java.util.Enumeration<String> aliases = system.aliases();
                while (aliases.hasMoreElements()) {
                    String alias = aliases.nextElement();
                    Certificate certificate = system.getCertificate(alias);
                    if (certificate != null) store.setCertificateEntry(alias, certificate);
                }
                CertificateFactory factory = CertificateFactory.getInstance("X.509");
                store.setCertificateEntry("jasper-isrg-root-x1", factory.generateCertificate(new ByteArrayInputStream(ISRG_ROOT_X1.getBytes("US-ASCII"))));
                store.setCertificateEntry("jasper-isrg-root-x2", factory.generateCertificate(new ByteArrayInputStream(ISRG_ROOT_X2.getBytes("US-ASCII"))));
                TrustManagerFactory trust = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
                trust.init(store);
                SSLContext context = SSLContext.getInstance("TLS");
                context.init(null, trust.getTrustManagers(), null);
                legacyFactory = context.getSocketFactory();
            } catch (Exception e) {
                return;
            }
        }
        c.setSSLSocketFactory(legacyFactory);
    }

    // Let's Encrypt's roots (https://letsencrypt.org/certificates/), valid until 2035 / 2040
    private static final String ISRG_ROOT_X1 =
        "-----BEGIN CERTIFICATE-----\n" +
        "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\n" +
        "-----END CERTIFICATE-----\n";
    private static final String ISRG_ROOT_X2 =
        "-----BEGIN CERTIFICATE-----\n" +
        "MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw\n" +
        "CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg\n" +
        "R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00\n" +
        "MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT\n" +
        "ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw\n" +
        "EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW\n" +
        "+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9\n" +
        "ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T\n" +
        "AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI\n" +
        "zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW\n" +
        "tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1\n" +
        "/q4AaOeMSQ+2b1tbFfLn\n" +
        "-----END CERTIFICATE-----\n";
}
