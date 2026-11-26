// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleStrings.h"

#include "TitleLanguageNames.h"
#include "TitleLanguageOps.h"
#include "titleart/TitleArtOps.h"
#include "titleart/TitleTranslations.h"
#include "util/TitleArtLanguage.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;
namespace fs = std::filesystem;

namespace {

    std::string str(const Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    void eachLayer(const Json& list, const std::function<void(const Json&)>& visit) {
        if (!list.is_array()) return;
        for (const Json& layer : list) {
            visit(layer);
            if (auto children = layer.find("children"); children != layer.end()) eachLayer(*children, visit);
        }
    }

    bool exported(const Json& layer) {
        return titleart::layerTypeOf(layer) == titleart::LayerType::Text && layer.value("translate", true) && !str(layer, "text").empty() &&
               !str(layer, "text_key").empty();
    }

    // a document as StringFiles exports it: its text layers with a key, their translations
    StringsExportDoc exportDocOf(const TitleStringsDocument& d) {
        StringsExportDoc out;
        out.file = d.file;
        out.sourceLanguage = titleart::titleSourceLanguage(d.doc->root);
        for (const std::string& code : titleart::titleLanguageCodes(d.doc->root)) {
            if (!titleart::isSourceLanguage(d.doc->root, code)) out.languages.push_back(code);
        }
        eachLayer(d.doc->root["layers"], [&](const Json& layer) {
            if (!exported(layer)) return;
            StringsExportUnit unit;
            unit.key = titleStringKey(d.doc->root, layer);
            unit.context = str(layer, "translator_note");
            unit.maxChars = layer.value("max_chars", 0);
            unit.source = str(layer, "text");
            for (const std::string& code : out.languages) {
                const Json* t = titleart::findTranslation(layer, code);
                if (!titleart::translationHasText(t)) continue;
                const bool review = str(*t, "status") == "needs_review" ||
                                    str(*t, "source_hash") != titleart::translationSourceHash(unit.source);
                unit.targets[code] = {str(*t, "text"), review};
            }
            out.units.push_back(std::move(unit));
        });
        return out;
    }

    std::vector<StringsExportDoc> exportDocsOf(const std::vector<TitleStringsDocument>& docs) {
        std::vector<StringsExportDoc> out;
        for (const TitleStringsDocument& d : docs) out.push_back(exportDocOf(d));
        return out;
    }

}

// ==================================================================== export

std::string editor::exportTitleStringsCsv(const std::vector<TitleStringsDocument>& docs, const std::vector<std::string>& languages) {
    return exportStringsCsv(exportDocsOf(docs), languages);
}

std::vector<std::pair<std::string, std::string>> editor::exportTitleStringsXliff(const std::vector<TitleStringsDocument>& docs,
                                                                                const std::vector<std::string>& languages) {
    return exportStringsXliff(exportDocsOf(docs), languages);
}

// ==================================================================== parse

bool editor::parseTitleStringsCsv(const std::string& source, TitleStringsFile& out, std::string& error) {
    return parseStringsCsv(source, out, error);
}

bool editor::parseTitleStringsXliff(const std::string& source, TitleStringsFile& out, std::string& error) {
    return parseStringsXliff(source, out, error);
}

bool editor::parseTitleStrings(const std::string& text, const std::string& format, TitleStringsFile& out, std::string& error) {
    return parseStrings(text, format, out, error);
}

bool editor::titleStringRowIsFor(const TitleStringRow& row, const std::string& documentFile, const titleart::TitleArtDoc& doc) {
    if (!row.file.empty()) return fs::path(row.file).lexically_normal() == fs::path(documentFile).lexically_normal();
    const std::string ns = str(doc.root, "string_namespace");
    const size_t dot = row.key.find('.');
    return ns.empty() ? dot == std::string::npos : (dot != std::string::npos && row.key.substr(0, dot) == ns);
}

// ==================================================================== the op

namespace {

    bool importStrings(TitleOpContext& ctx, const Json& op) {
        std::string file;
        if (!ctx.requireString(op, "file", file)) return false;
        fs::path path = fs::path(file);
        if (!path.is_absolute() && ctx.options && !ctx.options->projectPath.empty()) path = fs::path(ctx.options->projectPath) / path;
        std::string text;
        {
            std::ifstream in(path, std::ios::binary);
            if (!in) return ctx.fail("file_not_found", "cannot read '" + file + "'", "file");
            std::ostringstream buffer;
            buffer << in.rdbuf();
            text = buffer.str();
        }
        TitleStringsFile strings;
        std::string error;
        if (!parseTitleStrings(text, op.value("format", std::string("auto")), strings, error)) return ctx.fail("bad_file", error, "file");
        std::vector<std::string> only;
        if (op.contains("languages")) {
            if (!op["languages"].is_array()) return ctx.fail("bad_op", "`languages` is a list of codes", "languages");
            for (const Json& l : op["languages"]) if (l.is_string()) only.push_back(TitleArtLanguage::canonicalize(l.get<std::string>()));
        }
        const bool addLanguages = op.value("add_languages", false);
        const std::string status = op.value("status", std::string("translated"));
        if (status != "translated" && status != "needs_review") return ctx.fail("bad_value", "status is translated or needs_review", "status");
        std::string documentFile;
        if (ctx.options && !ctx.options->documentPath.empty() && !ctx.options->projectPath.empty()) {
            std::error_code ec;
            documentFile = fs::relative(ctx.options->documentPath, ctx.options->projectPath, ec).generic_string();
        }
        ensureTitleTextKeys(ctx);
        Json& root = ctx.doc.root;
        Json notes = Json::array();
        for (const TitleStringsNote& n : strings.notes) notes.push_back(Json{{"code", n.code}, {"key", n.key}, {"language", n.language}, {"message", n.message}, {"line", n.line}});
        // the language columns: known, added, or reported
        std::set<std::string> usable;
        for (const std::string& code : strings.languages) {
            if (!only.empty() && std::find(only.begin(), only.end(), code) == only.end()) continue;
            if (titleart::isSourceLanguage(root, code)) continue;
            if (titleart::findTitleLanguage(root, code)) {
                usable.insert(code);
            } else if (addLanguages) {
                Json list = root["languages"];
                list.push_back(Json{{"code", code}, {"name", ""}, {"font", nullptr}, {"fallback_fonts", Json::array()}, {"characters", ""}, {"bake", true}});
                if (str(root, "source_language").empty() && !ctx.set(nullptr, "source_language", ctx.options && !ctx.options->defaultLanguage.empty() ? ctx.options->defaultLanguage : "en")) return false;
                if (!ctx.set(nullptr, "languages", list)) return false;
                usable.insert(code);
            } else {
                notes.push_back(Json{{"code", "unknown_language"}, {"language", code},
                                     {"message", "the title has no language '" + code + "' (add_languages: true adds it)"}});
            }
        }
        // the keys of this document
        std::map<std::string, Json*> layers;
        std::function<void(Json&)> walk = [&](Json& list) {
            if (!list.is_array()) return;
            for (Json& layer : list) {
                if (titleart::layerTypeOf(layer) == titleart::LayerType::Text && !str(layer, "text_key").empty()) layers[titleStringKey(root, layer)] = &layer;
                if (auto children = layer.find("children"); children != layer.end()) walk(*children);
            }
        };
        walk(root["layers"]);
        size_t applied = 0, unchanged = 0, rows = 0;
        for (const TitleStringRow& row : strings.rows) {
            if (!titleStringRowIsFor(row, documentFile, ctx.doc)) continue;
            rows++;
            auto it = layers.find(row.key);
            if (it == layers.end()) {
                notes.push_back(Json{{"code", "unknown_key"}, {"key", row.key}, {"line", row.line}, {"message", "no text layer has the key '" + row.key + "'"}});
                continue;
            }
            Json* layer = it->second;
            const std::string current = str(*layer, "text");
            const bool sourceChanged = !row.source.empty() && row.source != current;
            if (sourceChanged) {
                notes.push_back(Json{{"code", "source_changed"}, {"key", row.key}, {"line", row.line},
                                     {"message", "the file's source text differs from the title's: imported as needs_review"}});
            }
            for (const auto& [code, value] : row.targets) {
                if (!usable.count(code)) continue;
                const Json* t = titleart::findTranslation(*layer, code);
                if (titleart::translationHasText(t) && str(*t, "text") == value) {
                    unchanged++;
                    continue;
                }
                const std::string rowStatus = sourceChanged ? "needs_review" : row.states.count(code) ? row.states.at(code) : status;
                const std::string prefix = "translations." + code + ".";
                if (!ctx.set(layer, prefix + "text", value) || !ctx.set(layer, prefix + "status", rowStatus == "needs_review" ? "needs_review" : status) ||
                    !ctx.set(layer, prefix + "source_hash", titleart::translationSourceHash(current))) {
                    return false;
                }
                applied++;
            }
        }
        ctx.result.reports[std::to_string(ctx.opIndex)] = Json{{"rows", rows}, {"applied", applied}, {"unchanged", unchanged}, {"notes", notes}};
        ctx.undoName = "Import Strings (" + fs::path(file).filename().string() + ")";
        return true;
    }

}

void editor::registerTitleStringsOps() {
    TitleArtOps::registerOp("import_strings", importStrings);
}
