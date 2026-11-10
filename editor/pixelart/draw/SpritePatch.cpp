// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpritePatch.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace jasperengine::editor::pixelart::draw {

    namespace {

        namespace ase = aseprite;

        bool isBackground(const ase::Document& doc, int layer) {
            return layer >= 0 && layer < static_cast<int>(doc.layers.size()) && doc.layers[static_cast<size_t>(layer)].background();
        }

        PixelImage blank(const ase::Document& doc, int layer, int w, int h) {
            if (doc.mode != ase::ColorMode::Indexed) return PixelImage(w, h);
            return PixelImage::fromIndices(w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h, doc.transparentIndex), doc.paletteRgba(0),
                                           isBackground(doc, layer) ? -1 : doc.transparentIndex);
        }

        PixelImage regionOf(const ase::Document& doc, int layer, const ase::Cel* cel, const IntRect& rect) {
            PixelImage out = blank(doc, layer, rect.w, rect.h);
            if (cel && cel->image) out.blit(*cel->image, cel->x - rect.x, cel->y - rect.y);
            return out;
        }

        IntRect boundsOf(const ase::Cel* cel) {
            return cel && cel->image ? IntRect{cel->x, cel->y, cel->image->width(), cel->image->height()} : IntRect{};
        }

        void appendSide(std::vector<uint8_t>& data, const PixelImage& image, bool indexed) {
            if (indexed) data.insert(data.end(), image.indices().begin(), image.indices().end());
            else data.insert(data.end(), image.pixels().begin(), image.pixels().end());
        }

        PixelImage sideImage(const ase::Document& doc, const CelPatch& patch, bool forward) {
            const size_t count = static_cast<size_t>(patch.rect.w) * patch.rect.h;
            const size_t perSide = patch.indexed ? count : count * 4;
            const uint8_t* from = patch.data.data() + (forward ? perSide : 0);
            if (patch.indexed) {
                return PixelImage::fromIndices(patch.rect.w, patch.rect.h, std::vector<uint8_t>(from, from + perSide), doc.paletteRgba(0),
                                               isBackground(doc, patch.layer) ? -1 : doc.transparentIndex);
            }
            PixelImage image(patch.rect.w, patch.rect.h);
            std::memcpy(image.editPixels().data(), from, perSide);
            return image;
        }

        void eraseCel(ase::Document& doc, int layer, int frame) {
            doc.cels.erase(std::remove_if(doc.cels.begin(), doc.cels.end(), [&](const ase::Cel& c) { return c.layer == layer && c.frame == frame; }), doc.cels.end());
        }

        // the image cels of a document, by (layer, frame)
        std::map<std::pair<int, int>, const ase::Cel*> imageCels(const ase::Document& doc) {
            std::map<std::pair<int, int>, const ase::Cel*> out;
            for (const ase::Cel& cel : doc.cels) {
                if (cel.linkedFrame < 0 && cel.image) out[{cel.layer, cel.frame}] = &cel;
            }
            return out;
        }

        size_t documentBytes(const ase::Document& doc) {
            return sizeof(ase::Document) + doc.layers.size() * sizeof(ase::Layer) + doc.cels.size() * sizeof(ase::Cel) + doc.tags.size() * sizeof(ase::Tag) +
                   doc.slices.size() * sizeof(ase::Slice) + doc.palette.size() * sizeof(ase::PaletteEntry) + doc.frameDurations.size() * sizeof(uint16_t);
        }

        size_t imageBytes(const PixelImage& image) {
            return image.pixels().size() + image.indices().size();
        }

    }

    size_t SpritePatchSet::memoryBytes() const {
        size_t total = beforeFile ? beforeFile->size() : 0;
        for (const CelPatch& patch : cels) total += sizeof(CelPatch) + patch.data.capacity();
        if (structure) {
            total += documentBytes(structure->before) + documentBytes(structure->after);
            std::set<const void*> after;
            for (const ase::Cel& cel : structure->after.cels) after.insert(cel.image.get());
            // what is on one side only is what the patch keeps alive; the rest the sprite has too
            std::set<const void*> seen;
            for (const ase::Cel& cel : structure->before.cels) {
                if (cel.image && !after.count(cel.image.get()) && seen.insert(cel.image.get()).second) total += imageBytes(*cel.image);
            }
        }
        return total;
    }

    std::shared_ptr<SpritePatchSet> makePatch(const SpriteDoc& before, const SpriteDoc& after, bool structureChanged) {
        auto patch = std::make_shared<SpritePatchSet>();
        if (structureChanged) {
            patch->structure = std::make_unique<StructurePatch>();
            patch->structure->before = before.doc;
            patch->structure->after = after.doc;
            patch->structure->before.provenance.reset();
            patch->structure->after.provenance.reset();
            return patch;
        }
        const std::map<std::pair<int, int>, const ase::Cel*> a = imageCels(before.doc), b = imageCels(after.doc);
        std::set<std::pair<int, int>> keys;
        for (const auto& [key, cel] : a) keys.insert(key);
        for (const auto& [key, cel] : b) keys.insert(key);
        const ase::Document& doc = after.doc;
        for (const std::pair<int, int>& key : keys) {
            const auto ia = a.find(key);
            const auto ib = b.find(key);
            const ase::Cel* ca = ia == a.end() ? nullptr : ia->second;
            const ase::Cel* cb = ib == b.end() ? nullptr : ib->second;
            if (ca && cb && ca->image == cb->image && ca->x == cb->x && ca->y == cb->y) continue;
            const IntRect ra = boundsOf(ca), rb = boundsOf(cb), all = ra.unite(rb);
            if (all.empty()) continue;
            const PixelImage ia2 = regionOf(doc, key.first, ca, all);
            const PixelImage ib2 = regionOf(doc, key.first, cb, all);
            const bool indexed = ia2.hasIndices() && ib2.hasIndices();
            int x0 = all.w, y0 = all.h, x1 = -1, y1 = -1;
            for (int y = 0; y < all.h; y++) {
                for (int x = 0; x < all.w; x++) {
                    const bool differs = indexed ? ia2.indexAt(x, y) != ib2.indexAt(x, y) : std::memcmp(ia2.row(y) + x * 4, ib2.row(y) + x * 4, 4) != 0;
                    if (!differs) continue;
                    x0 = std::min(x0, x);
                    y0 = std::min(y0, y);
                    x1 = std::max(x1, x);
                    y1 = std::max(y1, y);
                }
            }
            if (x1 < 0 && ra == rb && (ca != nullptr) == (cb != nullptr)) continue;
            CelPatch cel;
            cel.layer = key.first;
            cel.frame = key.second;
            cel.celBefore = ra;
            cel.celAfter = rb;
            cel.existedBefore = ca != nullptr;
            cel.existedAfter = cb != nullptr;
            cel.indexed = indexed;
            if (x1 >= 0) {
                const IntRect local{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
                cel.rect = {all.x + local.x, all.y + local.y, local.w, local.h};
                appendSide(cel.data, ia2.crop(local), indexed);
                appendSide(cel.data, ib2.crop(local), indexed);
                cel.data.shrink_to_fit();
            }
            patch->cels.push_back(std::move(cel));
        }
        return patch;
    }

    void applyPatch(SpriteDoc& sprite, const SpritePatchSet& patch, bool forward) {
        ase::Document& doc = sprite.doc;
        if (patch.structure) {
            std::shared_ptr<const ase::Provenance> provenance = doc.provenance;
            doc = forward ? patch.structure->after : patch.structure->before;
            doc.provenance = std::move(provenance);
            return;
        }
        for (const CelPatch& cel : patch.cels) {
            const bool exists = forward ? cel.existedAfter : cel.existedBefore;
            if (!exists) {
                eraseCel(doc, cel.layer, cel.frame);
                continue;
            }
            const IntRect bounds = forward ? cel.celAfter : cel.celBefore;
            PixelImage image = blank(doc, cel.layer, bounds.w, bounds.h);
            ase::Cel* record = doc.celRecord(cel.layer, cel.frame);
            if (record && record->linkedFrame >= 0) record = nullptr;
            if (record && record->image) image.blit(*record->image, record->x - bounds.x, record->y - bounds.y);
            if (!cel.rect.empty()) image.blit(sideImage(doc, cel, forward), cel.rect.x - bounds.x, cel.rect.y - bounds.y);
            if (!record) {
                ase::Cel made;
                made.layer = cel.layer;
                made.frame = cel.frame;
                doc.cels.push_back(std::move(made));
                doc.sortCels();
                record = doc.celRecord(cel.layer, cel.frame);
            }
            record->x = bounds.x;
            record->y = bounds.y;
            record->image = std::make_shared<const PixelImage>(std::move(image));
        }
    }

    bool patchSurvives(const SpriteDoc& sprite, const SpritePatchSet& patch) {
        const ase::Document& doc = sprite.doc;
        if (patch.structure) {
            const ase::Document& after = patch.structure->after;
            return doc.width == after.width && doc.height == after.height && doc.frameCount() == after.frameCount() && doc.layers.size() == after.layers.size();
        }
        for (const CelPatch& cel : patch.cels) {
            if (cel.rect.empty()) continue;
            if (cel.layer >= static_cast<int>(doc.layers.size()) || cel.frame >= doc.frameCount()) return false;
            const ase::Cel* current = doc.cel(cel.layer, cel.frame);
            const PixelImage now = regionOf(doc, cel.layer, current, cel.rect);
            const PixelImage want = sideImage(doc, cel, true);
            if (now.hasIndices() && want.hasIndices() ? !std::equal(now.indices().begin(), now.indices().end(), want.indices().begin(), want.indices().end())
                                                      : !std::equal(now.pixels().begin(), now.pixels().end(), want.pixels().begin(), want.pixels().end())) {
                return false;
            }
        }
        return true;
    }

}
