// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteDiff.h"

#include "GridCodec.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace jasperengine::editor::pixelart::draw {

    namespace {

        namespace ase = aseprite;

        PixelImage imageOf(const SpriteDoc& sprite, int frame, int layer) {
            if (frame < 0 || frame >= sprite.doc.frameCount()) return PixelImage(sprite.doc.width, sprite.doc.height);
            if (layer < 0) {
                ase::RenderOptions options;
                options.keepIndices = sprite.indexed();
                return ase::renderFrame(sprite.doc, frame, options);
            }
            return celCanvas(sprite.doc, layer, frame);
        }

        bool samePalette(const ase::Document& a, const ase::Document& b) {
            return a.palette == b.palette && a.transparentIndex == b.transparentIndex;
        }

        nlohmann::json structureChanges(const ase::Document& now, const ase::Document& was) {
            nlohmann::json out = nlohmann::json::array();
            if (now.frameCount() != was.frameCount()) out.push_back("frames " + std::to_string(was.frameCount()) + " -> " + std::to_string(now.frameCount()));
            for (size_t i = 0; i < std::min(now.frameDurations.size(), was.frameDurations.size()); i++) {
                if (now.frameDurations[i] != was.frameDurations[i]) {
                    out.push_back("frame " + std::to_string(i + 1) + " duration " + std::to_string(was.frameDurations[i]) + " -> " + std::to_string(now.frameDurations[i]));
                }
            }
            auto names = [](const ase::Document& d) {
                std::vector<std::string> list;
                for (int i = 0; i < static_cast<int>(d.layers.size()); i++) list.push_back(d.layerPath(i));
                return list;
            };
            const std::vector<std::string> a = names(now), b = names(was);
            for (const std::string& n : a) {
                if (std::find(b.begin(), b.end(), n) == b.end()) out.push_back("layer added: " + n);
            }
            for (const std::string& n : b) {
                if (std::find(a.begin(), a.end(), n) == a.end()) out.push_back("layer removed: " + n);
            }
            auto tag = [](const ase::Tag& t) { return t.name + " " + std::to_string(t.from + 1) + "-" + std::to_string(t.to + 1); };
            for (const ase::Tag& t : now.tags) {
                const bool found = std::any_of(was.tags.begin(), was.tags.end(), [&](const ase::Tag& o) { return tag(o) == tag(t); });
                if (!found) out.push_back("tag set: " + tag(t));
            }
            for (const ase::Tag& t : was.tags) {
                const bool found = std::any_of(now.tags.begin(), now.tags.end(), [&](const ase::Tag& o) { return o.name == t.name; });
                if (!found) out.push_back("tag removed: " + t.name);
            }
            for (const ase::Slice& s : now.slices) {
                if (!std::any_of(was.slices.begin(), was.slices.end(), [&](const ase::Slice& o) { return o.name == s.name; })) out.push_back("slice added: " + s.name);
            }
            for (const ase::Slice& s : was.slices) {
                if (!std::any_of(now.slices.begin(), now.slices.end(), [&](const ase::Slice& o) { return o.name == s.name; })) out.push_back("slice removed: " + s.name);
            }
            if (now.mode != was.mode) out.push_back(std::string("mode ") + ase::colorModeName(was.mode) + " -> " + ase::colorModeName(now.mode));
            return out;
        }

    }

    bool diffSprites(const SpriteDoc& current, const SpriteDoc& against, const DiffOptions& options, nlohmann::json& out, DiffImages* images, PixelProblem& problem) {
        const ase::Document& now = current.doc;
        const ase::Document& was = against.doc;
        if (now.width != was.width || now.height != was.height) {
            problem = {"size_mismatch", "against", "the sprites are " + std::to_string(now.width) + "x" + std::to_string(now.height) + " and " + std::to_string(was.width) + "x" + std::to_string(was.height),
                       "diff sprites of one size (canvas resize changes it)"};
            return false;
        }
        const bool palettesSame = samePalette(now, was);
        const IntRect canvas{0, 0, now.width, now.height};
        const IntRect area = options.rect ? options.rect->intersect(canvas) : canvas;
        const std::vector<uint8_t> palette = now.paletteRgba(0);
        nlohmann::json frames = nlohmann::json::array();
        int totalChanged = 0;
        bool imagesTaken = false;
        for (int frame : options.frames) {
            const PixelImage after = imageOf(current, frame, options.layer);
            const PixelImage before = imageOf(options.againstFrame >= 0 ? current : against, options.againstFrame >= 0 ? options.againstFrame : frame, options.layer);
            const bool byIndex = palettesSame && after.hasIndices() && before.hasIndices();
            nlohmann::json cells = nlohmann::json::array();
            int changed = 0, x0 = now.width, y0 = now.height, x1 = -1, y1 = -1;
            auto differs = [&](int x, int y) { return byIndex ? after.indexAt(x, y) != before.indexAt(x, y) : after.at(x, y) != before.at(x, y); };
            for (int y = area.y; y < area.y + area.h; y++) {
                for (int x = area.x; x < area.x + area.w; x++) {
                    if (!differs(x, y)) continue;
                    changed++;
                    x0 = std::min(x0, x);
                    y0 = std::min(y0, y);
                    x1 = std::max(x1, x);
                    y1 = std::max(y1, y);
                    if (static_cast<int>(cells.size()) < options.maxCells) {
                        nlohmann::json from, to;
                        if (byIndex) {
                            from = before.indexAt(x, y);
                            to = after.indexAt(x, y);
                        } else {
                            from = hexColor(before.at(x, y));
                            to = hexColor(after.at(x, y));
                        }
                        cells.push_back({{"x", x}, {"y", y}, {"from", from}, {"to", to}});
                    }
                }
            }
            totalChanged += changed;
            nlohmann::json item = {{"frame", frame + 1}, {"changed", changed}};
            if (changed > 0) {
                const IntRect bbox{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
                item["bbox"] = {bbox.x, bbox.y, bbox.w, bbox.h};
                item["cells"] = std::move(cells);
                if (changed > options.maxCells) item["cells_truncated"] = changed - options.maxCells;
                if (options.grid) {
                    EncodedGrid grid = encodeRegion(after, bbox, GridEncoding::Auto, palette, after.hasIndices() ? after.transparentIndex() : -1);
                    const int perPixel = grid.encoding == GridEncoding::Hex2 ? 2 : grid.encoding == GridEncoding::Hex8 ? 8 : 1;
                    for (int y = 0; y < bbox.h; y++) {
                        std::string& row = grid.rows[static_cast<size_t>(y)];
                        for (int x = 0; x < bbox.w; x++) {
                            if (!differs(bbox.x + x, bbox.y + y)) row.replace(static_cast<size_t>(x) * perPixel, static_cast<size_t>(perPixel), static_cast<size_t>(perPixel), '-');
                        }
                    }
                    item["rows"] = grid.rows;
                    item["encoding"] = gridEncodingName(grid.encoding);
                    if (grid.encoding == GridEncoding::Legend) item["legend"] = grid.colors;
                }
                if (images && !imagesTaken) {
                    images->before = before;
                    images->after = after;
                    images->bbox = bbox;
                    imagesTaken = true;
                }
            }
            frames.push_back(std::move(item));
        }
        nlohmann::json structure = structureChanges(now, was);
        const bool same = totalChanged == 0 && palettesSame && structure.empty();
        out = {{"same", same}, {"changed", totalChanged}, {"palette_same", palettesSame}, {"structure_changes", std::move(structure)}, {"frames", std::move(frames)}};
        return true;
    }

}
