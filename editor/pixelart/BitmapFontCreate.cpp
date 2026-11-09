// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/BitmapFontCreate.h"

#include "Project.h"
#include "util/ProjectFileWrites.h"
#include "pixelart/BitmapFont.h"
#include "pixelart/BitmapFontParsers.h"
#include "util/FontFace.h"
#include "json.hpp"

#include <fstream>

namespace fs = std::filesystem;

namespace jasperengine::editor::pixelart {

    namespace{
        namespace px = ::jasperengine::pixelart;

        std::string createExtension(const fs::path& path){
            std::string ext = path.extension().string();
            for (char& c : ext) c = (char)std::tolower((unsigned char)c);
            return ext;
        }

        std::vector<uint8_t> createRead(const fs::path& path){
            std::ifstream in(path, std::ios::binary);
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }

        // the engine's path of a project file (relative to the assets folder)
        std::string createEnginePath(const fs::path& file, const fs::path& assetsRoot){
            const fs::path rel = file.lexically_normal().lexically_relative(assetsRoot.lexically_normal());
            return (rel.empty() || rel.generic_string().rfind("..", 0) == 0) ? file.generic_string() : rel.generic_string();
        }
    }

    bool looksLikeFontSheet(const std::string& path, int& cellWidth, int& cellHeight){
        return createExtension(fs::path(path)) == ".png" && px::cellFromFileName(path, cellWidth, cellHeight);
    }

    int detectedPixelFontSize(const fs::path& source, const fs::path& assetsRoot){
        std::shared_ptr<FontFace> face = FontFace::loadFile(createEnginePath(source, assetsRoot));
        return face ? px::detectPixelSize(*face) : 0;
    }

    bool bitmapFontFor(const fs::path& source, const fs::path& assetsRoot, std::string& json, std::string& error){
        const std::string ext = createExtension(source);
        const std::string name = source.filename().generic_string();
        nlohmann::json doc = {{"format", "jasperengine.bitmapfont"}, {"version", px::kBitmapFontVersion}};
        if (ext == ".png"){
            nlohmann::json src = {{"type", "grid"}, {"image", name}};
            int w = 0, h = 0;
            if (!px::cellFromFileName(name, w, h)){
                px::DecodedPage image;
                int gx = 0, gy = 0;
                if (!px::decodeImage(createRead(source), image) || !px::detectGridCell(image.rgba.data(), image.width, image.height, w, h, gx, gy)){
                    error = name + " has no cell size: name it <name>@WxH.png (for example hud@6x9.png), or leave transparent gutters between cells";
                    return false;
                }
                src["cell"] = {w, h};
                if (gx || gy) src["gap"] = {gx, gy};
            }
            doc["source"] = src;
        }else if (ext == ".fnt"){
            doc["source"] = {{"type", "bmfont"}, {"file", name}};
        }else if (ext == ".ttf" || ext == ".otf"){
            const int size = detectedPixelFontSize(source, assetsRoot);
            if (size <= 0){
                error = name + " is not a pixel font (its outlines sit on no pixel grid of 5 to 64 px): it stays a vector font";
                return false;
            }
            doc["source"] = {{"type", "truetype"}, {"file", name}, {"pixel_size", size}};
        }else{
            error = "Create Bitmap Font takes a PNG sheet, a BMFont .fnt or a TTF/OTF pixel font";
            return false;
        }
        // what detection found goes into the file, so loading never detects again (D8)
        const std::string probe = doc.dump();
        const fs::path fontPath = source.parent_path() / (source.stem().string() + ".bitmapfont");
        px::BitmapFontReport report;
        std::shared_ptr<px::BitmapFont> font = px::BitmapFont::loadSettings(createEnginePath(fontPath, assetsRoot), probe, &report);
        if (!font){
            error = report.errors.empty() ? name + " does not make a font" : report.errors[0].message + " (" + report.errors[0].fix + ")";
            return false;
        }
        if (doc["source"]["type"] == "grid")
            doc["baseline"] = font->getBaseline();
        json = px::canonicalBitmapFontJson(doc.dump());
        return true;
    }

    fs::path createBitmapFont(Project* project, const fs::path& source, std::string& error){
        std::string json;
        if (!bitmapFontFor(source, project->getAssetsPath(), json, error))
            return {};
        // hud@6x9.png -> hud.bitmapfont; a taken name gets " 1", " 2"...
        std::string stem = source.stem().string();
        const size_t at = stem.find_last_of('@');
        if (at != std::string::npos && at > 0) stem = stem.substr(0, at);
        fs::path target = source.parent_path() / (stem + ".bitmapfont");
        std::error_code ec;
        for (int n = 1; fs::exists(target, ec) && n < 1000; n++)
            target = source.parent_path() / (stem + " " + std::to_string(n) + ".bitmapfont");
        if (!ProjectFileWrites::writeUndoable(project, {{target, json}}, error, nullptr, nullptr, {}, false,
                                              "Create Bitmap Font " + target.filename().string()))
            return {};
        return target;
    }

}
