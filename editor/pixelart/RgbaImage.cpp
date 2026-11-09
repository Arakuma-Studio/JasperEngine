// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/RgbaImage.h"

#include "util/PngWriter.h"

#include "stb_image.h"
#include "stb_image_write.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

namespace fs = std::filesystem;

static_assert(sizeof(jasperengine::pixelart::Rgba8) == 4, "RgbaImage hands its pixels to stb as tightly packed RGBA8");

namespace jasperengine::editor::pixelart {

    std::optional<RgbaImage> decodeRgbaImage(const void* bytes, size_t size, std::string* error) {
        if (!bytes || size == 0) {
            if (error) *error = "the file is empty";
            return std::nullopt;
        }
        if (size > (size_t)std::numeric_limits<int>::max()) {
            if (error) *error = "the file is too large to decode";
            return std::nullopt;
        }
        int width = 0, height = 0, channels = 0;
        stbi_uc* decoded = stbi_load_from_memory(static_cast<const stbi_uc*>(bytes), (int)size, &width, &height, &channels, 4);
        if (!decoded) {
            if (error) {
                const char* reason = stbi_failure_reason();
                *error = std::string("not a readable PNG, BMP, TGA or GIF image") + (reason ? std::string(" (") + reason + ")" : std::string());
            }
            return std::nullopt;
        }
        RgbaImage image(width, height);
        std::memcpy(image.pixels.data(), decoded, image.pixelCount() * 4);
        stbi_image_free(decoded);
        return image;
    }

    std::optional<RgbaImage> loadRgbaImage(const fs::path& path, std::string* error) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            if (error) *error = "can't open " + path.generic_string();
            return std::nullopt;
        }
        const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string reason;
        std::optional<RgbaImage> image = decodeRgbaImage(bytes.data(), bytes.size(), &reason);
        if (!image && error) *error = path.generic_string() + ": " + reason;
        return image;
    }

    bool saveRgbaPng(const RgbaImage& image, const fs::path& path) {
        if (image.empty() || !image.valid()) return false;
        return writePng(path, image.width, image.height, 4, image.pixels.data(), image.width * 4);
    }

    std::string encodeRgbaPng(const RgbaImage& image) {
        std::string png;
        if (image.empty() || !image.valid()) return png;
        auto append = [](void* context, void* data, int size) {
            static_cast<std::string*>(context)->append(static_cast<const char*>(data), (size_t)size);
        };
        if (!stbi_write_png_to_func(append, &png, image.width, image.height, 4, image.pixels.data(), image.width * 4)) {
            png.clear();
        }
        return png;
    }

}
