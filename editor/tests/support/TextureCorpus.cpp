// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureCorpus.h"

#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;
namespace fs = std::filesystem;

namespace {

    uint32_t corpusHash(uint32_t x, uint32_t y, uint32_t seed){
        uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return h ^ (h >> 16);
    }

    float corpusLattice(int x, int y, uint32_t seed){
        return (corpusHash((uint32_t)x, (uint32_t)y, seed) & 0xFFFFFF) / float(0xFFFFFF);
    }

    // smooth value noise, [0,1]
    float corpusNoise(float x, float y, uint32_t seed){
        const int xi = (int)std::floor(x), yi = (int)std::floor(y);
        const float fx = x - xi, fy = y - yi;
        const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
        const float a = corpusLattice(xi, yi, seed), b = corpusLattice(xi + 1, yi, seed);
        const float c = corpusLattice(xi, yi + 1, seed), d = corpusLattice(xi + 1, yi + 1, seed);
        return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
    }

    float corpusFbm(float x, float y, uint32_t seed, int octaves){
        float sum = 0.0f, amplitude = 0.5f, norm = 0.0f;
        for (int o = 0; o < octaves; o++){
            sum += amplitude * corpusNoise(x, y, seed + (uint32_t)o * 17u);
            norm += amplitude;
            x *= 2.03f;
            y *= 2.03f;
            amplitude *= 0.5f;
        }
        return sum / norm;
    }

    uint8_t corpusByte(float v){
        return (uint8_t)std::clamp((int)std::lround(v * 255.0f), 0, 255);
    }

}

const std::vector<std::string>& jasperengine::editor::test::corpusNames(){
    static const std::vector<std::string> names = {"photo", "albedo", "ui", "foliage", "normal", "mask", "gradient", "sky"};
    return names;
}

CorpusImage jasperengine::editor::test::corpusImage(const std::string& name, int width, int height){
    CorpusImage image;
    image.name = name;
    image.width = width;
    image.height = height;
    const size_t texels = (size_t)width * (size_t)height;
    image.rgba.assign(texels * 4, 255);
    const float scale = 8.0f / 512.0f;
    for (int y = 0; y < height; y++){
        for (int x = 0; x < width; x++){
            uint8_t* p = &image.rgba[((size_t)y * width + x) * 4];
            const float u = x * scale, v = y * scale;
            if (name == "photo"){
                // a landscape-like mix: hue regions, fine detail and soft edges
                const float region = corpusFbm(u * 0.6f, v * 0.6f, 1, 4);
                const float detail = corpusFbm(u * 4.0f, v * 4.0f, 2, 5);
                const float r = 0.25f + 0.55f * region + 0.2f * detail;
                const float g = 0.35f + 0.35f * corpusFbm(u * 0.8f, v * 0.8f, 3, 4) + 0.2f * detail;
                const float b = 0.2f + 0.5f * (1.0f - region) * (0.6f + 0.4f * detail);
                p[0] = corpusByte(r);
                p[1] = corpusByte(g);
                p[2] = corpusByte(b);
            }else if (name == "albedo"){
                // bricks: mortar lines, per-brick tint, grain
                const float bu = x / 64.0f + ((y / 32) % 2 ? 0.5f : 0.0f), bv = y / 32.0f;
                const float fu = bu - std::floor(bu), fv = bv - std::floor(bv);
                const bool mortar = fu < 0.06f || fv < 0.12f;
                const float tint = corpusLattice((int)std::floor(bu), (int)std::floor(bv), 4);
                const float grain = corpusFbm(u * 6.0f, v * 6.0f, 5, 3);
                const float base = mortar ? 0.7f : 0.45f + 0.25f * tint;
                p[0] = corpusByte(base * (mortar ? 0.95f : 1.0f) * (0.8f + 0.25f * grain));
                p[1] = corpusByte(base * (mortar ? 0.93f : 0.55f) * (0.8f + 0.25f * grain));
                p[2] = corpusByte(base * (mortar ? 0.9f : 0.42f) * (0.8f + 0.25f * grain));
            }else if (name == "ui"){
                // a rounded panel with a soft shadow, a circle icon and a gradient bar
                image.alpha = true;
                image.type = TextureImportType::UI;
                const float px = (x + 0.5f) / width, py = (y + 0.5f) / height;
                const float dx = std::max(std::abs(px - 0.5f) - 0.32f, 0.0f), dy = std::max(std::abs(py - 0.5f) - 0.22f, 0.0f);
                const float panel = std::sqrt(dx * dx + dy * dy) - 0.08f;
                const float coverage = std::clamp(0.5f - panel * width, 0.0f, 1.0f);
                const float shadow = std::clamp(1.0f - (panel + 0.02f) * 12.0f, 0.0f, 1.0f) * 0.35f;
                const float circle = std::clamp(0.5f - (std::hypot(px - 0.3f, py - 0.45f) - 0.08f) * width, 0.0f, 1.0f);
                float r = 0.18f, g = 0.2f, b = 0.26f;
                if (py > 0.6f && py < 0.66f && px > 0.25f && px < 0.75f){
                    r = 0.2f + 0.7f * (px - 0.25f) * 2.0f;
                    g = 0.6f;
                    b = 0.9f;
                }
                r = r + (0.95f - r) * circle;
                g = g + (0.75f - g) * circle;
                b = b + (0.2f - b) * circle;
                const float a = std::max(coverage, shadow);
                p[0] = corpusByte(coverage > 0.0f ? r : 0.0f);
                p[1] = corpusByte(coverage > 0.0f ? g : 0.0f);
                p[2] = corpusByte(coverage > 0.0f ? b : 0.0f);
                p[3] = corpusByte(a);
            }else if (name == "foliage"){
                // cut-out leaves: a hard-edged alpha mask over green detail
                image.alpha = true;
                const float leaf = corpusFbm(u * 1.5f, v * 1.5f, 6, 4) + 0.25f * corpusFbm(u * 8.0f, v * 8.0f, 7, 2);
                const bool inside = leaf > 0.62f;
                const float shade = corpusFbm(u * 5.0f, v * 5.0f, 8, 3);
                p[0] = corpusByte(0.15f + 0.2f * shade);
                p[1] = corpusByte(0.35f + 0.4f * shade);
                p[2] = corpusByte(0.1f + 0.1f * shade);
                p[3] = inside ? 255 : 0;
            }else if (name == "normal"){
                // the tangent-space normal of a height field
                image.type = TextureImportType::Normal;
                const float e = 1.0f / 64.0f;
                // four octaves: the finest bump spans about 4 texels, as in a sculpted or baked map
                auto h = [&](float a, float b){ return corpusFbm(a * 2.0f, b * 2.0f, 9, 4) * 0.6f; };
                const float hx = (h(u + e, v) - h(u - e, v)) / (2.0f * e) * 0.15f;
                const float hy = (h(u, v + e) - h(u, v - e)) / (2.0f * e) * 0.15f;
                const float len = std::sqrt(hx * hx + hy * hy + 1.0f);
                p[0] = corpusByte(-hx / len * 0.5f + 0.5f);
                p[1] = corpusByte(-hy / len * 0.5f + 0.5f);
                p[2] = corpusByte(1.0f / len * 0.5f + 0.5f);
            }else if (name == "mask"){
                // ambient occlusion: dark creases in grey
                image.type = TextureImportType::SingleChannel;
                const float ao = std::pow(corpusFbm(u * 2.0f, v * 2.0f, 10, 5), 0.7f);
                p[0] = p[1] = p[2] = corpusByte(ao);
            }else if (name == "gradient"){
                // smooth ramps: banding shows here first
                const float px = (x + 0.5f) / width, py = (y + 0.5f) / height;
                p[0] = corpusByte(px);
                p[1] = corpusByte(py);
                p[2] = corpusByte(0.5f + 0.5f * std::sin((px + py) * 3.14159f));
            }else if (name == "sky"){
                image.hdr = true;
                image.type = TextureImportType::HDR;
            }else{
                throw std::runtime_error("no corpus image " + name);
            }
        }
    }
    if (image.hdr){
        image.rgba.clear();
        image.rgbaFloat.assign(texels * 4, 1.0f);
        for (int y = 0; y < height; y++){
            for (int x = 0; x < width; x++){
                float* p = &image.rgbaFloat[((size_t)y * width + x) * 4];
                const float px = (x + 0.5f) / width, py = (y + 0.5f) / height;
                const float sun = std::exp(-std::pow(std::hypot(px - 0.7f, py - 0.3f) * 18.0f, 2.0f)) * 40.0f;
                const float cloud = std::max(0.0f, corpusFbm(px * 6.0f, py * 3.0f, 11, 5) - 0.5f) * 2.0f;
                p[0] = 0.3f + 0.4f * py + sun + cloud * 1.5f;
                p[1] = 0.5f + 0.3f * py + sun * 0.9f + cloud * 1.5f;
                p[2] = 1.2f - 0.4f * py + sun * 0.7f + cloud * 1.6f;
            }
        }
    }
    return image;
}

fs::path jasperengine::editor::test::writeCorpusImage(const CorpusImage& image, const fs::path& dir){
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (image.hdr){
        const fs::path path = dir / (image.name + ".hdr");
        std::vector<float> rgb((size_t)image.width * image.height * 3);
        for (size_t i = 0; i < (size_t)image.width * image.height; i++)
            for (int c = 0; c < 3; c++)
                rgb[i * 3 + c] = image.rgbaFloat[i * 4 + c];
        if (!stbi_write_hdr(path.string().c_str(), image.width, image.height, 3, rgb.data()))
            throw std::runtime_error("cannot write " + path.string());
        return path;
    }
    const fs::path path = dir / (image.name + ".png");
    if (!stbi_write_png(path.string().c_str(), image.width, image.height, 4, image.rgba.data(), image.width * 4))
        throw std::runtime_error("cannot write " + path.string());
    return path;
}

ScratchDir::ScratchDir(const std::string& name){
    std::error_code ec;
    path = fs::temp_directory_path(ec) / ("je-" + name + "-" + std::to_string(std::rand()) + std::to_string((uintptr_t)this));
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
}

ScratchDir::~ScratchDir(){
    std::error_code ec;
    fs::remove_all(path, ec);
}
