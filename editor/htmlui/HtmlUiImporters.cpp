// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUiImporters.h"

#include "GoogleFontsImporter.h"
#include "ImportManifestWriter.h"
#include "RemoteImporter.h"

#include "Out.h"
#include "util/FileUtils.h"

// the built-in Lucide pack (p2-runtime); included unconditionally: ccache's direct mode does not
// see a __has_include that turns true and can return an object built without the header
#include "htmlui/compat/runtime/LucideIcons.h"
#define JASPER_HTMLUI_HAS_LUCIDE 1

#include <fstream>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor::htmlui;
using jasperengine::editor::Out;
using jasperengine::htmlui::compat::ImportEntry;
using jasperengine::htmlui::compat::ImportManifest;
namespace fs = std::filesystem;

namespace {

    // logs what an import reported, the way compat logs a document's diagnostics
    void logDiagnostics(const std::vector<compat::Diagnostic>& diagnostics){
        for (const compat::Diagnostic& diagnostic : diagnostics){
            const std::string text = compat::toText(diagnostic);
            switch (diagnostic.severity){
                case compat::Severity::Error: Out::error("[HTML UI] %s", text.c_str()); break;
                case compat::Severity::Warning: Out::warning("[HTML UI] %s", text.c_str()); break;
                case compat::Severity::Info: Out::info("[HTML UI] %s", text.c_str()); break;
            }
        }
    }

    void addFailure(ImportResult& result, const std::string& what, const std::string& error){
        result.failed.emplace_back(what, error);
        if (!result.error.empty())
            result.error += "\n";
        result.error += what + ": " + error;
    }

    // one imported resource into the facade's result
    void collect(const ImportTarget& target, const ImportedResource& resource, ImportResult& result){
        for (const std::string& file : resource.written){
            const std::string projectPath = target.toProjectRelative(file);
            if (std::find(result.written.begin(), result.written.end(), projectPath) == result.written.end())
                result.written.push_back(projectPath);
        }
        if (!resource.ok){
            addFailure(result, resource.url, resource.error.empty() ? std::string("import failed") : resource.error);
            return;
        }
        if (const ImportEntry* entry = resource.entry()){
            result.replaced.emplace_back(resource.url, target.toProjectRelative(entry->path));
            if (entry->kind == "google-fonts-css" && result.fontFaceCss.empty())
                result.fontFaceCss = target.toProjectRelative(entry->path);
        }
    }

    void finishResult(RemoteImporter& importer, ImportResult& result){
        result.diagnostics = importer.takeDiagnostics();
        compat::sortDiagnostics(result.diagnostics);
        logDiagnostics(result.diagnostics);
        result.ok = result.failed.empty();
    }

    std::unique_ptr<RemoteImporter> makeImporter(const fs::path& projectRoot, const std::string& destDir, ImportResult& result){
        ImportTarget target;
        std::string error;
        if (!ImportTarget::forProject(projectRoot, destDir, target, error)){
            addFailure(result, destDir, error);
            return nullptr;
        }
        auto importer = std::make_unique<RemoteImporter>(target);
        importer->setProgress([](const std::string& line){ Out::info("%s", line.c_str()); });
        return importer;
    }

    // "ArrowRight" -> "arrow-right", "lucide:play" -> "play"
    std::string lucideName(std::string name){
        if (name.compare(0, 7, "lucide:") == 0)
            name.erase(0, 7);
        std::string kebab;
        for (size_t i = 0; i < name.size(); i++){
            const char c = name[i];
            if (c >= 'A' && c <= 'Z'){
                if (i > 0 && name[i - 1] != '-')
                    kebab += '-';
                kebab += (char)(c - 'A' + 'a');
            }else if (c == ' ' || c == '_'){
                kebab += '-';
            }else{
                kebab += c;
            }
        }
        return kebab;
    }

}

bool jasperengine::editor::htmlui::lucideIconsAvailable(){
#ifdef JASPER_HTMLUI_HAS_LUCIDE
    return true;
#else
    return false;
#endif
}

bool jasperengine::editor::htmlui::lucideIconSvg(const std::string& name, std::string& svg, std::string* suggestion){
    svg.clear();
#ifdef JASPER_HTMLUI_HAS_LUCIDE
    const std::string key = lucideName(name);
    const auto inner = jasperengine::htmlui::compat::LucideIcons::find(key);
    if (!inner){
        if (suggestion){
            suggestion->clear();
            for (const auto& candidate : jasperengine::htmlui::compat::LucideIcons::closest(key, 3))
                *suggestion += (suggestion->empty() ? "" : ", ") + std::string(candidate);
        }
        return false;
    }
    // Lucide's own root attributes (the pack keeps only the shapes)
    svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" fill=\"none\" "
          "stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\" class=\"lucide lucide-" +
          key + "\">" + std::string(*inner) + "</svg>\n";
    return true;
#else
    (void)name;
    if (suggestion)
        suggestion->clear();
    return false;
#endif
}

ImportResult jasperengine::editor::htmlui::importGoogleFonts(const fs::path& projectRoot, const std::vector<std::string>& specs,
                                                            const std::string& destDir){
    ImportResult result;
    std::vector<GoogleFontsImporter::FamilyRequest> families;
    for (const std::string& spec : specs){
        GoogleFontsImporter::FamilyRequest request;
        std::string error;
        if (!GoogleFontsImporter::parseSpec(spec, request, error)){
            addFailure(result, spec, error);
            continue;
        }
        families.push_back(std::move(request));
    }
    if (families.empty()){
        if (result.failed.empty())
            addFailure(result, "fonts", "no font given (write them like \"Inter:400,700\")");
        return result;
    }

    std::unique_ptr<RemoteImporter> importer = makeImporter(projectRoot, destDir, result);
    if (!importer)
        return result;
    const ImportedResource resource = importer->import(GoogleFontsImporter::css2Url(families), "google-fonts-css");
    collect(importer->target(), resource, result);
    const bool specsFailed = !result.failed.empty() && resource.ok;
    finishResult(*importer, result);
    if (specsFailed)
        result.ok = false;
    return result;
}

ImportResult jasperengine::editor::htmlui::importLucideIcons(const fs::path& projectRoot, const std::vector<std::string>& names,
                                                            const std::string& destDir){
    ImportResult result;
    ImportTarget target;
    std::string error;
    if (!ImportTarget::forProject(projectRoot, destDir, target, error)){
        addFailure(result, destDir, error);
        return result;
    }
    if (destDir.empty())
        target.root = "htmlui-imports/icons";
    if (names.empty()){
        addFailure(result, "icons", "no icon given (Lucide names like \"play\" or \"lucide:play\")");
        return result;
    }
    if (!lucideIconsAvailable()){
        addFailure(result, "icons", "the Lucide icon pack is not built into this engine");
        return result;
    }
    for (const std::string& name : names){
        std::string svg;
        std::string suggestion;
        if (!lucideIconSvg(name, svg, &suggestion)){
            addFailure(result, name, "no Lucide icon named '" + lucideName(name) + "'" +
                       (suggestion.empty() ? std::string() : " (did you mean " + suggestion + "?)"));
            continue;
        }
        const std::string assetPath = (target.root.empty() ? "" : target.root + "/") + lucideName(name) + ".svg";
        if (!ImportManifestWriter::writeFileAtomic(target.toAbsolute(assetPath), svg, &error)){
            addFailure(result, name, error);
            continue;
        }
        const std::string projectPath = target.toProjectRelative(assetPath);
        result.written.push_back(projectPath);
        result.replaced.emplace_back(name, projectPath);
    }
    result.ok = result.failed.empty();
    return result;
}

ImportResult jasperengine::editor::htmlui::importUrl(const fs::path& projectRoot, const std::string& url, const std::string& destDir){
    ImportResult result;
    std::unique_ptr<RemoteImporter> importer = makeImporter(projectRoot, destDir, result);
    if (!importer)
        return result;
    const ImportedResource resource = importer->import(url);
    collect(importer->target(), resource, result);
    finishResult(*importer, result);
    return result;
}

ImportResult jasperengine::editor::htmlui::importDocumentReferences(const fs::path& projectRoot, const std::string& htmlOrCss,
                                                                   const std::string& destDir){
    ImportResult result;
    std::unique_ptr<RemoteImporter> importer = makeImporter(projectRoot, destDir, result);
    if (!importer)
        return result;
    const ImportTarget& target = importer->target();

    fs::path file = editor::FileUtils::pathFromUtf8(htmlOrCss);
    if (file.is_relative())
        file = target.projectRoot / file;
    const std::string assetPath = target.toAssetPath(file);
    std::error_code ec;
    if (assetPath.empty() || !fs::is_regular_file(file, ec)){
        addFailure(result, htmlOrCss, assetPath.empty() ? "not a file inside the project's assets directory" : "file not found");
        return result;
    }

    const ImportManifest manifest = ImportManifestWriter(target.assetsDir).read();
    for (const DiscoveredReference& reference : discoverRemoteReferences(target, assetPath)){
        const ImportEntry* entry = manifest.find(reference.url);
        if (entry && fs::is_regular_file(target.toAbsolute(entry->path), ec)){
            result.replaced.emplace_back(reference.url, target.toProjectRelative(entry->path));
            if (entry->kind == "google-fonts-css" && result.fontFaceCss.empty())
                result.fontFaceCss = target.toProjectRelative(entry->path);
            continue;
        }
        const ImportedResource resource = importer->import(reference.url, reference.kind, reference.location);
        collect(target, resource, result);
    }
    finishResult(*importer, result);
    return result;
}
