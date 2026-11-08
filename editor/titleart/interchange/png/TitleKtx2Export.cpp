// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// export_title's `ktx2` format (docs/specs/S87-title-localisation-interchange.md R43): the PNG of
// each language rendered in memory, then S14's cook as a `ui` texture (sRGB, alpha is
// transparency, no mips unless asked) for one family, written as `<stem>[.<code>].ktx2`. For
// delivery outside the project: bakes inside it are cooked by S14 as usual.

#include "TitlePngExport.h"

#include "Project.h"
#include "texture/TextureCookQueue.h"
#include "texture/TextureCooker.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::TitleRectI;
namespace fs = std::filesystem;

namespace {

    bool readAll(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream buffer;
        buffer << in.rdbuf();
        out = buffer.str();
        return true;
    }

    bool planKtx2(TitleExportContext& ctx) {
        const TitleJson& o = ctx.request.options;
        TextureFamily family = TextureFamily::Desktop;
        const std::string familyName = o.value("family", std::string("desktop"));
        if (familyName == "mobile") family = TextureFamily::Mobile;
        else if (familyName == "web") family = TextureFamily::Web;
        else if (familyName != "desktop") return ctx.fail("bad_option", "family is desktop, mobile or web");
        TextureCookQuality quality = TextureCookQuality::High;
        const std::string qualityName = o.value("quality", std::string("high"));
        if (qualityName == "low") quality = TextureCookQuality::Low;
        else if (qualityName == "normal") quality = TextureCookQuality::Normal;
        else if (qualityName == "none") quality = TextureCookQuality::None;
        else if (qualityName != "high") return ctx.fail("bad_option", "quality is none, low, normal or high");
        if (o.contains("mipmaps") && !o["mipmaps"].is_boolean()) return ctx.fail("bad_option", "mipmaps is true or false");
        const bool mipmaps = o.value("mipmaps", false);
        if (!ctx.request.project) return ctx.fail("no_project", "a KTX2 export cooks with the project's texture settings");
        std::vector<std::string> languages = ctx.request.languages;
        if (languages.empty()) languages.push_back("");
        TitleExportContext* c = &ctx;
        for (const std::string& code : languages) {
            auto doc = std::make_shared<titleart::TitleArtDoc>(ctx.resolved(code));
            ctx.measure(doc, "", "", [c, doc, code, family, quality, mipmaps](TitleRenderedImage&, const TitleRectI& trimmed) {
                double scale = c->image.scale;
                int width = 0, height = 0;
                const TitleRectI frame = c->finishFrame(trimmed, scale, &width, &height);
                c->result.frames[code.empty() ? titleart::titleSourceLanguage(c->request.doc.root) : code] =
                    TitleJson::array({frame.x, frame.y, frame.width, frame.height});
                c->renderFrame(doc, "", frame, scale, "", [c, code, family, quality, mipmaps](texsynth::Image& pixels) {
                    std::vector<uint8_t> png;
                    if (!c->encodePng(pixels, code, png)) return;
                    // the cook reads a file: the PNG in the project's cache, cooked into its own cache
                    Project* project = c->request.project;
                    const fs::path work = project->getProjectInternalPath() / "cache" / "titleart" / "ktx2";
                    std::error_code ec;
                    fs::create_directories(work, ec);
                    const fs::path source = work / c->fileName("", code, "png");
                    {
                        std::ofstream out(source, std::ios::binary);
                        out.write(reinterpret_cast<const char*>(png.data()), (std::streamsize)png.size());
                        if (!out) {
                            c->fail("write_failed", "cannot write " + source.generic_string());
                            return;
                        }
                    }
                    TextureCookCache cache;
                    cache.open(work / "cache", "", 256ull << 20);
                    TextureCookRequest request;
                    request.source = source;
                    request.assetPath = c->fileName("", code, "png");
                    request.settings.type = TextureImportType::UI;
                    request.settings.srgb = true;
                    request.settings.alphaIsTransparency = true;
                    request.settings.mipmaps = mipmaps;
                    request.settings.quality = quality;
                    request.project = textureProjectSettingsOf(*project);
                    request.family = family;
                    const TextureCookResult cooked = TextureCooker::cook(cache, request, [] { return false; });
                    fs::remove(source, ec);
                    if (!cooked.ok) {
                        c->fail("cook_failed", "cooking the KTX2: " + cooked.error);
                        return;
                    }
                    std::string bytes;
                    if (!readAll(cache.artifactPath(cooked.plan.key), bytes)) {
                        c->fail("cook_failed", "the cooked KTX2 is not in the cache");
                        return;
                    }
                    cache.close();
                    c->result.extra["format_" + (code.empty() ? std::string("source") : code)] = cooked.record.label;
                    c->write(c->fileName("", code, "ktx2"), std::move(bytes));
                }, width, height);
            });
        }
        return true;
    }

}

void editor::registerTitleKtx2Export() {
    TitleExportFormat ktx2;
    ktx2.name = "ktx2";
    ktx2.optionKeys = {"family", "quality", "mipmaps"};
    ktx2.plan = planKtx2;
    registerTitleExportFormat(std::move(ktx2));
}
