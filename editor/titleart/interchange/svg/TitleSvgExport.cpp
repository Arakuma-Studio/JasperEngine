// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// export_title's `svg` format (docs/specs/S87-title-localisation-interchange.md R41): per language
// the document resolved for it (fit applied), its trimmed export frame (S80's TitleFrame through
// TitleExport's measure), with `embed_raster` the non-vector layers rendered alone on that frame
// first, then SvgWriter's outlines as `<stem>[.<code>].svg`; the writer's warnings name each layer
// left out, and `strict` fails with the list.

#include "SvgWriter.h"

#include "titleart/TitleTranslations.h"
#include "titleart/interchange/TitleExport.h"

#include <map>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::TitleRectI;

namespace {

    struct SvgLanguage {
        std::string code;
        std::shared_ptr<const titleart::TitleArtDoc> doc;
        TitleRectI frame;
        double scale = 1.0;
        std::map<std::string, TitleSvgRaster> rasters;      // by layer id
    };

    bool planSvg(TitleExportContext& ctx) {
        for (const char* key : {"embed_raster", "strict"}) {
            if (ctx.request.options.contains(key) && !ctx.request.options[key].is_boolean()) {
                return ctx.fail("bad_option", std::string(key) + " is true or false");
            }
        }
        const bool embed = ctx.request.options.value("embed_raster", false);
        const bool strict = ctx.request.options.value("strict", false);
        std::vector<std::string> codes = ctx.request.languages;
        if (codes.empty()) codes.push_back("");
        TitleExportContext* c = &ctx;
        for (const std::string& code : codes) {
            auto language = std::make_shared<SvgLanguage>();
            language->code = code;
            language->doc = std::make_shared<titleart::TitleArtDoc>(ctx.resolved(code));
            ctx.measure(language->doc, "", "", [c, language, embed, strict](TitleRenderedImage&, const TitleRectI& trimmed) {
                language->scale = c->image.scale;
                language->frame = c->finishFrame(trimmed, language->scale);
                c->result.frames[language->code.empty() ? titleart::titleSourceLanguage(c->request.doc.root) : language->code] =
                    TitleJson::array({language->frame.x, language->frame.y, language->frame.width, language->frame.height});
                if (embed) {
                    for (const std::string& layer : titleSvgRasterLayers(*language->doc)) {
                        c->renderFrame(language->doc, "", language->frame, language->scale, layer, [c, language, layer](texsynth::Image& pixels) {
                            TitleSvgRaster raster;
                            raster.frame = language->frame;
                            if (c->encodePng(pixels, language->code, raster.png)) language->rasters[layer] = std::move(raster);
                        });
                    }
                }
                c->then([c, language, embed, strict] {
                    TitleSvgOptions options;
                    options.scale = language->scale;
                    options.embedRaster = embed;
                    options.strict = strict;
                    const TitleSvgResult svg = writeTitleSvg(*language->doc, language->frame, options, TitleArtRenderHost::editor().resolver(c->request.project),
                                                             [language](const titleart::Json& layer, TitleSvgRaster& out) {
                                                                 auto it = language->rasters.find(layer.value("id", std::string()));
                                                                 if (it == language->rasters.end()) return false;
                                                                 out = it->second;
                                                                 return true;
                                                             });
                    for (const std::string& w : svg.warnings) c->warn(w);
                    if (!svg.ok) return c->fail(svg.code.empty() ? "svg_failed" : svg.code, svg.error);
                    return c->write(c->fileName("", language->code, "svg"), svg.text);
                });
            });
        }
        return true;
    }

}

void editor::registerTitleSvgExport() {
    TitleExportFormat svg;
    svg.name = "svg";
    svg.optionKeys = {"embed_raster", "strict"};
    svg.plan = planSvg;
    registerTitleExportFormat(std::move(svg));
}
