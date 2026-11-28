// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A player's "Report a Problem" (UserReport.h): the capture's state machine on the main thread,
// the JPEG encode on a job (the web's single thread: in the frame after the read), and the
// report's content, written by the crash handler (CrashHandler.cpp writeUserReport).

#include "UserReport.h"
#include "Journal.h"
#include "detail/CrashState.h"

#include "Engine.h"
#include "System.h"
#include "device/Device.h"
#include "jobs/JobChannel.h"
#include "jobs/JobSystem.h"
#include "log/Logger.h"
#include "pool/ShaderPool.h"
#include "pool/TextureDataPool.h"
#include "render/SystemRender.h"

#include "json.hpp"
#include "stb_image_write.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>

using namespace jasperengine;

namespace je_user_report {

    enum class Phase {
        Idle,
        Capturing,       // the next drawn frames are read until one has no stand-in
        EncodePending,   // single-threaded: the read frame is encoded at the next update
        Encoding,        // a job encodes the read frame
        Finished,        // encoded or failed: the callback runs at the next update
    };

    struct UserReportState {
        Phase phase = Phase::Idle;
        diag::userreport::Callback callback;
        int maxWidth = diag::userreport::kDefaultMaxWidth;
        size_t maxBytes = diag::userreport::kDefaultMaxBytes;
        int framesTried = 0;
        bool idleAtFrameStart = true;
        // a new capture, a cancel or a reset makes an encode still running stale
        uint64_t generation = 0;
        std::vector<uint8_t> pendingRgba;
        int pendingWidth = 0;
        int pendingHeight = 0;
        // the draft: the screenshot (empty when there is none) and why there is none
        bool draft = false;
        std::string jpeg;
        std::string error;
    };

    UserReportState& userReportState() {
        static UserReportState* s = new UserReportState();
        return *s;
    }

    // a reset (Play stop) drops async report callbacks still in flight
    std::atomic<uint64_t> createEpoch{0};

    bool (*creationIdleProbe)() = nullptr;

    // what a screenshot waits for (spec S32): no shader or pipeline being made, no texture loading
    bool creationIdle() {
        if (creationIdleProbe) return creationIdleProbe();
        return ShaderPool::isCreationIdle() && TextureDataPool::getLoadingCount() == 0;
    }

    void appendBytes(void* context, void* data, int size) {
        static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<size_t>(size));
    }

    // RGBA rows top first to RGB at most maxWidth wide (an area average: each destination pixel
    // is the mean of the source pixels it covers). R31's 60 ms at 1080p leave no room for divisions
    // per pixel: the columns' spans are found once, and a mean divides through an exact reciprocal
    std::vector<uint8_t> downscale(const std::vector<uint8_t>& rgba, int width, int height, int maxWidth, int& outWidth, int& outHeight) {
        outWidth = std::max(1, std::min(width, maxWidth));
        outHeight = std::max(1, static_cast<int>(static_cast<int64_t>(height) * outWidth / width));
        std::vector<uint8_t> rgb(static_cast<size_t>(outWidth) * outHeight * 3);
        std::vector<int> columns(static_cast<size_t>(outWidth) * 2);
        int widest = 1;
        for (int x = 0; x < outWidth; x++) {
            const int x0 = static_cast<int>(static_cast<int64_t>(x) * width / outWidth);
            const int x1 = std::max(x0 + 1, static_cast<int>(static_cast<int64_t>(x + 1) * width / outWidth));
            columns[static_cast<size_t>(x) * 2] = x0;
            columns[static_cast<size_t>(x) * 2 + 1] = x1;
            widest = std::max(widest, x1 - x0);
        }
        // (sum + count / 2) / count as (n * ceil(2^32 / count)) >> 32: exact while n and count are
        // below 2^16 (n * count < 2^32), so for blocks of at most 256 pixels; larger ones divide
        std::vector<uint64_t> reciprocals;
        for (int y = 0; y < outHeight; y++) {
            const int y0 = static_cast<int>(static_cast<int64_t>(y) * height / outHeight);
            const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<int64_t>(y + 1) * height / outHeight));
            const int rows = y1 - y0;
            const bool fast = static_cast<int64_t>(rows) * widest <= 256;
            if (fast && reciprocals.size() <= static_cast<size_t>(rows * widest)) {
                for (size_t count = reciprocals.size(); count <= static_cast<size_t>(rows * widest); count++)
                    reciprocals.push_back(count == 0 ? 0 : ((uint64_t(1) << 32) + count - 1) / count);
            }
            uint8_t* out = rgb.data() + static_cast<size_t>(y) * outWidth * 3;
            for (int x = 0; x < outWidth; x++, out += 3) {
                const int x0 = columns[static_cast<size_t>(x) * 2];
                const int x1 = columns[static_cast<size_t>(x) * 2 + 1];
                uint32_t sum[3] = {0, 0, 0};
                for (int sy = y0; sy < y1; sy++) {
                    const uint8_t* row = rgba.data() + (static_cast<size_t>(sy) * width + x0) * 4;
                    for (int sx = x0; sx < x1; sx++, row += 4) {
                        sum[0] += row[0];
                        sum[1] += row[1];
                        sum[2] += row[2];
                    }
                }
                const uint32_t count = static_cast<uint32_t>(rows * (x1 - x0));
                if (fast) {
                    const uint64_t reciprocal = reciprocals[count];
                    for (int c = 0; c < 3; c++) out[c] = static_cast<uint8_t>((static_cast<uint64_t>(sum[c] + count / 2) * reciprocal) >> 32);
                } else {
                    for (int c = 0; c < 3; c++) out[c] = static_cast<uint8_t>((sum[c] + count / 2) / count);
                }
            }
        }
        return rgb;
    }

    std::string jpegOf(const std::vector<uint8_t>& rgb, int width, int height, int quality) {
        std::string out;
        if (!stbi_write_jpg_to_func(&appendBytes, &out, width, height, 3, rgb.data(), quality)) out.clear();
        return out;
    }

    void finish(UserReportState& s, std::string jpeg, std::string error) {
        s.phase = Phase::Finished;
        s.draft = true;
        s.jpeg = std::move(jpeg);
        s.error = std::move(error);
    }

    void applyEncoded(UserReportState& s, diag::userreport::Encoded&& encoded) {
        if (encoded.jpeg.empty()) {
            finish(s, {}, encoded.error.empty() ? std::string("screenshot_too_large") : encoded.error);
            return;
        }
        finish(s, std::move(encoded.jpeg), {});
    }

    bool validCategory(const std::string& category) {
        if (category.empty() || category.size() > 32) return false;
        for (char c : category) {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
        }
        return true;
    }

    // The content of a report: what needs the main thread (the draft, the device) gathered now
    diag::crash::UserReportContent contentOf(const diag::userreport::Options& options) {
        UserReportState& s = userReportState();
        diag::crash::UserReportContent content;
        content.note = options.note;
        content.category = options.category.empty() ? std::string("other") : options.category;
        content.fields = options.fields;
        content.deviceJson = diag::userreport::deviceJson();
        content.journal = diag::journal::text();
        if (options.logTail.value_or(diag::crash::detail::state().includeLogTail)) content.logTail = diag::crash::ringContents();
        if (options.screenshot && s.draft) content.screenshotJpeg = s.jpeg;
        // the report ends the draft (and a capture still running for it)
        s.generation++;
        s.phase = Phase::Idle;
        s.callback = nullptr;
        s.draft = false;
        s.jpeg.clear();
        s.error.clear();
        s.pendingRgba.clear();
        return content;
    }

    void noteCreated(const diag::crash::ReportInfo& report) {
        const diag::journal::Field fields[] = {{"id", std::string_view(report.id)}};
        diag::journal::logEngine("engine.user_report", fields);
        Logger::write(LogLevel::Info, logCategoryEngine(), "User report: " + report.id + " (" + report.reason + ")");
    }

}

namespace jasperengine::diag::userreport {

    // inside this namespace only: a unity build's next source must not see je_user_report's names

    void begin(Callback fn, int maxWidth, size_t maxBytes) {
        je_user_report::UserReportState& s = je_user_report::userReportState();
        if (s.phase == je_user_report::Phase::Capturing || s.phase == je_user_report::Phase::EncodePending || s.phase == je_user_report::Phase::Encoding) {
            s.callback = std::move(fn);
            return;
        }
        s.generation++;
        s.phase = je_user_report::Phase::Capturing;
        s.callback = std::move(fn);
        s.maxWidth = maxWidth > 0 ? maxWidth : kDefaultMaxWidth;
        s.maxBytes = maxBytes > 0 ? maxBytes : kDefaultMaxBytes;
        s.framesTried = 0;
        s.draft = false;
        s.jpeg.clear();
        s.error.clear();
        s.pendingRgba.clear();
    }

    void cancel() {
        je_user_report::UserReportState& s = je_user_report::userReportState();
        s.generation++;
        s.phase = je_user_report::Phase::Idle;
        s.callback = nullptr;
        s.draft = false;
        s.jpeg.clear();
        s.error.clear();
        s.pendingRgba.clear();
    }

    void reset() {
        cancel();
        je_user_report::createEpoch.fetch_add(1, std::memory_order_relaxed);
    }

    bool wantsCapture() {
        return je_user_report::userReportState().phase == je_user_report::Phase::Capturing;
    }

    void beforeCaptureFrame() {
        je_user_report::userReportState().idleAtFrameStart = je_user_report::creationIdle();
    }

    void frameDrawn(FrameReader reader) {
        je_user_report::UserReportState& s = je_user_report::userReportState();
        if (s.phase != je_user_report::Phase::Capturing) return;
        // a stand-in or a skipped draw would be in the image: the next frame is read instead
        if (!s.idleAtFrameStart || !je_user_report::creationIdle()) {
            if (++s.framesTried >= kMaxCaptureFrames) je_user_report::finish(s, {}, "screenshot_unavailable: shaders still building");
            return;
        }
        std::vector<uint8_t> rgba;
        int width = 0, height = 0;
        std::string error;
        const auto start = std::chrono::steady_clock::now();
        const bool read = reader && reader(rgba, width, height, error);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (!read || width <= 0 || height <= 0 || rgba.size() != static_cast<size_t>(width) * height * 4) {
            je_user_report::finish(s, {}, "screenshot_unavailable: " + (error.empty() ? std::string("the frame could not be read") : error));
            return;
        }
        Logger::writef(LogLevel::Info, logCategoryEngine(), nullptr, "User report: captured %dx%d in %.1f ms", width, height, ms);
        if (!jobs::hasSystem() || jobs::system().isSingleThreaded()) {
            // no worker (the web): the frame after the read encodes it on the main thread
            s.phase = je_user_report::Phase::EncodePending;
            s.pendingRgba = std::move(rgba);
            s.pendingWidth = width;
            s.pendingHeight = height;
            return;
        }
        s.phase = je_user_report::Phase::Encoding;
        const uint64_t generation = s.generation;
        const int maxWidth = s.maxWidth;
        const size_t maxBytes = s.maxBytes;
        jobs::enqueueDetached([pixels = std::move(rgba), width, height, maxWidth, maxBytes, generation]() mutable {
            Encoded encoded = encode(std::move(pixels), width, height, maxWidth, maxBytes);
            jobs::postToMain([encoded = std::move(encoded), generation]() mutable {
                je_user_report::UserReportState& current = je_user_report::userReportState();
                if (current.generation != generation || current.phase != je_user_report::Phase::Encoding) return;
                je_user_report::applyEncoded(current, std::move(encoded));
            });
        }, jobs::Lane::Normal, "user-report-encode");
    }

    void update() {
        je_user_report::UserReportState& s = je_user_report::userReportState();
        if (s.phase == je_user_report::Phase::EncodePending) {
            Encoded encoded = encode(std::move(s.pendingRgba), s.pendingWidth, s.pendingHeight, s.maxWidth, s.maxBytes);
            s.pendingRgba.clear();
            je_user_report::applyEncoded(s, std::move(encoded));
            // the callback waits for the next frame, as it does after a job
            return;
        }
        if (s.phase != je_user_report::Phase::Finished) return;
        s.phase = je_user_report::Phase::Idle;
        Callback callback = std::move(s.callback);
        s.callback = nullptr;
        if (callback) callback(!s.jpeg.empty(), s.error);
    }

    bool hasDraft() {
        return je_user_report::userReportState().draft;
    }

    bool draftHasScreenshot() {
        return je_user_report::userReportState().draft && !je_user_report::userReportState().jpeg.empty();
    }

    std::string validate(const Options& options) {
        if (options.note.empty()) return "invalid_note: the note is empty";
        if (options.note.size() > kMaxNoteBytes) return "invalid_note: the note is over 4000 bytes";
        if (!je_user_report::validCategory(options.category)) return "invalid_category: \"" + options.category + "\" is not [a-z0-9_]{1,32}";
        if (options.fields.size() > kMaxFields) return "invalid_fields: at most 16 fields";
        return {};
    }

    std::optional<crash::ReportInfo> create(const Options& options, std::string& error) {
        error = validate(options);
        if (!error.empty()) return std::nullopt;
        std::optional<crash::ReportInfo> report = crash::writeUserReport(je_user_report::contentOf(options), error);
        if (report) je_user_report::noteCreated(*report);
        return report;
    }

    void createAsync(const Options& options, Done done) {
        std::string error = validate(options);
        if (!error.empty()) {
            jobs::postToMain([done = std::move(done), error]() { if (done) done(std::nullopt, error); });
            return;
        }
        const uint64_t epoch = je_user_report::createEpoch.load(std::memory_order_relaxed);
        jobs::system().ioWriteChannel().submit([content = je_user_report::contentOf(options), done = std::move(done), epoch]() mutable {
            std::string writeError;
            std::optional<crash::ReportInfo> report = crash::writeUserReport(content, writeError);
            jobs::postToMain([report = std::move(report), writeError, done = std::move(done), epoch]() {
                if (report) je_user_report::noteCreated(*report);
                // the Lua state behind done is gone after a reset (Play stop)
                if (je_user_report::createEpoch.load(std::memory_order_relaxed) != epoch || !done) return;
                done(report, writeError);
            });
        });
    }

    std::string deviceJson() {
        nlohmann::json out;
        const DeviceDetection& detection = Device::getDetection();
        const bool system = System::hasInstance();
        out["os_version"] = system ? System::instance().getOperatingSystemVersion() : std::string();
#if defined(__ANDROID__)
        out["android_api"] = system ? System::instance().getAndroidApiLevel() : 0;
#endif
        out["gpu"] = detection.profile.gpuName.empty() ? Device::getGpuName() : detection.profile.gpuName;
        // release-performance-and-gpu-quirks.md fills these; empty until it lands
        out["gpu_driver"] = "";
        out["gpu_quirks"] = nlohmann::json::array();
        out["device_tier"] = DeviceNames::tier(Device::getTier());
        out["tier_reason"] = detection.reason;
        out["memory_mb"] = detection.profile.memoryMB > 0 ? detection.profile.memoryMB : (system ? System::instance().getSystemMemorySize() : 0);
        out["cores"] = detection.profile.cores > 0 ? detection.profile.cores : (system ? System::instance().getProcessorCount() : 0);
        const float dpi = system ? System::instance().getScreenDpi() : 0.0f;
        const Platform platform = Engine::getPlatform();
        const float baseDpi = platform == Platform::Android || platform == Platform::iOS ? 160.0f : 96.0f;
        out["screen"] = {{"width", system ? System::instance().getScreenWidth() : 0},
                         {"height", system ? System::instance().getScreenHeight() : 0},
                         {"dpi_scale", dpi > 0.0f ? dpi / baseDpi : 1.0f}};
        out["thermal_state"] = DeviceNames::thermal(Device::getThermalState());
        out["low_memory_warnings"] = Device::getLowMemoryCount();
        const uint64_t startNs = crash::detail::state().startMonotonicNs;
        const uint64_t nowNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        out["uptime_s"] = startNs > 0 && nowNs > startNs ? static_cast<double>(nowNs - startNs) / 1e9 : Engine::getSystemTime();
        return out.dump(2) + "\n";
    }

    Encoded encode(std::vector<uint8_t> rgba, int width, int height, int maxWidth, size_t maxBytes) {
        Encoded out;
        if (width <= 0 || height <= 0 || rgba.size() != static_cast<size_t>(width) * height * 4) {
            out.error = "screenshot_unavailable: no frame pixels";
            return out;
        }
        int w = 0, h = 0;
        std::vector<uint8_t> rgb = je_user_report::downscale(rgba, width, height, maxWidth, w, h);
        // the full-size frame is not kept while encoding
        std::vector<uint8_t>().swap(rgba);
        for (int quality : {80, 70, 60, 50, 40}) {
            std::string jpeg = je_user_report::jpegOf(rgb, w, h, quality);
            if (!jpeg.empty() && jpeg.size() <= maxBytes) {
                out.jpeg = std::move(jpeg);
                out.width = w;
                out.height = h;
                out.quality = quality;
                return out;
            }
        }
        // still over at 40: once at half the width
        int halfWidth = 0, halfHeight = 0;
        std::vector<uint8_t> rgba2(static_cast<size_t>(w) * h * 4);
        for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; i++) {
            rgba2[i * 4] = rgb[i * 3];
            rgba2[i * 4 + 1] = rgb[i * 3 + 1];
            rgba2[i * 4 + 2] = rgb[i * 3 + 2];
            rgba2[i * 4 + 3] = 255;
        }
        const std::vector<uint8_t> half = je_user_report::downscale(rgba2, w, h, std::max(1, w / 2), halfWidth, halfHeight);
        std::string jpeg = je_user_report::jpegOf(half, halfWidth, halfHeight, 40);
        if (!jpeg.empty() && jpeg.size() <= maxBytes) {
            out.jpeg = std::move(jpeg);
            out.width = halfWidth;
            out.height = halfHeight;
            out.quality = 40;
            return out;
        }
        out.error = "screenshot_too_large";
        return out;
    }

    void setCreationIdleProbeForTesting(bool (*probe)()) {
        je_user_report::creationIdleProbe = probe;
    }

}
