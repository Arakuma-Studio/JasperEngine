// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef DIAG_USERREPORT_H
#define DIAG_USERREPORT_H

// A player's "Report a Problem" (docs/crash-diagnostics.md, Lua Crash.beginReport and
// Crash.createReport): a capture of the frame being drawn, encoded to a JPEG draft off the main
// thread, then a <session>-user<N> report folder with the note, the device, the Gameplay
// Journal, the log tail and the screenshot (CrashHandler.h writes it). The engine never sends it:
// the game uploads the files with its own Http and marks the report handled.
//
// Only a pending capture draws a frame offscreen (Engine::systemDraw asks wantsCapture()).

#include "Export.h"
#include "diag/CrashHandler.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::diag::userreport {

    constexpr int kDefaultMaxWidth = 1280;
    constexpr size_t kDefaultMaxBytes = 600u << 10;
    // frames a capture may wait for shaders and pipelines being made (S32) before it fails
    constexpr int kMaxCaptureFrames = 30;
    constexpr size_t kMaxNoteBytes = 4000;
    constexpr size_t kMaxFields = 16;

    // The frame just drawn as RGBA8, rows top first (Engine::readUserReportFrame)
    using FrameReader = bool (*)(std::vector<uint8_t>& rgba, int& width, int& height, std::string& error);
    // ok: the draft holds a screenshot; else error says why ("screenshot_unavailable: ...")
    using Callback = std::function<void(bool ok, const std::string& error)>;

    // Requests a capture of the frame being drawn; fn runs on the main thread on the first frame
    // after it was encoded or failed. While one is pending, a new call replaces fn and keeps it.
    JASPERENGINE_API void begin(Callback fn, int maxWidth = kDefaultMaxWidth, size_t maxBytes = kDefaultMaxBytes);
    // Drops the draft (the form's Cancel)
    JASPERENGINE_API void cancel();
    // Drops the draft and any callback (Play stop: the Lua state behind the callbacks goes)
    JASPERENGINE_API void reset();

    // ---- the Engine's frame (main thread)

    // This frame must draw offscreen and be read after it
    JASPERENGINE_API bool wantsCapture();
    // Before the capture frame draws: whether a stand-in may be drawn in it
    JASPERENGINE_API void beforeCaptureFrame();
    // After the capture frame drew (read it, or retry next frame while shaders are being made)
    JASPERENGINE_API void frameDrawn(FrameReader reader);
    // Every frame: the callbacks, and the web's encode (the frame after the read)
    JASPERENGINE_API void update();

    JASPERENGINE_API bool hasDraft();
    JASPERENGINE_API bool draftHasScreenshot();

    // ---- the report

    struct Options {
        std::string note;                                          // required, at most 4000 bytes
        std::string category = "other";                            // [a-z0-9_]{1,32}
        std::vector<std::pair<std::string, std::string>> fields;   // at most 16, values as text
        bool screenshot = true;                                    // the draft's JPEG when there is one
        std::optional<bool> logTail;                               // default: Include Log Tail
    };
    // Writes the report now (main thread); nullopt with "invalid_note: ...", "too_many_pending: ..."
    JASPERENGINE_API std::optional<crash::ReportInfo> create(const Options& options, std::string& error);
    using Done = std::function<void(const std::optional<crash::ReportInfo>& report, const std::string& error)>;
    // The same, its files written on the I/O job channel; done runs on the main thread after
    JASPERENGINE_API void createAsync(const Options& options, Done done);
    // An empty string when the options are fine
    JASPERENGINE_API std::string validate(const Options& options);

    // device.json's fields from Device, System and the engine (the crash handler adds the
    // product, version, build id, platform, backend and session)
    JASPERENGINE_API std::string deviceJson();

    // ---- the encode (tests, benches)

    struct Encoded {
        std::string jpeg;
        int width = 0;
        int height = 0;
        int quality = 0;
        std::string error;      // "screenshot_too_large" when nothing fits maxBytes
    };
    // Box-downscales to at most maxWidth, then JPEG at quality 80, 70, 60, 50, 40 until it fits
    // maxBytes; then once at half the width and quality 40
    JASPERENGINE_API Encoded encode(std::vector<uint8_t> rgba, int width, int height, int maxWidth, size_t maxBytes);

    // Test seam: what says whether shaders, pipelines or textures are still being made
    JASPERENGINE_API void setCreationIdleProbeForTesting(bool (*probe)());

}

#endif // DIAG_USERREPORT_H
