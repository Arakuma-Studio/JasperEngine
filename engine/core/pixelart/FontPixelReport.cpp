// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/FontPixelReport.h"

#include "pixelart/BitmapFontParsers.h"
#include "pixelart/BitmapFontPool.h"
#include "util/FontFace.h"

namespace jasperengine::pixelart {

    FontPixelReport analyzeFontForPixelArt(const std::string& path){
        FontPixelReport report;
        report.kind = fontKindOf(path);
        if (report.kind == FontKind::BITMAP){
            BitmapFontReport loaded;
            std::shared_ptr<BitmapFont> font = BitmapFontPool::get(path, &loaded);
            if (!font){
                report.loads = false;
                report.errors = loaded.errors;
                return report;
            }
            report.partialAlphaShare = font->getReport().partialAlphaShare;
            report.antialiased = report.partialAlphaShare > 0.0f;
            report.nativeSize = font->getNativeSize();
            return report;
        }
        // a vector font is antialiased; its outlines may still sit on a pixel grid (R4)
        report.antialiased = true;
        if (path.empty())
            return report;
        std::string error;
        std::shared_ptr<FontFace> face = FontFace::loadFile(path, 0, &error);
        if (!face){
            report.loads = false;
            FontError e;
            e.kind = "font_file_missing";
            e.message = error;
            e.fix = "check the path";
            e.field = "font";
            report.errors.push_back(e);
            return report;
        }
        report.nativeSize = detectPixelSize(*face);
        report.pixelGridDetected = report.nativeSize > 0;
        return report;
    }

    TextPixelReport analyzeTextForPixelArt(const std::vector<std::string>& fonts, unsigned int fontSize){
        TextPixelReport report;
        const std::string primary = fonts.empty() ? std::string() : fonts[0];
        report.font = analyzeFontForPixelArt(primary);
        if (report.font.kind != FontKind::BITMAP || !report.font.loads)
            return report;
        report.pixelScale = pixelScaleFor(fontSize, report.font.nativeSize);
        report.effectiveFontSize = report.pixelScale * report.font.nativeSize;
        report.sizeIsMultiple = (int)fontSize == report.effectiveFontSize;
        for (size_t i = 1; i < fonts.size(); i++)
            if (!fonts[i].empty() && fontKindOf(fonts[i]) == FontKind::VECTOR)
                report.vectorFallbacks.push_back(fonts[i]);
        return report;
    }

}
