// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "tilemap/TilePageTextures.h"

#include "Backend.h"
#include "asset/ImportedAssets.h"
#include "io/FileData.h"
#include "texture/TextureData.h"
#include "tilemap/TilesetEdit.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::tilemap;

// file-local, in a namespace of its own: unity builds merge the editor's sources
namespace jasperengine::editor::tilepagetextures {

    // the stamp of the file a page reads (an imported "<file>#<key>" reads its imported file); 0
    // when there is none
    int64_t pageFileStamp(const std::string& pagePath) {
        std::string file = pagePath;
        if (file.find('#') != std::string::npos && !ImportedAssets::find(ImportedAssets::normalize(file), file)) return 0;
        std::error_code error;
        const auto time = std::filesystem::last_write_time(FileData::getSystemPath(file), error);
        return error ? 0 : (int64_t)time.time_since_epoch().count();
    }

}

using namespace jasperengine::editor::tilepagetextures;

TilePageTextures& TilePageTextures::get() {
    static TilePageTextures instance;
    return instance;
}

void TilePageTextures::beginFrame() {
    for (Texture& texture : retired) texture.destroy();
    retired.clear();
    frame++;
}

void TilePageTextures::clear() {
    for (auto& [path, entry] : entries) {
        if (!entry.texture.empty()) retired.push_back(entry.texture);
    }
    entries.clear();
}

ImTextureID TilePageTextures::page(const std::string& pagePath, const TilesetRuntime& tileset) {
    Entry& entry = entries[pagePath];
    bool retry = entry.generation != tileset.generation || (entry.texture.empty() && !entry.failed);
    if (!retry && entry.failed && entry.checkedFrame != frame) {
        // once a frame at most: the file's stamp, not its pixels
        entry.checkedFrame = frame;
        retry = pageFileStamp(pagePath) != entry.failedStamp;
    }
    if (retry) {
        if (!entry.texture.empty()) {
            retired.push_back(entry.texture);
            entry.texture = Texture();
        }
        entry.generation = tileset.generation;
        entry.loaded = false;
        entry.failed = false;
        decodeCount++;
        // the default loader reads what a TilesetRuntime lists: a texture path, an imported "<file>#<key>"
        TilesetPagePixels pixels;
        if (const PixelImage* image = pixels.get(pagePath)) {
            const size_t bytes = image->pixels().size();
            // TextureData frees what it is given (malloc'd)
            void* copy = std::malloc(bytes);
            if (copy) {
                std::memcpy(copy, image->pixels().data(), bytes);
                TextureData data(image->width(), image->height(), static_cast<unsigned int>(bytes), ColorFormat::RGBA, 4, copy);
                entry.texture.setData("editor:tileset-page:" + std::to_string(++uploads), data);
                entry.texture.setMinFilter(TextureFilter::NEAREST);
                entry.texture.setMagFilter(TextureFilter::NEAREST);
                TextureRender* render = entry.texture.getRender();
                entry.texture.releaseData();
                entry.loaded = render && render->isCreated();
            }
        }
        if (!entry.loaded) {
            entry.failed = true;
            entry.failedStamp = pageFileStamp(pagePath);
            entry.checkedFrame = frame;
        }
    }
    if (!entry.loaded) return ImTextureID{};
    return Backend::getImGuiTexture(entry.texture.getRender());
}

void jasperengine::editor::tilemap::drawTileImage(ImDrawList* list, ImVec2 min, float pixelZoom, const TilesetRuntime& tileset, int32_t tile, uint32_t flags, ImU32 tint) {
    const TileRenderInfo* info = tileset.tile(tile);
    if (!info || !list) return;
    ImTextureID texture = TilePageTextures::get().page(tileset.pages[info->page], tileset);
    if (!texture) return;
    const ImVec2 size(static_cast<float>(info->width) * pixelZoom, static_cast<float>(info->height) * pixelZoom);
    ImVec2 uv[4] = {ImVec2(info->u0, info->v0), ImVec2(info->u1, info->v0), ImVec2(info->u1, info->v1), ImVec2(info->u0, info->v1)};
    if (flags & TILE_TRANSPOSE) std::swap(uv[1], uv[3]);
    if (flags & TILE_FLIP_X) {
        std::swap(uv[0], uv[1]);
        std::swap(uv[3], uv[2]);
    }
    if (flags & TILE_FLIP_Y) {
        std::swap(uv[0], uv[3]);
        std::swap(uv[1], uv[2]);
    }
    const ImVec2 max(min.x + size.x, min.y + size.y);
    list->AddImageQuad(texture, min, ImVec2(max.x, min.y), max, ImVec2(min.x, max.y), uv[0], uv[1], uv[2], uv[3], tint);
}

void jasperengine::editor::tilemap::tileSourceGrid(const TilesetRuntime& tileset, int sourceId, std::vector<TileGridCell>& out, int& columns, int& rows) {
    out.clear();
    columns = 0;
    rows = 0;
    const TilesetData* data = tileset.data.get();
    if (!data) return;
    const TilesetSource* source = data->findSource(sourceId);
    if (!source) return;
    if (source->kind == TilesetSourceKind::Texture) {
        for (const TileDef& tile : data->tiles) {
            if (tile.source != sourceId) continue;
            out.push_back(TileGridCell{tile.id, tile.atX, tile.atY});
            columns = std::max(columns, tile.atX + 1);
            rows = std::max(rows, tile.atY + 1);
        }
        return;
    }
    int index = 0;
    for (const TileDef& tile : data->tiles) {
        if (tile.source != sourceId) continue;
        const int col = index % 10, row = index / 10;
        out.push_back(TileGridCell{tile.id, col, row});
        columns = std::max(columns, col + 1);
        rows = std::max(rows, row + 1);
        index++;
    }
}

void jasperengine::editor::tilemap::showTileTooltip(const TilesetRuntime& tileset, int32_t tile, int zoom) {
    const TilesetData* data = tileset.data.get();
    const TileDef* def = data ? data->findTile(tile) : nullptr;
    ImGui::BeginTooltip();
    if (const TileRenderInfo* info = tileset.tile(tile)) {
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        drawTileImage(ImGui::GetWindowDrawList(), cursor, static_cast<float>(zoom), tileset, tile);
        ImGui::Dummy(ImVec2(static_cast<float>(info->width * zoom), static_cast<float>(info->height * zoom)));
    }
    ImGui::Text("#%d  %s", tile, def ? data->tileName(*def).c_str() : "missing");
    if (def) {
        for (const auto& [name, value] : def->properties) {
            switch (value.type) {
                case TilePropertyType::Bool: ImGui::Text("%s: %s", name.c_str(), value.b ? "true" : "false"); break;
                case TilePropertyType::Int: ImGui::Text("%s: %lld", name.c_str(), (long long)value.i); break;
                case TilePropertyType::Float: ImGui::Text("%s: %g", name.c_str(), value.f); break;
                case TilePropertyType::String: ImGui::Text("%s: %s", name.c_str(), value.s.c_str()); break;
                case TilePropertyType::Color: ImGui::Text("%s: (%.2f, %.2f, %.2f, %.2f)", name.c_str(), value.color.x, value.color.y, value.color.z, value.color.w); break;
            }
        }
    }
    ImGui::EndTooltip();
}
