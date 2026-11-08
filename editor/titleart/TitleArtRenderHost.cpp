// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtRenderHost.h"

#include "Project.h"
#include "asset/Guid.h"
#include "asset/AssetFileWrite.h"
#include "render/FrameReadback.h"
#include "render/SystemRender.h"
#include "texture/TexturePrepare.h"
#include "titleart/TitleArtHash.h"
#include "titleart/render/TitleGpu.h"
#include "util/AssetDatabase.h"
#include "util/FontFace.h"
#include "util/FontRegistry.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    AssetType expectedType(titleart::TitleAssetKind kind) {
        return kind == titleart::TitleAssetKind::Font ? AssetType::Font : AssetType::Texture;
    }

    uint64_t fileRevision(const fs::path& path) {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        if (ec) return 0;
        const auto time = fs::last_write_time(path, ec);
        if (ec) return 0;
        const std::string stamp = std::to_string(size) + ":" + std::to_string(time.time_since_epoch().count());
        return titleart::fnv1a64(stamp);
    }

    uint8_t toByte(float v) {
        return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    }
    uint16_t toWord(float v) {
        return static_cast<uint16_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f));
    }

}

// ==================================================================== resolver

EditorTitleAssetResolver::EditorTitleAssetResolver(const AssetDatabase* database, fs::path projectPath, fs::path assetsPath)
    : database(database), project(std::move(projectPath)), assets(std::move(assetsPath)) {
    const fs::path relative = assets.lexically_relative(project);
    const std::string prefix = relative.generic_string();
    assetsPrefix = (prefix.empty() || prefix == ".") ? std::string() : prefix + "/";
}

std::string EditorTitleAssetResolver::projectRelative(const std::string& assetsRelative) const {
    return assetsPrefix + assetsRelative;
}

titleart::ResolvedTitleAsset EditorTitleAssetResolver::resolve(const titleart::TitleAssetRef& ref, titleart::TitleAssetKind kind) const {
    titleart::ResolvedTitleAsset out;
    if (ref.empty()) return out;
    std::lock_guard<std::mutex> guard(lock);
    std::string projectPathOfRef;
    if (database) {
        const AssetDatabase::Resolution resolution =
            database->resolve(Guid::parse(ref.guid), ref.path.empty() ? std::string() : projectRelative(ref.path), expectedType(kind), true);
        if (resolution.found() && resolution.path.find('#') == std::string::npos) {
            projectPathOfRef = resolution.path;
            out.guid = resolution.guid.str();
        }
    } else if (!ref.path.empty()) {
        projectPathOfRef = projectRelative(ref.path);
        out.guid = ref.guid;
    }
    if (projectPathOfRef.empty()) return out;
    const fs::path absolute = project / fs::path(projectPathOfRef);
    std::error_code ec;
    if (!fs::is_regular_file(absolute, ec)) return out;
    out.found = true;
    out.path = absolute.string();
    const fs::path assetsRelative = absolute.lexically_relative(assets);
    out.assetPath = assetsRelative.empty() ? projectPathOfRef : assetsRelative.generic_string();
    out.revision = fileRevision(absolute);
    return out;
}

std::shared_ptr<FontFace> EditorTitleAssetResolver::loadFont(const titleart::ResolvedTitleAsset& asset, int faceIndex, std::string* error) const {
    if (!asset.found) {
        if (error) *error = "the font is missing";
        return nullptr;
    }
    std::lock_guard<std::mutex> guard(lock);
    // keyed by the content revision too: an edited font file loads again
    const auto key = std::make_pair(asset.path + "@" + std::to_string(asset.revision), faceIndex);
    auto it = fonts.find(key);
    if (it != fonts.end()) return it->second;
    // Title Art draws outlines through HarfBuzz, so a face stb_truetype cannot rasterize (CFF2) opens
    std::shared_ptr<FontFace> face = FontFace::loadFile(asset.path, faceIndex, error, FontFaceRasterizer::Optional);
    if (face) fonts[key] = face;
    return face;
}

bool EditorTitleAssetResolver::readBytes(const titleart::ResolvedTitleAsset& asset, std::vector<uint8_t>& out, std::string* error) const {
    if (!asset.found) {
        if (error) *error = "the file is missing";
        return false;
    }
    std::string text;
    if (!assetio::readFile(fs::path(asset.path), text)) {
        if (error) *error = "cannot read " + asset.assetPath;
        return false;
    }
    out.assign(text.begin(), text.end());
    return true;
}

bool EditorTitleAssetResolver::loadImage(const titleart::ResolvedTitleAsset& asset, titleart::TitleImageData& out, std::string* error) const {
    if (!asset.found) {
        if (error) *error = "the image is missing";
        return false;
    }
    texsynth::Image image;
    std::string message;
    if (!texsynth::loadImage(fs::path(asset.path), image, message)) {
        if (error) *error = asset.assetPath + ": " + message;
        return false;
    }
    const texsynth::Image rgba = texsynth::toRgba8(image);
    out.width = rgba.width;
    out.height = rgba.height;
    out.rgba = rgba.data;
    return true;
}

std::shared_ptr<FontFace> EditorTitleAssetResolver::builtinFont() const {
    return FontRegistry::getBuiltInLatin();
}

std::vector<titleart::ResolvedTitleAsset> EditorTitleAssetResolver::projectFonts() const {
    return projectAssets(titleart::TitleAssetKind::Font);
}

std::vector<titleart::ResolvedTitleAsset> EditorTitleAssetResolver::projectAssets(titleart::TitleAssetKind kind) const {
    std::vector<titleart::ResolvedTitleAsset> out;
    if (!database) return out;
    const AssetType wanted = expectedType(kind);
    std::vector<std::pair<std::string, std::string>> found;
    {
        std::lock_guard<std::mutex> guard(lock);
        database->forEachAsset([&](const AssetDatabase::AssetInfo& info) {
            if (info.type == wanted) found.push_back({info.path, info.guid.str()});
        });
    }
    std::sort(found.begin(), found.end());
    for (const auto& [path, guid] : found) {
        titleart::ResolvedTitleAsset asset;
        const fs::path absolute = project / fs::path(path);
        asset.found = true;
        asset.path = absolute.string();
        asset.guid = guid;
        asset.assetPath = absolute.lexically_relative(assets).generic_string();
        asset.revision = fileRevision(absolute);
        out.push_back(std::move(asset));
    }
    return out;
}

// ==================================================================== host

struct TitleArtRenderHost::Backend {
    titleart::TitleArtRenderer renderer;
};

TitleArtRenderHost::TitleArtRenderHost() = default;
TitleArtRenderHost::~TitleArtRenderHost() = default;

TitleArtRenderHost& TitleArtRenderHost::editor() {
    static TitleArtRenderHost host;
    return host;
}

titleart::TitleArtRenderer& TitleArtRenderHost::rendererInstance() {
    if (!backend) backend = std::make_unique<Backend>();
    return backend->renderer;
}

const EditorTitleAssetResolver* TitleArtRenderHost::resolver(Project* project) {
    if (!project) return nullptr;
    const fs::path path = project->getProjectPath();
    if (!projectResolver || resolverProject != project || resolverPath != path) {
        projectResolver = std::make_unique<EditorTitleAssetResolver>(&project->getAssetDatabase(), path, project->getAssetsPath());
        resolverProject = project;
        resolverPath = path;
    }
    return projectResolver.get();
}

std::unique_ptr<titleart::TitleRenderJob> TitleArtRenderHost::begin(Project* project, titleart::RenderRequest request, std::string& error) {
    if (!request.doc) {
        error = "the request has no document";
        return nullptr;
    }
    if (!request.assets) request.assets = resolver(project);
    std::unique_ptr<titleart::TitleRenderJob> job = rendererInstance().begin(request);
    if (!job) error = "the renderer refused the request";
    return job;
}

std::unique_ptr<titleart::TitleRenderJob> TitleArtRenderHost::runToEnd(Project* project, titleart::RenderRequest request, std::string& error) {
    std::unique_ptr<titleart::TitleRenderJob> job = begin(project, std::move(request), error);
    if (!job) return nullptr;
    // no editor frame runs while this call does, so a program the job waits for (built in the
    // background, created off the render thread) is published here, within a deadline, and the
    // first draws are not spread over frames (S86's TitleFirstDraws)
    titleart::TitleFirstDraws::SynchronousScope synchronous;
    constexpr double PROGRAM_WAIT_MS = 60000.0;
    const auto waitStart = std::chrono::steady_clock::now();
    while (!job->done()) {
        if (job->step(titleart::StepBudget{})) break;
        if (job->failed()) break;
        if (!job->waiting()) continue;
        if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count() > PROGRAM_WAIT_MS) {
            error = "a title art program was not built within " + std::to_string(static_cast<int>(PROGRAM_WAIT_MS / 1000.0)) + " s";
            return nullptr;
        }
        SystemRender::waitGpuCreationIdle(5.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return job;
}

bool TitleArtRenderHost::render(Project* project, titleart::RenderRequest request, TitleRenderedImage& out, std::string& error) {
    const auto start = std::chrono::steady_clock::now();
    const int bits = request.outputBitDepth == 16 ? 16 : 8;
    const float scale = request.pxScale;
    const titleart::TitleRectI asked = request.region.empty() && request.doc
        ? titleart::TitleRectI{0, 0, request.doc->canvasWidth(), request.doc->canvasHeight()} : request.region;
    std::unique_ptr<titleart::TitleRenderJob> job = runToEnd(project, std::move(request), error);
    if (!job) return false;
    if (job->failed() || !job->done()) {
        error = job->error().empty() ? std::string("the render did not finish") : job->error();
        out.problems = job->problems();
        return false;
    }
    if (!readBack(*job, bits, out, error)) return false;
    // a renderer that leaves the frame unset rendered what was asked
    if (out.frame.empty()) out.frame = asked;
    out.pxScale = scale;
    out.renderMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (const titleart::RendererStats* counters = stats()) out.pathUsed = counters->path ? counters->path : "";
    return true;
}

titleart::TitleRectI TitleArtRenderHost::alphaBoundsOf(const texsynth::Image& image) {
    titleart::TitleRectI bounds;
    if (image.empty() || image.channels != 4) return bounds;
    int minX = image.width, minY = image.height, maxX = -1, maxY = -1;
    for (int y = 0; y < image.height; y++) {
        for (int x = 0; x < image.width; x++) {
            const uint8_t* p = image.pixel(x, y);
            const bool visible = image.bitDepth == 16 ? reinterpret_cast<const uint16_t*>(p)[3] >= 257 : p[3] >= 1;
            if (!visible) continue;
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
    }
    if (maxX < 0) return bounds;
    bounds.x = minX;
    bounds.y = minY;
    bounds.width = maxX - minX + 1;
    bounds.height = maxY - minY + 1;
    return bounds;
}

bool TitleArtRenderHost::readBack(const titleart::TitleRenderJob& job, int bitDepth, TitleRenderedImage& out, std::string& error) {
    if (!readImage(job.image(), bitDepth, out, error)) return false;
    out.problems = job.problems();
    out.passes = job.passesDone();
    return true;
}

bool TitleArtRenderHost::readImage(const titleart::TitleImage& image, int bitDepth, TitleRenderedImage& out, std::string& error) {
    titleart::TitleTarget* target = image.target.get();
    TextureRender* texture = target ? target->colorTexture() : nullptr;
    if (!texture || image.width <= 0 || image.height <= 0) {
        error = "the render has no image to read";
        return false;
    }
    const bool floatImage = image.format == ColorFormat::RGBA16F;
    if (!floatImage && image.format != ColorFormat::RGBA && image.format != ColorFormat::SRGB8A8) {
        error = "the render's image format cannot be read back";
        return false;
    }
    if (bitDepth == 16 && !floatImage) {
        error = "bit_depth_unsupported: a 16-bit output needs an RGBA16F image and this device renders RGBA8 only";
        return false;
    }
    const int bytesPerPixel = floatImage ? 8 : 4;
    std::vector<uint8_t> pixels;
    std::string readError;
    if (!FrameReadback::readTexture(*texture, image.width, image.height, bytesPerPixel, pixels, &readError)) {
        error = "readback failed: " + readError;
        return false;
    }
    const size_t count = static_cast<size_t>(image.width) * image.height;
    out.image = texsynth::Image::make(image.width, image.height, 4, bitDepth == 16 ? 16 : 8);
    for (size_t i = 0; i < count; i++) {
        float c[4];
        if (floatImage) {
            const uint16_t* h = reinterpret_cast<const uint16_t*>(pixels.data() + i * 8);
            for (int k = 0; k < 4; k++) c[k] = halfToFloat(h[k]);
        } else {
            for (int k = 0; k < 4; k++) c[k] = pixels[i * 4 + k] / 255.0f;
        }
        if (image.premultiplied) {
            // a linear premultiplied intermediate: straight, then sRGB-encoded like an output
            const float a = c[3];
            for (int k = 0; k < 3; k++) c[k] = a > 0.0f ? titleart::linearToSrgb(std::clamp(c[k] / a, 0.0f, 1.0f)) : 0.0f;
        }
        if (out.image.bitDepth == 16) {
            uint16_t* dst = reinterpret_cast<uint16_t*>(out.image.data.data()) + i * 4;
            for (int k = 0; k < 4; k++) dst[k] = toWord(c[k]);
        } else if (floatImage || image.premultiplied) {
            for (int k = 0; k < 4; k++) out.image.data[i * 4 + k] = toByte(c[k]);
        } else {
            // exact codes stay exact (the output pass's dither already happened)
            for (int k = 0; k < 4; k++) out.image.data[i * 4 + k] = pixels[i * 4 + k];
        }
    }
    out.frame = image.frame;
    out.alphaBounds = alphaBoundsOf(out.image);
    return true;
}

const titleart::RendererStats* TitleArtRenderHost::stats() const {
    return backend ? &backend->renderer.stats() : nullptr;
}

void TitleArtRenderHost::trim() {
    if (backend) backend->renderer.trim();
}

void TitleArtRenderHost::reset() {
    projectResolver.reset();
    resolverProject = nullptr;
    resolverPath.clear();
    trim();
}
