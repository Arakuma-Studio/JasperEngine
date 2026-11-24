// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleStrings.h"

#include "TitleLanguageNames.h"
#include "TitleLanguageOps.h"
#include "titleart/TitleArtOps.h"
#include "titleart/TitleTranslations.h"
#include "util/TitleArtLanguage.h"
#include "tinyxml2.h"

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

    constexpr size_t MAX_STRINGS_BYTES = 64u << 20;

    std::string str(const Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    std::string lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    std::string trim(const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace((unsigned char)s[a])) a++;
        while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
        return s.substr(a, b - a);
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

    // the document's translation languages, then `asked` restricts them
    std::vector<std::string> targetsOf(const std::vector<TitleStringsDocument>& docs, const std::vector<std::string>& asked) {
        std::vector<std::string> out;
        for (const TitleStringsDocument& d : docs) {
            for (const std::string& code : titleart::titleLanguageCodes(d.doc->root)) {
                if (titleart::isSourceLanguage(d.doc->root, code)) continue;
                if (!asked.empty() && std::none_of(asked.begin(), asked.end(), [&](const std::string& a) { return lower(a) == lower(code); })) continue;
                if (std::none_of(out.begin(), out.end(), [&](const std::string& o) { return lower(o) == lower(code); })) out.push_back(code);
            }
        }
        return out;
    }

    // ---- CSV ----

    // RFC 4180 quoting: a comma, a quote, CR/LF, leading or trailing spaces
    std::string csvField(const std::string& value, char delimiter = ',') {
        const bool quote = value.find_first_of(std::string("\"\r\n") + delimiter) != std::string::npos ||
                           (!value.empty() && (value.front() == ' ' || value.back() == ' '));
        if (!quote) return value;
        std::string out = "\"";
        for (char c : value) {
            if (c == '"') out += "\"\"";
            else out += c;
        }
        return out + "\"";
    }

    // the records of a CSV text (quoted newlines kept); each with its first line
    bool csvRecords(const std::string& text, char delimiter, std::vector<std::pair<int, std::vector<std::string>>>& out, std::string& error) {
        size_t i = 0;
        int line = 1;
        while (i < text.size()) {
            std::vector<std::string> fields;
            std::string field;
            const int start = line;
            bool quoted = false, inQuotes = false, endOfRecord = false;
            while (i < text.size() && !endOfRecord) {
                const char c = text[i];
                if (inQuotes) {
                    if (c == '"') {
                        if (i + 1 < text.size() && text[i + 1] == '"') {
                            field += '"';
                            i += 2;
                            continue;
                        }
                        inQuotes = false;
                        i++;
                        continue;
                    }
                    if (c == '\n') line++;
                    field += c;
                    i++;
                    continue;
                }
                if (c == '"' && field.empty() && !quoted) {
                    inQuotes = quoted = true;
                    i++;
                } else if (c == delimiter) {
                    fields.push_back(std::move(field));
                    field.clear();
                    quoted = false;
                    i++;
                } else if (c == '\r' || c == '\n') {
                    if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') i++;
                    i++;
                    line++;
                    endOfRecord = true;
                } else {
                    field += c;
                    i++;
                }
            }
            if (inQuotes) {
                error = "line " + std::to_string(start) + ": a quoted field is not closed";
                return false;
            }
            fields.push_back(std::move(field));
            const bool blank = fields.size() == 1 && fields[0].empty();
            if (!blank) out.emplace_back(start, std::move(fields));
        }
        return true;
    }

    // a header's language code: the code in parentheses ("German (de)"), else the header itself
    std::string headerCode(const std::string& header) {
        const size_t open = header.rfind('('), close = header.rfind(')');
        std::string code = trim(open != std::string::npos && close != std::string::npos && close > open ? header.substr(open + 1, close - open - 1) : header);
        return TitleArtLanguage::isValid(code) ? TitleArtLanguage::canonicalize(code) : std::string();
    }

    // ---- XLIFF ----

    std::string xmlEscape(const std::string& s, bool attribute) {
        std::string out;
        for (char c : s) {
            switch (c) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '"': out += attribute ? "&quot;" : "\""; break;
                default: out += c;
            }
        }
        return out;
    }

    // the text of an element with no child elements; false when it has markup
    bool plainText(const tinyxml2::XMLElement* e, std::string& out) {
        out.clear();
        for (const tinyxml2::XMLNode* n = e->FirstChild(); n; n = n->NextSibling()) {
            if (n->ToElement()) return false;
            if (const tinyxml2::XMLText* t = n->ToText()) out += t->Value() ? t->Value() : "";
        }
        return true;
    }

    std::string attr(const tinyxml2::XMLElement* e, const char* name) {
        const char* v = e->Attribute(name);
        return v ? v : "";
    }

}

// ==================================================================== export

std::string editor::exportTitleStringsCsv(const std::vector<TitleStringsDocument>& docs, const std::vector<std::string>& languages) {
    const bool multi = docs.size() > 1;
    std::set<std::string> sources;
    for (const TitleStringsDocument& d : docs) sources.insert(titleart::titleSourceLanguage(d.doc->root));
    const bool mixed = sources.size() > 1;
    const std::vector<std::string> targets = targetsOf(docs, languages);
    std::string out = "\xEF\xBB\xBF";
    std::vector<std::string> header;
    if (multi) header.push_back("file");
    header.insert(header.end(), {"key", "context", "max_chars"});
    header.push_back(mixed || sources.empty() || sources.begin()->empty() ? std::string("source") : *sources.begin());
    if (mixed) header.push_back("source_language");
    header.insert(header.end(), targets.begin(), targets.end());
    for (size_t i = 0; i < header.size(); i++) out += (i ? "," : "") + csvField(header[i]);
    out += "\r\n";
    for (const TitleStringsDocument& d : docs) {
        eachLayer(d.doc->root["layers"], [&](const Json& layer) {
            if (!exported(layer)) return;
            std::vector<std::string> row;
            if (multi) row.push_back(d.file);
            row.push_back(titleStringKey(d.doc->root, layer));
            row.push_back(str(layer, "translator_note"));
            row.push_back(layer.value("max_chars", 0) > 0 ? std::to_string(layer.value("max_chars", 0)) : std::string());
            row.push_back(str(layer, "text"));
            if (mixed) row.push_back(titleart::titleSourceLanguage(d.doc->root));
            for (const std::string& code : targets) {
                const Json* t = titleart::findTranslation(layer, code);
                row.push_back(titleart::translationHasText(t) ? str(*t, "text") : std::string());
            }
            for (size_t i = 0; i < row.size(); i++) out += (i ? "," : "") + csvField(row[i]);
            out += "\r\n";
        });
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> editor::exportTitleStringsXliff(const std::vector<TitleStringsDocument>& docs,
                                                                                const std::vector<std::string>& languages) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const std::string& code : targetsOf(docs, languages)) {
        std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<xliff version=\"1.2\" xmlns=\"urn:oasis:names:tc:xliff:document:1.2\">\n";
        for (const TitleStringsDocument& d : docs) {
            if (!titleart::findTitleLanguage(d.doc->root, code)) continue;
            x += "  <file original=\"" + xmlEscape(d.file, true) + "\" source-language=\"" + xmlEscape(titleart::titleSourceLanguage(d.doc->root), true) +
                 "\" target-language=\"" + xmlEscape(code, true) + "\" datatype=\"plaintext\">\n    <body>\n";
            eachLayer(d.doc->root["layers"], [&](const Json& layer) {
                if (!exported(layer)) return;
                const std::string key = titleStringKey(d.doc->root, layer);
                x += "      <trans-unit id=\"" + xmlEscape(key, true) + "\" resname=\"" + xmlEscape(key, true) + "\"";
                if (layer.value("max_chars", 0) > 0) x += " maxwidth=\"" + std::to_string(layer.value("max_chars", 0)) + "\" size-unit=\"char\"";
                x += " xml:space=\"preserve\">\n        <source>" + xmlEscape(str(layer, "text"), false) + "</source>";
                const Json* t = titleart::findTranslation(layer, code);
                if (titleart::translationHasText(t)) {
                    const bool review = str(*t, "status") == "needs_review" ||
                                        str(*t, "source_hash") != titleart::translationSourceHash(str(layer, "text"));
                    x += "<target state=\"" + std::string(review ? "needs-review-translation" : "translated") + "\">" + xmlEscape(str(*t, "text"), false) + "</target>";
                }
                x += "\n";
                if (!str(layer, "translator_note").empty()) x += "        <note>" + xmlEscape(str(layer, "translator_note"), false) + "</note>\n";
                x += "      </trans-unit>\n";
            });
            x += "    </body>\n  </file>\n";
        }
        x += "</xliff>\n";
        out.emplace_back(code, std::move(x));
    }
    return out;
}

// ==================================================================== parse

bool editor::parseTitleStringsCsv(const std::string& source, TitleStringsFile& out, std::string& error) {
    out = TitleStringsFile();
    if (source.size() > MAX_STRINGS_BYTES) {
        error = "the strings file is over 64 MiB";
        return false;
    }
    std::string text = source.rfind("\xEF\xBB\xBF", 0) == 0 ? source.substr(3) : source;
    // the delimiter of the header row (outside quotes): comma, semicolon or tab
    const size_t eol = text.find_first_of("\r\n");
    const std::string first = text.substr(0, eol);
    size_t counts[3] = {0, 0, 0};
    const char delimiters[3] = {',', ';', '\t'};
    bool quoted = false;
    for (char c : first) {
        if (c == '"') quoted = !quoted;
        for (int d = 0; d < 3; d++) counts[d] += !quoted && c == delimiters[d] ? 1 : 0;
    }
    char delimiter = ',';
    if (counts[1] > counts[0] && counts[1] >= counts[2]) delimiter = ';';
    else if (counts[2] > counts[0] && counts[2] > counts[1]) delimiter = '\t';
    std::vector<std::pair<int, std::vector<std::string>>> records;
    if (!csvRecords(text, delimiter, records, error)) return false;
    if (records.empty()) {
        error = "the file has no header row";
        return false;
    }
    const std::vector<std::string>& header = records[0].second;
    int fileColumn = -1, keyColumn = -1, contextColumn = -1, maxColumn = -1, sourceColumn = -1, sourceLanguageColumn = -1;
    std::vector<std::pair<int, std::string>> languageColumns;
    for (size_t c = 0; c < header.size(); c++) {
        const std::string h = lower(trim(header[c]));
        if (h == "file") fileColumn = (int)c;
        else if (h == "key" || h == "id") keyColumn = (int)c;
        else if (h == "context" || h == "comment" || h == "note") contextColumn = (int)c;
        else if (h == "max_chars" || h == "max") maxColumn = (int)c;
        else if (h == "source") sourceColumn = (int)c;
        else if (h == "source_language") sourceLanguageColumn = (int)c;
        else {
            const std::string code = headerCode(header[c]);
            if (code.empty()) {
                out.notes.push_back({"unknown_column", "", "", "column '" + header[c] + "' is not a language code (ignored)", records[0].first});
                continue;
            }
            languageColumns.emplace_back((int)c, code);
        }
    }
    if (keyColumn < 0) {
        error = "the header has no `key` column";
        return false;
    }
    // without a `source` column the first language column is the source's (the documents' code)
    std::string sourceCode;
    if (sourceColumn < 0 && !languageColumns.empty()) {
        sourceColumn = languageColumns.front().first;
        sourceCode = languageColumns.front().second;
        languageColumns.erase(languageColumns.begin());
    }
    for (const auto& lc : languageColumns) out.languages.push_back(lc.second);
    for (size_t r = 1; r < records.size(); r++) {
        const std::vector<std::string>& f = records[r].second;
        auto at = [&](int c) { return c >= 0 && (size_t)c < f.size() ? f[(size_t)c] : std::string(); };
        TitleStringRow row;
        row.line = records[r].first;
        row.file = at(fileColumn);
        row.key = trim(at(keyColumn));
        row.context = at(contextColumn);
        row.maxChars = std::atoi(at(maxColumn).c_str());
        row.source = at(sourceColumn);
        row.sourceLanguage = sourceLanguageColumn >= 0 ? TitleArtLanguage::canonicalize(at(sourceLanguageColumn)) : sourceCode;
        if (row.key.empty()) {
            out.notes.push_back({"bad_row", "", "", "a row without a key (ignored)", row.line});
            continue;
        }
        for (const auto& [column, code] : languageColumns) {
            const std::string cell = at(column);
            if (!cell.empty()) row.targets[code] = cell;
        }
        out.rows.push_back(std::move(row));
    }
    return true;
}

bool editor::parseTitleStringsXliff(const std::string& source, TitleStringsFile& out, std::string& error) {
    out = TitleStringsFile();
    if (source.size() > MAX_STRINGS_BYTES) {
        error = "the strings file is over 64 MiB";
        return false;
    }
    tinyxml2::XMLDocument xml;
    if (xml.Parse(source.data(), source.size()) != tinyxml2::XML_SUCCESS) {
        error = std::string("not XML: ") + (xml.ErrorStr() ? xml.ErrorStr() : "parse error");
        return false;
    }
    const tinyxml2::XMLElement* root = xml.RootElement();
    if (!root || std::string(root->Name()) != "xliff") {
        error = "not an XLIFF file (the root is not <xliff>)";
        return false;
    }
    const std::string version = attr(root, "version");
    const bool two = version.rfind("2", 0) == 0;
    if (!two && version.rfind("1.2", 0) != 0) {
        error = "XLIFF " + version + " is not read (1.2 and 2.0 are)";
        return false;
    }
    std::set<std::string> languages;
    const std::string globalTarget = TitleArtLanguage::canonicalize(attr(root, "trgLang"));
    const std::string globalSource = TitleArtLanguage::canonicalize(attr(root, "srcLang"));
    for (const tinyxml2::XMLElement* file = root->FirstChildElement("file"); file; file = file->NextSiblingElement("file")) {
        const std::string original = attr(file, "original");
        const std::string target = two ? globalTarget : TitleArtLanguage::canonicalize(attr(file, "target-language"));
        const std::string sourceLanguage = two ? globalSource : TitleArtLanguage::canonicalize(attr(file, "source-language"));
        if (target.empty()) {
            out.notes.push_back({"bad_row", "", "", "a <file> without a target language (ignored)", file->GetLineNum()});
            continue;
        }
        languages.insert(target);
        // 1.2: <body><trans-unit>; 2.0: <unit><segment> (groups at any depth)
        std::function<void(const tinyxml2::XMLElement*)> visit = [&](const tinyxml2::XMLElement* parent) {
            for (const tinyxml2::XMLElement* e = parent->FirstChildElement(); e; e = e->NextSiblingElement()) {
                const std::string name = e->Name();
                if (name == "body" || name == "group") {
                    visit(e);
                    continue;
                }
                if (name != (two ? "unit" : "trans-unit")) continue;
                TitleStringRow row;
                row.line = e->GetLineNum();
                row.file = original;
                row.key = attr(e, two ? "name" : "resname");
                if (row.key.empty()) row.key = attr(e, "id");
                row.sourceLanguage = sourceLanguage;
                row.maxChars = std::atoi(attr(e, "maxwidth").c_str());
                const tinyxml2::XMLElement* holder = two ? e->FirstChildElement("segment") : e;
                if (!holder) continue;
                const tinyxml2::XMLElement* src = holder->FirstChildElement("source");
                const tinyxml2::XMLElement* tgt = holder->FirstChildElement("target");
                std::string text;
                if (src && plainText(src, text)) row.source = text;
                if (const tinyxml2::XMLElement* note = (two ? e->FirstChildElement("notes") : e); note) {
                    const tinyxml2::XMLElement* n = note->FirstChildElement("note");
                    std::string noteText;
                    if (n && plainText(n, noteText)) row.context = noteText;
                }
                if (!tgt) continue;     // an absent target is skipped
                if (!plainText(tgt, text)) {
                    out.notes.push_back({"inline_markup", row.key, target, "the target has inline markup; titles are plain text (refused)", row.line});
                    continue;
                }
                if (text.empty()) continue;
                const std::string state = two ? attr(holder, "state") : attr(tgt, "state");
                std::string status = "translated";
                if (two) {
                    if (state == "initial") status = "needs_review";
                } else if (state.rfind("needs-", 0) == 0 || state == "new") {
                    status = "needs_review";
                }
                row.targets[target] = text;
                row.states[target] = status;
                out.rows.push_back(std::move(row));
            }
        };
        visit(file);
    }
    out.languages.assign(languages.begin(), languages.end());
    return true;
}

bool editor::parseTitleStrings(const std::string& text, const std::string& format, TitleStringsFile& out, std::string& error) {
    std::string kind = format;
    if (kind.empty() || kind == "auto") {
        size_t i = text.rfind("\xEF\xBB\xBF", 0) == 0 ? 3 : 0;
        while (i < text.size() && std::isspace((unsigned char)text[i])) i++;
        kind = i < text.size() && text[i] == '<' ? "xliff" : "csv";
    }
    if (kind == "xliff") return parseTitleStringsXliff(text, out, error);
    if (kind == "csv") return parseTitleStringsCsv(text, out, error);
    error = "format is auto, csv or xliff";
    return false;
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
