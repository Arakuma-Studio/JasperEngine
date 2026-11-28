// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Bake to Texture (docs/specs/V1-vertex-colors.md R62): a target's colors rasterized into a PNG in UV
// space on the CPU, corner values interpolated over each triangle (hard edges stay hard), the island
// borders grown by `padding` texels of dilation so mip levels and bilinear reads do not bleed the
// background in, written through AtomicFile and imported.

#include "VertexColorEditing.h"
#include "VertexColorEditorInternal.h"

#include "Project.h"
#include "io/AtomicFile.h"
#include "io/watch/FileWatchService.h"
#include "util/FileUtils.h"
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::vertexcolor {

namespace vcd = jasperengine::editor::vertexcolor::detail;

namespace {

    constexpr int kVcBakeMinSize = 16;
    constexpr int kVcBakeMaxSize = 8192;

    float vcEdge(float ax, float ay, float bx, float by, float px, float py) {
        return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
    }

}

bool bakeToTexture(Project* project, uint32_t sceneId, Entity entity, const BakeTextureOptions& options, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const vc::TargetData* data = targetData(project, sceneId, entity, options.key);
    if (!data) return fail(options.key.describe() + " is not painted: paint it first.");
    const std::shared_ptr<const vc::MeshView> mesh = meshView(project, sceneId, entity, options.key);
    if (!mesh) return fail("The mesh of " + options.key.describe() + " is not loaded yet.");
    if (mesh->fingerprint() != data->fingerprint) return fail(options.key.describe() + " was painted on another version of its mesh: open and save the scene first.");
    if (options.uvSet != 0 && options.uvSet != 1) return fail("uvSet: 0 or 1.");
    const std::vector<Vector2>& uv = options.uvSet == 1 ? mesh->uv1 : mesh->uv0;
    if (uv.size() != mesh->positions.size()) return fail("The mesh has no UV set " + std::to_string(options.uvSet) + " to bake into.");
    if (options.size < kVcBakeMinSize || options.size > kVcBakeMaxSize) {
        return fail("size: between " + std::to_string(kVcBakeMinSize) + " and " + std::to_string(kVcBakeMaxSize) + " texels.");
    }
    if (options.padding < 0 || options.padding > 64) return fail("padding: 0 to 64 texels.");
    if (FileUtils::pathFromUtf8(options.path).extension() != ".png") return fail("path: Bake to Texture writes a PNG; end the name with .png.");
    const fs::path file = vcd::absoluteAssetPath(*project, options.path);
    if (!project->isInsideAssetsPath(file)) return fail("path: '" + options.path + "' is outside the project's assets.");

    const int size = options.size;
    std::vector<Vector4> texels((size_t)size * size, Vector4(0.0f, 0.0f, 0.0f, 0.0f));
    std::vector<uint8_t> covered((size_t)size * size, 0);
    for (uint32_t t = 0; t < mesh->triangleCount(); t++) {
        float x[3], y[3];
        Vector4 value[3];
        for (int k = 0; k < 3; k++) {
            const uint32_t vertex = mesh->indices[t * 3 + (uint32_t)k];
            // glTF's UV origin is the image's top left
            x[k] = uv[vertex].x * (float)size;
            y[k] = uv[vertex].y * (float)size;
            value[k] = data->cornerValue(t * 3 + (uint32_t)k, mesh->indices);
        }
        const float area = vcEdge(x[0], y[0], x[1], y[1], x[2], y[2]);
        if (std::fabs(area) < 1e-12f) continue;
        const int x0 = std::max(0, (int)std::floor(std::min({x[0], x[1], x[2]})));
        const int x1 = std::min(size - 1, (int)std::ceil(std::max({x[0], x[1], x[2]})));
        const int y0 = std::max(0, (int)std::floor(std::min({y[0], y[1], y[2]})));
        const int y1 = std::min(size - 1, (int)std::ceil(std::max({y[0], y[1], y[2]})));
        for (int py = y0; py <= y1; py++) {
            for (int px = x0; px <= x1; px++) {
                const float cx = (float)px + 0.5f, cy = (float)py + 0.5f;
                const float w0 = vcEdge(x[1], y[1], x[2], y[2], cx, cy) / area;
                const float w1 = vcEdge(x[2], y[2], x[0], y[0], cx, cy) / area;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f) continue;
                const size_t at = (size_t)py * size + px;
                texels[at] = value[0] * w0 + value[1] * w1 + value[2] * w2;
                covered[at] = 1;
            }
        }
    }
    // dilation: each ring takes the mean of its covered neighbours
    std::vector<uint8_t> next;
    for (int ring = 0; ring < options.padding; ring++) {
        next = covered;
        bool grew = false;
        for (int py = 0; py < size; py++) {
            for (int px = 0; px < size; px++) {
                const size_t at = (size_t)py * size + px;
                if (covered[at]) continue;
                Vector4 sum(0.0f, 0.0f, 0.0f, 0.0f);
                int count = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        const int nx = px + dx, ny = py + dy;
                        if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= size || ny >= size) continue;
                        const size_t n = (size_t)ny * size + nx;
                        if (!covered[n]) continue;
                        sum = sum + texels[n];
                        count++;
                    }
                }
                if (!count) continue;
                texels[at] = sum / (float)count;
                next[at] = 1;
                grew = true;
            }
        }
        covered.swap(next);
        if (!grew) break;
    }

    std::vector<uint8_t> pixels((size_t)size * size * 4);
    for (size_t i = 0; i < texels.size(); i++) {
        const float c[4] = {texels[i].x, texels[i].y, texels[i].z, texels[i].w};
        for (int k = 0; k < 4; k++) {
            float v = std::min(1.0f, std::max(0.0f, c[k]));
            if (options.srgb && k < 3) v = vc::linearToSrgb(v);
            pixels[i * 4 + (size_t)k] = (uint8_t)(v * 255.0f + 0.5f);
        }
    }
    std::string png;
    auto append = [](void* context, void* bytes, int count) { static_cast<std::string*>(context)->append(static_cast<const char*>(bytes), (size_t)count); };
    if (!stbi_write_png_to_func(append, &png, size, size, 4, pixels.data(), size * 4)) return fail("The PNG could not be encoded.");
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    {
        ScopedExpectedWrite expected(file);
        const io::WriteResult written = io::writeFileAtomic(file, std::string_view(png));
        if (!written.ok()) return fail("Could not write '" + options.path + "': " + io::describeWriteError(written));
    }
    project->onAssetFilesChanged(project->getAssetDatabase().importAssets(file));
    return true;
}

}  // namespace jasperengine::editor::vertexcolor
