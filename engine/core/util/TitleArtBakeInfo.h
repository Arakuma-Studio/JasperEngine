// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TITLE_ART_BAKE_INFO_H
#define TITLE_ART_BAKE_INFO_H

// Contract (docs/specs/S81-title-art-runtime.md R8, R36; S80 §6.9)
// - Purpose: the runtime's reader of a bake sidecar, `logo.titleart.bake.json` (format
//   `jasperengine.titleart.bakeinfo`, version 1), which S80's bake writes beside its PNGs. A
//   Baked title reads only this file and the PNGs, so it lives in core: no `titleart` module.
// - Lenient like S80's codec: a member of the wrong type takes its default, unknown members are
//   ignored; only text that is not a sidecar, or a newer version, fails (with a sentence).
// - Coordinates: `frame` and `alphaBounds` are canvas pixels, image space (y down); `origin` is
//   the document origin in frame pixels; anchors are frame-normalised (0..1, y down).
// - Thread-safety: plain values.

#include "Export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine{

    constexpr const char* TITLE_ART_BAKEINFO_FORMAT_NAME = "jasperengine.titleart.bakeinfo";
    constexpr int TITLE_ART_BAKEINFO_READ_VERSION = 1;
    constexpr const char* TITLE_ART_BAKEINFO_SUFFIX = ".bake.json";
    // R36: the live glints draw at most this many anchors
    constexpr size_t TITLE_ART_MAX_ANCHORS = 16;

    struct TitleArtRectI{
        int x = 0, y = 0, width = 0, height = 0;
        bool empty() const{ return width <= 0 || height <= 0; }
    };

    struct TitleArtBakeOutput{
        std::string id;
        std::string file;           // relative to the document's directory
        std::string content;        // all, layers, ink, glow, mask
        int width = 0, height = 0;
    };

    // a highlight anchor (S83 R13-R14): frame-normalised position and size, strength and fade 0..1
    struct TitleArtAnchor{
        float x = 0.0f, y = 0.0f;
        float size = 0.0f;
        float strength = 0.0f;
        float fade = 1.0f;
        bool operator==(const TitleArtAnchor& o) const{
            return x == o.x && y == o.y && size == o.size && strength == o.strength && fade == o.fade;
        }
    };

    struct JASPERENGINE_API TitleArtBakeInfo{
        TitleArtRectI frame;
        double originX = 0.0, originY = 0.0;
        std::vector<TitleArtBakeOutput> outputs;
        std::vector<TitleArtAnchor> anchors;    // at most TITLE_ART_MAX_ANCHORS, strongest first
        TitleArtRectI alphaBounds;
        std::string sourceHash;
        uint32_t bakeVersion = 0;
        std::string language;                   // S87: the language a language set's sidecar was baked in

        // false with `error` (one sentence) for text that is not a sidecar or a newer version
        static bool parse(const std::string& text, TitleArtBakeInfo& out, std::string& error);
        // reads `path` (a project-relative or virtual path the engine opens); false with `error`
        static bool load(const std::string& path, TitleArtBakeInfo& out, std::string& error);

        // the first output with that content, null when none ("all" for the main image, "glow")
        const TitleArtBakeOutput* firstOutput(const std::string& content) const;
        const TitleArtBakeOutput* output(const std::string& id) const;
        // where the document origin falls in the frame, 0..1 from the top-left (RectTransform's
        // pivot convention: (0, 0) top-left, (1, 1) bottom-right)
        float pivotX() const;
        float pivotY() const;
    };

    // "assets/titles/logo.titleart" -> "assets/titles/logo.titleart.bake.json"
    JASPERENGINE_API std::string titleArtSidecarPath(const std::string& titlePath);
    // a sidecar output's file as a path beside the document ("assets/titles/" + file)
    JASPERENGINE_API std::string titleArtSiblingPath(const std::string& titlePath, const std::string& file);
    // R11: the image a build prebakes for one initial state of a Realtime title, beside the
    // document ("assets/titles/logo.titleart.prebake-0123456789abcdef.png")
    JASPERENGINE_API std::string titleArtPrebakePath(const std::string& titlePath, uint64_t stateHash);
    // the main bake's default file name when the sidecar is missing ("logo.titleart.png")
    JASPERENGINE_API std::string titleArtDefaultBakeFile(const std::string& titlePath);

}

#endif //TITLE_ART_BAKE_INFO_H
