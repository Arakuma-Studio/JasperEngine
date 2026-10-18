// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The editor's video import (docs/html-ui/phase-6-video.md §3.12.1): a video the engine plays (WebM
// with AV1 video and Opus audio) is copied as it is; any other one (MP4/H.264, MOV, MKV, VP8/VP9
// WebM, Ogg/Theora, AVI, ...) is converted to that format with a user-installed FFmpeg, run as a
// separate program (never linked, never shipped). Without FFmpeg, or without an AV1 encoder in it,
// the original is copied and the result says what to run (HTMLUI-IMP-0010).

#include "ContentSniffer.h"
#include "RemoteImporter.h"

#include "htmlui/compat/Diagnostics.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor::htmlui {

    struct VideoImportResult {
        bool ok = false;                 // the file is in the project in a format the engine plays (or was copied as asked)
        bool converted = false;          // FFmpeg wrote the .webm
        bool copied = false;             // the input was copied unchanged (playable, or no converter)
        std::filesystem::path output;    // absolute path of the file written
        std::string command;             // the FFmpeg command line (run, or to run)
        std::string ffmpeg;              // the FFmpeg used ("" = none found)
        std::string encoder;             // libsvtav1, libaom-av1 or librav1e
        std::string message;             // for the report
        std::vector<jasperengine::htmlui::compat::Diagnostic> diagnostics;   // HTMLUI-IMP-0010 / 0011
        double seconds = 0.0;            // the conversion's duration
    };

    class VideoImporter {
    public:
        // a file a video importer must look at: any video extension (.webm, .mkv, .mp4, .mov, ...)
        static bool isVideoFile(const std::filesystem::path& path);
        // the engine plays it as it is (the probe: AV1/Opus WebM or Matroska)
        static bool isPlayable(const std::filesystem::path& path);

        // FFmpeg: JASPER_FFMPEG, then the editor's override (setFfmpegPath: Preferences > External
        // Tools > FFmpeg), then PATH; "" = none. why says where it looked when none is found.
        // overridePath: look up with that override instead of the set one (the Preferences
        // window's status of a path being edited)
        static std::string findFfmpeg(std::string* why = nullptr, const std::string* overridePath = nullptr);
        static void setFfmpegPath(const std::string& path);
        // its AV1 encoders, in the order they are preferred (libsvtav1, libaom-av1, librav1e)
        static std::vector<std::string> av1Encoders(const std::string& ffmpeg);

        // The command that converts `input` into `output` (AV1 + Opus WebM, keyframes every 2 s,
        // 8-bit 4:2:0 (10-bit kept), at most 4096 px, stereo Opus 128 kb/s, the first video and
        // audio streams)
        static std::string conversionCommand(const std::string& ffmpeg, const std::string& encoder, const std::filesystem::path& input,
                                             const std::filesystem::path& output, bool tenBit);

        // Imports one file into `targetDir` (absolute): copied when the engine plays it, else
        // converted to <stem>.webm (the original is not copied), else copied with HTMLUI-IMP-0010.
        // Blocks while FFmpeg runs (call it off the main thread); onLine gets FFmpeg's output.
        static VideoImportResult importFile(const std::filesystem::path& input, const std::filesystem::path& targetDir, bool overwrite,
                                            const std::function<void(const std::string&)>& onLine = {});
    };

    // A downloaded video (RemoteImporter: import_files URLs and a document's remote <video src>,
    // <source src>): saved as videos/<host>/<sha1-12>-<basename>.webm under the import root when it
    // plays as it is or FFmpeg converts it (the manifest maps the URL to that file, kind "video");
    // without FFmpeg the original is saved with its own extension and HTMLUI-IMP-0010, a failed
    // conversion saves nothing (HTMLUI-IMP-0011). License unknown: HTMLUI-IMP-0008.
    class RemoteVideoImporter {
    public:
        explicit RemoteVideoImporter(RemoteImporter& importer);

        ImportedResource run(const std::string& url, const std::string& body, const SniffedContent& sniffed, const ImportLocation& at);

    private:
        RemoteImporter& importer;
    };

}
