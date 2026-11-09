// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// edit_sprite's structure family (docs/specs/S47-agent-sprite-drawing.md R24, R25): layers, frames,
// tags, slices, cels, the palette and the canvas. applyOps has flushed the pixel canvases before
// any of these runs, so they work on the document's cels; indices already recorded in the batch's
// changes are re-addressed through remapRecorded.

#include "SpriteEditInternal.h"

#include "pixelart/PixelColor.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace jasperengine::editor::pixelart::draw::detail {

    namespace {

        PixelRgba hexValue(const Json& value, const std::string& field) {
            jasperengine::pixelart::Rgba8 c;
            if (value.is_string()) {
                const std::string text = value.get<std::string>();
                if (text == "transparent") return {0, 0, 0, 0};
                if (!text.empty() && text[0] == '#' && (text.size() == 7 || text.size() == 9) && jasperengine::pixelart::parseHex(text, c)) {
                    return {c.r, c.g, c.b, c.a};
                }
            }
            fail("bad_color", field, field + " " + value.dump() + " is not a colour", "here a colour is \"#rrggbb\" or \"#rrggbbaa\"");
        }

        std::string requiredString(const Json& op, const char* name, const std::string& what) {
            if (!op.contains(name) || !op[name].is_string() || op[name].get<std::string>().empty()) {
                fail("bad_op_field", name, what + " needs " + name, "");
            }
            return op[name].get<std::string>();
        }

        // ------------------------------------------------------------------ layers

        struct TreeNode {
            int layer = -1;
            std::vector<TreeNode> children;
        };

        TreeNode buildTree(const ase::Document& doc, int group) {
            TreeNode node;
            node.layer = group;
            for (int child : doc.children(group)) node.children.push_back(buildTree(doc, child));
            return node;
        }

        void flatten(const TreeNode& node, int parentNew, std::vector<int>& order, std::vector<int>& parents) {
            for (const TreeNode& child : node.children) {
                const int at = static_cast<int>(order.size());
                order.push_back(child.layer);
                parents.push_back(parentNew);
                flatten(child, at, order, parents);
            }
        }

        TreeNode* findNode(TreeNode& node, int layer) {
            if (node.layer == layer) return &node;
            for (TreeNode& child : node.children) {
                if (TreeNode* found = findNode(child, layer)) return found;
            }
            return nullptr;
        }

        bool detach(TreeNode& node, int layer, TreeNode& out) {
            for (size_t i = 0; i < node.children.size(); i++) {
                if (node.children[i].layer == layer) {
                    out = std::move(node.children[i]);
                    node.children.erase(node.children.begin() + static_cast<std::ptrdiff_t>(i));
                    return true;
                }
                if (detach(node.children[i], layer, out)) return true;
            }
            return false;
        }

        void collect(const TreeNode& node, std::vector<int>& out) {
            out.push_back(node.layer);
            for (const TreeNode& c : node.children) collect(c, out);
        }

        // the layers become the tree's order (each node's layer index is the old one; -1 entries
        // are appended by the caller first); cels and the batch's records follow
        void applyTree(EditState& s, const TreeNode& root) {
            ase::Document& doc = s.doc;
            std::vector<int> order, parents;
            flatten(root, -1, order, parents);
            std::vector<int> newOfOld(doc.layers.size(), -1);
            std::vector<ase::Layer> layers;
            layers.reserve(order.size());
            for (size_t i = 0; i < order.size(); i++) {
                newOfOld[static_cast<size_t>(order[i])] = static_cast<int>(i);
                layers.push_back(std::move(doc.layers[static_cast<size_t>(order[i])]));
                layers.back().parent = parents[i];
            }
            doc.layers = std::move(layers);
            doc.rebuildLevelsFromTree();
            std::vector<ase::Cel> cels;
            cels.reserve(doc.cels.size());
            for (ase::Cel& cel : doc.cels) {
                const int mapped = newOfOld[static_cast<size_t>(cel.layer)];
                if (mapped < 0) continue;
                cel.layer = mapped;
                cels.push_back(std::move(cel));
            }
            doc.cels = std::move(cels);
            doc.sortCels();
            s.remapRecorded([&](DirtyRect& d) {
                const int mapped = newOfOld[static_cast<size_t>(d.layer)];
                if (mapped < 0) return false;
                d.layer = mapped;
                return true;
            });
        }

        int targetLayer(EditState& s, const Json& op) {
            if (op.contains("name")) return s.parseLayer(op["name"], "name", false);
            if (op.contains("index")) return s.parseLayer(op["index"], "index", false);
            fail("bad_op_field", "name", "layer " + op.value("action", std::string()) + " needs name or index", s.layerList());
        }

        // the group a layer goes in: -1 for "" (the top level)
        int groupOf(EditState& s, const std::string& path) {
            if (path.empty()) return -1;
            const int g = s.doc.findLayer(path);
            if (g < 0 || !s.doc.layers[static_cast<size_t>(g)].isGroup()) {
                fail("no_layer", "group", "the sprite has no group \"" + path + "\"", s.layerList());
            }
            return g;
        }

        void placeInGroup(TreeNode& root, TreeNode node, int group, int at) {
            TreeNode* parent = group < 0 ? &root : findNode(root, group);
            const int count = static_cast<int>(parent->children.size());
            const int position = at < 0 ? count : std::min(at, count);
            parent->children.insert(parent->children.begin() + position, std::move(node));
        }

        void refuseBackgroundOrder(EditState& s, const TreeNode& root) {
            // Aseprite keeps the background the bottom layer
            std::vector<int> order, parents;
            flatten(root, -1, order, parents);
            for (size_t i = 1; i < order.size(); i++) {
                if (s.doc.layers[static_cast<size_t>(order[i])].background()) {
                    fail("bad_op_field", "at", s.doc.layers[static_cast<size_t>(order[i])].name + " is the background layer, which stays at the bottom",
                         "use at: 1 or above, or make it a normal layer in Aseprite first");
                }
            }
        }

        // a deterministic UUID, for files whose header says layers have them
        ase::Uuid layerUuid(const std::string& name, size_t count) {
            ase::Uuid uuid;
            uint64_t h = 0xcbf29ce484222325ull;
            auto mix = [&](uint8_t b) { h = (h ^ b) * 0x100000001b3ull; };
            for (char c : name) mix(static_cast<uint8_t>(c));
            for (int i = 0; i < 8; i++) mix(static_cast<uint8_t>(count >> (i * 8)));
            for (int i = 0; i < 16; i++) {
                if (i % 8 == 0) mix(0x5a);
                uuid.bytes[static_cast<size_t>(i)] = static_cast<uint8_t>(h >> ((i % 8) * 8));
            }
            uuid.bytes[6] = static_cast<uint8_t>((uuid.bytes[6] & 0x0f) | 0x40);
            uuid.bytes[8] = static_cast<uint8_t>((uuid.bytes[8] & 0x3f) | 0x80);
            return uuid;
        }

        void layerOp(EditState& s, const Json& op) {
            ase::Document& doc = s.doc;
            const std::string action = enumField(op, "action", "", {"add", "remove", "rename", "move", "set"});
            if (action.empty()) fail("bad_op_field", "action", "layer needs action", "action is one of add, remove, rename, move, set");
            if (action == "add") {
                if (doc.layers.size() >= static_cast<size_t>(kMaxLayers)) {
                    fail("too_many_ops", "layer", "the sprite has 256 layers, the most an edit keeps", "remove a layer first (at most 256 layers)");
                }
                std::string name = op.contains("name") && op["name"].is_string() ? op["name"].get<std::string>() : "Layer " + std::to_string(doc.layers.size() + 1);
                ase::Layer layer;
                layer.name = name;
                if (doc.headerFlags & ase::kHeaderLayerUuids) layer.uuid = layerUuid(name, doc.layers.size());
                if (op.contains("opacity")) layer.opacity = static_cast<uint8_t>(intField(op, "opacity", 255, 0, 255));
                if (op.contains("visible") && !op["visible"].get<bool>()) layer.flags &= static_cast<uint16_t>(~ase::kLayerVisible);
                if (op.contains("blend_mode")) ase::parseBlendMode(op["blend_mode"].get<std::string>(), layer.blend);
                TreeNode root = buildTree(doc, -1);
                doc.layers.push_back(std::move(layer));
                TreeNode node;
                node.layer = static_cast<int>(doc.layers.size()) - 1;
                placeInGroup(root, std::move(node), groupOf(s, op.value("group", std::string())), intField(op, "at", -1, 0, 255));
                refuseBackgroundOrder(s, root);
                applyTree(s, root);
                s.markStructure();
                return;
            }
            const int layer = targetLayer(s, op);
            if (action == "remove") {
                TreeNode root = buildTree(doc, -1);
                TreeNode gone;
                detach(root, layer, gone);
                std::vector<int> removed;
                collect(gone, removed);
                bool anyPixelLeft = false;
                for (int i = 0; i < static_cast<int>(doc.layers.size()); i++) {
                    if (std::find(removed.begin(), removed.end(), i) == removed.end() && !doc.layers[static_cast<size_t>(i)].isGroup()) anyPixelLeft = true;
                }
                if (!anyPixelLeft) {
                    fail("last_layer", "name", "the sprite would have no layer left", "add another layer first: {\"op\": \"layer\", \"action\": \"add\"}");
                }
                applyTree(s, root);
                s.markStructure();
                return;
            }
            if (action == "rename") {
                const std::string name = requiredString(op, "new_name", "layer rename");
                if (doc.layers[static_cast<size_t>(layer)].name != name) {
                    doc.layers[static_cast<size_t>(layer)].name = name;
                    s.markStructure();
                }
                return;
            }
            if (action == "move") {
                if (!op.contains("at") && !op.contains("group")) fail("bad_op_field", "at", "layer move needs at (position in its group, 0 is the bottom) or group", s.layerList());
                TreeNode root = buildTree(doc, -1);
                const int oldParent = doc.layers[static_cast<size_t>(layer)].parent;
                const int group = op.contains("group") ? groupOf(s, op["group"].get<std::string>()) : oldParent;
                TreeNode node;
                detach(root, layer, node);
                if (group >= 0 && !findNode(root, group)) fail("no_layer", "group", "a group can't move inside itself", s.layerList());
                placeInGroup(root, std::move(node), group, intField(op, "at", -1, 0, 255));
                refuseBackgroundOrder(s, root);
                applyTree(s, root);
                s.markStructure();
                return;
            }
            // set
            ase::Layer& target = doc.layers[static_cast<size_t>(layer)];
            bool changed = false;
            if (op.contains("visible")) {
                const uint16_t flags = op["visible"].get<bool>() ? (target.flags | ase::kLayerVisible) : (target.flags & static_cast<uint16_t>(~ase::kLayerVisible));
                changed = changed || flags != target.flags;
                target.flags = flags;
            }
            if (op.contains("opacity")) {
                const uint8_t opacity = static_cast<uint8_t>(intField(op, "opacity", 255, 0, 255));
                changed = changed || opacity != target.opacity;
                target.opacity = opacity;
            }
            if (op.contains("blend_mode")) {
                ase::BlendMode blend = target.blend;
                ase::parseBlendMode(op["blend_mode"].get<std::string>(), blend);
                changed = changed || blend != target.blend;
                target.blend = blend;
            }
            if (op.contains("group")) {
                const int group = groupOf(s, op["group"].get<std::string>());
                if (group != target.parent) {
                    TreeNode root = buildTree(doc, -1);
                    TreeNode node;
                    detach(root, layer, node);
                    if (group >= 0 && !findNode(root, group)) fail("no_layer", "group", "a group can't move inside itself", s.layerList());
                    placeInGroup(root, std::move(node), group, -1);
                    refuseBackgroundOrder(s, root);
                    applyTree(s, root);
                    changed = true;
                }
            }
            if (changed) s.markStructure();
        }

        // ------------------------------------------------------------------ frames

        // a frame about to go away or be replaced: its links move to the next frame holding them
        void rehomeLinks(ase::Document& doc, int layer, int frame) {
            ase::Cel* holder = doc.celRecord(layer, frame);
            if (!holder || holder->linkedFrame >= 0) return;
            ase::Cel* heir = nullptr;
            for (ase::Cel& c : doc.cels) {
                if (c.layer != layer || c.linkedFrame != frame) continue;
                if (!heir) {
                    heir = &c;
                    c.linkedFrame = -1;
                    c.x = holder->x;
                    c.y = holder->y;
                    c.image = holder->image;
                    c.tilemap = holder->tilemap;
                } else {
                    c.linkedFrame = heir->frame;
                }
            }
        }

        // Aseprite's DocApi::adjustTags for one frame added before `frame`
        void tagsOnInsert(ase::Document& doc, int frame) {
            for (ase::Tag& t : doc.tags) {
                if (frame <= t.from) t.from++;
                if (frame <= t.to + 1) t.to++;
            }
        }

        // ... and for one frame removed; false when a tag was left empty
        void tagsOnRemove(EditState& s, int frame) {
            std::vector<ase::Tag> kept;
            for (ase::Tag& t : s.doc.tags) {
                if (frame < t.from) t.from--;
                if (frame <= t.to) t.to--;
                if (t.from > t.to) {
                    s.warn("tag_removed", "tag \"" + t.name + "\" lost all its frames and was removed");
                    continue;
                }
                kept.push_back(std::move(t));
            }
            s.doc.tags = std::move(kept);
        }

        // every frame reference through a map (new index of each old one; -1: gone)
        void remapFrames(EditState& s, const std::vector<int>& newOfOld, int newCount) {
            ase::Document& doc = s.doc;
            std::vector<ase::Cel> cels;
            for (ase::Cel& c : doc.cels) {
                const int mapped = newOfOld[static_cast<size_t>(c.frame)];
                if (mapped < 0) continue;
                c.frame = mapped;
                if (c.linkedFrame >= 0) c.linkedFrame = newOfOld[static_cast<size_t>(c.linkedFrame)];
                cels.push_back(std::move(c));
            }
            doc.cels = std::move(cels);
            doc.sortCels();
            std::vector<uint16_t> durations(static_cast<size_t>(newCount), 100);
            for (size_t i = 0; i < newOfOld.size(); i++) {
                if (newOfOld[i] >= 0) durations[static_cast<size_t>(newOfOld[i])] = doc.frameDurations[i];
            }
            doc.frameDurations = std::move(durations);
            std::map<int, std::vector<ase::PaletteEntry>> changes;
            for (auto& [frame, entries] : doc.paletteChanges) {
                if (frame < static_cast<int>(newOfOld.size()) && newOfOld[static_cast<size_t>(frame)] >= 0) changes[newOfOld[static_cast<size_t>(frame)]] = std::move(entries);
            }
            doc.paletteChanges = std::move(changes);
            for (ase::Slice& slice : doc.slices) {
                std::vector<ase::SliceKey> keys;
                for (ase::SliceKey& key : slice.keys) {
                    const int mapped = key.frame < newOfOld.size() ? newOfOld[key.frame] : -1;
                    if (mapped < 0 && key.frame != 0) continue;
                    key.frame = mapped < 0 ? 0 : static_cast<uint32_t>(mapped);
                    keys.push_back(std::move(key));
                }
                std::stable_sort(keys.begin(), keys.end(), [](const ase::SliceKey& a, const ase::SliceKey& b) { return a.frame < b.frame; });
                slice.keys = std::move(keys);
            }
            std::vector<ase::Mask> masks;
            for (ase::Mask& mask : doc.masks) {
                if (mask.frame < static_cast<int>(newOfOld.size()) && newOfOld[static_cast<size_t>(mask.frame)] >= 0) {
                    mask.frame = newOfOld[static_cast<size_t>(mask.frame)];
                    masks.push_back(std::move(mask));
                }
            }
            doc.masks = std::move(masks);
            std::vector<ase::OpaqueChunk> opaque;
            for (ase::OpaqueChunk& chunk : doc.opaque) {
                if (chunk.frame >= static_cast<int>(newOfOld.size())) continue;
                const int mapped = newOfOld[static_cast<size_t>(chunk.frame)];
                if (mapped < 0) continue;
                chunk.frame = mapped;
                opaque.push_back(std::move(chunk));
            }
            doc.opaque = std::move(opaque);
            s.remapRecorded([&](DirtyRect& d) {
                const int mapped = newOfOld[static_cast<size_t>(d.frame - 1)];
                if (mapped < 0) return false;
                d.frame = mapped + 1;
                return true;
            });
        }

        // `count` empty frames before frame index `at`
        void insertFrames(EditState& s, int at, int count) {
            const int old = s.doc.frameCount();
            std::vector<int> newOfOld(static_cast<size_t>(old));
            for (int i = 0; i < old; i++) newOfOld[static_cast<size_t>(i)] = i < at ? i : i + count;
            remapFrames(s, newOfOld, old + count);
            for (int i = 0; i < count; i++) tagsOnInsert(s.doc, at + i);
        }

        void removeFrames(EditState& s, int from, int to) {
            ase::Document& doc = s.doc;
            // a link inherited by another removed frame moves on until every link is outside the range
            for (int layer = 0; layer < static_cast<int>(doc.layers.size()); layer++) {
                for (bool again = true; again;) {
                    again = false;
                    for (int f = from; f <= to; f++) {
                        const bool linkedTo = std::any_of(doc.cels.begin(), doc.cels.end(), [&](const ase::Cel& c) { return c.layer == layer && c.linkedFrame == f; });
                        const ase::Cel* holder = doc.celRecord(layer, f);
                        if (linkedTo && holder && holder->linkedFrame < 0) {
                            rehomeLinks(doc, layer, f);
                            again = true;
                        }
                    }
                }
            }
            const int old = doc.frameCount();
            std::vector<int> newOfOld(static_cast<size_t>(old), -1);
            int next = 0;
            for (int i = 0; i < old; i++) {
                if (i < from || i > to) newOfOld[static_cast<size_t>(i)] = next++;
            }
            remapFrames(s, newOfOld, next);
            for (int f = to; f >= from; f--) tagsOnRemove(s, f);
        }

        // copies frame `source`'s cels to frame `target` (empty so far)
        void copyFrameCels(EditState& s, int source, int target, bool link) {
            ase::Document& doc = s.doc;
            std::vector<ase::Cel> added;
            for (int layer = 0; layer < static_cast<int>(doc.layers.size()); layer++) {
                const ase::Cel* rec = doc.celRecord(layer, source);
                if (!rec) continue;
                const ase::Cel* holder = doc.cel(layer, source);
                if (!holder) continue;
                ase::Cel cel = *holder;
                cel.frame = target;
                cel.opacity = rec->opacity;
                cel.zIndex = rec->zIndex;
                cel.userData = rec->userData;
                cel.extra = rec->extra;
                cel.linkedFrame = link ? holder->frame : -1;
                if (link) {
                    cel.image.reset();
                    cel.tilemap.reset();
                }
                added.push_back(std::move(cel));
            }
            for (ase::Cel& cel : added) doc.cels.push_back(std::move(cel));
            doc.sortCels();
        }

        int frameArg(const EditState& s, const Json& op, const char* name, int def) {
            if (!op.contains(name)) return def;
            const int f = op[name].get<int>();
            if (f < 1 || f > s.doc.frameCount()) fail("no_frame", name, std::string(name) + " " + std::to_string(f) + " is not a frame of this sprite", s.frameRange());
            return f - 1;
        }

        void frameOp(EditState& s, const Json& op) {
            ase::Document& doc = s.doc;
            const std::string action = enumField(op, "action", "", {"add", "remove", "move", "duplicate", "set"});
            if (action.empty()) fail("bad_op_field", "action", "frame needs action", "action is one of add, remove, move, duplicate, set");
            const int count = intField(op, "count", 1, 1, 256);
            auto refuseGrowth = [&](int adding) {
                if (doc.frameCount() + adding > kMaxFrames) {
                    fail("too_many_ops", "count", "the sprite would have " + std::to_string(doc.frameCount() + adding) + " frames, above 256",
                         "a sprite edited here has at most 256 frames");
                }
            };
            if (action == "add") {
                refuseGrowth(count);
                const int at = op.contains("at") ? intField(op, "at", 1, 1, doc.frameCount() + 1) - 1 : doc.frameCount();
                const bool copy = boolField(op, "copy", true);
                const bool link = boolField(op, "link", false);
                const int source = at - 1;
                const uint16_t duration = static_cast<uint16_t>(intField(op, "duration_ms", doc.frameDurations[static_cast<size_t>(std::max(source, 0))], 1, 65535));
                insertFrames(s, at, count);
                for (int i = 0; i < count; i++) {
                    doc.frameDurations[static_cast<size_t>(at + i)] = duration;
                    if (copy && source >= 0) copyFrameCels(s, source, at + i, link);
                }
                s.markStructure();
                return;
            }
            if (action == "duplicate") {
                const int from = frameArg(s, op, "from", -1);
                if (from < 0) fail("bad_op_field", "from", "frame duplicate needs from (the frame to copy, from 1)", s.frameRange());
                const int to = frameArg(s, op, "to", from);
                if (to < from) fail("bad_op_field", "to", "to must not be below from", s.frameRange());
                const int span = to - from + 1;
                refuseGrowth(span * count);
                const bool link = boolField(op, "link", false);
                const int at = to + 1;
                insertFrames(s, at, span * count);
                for (int c = 0; c < count; c++) {
                    for (int i = 0; i < span; i++) {
                        const int target = at + c * span + i;
                        doc.frameDurations[static_cast<size_t>(target)] = doc.frameDurations[static_cast<size_t>(from + i)];
                        copyFrameCels(s, from + i, target, link);
                    }
                }
                s.markStructure();
                return;
            }
            const int from = frameArg(s, op, "from", action == "set" ? 0 : -1);
            if (from < 0) fail("bad_op_field", "from", "frame " + action + " needs from (a frame, from 1)", s.frameRange());
            const int to = op.contains("from") ? frameArg(s, op, "to", from) : (action == "set" ? doc.frameCount() - 1 : from);
            if (to < from) fail("bad_op_field", "to", "to must not be below from", s.frameRange());
            if (action == "set") {
                if (!op.contains("duration_ms")) fail("bad_op_field", "duration_ms", "frame set needs duration_ms (1-65535)", "");
                const uint16_t duration = static_cast<uint16_t>(intField(op, "duration_ms", 100, 1, 65535));
                bool changed = false;
                for (int f = from; f <= to; f++) {
                    changed = changed || doc.frameDurations[static_cast<size_t>(f)] != duration;
                    doc.frameDurations[static_cast<size_t>(f)] = duration;
                }
                if (changed) s.markStructure();
                return;
            }
            if (action == "remove") {
                if (from == 0 && to == doc.frameCount() - 1) {
                    fail("last_frame", "from", "the sprite would have no frame left", "keep at least one frame; " + s.frameRange());
                }
                removeFrames(s, from, to);
                s.markStructure();
                return;
            }
            // move: the frames go before position `at` (counted in the frames as they are now)
            if (!op.contains("at")) fail("bad_op_field", "at", "frame move needs at (the frame to place them before, from 1; last + 1 for the end)", s.frameRange());
            const int at = intField(op, "at", 1, 1, doc.frameCount() + 1) - 1;
            if (at >= from && at <= to + 1) return;   // already there
            const int old = doc.frameCount();
            std::vector<int> order;
            for (int i = 0; i < old; i++) {
                if (i == at) {
                    for (int f = from; f <= to; f++) order.push_back(f);
                }
                if (i < from || i > to) order.push_back(i);
            }
            if (at == old) {
                for (int f = from; f <= to; f++) order.push_back(f);
            }
            std::vector<int> newOfOld(static_cast<size_t>(old), -1);
            for (size_t i = 0; i < order.size(); i++) newOfOld[static_cast<size_t>(order[i])] = static_cast<int>(i);
            remapFrames(s, newOfOld, old);
            // as Aseprite: the moved frames leave their tags and join the ones at the destination
            std::vector<ase::Tag> saved = doc.tags;
            for (int f = to; f >= from; f--) {
                for (ase::Tag& t : saved) {
                    if (f < t.from) t.from--;
                    if (f <= t.to) t.to--;
                }
            }
            const int target = at > to ? at - (to - from + 1) : at;
            for (int f = 0; f < to - from + 1; f++) {
                for (ase::Tag& t : saved) {
                    if (target + f <= t.from) t.from++;
                    if (target + f <= t.to + 1) t.to++;
                }
            }
            std::vector<ase::Tag> kept;
            for (ase::Tag& t : saved) {
                if (t.from <= t.to) kept.push_back(std::move(t));
                else s.warn("tag_removed", "tag \"" + t.name + "\" lost all its frames and was removed");
            }
            doc.tags = std::move(kept);
            s.markStructure();
        }

        // ------------------------------------------------------------------ tags, slices, cels

        void tagOp(EditState& s, const Json& op) {
            ase::Document& doc = s.doc;
            const std::string action = enumField(op, "action", "set", {"set", "remove"});
            const std::string name = requiredString(op, "name", "tag");
            auto it = std::find_if(doc.tags.begin(), doc.tags.end(), [&](const ase::Tag& t) { return t.name == name; });
            if (action == "remove") {
                if (it == doc.tags.end()) fail("no_tag", "name", "the sprite has no tag \"" + name + "\"", s.frameRange());
                doc.tags.erase(it);
                s.markStructure();
                return;
            }
            const bool create = it == doc.tags.end();
            ase::Tag tag = create ? ase::Tag{} : *it;
            const ase::Tag before = tag;
            if (create) {
                tag.name = name;
                if (!op.contains("from")) fail("bad_op_field", "from", "a new tag needs from (a frame, from 1)", s.frameRange());
                tag.from = 0;
                tag.to = 0;
            }
            if (op.contains("from")) tag.from = frameArg(s, op, "from", tag.from);
            if (op.contains("to")) tag.to = frameArg(s, op, "to", tag.to);
            else if (create) tag.to = tag.from;
            if (tag.to < tag.from) fail("bad_op_field", "to", "tag " + name + " has to below from", s.frameRange());
            if (op.contains("new_name")) tag.name = op["new_name"].get<std::string>();
            if (op.contains("direction")) {
                const std::string d = op["direction"].get<std::string>();
                tag.direction = d == "reverse" ? ase::TagDirection::Reverse : d == "pingpong" ? ase::TagDirection::PingPong :
                                d == "pingpong_reverse" ? ase::TagDirection::PingPongReverse : ase::TagDirection::Forward;
            }
            if (op.contains("repeat")) tag.repeat = static_cast<uint16_t>(intField(op, "repeat", 0, 0, 65535));
            if (op.contains("color")) {
                const PixelRgba c = hexValue(op["color"], "color");
                tag.legacyColor = {c[0], c[1], c[2], 255};
                tag.userData.color = c;
            }
            const bool changed = create || tag.from != before.from || tag.to != before.to || tag.name != before.name || tag.direction != before.direction ||
                                 tag.repeat != before.repeat || tag.legacyColor != before.legacyColor || tag.userData != before.userData;
            if (!changed) return;
            if (create) doc.tags.push_back(std::move(tag));
            else *it = std::move(tag);
            std::stable_sort(doc.tags.begin(), doc.tags.end(), [](const ase::Tag& a, const ase::Tag& b) { return a.from < b.from; });
            s.markStructure();
        }

        void sliceOp(EditState& s, const Json& op) {
            ase::Document& doc = s.doc;
            const std::string action = enumField(op, "action", "set", {"set", "remove"});
            const std::string name = requiredString(op, "name", "slice");
            auto it = std::find_if(doc.slices.begin(), doc.slices.end(), [&](const ase::Slice& t) { return t.name == name; });
            std::string names;
            for (const ase::Slice& slice : doc.slices) names += (names.empty() ? "" : ", ") + slice.name;
            if (action == "remove") {
                if (it == doc.slices.end()) fail("no_tag", "name", "the sprite has no slice \"" + name + "\"", names.empty() ? "it has no slices" : "slices: " + names);
                doc.slices.erase(it);
                s.markStructure();
                return;
            }
            const bool create = it == doc.slices.end();
            if (create) {
                if (!op.contains("rect")) fail("bad_op_field", "rect", "a new slice needs rect: [x, y, w, h]", "");
                ase::Slice slice;
                slice.name = name;
                slice.keys.push_back(ase::SliceKey{});
                doc.slices.push_back(std::move(slice));
                it = doc.slices.end() - 1;
            }
            ase::Slice& slice = *it;
            if (op.contains("new_name")) slice.name = op["new_name"].get<std::string>();
            if (op.contains("rect")) {
                // one rectangle for the whole animation; keys made in Aseprite are replaced
                ase::SliceKey key = slice.keys.empty() ? ase::SliceKey{} : slice.keys[0];
                key.frame = 0;
                key.bounds = rectValue(op["rect"], "rect");
                slice.keys = {key};
            }
            for (ase::SliceKey& key : slice.keys) {
                if (op.contains("pivot")) key.pivot = pointValue(op["pivot"], "pivot");
                if (op.contains("center")) key.center = rectValue(op["center"], "center");
            }
            s.markStructure();
        }

        void celOp(EditState& s, const Json& op) {
            ase::Document& doc = s.doc;
            const std::string action = enumField(op, "action", "", {"link", "unlink", "clear", "set"});
            if (action.empty()) fail("bad_op_field", "action", "cel needs action", "action is one of link, unlink, clear, set");
            const int layer = s.pixelLayer(op);
            const std::vector<int> frames = s.frames(op);
            bool changed = false;
            if (action == "link") {
                const int from = frameArg(s, op, "from", -1);
                if (from < 0) fail("bad_op_field", "from", "cel link needs from (the frame whose cel the others show)", s.frameRange());
                const ase::Cel* holder = doc.cel(layer, from);
                if (!holder) fail("no_frame", "from", "frame " + std::to_string(from + 1) + " has no cel on " + doc.layerPath(layer), "link to a frame that has one");
                const int holderFrame = holder->frame;
                for (int f : frames) {
                    if (f == holderFrame) continue;
                    ase::Cel* rec = doc.celRecord(layer, f);
                    if (rec && rec->linkedFrame == holderFrame) continue;
                    rehomeLinks(doc, layer, f);
                    rec = doc.celRecord(layer, f);
                    if (!rec) {
                        ase::Cel cel;
                        cel.layer = layer;
                        cel.frame = f;
                        doc.cels.push_back(std::move(cel));
                        doc.sortCels();
                        rec = doc.celRecord(layer, f);
                    }
                    const ase::Cel* source = doc.cel(layer, holderFrame);
                    rec->linkedFrame = holderFrame;
                    rec->image.reset();
                    rec->tilemap.reset();
                    rec->x = source->x;
                    rec->y = source->y;
                    changed = true;
                }
            } else if (action == "unlink") {
                for (int f : frames) {
                    ase::Cel* rec = doc.celRecord(layer, f);
                    if (!rec || rec->linkedFrame < 0) continue;
                    const ase::Cel holder = *doc.cel(layer, f);
                    rec = doc.celRecord(layer, f);
                    rec->linkedFrame = -1;
                    rec->image = holder.image;
                    rec->tilemap = holder.tilemap;
                    rec->x = holder.x;
                    rec->y = holder.y;
                    changed = true;
                }
            } else if (action == "clear") {
                for (int f : frames) {
                    if (!doc.celRecord(layer, f)) continue;
                    rehomeLinks(doc, layer, f);
                    doc.cels.erase(std::remove_if(doc.cels.begin(), doc.cels.end(), [&](const ase::Cel& c) { return c.layer == layer && c.frame == f; }), doc.cels.end());
                    changed = true;
                }
            } else {
                if (!op.contains("opacity") && !op.contains("z_index")) fail("bad_op_field", "opacity", "cel set needs opacity or z_index", "");
                for (int f : frames) {
                    ase::Cel* rec = doc.celRecord(layer, f);
                    if (!rec) continue;
                    if (op.contains("opacity")) {
                        const uint8_t v = static_cast<uint8_t>(intField(op, "opacity", 255, 0, 255));
                        changed = changed || rec->opacity != v;
                        rec->opacity = v;
                    }
                    if (op.contains("z_index")) {
                        const int16_t v = static_cast<int16_t>(intField(op, "z_index", 0, -32768, 32767));
                        changed = changed || rec->zIndex != v;
                        rec->zIndex = v;
                    }
                }
            }
            if (changed) s.markStructure();
        }

        // ------------------------------------------------------------------ palette and canvas

        // every image cel's RGBA again from the palette (its indices are unchanged)
        void repaintCels(EditState& s) {
            ase::Document& doc = s.doc;
            if (doc.mode != ase::ColorMode::Indexed) return;
            const std::vector<uint8_t> palette = doc.paletteRgba(0);
            for (ase::Cel& cel : doc.cels) {
                if (!cel.image || !cel.image->hasIndices()) continue;
                PixelImage image = *cel.image;
                const bool background = doc.layers[static_cast<size_t>(cel.layer)].background();
                image.repaint(palette, background ? -1 : doc.transparentIndex);
                cel.image = std::make_shared<const PixelImage>(std::move(image));
            }
        }

        int highestUsedIndex(const ase::Document& doc) {
            int highest = 0;
            for (const ase::Cel& cel : doc.cels) {
                if (!cel.image || !cel.image->hasIndices()) continue;
                for (uint8_t i : cel.image->indices()) highest = std::max<int>(highest, i);
            }
            return highest;
        }

        void needRoomFor(const EditState& s, int entries) {
            if (s.doc.mode != ase::ColorMode::Indexed) return;
            const int highest = highestUsedIndex(s.doc);
            if (entries <= highest) {
                fail("bad_color", "colors", "the pixels use palette index " + std::to_string(highest) + " but the new palette has indices 0-" + std::to_string(entries - 1),
                     "give at least " + std::to_string(highest + 1) + " colours, or use remap: \"nearest\" with action load");
            }
        }

        void paletteOp(EditState& s, const Json& op) {
            ase::Document& doc = s.doc;
            const std::string action = enumField(op, "action", "", {"set", "add", "replace", "load"});
            if (action.empty()) fail("bad_op_field", "action", "palette needs action", "action is one of set, add, replace, load");
            auto colorList = [&]() {
                if (!op.contains("colors")) fail("bad_op_field", "colors", "palette " + action + " needs colors: [\"#rrggbb\", ...]", "");
                std::vector<PixelRgba> colors;
                for (size_t i = 0; i < op["colors"].size(); i++) colors.push_back(hexValue(op["colors"][i], "colors[" + std::to_string(i) + "]"));
                return colors;
            };
            const std::vector<ase::PaletteEntry> before = doc.palette;
            if (!doc.tilesets.empty() && (action == "set" || action == "load")) {
                fail("unsupported_feature", "palette", "palette " + action + " would leave this sprite's tileset indices stale",
                     "edit the palette in Aseprite (which remaps the tilesets), or use action add or replace");
            }
            if (action == "set") {
                const std::vector<PixelRgba> colors = colorList();
                if (colors.size() > 256) fail("palette_full", "colors", "a palette holds at most 256 colours, this list " + std::to_string(colors.size()), "give at most 256 colours");
                needRoomFor(s, static_cast<int>(colors.size()));
                doc.palette.clear();
                for (const PixelRgba& c : colors) doc.palette.push_back({c, std::nullopt});
            } else if (action == "add") {
                const std::vector<PixelRgba> colors = colorList();
                if (doc.palette.size() + colors.size() > 256) {
                    fail("palette_full", "colors", "the palette has " + std::to_string(doc.palette.size()) + " colours: adding " + std::to_string(colors.size()) + " would pass 256",
                         "palette has indices 0-" + std::to_string(static_cast<int>(doc.palette.size()) - 1) + "; replace entries with action replace");
                }
                for (const PixelRgba& c : colors) doc.palette.push_back({c, std::nullopt});
            } else if (action == "replace") {
                if (!op.contains("index") || !op.contains("color")) fail("bad_op_field", op.contains("index") ? "color" : "index", "palette replace needs index and color", "");
                const int index = op["index"].get<int>();
                if (index < 0 || index >= static_cast<int>(doc.palette.size())) {
                    fail("bad_color", "index", "index " + std::to_string(index) + " is not in the palette", "palette has indices 0-" + std::to_string(static_cast<int>(doc.palette.size()) - 1));
                }
                if (s.indexed() && index == doc.transparentIndex) {
                    fail("index_0", "index", "index " + std::to_string(index) + " is the transparent index of this indexed sprite", "replace an index other than " + std::to_string(index));
                }
                doc.palette[static_cast<size_t>(index)].color = hexValue(op["color"], "color");
            } else {
                if (!op.contains("asset")) fail("bad_op_field", "asset", "palette load needs asset (a .palette path)", "");
                jasperengine::pixelart::Palette loaded;
                PixelProblem problem;
                if (!s.env.loadPalette || !s.env.loadPalette(op["asset"].get<std::string>(), loaded, problem)) {
                    if (problem.empty()) problem = {"bad_color", "asset", "the palette " + op["asset"].get<std::string>() + " can't be read", "list_assets finds .palette files"};
                    throw OpError{problem};
                }
                if (loaded.colors.size() > 256) fail("palette_full", "asset", "that palette holds more than 256 colours", "use a palette of at most 256 colours");
                const bool nearest = enumField(op, "remap", "keep_indices", {"keep_indices", "nearest"}) == "nearest";
                std::vector<ase::PaletteEntry> entries;
                for (const jasperengine::pixelart::Rgba8& c : loaded.colors) entries.push_back({{c.r, c.g, c.b, c.a}, std::nullopt});
                if (s.indexed() && nearest && !entries.empty()) {
                    // each used colour to the nearest of the new palette; the transparent index stays
                    std::vector<jasperengine::pixelart::Rgba8> colors = loaded.colors;
                    if (doc.transparentIndex < colors.size()) colors[doc.transparentIndex].a = 0;
                    jasperengine::pixelart::PaletteMatcher matcher(colors, jasperengine::pixelart::ColorMetric::OkLab);
                    std::vector<uint8_t> remap(256, 0);
                    for (int i = 0; i < 256; i++) {
                        if (i == doc.transparentIndex || i >= static_cast<int>(before.size())) {
                            remap[static_cast<size_t>(i)] = static_cast<uint8_t>(i == doc.transparentIndex ? i : doc.transparentIndex);
                            continue;
                        }
                        const PixelRgba c = before[static_cast<size_t>(i)].color;
                        remap[static_cast<size_t>(i)] = static_cast<uint8_t>(matcher.nearest({c[0], c[1], c[2], 255}));
                    }
                    doc.palette = entries;
                    const std::vector<uint8_t> palette = doc.paletteRgba(0);
                    for (ase::Cel& cel : doc.cels) {
                        if (!cel.image || !cel.image->hasIndices()) continue;
                        std::vector<uint8_t> indices(cel.image->indices().begin(), cel.image->indices().end());
                        for (uint8_t& i : indices) i = remap[i];
                        const bool background = doc.layers[static_cast<size_t>(cel.layer)].background();
                        cel.image = std::make_shared<const PixelImage>(PixelImage::fromIndices(cel.image->width(), cel.image->height(), std::move(indices), palette,
                                                                                               background ? -1 : doc.transparentIndex));
                    }
                } else {
                    needRoomFor(s, static_cast<int>(entries.size()));
                    doc.palette = entries;
                }
            }
            if (doc.palette == before) return;
            repaintCels(s);
            s.markStructure();
        }

        // the canvas's new size and where the old origin lands in it; cels cropped to it
        void reframe(EditState& s, int newW, int newH, int dx, int dy) {
            ase::Document& doc = s.doc;
            const IntRect canvas{0, 0, newW, newH};
            std::vector<ase::Cel> cels;
            for (ase::Cel& cel : doc.cels) {
                if (cel.linkedFrame >= 0) {
                    cels.push_back(std::move(cel));
                    continue;
                }
                if (cel.tilemap || !cel.image) {
                    cel.x += dx;
                    cel.y += dy;
                    cels.push_back(std::move(cel));
                    continue;
                }
                const bool background = doc.layers[static_cast<size_t>(cel.layer)].background();
                const IntRect bounds{cel.x + dx, cel.y + dy, cel.image->width(), cel.image->height()};
                const IntRect kept = background ? canvas : bounds.intersect(canvas);
                if (kept.empty()) {
                    // links to it lose their cel too: Aseprite has no empty linked cels
                    const int layer = cel.layer, frame = cel.frame;
                    for (ase::Cel& other : doc.cels) {
                        if (other.layer == layer && other.linkedFrame == frame) other.linkedFrame = -2;
                    }
                    continue;
                }
                // a blank of the document's model, not crop(): a background layer's image has no clear
                // index, so a rect leaving it would lose its plane
                PixelImage image = blankImage(doc, cel.layer, kept.w, kept.h);
                image.blit(*cel.image, bounds.x - kept.x, bounds.y - kept.y);
                cel.image = std::make_shared<const PixelImage>(std::move(image));
                cel.x = kept.x;
                cel.y = kept.y;
                cels.push_back(std::move(cel));
            }
            cels.erase(std::remove_if(cels.begin(), cels.end(), [](const ase::Cel& c) { return c.linkedFrame == -2; }), cels.end());
            doc.cels = std::move(cels);
            for (ase::Slice& slice : doc.slices) {
                for (ase::SliceKey& key : slice.keys) {
                    key.bounds.x += dx;
                    key.bounds.y += dy;
                }
            }
            doc.width = newW;
            doc.height = newH;
            s.hasSelection = false;
            s.selection.clear();
            s.remapRecorded([&](DirtyRect& d) {
                d.rect = IntRect{d.rect.x + dx, d.rect.y + dy, d.rect.w, d.rect.h}.intersect(canvas);
                return !d.rect.empty();
            });
        }

        void canvasOp(EditState& s, const Json& op) {
            ase::Document& doc = s.doc;
            const std::string action = enumField(op, "action", "", {"resize", "crop", "convert"});
            if (action.empty()) fail("bad_op_field", "action", "canvas needs action", "action is one of resize, crop, convert");
            if (action == "resize") {
                if (!op.contains("size")) fail("bad_op_field", "size", "canvas resize needs size: [w, h] (each 1-4096)", "");
                const IntPoint size = pointValue(op["size"], "size");
                const std::string anchor = enumField(op, "anchor", "top_left", {"center", "top_left", "top", "top_right", "left", "right", "bottom_left", "bottom", "bottom_right"});
                if (size.x < 1 || size.y < 1 || size.x > kMaxEditSide || size.y > kMaxEditSide) {
                    fail("canvas_too_large", "size", "the canvas would be " + std::to_string(size.x) + "x" + std::to_string(size.y), "each side is 1-4096");
                }
                if (size.x == doc.width && size.y == doc.height) return;
                const int spareX = size.x - doc.width, spareY = size.y - doc.height;
                const int fx = anchor.find("left") != std::string::npos ? 0 : anchor.find("right") != std::string::npos ? 2 : 1;
                const int fy = anchor.rfind("top", 0) == 0 ? 0 : anchor.rfind("bottom", 0) == 0 ? 2 : 1;
                reframe(s, size.x, size.y, static_cast<int>(std::floor(spareX * fx / 2.0)), static_cast<int>(std::floor(spareY * fy / 2.0)));
                s.markStructure();
                return;
            }
            if (action == "crop") {
                if (!op.contains("rect")) fail("bad_op_field", "rect", "canvas crop needs rect: [x, y, w, h]", "");
                const IntRect rect = rectValue(op["rect"], "rect").intersect({0, 0, doc.width, doc.height});
                if (rect.empty()) fail("bad_op_field", "rect", "rect lies outside the canvas", "the canvas is " + std::to_string(doc.width) + "x" + std::to_string(doc.height));
                if (rect == IntRect{0, 0, doc.width, doc.height}) return;
                reframe(s, rect.w, rect.h, -rect.x, -rect.y);
                s.markStructure();
                return;
            }
            const std::string mode = enumField(op, "mode", "", {"indexed", "rgba"});
            if (mode.empty()) fail("bad_op_field", "mode", "canvas convert needs mode: indexed or rgba", "");
            if ((mode == "indexed") == s.indexed() && (mode != "rgba" || !s.grayscale())) return;
            if (mode == "rgba") {
                // every image a new pointer, grayscale's too: the writer keys a chunk's unchanged
                // bytes by the pointer, and those bytes are in the old depth
                for (ase::Cel& cel : doc.cels) {
                    if (!cel.image) continue;
                    PixelImage image = *cel.image;
                    if (image.hasIndices()) image.dropIndices();
                    cel.image = std::make_shared<const PixelImage>(std::move(image));
                }
                doc.mode = ase::ColorMode::Rgba;
                s.markStructure();
                return;
            }
            if (!doc.tilesets.empty()) {
                fail("unsupported_feature", "mode", "converting to indexed does not reach this sprite's tilesets", "convert it in Aseprite");
            }
            if (doc.palette.empty()) fail("too_many_colors", "mode", "the sprite has no palette to convert to", "set one first: {\"op\": \"palette\", \"action\": \"set\", \"colors\": [...]}");
            const bool snap = boolField(op, "snap", false);
            const int transparent = doc.transparentIndex;
            std::map<uint32_t, int> exact;
            std::vector<jasperengine::pixelart::Rgba8> colors;
            for (size_t i = 0; i < doc.palette.size(); i++) {
                const PixelRgba& c = doc.palette[i].color;
                colors.push_back({c[0], c[1], c[2], static_cast<int>(i) == transparent ? uint8_t(0) : c[3]});
                if (static_cast<int>(i) == transparent) continue;
                const uint32_t key = (uint32_t(c[0]) << 24) | (uint32_t(c[1]) << 16) | (uint32_t(c[2]) << 8) | c[3];
                if (!exact.count(key)) exact[key] = static_cast<int>(i);
            }
            int snapped = 0;
            std::string example;
            std::vector<std::pair<const ase::Cel*, std::vector<uint8_t>>> planes;
            std::optional<jasperengine::pixelart::PaletteMatcher> matcher;
            for (const ase::Cel& cel : doc.cels) {
                if (!cel.image) continue;
                const int w = cel.image->width(), h = cel.image->height();
                std::vector<uint8_t> indices(static_cast<size_t>(w) * h, static_cast<uint8_t>(transparent));
                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) {
                        const PixelRgba c = cel.image->at(x, y);
                        if (c[3] == 0) continue;
                        const uint32_t key = (uint32_t(c[0]) << 24) | (uint32_t(c[1]) << 16) | (uint32_t(c[2]) << 8) | c[3];
                        auto hit = exact.find(key);
                        if (hit != exact.end()) {
                            indices[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(hit->second);
                            continue;
                        }
                        snapped++;
                        if (example.empty()) example = hexColor(c);
                        if (snap) {
                            if (!matcher) matcher.emplace(colors, jasperengine::pixelart::ColorMetric::OkLab);
                            indices[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(matcher->nearest({c[0], c[1], c[2], 255}));
                        }
                    }
                }
                planes.push_back({&cel, std::move(indices)});
            }
            if (snapped > 0 && !snap) {
                fail("too_many_colors", "mode", std::to_string(snapped) + " pixels are not in the palette (" + example + " is one)",
                     "pass snap: true to take the nearest palette colours (OKLab), or add the colours first with {\"op\": \"palette\", \"action\": \"add\"}");
            }
            const std::vector<uint8_t> palette = doc.paletteRgba(0);
            std::vector<std::shared_ptr<const PixelImage>> converted;
            for (auto& [cel, indices] : planes) {
                const bool background = doc.layers[static_cast<size_t>(cel->layer)].background();
                converted.push_back(std::make_shared<const PixelImage>(PixelImage::fromIndices(cel->image->width(), cel->image->height(), std::move(indices), palette,
                                                                                               background ? -1 : transparent)));
            }
            size_t next = 0;
            for (ase::Cel& cel : doc.cels) {
                if (cel.image) cel.image = converted[next++];
            }
            doc.mode = ase::ColorMode::Indexed;
            if (snapped > 0) s.warn("snapped", std::to_string(snapped) + " pixels took the nearest palette colour");
            s.markStructure();
        }

    }

    namespace {

        // Aseprite keeps a link's holder at the earliest frame (the writer refuses a link to a later
        // one): whoever an op moved, the earliest frame of each linked group becomes the holder
        void holdersAtEarliestFrame(ase::Document& doc) {
            for (int layer = 0; layer < static_cast<int>(doc.layers.size()); layer++) {
                // holder frame -> the frames showing it (itself included), ascending
                std::map<int, std::vector<int>> groups;
                for (const ase::Cel& c : doc.cels) {
                    if (c.layer != layer) continue;
                    if (c.linkedFrame >= 0) {
                        int root = c.linkedFrame;
                        for (int hops = 0; hops < 8; hops++) {
                            const ase::Cel* next = doc.celRecord(layer, root);
                            if (!next || next->linkedFrame < 0) break;
                            root = next->linkedFrame;
                        }
                        groups[root].push_back(c.frame);
                    } else if (c.image || c.tilemap) {
                        groups[c.frame];
                    }
                }
                for (auto& [root, frames] : groups) {
                    if (frames.empty()) continue;
                    frames.push_back(root);
                    std::sort(frames.begin(), frames.end());
                    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
                    const int earliest = frames.front();
                    ase::Cel* holder = doc.celRecord(layer, root);
                    ase::Cel* first = doc.celRecord(layer, earliest);
                    if (!holder || !first) continue;
                    if (earliest != root) {
                        first->image = holder->image;
                        first->tilemap = holder->tilemap;
                        first->x = holder->x;
                        first->y = holder->y;
                        first->opacity = holder->opacity;
                        first->linkedFrame = -1;
                        holder->image.reset();
                        holder->tilemap.reset();
                    }
                    for (int f : frames) {
                        if (f == earliest) continue;
                        ase::Cel* cel = doc.celRecord(layer, f);
                        if (cel) cel->linkedFrame = earliest;
                    }
                }
            }
        }

    }

    void runStructureOp(EditState& state, const Json& op, const std::string& name) {
        if (name == "layer") layerOp(state, op);
        else if (name == "frame") frameOp(state, op);
        else if (name == "tag") tagOp(state, op);
        else if (name == "slice") sliceOp(state, op);
        else if (name == "cel") celOp(state, op);
        else if (name == "palette") paletteOp(state, op);
        else if (name == "canvas") canvasOp(state, op);
        if (name == "frame" || name == "cel") holdersAtEarliestFrame(state.doc);
    }

}
