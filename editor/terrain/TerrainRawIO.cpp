// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainRawIO.h"

#include "util/FloatImage.h"
#include "util/PngWriter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>

using namespace jasperengine;

namespace {

    std::string rawExtension(const std::string& name){
        const size_t dot = name.find_last_of('.');
        if (dot == std::string::npos) return std::string();
        std::string extension = name.substr(dot + 1);
        for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return extension;
    }

    int bytesPerSample(editor::HeightFileFormat format){
        switch (format){
            case editor::HeightFileFormat::R8: return 1;
            case editor::HeightFileFormat::R16: return 2;
            case editor::HeightFileFormat::R32F: return 4;
            default: return 0;
        }
    }

    bool isPowerOfTwo(int64_t value){
        return value > 0 && (value & (value - 1)) == 0;
    }

    // Square sides first, a power of two or one more preferred (a heightmap's usual sizes)
    std::vector<int> squareSides(int64_t samples){
        std::vector<int> sides;
        const int64_t side = static_cast<int64_t>(std::llround(std::sqrt(static_cast<double>(samples))));
        for (int64_t s = std::max<int64_t>(1, side - 1); s <= side + 1; s++){
            if (s * s == samples) sides.push_back(static_cast<int>(s));
        }
        return sides;
    }

    double sampleAt(const uint8_t* bytes, size_t index, editor::HeightFileFormat format, bool little){
        switch (format){
            case editor::HeightFileFormat::R8:
                return bytes[index];
            case editor::HeightFileFormat::R16: {
                const uint8_t* at = bytes + index * 2;
                return little ? (at[0] | (at[1] << 8)) : (at[1] | (at[0] << 8));
            }
            case editor::HeightFileFormat::R32F: {
                const uint8_t* at = bytes + index * 4;
                uint8_t ordered[4];
                for (int i = 0; i < 4; i++) ordered[i] = little ? at[i] : at[3 - i];
                float value = 0.0f;
                std::memcpy(&value, ordered, 4);
                return std::isfinite(value) ? value : 0.0;
            }
            default:
                return 0.0;
        }
    }

    // How rough the samples are in one byte order: real terrain is smooth, the wrong order is noise
    double roughness(const uint8_t* bytes, int width, int height, editor::HeightFileFormat format, bool little){
        double sum = 0.0;
        double range = 0.0;
        double low = std::numeric_limits<double>::max(), high = std::numeric_limits<double>::lowest();
        const int step = std::max(1, std::max(width, height) / 256);
        for (int y = 0; y < height; y += step){
            for (int x = 0; x + 1 < width; x += step){
                const double a = sampleAt(bytes, static_cast<size_t>(y) * width + x, format, little);
                const double b = sampleAt(bytes, static_cast<size_t>(y) * width + x + 1, format, little);
                sum += std::fabs(a - b);
                low = std::min(low, a);
                high = std::max(high, a);
            }
        }
        range = high > low ? high - low : 1.0;
        return sum / range;
    }

}

editor::HeightFileFormat editor::TerrainRawIO::formatOfName(const std::string& name){
    const std::string extension = rawExtension(name);
    if (extension == "r8") return HeightFileFormat::R8;
    if (extension == "r16") return HeightFileFormat::R16;
    if (extension == "r32" || extension == "f32") return HeightFileFormat::R32F;
    if (extension == "png") return HeightFileFormat::Png;
    if (extension == "exr") return HeightFileFormat::Exr;
    return HeightFileFormat::Auto;
}

const char* editor::TerrainRawIO::formatName(HeightFileFormat format){
    switch (format){
        case HeightFileFormat::R8: return "r8";
        case HeightFileFormat::R16: return "r16";
        case HeightFileFormat::R32F: return "r32f";
        case HeightFileFormat::Png: return "png";
        case HeightFileFormat::Exr: return "exr";
        case HeightFileFormat::Auto: break;
    }
    return "auto";
}

bool editor::TerrainRawIO::formatFromName(const std::string& name, HeightFileFormat& out){
    for (HeightFileFormat format : {HeightFileFormat::Auto, HeightFileFormat::R8, HeightFileFormat::R16, HeightFileFormat::R32F,
                                    HeightFileFormat::Png, HeightFileFormat::Exr}){
        if (name == formatName(format)){
            out = format;
            return true;
        }
    }
    return false;
}

editor::HeightImport editor::TerrainRawIO::decode(const std::string& name, const std::vector<uint8_t>& bytes, const HeightImportOptions& options){
    HeightImport result;
    std::vector<double> values;

    HeightFileFormat format = options.format;
    if (format == HeightFileFormat::Auto){
        format = formatOfName(name);
        result.formatGuessed = true;
    }

    if (format == HeightFileFormat::Png || format == HeightFileFormat::Exr){
        FloatImage image;
        std::string error;
        if (!readFloatImageFromMemory(bytes.data(), bytes.size(), name, image, &error)){
            result.error = error;
            return result;
        }
        result.width = image.width;
        result.height = image.height;
        values.resize(static_cast<size_t>(image.width) * image.height);
        // a PNG reads 0..1 whatever its depth: its full range is 0..1
        for (size_t i = 0; i < values.size(); i++) values[i] = image.rgba[i * 4];
    } else {
        // depth from the extension, else from the length with the size
        std::vector<HeightFileFormat> depths;
        if (format != HeightFileFormat::Auto){
            depths.push_back(format);
        } else {
            depths = {HeightFileFormat::R16, HeightFileFormat::R8, HeightFileFormat::R32F};
        }
        struct Candidate{
            HeightFileFormat format;
            int width;
            int height;
        };
        std::vector<Candidate> candidates;
        for (HeightFileFormat depth : depths){
            const int per = bytesPerSample(depth);
            if (bytes.size() % per != 0) continue;
            const int64_t samples = static_cast<int64_t>(bytes.size() / per);
            if (options.width > 0 && options.height > 0){
                if (static_cast<int64_t>(options.width) * options.height == samples) candidates.push_back({depth, options.width, options.height});
            } else if (options.width > 0){
                if (samples % options.width == 0) candidates.push_back({depth, options.width, static_cast<int>(samples / options.width)});
            } else {
                for (int side : squareSides(samples)) candidates.push_back({depth, side, side});
            }
        }
        // a power of two (or one more) is a heightmap's size; prefer those, then the extension's depth
        if (candidates.size() > 1){
            std::vector<Candidate> preferred;
            for (const Candidate& c : candidates){
                if (isPowerOfTwo(c.width) || isPowerOfTwo(c.width - 1)) preferred.push_back(c);
            }
            if (!preferred.empty()) candidates = preferred;
        }
        if (candidates.empty()){
            result.error = "The file's " + std::to_string(bytes.size()) + " bytes fit no square map of 8, 16 or 32-bit samples: give width and height.";
            return result;
        }
        if (candidates.size() > 1){
            std::string list;
            for (const Candidate& c : candidates){
                if (!list.empty()) list += ", ";
                list += std::string(formatName(c.format)) + " " + std::to_string(c.width) + "x" + std::to_string(c.height);
            }
            result.error = "The file could be " + list + ": give format, width and height.";
            return result;
        }
        format = candidates[0].format;
        result.width = candidates[0].width;
        result.height = candidates[0].height;
        result.sizeGuessed = options.width <= 0 || options.height <= 0;
        result.formatGuessed = result.formatGuessed || options.format == HeightFileFormat::Auto;

        bool little = options.byteOrder != HeightByteOrder::Big;
        if (options.byteOrder == HeightByteOrder::Auto && format != HeightFileFormat::R8){
            little = roughness(bytes.data(), result.width, result.height, format, true) <=
                     roughness(bytes.data(), result.width, result.height, format, false);
            result.byteOrderGuessed = true;
        }
        result.byteOrder = little ? HeightByteOrder::Little : HeightByteOrder::Big;
        values.resize(static_cast<size_t>(result.width) * result.height);
        for (size_t i = 0; i < values.size(); i++) values[i] = sampleAt(bytes.data(), i, format, little);
    }
    result.format = format;

    if (values.empty()){
        result.error = "The file holds no samples.";
        return result;
    }
    result.dataMin = *std::min_element(values.begin(), values.end());
    result.dataMax = *std::max_element(values.begin(), values.end());

    switch (options.range){
        case HeightRange::Auto:
            result.rangeLow = result.dataMin;
            result.rangeHigh = result.dataMax;
            break;
        case HeightRange::Full:
            result.rangeLow = 0.0;
            result.rangeHigh = format == HeightFileFormat::R8 ? 255.0 : (format == HeightFileFormat::R16 ? 65535.0 : 1.0);
            break;
        case HeightRange::Source:
            result.rangeLow = options.sourceMin;
            result.rangeHigh = options.sourceMax;
            break;
    }
    const double span = result.rangeHigh - result.rangeLow;
    result.normalized.resize(values.size());
    for (int y = 0; y < result.height; y++){
        const int row = options.flipVertically ? result.height - 1 - y : y;
        for (int x = 0; x < result.width; x++){
            const double value = values[static_cast<size_t>(row) * result.width + x];
            double n = span > 0.0 ? (value - result.rangeLow) / span : 0.0;
            if (n < 0.0){
                result.clippedLow++;
                n = 0.0;
            } else if (n > 1.0){
                result.clippedHigh++;
                n = 1.0;
            }
            result.normalized[static_cast<size_t>(y) * result.width + x] = static_cast<float>(n);
        }
    }
    result.ok = true;
    return result;
}

bool editor::TerrainRawIO::encode(const std::vector<float>& normalized, int width, int height, const HeightExportOptions& options,
                                  std::vector<uint8_t>& bytes, std::string& error, double* rangeLow, double* rangeHigh){
    if (width <= 0 || height <= 0 || normalized.size() < static_cast<size_t>(width) * height){
        error = "No heights to write.";
        return false;
    }
    double low = 0.0, high = 1.0;
    if (options.range == HeightRange::Auto){
        low = *std::min_element(normalized.begin(), normalized.end());
        high = *std::max_element(normalized.begin(), normalized.end());
        if (high <= low) high = low + 1.0;
    }
    if (rangeLow) *rangeLow = low;
    if (rangeHigh) *rangeHigh = high;
    auto stretched = [&](int x, int y){
        const int row = options.flipVertically ? height - 1 - y : y;
        const double n = (normalized[static_cast<size_t>(row) * width + x] - low) / (high - low);
        return std::clamp(n, 0.0, 1.0);
    };
    const bool little = options.byteOrder != HeightByteOrder::Big;
    bytes.clear();
    switch (options.format){
        case HeightFileFormat::R8:
            for (int y = 0; y < height; y++){
                for (int x = 0; x < width; x++) bytes.push_back(static_cast<uint8_t>(std::lround(stretched(x, y) * 255.0)));
            }
            return true;
        case HeightFileFormat::R16:
        case HeightFileFormat::Png: {
            std::vector<uint8_t> samples;
            samples.reserve(static_cast<size_t>(width) * height * 2);
            for (int y = 0; y < height; y++){
                for (int x = 0; x < width; x++){
                    const uint32_t v = static_cast<uint32_t>(std::lround(stretched(x, y) * 65535.0));
                    const bool lowFirst = options.format == HeightFileFormat::Png || little;
                    samples.push_back(static_cast<uint8_t>(lowFirst ? (v & 0xFF) : (v >> 8)));
                    samples.push_back(static_cast<uint8_t>(lowFirst ? (v >> 8) : (v & 0xFF)));
                }
            }
            if (options.format == HeightFileFormat::R16){
                bytes = std::move(samples);
                return true;
            }
            std::vector<unsigned char> png;
            if (!encodePng16(width, height, 1, samples.data(), samples.size(), png)){
                error = "The PNG could not be encoded.";
                return false;
            }
            bytes.assign(png.begin(), png.end());
            return true;
        }
        case HeightFileFormat::R32F:
            for (int y = 0; y < height; y++){
                for (int x = 0; x < width; x++){
                    const float v = static_cast<float>(stretched(x, y));
                    uint8_t raw[4];
                    std::memcpy(raw, &v, 4);
                    for (int i = 0; i < 4; i++) bytes.push_back(little ? raw[i] : raw[3 - i]);
                }
            }
            return true;
        default:
            error = std::string("Format ") + formatName(options.format) + " is not written as bytes.";
            return false;
    }
}
