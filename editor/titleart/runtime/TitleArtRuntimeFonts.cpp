// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtRuntimeFonts.h"

#include "Project.h"
#include "asset/AssetFileWrite.h"
#include "asset/AssetIndex.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtRefs.h"
#include "titleart/TitleArtRenderHost.h"
#include "titleart/runtime/TitleCharacters.h"

#include <fstream>
#include <iterator>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    struct ShippedFont {
        titleart::ResolvedTitleAsset asset;
        TitleCharacterSet characters;
        std::set<std::string> documents;    // assets-relative runtime documents that read it
    };

    bool readFile(const fs::path& path, std::string& out){
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }

    bool isCollection(const std::vector<uint8_t>& bytes){
        return bytes.size() >= 4 && bytes[0] == 't' && bytes[1] == 't' && bytes[2] == 'c' && bytes[3] == 'f';
    }

    // the font is read by Title Art documents only (no component, page or script names it)
    bool onlyTitlesReference(const Project* project, const titleart::ResolvedTitleAsset& asset, const std::string& projectRelative){
        const AssetIndex& index = project->getAssetIndex();
        if (!index.isOpen() || !index.isReady()) return false;
        const std::vector<DepEdge> edges = index.referencesTo(Guid::parse(asset.guid), projectRelative, false, 0);
        if (edges.empty()) return false;
        for (const DepEdge& edge : edges)
            if (edge.kind != DepEdgeKind::TitleArtAsset) return false;
        return true;
    }

}

TitleArtFontShipResult editor::shipTitleArtFonts(Project* project, const TitleArtExportPlan& plan, const fs::path& assetsDst){
    TitleArtFontShipResult result;
    if (!project || plan.runtimeTitles.empty()) return result;
    const fs::path assetsRoot = project->getAssetsPath();
    const fs::path assetsRelative = assetsRoot.lexically_relative(project->getProjectPath());
    const std::string assetsPrefix = assetsRelative.empty() || assetsRelative == "." ? std::string() : assetsRelative.generic_string() + "/";
    EditorTitleAssetResolver resolver(&project->getAssetDatabase(), project->getProjectPath(), assetsRoot);
    const TitleCharacterFileReader readCustomFile = titleCharacterFileReader(resolver);

    // the fonts and the characters each must keep
    std::map<std::string, ShippedFont> fonts;      // by assets-relative path
    for (const std::string& title : plan.runtimeTitles){
        std::string text;
        if (!readFile(assetsRoot / title, text)){
            result.warnings.push_back("Title Art " + title + ": cannot read the document for its fonts");
            continue;
        }
        const titleart::TitleArtParseResult parsed = titleart::parseTitleArt(text);
        if (!parsed.ok){
            result.warnings.push_back("Title Art " + title + ": " + parsed.error.toString());
            continue;
        }
        TitleCharactersReport report;
        TitleCharacterSet characters = titleDocumentCharacters(parsed.doc, readCustomFile, &report);
        for (const std::string& warning : report.warnings) result.warnings.push_back("Title Art " + title + ": " + warning);
        for (const TitleArtShippedInstance& instance : plan.instances){
            if (instance.title != title) continue;
            characters.addUtf8(instance.additionalCharacters, true);
            for (const TitleArtOverride& o : instance.overrides)
                if (o.type == "text") characters.addUtf8(o.text, true);
        }
        titleart::visitTitleArtRefs(parsed.doc.root, [&](const titleart::TitleAssetRef& ref, const std::string&, titleart::TitleAssetKind kind) {
            if (kind != titleart::TitleAssetKind::Font || ref.empty()) return true;
            const titleart::ResolvedTitleAsset asset = resolver.resolve(ref, kind);
            if (!asset.found){
                result.warnings.push_back("Title Art " + title + ": the font " + ref.path + " is missing: the title draws with the built-in font");
                return true;
            }
            ShippedFont& font = fonts[asset.assetPath];
            font.asset = asset;
            font.characters.merge(characters);
            font.documents.insert(title);
            return true;
        });
    }

    TitleFontSubsetter subsetter(TitleFontSubsetter::projectCacheDir(project->getProjectPath()));
    std::map<std::string, std::map<std::string, std::string>> relinks;     // document -> font path -> subset path
    for (auto& [path, font] : fonts){
        std::vector<uint8_t> bytes;
        std::string error;
        if (!resolver.readBytes(font.asset, bytes, &error)){
            result.warnings.push_back("Title Art font " + path + ": " + error + " (it ships as it is)");
            continue;
        }
        result.bytesBefore += bytes.size();
        TitleFontInfo info;
        if (!readTitleFontInfo(bytes, 0, info, error)){
            result.warnings.push_back("Title Art font " + path + ": " + error + " (it ships as it is)");
            result.bytesAfter += bytes.size();
            continue;
        }
        const TitleFontEmbeddingCheck check = checkTitleFontEmbedding(info, titleFontEmbeddingAcknowledged(assetsRoot / path), path);
        if (!check.ok){
            result.ok = false;
            result.error = check.message;
            return result;
        }
        if (check.shipWhole || isCollection(bytes)){
            result.warnings.push_back(check.shipWhole ? check.message
                                                      : "W-TITLEART-FONT-COLLECTION: " + path + " is a font collection: it ships whole (its "
                                                        "subset would be one face); a single-face font file ships a subset");
            result.credits.push_back(titleFontCredit(info, assetsPrefix + path, false));
            result.shipped.push_back(path + " (whole, " + std::to_string(bytes.size() / 1024) + " KiB)");
            result.bytesAfter += bytes.size();
            continue;
        }
        TitleSubsetRequest request;
        request.bytes = bytes;
        request.codepoints = font.characters.list();
        const TitleSubsetResult subset = subsetter.subset(request);
        if (!subset.ok){
            result.warnings.push_back("Title Art font " + path + ": the subset failed (" + subset.error + "): it ships whole");
            result.credits.push_back(titleFontCredit(info, assetsPrefix + path, false));
            result.bytesAfter += bytes.size();
            continue;
        }
        const fs::path original(path);
        const std::string subsetPath = (original.parent_path() / (original.stem().string() + ".subset-" + titleFontHash(bytes).substr(0, 8) +
                                                                 font.characters.hash().substr(0, 8) + original.extension().string())).generic_string();
        const fs::path target = assetsDst / subsetPath;
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        if (!assetio::writeFile(target, std::string_view(reinterpret_cast<const char*>(subset.bytes.data()), subset.bytes.size()),
                                assetio::Durability::Cache, &error)){
            result.ok = false;
            result.error = "cannot write " + target.string() + ": " + error;
            return result;
        }
        for (const std::string& document : font.documents) relinks[document][path] = subsetPath;
        if (onlyTitlesReference(project, font.asset, assetsPrefix + path)) fs::remove(assetsDst / path, ec);
        TitleFontCredit credit = titleFontCredit(info, assetsPrefix + subsetPath, true);
        if (subset.renamed) credit.family = subset.family;
        result.credits.push_back(credit);
        result.shipped.push_back(path + " -> " + subsetPath + " (" + std::to_string(font.characters.size()) + " characters, " +
                                 std::to_string(subset.glyphs) + " glyphs, " + std::to_string(subset.bytes.size() / 1024) + " KiB of " +
                                 std::to_string(bytes.size() / 1024) + " KiB" + (subset.renamed ? ", renamed (Reserved Font Name)" : "") + ")");
        result.bytesAfter += subset.bytes.size();
    }

    // the build's copies of the documents name the subsets
    for (const auto& [document, paths] : relinks){
        const fs::path shipped = assetsDst / document;
        std::string text;
        if (!readFile(shipped, text)){
            result.ok = false;
            result.error = "the build has no copy of " + document + " to point at its font subsets";
            return result;
        }
        titleart::TitleArtParseResult parsed = titleart::parseTitleArt(text);
        if (!parsed.ok){
            result.ok = false;
            result.error = "the build's copy of " + document + " does not parse: " + parsed.error.toString();
            return result;
        }
        titleart::visitTitleArtRefs(parsed.doc.root, [&](titleart::Json& value, const std::string&, titleart::TitleAssetKind kind) {
            if (kind != titleart::TitleAssetKind::Font) return true;
            const titleart::TitleAssetRef ref = titleart::decodeAssetRef(value);
            if (ref.empty()) return true;
            const titleart::ResolvedTitleAsset asset = resolver.resolve(ref, kind);
            auto it = asset.found ? paths.find(asset.assetPath) : paths.end();
            if (it != paths.end()) value = titleart::encodeAssetRef(titleart::TitleAssetRef{it->second, ""});
            return true;
        });
        std::string error;
        if (!assetio::writeFile(shipped, titleart::writeTitleArt(parsed.doc), assetio::Durability::Cache, &error)){
            result.ok = false;
            result.error = "cannot write " + shipped.string() + ": " + error;
            return result;
        }
    }
    return result;
}
