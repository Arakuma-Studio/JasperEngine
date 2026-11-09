// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PaletteSwapChecks.h"

#include "Project.h"
#include "pixelart/PaletteMaterial.h"
#include "pixelart/PaletteSwap.h"
#include "pixelart/PaletteUsage.h"
#include "shader/ShaderManifest.h"
#include "texture/TextureCookQueue.h"
#include "texture/TextureFormat.h"
#include "texture/TextureImportSettings.h"
#include "util/FileUtils.h"

#include "stb_image.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>

namespace fs = std::filesystem;

// file-local, in a namespace of its own: a unity build merges the editor's sources
namespace jasperengine::editor::pixelart::paletteswapchecks {
namespace {

    namespace pxc = jasperengine::pixelart;

    std::string effectiveShader(const Material& material, const std::string& shader) {
        return shader.empty() ? material.shader.path : shader;
    }

    // the project's families the editor shows (the Texture Importer's tabs)
    std::vector<TextureFamily> shownFamilies(const Project& project) {
        std::vector<TextureFamily> families = {TextureFamily::Desktop, TextureFamily::Mobile, TextureFamily::Web};
        if (project.getTextureSettings().androidEtc2Fallback) families.push_back(TextureFamily::AndroidEtc2);
        return families;
    }

    // the stored settings, or what the cook infers when there are none
    TextureImportSettings importSettingsOf(Project& project, const std::string& assetPath) {
        TextureImportSettings settings;
        TextureCookQueue& queue = TextureCookQueue::get();
        if (queue.getProject() == &project && queue.settingsOf(assetPath, settings)) return settings;
        TextureCookInputs inputs;
        if (gatherTextureInputs(project, assetPath, inputs)) return inputs.settings;
        return settings;
    }

    // an explicit cook format that keeps every 8-bit RGBA texel
    bool losslessFormat(const std::string& format) {
        GpuTextureFormat parsed;
        return gpuFormatFromName(format, parsed) && parsed == GpuTextureFormat::RGBA8;
    }

    bool compressedFor(const ResolvedTextureSettings& resolved) {
        if (!resolved.format.empty()) return !losslessFormat(resolved.format);
        return resolved.quality != TextureCookQuality::None;
    }

    bool powerOfTwo(int value) {
        return value > 0 && (value & (value - 1)) == 0;
    }

    // the smallest Max Size (a power of two, 32...16384) that keeps a side of `side` pixels
    int maxSizeKeeping(int side) {
        int size = 32;
        while (size < side && size < 16384) size *= 2;
        return size;
    }

    bool filteredSampler(const Texture& texture) {
        return texture.getMinFilter() != TextureFilter::NEAREST || texture.getMagFilter() != TextureFilter::NEAREST;
    }

    // "Mobile, Web" when only some families, "" when all of them
    std::string familyList(const std::vector<TextureFamily>& hit, size_t shown) {
        if (hit.size() == shown) return std::string();
        std::string text;
        for (TextureFamily family : hit) text += (text.empty() ? "" : ", ") + std::string(textureFamilyLabel(family));
        return " (" + text + ")";
    }

    bool readPaletteFile(const fs::path& file, pxc::Palette& out) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<pxc::Diagnostic> diagnostics;
        return pxc::PaletteCodec::read(text, out, diagnostics);
    }

}
}

using namespace jasperengine::editor::pixelart::paletteswapchecks;

namespace jasperengine::editor::pixelart {

bool paletteSwapEnabled(const Material& material, const std::string& shader) {
    const std::string reference = effectiveShader(material, shader);
    std::shared_ptr<const ShaderManifest> manifest = reference.empty() ? nullptr : ShaderManifests::get(reference);
    if (!manifest || !manifest->present || manifest->palettes.empty()) return false;
    const std::vector<std::string> enabled = manifest->resolveFeatures(material.shaderFeatures);
    return std::any_of(manifest->palettes.begin(), manifest->palettes.end(),
                       [&](const ShaderManifestPalette& slot) { return slot.isEnabled(enabled); });
}

std::string PaletteSwapTextureCheck::message() const {
    if (reasons.empty()) return std::string();
    std::string text = subject + " needs lossless textures: " + FileUtils::pathToUtf8(FileUtils::pathFromUtf8(texture).filename()) + " ";
    for (size_t i = 0; i < reasons.size(); i++) text += (i ? ", " : "") + reasons[i];
    return text;
}

PaletteSwapTextureCheck checkPaletteSwapTexture(Project& project, const Material& material, const std::string& shader, int width, int height) {
    PaletteSwapTextureCheck check;
    if (!paletteSwapEnabled(material, shader)) return check;
    const Texture& texture = material.baseColorTexture;
    if (texture.getPath().empty() || texture.isFramebuffer()) return check;
    check = checkLosslessTexture(project, texture.getPath(), width, height);
    if (filteredSampler(texture)) check.reasons.push_back("is filtered (set its Filter to Nearest)");
    return check;
}

PaletteSwapTextureCheck checkLosslessTexture(Project& project, const std::string& texture, int width, int height) {
    PaletteSwapTextureCheck check;
    check.texture = texture;
    if (width <= 0 || height <= 0) {
        int channels = 0;
        const fs::path absolute = project.getAssetsPath() / FileUtils::pathFromUtf8(check.texture);
        if (!stbi_info(FileUtils::pathToUtf8(absolute).c_str(), &width, &height, &channels)) width = height = 0;
    }
    check.width = width;
    check.height = height;

    const TextureImportSettings settings = importSettingsOf(project, check.texture);
    const TextureProjectSettings projectSettings = textureProjectSettingsOf(project);
    const std::vector<TextureFamily> families = shownFamilies(project);
    std::vector<TextureFamily> compressed, mipmapped, resized;
    int smallestMax = 0;
    bool strategyResize = false;
    for (TextureFamily family : families) {
        const ResolvedTextureSettings resolved = settings.resolve(family, projectSettings);
        if (compressedFor(resolved)) compressed.push_back(family);
        if (resolved.mipmaps) mipmapped.push_back(family);
        const int side = std::max(width, height);
        if (side > 0 && resolved.maxSize < side) {
            resized.push_back(family);
            smallestMax = smallestMax == 0 ? resolved.maxSize : std::min(smallestMax, resolved.maxSize);
        }
        if (resolved.strategy == TextureSizeStrategy::Resize && width > 0 && (!powerOfTwo(width) || !powerOfTwo(height)))
            strategyResize = true;
    }
    if (!compressed.empty()) check.reasons.push_back("is compressed" + familyList(compressed, families.size()));
    if (!mipmapped.empty()) check.reasons.push_back("has mipmaps" + familyList(mipmapped, families.size()));
    if (!resized.empty())
        check.reasons.push_back("is resized to Max Size " + std::to_string(smallestMax) + familyList(resized, families.size()) +
                                " (the image is " + std::to_string(width) + "x" + std::to_string(height) + ")");
    if (strategyResize) {
        check.projectResize = true;
        check.reasons.push_back("is resized to a power of two (Project Settings > Graphics > Texture Strategy)");
    }
    return check;
}

std::string paletteSwapLossyTexture(Project& project, const Material& material, const std::string& shader) {
    return checkPaletteSwapTexture(project, material, shader).message();
}

bool planPaletteSwapTextureFix(Project& project, const Material& material, const PaletteSwapTextureCheck& check, PaletteSwapTextureFix& fix) {
    fix = PaletteSwapTextureFix();
    if (check.texture.empty()) return false;
    fix.texture = check.texture;

    TextureImportSettings settings = importSettingsOf(project, check.texture);
    const TextureImportSettings before = settings;
    const int side = std::max(check.width, check.height);
    settings.quality = TextureCookQuality::None;
    settings.mipmaps = false;
    if (side > 0 && settings.maxSize.value_or(0) < side) {
        const TextureProjectSettings projectSettings = textureProjectSettingsOf(project);
        bool small = false;
        for (TextureFamily family : shownFamilies(project))
            small = small || settings.resolve(family, projectSettings).maxSize < side;
        if (small) settings.maxSize = maxSizeKeeping(side);
    }
    for (TexturePlatformSettings& platform : settings.platforms) {
        if (!platform.override) continue;
        if (platform.format && !losslessFormat(*platform.format)) platform.format.reset();
        if (platform.quality && *platform.quality != TextureCookQuality::None) platform.quality = TextureCookQuality::None;
        if (platform.maxSize && side > 0 && *platform.maxSize < side) platform.maxSize = maxSizeKeeping(side);
        platform.rdo.reset();
    }
    if (!(settings == before)) {
        fix.importChanged = true;
        fix.importSettings = settings.serialize();
    }

    fix.sampler = material.baseColorTexture;
    if (filteredSampler(fix.sampler)) {
        fix.sampler.setMinFilter(TextureFilter::NEAREST);
        fix.sampler.setMagFilter(TextureFilter::NEAREST);
        fix.samplerChanged = true;
    }
    return fix.importChanged || fix.samplerChanged;
}

int64_t paletteSwapOffPaletteColours(const RgbaImage& image, const jasperengine::pixelart::Palette& source) {
    return measureUsage(image, source.colors, 1, 0).offColorCount;
}

int64_t paletteSwapOffPaletteColours(Project& project, const Material& material) {
    const jasperengine::pixelart::PaletteBinding* binding = jasperengine::pixelart::findPaletteBinding(material);
    const std::string texture = material.baseColorTexture.getPath();
    if (!binding || binding->source.empty() || texture.empty()) return -1;
    jasperengine::pixelart::Palette source;
    if (!readPaletteFile(project.getAssetsPath() / FileUtils::pathFromUtf8(binding->source.path), source)) return -1;
    std::optional<RgbaImage> image = loadRgbaImage(project.getAssetsPath() / FileUtils::pathFromUtf8(texture));
    if (!image) return -1;
    return paletteSwapOffPaletteColours(*image, source);
}

std::vector<std::string> palettesCoveringImage(const RgbaImage& image, const std::vector<ProjectPalette>& palettes) {
    std::vector<std::string> covering;
    for (const ProjectPalette& entry : palettes) {
        if (entry.palette.colors.empty()) continue;
        const PaletteUsageReport report = measureUsage(image, entry.palette.colors, 1, 0);
        int64_t counted = 0;
        for (int64_t count : report.counts) counted += count;
        if (report.offPalettePixels == 0 && counted > 0) covering.push_back(entry.assetPath);
    }
    return covering;
}

std::vector<jasperengine::pixelart::Diagnostic> paletteSwapDiagnostics(const jasperengine::pixelart::PaletteBinding& binding) {
    namespace pxc = jasperengine::pixelart;
    std::vector<pxc::Diagnostic> diagnostics;
    if (binding.source.empty()) return diagnostics;
    std::shared_ptr<const pxc::Palette> source = pxc::PaletteLibrary::get(binding.source.path, &diagnostics);
    if (!source) return diagnostics;
    const std::vector<pxc::PaletteSwapRow> rows = pxc::resolvePaletteRows(*source, binding, diagnostics);
    pxc::PaletteSwapData data;
    pxc::buildPaletteSwapData(*source, rows, data, diagnostics);
    return diagnostics;
}

}
