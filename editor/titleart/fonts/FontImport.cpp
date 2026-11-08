// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FontImport.h"

#include "FontTables.h"
#include "WebFontDecode.h"

#include "Project.h"
#include "command/CommandHistory.h"
#include "htmlui/GoogleFontsImporter.h"
#include "htmlui/RemoteImporter.h"
#include "util/FileUtils.h"
#include "util/ProjectFileWrites.h"
#include "util/SystemFonts.h"

#include <fstream>
#include <iterator>
#include <memory>
#include <system_error>

namespace jasperengine::editor::titlefonts {

    namespace fs = std::filesystem;

    namespace {

        // larger than any font file seen (system CJK collections are about 100 MB)
        constexpr uintmax_t MAX_FONT_FILE_BYTES = 512ull * 1024 * 1024;

        bool readWhole(const fs::path& path, std::string& out, std::string& error) {
            std::error_code ec;
            const uintmax_t size = fs::file_size(path, ec);
            if (ec) {
                error = "cannot read '" + FileUtils::pathToUtf8(path) + "' (" + ec.message() + ")";
                return false;
            }
            if (size > MAX_FONT_FILE_BYTES) {
                error = "'" + FileUtils::pathToUtf8(path.filename()) + "' is larger than 512 MB";
                return false;
            }
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                error = "cannot open '" + FileUtils::pathToUtf8(path) + "'";
                return false;
            }
            out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (out.size() != size) {
                error = "cannot read all of '" + FileUtils::pathToUtf8(path) + "'";
                return false;
            }
            return true;
        }

        const uint8_t* bytesOf(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }

        std::string joinAssetPath(const std::string& folder, const std::string& fileName) {
            if (folder.empty()) return fileName;
            return folder.back() == '/' ? folder + fileName : folder + "/" + fileName;
        }

        FontImportPlan failed(std::string error) {
            FontImportPlan plan;
            plan.error = std::move(error);
            return plan;
        }

        // the family and style of the face the document will use, "" family -> the file stem
        void nameFace(FontImportPlan& plan, const std::string& bytes, int faceIndex, const fs::path& source) {
            const FontNames names = fontNames(bytesOf(bytes), bytes.size(), faceIndex);
            plan.family = names.family.empty() ? FileUtils::pathToUtf8(source.stem()) : names.family;
            plan.style = names.style;
            plan.undoName = importFontUndoName(plan.family);
        }

    }

    std::string importFontUndoName(const std::string& family) {
        return "Import Font " + family;
    }

    std::string freeFontFileName(const fs::path& directory, const std::string& fileName, const std::string& content, bool& identical) {
        identical = false;
        const fs::path given = FileUtils::pathFromUtf8(fileName);
        const std::string stem = FileUtils::pathToUtf8(given.stem());
        const std::string extension = FileUtils::pathToUtf8(given.extension());
        std::string candidate = fileName;
        for (int n = 1;; n++) {
            const fs::path path = directory / FileUtils::pathFromUtf8(candidate);
            std::error_code ec;
            if (!fs::exists(path, ec)) return candidate;
            if (fs::is_regular_file(path, ec) && fs::file_size(path, ec) == content.size() && !ec) {
                std::string existing, error;
                if (readWhole(path, existing, error) && existing == content) {
                    identical = true;
                    return candidate;
                }
            }
            candidate = stem + " " + std::to_string(n) + extension;
        }
    }

    FontImportPlan planFontCopy(const FontCopyRequest& request) {
        std::string bytes, error;
        if (!readWhole(request.source, bytes, error)) return failed(error);
        const std::string fileName = FileUtils::pathToUtf8(request.source.filename());
        const std::vector<uint32_t> faces = sfntFaceOffsets(bytesOf(bytes), bytes.size());
        if (faces.empty()) {
            if (isWebFontFileName(fileName)) return failed("'" + fileName + "' is a web font: convert it instead");
            return failed("'" + fileName + "' is not a TrueType or OpenType font");
        }
        if (request.faceIndex < 0 || (size_t)request.faceIndex >= faces.size()) {
            return failed("'" + fileName + "' has no face " + std::to_string(request.faceIndex) + " (it has " + std::to_string(faces.size()) + ")");
        }
        const OutlineKind kind = outlineKind(bytesOf(bytes), bytes.size(), request.faceIndex);
        if (kind == OutlineKind::None) return failed("'" + fileName + "' has no outlines (a bitmap-only font): Title Art draws outlines");
        // stb_truetype's checks (the editor's own font loader); CFF2 variable fonts are read by HarfBuzz alone
        if (kind != OutlineKind::Cff2 && !SystemFonts::isLoadable(bytes.data(), bytes.size(), request.faceIndex)) {
            return failed("'" + fileName + "' is damaged or uses tables the editor cannot read");
        }

        FontImportPlan plan;
        plan.faceIndex = request.faceIndex;
        plan.licence = readFontLicence(bytesOf(bytes), bytes.size(), request.faceIndex);
        nameFace(plan, bytes, request.faceIndex, request.source);
        if (request.from == FontImportSource::System) {
            plan.licenceNote = systemFontLicenceNote();
            if (plan.licence.asksBeforeCopy() && !request.licenceConfirmed) {
                plan.needsLicenceConfirmation = true;
                plan.error = restrictedLicenceQuestion();
                return plan;
            }
        }

        const fs::path directory = request.assetsDir / FileUtils::pathFromUtf8(request.folder);
        bool identical = false;
        const std::string name = freeFontFileName(directory, fileName, bytes, identical);
        plan.assetPath = joinAssetPath(request.folder, name);
        if (!identical) plan.items.push_back({directory / FileUtils::pathFromUtf8(name), std::move(bytes)});
        for (const fs::path& companion : request.companions) {
            std::string text;
            if (!readWhole(companion, text, error)) continue;
            bool same = false;
            const std::string companionName = freeFontFileName(directory, FileUtils::pathToUtf8(companion.filename()), text, same);
            if (!same) plan.items.push_back({directory / FileUtils::pathFromUtf8(companionName), std::move(text)});
        }
        plan.alreadyPresent = plan.items.empty();
        plan.ok = true;
        return plan;
    }

    FontImportPlan planWebFontConversion(const WebFontConversionRequest& request) {
        std::string bytes, error;
        if (!readWhole(request.source, bytes, error)) return failed(error);
        const std::string fileName = FileUtils::pathToUtf8(request.source.filename());
        DecodedFont decoded = decodeWebFont(bytesOf(bytes), bytes.size());
        if (!decoded.ok) return failed("cannot convert '" + fileName + "': " + decoded.error);

        FontImportPlan plan;
        std::string sfnt(decoded.bytes.begin(), decoded.bytes.end());
        plan.licence = readFontLicence(decoded.bytes.data(), decoded.bytes.size(), 0);
        nameFace(plan, sfnt, 0, request.source);
        const fs::path directory = request.assetsDir / FileUtils::pathFromUtf8(request.folder);
        bool identical = false;
        const std::string name = freeFontFileName(directory, FileUtils::pathToUtf8(request.source.stem()) + decoded.extension, sfnt, identical);
        plan.assetPath = joinAssetPath(request.folder, name);
        if (!identical) plan.items.push_back({directory / FileUtils::pathFromUtf8(name), std::move(sfnt)});
        plan.alreadyPresent = identical;
        plan.ok = true;
        return plan;
    }

    FontImportPlan planGoogleFontImport(const GoogleFontImportRequest& request) {
        using htmlui::GoogleFontsImporter;
        GoogleFontsImporter::FamilyRequest family;
        family.family = request.family;
        for (int weight : request.weights) family.variants.push_back({weight, false});
        for (int weight : request.italicWeights) family.variants.push_back({weight, true});
        family.rangeMin = request.rangeMin;
        family.rangeMax = request.rangeMax;
        const std::string url = GoogleFontsImporter::css2Url({family});

        htmlui::ImportTarget target;
        target.projectRoot = request.projectRoot;
        target.assetsDir = request.assetsDir;
        htmlui::RemoteImporter importer(target);
        htmlui::ImportStaging staging;
        GoogleFontsImporter::Options options;
        options.writeCss = false;
        options.writeManifest = false;
        options.dir = request.folder;
        options.staging = &staging;
        const htmlui::ImportedResource result = GoogleFontsImporter(importer, options).run(url, htmlui::ImportLocation());
        if (!result.ok) {
            std::string why = result.error;
            for (const auto& diagnostic : importer.diagnostics()) {
                if (diagnostic.severity == jasperengine::htmlui::compat::Severity::Error && !diagnostic.message.empty()) {
                    why = diagnostic.message;
                    break;
                }
            }
            return failed("Google Fonts '" + request.family + "' could not be imported: " + (why.empty() ? std::string("no font came back") : why));
        }

        FontImportPlan plan;
        std::vector<WriteFilesCmd::Item> fonts, others;
        std::string regular;
        for (const auto& file : staging.files()) {
            const fs::path absolute = request.assetsDir / FileUtils::pathFromUtf8(file.first);
            std::string existing, error;
            if (readWhole(absolute, existing, error) && existing == file.second) continue;
            const bool font = !sfntFaceOffsets(bytesOf(file.second), file.second.size()).empty();
            if (font && (regular.empty() || file.first.find("-400.") != std::string::npos)) regular = file.first;
            (font ? fonts : others).push_back({absolute, file.second});
        }
        if (regular.empty()) {
            for (const auto& file : staging.files()) {
                if (!sfntFaceOffsets(bytesOf(file.second), file.second.size()).empty() && (regular.empty() || file.first.find("-400.") != std::string::npos))
                    regular = file.first;
            }
        }
        if (regular.empty()) return failed("Google Fonts '" + request.family + "' sent no TrueType file");
        plan.assetPath = regular;
        const auto regularFile = staging.files().find(regular);
        nameFace(plan, regularFile->second, 0, FileUtils::pathFromUtf8(regular));
        plan.licence = readFontLicence(bytesOf(regularFile->second), regularFile->second.size(), 0);
        plan.items = std::move(fonts);
        plan.items.insert(plan.items.end(), others.begin(), others.end());
        plan.alreadyPresent = plan.items.empty();
        plan.ok = true;
        return plan;
    }

    bool commitFontImport(Project* project, const FontImportPlan& plan, std::string& error) {
        if (!plan.ok) {
            error = plan.error.empty() ? std::string("the font import was not planned") : plan.error;
            return false;
        }
        if (plan.items.empty()) return true;
        if (!project) {
            error = "No project is open to import the font into.";
            return false;
        }
        auto* command = new WriteFilesCmd(plan.items, [project](const std::vector<fs::path>& changed) {
            ProjectFileWrites::filesWritten(project, changed);
        });
        command->setName(plan.undoName.empty() ? importFontUndoName(plan.family) : plan.undoName);
        command->setNoMerge();
        command->writeBeneath(project->getAssetsPath());
        auto failure = std::make_shared<std::string>();
        command->reportFailureTo(failure);
        // addCommand runs execute() and deletes the command when it fails (nothing written then)
        if (!project->getProjectCommandHistory()->addCommand(command)) {
            error = "Importing the font failed" + (failure->empty() ? std::string(".") : ": " + *failure);
            return false;
        }
        return true;
    }

}
