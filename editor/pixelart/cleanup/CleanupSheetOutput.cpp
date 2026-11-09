// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/spritesheet.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

#include "CleanupSheetOutput.h"

#include "CleanupCommon.h"
#include "CleanupRecipe.h"
#include "CleanupSample.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>

namespace jasperengine::editor::pixelart::cleanup {

    using nlohmann::ordered_json;

    namespace {

        // spritesheet.ts `round`: 4 decimals
        double atlasRound(double value) { return jsRound(value * 10000) / 10000; }

        int atlasClipDuration(const AtlasClip& clip, size_t frame) {
            if (frame < clip.durationsMs.size() && clip.durationsMs[frame] > 0) return clip.durationsMs[frame];
            return (int)jsRound(1000 / std::max(0.1, clip.fps));
        }

    }

    Atlas atlasOf(const std::vector<AtlasClip>& clips, const SheetLayout& layout) {
        const int spacing = std::max(0, layout.spacing);
        Atlas atlas;
        int top = 0;
        int width = 0;
        for (int c = 0; c < (int)clips.size(); c++) {
            const AtlasClip& clip = clips[c];
            if (clip.frames.empty()) continue;
            const PixelArt& first = clip.frames[0];
            const int count = (int)clip.frames.size();
            const int columns = std::max(1, std::min(count, layout.columns));
            for (int f = 0; f < count; f++) {
                atlas.rects.push_back({c, f, (f % columns) * (first.width + spacing), top + (f / columns) * (first.height + spacing), first.width, first.height});
            }
            const int rows = (count + columns - 1) / columns;
            width = std::max(width, columns * first.width + (columns - 1) * spacing);
            top += rows * first.height + (rows - 1) * spacing + spacing;
        }
        const int height = std::max(0, top - spacing);
        for (const AtlasClip& clip : clips) {
            if (!clip.frames.empty()) {
                atlas.art.palette = clip.frames[0].palette;
                break;
            }
        }
        atlas.art.width = width;
        atlas.art.height = height;
        atlas.art.indices.assign(static_cast<size_t>(width) * height, TRANSPARENT);
        for (const AtlasRect& rect : atlas.rects) {
            const PixelArt& art = clips[rect.clip].frames[rect.frame];
            for (int y = 0; y < art.height; y++) {
                std::copy_n(art.indices.begin() + static_cast<ptrdiff_t>(y) * art.width, art.width,
                            atlas.art.indices.begin() + static_cast<ptrdiff_t>(rect.y + y) * width + rect.x);
            }
        }
        return atlas;
    }

    std::string spritesheetJson(const SpritesheetInfo& info) {
        const int scale = std::max(1, info.scale);
        ordered_json frames = ordered_json::object();
        ordered_json animations = ordered_json::object();
        ordered_json frameTags = ordered_json::array();
        ordered_json fps = ordered_json::object();
        int written = 0;
        for (int c = 0; c < (int)info.clips->size(); c++) {
            const AtlasClip& clip = (*info.clips)[c];
            fps[clip.name] = clip.fps;
            std::vector<const AtlasRect*> rects;
            for (const AtlasRect& rect : info.atlas->rects) {
                if (rect.clip == c) rects.push_back(&rect);
            }
            if (rects.empty()) continue;
            const ordered_json anchor = {{"x", atlasRound((clip.anchor.x + 0.5) / rects[0]->w)}, {"y", atlasRound((clip.anchor.y + 1.0) / rects[0]->h)}};
            ordered_json names = ordered_json::array();
            for (const AtlasRect* rect : rects) {
                const std::string name = clip.name + "-" + std::to_string(rect->frame);
                const int w = rect->w * scale, h = rect->h * scale;
                frames[name] = {{"frame", {{"x", rect->x * scale}, {"y", rect->y * scale}, {"w", w}, {"h", h}}},
                                {"rotated", false},
                                {"trimmed", false},
                                {"spriteSourceSize", {{"x", 0}, {"y", 0}, {"w", w}, {"h", h}}},
                                {"sourceSize", {{"w", w}, {"h", h}}},
                                {"duration", atlasClipDuration(clip, rect->frame)},
                                {"anchor", anchor}};
                names.push_back(name);
            }
            frameTags.push_back({{"name", clip.name}, {"from", written}, {"to", written + (int)names.size() - 1}, {"direction", loopName(clip.loop)}});
            written += (int)names.size();
            animations[clip.name] = std::move(names);
        }
        ordered_json meta = {{"app", "jasperengine"},
                             {"version", "1"},
                             {"image", info.image},
                             {"format", "RGBA8888"},
                             {"size", {{"w", info.atlas->art.width * scale}, {"h", info.atlas->art.height * scale}}},
                             {"scale", "1"},
                             {"frameTags", std::move(frameTags)},
                             {"fps", std::move(fps)}};
        if (!info.recipe.is_null()) meta["jasperengine"] = {{"recipe", ordered_json::parse(info.recipe.dump())}};
        const ordered_json out = {{"frames", std::move(frames)}, {"animations", std::move(animations)}, {"meta", std::move(meta)}};
        return out.dump(2) + "\n";
    }

    Atlas pngSheet(const std::vector<AtlasClip>& clips) { return atlasOf(clips, {INT_MAX, 1}); }

    aseprite::Document asepriteDocument(const std::vector<AtlasClip>& clips) {
        aseprite::Document doc;
        doc.width = doc.height = 1;
        const std::vector<Rgba8>* palette = nullptr;
        for (const AtlasClip& clip : clips) {
            for (const PixelArt& art : clip.frames) {
                doc.width = std::max(doc.width, art.width);
                doc.height = std::max(doc.height, art.height);
                if (!palette) palette = &art.palette;
            }
        }
        const bool indexed = palette && palette->size() <= 255;
        doc.mode = indexed ? aseprite::ColorMode::Indexed : aseprite::ColorMode::Rgba;
        doc.transparentIndex = 0;
        if (indexed) {
            doc.palette.push_back(aseprite::PaletteEntry{{0, 0, 0, 255}, std::nullopt});
            for (const Rgba8 c : *palette) doc.palette.push_back(aseprite::PaletteEntry{{c.r, c.g, c.b, 255}, std::nullopt});
        }
        aseprite::Layer layer;
        layer.name = "Cleanup";
        doc.layers = {layer};
        const std::vector<uint8_t> paletteRgba = doc.paletteRgba();
        for (const AtlasClip& clip : clips) {
            if (clip.frames.empty()) continue;
            const int from = doc.frameCount();
            for (size_t f = 0; f < clip.frames.size(); f++) {
                const PixelArt& art = clip.frames[f];
                aseprite::Cel cel;
                cel.layer = 0;
                cel.frame = doc.frameCount();
                if (indexed) {
                    std::vector<uint8_t> plane(art.indices.size());
                    for (size_t i = 0; i < plane.size(); i++) plane[i] = art.indices[i] == TRANSPARENT ? 0 : static_cast<uint8_t>(art.indices[i] + 1);
                    cel.image = std::make_shared<const PixelImage>(PixelImage::fromIndices(art.width, art.height, std::move(plane), paletteRgba, 0));
                } else {
                    cel.image = std::make_shared<const PixelImage>(toRgba(art));
                }
                doc.cels.push_back(std::move(cel));
                doc.frameDurations.push_back(static_cast<uint16_t>(std::clamp(atlasClipDuration(clip, f), 1, 65535)));
            }
            if (!clip.name.empty()) {
                aseprite::Tag tag;
                tag.from = from;
                tag.to = doc.frameCount() - 1;
                tag.direction = clip.loop == Loop::PingPong ? aseprite::TagDirection::PingPong : aseprite::TagDirection::Forward;
                tag.name = clip.name;
                doc.tags.push_back(std::move(tag));
            }
        }
        // a file has at least one frame, even with no animation to export
        if (doc.frameDurations.empty()) doc.frameDurations.push_back(100);
        doc.rebuildTreeFromLevels();
        return doc;
    }

}
