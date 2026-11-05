// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TiledLights.h"

#include <algorithm>
#include <limits>
#include <cmath>

using namespace jasperengine;

namespace {
    // the clip-space position of a view-space point
    Vector4 projectViewPoint(const Matrix4& projection, float x, float y, float z){
        return projection * Vector4(x, y, z, 1.0f);
    }
}

TiledLights::Grid TiledLights::makeGrid(int width, int height, int columns, int rows){
    Grid grid;
    grid.columns = std::max(columns, 1);
    grid.rows = std::max(rows, 1);
    // even sizes, so the tiles start on quad boundaries
    const int tileWidth = (std::max(width, 1) + grid.columns - 1) / grid.columns;
    const int tileHeight = (std::max(height, 1) + grid.rows - 1) / grid.rows;
    grid.tileWidth = std::max(2, (tileWidth + 1) & ~1);
    grid.tileHeight = std::max(2, (tileHeight + 1) & ~1);
    return grid;
}

int TiledLights::textureRows(const Grid& grid){
    const int texels = LIGHT_TEXELS + grid.tileCount() * TILE_TEXELS;
    return (texels + TEXTURE_WIDTH - 1) / TEXTURE_WIDTH;
}

bool TiledLights::projectSphere(const View& view, const Vector3& center, float radius, float rect[4]){
    const float width = (float)view.width;
    const float height = (float)view.height;
    if (width <= 0.0f || height <= 0.0f)
        return false;

    const Vector4 c = view.viewMatrix * Vector4(center.x, center.y, center.z, 1.0f);
    const Matrix4& p = view.projectionMatrix;
    const float r = std::max(radius, 0.0f);

    // the camera looks down -z: the sphere is in front of the near plane while c.z + r < -near
    if (c.z - r > -view.nearClip || c.z + r < -view.farClip)
        return false;

    float ndc[4] = {-1.0f, -1.0f, 1.0f, 1.0f}; // min x, min y, max x, max y
    bool full = false;
    if (view.perspective){
        // reaching the near plane (or holding the camera): its projection is unbounded there
        if (c.z + r > -view.nearClip){
            full = true;
        }else{
            // per axis, the two lines from the eye tangent to the sphere's circle in that axis'
            // plane with z, projected (the sphere's exact bounds along the axis)
            for (int axis = 0; axis < 2; axis++){
                const float a = (axis == 0) ? c.x : c.y;
                const float z = c.z;
                const float lengthSquared = a * a + z * z;
                const float tangent = std::sqrt(std::max(lengthSquared - r * r, 0.0f));
                const float invLength = 1.0f / std::sqrt(lengthSquared);
                const float cosAngle = tangent * invLength;
                const float sinAngle = r * invLength;
                const float va = a * tangent * invLength;
                const float vz = z * tangent * invLength;
                const float points[2][2] = {
                    {cosAngle * va - sinAngle * vz, sinAngle * va + cosAngle * vz},
                    {cosAngle * va + sinAngle * vz, -sinAngle * va + cosAngle * vz}
                };
                // (not seeded with the view's -1..1: a sphere wholly off one side must stay off it)
                float low = std::numeric_limits<float>::max();
                float high = -std::numeric_limits<float>::max();
                for (int k = 0; k < 2; k++){
                    const Vector4 clip = (axis == 0) ? projectViewPoint(p, points[k][0], c.y, points[k][1])
                                                     : projectViewPoint(p, c.x, points[k][0], points[k][1]);
                    if (clip.w <= 1e-6f){
                        low = -1.0f;
                        high = 1.0f;
                        break;
                    }
                    const float v = ((axis == 0) ? clip.x : clip.y) / clip.w;
                    low = std::min(low, v);
                    high = std::max(high, v);
                }
                ndc[axis] = low;
                ndc[axis + 2] = high;
            }
        }
    }else{
        // orthographic: the sphere's extent along each axis, projected linearly
        const Vector4 x0 = projectViewPoint(p, c.x - r, c.y, c.z);
        const Vector4 x1 = projectViewPoint(p, c.x + r, c.y, c.z);
        const Vector4 y0 = projectViewPoint(p, c.x, c.y - r, c.z);
        const Vector4 y1 = projectViewPoint(p, c.x, c.y + r, c.z);
        ndc[0] = std::min(x0.x / x0.w, x1.x / x1.w);
        ndc[2] = std::max(x0.x / x0.w, x1.x / x1.w);
        ndc[1] = std::min(y0.y / y0.w, y1.y / y1.w);
        ndc[3] = std::max(y0.y / y0.w, y1.y / y1.w);
    }

    if (!full){
        if (ndc[2] < -1.0f || ndc[0] > 1.0f || ndc[3] < -1.0f || ndc[1] > 1.0f)
            return false;
        for (float& v : ndc)
            v = std::clamp(v, -1.0f, 1.0f);
    }

    rect[0] = (ndc[0] * 0.5f + 0.5f) * width;
    rect[2] = (ndc[2] * 0.5f + 0.5f) * width;
    if (view.fragYUp){
        rect[1] = (ndc[1] * 0.5f + 0.5f) * height;
        rect[3] = (ndc[3] * 0.5f + 0.5f) * height;
    }else{
        rect[1] = (0.5f - ndc[3] * 0.5f) * height;
        rect[3] = (0.5f - ndc[1] * 0.5f) * height;
    }
    return true;
}

void TiledLights::build(const View& view, const std::vector<Light>& lights, std::vector<float>& texels, Stats& stats){
    const Grid& grid = view.grid;
    const int tiles = grid.tileCount();
    stats = Stats();
    stats.tiles = tiles;

    const size_t size = (size_t)TEXTURE_WIDTH * (size_t)textureRows(grid) * 4;
    texels.assign(size, 0.0f);
    viewLights.clear();
    candidates.clear();
    if (tiles <= 0 || grid.tileWidth <= 0 || grid.tileHeight <= 0)
        return;

    for (int i = 0; i < (int)lights.size(); i++){
        const Light& light = lights[i];
        Candidate candidate;
        candidate.light = i;
        if (light.global){
            candidate.distance = -1.0f;
            candidate.rect[0] = 0;
            candidate.rect[1] = 0;
            candidate.rect[2] = grid.columns - 1;
            candidate.rect[3] = grid.rows - 1;
        }else{
            float rect[4];
            if (!projectSphere(view, light.position, light.range, rect))
                continue;
            candidate.distance = std::max((light.position - view.cameraPosition).length() - light.range, 0.0f);
            candidate.rect[0] = std::clamp((int)std::floor(rect[0] / (float)grid.tileWidth), 0, grid.columns - 1);
            candidate.rect[1] = std::clamp((int)std::floor(rect[1] / (float)grid.tileHeight), 0, grid.rows - 1);
            candidate.rect[2] = std::clamp((int)std::floor(rect[2] / (float)grid.tileWidth), 0, grid.columns - 1);
            candidate.rect[3] = std::clamp((int)std::floor(rect[3] / (float)grid.tileHeight), 0, grid.rows - 1);
        }
        candidates.push_back(candidate);
    }

    // the nearest win the view's places (global lights first), ties in scene order
    std::sort(candidates.begin(), candidates.end(), [&lights](const Candidate& a, const Candidate& b){
        if (a.distance != b.distance)
            return a.distance < b.distance;
        return lights[a.light].sceneIndex < lights[b.light].sceneIndex;
    });
    if ((int)candidates.size() > MAX_LIGHTS_PER_VIEW){
        stats.droppedFromView = (int)candidates.size() - MAX_LIGHTS_PER_VIEW;
        candidates.resize(MAX_LIGHTS_PER_VIEW);
    }
    stats.lights = (int)candidates.size();

    // view-local indices in scene order, so a tile adds its lights up in the order an object's
    // own list does (includes/lights.glsl): the same lights light a pixel identically
    for (const Candidate& candidate : candidates)
        viewLights.push_back(lights[candidate.light].sceneIndex);
    std::sort(viewLights.begin(), viewLights.end());
    auto localOf = [this](int sceneIndex){
        return (int)(std::lower_bound(viewLights.begin(), viewLights.end(), sceneIndex) - viewLights.begin());
    };

    for (const Candidate& candidate : candidates){
        const Light& light = lights[candidate.light];
        const int local = localOf(light.sceneIndex);
        float* record = texels.data() + (size_t)local * LIGHT_RECORD_TEXELS * 4;
        for (int t = 0; t < LIGHT_RECORD_TEXELS; t++){
            record[t * 4 + 0] = light.record[t].x;
            record[t * 4 + 1] = light.record[t].y;
            record[t * 4 + 2] = light.record[t].z;
            record[t * 4 + 3] = light.record[t].w;
        }
    }

    // the tiles, nearest light first, each stopping at its cap
    tileCounts.assign((size_t)tiles, 0);
    float* tileData = texels.data() + (size_t)LIGHT_TEXELS * 4;
    for (const Candidate& candidate : candidates){
        const int local = localOf(lights[candidate.light].sceneIndex);
        for (int y = candidate.rect[1]; y <= candidate.rect[3]; y++){
            for (int x = candidate.rect[0]; x <= candidate.rect[2]; x++){
                const int tile = x + y * grid.columns;
                uint8_t& count = tileCounts[(size_t)tile];
                if (count >= MAX_LIGHTS_PER_TILE){
                    stats.droppedFromTiles++;
                    continue;
                }
                // after the header texel, four indices a texel
                tileData[(size_t)tile * TILE_TEXELS * 4 + 4 + count] = (float)local;
                count++;
            }
        }
    }

    long long total = 0;
    for (int tile = 0; tile < tiles; tile++){
        const int count = tileCounts[(size_t)tile];
        float* header = tileData + (size_t)tile * TILE_TEXELS * 4;
        std::sort(header + 4, header + 4 + count);
        header[0] = (float)count;
        stats.maxPerTile = std::max(stats.maxPerTile, count);
        total += count;
    }
    stats.averagePerTile = (float)total / (float)tiles;
}
