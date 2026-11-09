// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The drawing tools of the `sprites` group (docs/specs/S47-agent-sprite-drawing.md §6.1):
// create_sprite, get_sprite, read_sprite_pixels, edit_sprite, render_sprite and diff_sprite. They
// work on project files with no "open" step: SpriteDocCache follows each file on disk, and each
// edit_sprite is one SpriteEditCmd on the project history.

#include "McpTools.h"
#include "PlayViewCommon.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpServer.h"

#include "Project.h"
#include "Scene.h"
#include "asset/AssetIndex.h"
#include "component/CameraComponent.h"
#include "component/MeshComponent.h"
#include "render/SceneRender.h"
#include "object/Camera.h"
#include "asset/Guid.h"
#include "command/CommandHistory.h"
#include "command/type/SpriteEditCmd.h"
#include "pixelart/BuiltinPalettes.h"
#include "pixelart/PaletteAssets.h"
#include "pixelart/PixelColor.h"
#include "pixelart/aseprite/AsepriteImporter.h"
#include "pixelart/draw/GridCodec.h"
#include "pixelart/draw/SpriteDiff.h"
#include "pixelart/draw/SpriteDocCache.h"
#include "pixelart/draw/SpriteDrawLint.h"
#include "pixelart/draw/SpriteEdit.h"
#include "pixelart/draw/SpriteImportFrames.h"
#include "pixelart/draw/SpriteRender.h"
#include "util/Base64.h"
#include "util/ProjectFileWrites.h"
#include "vcs/VcsService.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <set>

namespace fs = std::filesystem;

namespace jasperengine::editor::mcp {

namespace {

namespace ase = pixelart::aseprite;
namespace draw = pixelart::draw;
using pixelart::IntRect;
using pixelart::PixelProblem;
using pixelart::PixelRgba;

Json problemJson(const PixelProblem& p, int opIndex = -1) {
    Json j = {{"kind", p.kind}, {"message", p.message}, {"fix", p.fix}};
    if (!p.field.empty()) j["field"] = p.field;
    if (opIndex >= 0) j["op_index"] = opIndex;
    return j;
}

void failProblem(const CallPtr& call, const PixelProblem& p, int opIndex = -1, Json extra = Json::object()) {
    std::string text = p.kind + ": " + p.message;
    if (opIndex >= 0) text = "ops[" + std::to_string(opIndex) + "]: " + text;
    if (!p.fix.empty()) text += " Fix: " + p.fix;
    Json error = problemJson(p, opIndex);
    for (auto it = extra.begin(); it != extra.end(); ++it) error[it.key()] = it.value();
    call->fail(text, Json{{"error", error}});
}

std::string sessionKey(const CallPtr& call) {
    SessionPtr session = call->session();
    return session ? session->id : std::string();
}

std::string clientOf(const CallPtr& call) {
    const std::string& client = call->getUndoOrigin().client;
    if (!client.empty()) return client;
    return call->client().empty() ? "Agent" : call->client();
}

Json rectJson(const IntRect& r) { return {r.x, r.y, r.w, r.h}; }

std::string rectText(const IntRect& r) {
    return "[" + std::to_string(r.x) + "," + std::to_string(r.y) + "," + std::to_string(r.w) + "," + std::to_string(r.h) + "]";
}

std::string baseName(const std::string& path) { return fs::path(path).filename().generic_string(); }

Json rectsJson(const ase::Document& doc, const std::vector<draw::DirtyRect>& rects) {
    Json list = Json::array();
    for (const draw::DirtyRect& d : rects) {
        const std::string layer = d.layer >= 0 && d.layer < static_cast<int>(doc.layers.size()) ? doc.layerPath(d.layer) : std::to_string(d.layer);
        list.push_back({{"frame", d.frame}, {"layer", layer}, {"rect", rectJson(d.rect)}});
    }
    return list;
}

const char* modeName(const ase::Document& doc) { return ase::colorModeName(doc.mode); }

// R10: the structure, frames from 1, compact
Json spriteJson(Project* project, draw::SpriteSession& session) {
    const ase::Document& doc = session.sprite.doc;
    Json j;
    j["path"] = session.path;
    j["revision"] = session.revision;
    j["size"] = {doc.width, doc.height};
    j["mode"] = modeName(doc);
    Json palette = Json::array();
    for (const ase::PaletteEntry& e : doc.palette) palette.push_back(draw::hexColor(e.color));
    j["palette"] = std::move(palette);
    if (doc.mode == ase::ColorMode::Indexed) j["transparent_index"] = doc.transparentIndex;
    Json layers = Json::array();
    for (int i = 0; i < static_cast<int>(doc.layers.size()); i++) {
        const ase::Layer& l = doc.layers[static_cast<size_t>(i)];
        Json lj = {{"index", i}, {"name", doc.layerPath(i)}};
        if (l.parent >= 0) lj["group"] = doc.layerPath(l.parent);
        if (!l.visible()) lj["visible"] = false;
        if (l.opacity != 255) lj["opacity"] = l.opacity;
        if (l.blend != ase::BlendMode::Normal) lj["blend_mode"] = ase::blendModeName(l.blend);
        if (l.isGroup()) lj["kind"] = "group";
        else if (l.type == ase::LayerType::Tilemap) lj["kind"] = "tilemap";
        layers.push_back(std::move(lj));
    }
    j["layers"] = std::move(layers);
    Json frames = Json::array();
    for (int f = 0; f < doc.frameCount(); f++) frames.push_back({{"frame", f + 1}, {"duration_ms", doc.frameDurations[static_cast<size_t>(f)]}});
    j["frames"] = std::move(frames);
    Json tags = Json::array();
    for (const ase::Tag& t : doc.tags) {
        Json tj = {{"name", t.name}, {"from", t.from + 1}, {"to", t.to + 1}};
        if (t.direction != ase::TagDirection::Forward) tj["direction"] = ase::tagDirectionName(t.direction);
        if (t.repeat != 0) tj["repeat"] = t.repeat;
        tags.push_back(std::move(tj));
    }
    j["tags"] = std::move(tags);
    Json slices = Json::array();
    for (const ase::Slice& s : doc.slices) {
        Json sj = {{"name", s.name}};
        if (!s.keys.empty()) {
            const ase::SliceKey& key = s.keys[0];
            sj["rect"] = rectJson(key.bounds);
            if (key.pivot) sj["pivot"] = {key.pivot->x, key.pivot->y};
            if (key.center) sj["center"] = rectJson(*key.center);
            if (s.keys.size() > 1) sj["keys"] = s.keys.size();
        }
        slices.push_back(std::move(sj));
    }
    j["slices"] = std::move(slices);
    Json cels = Json::array();
    for (const ase::Cel& c : doc.cels) {
        Json cj = {{"frame", c.frame + 1}, {"layer", c.layer}};
        if (c.linkedFrame >= 0) {
            cj["linked_to"] = c.linkedFrame + 1;
        } else if (c.image) {
            const pixelart::IntRect bbox{c.x, c.y, c.image->width(), c.image->height()};
            cj["bbox"] = rectJson(bbox);
        }
        cels.push_back(std::move(cj));
    }
    j["cels"] = std::move(cels);
    Json usedBy = Json::array();
    AssetDatabase::Resolution found = project->resolveAsset(session.path, "", AssetType::Unknown);
    if (found.found() && project->getAssetIndex().isReady()) {
        for (const DepEdge& edge : project->getAssetIndex().referencesTo(found.guid, session.path, false, 64)) {
            if (edge.entity == 0 || edge.component.find("Sprite") == std::string::npos) continue;
            usedBy.push_back({{"file", project->getAssetIndex().referrerPath(edge.referrer)}, {"object", edge.entityPath}});
        }
    }
    j["used_by"] = std::move(usedBy);
    if (session.lastEdit) {
        j["last_edit"] = {{"by", session.lastEdit->by}, {"revision", session.lastEdit->revision}, {"rects", rectsJson(doc, session.lastEdit->rects)}};
    }
    return j;
}

std::string summaryOf(const draw::SpriteSession& session) {
    const ase::Document& doc = session.sprite.doc;
    std::string out = baseName(session.path) + ": " + std::to_string(doc.width) + "x" + std::to_string(doc.height) + " " + modeName(doc) + ", " +
                      std::to_string(doc.frameCount()) + " frame" + (doc.frameCount() == 1 ? "" : "s") + ", " + std::to_string(doc.layers.size()) + " layer" +
                      (doc.layers.size() == 1 ? "" : "s");
    if (!doc.tags.empty()) out += ", tags";
    for (size_t i = 0; i < doc.tags.size(); i++) out += std::string(i == 0 ? " " : ", ") + doc.tags[i].name;
    return out + ". Revision " + session.revision + ".";
}

// the image of a frame as a read sees it: the composite or one layer's canvas
pixelart::PixelImage readImage(draw::SpriteDoc& sprite, int frame, int layer) {
    if (layer < 0) {
        ase::RenderOptions options;
        options.keepIndices = sprite.indexed();
        return ase::renderFrame(sprite.doc, frame, options);
    }
    return draw::celCanvas(sprite.doc, layer, frame);
}

std::string layerLabel(const ase::Document& doc, int layer) {
    return layer < 0 ? std::string("composite") : doc.layerPath(layer);
}

int clearIndexOf(const pixelart::PixelImage& image) {
    return image.hasIndices() ? image.transparentIndex() : -1;
}

// ------------------------------------------------------------------ get_sprite

void getSpriteTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    PixelProblem problem;
    std::shared_ptr<draw::SpriteSession> session = draw::SpriteDocCache::get().open(project, call->args().value("sprite", std::string()), problem);
    if (!session) {
        failProblem(call, problem);
        return;
    }
    session->lastSeen[sessionKey(call)] = session->revision;
    call->ok(summaryOf(*session), spriteJson(project, *session));
}

// ------------------------------------------------------------------ read_sprite_pixels

void readSpritePixelsTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    PixelProblem problem;
    std::shared_ptr<draw::SpriteSession> session = draw::SpriteDocCache::get().open(project, args.value("sprite", std::string()), problem);
    if (!session) {
        failProblem(call, problem);
        return;
    }
    draw::SpriteDoc& sprite = session->sprite;
    const ase::Document& doc = sprite.doc;
    std::vector<int> frames{0};
    if (args.contains("frame") && !draw::resolveFrames(sprite, args["frame"], "frame", frames, problem)) {
        failProblem(call, problem);
        return;
    }
    if (frames.size() > 16) {
        failProblem(call, {"region_too_large", "frame", "a read takes at most 16 frames, this one " + std::to_string(frames.size()), "read fewer frames, or render_sprite to look"});
        return;
    }
    int layer = -1;
    const Json layerArg = args.value("layer", Json("composite"));
    if (!(layerArg.is_string() && layerArg == "composite") && !draw::resolveLayer(sprite, layerArg, "layer", true, layer, problem)) {
        failProblem(call, problem);
        return;
    }
    const IntRect canvas{0, 0, doc.width, doc.height};
    IntRect rect = canvas;
    if (args.contains("rect")) {
        const Json& r = args["rect"];
        if (!r.is_array() || r.size() != 4 || !r[0].is_number_integer() || !r[1].is_number_integer() || !r[2].is_number_integer() || !r[3].is_number_integer()) {
            failProblem(call, {"bad_op_field", "rect", "rect must be [x, y, w, h] integers", "the canvas is " + std::to_string(doc.width) + "x" + std::to_string(doc.height)});
            return;
        }
        rect = IntRect{r[0].get<int>(), r[1].get<int>(), r[2].get<int>(), r[3].get<int>()}.intersect(canvas);
        if (rect.empty()) {
            failProblem(call, {"bad_op_field", "rect", "rect lies outside the canvas", "x is 0-" + std::to_string(doc.width - 1) + " and y is 0-" + std::to_string(doc.height - 1)});
            return;
        }
    }
    const int maxPixels = std::clamp(args.value("max_pixels", 4096), 1, 16384);
    const long long area = static_cast<long long>(rect.w) * rect.h * static_cast<long long>(frames.size());
    if (area > maxPixels) {
        const int tile = std::min(64, std::max(1, rect.w));
        failProblem(call, {"region_too_large", "rect", "the region is " + std::to_string(area) + " pixels, above max_pixels " + std::to_string(maxPixels),
                           "read tiles (for example rect [" + std::to_string(rect.x) + "," + std::to_string(rect.y) + "," + std::to_string(tile) + "," + std::to_string(tile) + "] then [" +
                               std::to_string(rect.x + tile) + "," + std::to_string(rect.y) + "," + std::to_string(tile) + "," + std::to_string(tile) + "]), or render_sprite to look"});
        return;
    }
    draw::GridEncoding encoding = draw::GridEncoding::Auto;
    if (args.contains("encoding") && !draw::parseGridEncoding(args["encoding"].get<std::string>(), encoding)) {
        failProblem(call, {"bad_op_field", "encoding", "unknown encoding", "encoding is auto, chars, hex2, legend or hex8"});
        return;
    }
    // every frame in one pass over a tall image, so all of them share one legend and encoding
    std::vector<pixelart::PixelImage> images;
    bool anyIndexed = false;
    for (int f : frames) {
        images.push_back(readImage(sprite, f, layer));
        anyIndexed = anyIndexed || images.back().hasIndices();
    }
    pixelart::PixelImage stack;
    IntRect regionRect{0, 0, rect.w, rect.h * static_cast<int>(frames.size())};
    if (images[0].hasIndices()) {
        stack = pixelart::PixelImage::fromIndices(rect.w, regionRect.h, std::vector<uint8_t>(static_cast<size_t>(rect.w) * regionRect.h, doc.transparentIndex), doc.paletteRgba(0),
                                                  clearIndexOf(images[0]));
    } else {
        stack = pixelart::PixelImage(rect.w, regionRect.h);
    }
    for (size_t i = 0; i < images.size(); i++) stack.blit(images[i].crop(rect), 0, static_cast<int>(i) * rect.h);
    const std::vector<uint8_t> palette = doc.paletteRgba(0);
    const draw::EncodedGrid grid = draw::encodeRegion(stack, regionRect, encoding, palette, clearIndexOf(stack));
    Json frameItems = Json::array();
    std::string text = baseName(session->path) + " " + draw::gridEncodingName(grid.encoding) + " revision " + session->revision;
    for (size_t i = 0; i < frames.size(); i++) {
        Json rows = Json::array();
        text += "\nframe " + std::to_string(frames[i] + 1) + " layer " + layerLabel(doc, layer) + " rect " + rectText(rect);
        for (int y = 0; y < rect.h; y++) {
            const std::string& row = grid.rows[i * static_cast<size_t>(rect.h) + static_cast<size_t>(y)];
            rows.push_back(row);
            text += "\n" + row;
        }
        frameItems.push_back({{"frame", frames[i] + 1}, {"layer", layerLabel(doc, layer)}, {"rect", rectJson(rect)}, {"rows", std::move(rows)}});
    }
    Json out = {{"revision", session->revision}, {"mode", modeName(doc)}, {"size", {doc.width, doc.height}}, {"encoding", draw::gridEncodingName(grid.encoding)}, {"frames", std::move(frameItems)}};
    if (grid.indexed()) out["used"] = grid.colors;
    else if (grid.encoding == draw::GridEncoding::Legend) out["legend"] = grid.colors;
    session->lastSeen[sessionKey(call)] = session->revision;
    if (grid.encoding == draw::GridEncoding::Legend || grid.indexed()) text += "\ncolors " + grid.colors.dump();
    call->ok(text, std::move(out));
}

// ------------------------------------------------------------------ edit_sprite

// R34: a stale view of the file
void failChanged(const CallPtr& call, draw::SpriteSession& session, const std::string& lastSeen) {
    draw::SpriteDocCache& cache = draw::SpriteDocCache::get();
    Json extra = {{"current_revision", session.revision}, {"last_seen_revision", lastSeen}, {"changed_by", session.changedExternally ? "external" : "editor"}};
    Json rects = Json::array();
    if (std::optional<draw::SpriteDoc> seen = cache.rebuild(session, lastSeen)) {
        if (seen->doc.width == session.sprite.doc.width && seen->doc.height == session.sprite.doc.height) {
            draw::DiffOptions options;
            for (int f = 0; f < session.sprite.doc.frameCount() && f < seen->doc.frameCount(); f++) options.frames.push_back(f);
            options.grid = false;
            options.maxCells = 0;
            Json diff;
            PixelProblem ignored;
            if (draw::diffSprites(session.sprite, *seen, options, diff, nullptr, ignored)) {
                for (const Json& frame : diff["frames"]) {
                    if (frame.contains("bbox")) rects.push_back({{"frame", frame["frame"]}, {"rect", frame["bbox"]}});
                }
            }
        }
    }
    extra["changed_rects"] = std::move(rects);
    failProblem(call, {"sprite_changed", "sprite", baseName(session.path) + " changed since this session last saw it (revision " + lastSeen + " to " + session.revision + ")",
                       "read the region again, then resend with expect_revision: " + session.revision}, -1, std::move(extra));
}

draw::EditEnv editEnvFor(Project* project) {
    draw::EditEnv env;
    env.loadPalette = [project](const std::string& asset, jasperengine::pixelart::Palette& out, PixelProblem& problem) {
        pixelart::ResolvedPalette resolved = pixelart::resolvePaletteName(*project, asset);
        if (!resolved.ok) {
            problem = resolved.error;
            if (problem.empty()) problem = {"bad_color", "asset", "no palette " + asset, "list_assets finds .palette files"};
            return false;
        }
        out = std::move(resolved.palette);
        return true;
    };
    env.findRamp = [project](const std::string& name, std::string& known) {
        std::vector<PixelRgba> colors;
        std::vector<pixelart::ProjectPalette> palettes = pixelart::listProjectPalettes(*project);
        const std::string defaultPath = pixelart::defaultPalettePath(*project);
        std::stable_sort(palettes.begin(), palettes.end(), [&](const pixelart::ProjectPalette& a, const pixelart::ProjectPalette& b) {
            return (a.path == defaultPath) > (b.path == defaultPath);
        });
        for (const pixelart::ProjectPalette& entry : palettes) {
            for (const jasperengine::pixelart::PaletteRamp& ramp : entry.palette.ramps) known += (known.empty() ? "" : ", ") + ramp.name;
            if (!colors.empty()) continue;
            for (const jasperengine::pixelart::Rgba8& c : entry.palette.rampColors(name)) colors.push_back({c.r, c.g, c.b, c.a});
        }
        return colors;
    };
    return env;
}

std::vector<jasperengine::pixelart::Rgba8> backdropsOf(Project* project) {
    std::vector<jasperengine::pixelart::Rgba8> out;
    for (const std::string& hex : project->getPixelArtSettings().backdrops) {
        jasperengine::pixelart::Rgba8 c;
        if (jasperengine::pixelart::parseHex(hex, c)) out.push_back(c);
    }
    return out;
}

// S29: a file another user locked
bool lockedByOther(Project* project, const fs::path& absolute, std::string& owner) {
    const vcs::VcsChange* change = project->getVcs().find(absolute);
    if (!change || change->lock != vcs::LockState::LockedByOther) return false;
    owner = change->lockOwner;
    return true;
}

void editSpriteTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    PixelProblem problem;
    draw::SpriteDocCache& cache = draw::SpriteDocCache::get();
    std::shared_ptr<draw::SpriteSession> session = cache.open(project, args.value("sprite", std::string()), problem);
    if (!session) {
        failProblem(call, problem);
        return;
    }
    int opIndex = -1;
    if (!draw::validateOps(args.value("ops", Json()), problem, opIndex)) {
        failProblem(call, problem, opIndex);
        return;
    }
    if (args.contains("lint") && !draw::validateLintArg(args["lint"], problem)) {
        failProblem(call, problem);
        return;
    }
    const std::string echoMode = args.value("echo", std::string("dirty"));
    const int echoMax = std::clamp(args.value("echo_max", 1024), 0, 4096);
    if (echoMode != "dirty" && echoMode != "composite" && echoMode != "none") {
        failProblem(call, {"bad_op_field", "echo", "echo must be dirty, composite or none", "echo is one of dirty, composite, none"});
        return;
    }
    std::string owner;
    if (lockedByOther(project, session->absolute, owner)) {
        failProblem(call, {"file_locked", "sprite", baseName(session->path) + " is locked by " + owner, "ask " + owner + " to release the lock, or edit another file"});
        return;
    }
    const std::string sessionId = sessionKey(call);
    if (args.contains("expect_revision")) {
        const std::string expected = args["expect_revision"].get<std::string>();
        if (expected != session->revision) {
            failChanged(call, *session, expected);
            return;
        }
    } else {
        auto seen = session->lastSeen.find(sessionId);
        if (seen != session->lastSeen.end() && seen->second != session->revision) {
            const std::string lastSeen = seen->second;
            seen->second = session->revision;
            failChanged(call, *session, lastSeen);
            return;
        }
    }
    draw::SpriteDoc working = session->sprite;
    draw::EditRequest request;
    request.ops = args["ops"];
    request.frame = args.value("frame", Json());
    request.layer = args.value("layer", Json());
    request.symmetry = args.value("symmetry", Json());
    draw::EditResult result;
    if (!draw::applyOps(working, request, editEnvFor(project), result, problem, opIndex)) {
        failProblem(call, problem, opIndex);
        return;
    }
    session->lastSeen[sessionId] = session->revision;
    const std::string previous = session->revision;
    Json warnings = Json::array();
    for (const draw::EditWarning& w : result.warnings) {
        Json item = {{"kind", w.kind}, {"message", w.message}};
        if (w.opIndex >= 0) item["op_index"] = w.opIndex;
        warnings.push_back(std::move(item));
    }
    Json out = {{"revision", previous}, {"previous_revision", previous}, {"applied", result.applied}};
    auto reportNothing = [&]() {
        out["changed"] = {{"pixels", 0}, {"rects", Json::array()}};
        if (!warnings.empty()) out["warnings"] = warnings;
        call->ok(baseName(session->path) + ": " + std::to_string(result.applied) + " op(s) changed nothing; nothing written. Revision " + previous + ".", std::move(out));
    };
    if (!result.changedAnything()) {
        reportNothing();
        return;
    }
    auto bytes = std::make_shared<std::vector<uint8_t>>();
    if (!draw::encodeSprite(working, *bytes, problem)) {
        failProblem(call, problem);
        return;
    }
    const std::string revision = draw::SpriteDocCache::revisionOf(*bytes);
    if (revision == previous) {
        reportNothing();
        return;
    }
    // the session's next state: the written bytes read back, with their provenance
    draw::SpriteDoc written;
    if (!draw::decodeSprite(session->absolute, *bytes, written, problem)) {
        failProblem(call, problem);
        return;
    }
    std::shared_ptr<draw::SpritePatchSet> patch = draw::makePatch(session->sprite, working, result.structureChanged);
    patch->revisionBefore = previous;
    patch->revisionAfter = revision;
    patch->rects = result.changed;
    patch->by = clientOf(call);
    patch->summary = draw::summarizeOps(result.opKinds);
    // the undo must give the old bytes back (R32): when re-encoding would not, it keeps them
    {
        draw::SpriteDoc undone = written;
        draw::applyPatch(undone, *patch, false);
        std::vector<uint8_t> undoneBytes;
        PixelProblem ignored;
        if (!draw::encodeSprite(undone, undoneBytes, ignored) || draw::SpriteDocCache::revisionOf(undoneBytes) != previous) patch->beforeFile = session->bytes;
    }
    CommandHistory* history = project->getProjectCommandHistory();
    auto* command = new SpriteEditCmd(project, session->absolute, patch, bytes, written);
    if (!history->addCommand(command)) {
        failProblem(call, {"write_failed", "sprite", "couldn't write " + session->path, "check the file is writable"});
        return;
    }
    session = cache.open(project, args.value("sprite", std::string()), problem);
    if (!session) {
        failProblem(call, problem);
        return;
    }
    draw::LastEdit last;
    last.by = patch->by;
    last.revision = revision;
    last.summary = patch->summary;
    last.rects = result.changed;
    last.timeMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    session->lastEdit = last;
    session->lastSeen[sessionId] = session->revision;

    ase::Document& doc = session->sprite.doc;
    out["revision"] = session->revision;
    out["changed"] = {{"pixels", result.pixels}, {"rects", rectsJson(doc, result.changed)}};
    // R13: the dirty rects, up to echo_max pixels
    std::string text = baseName(session->path) + ": " + std::to_string(result.applied) + " op(s), " + std::to_string(result.pixels) + " pixel(s) in " + std::to_string(result.changed.size()) +
                       " rect(s). Revision " + previous + " to " + session->revision + ".";
    if (echoMode != "none") {
        Json echoes = Json::array();
        Json truncated = Json::array();
        int budget = echoMax;
        for (const draw::DirtyRect& d : result.changed) {
            const int area = d.rect.w * d.rect.h;
            if (area > budget || d.layer >= static_cast<int>(doc.layers.size()) || d.frame - 1 >= doc.frameCount()) {
                truncated.push_back({{"frame", d.frame}, {"layer", doc.layerPath(d.layer)}, {"rect", rectJson(d.rect)}});
                continue;
            }
            budget -= area;
            const pixelart::PixelImage image = readImage(session->sprite, d.frame - 1, echoMode == "composite" ? -1 : d.layer);
            const draw::EncodedGrid grid = draw::encodeRegion(image, d.rect, draw::GridEncoding::Auto, doc.paletteRgba(0), clearIndexOf(image));
            Json item = {{"frame", d.frame}, {"layer", echoMode == "composite" ? std::string("composite") : doc.layerPath(d.layer)}, {"rect", rectJson(d.rect)}, {"encoding", draw::gridEncodingName(grid.encoding)}, {"rows", grid.rows}};
            if (grid.indexed()) item["used"] = grid.colors;
            else if (grid.encoding == draw::GridEncoding::Legend) item["legend"] = grid.colors;
            text += "\nframe " + std::to_string(d.frame) + " " + item["layer"].get<std::string>() + " " + rectText(d.rect);
            for (const std::string& row : grid.rows) text += "\n" + row;
            echoes.push_back(std::move(item));
        }
        out["echo"] = std::move(echoes);
        if (!truncated.empty()) {
            out["echo_truncated"] = std::move(truncated);
            out["next"] = "Some changed rects were too large to echo (echo_max " + std::to_string(echoMax) + " pixels): read_sprite_pixels {rect} or render_sprite shows them.";
            text += "\nSome rects were too large to echo: read_sprite_pixels {rect} or render_sprite shows them.";
        }
    }
    if (result.structureChanged) out["structure"] = spriteJson(project, *session);
    if (!warnings.empty()) out["warnings"] = warnings;
    if (args.contains("lint") && args["lint"] != Json(false)) {
        out["lints"] = draw::lintAfterDraw(session->sprite, result.changed, args["lint"], backdropsOf(project));
        const Json& lints = out["lints"];
        text += "\nLints: " + std::to_string(lints["counts"]["warn"].get<int>()) + " warn, " + std::to_string(lints["counts"]["fail"].get<int>()) + " fail.";
    }
    ToolResult reply = ToolResult::success(text, std::move(out));
    if (args.value("image", false)) {
        draw::SheetOptions sheet;
        std::set<int> frames;
        for (const draw::DirtyRect& d : result.changed) {
            if (d.frame - 1 < doc.frameCount()) frames.insert(d.frame - 1);
        }
        if (frames.empty()) frames.insert(0);
        sheet.frames.assign(frames.begin(), frames.end());
        sheet.highlight = result.changed;
        Json layout;
        const pixelart::PixelImage image = draw::renderSheet(session->sprite, sheet, layout);
        const std::string png = image.encodePng();
        reply.images.push_back({Base64::encode(reinterpret_cast<const unsigned char*>(png.data()), png.size()), "image/png"});
    }
    call->reply(std::move(reply));
}

// ------------------------------------------------------------------ render_sprite

bool parseBackdrops(Project* project, const Json& value, std::vector<draw::SheetBackdrop>& out, PixelProblem& problem) {
    const std::vector<std::string> projectBackdrops = project->getPixelArtSettings().backdrops;
    auto named = [&](const std::string& text) -> bool {
        draw::SheetBackdrop backdrop;
        backdrop.name = text;
        jasperengine::pixelart::Rgba8 c;
        if (text == "checker") {
            backdrop.checker = true;
        } else if (text == "dark" || text == "light") {
            const std::string hex = projectBackdrops.size() > (text == "dark" ? 0u : 1u) ? projectBackdrops[text == "dark" ? 0 : 1] : std::string(text == "dark" ? "#1a1c2c" : "#f4f4f4");
            if (!jasperengine::pixelart::parseHex(hex, c)) return false;
            backdrop.color = {c.r, c.g, c.b, 255};
        } else if (jasperengine::pixelart::parseHex(text, c)) {
            backdrop.color = {c.r, c.g, c.b, 255};
        } else {
            return false;
        }
        out.push_back(backdrop);
        return true;
    };
    if (value.is_string() ? !named(value.get<std::string>()) : !value.is_array()) {
        problem = {"bad_op_field", "backdrops", "backdrops holds \"checker\", \"dark\", \"light\" or \"#rrggbb\"", "for example [\"checker\", \"dark\"]"};
        return false;
    }
    if (value.is_array()) {
        for (const Json& item : value) {
            if (!item.is_string() || !named(item.get<std::string>())) {
                problem = {"bad_op_field", "backdrops", "backdrops holds \"checker\", \"dark\", \"light\" or \"#rrggbb\"", "for example [\"checker\", \"dark\"]"};
                return false;
            }
        }
    }
    return true;
}

// R27: a second image, the view around a GameObject
void renderSpriteContext(const CallPtr& call, ToolResult reply, const Json& context) {
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* sp = McpRefs::resolveScene(project, Json(), error);
    if (!sp) {
        call->fail(error);
        return;
    }
    Entity entity = w3::resolveLiveEntity(project, sp, context.value("entity", Json()), error);
    if (entity == NULL_ENTITY) {
        call->fail(error);
        return;
    }
    const std::string view = context.value("view", std::string("game"));
    if (view != "game" && view != "scene") {
        failProblem(call, {"bad_op_field", "context.view", "context.view is game or scene", ""});
        return;
    }
    w3::CaptureRequest request;
    request.view = view;
    request.scene = sp->id;
    request.maxWidth = 0;
    w3::captureView(call, request, [call, reply = std::move(reply), project, sceneId = sp->id, entity, view](w3::CaptureResult capture) mutable {
        if (!capture.ok) {
            call->fail(capture.error);
            return;
        }
        std::string error;
        SceneProject* scene = project->getScene(sceneId);
        const std::vector<unsigned char> decoded = Base64::decode(capture.image.data);
        std::optional<pixelart::PixelImage> image = pixelart::PixelImage::decodePng(std::span<const uint8_t>(decoded.data(), decoded.size()), &error);
        if (!image || !scene) {
            call->fail("The " + view + " view could not be read: " + error);
            return;
        }
        Matrix4 viewProjection;
        IntRect drawn = image->bounds();
        if (view == "scene") {
            if (Camera* camera = scene->sceneRender ? scene->sceneRender->getCamera() : nullptr) viewProjection = camera->getViewProjectionMatrix();
        } else {
            Entity cameraEntity = scene->scene->getCamera();
            viewProjection = scene->scene->getComponent<CameraComponent>(cameraEntity).viewProjectionMatrix;
            if (capture.info.contains("canvas_rect_in_image")) {
                const Json& r = capture.info["canvas_rect_in_image"];
                drawn = {r.value("x", 0), r.value("y", 0), r.value("width", image->width()), r.value("height", image->height())};
            }
        }
        // a sprite's mesh holds its world bounds; without one the whole view is shown
        AABB box;
        if (MeshComponent* mesh = scene->scene->findComponent<MeshComponent>(entity)) box = mesh->worldAABB;
        const Vector3 lo = box.getMinimum(), hi = box.getMaximum();
        double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
        for (int corner = 0; corner < 8 && !box.isNull(); corner++) {
            const Vector4 clip = viewProjection * Vector4((corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y, (corner & 4) ? hi.z : lo.z, 1.0f);
            if (clip.w == 0.0f) continue;
            const double ndcX = clip.x / clip.w, ndcY = clip.y / clip.w;
            const double px = drawn.x + (ndcX + 1.0) * 0.5 * drawn.w, py = drawn.y + (1.0 - ndcY) * 0.5 * drawn.h;
            minX = std::min(minX, px);
            minY = std::min(minY, py);
            maxX = std::max(maxX, px);
            maxY = std::max(maxY, py);
        }
        IntRect crop = image->bounds();
        if (maxX > minX && maxY > minY) {
            // the sprite's bounds plus 50 %
            const double marginX = (maxX - minX) * 0.25, marginY = (maxY - minY) * 0.25;
            crop = IntRect{static_cast<int>(std::floor(minX - marginX)), static_cast<int>(std::floor(minY - marginY)),
                           static_cast<int>(std::ceil(maxX - minX + marginX * 2)), static_cast<int>(std::ceil(maxY - minY + marginY * 2))}.intersect(image->bounds());
        }
        if (crop.empty()) crop = image->bounds();
        const pixelart::PixelImage cut = image->crop(crop);
        const int scale = std::max(1, 512 / std::max(cut.width(), cut.height()));
        pixelart::PixelImage scaled(cut.width() * scale, cut.height() * scale);
        std::span<uint8_t> out = scaled.editPixels();
        for (int y = 0; y < cut.height(); y++) {
            const uint8_t* row = cut.row(y);
            for (int x = 0; x < cut.width(); x++) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        std::memcpy(out.data() + (static_cast<size_t>(y * scale + sy) * scaled.width() + static_cast<size_t>(x * scale + sx)) * 4, row + x * 4, 4);
                    }
                }
            }
        }
        const std::string png = scaled.encodePng();
        ToolResult done = std::move(reply);
        done.images.push_back({Base64::encode(reinterpret_cast<const unsigned char*>(png.data()), png.size()), "image/png"});
        if (done.structured.is_object()) done.structured["context"] = {{"view", view}, {"scale", scale}, {"rect", rectJson(crop)}};
        call->reply(std::move(done));
    });
}

void renderSpriteTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    PixelProblem problem;
    std::shared_ptr<draw::SpriteSession> session = draw::SpriteDocCache::get().open(project, args.value("sprite", std::string()), problem);
    if (!session) {
        failProblem(call, problem);
        return;
    }
    draw::SpriteDoc& sprite = session->sprite;
    const ase::Document& doc = sprite.doc;
    draw::SheetOptions sheet;
    bool capped = false;
    if (args.contains("frames")) {
        if (!draw::resolveFrames(sprite, args["frames"], "frames", sheet.frames, problem)) {
            failProblem(call, problem);
            return;
        }
        if (sheet.frames.size() > 64) {
            failProblem(call, {"region_too_large", "frames", "a render takes at most 64 frames, this one " + std::to_string(sheet.frames.size()), "render a tag or a list of at most 64 frames"});
            return;
        }
    } else {
        for (int f = 0; f < doc.frameCount() && f < 64; f++) sheet.frames.push_back(f);
        capped = doc.frameCount() > 64;
    }
    if (args.contains("layers")) {
        const Json& layers = args["layers"];
        if (layers.is_string() && layers == "all") {
            sheet.allLayers = true;
        } else if (layers.is_array()) {
            for (const Json& name : layers) {
                int index = -1;
                if (!draw::resolveLayer(sprite, name, "layers", false, index, problem)) {
                    failProblem(call, problem);
                    return;
                }
                sheet.layers.push_back(index);
            }
        } else if (!(layers.is_string() && layers == "visible")) {
            failProblem(call, {"bad_op_field", "layers", "layers is \"visible\", \"all\" or a list of names", ""});
            return;
        }
    }
    sheet.scale = std::clamp(args.value("scale", 0), 0, 64);
    sheet.columns = std::clamp(args.value("columns", 0), 0, 64);
    sheet.maxWidth = std::clamp(args.value("max_width", 1024), 128, 2048);
    sheet.grid = args.value("grid", false);
    sheet.labels = args.value("labels", true);
    if (args.contains("backdrops") && !parseBackdrops(project, args["backdrops"], sheet.backdrops, problem)) {
        failProblem(call, problem);
        return;
    }
    if (args.contains("onion")) {
        const Json& onion = args["onion"];
        sheet.onionBefore = std::clamp(onion.value("before", 1), 0, 3);
        sheet.onionAfter = std::clamp(onion.value("after", 0), 0, 3);
        sheet.onionOpacity = std::clamp(onion.value("opacity", 0.3), 0.1, 0.9);
    }
    Json lastEdit;
    if (session->lastEdit) lastEdit = {{"by", session->lastEdit->by}, {"revision", session->lastEdit->revision}, {"rects", rectsJson(doc, session->lastEdit->rects)}};
    const Json highlight = args.value("highlight", Json("none"));
    if (highlight.is_string() && highlight == "last_edit") {
        if (session->lastEdit) sheet.highlight = session->lastEdit->rects;
    } else if (highlight.is_array()) {
        if (highlight.size() != 4 || !std::all_of(highlight.begin(), highlight.end(), [](const Json& v) { return v.is_number_integer(); })) {
            failProblem(call, {"bad_op_field", "highlight", "highlight is \"last_edit\", \"none\" or a rect [x, y, w, h]", ""});
            return;
        }
        const IntRect r{highlight[0].get<int>(), highlight[1].get<int>(), highlight[2].get<int>(), highlight[3].get<int>()};
        for (int f : sheet.frames) sheet.highlight.push_back({f + 1, 0, r, 0});
    } else if (!(highlight.is_string() && highlight == "none")) {
        failProblem(call, {"bad_op_field", "highlight", "highlight is \"last_edit\", \"none\" or a rect [x, y, w, h]", ""});
        return;
    }
    Json layout;
    const pixelart::PixelImage image = draw::renderSheet(sprite, sheet, layout);
    const std::string png = image.encodePng();
    Json out = {{"scale", layout["scale"]}, {"layout", layout["layout"]}, {"last_edit", lastEdit}, {"revision", session->revision}};
    if (capped) out["next"] = "The sprite has " + std::to_string(doc.frameCount()) + " frames: this shows the first 64; pass frames for the rest.";
    session->lastSeen[sessionKey(call)] = session->revision;
    std::string text = baseName(session->path) + ": " + std::to_string(sheet.frames.size()) + " frame(s) at " + std::to_string(layout["scale"].get<int>()) + "x (" +
                       std::to_string(image.width()) + "x" + std::to_string(image.height()) + ").";
    ToolResult reply = ToolResult::success(text, std::move(out));
    reply.images.push_back({Base64::encode(reinterpret_cast<const unsigned char*>(png.data()), png.size()), "image/png"});
    if (args.contains("context")) {
        renderSpriteContext(call, std::move(reply), args["context"]);
        return;
    }
    call->reply(std::move(reply));
}

// ------------------------------------------------------------------ diff_sprite

void diffSpriteTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    PixelProblem problem;
    draw::SpriteDocCache& cache = draw::SpriteDocCache::get();
    std::shared_ptr<draw::SpriteSession> session = cache.open(project, args.value("sprite", std::string()), problem);
    if (!session) {
        failProblem(call, problem);
        return;
    }
    draw::SpriteDoc& sprite = session->sprite;
    draw::DiffOptions options;
    if (args.contains("frames")) {
        if (!draw::resolveFrames(sprite, args["frames"], "frames", options.frames, problem)) {
            failProblem(call, problem);
            return;
        }
    } else {
        for (int f = 0; f < sprite.doc.frameCount(); f++) options.frames.push_back(f);
    }
    options.maxCells = std::clamp(args.value("max_cells", 64), 0, 1024);
    options.grid = args.value("grid", true);
    const Json layerArg = args.value("layer", Json("composite"));
    if (!(layerArg.is_string() && layerArg == "composite") && !draw::resolveLayer(sprite, layerArg, "layer", true, options.layer, problem)) {
        failProblem(call, problem);
        return;
    }
    if (args.contains("rect")) {
        const Json& r = args["rect"];
        if (!r.is_array() || r.size() != 4 || !std::all_of(r.begin(), r.end(), [](const Json& v) { return v.is_number_integer(); })) {
            failProblem(call, {"bad_op_field", "rect", "rect must be [x, y, w, h] integers", ""});
            return;
        }
        options.rect = IntRect{r[0].get<int>(), r[1].get<int>(), r[2].get<int>(), r[3].get<int>()};
    }
    const Json against = args.value("against", Json("previous"));
    std::optional<draw::SpriteDoc> other;
    std::string knownList;
    for (const std::string& r : cache.knownRevisions(*session)) knownList += (knownList.empty() ? "" : ", ") + r;
    if (against.is_string()) {
        std::string revision = against.get<std::string>();
        if (revision == "previous") {
            std::shared_ptr<const draw::SpritePatchSet> patch = cache.newestPatch(*session);
            if (!patch) {
                failProblem(call, {"revision_unknown", "against", "this editor session has no edit_sprite on " + baseName(session->path) + " to compare with", "known revisions: " + knownList});
                return;
            }
            revision = patch->revisionBefore;
        }
        other = cache.rebuild(*session, revision);
        if (!other) {
            failProblem(call, {"revision_unknown", "against", "revision " + revision + " can't be rebuilt in this editor session", "known revisions: " + knownList});
            return;
        }
    } else if (against.is_object() && against.contains("frame")) {
        std::vector<int> single;
        if (!draw::resolveFrames(sprite, against["frame"], "against.frame", single, problem)) {
            failProblem(call, problem);
            return;
        }
        options.againstFrame = single[0];
        other = sprite;
    } else if (against.is_object() && against.contains("sprite")) {
        std::shared_ptr<draw::SpriteSession> otherSession = cache.open(project, against["sprite"].get<std::string>(), problem);
        if (!otherSession) {
            failProblem(call, problem);
            return;
        }
        other = otherSession->sprite;
    } else {
        failProblem(call, {"bad_op_field", "against", "against is \"previous\", a revision, {\"sprite\": path} or {\"frame\": n}", ""});
        return;
    }
    Json out;
    draw::DiffImages images;
    if (!draw::diffSprites(sprite, *other, options, out, &images, problem)) {
        failProblem(call, problem);
        return;
    }
    out["revision"] = session->revision;
    session->lastSeen[sessionKey(call)] = session->revision;
    const bool same = out["same"].get<bool>();
    const int changed = out["changed"].get<int>();
    std::string text = baseName(session->path) + ": " + (same ? std::string("no difference") : std::to_string(changed) + " pixel(s) differ");
    if (!out["structure_changes"].empty()) text += "; structure: " + out["structure_changes"].dump();
    text += ".";
    ToolResult reply = ToolResult::success(text, std::move(out));
    if (args.value("image", false) && !images.before.empty()) {
        const std::string png = draw::renderDiffImage(images.before, images.after, images.bbox, 768).encodePng();
        reply.images.push_back({Base64::encode(reinterpret_cast<const unsigned char*>(png.data()), png.size()), "image/png"});
    }
    call->reply(std::move(reply));
}

// ------------------------------------------------------------------ create_sprite

// the palette as entries, index 0 kept for transparency on an indexed sprite
bool paletteEntries(Project* project, const Json& args, bool indexed, std::vector<ase::PaletteEntry>& out, PixelProblem& problem) {
    std::vector<jasperengine::pixelart::Rgba8> colors;
    int transparent = -1;
    if (args.contains("palette") && args["palette"].is_array()) {
        for (const Json& item : args["palette"]) {
            jasperengine::pixelart::Rgba8 c;
            if (!item.is_string() || !jasperengine::pixelart::parseHex(item.get<std::string>(), c)) {
                problem = {"bad_color", "palette", "palette holds \"#rrggbb\" or \"#rrggbbaa\" colours", "a list of hex colours, a .palette path, or a built-in name"};
                return false;
            }
            colors.push_back(c);
        }
    } else {
        std::string name = args.value("palette", std::string());
        if (name.empty()) {
            const std::string defaultPath = pixelart::defaultPalettePath(*project);
            if (!defaultPath.empty()) {
                name = defaultPath;
            } else {
                const std::vector<pixelart::ProjectPalette> palettes = pixelart::listProjectPalettes(*project);
                name = palettes.size() == 1 ? palettes[0].path : "builtin:sweetie-16";
            }
        }
        pixelart::ResolvedPalette resolved = pixelart::resolvePaletteName(*project, name);
        if (!resolved.ok) {
            problem = resolved.error.empty() ? PixelProblem{"bad_color", "palette", "no palette " + name, "a .palette path or a built-in name such as sweetie-16"} : resolved.error;
            return false;
        }
        colors = resolved.palette.colors;
        transparent = resolved.palette.transparent;
    }
    if (indexed && transparent != 0 && !(!colors.empty() && colors[0].a == 0)) {
        // the palette's colours keep their order, one index up: index 0 is the sprite's transparent index
        colors.insert(colors.begin(), jasperengine::pixelart::Rgba8{0, 0, 0, 0});
    }
    if (colors.size() > 256) {
        problem = {"palette_full", "palette", "the palette holds " + std::to_string(colors.size()) + " colours, above 256", "an indexed sprite has at most 255 colours and the transparent index"};
        return false;
    }
    for (const jasperengine::pixelart::Rgba8& c : colors) out.push_back({{c.r, c.g, c.b, c.a}, std::nullopt});
    return true;
}

void createSpriteTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    const std::string given = args.value("path", std::string());
    const fs::path absolute = (project->getProjectPath() / fs::path(given)).lexically_normal();
    const std::string assetPath = absolute.lexically_relative(project->getAssetsPath().lexically_normal()).generic_string();
    if (assetPath.empty() || assetPath.rfind("..", 0) == 0) {
        failProblem(call, {"outside_assets", "path", given + " is outside the assets directory", "sprites live under " + project->getAssetsPath().generic_string()});
        return;
    }
    if (draw::isPngPath(absolute) || !draw::isSpritePath(absolute)) {
        failProblem(call, {"not_a_sprite", "path", given + " is not an .aseprite or .ase path", "pass a path ending in .aseprite or .ase"});
        return;
    }
    std::error_code ec;
    const bool exists = fs::exists(absolute, ec);
    if (exists && !args.value("overwrite", false)) {
        failProblem(call, {"output_exists", "path", given + " already exists", "pass overwrite: true, or choose another path"});
        return;
    }
    ase::Document doc;
    doc.headerFlags = ase::kHeaderLayerOpacity;
    const std::string mode = args.value("mode", std::string("indexed"));
    if (mode != "indexed" && mode != "rgba") {
        failProblem(call, {"bad_op_field", "mode", "mode is indexed or rgba", ""});
        return;
    }
    doc.mode = mode == "indexed" ? ase::ColorMode::Indexed : ase::ColorMode::Rgba;
    doc.transparentIndex = 0;
    const int duration = args.value("duration_ms", 100);
    if (duration < 1 || duration > 65535) {
        failProblem(call, {"bad_op_field", "duration_ms", "duration_ms is 1-65535", ""});
        return;
    }
    doc.speed = static_cast<uint16_t>(duration);
    std::vector<ase::Layer> layers;
    if (args.contains("layers")) {
        if (!args["layers"].is_array() || args["layers"].empty() || args["layers"].size() > static_cast<size_t>(draw::kMaxLayers)) {
            failProblem(call, {"bad_op_field", "layers", "layers is a list of 1-256 names, bottom first", ""});
            return;
        }
        for (const Json& name : args["layers"]) {
            ase::Layer layer;
            layer.name = name.get<std::string>();
            layers.push_back(std::move(layer));
        }
    } else {
        ase::Layer layer;
        layer.name = "Layer 1";
        layers.push_back(std::move(layer));
    }
    doc.layers = std::move(layers);
    PixelProblem problem;
    if (args.contains("from")) {
        const std::string fromGiven = args["from"].get<std::string>();
        const fs::path from = (project->getProjectPath() / fs::path(fromGiven)).lexically_normal();
        draw::ImportedFrames imported;
        if (!draw::readFramesFile(from, imported, problem)) {
            failProblem(call, problem);
            return;
        }
        doc.width = imported.frames[0].width();
        doc.height = imported.frames[0].height();
        doc.frameDurations = imported.durations;
        // indexed when it fits: the colours by first appearance, index 0 transparent
        std::vector<PixelRgba> colors;
        std::map<uint32_t, int> slot;
        bool fits = args.value("mode", std::string()) != "rgba";
        for (const pixelart::PixelImage& frame : imported.frames) {
            for (int y = 0; y < frame.height() && fits; y++) {
                for (int x = 0; x < frame.width(); x++) {
                    const PixelRgba c = frame.at(x, y);
                    if (c[3] == 0) continue;
                    const uint32_t key = (uint32_t(c[0]) << 24) | (uint32_t(c[1]) << 16) | (uint32_t(c[2]) << 8) | c[3];
                    if (slot.count(key)) continue;
                    if (colors.size() >= 255) {
                        fits = false;
                        break;
                    }
                    colors.push_back(c);
                    slot[key] = static_cast<int>(colors.size());
                }
            }
        }
        doc.mode = fits ? ase::ColorMode::Indexed : ase::ColorMode::Rgba;
        if (fits) {
            doc.palette.push_back({{0, 0, 0, 0}, std::nullopt});
            for (const PixelRgba& c : colors) doc.palette.push_back({c, std::nullopt});
        }
        const std::vector<uint8_t> paletteRgba = doc.paletteRgba(0);
        for (size_t f = 0; f < imported.frames.size(); f++) {
            const pixelart::PixelImage& frame = imported.frames[f];
            const IntRect bounds = frame.opaqueBounds();
            if (bounds.empty()) continue;
            ase::Cel cel;
            cel.layer = 0;
            cel.frame = static_cast<int>(f);
            cel.x = bounds.x;
            cel.y = bounds.y;
            pixelart::PixelImage crop = frame.crop(bounds);
            if (fits) {
                std::vector<uint8_t> indices(static_cast<size_t>(bounds.w) * bounds.h, 0);
                for (int y = 0; y < bounds.h; y++) {
                    for (int x = 0; x < bounds.w; x++) {
                        const PixelRgba c = crop.at(x, y);
                        if (c[3] == 0) continue;
                        const uint32_t key = (uint32_t(c[0]) << 24) | (uint32_t(c[1]) << 16) | (uint32_t(c[2]) << 8) | c[3];
                        indices[static_cast<size_t>(y) * bounds.w + x] = static_cast<uint8_t>(slot[key]);
                    }
                }
                crop = pixelart::PixelImage::fromIndices(bounds.w, bounds.h, std::move(indices), paletteRgba, 0);
            }
            cel.image = std::make_shared<const pixelart::PixelImage>(std::move(crop));
            doc.cels.push_back(std::move(cel));
        }
        doc.sortCels();
    } else {
        if (!args.contains("size") || !args["size"].is_array() || args["size"].size() != 2 || !args["size"][0].is_number_integer() || !args["size"][1].is_number_integer()) {
            failProblem(call, {"bad_op_field", "size", "create_sprite needs size [w, h] or from", "size is [w, h], each 1-4096"});
            return;
        }
        doc.width = args["size"][0].get<int>();
        doc.height = args["size"][1].get<int>();
        const int frames = args.value("frames", 1);
        if (frames < 1 || frames > draw::kMaxFrames) {
            failProblem(call, {"bad_op_field", "frames", "frames is 1-256", ""});
            return;
        }
        doc.frameDurations.assign(static_cast<size_t>(frames), static_cast<uint16_t>(duration));
        if (!paletteEntries(project, args, doc.mode == ase::ColorMode::Indexed, doc.palette, problem)) {
            failProblem(call, problem);
            return;
        }
    }
    if (doc.width < 1 || doc.height < 1 || doc.width > draw::kMaxEditSide || doc.height > draw::kMaxEditSide) {
        failProblem(call, {"canvas_too_large", "size", "the canvas would be " + std::to_string(doc.width) + "x" + std::to_string(doc.height), "each side is 1-4096"});
        return;
    }
    if (doc.frameCount() > draw::kMaxFrames) {
        failProblem(call, {"bad_op_field", "frames", "the source has " + std::to_string(doc.frameCount()) + " frames, above 256", "frames is 1-256"});
        return;
    }
    if (doc.mode == ase::ColorMode::Indexed && doc.palette.empty()) {
        failProblem(call, {"bad_color", "palette", "an indexed sprite needs a palette", "pass palette, or mode: \"rgba\""});
        return;
    }
    std::vector<uint8_t> bytes;
    ase::CodecError error;
    if (!ase::write(doc, bytes, &error)) {
        failProblem(call, error);
        return;
    }
    std::string why;
    const std::string stepName = "Create Sprite (" + absolute.filename().generic_string() + ")";
    auto afterChange = [project, assetPath](const std::vector<fs::path>& changed) {
        ProjectFileWrites::filesWritten(project, changed);
        std::error_code exists;
        if (fs::exists(changed.front(), exists)) ase::AsepriteImporter::get().reimport(assetPath);
    };
    if (!ProjectFileWrites::writeUndoable(project, {{absolute, std::string(bytes.begin(), bytes.end())}}, why, afterChange, nullptr, {}, false, stepName)) {
        failProblem(call, {"write_failed", "path", "couldn't write " + given + ": " + why, "check the folder is writable"});
        return;
    }
    // an overwrite replaced whatever the session remembered: not an outside save (R34.3)
    draw::SpriteDocCache::get().forget(absolute);
    std::shared_ptr<draw::SpriteSession> session = draw::SpriteDocCache::get().open(project, absolute.generic_string(), problem);
    if (!session) {
        failProblem(call, problem);
        return;
    }
    session->lastSeen[sessionKey(call)] = session->revision;
    call->ok("Created " + summaryOf(*session), spriteJson(project, *session));
}

Json frameArg(const std::string& description) {
    return Json{{"type", Json::array({"integer", "array", "string"})}, {"description", description}};
}

}

void registerSpriteDrawTools(McpRegistry& registry) {
    using namespace schema;
    {
        ToolDef tool;
        tool.name = "create_sprite";
        tool.title = "Create Sprite";
        tool.description = "A new sprite file (.aseprite) with a palette, frames and layers, or from a .png/GIF/APNG. One undo step removes it.";
        tool.inputSchema = object({
            {"path", assetPath("Project-relative .aseprite or .ase path.")},
            {"size", array(integer(""), "[w, h], 1-4096.")},
            {"mode", stringEnum("", {"indexed", "rgba"})},
            {"palette", Json{{"type", Json::array({"string", "array"})}, {"description", ".palette path, built-in name or hex list; default the project's Default Palette."}}},
            {"frames", withDefault(integer("", 1, 256), 1)},
            {"duration_ms", withDefault(integer("", 1, 65535), 100)},
            {"layers", array(string(""), "Layer names, bottom first.")},
            {"from", assetPath("A .png, .gif or APNG to take frames and colours from.")},
            {"overwrite", withDefault(boolean(""), false)},
        }, {"path"});
        tool.annotations.destructive = true;
        tool.activity = ToolActivity::Edit;
        tool.writesFiles = [](const Json&) { return true; };
        tool.group = "sprites";
        tool.handler = createSpriteTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "get_sprite";
        tool.title = "Get Sprite";
        tool.description = "A sprite file's (.aseprite, .png) structure: layers, frames from 1, tags, slices, palette, cels, revision; no pixels (read_sprite_pixels).";
        tool.inputSchema = object({{"sprite", assetPath("Project-relative path or GUID.")}}, {"sprite"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.activity = ToolActivity::Read;
        tool.group = "sprites";
        tool.handler = getSpriteTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "read_sprite_pixels";
        tool.title = "Read Sprite Pixels";
        tool.description = "A sprite file's region as a text grid ('.'=0, 1-9, a-z=10-35, A-Z=36-61); max_pixels caps it (default 4096, tile larger areas). render_sprite to look at the whole.";
        tool.inputSchema = object({
            {"sprite", assetPath("")},
            {"frame", frameArg("From 1, list, tag or \"all\"; default 1; at most 16.")},
            {"layer", Json{{"type", Json::array({"integer", "string"})}, {"description", "Default \"composite\"."}}},
            {"rect", array(integer(""), "[x, y, w, h].")},
            {"encoding", stringEnum("", {"auto", "chars", "hex2", "legend", "hex8"})},
            {"max_pixels", withDefault(integer("", 1, 16384), 4096)},
        }, {"sprite"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.activity = ToolActivity::Read;
        tool.group = "sprites";
        tool.handler = readSpritePixelsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "edit_sprite";
        tool.title = "Edit Sprite";
        tool.description = "An atomic batch of drawing ops on a sprite file (.aseprite, .png), not a Sprite component: one undo step, an echo of what changed. Each op's fields are in jasper://docs/sprites; a wrong field fails with bad_op_field before anything changes.";
        tool.inputSchema = object({
            {"sprite", assetPath("")},
            {"ops", [] {
                Json items = object({{"op", stringEnum("", draw::opNames())}}, {"op"}, true);
                Json a = array(items, "1-256 ops, run in order.");
                a["minItems"] = 1;
                a["maxItems"] = draw::kMaxOps;
                return a;
            }()},
            {"frame", frameArg("Default for ops naming none (default 1, frames from 1).")},
            {"layer", Json{{"type", Json::array({"integer", "string"})}, {"description", "Default: the topmost visible pixel layer."}}},
            {"symmetry", object({{"x", any("")}, {"y", any("")}, {"diagonal", stringEnum("", {"xy", "x_minus_y", "both"})}})},
            {"echo", withDefault(stringEnum("", {"dirty", "composite", "none"}), "dirty")},
            {"echo_max", withDefault(integer("", 0, 4096), 1024)},
            {"image", withDefault(boolean("Also render the touched frames."), false)},
            {"lint", any("true or a list of lint ids.")},
            {"expect_revision", string("Fail unless this is the file's revision.")},
        }, {"sprite", "ops"});
        tool.annotations.destructive = true;
        tool.activity = ToolActivity::Edit;
        tool.writesFiles = [](const Json&) { return true; };
        tool.group = "sprites";
        tool.handler = editSpriteTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "render_sprite";
        tool.title = "Render Sprite";
        tool.description = "A sprite file's frames as one PNG at integer scale (nearest), a row per backdrop, with onion skin, labels and the last edit outlined. Scale 0 fits max_width.";
        tool.inputSchema = object({
            {"sprite", assetPath("")},
            {"frames", frameArg("Default all (at most 64).")},
            {"layers", Json{{"type", Json::array({"string", "array"})}, {"description", "visible (default), all, or names."}}},
            {"scale", withDefault(integer("", 0, 64), 0)},
            {"columns", integer("Default auto, at most 8.", 0, 64)},
            {"backdrops", array(string(""), "checker (default), dark, light, or hex; one row each.")},
            {"onion", object({{"before", integer("", 0, 3)}, {"after", integer("", 0, 3)}, {"opacity", number("", 0.1, 0.9)}})},
            {"grid", withDefault(boolean("Pixel grid at scale 6 and up."), false)},
            {"labels", withDefault(boolean(""), true)},
            {"highlight", any("last_edit, none (default) or [x, y, w, h].")},
            {"max_width", withDefault(integer("", 128, 2048), 1024)},
            {"context", object({{"entity", entityRef()}, {"view", stringEnum("", {"game", "scene"})}}, {"entity"})},
        }, {"sprite"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.activity = ToolActivity::Read;
        tool.group = "sprites";
        tool.handler = renderSpriteTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "diff_sprite";
        tool.title = "Diff Sprite";
        tool.description = "What changed in a sprite file: against the previous edit_sprite, a revision, another file or another frame. Its rows paste back as a patch.";
        tool.inputSchema = object({
            {"sprite", assetPath("")},
            {"against", anyOf("Default \"previous\".", {string("\"previous\" or a revision"), object({{"sprite", assetPath("")}, {"frame", integer("From 1.")}})})},
            {"frames", frameArg("Default all.")},
            {"layer", Json{{"type", Json::array({"integer", "string"})}, {"description", "Default \"composite\"."}}},
            {"rect", array(integer(""), "[x, y, w, h].")},
            {"max_cells", withDefault(integer("", 0, 1024), 64)},
            {"grid", withDefault(boolean(""), true)},
            {"image", withDefault(boolean(""), false)},
        }, {"sprite"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.activity = ToolActivity::Read;
        tool.group = "sprites";
        tool.handler = diffSpriteTool;
        registry.addTool(std::move(tool));
    }
}

}
