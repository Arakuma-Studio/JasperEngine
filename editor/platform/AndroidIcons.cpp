// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AndroidIcons.h"

#include "io/AtomicFile.h"
#include "util/FileUtils.h"
#include "util/PngWriter.h"

#include "stb_image.h"
#include "stb_image_resize2.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::platform;
namespace fs = std::filesystem;

namespace {

    struct AndroidIconImage {
        int size = 0;
        std::vector<unsigned char> rgba;
    };

    bool iconLoad(const fs::path& projectPath, const fs::path& path, AndroidIconImage& image, int& width, int& height, std::string& error) {
        const fs::path file = path.is_absolute() ? path : projectPath / path;
        int channels = 0;
        unsigned char* pixels = stbi_load(FileUtils::pathToUtf8(file).c_str(), &width, &height, &channels, 4);
        if (!pixels) {
            error = "Android icon could not be loaded: " + FileUtils::pathToUtf8(file) + " (Project Settings > Android > Launcher Icons)";
            return false;
        }
        image.rgba.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
        stbi_image_free(pixels);
        return true;
    }

    // the image at size x size (a non-square one is stretched, as Unity's icon slots do)
    AndroidIconImage iconResized(const std::vector<unsigned char>& rgba, int width, int height, int size) {
        AndroidIconImage out;
        out.size = size;
        out.rgba.resize(static_cast<size_t>(size) * size * 4);
        if (width == size && height == size) out.rgba = rgba;
        else stbir_resize_uint8_srgb(rgba.data(), width, height, 0, out.rgba.data(), size, size, 0, STBIR_RGBA);
        return out;
    }

    // `top` over `bottom`, both size x size, non-premultiplied
    AndroidIconImage iconComposed(const AndroidIconImage& bottom, const AndroidIconImage& top) {
        AndroidIconImage out = bottom;
        for (size_t i = 0; i < out.rgba.size(); i += 4) {
            const float ta = top.rgba[i + 3] / 255.0f, ba = bottom.rgba[i + 3] / 255.0f;
            const float a = ta + ba * (1.0f - ta);
            for (int c = 0; c < 3; c++) {
                const float v = a > 0.0f ? (top.rgba[i + c] * ta + bottom.rgba[i + c] * ba * (1.0f - ta)) / a : 0.0f;
                out.rgba[i + c] = static_cast<unsigned char>(std::clamp(v + 0.5f, 0.0f, 255.0f));
            }
            out.rgba[i + 3] = static_cast<unsigned char>(std::clamp(a * 255.0f + 0.5f, 0.0f, 255.0f));
        }
        return out;
    }

    // the circle a round launcher shows: alpha outside it 0, a one-pixel antialiased edge
    AndroidIconImage iconRounded(AndroidIconImage image) {
        const float radius = image.size * 0.5f;
        for (int y = 0; y < image.size; y++) {
            for (int x = 0; x < image.size; x++) {
                const float dx = x + 0.5f - radius, dy = y + 0.5f - radius;
                const float coverage = std::clamp(radius - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
                unsigned char& alpha = image.rgba[(static_cast<size_t>(y) * image.size + x) * 4 + 3];
                alpha = static_cast<unsigned char>(alpha * coverage + 0.5f);
            }
        }
        return image;
    }

    bool iconWrite(const fs::path& resDir, const std::string& relative, const AndroidIconImage& image, AndroidIcons& icons, std::string& error) {
        const fs::path file = resDir / FileUtils::pathFromUtf8(relative);
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        if (!writePng(file, image.size, image.size, 4, image.rgba.data(), image.size * 4, io::Durability::Cache)) {
            error = "cannot write the Android icon " + FileUtils::pathToUtf8(file);
            return false;
        }
        icons.files.push_back(relative);
        return true;
    }

    bool iconWriteText(const fs::path& resDir, const std::string& relative, const std::string& text, AndroidIcons& icons, std::string& error) {
        const fs::path file = resDir / FileUtils::pathFromUtf8(relative);
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        if (!io::writeFileAtomic(file, text, {io::Durability::Cache}).ok()) {
            error = "cannot write " + FileUtils::pathToUtf8(file);
            return false;
        }
        icons.files.push_back(relative);
        return true;
    }

}

const std::vector<AndroidIconDensity>& platform::androidIconDensities() {
    static const std::vector<AndroidIconDensity> densities = {
        {"mipmap-mdpi", 48, 108}, {"mipmap-hdpi", 72, 162}, {"mipmap-xhdpi", 96, 216}, {"mipmap-xxhdpi", 144, 324}, {"mipmap-xxxhdpi", 192, 432}};
    return densities;
}

bool platform::writeAndroidLauncherIcons(const AndroidProjectSettings& settings, const fs::path& projectPath, const fs::path& resDir,
                                         const std::string& name, AndroidIcons& icons, std::string& error) {
    icons = AndroidIcons();
    const bool launcher = !settings.launcherIcon.empty();
    const bool adaptive = !settings.adaptiveIconForeground.empty() && !settings.adaptiveIconBackground.empty();
    std::error_code ec;
    if (!adaptive) {
        for (const std::string& xml : {name + ".xml", name + "_round.xml"}) fs::remove(resDir / "mipmap-anydpi-v26" / xml, ec);
    }
    if (!launcher && !adaptive) return true;

    // the adaptive layers at the largest density; the others are resampled from the source
    AndroidIconImage foreground, background;
    std::vector<unsigned char> foregroundSource, backgroundSource;
    int foregroundW = 0, foregroundH = 0, backgroundW = 0, backgroundH = 0;
    if (adaptive) {
        AndroidIconImage loaded;
        if (!iconLoad(projectPath, settings.adaptiveIconForeground, loaded, foregroundW, foregroundH, error)) return false;
        foregroundSource = std::move(loaded.rgba);
        if (!iconLoad(projectPath, settings.adaptiveIconBackground, loaded, backgroundW, backgroundH, error)) return false;
        backgroundSource = std::move(loaded.rgba);
    }
    std::vector<unsigned char> legacySource;
    int legacyW = 0, legacyH = 0;
    if (launcher) {
        AndroidIconImage loaded;
        if (!iconLoad(projectPath, settings.launcherIcon, loaded, legacyW, legacyH, error)) return false;
        legacySource = std::move(loaded.rgba);
    } else {
        // no Launcher Icon: devices before Android 8 show the adaptive layers composed, the 72 dp a
        // launcher's mask shows of the 108 dp
        const int full = androidIconDensities().back().adaptivePx;
        const AndroidIconImage whole = iconComposed(iconResized(backgroundSource, backgroundW, backgroundH, full),
                                         iconResized(foregroundSource, foregroundW, foregroundH, full));
        const int visible = full * 72 / 108, offset = (full - visible) / 2;
        legacySource.resize(static_cast<size_t>(visible) * visible * 4);
        for (int y = 0; y < visible; y++)
            std::copy_n(whole.rgba.begin() + (static_cast<size_t>(y + offset) * full + offset) * 4, static_cast<size_t>(visible) * 4,
                        legacySource.begin() + static_cast<size_t>(y) * visible * 4);
        legacyW = legacyH = visible;
    }

    for (const AndroidIconDensity& density : androidIconDensities()) {
        const std::string folder = std::string(density.folder) + "/";
        const AndroidIconImage legacy = iconResized(legacySource, legacyW, legacyH, density.legacyPx);
        if (!iconWrite(resDir, folder + name + ".png", legacy, icons, error)) return false;
        if (!iconWrite(resDir, folder + name + "_round.png", iconRounded(legacy), icons, error)) return false;
        if (adaptive) {
            if (!iconWrite(resDir, folder + name + "_foreground.png", iconResized(foregroundSource, foregroundW, foregroundH, density.adaptivePx), icons, error))
                return false;
            if (!iconWrite(resDir, folder + name + "_background.png", iconResized(backgroundSource, backgroundW, backgroundH, density.adaptivePx), icons, error))
                return false;
        }
    }
    if (adaptive) {
        const std::string xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                "<adaptive-icon xmlns:android=\"http://schemas.android.com/apk/res/android\">\n"
                                "    <background android:drawable=\"@mipmap/" + name + "_background\" />\n"
                                "    <foreground android:drawable=\"@mipmap/" + name + "_foreground\" />\n"
                                "</adaptive-icon>\n";
        // a launcher that masks icons round reads the round entry: the same layers
        if (!iconWriteText(resDir, "mipmap-anydpi-v26/" + name + ".xml", xml, icons, error)) return false;
        if (!iconWriteText(resDir, "mipmap-anydpi-v26/" + name + "_round.xml", xml, icons, error)) return false;
    }
    icons.icon = "@mipmap/" + name;
    icons.roundIcon = "@mipmap/" + name + "_round";
    return true;
}
