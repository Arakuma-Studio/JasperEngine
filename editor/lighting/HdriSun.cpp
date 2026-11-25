// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HdriSun.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace jasperengine::editor::hdrisun {

namespace {

    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kRadiansPerDegree = kPi / 180.0f;
    // the farthest a cube face's texel is from its axis (the corner: atan(sqrt(2)))
    constexpr float kFaceReach = 0.9553166f;

    struct Dir {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    float dot(const Dir& a, const Dir& b){
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    Dir normalized(float x, float y, float z){
        const float length = std::sqrt(x * x + y * y + z * z);
        if (length <= 0.0f)
            return Dir{0.0f, 0.0f, 1.0f};
        return Dir{x / length, y / length, z / length};
    }

    float luminanceOf(const float* p){
        const float y = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
        return std::isfinite(y) ? std::max(y, 0.0f) : 0.0f;
    }

    float channelOf(const float* p, int c){
        return std::isfinite(p[c]) ? std::max(p[c], 0.0f) : 0.0f;
    }

    // a face texel's direction, u and v in -1..1 with v down the image: the engine's cubemap
    // convention (IBLEnvironment, TextureData::splitCubeMapCross)
    Dir faceDirection(int face, float u, float v){
        switch (face){
            case 0:  return normalized(1.0f, -v, -u);
            case 1:  return normalized(-1.0f, -v, u);
            case 2:  return normalized(u, 1.0f, v);
            case 3:  return normalized(u, -1.0f, -v);
            case 4:  return normalized(u, -v, 1.0f);
            default: return normalized(-u, -v, -1.0f);
        }
    }

    void faceOf(const Dir& d, int& face, float& u, float& v){
        const float ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
        if (ax >= ay && ax >= az){
            face = d.x > 0.0f ? 0 : 1;
            u = (d.x > 0.0f ? -d.z : d.z) / ax;
            v = -d.y / ax;
        }else if (ay >= ax && ay >= az){
            face = d.y > 0.0f ? 2 : 3;
            u = d.x / ay;
            v = (d.y > 0.0f ? d.z : -d.z) / ay;
        }else{
            face = d.z > 0.0f ? 4 : 5;
            u = (d.z > 0.0f ? d.x : -d.x) / az;
            v = -d.y / az;
        }
    }

    int strideOf(const SkyImage& image){
        return image.stride > 0 ? image.stride : image.width * 4;
    }

    // every texel of the image with its direction and solid angle
    template<typename F>
    void forEachTexel(const SkyImage& image, F&& visit){
        const int stride = strideOf(image);
        if (image.layout == SkyLayout::CubeFace){
            const float du = 2.0f / (float)image.width;
            const float dv = 2.0f / (float)image.height;
            for (int y = 0; y < image.height; y++){
                const float v = ((float)y + 0.5f) * dv - 1.0f;
                float* row = image.rgba + (size_t)y * (size_t)stride;
                for (int x = 0; x < image.width; x++){
                    const float u = ((float)x + 0.5f) * du - 1.0f;
                    const float r2 = 1.0f + u * u + v * v;
                    visit(row + (size_t)x * 4, faceDirection(image.face, u, v), du * dv / (r2 * std::sqrt(r2)));
                }
            }
            return;
        }
        const float dLon = 2.0f * kPi / (float)image.width;
        const float dLat = kPi / (float)image.height;
        for (int y = 0; y < image.height; y++){
            const float lat = 0.5f * kPi - ((float)y + 0.5f) * dLat;
            const float cosLat = std::cos(lat), sinLat = std::sin(lat);
            const float omega = dLon * dLat * cosLat;
            float* row = image.rgba + (size_t)y * (size_t)stride;
            for (int x = 0; x < image.width; x++){
                const float lon = ((float)x + 0.5f) * dLon - kPi;
                visit(row + (size_t)x * 4, Dir{cosLat * std::sin(lon), sinLat, cosLat * std::cos(lon)}, omega);
            }
        }
    }

    // every texel within `radius` (radians) of `centre`, with the cosine of its angle to it; the
    // faces that cannot reach it are skipped
    template<typename F>
    void forEachNear(const std::vector<SkyImage>& images, const Dir& centre, float radius, F&& visit){
        const float cosRadius = std::cos(radius);
        for (const SkyImage& image : images){
            if (image.layout == SkyLayout::CubeFace){
                const Dir axis = faceDirection(image.face, 0.0f, 0.0f);
                if (std::acos(std::clamp(dot(axis, centre), -1.0f, 1.0f)) > kFaceReach + radius)
                    continue;
            }
            forEachTexel(image, [&](float* p, const Dir& d, float omega){
                const float c = dot(d, centre);
                if (c >= cosRadius)
                    visit(p, d, omega, c);
            });
        }
    }

    // the luminance of the sky in a direction, from the texel nearest it; false when no image
    // holds the direction (a lone face)
    bool lumaAt(const std::vector<SkyImage>& images, const Dir& d, float& luma){
        for (const SkyImage& image : images){
            int x = 0, y = 0;
            if (image.layout == SkyLayout::CubeFace){
                int face = 0;
                float u = 0.0f, v = 0.0f;
                faceOf(d, face, u, v);
                if (face != image.face)
                    continue;
                x = (int)std::floor((u * 0.5f + 0.5f) * (float)image.width);
                y = (int)std::floor((v * 0.5f + 0.5f) * (float)image.height);
            }else{
                const float lon = std::atan2(d.x, d.z);
                const float lat = std::asin(std::clamp(d.y, -1.0f, 1.0f));
                x = (int)std::floor((lon + kPi) / (2.0f * kPi) * (float)image.width);
                y = (int)std::floor((0.5f * kPi - lat) / kPi * (float)image.height);
            }
            x = std::clamp(x, 0, image.width - 1);
            y = std::clamp(y, 0, image.height - 1);
            luma = luminanceOf(image.rgba + (size_t)y * (size_t)strideOf(image) + (size_t)x * 4);
            return true;
        }
        return false;
    }

    Dir binCentre(int bx, int by){
        const float lon = ((float)bx + 0.5f) / (float)EQUIRECT_WIDTH * 2.0f * kPi - kPi;
        const float lat = 0.5f * kPi - ((float)by + 0.5f) / (float)EQUIRECT_HEIGHT * kPi;
        return Dir{std::cos(lat) * std::sin(lon), std::sin(lat), std::cos(lat) * std::cos(lon)};
    }

    // the solid-angle weighted mean colour and luminance of the texels from `inner` to `outer`
    // (radians) around `centre`; false when there are none
    bool ringMean(const std::vector<SkyImage>& images, const Dir& centre, float inner, float outer, Vector3& color, float& luma){
        const float cosInner = std::cos(inner);
        double sum[3] = {0.0, 0.0, 0.0};
        double lumaSum = 0.0, weight = 0.0;
        forEachNear(images, centre, outer, [&](float* p, const Dir&, float omega, float c){
            if (c > cosInner)
                return;
            for (int k = 0; k < 3; k++)
                sum[k] += (double)channelOf(p, k) * omega;
            lumaSum += (double)luminanceOf(p) * omega;
            weight += omega;
        });
        if (weight <= 0.0)
            return false;
        color = Vector3((float)(sum[0] / weight), (float)(sum[1] / weight), (float)(sum[2] / weight));
        luma = (float)(lumaSum / weight);
        return true;
    }

    std::string ratioText(float ratio){
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f", ratio);
        return text;
    }

}

bool findSun(const std::vector<SkyImage>& images, SunResult& out, std::string& error){
    out = SunResult();
    for (const SkyImage& image : images){
        if (!image.rgba || image.width <= 0 || image.height <= 0){
            error = "the sky has an empty image";
            return false;
        }
    }
    if (images.empty()){
        error = "the sky has no image";
        return false;
    }

    // the downsampled equirect: each bin the solid-angle weighted mean luminance of its texels
    const size_t bins = (size_t)EQUIRECT_WIDTH * (size_t)EQUIRECT_HEIGHT;
    std::vector<double> binLuma(bins, 0.0), binWeight(bins, 0.0);
    double totalLuma = 0.0, totalWeight = 0.0;
    for (const SkyImage& image : images){
        forEachTexel(image, [&](float* p, const Dir& d, float omega){
            const float luma = luminanceOf(p);
            const float lon = std::atan2(d.x, d.z);
            const float lat = std::asin(std::clamp(d.y, -1.0f, 1.0f));
            const int bx = std::clamp((int)std::floor((lon + kPi) / (2.0f * kPi) * (float)EQUIRECT_WIDTH), 0, EQUIRECT_WIDTH - 1);
            const int by = std::clamp((int)std::floor((0.5f * kPi - lat) / kPi * (float)EQUIRECT_HEIGHT), 0, EQUIRECT_HEIGHT - 1);
            const size_t b = (size_t)by * EQUIRECT_WIDTH + (size_t)bx;
            binLuma[b] += (double)luma * omega;
            binWeight[b] += omega;
            totalLuma += (double)luma * omega;
            totalWeight += omega;
        });
    }
    if (totalWeight <= 0.0 || totalLuma <= 0.0){
        error = "no distinct sun: the sky is black";
        return false;
    }
    const float meanLuma = (float)(totalLuma / totalWeight);

    // bins no texel fell in (a sky coarser than the bins) take the texel at their centre; bins
    // outside every image (a lone face) are left out
    std::vector<float> luma(bins, 0.0f);
    std::vector<uint8_t> valid(bins, 0);
    for (int by = 0; by < EQUIRECT_HEIGHT; by++){
        for (int bx = 0; bx < EQUIRECT_WIDTH; bx++){
            const size_t b = (size_t)by * EQUIRECT_WIDTH + (size_t)bx;
            if (binWeight[b] > 0.0){
                luma[b] = (float)(binLuma[b] / binWeight[b]);
                valid[b] = 1;
            }else if (lumaAt(images, binCentre(bx, by), luma[b])){
                valid[b] = 1;
            }
        }
    }

    // the brightest 3x3 block (longitude wraps)
    float peak = -1.0f;
    int peakX = 0, peakY = 0;
    for (int by = 0; by < EQUIRECT_HEIGHT; by++){
        for (int bx = 0; bx < EQUIRECT_WIDTH; bx++){
            if (!valid[(size_t)by * EQUIRECT_WIDTH + (size_t)bx])
                continue;
            float sum = 0.0f;
            int count = 0;
            for (int dy = -1; dy <= 1; dy++){
                const int y = by + dy;
                if (y < 0 || y >= EQUIRECT_HEIGHT)
                    continue;
                for (int dx = -1; dx <= 1; dx++){
                    const int x = (bx + dx + EQUIRECT_WIDTH) % EQUIRECT_WIDTH;
                    const size_t b = (size_t)y * EQUIRECT_WIDTH + (size_t)x;
                    if (!valid[b])
                        continue;
                    sum += luma[b];
                    count++;
                }
            }
            const float block = count > 0 ? sum / (float)count : 0.0f;
            if (block > peak){
                peak = block;
                peakX = bx;
                peakY = by;
            }
        }
    }
    out.peakRatio = peak / meanLuma;
    if (out.peakRatio < MIN_PEAK_RATIO){
        error = "no distinct sun: the sky's brightest spot is " + ratioText(out.peakRatio) + "x its mean luminance, " +
                ratioText(MIN_PEAK_RATIO) + "x is needed (an overcast, night or LDR sky has none)";
        return false;
    }

    // the centroid of the light above the local background within CENTROID_DEGREES, twice: the
    // second around the first, the bin's centre being up to a bin and a half off
    Dir centre = binCentre(peakX, peakY);
    const float capRadius = CENTROID_DEGREES * kRadiansPerDegree;
    for (int pass = 0; pass < 2; pass++){
        Vector3 ringColor;
        float background = meanLuma;
        ringMean(images, centre, capRadius * 0.8f, capRadius, ringColor, background);
        double sum[3] = {0.0, 0.0, 0.0};
        double weight = 0.0;
        forEachNear(images, centre, capRadius, [&](float* p, const Dir& d, float omega, float){
            const double w = (double)std::max(luminanceOf(p) - background, 0.0f) * omega;
            sum[0] += w * d.x;
            sum[1] += w * d.y;
            sum[2] += w * d.z;
            weight += w;
        });
        if (weight <= 0.0)
            break;
        centre = normalized((float)sum[0], (float)sum[1], (float)sum[2]);
    }

    // the disc: the texels within MAX_DISC_DEGREES clearly above the background around it
    const float maxDisc = MAX_DISC_DEGREES * kRadiansPerDegree;
    Vector3 outsideColor;
    float outside = meanLuma;
    ringMean(images, centre, maxDisc, maxDisc * 1.2f, outsideColor, outside);
    float brightest = 0.0f;
    float nearestCos = -1.0f;
    float nearestOmega = 0.0f;
    forEachNear(images, centre, maxDisc, [&](float* p, const Dir&, float omega, float c){
        brightest = std::max(brightest, luminanceOf(p));
        if (c > nearestCos){
            nearestCos = c;
            nearestOmega = omega;
        }
    });
    const float threshold = std::max(2.0f * outside, outside + 0.02f * (brightest - outside));
    float discRadius = 0.0f;
    forEachNear(images, centre, maxDisc, [&](float* p, const Dir&, float, float c){
        if (luminanceOf(p) > threshold)
            discRadius = std::max(discRadius, std::acos(std::clamp(c, -1.0f, 1.0f)));
    });
    // a texel's own width past the farthest bright texel's centre
    const float texelAngle = std::sqrt(std::max(nearestOmega, 0.0f));
    discRadius = std::min(discRadius + texelAngle, maxDisc);
    const float ringWidth = std::max(kRadiansPerDegree, 2.0f * texelAngle);
    Vector3 ringColor = outsideColor;
    float ringLuma = outside;
    ringMean(images, centre, discRadius, discRadius + ringWidth, ringColor, ringLuma);

    double irradiance[3] = {0.0, 0.0, 0.0};
    forEachNear(images, centre, discRadius, [&](float* p, const Dir&, float omega, float){
        for (int k = 0; k < 3; k++)
            irradiance[k] += (double)std::max(channelOf(p, k) - ringColor[k], 0.0f) * omega;
    });

    out.direction = Vector3(centre.x, centre.y, centre.z);
    out.irradiance = Vector3((float)irradiance[0], (float)irradiance[1], (float)irradiance[2]);
    out.ringColor = ringColor;
    out.discDegrees = discRadius / kRadiansPerDegree;
    return true;
}

size_t removeSunDisc(const std::vector<SkyImage>& images, const SunResult& sun){
    const Dir centre = normalized(sun.direction.x, sun.direction.y, sun.direction.z);
    const float radius = std::min(sun.discDegrees, MAX_DISC_DEGREES) * kRadiansPerDegree;
    size_t changed = 0;
    forEachNear(images, centre, radius, [&](float* p, const Dir&, float, float){
        p[0] = sun.ringColor.x;
        p[1] = sun.ringColor.y;
        p[2] = sun.ringColor.z;
        changed++;
    });
    return changed;
}

bool viewsOf(float* rgba, int width, int height, bool cube, std::vector<SkyImage>& out){
    out.clear();
    if (!rgba || width <= 0 || height <= 0)
        return false;
    if (cube){
        // the tiles of TextureData::splitCubeMapCross: +X, -X, +Y, -Y, +Z, -Z
        const int ratio = (int)((uint64_t)width * 100 / (uint64_t)height);
        int tileW = 0, tileH = 0;
        int tiles[6][2];
        if (ratio == 133){
            tileW = width / 4;
            tileH = height / 3;
            const int horizontal[6][2] = {{2, 1}, {0, 1}, {1, 0}, {1, 2}, {1, 1}, {3, 1}};
            for (int f = 0; f < 6; f++){
                tiles[f][0] = horizontal[f][0];
                tiles[f][1] = horizontal[f][1];
            }
        }else if (ratio == 75){
            tileW = width / 3;
            tileH = height / 4;
            const int vertical[6][2] = {{2, 1}, {0, 1}, {1, 0}, {1, 2}, {1, 1}, {1, 3}};
            for (int f = 0; f < 6; f++){
                tiles[f][0] = vertical[f][0];
                tiles[f][1] = vertical[f][1];
            }
        }else{
            return false;
        }
        for (int f = 0; f < 6; f++){
            SkyImage view;
            view.rgba = rgba + ((size_t)tiles[f][1] * (size_t)tileH * (size_t)width + (size_t)tiles[f][0] * (size_t)tileW) * 4;
            view.width = tileW;
            view.height = tileH;
            view.stride = width * 4;
            view.layout = SkyLayout::CubeFace;
            view.face = f;
            out.push_back(view);
        }
        return true;
    }
    SkyImage view;
    view.rgba = rgba;
    view.width = width;
    view.height = height;
    if (width == height){
        view.layout = SkyLayout::CubeFace;
    }else if (width == 2 * height){
        view.layout = SkyLayout::Equirect;
    }else{
        return false;
    }
    out.push_back(view);
    return true;
}

bool removeSunDiscInPlace(float* rgba, int width, int height, bool cube, std::string& why){
    std::vector<SkyImage> views;
    if (!viewsOf(rgba, width, height, cube, views)){
        why = cube ? "not a 4:3 or 3:4 cubemap cross" : "neither square (a cube face) nor 2:1 (an equirect)";
        return false;
    }
    SunResult sun;
    if (!findSun(views, sun, why))
        return false;
    removeSunDisc(views, sun);
    return true;
}

}
