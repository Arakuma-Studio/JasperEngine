// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUiImportMaintenance.h"

#include "ImportManifestWriter.h"

#include "htmlui/HtmlUIDependencies.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor::htmlui;
using jasperengine::htmlui::compat::ImportEntry;
using jasperengine::htmlui::compat::ImportManifest;
namespace fs = std::filesystem;

namespace {

    bool isDocumentFile(const fs::path& path){
        const std::string extension = lowerAscii(path.extension().string());
        return extension == ".html" || extension == ".htm" || extension == ".css" || extension == ".rml" || extension == ".rcss";
    }

    bool skippedDirectory(const fs::path& name){
        const std::string text = name.string();
        return text.empty() || text[0] == '.' || text == "build";
    }

}

std::vector<RemoteResourceState> jasperengine::editor::htmlui::remoteResourceStates(const ImportTarget& target, const std::string& documentPath){
    std::vector<RemoteResourceState> states;
    const ImportManifest manifest = ImportManifestWriter(target.assetsDir).read();
    std::error_code ec;
    for (const DiscoveredReference& reference : discoverRemoteReferences(target, documentPath)){
        RemoteResourceState state;
        state.reference = reference;
        if (const ImportEntry* entry = manifest.find(reference.url)){
            state.localPath = entry->path;
            state.licenseName = entry->licenseName;
            state.imported = fs::is_regular_file(target.toAbsolute(entry->path), ec);
            state.fileMissing = !state.imported;
        }
        states.push_back(std::move(state));
    }
    return states;
}

std::vector<std::string> jasperengine::editor::htmlui::findProjectDocuments(const ImportTarget& target){
    std::vector<std::string> documents;
    std::error_code ec;
    if (!fs::is_directory(target.assetsDir, ec))
        return documents;
    fs::recursive_directory_iterator it(target.assetsDir, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)){
        if (it->is_directory(ec)){
            if (skippedDirectory(it->path().filename()))
                it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec) || !isDocumentFile(it->path()))
            continue;
        const std::string assetPath = target.toAssetPath(it->path());
        if (!assetPath.empty())
            documents.push_back(assetPath);
    }
    std::sort(documents.begin(), documents.end());
    return documents;
}

RemoveUnusedResult jasperengine::editor::htmlui::removeUnusedImports(const ImportTarget& target){
    RemoveUnusedResult result;
    ImportManifestWriter writer(target.assetsDir);
    const ImportManifest manifest = writer.read();
    if (manifest.empty())
        return result;

    // what the documents reference
    std::set<std::string> referenced;
    const compat::FileAccess files = target.fileAccess();
    const fs::path assetsDir = target.assetsDir;
    const HtmlUIFileReader reader = [assetsDir](const std::string& path, std::string* content){
        const fs::path file = assetsDir / editor::FileUtils::pathFromUtf8(path);
        std::error_code ec;
        if (!fs::is_regular_file(file, ec))
            return false;
        if (content){
            std::ifstream in(file, std::ios::binary);
            std::ostringstream stream;
            stream << in.rdbuf();
            *content = stream.str();
        }
        return true;
    };
    for (const std::string& document : findProjectDocuments(target)){
        for (const DiscoveredReference& reference : discoverRemoteReferences(target, document))
            referenced.insert(ImportManifest::normalizeUrl(reference.url));
        for (const HtmlUIReference& reference : collectHtmlUIReferences(document, reader)){
            if (reference.remote)
                referenced.insert(ImportManifest::normalizeUrl(reference.raw.empty() ? reference.path : reference.raw));
        }
    }

    // entries referenced directly, then the ones whose file a used entry lists
    std::set<std::string> used;
    for (const ImportEntry& entry : manifest.entries()){
        if (referenced.count(entry.url))
            used.insert(entry.url);
    }
    for (bool grew = true; grew;){
        grew = false;
        std::set<std::string> usedFiles;
        for (const ImportEntry& entry : manifest.entries()){
            if (!used.count(entry.url))
                continue;
            usedFiles.insert(entry.path);
            usedFiles.insert(entry.files.begin(), entry.files.end());
        }
        for (const ImportEntry& entry : manifest.entries()){
            if (!used.count(entry.url) && usedFiles.count(entry.path)){
                used.insert(entry.url);
                grew = true;
            }
        }
    }

    std::set<std::string> keptFiles;
    std::vector<const ImportEntry*> unused;
    for (const ImportEntry& entry : manifest.entries()){
        if (used.count(entry.url)){
            keptFiles.insert(entry.path);
            keptFiles.insert(entry.files.begin(), entry.files.end());
            if (!entry.license.empty())
                keptFiles.insert(entry.license);
        }else{
            unused.push_back(&entry);
        }
    }
    if (unused.empty())
        return result;

    std::set<std::string> toDelete;
    for (const ImportEntry* entry : unused){
        result.removedUrls.push_back(entry->url);
        toDelete.insert(entry->path);
        toDelete.insert(entry->files.begin(), entry->files.end());
        if (!entry->license.empty())
            toDelete.insert(entry->license);
    }
    if (!writer.remove(result.removedUrls, &result.error))
        return result;

    std::error_code ec;
    for (const std::string& file : toDelete){
        if (file.empty() || keptFiles.count(file))
            continue;
        const fs::path absolute = target.toAbsolute(file);
        if (target.toAssetPath(absolute).empty())
            continue;   // never outside the assets
        if (fs::remove(absolute, ec))
            result.deletedFiles.push_back(file);
        // directories the import created and left empty
        fs::path parent = absolute.parent_path();
        while (!target.toAssetPath(parent).empty() && fs::is_directory(parent, ec) && fs::is_empty(parent, ec)){
            fs::remove(parent, ec);
            parent = parent.parent_path();
        }
    }
    return result;
}
