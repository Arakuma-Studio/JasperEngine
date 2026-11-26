// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "StringFiles.h"

#include "util/TitleArtLanguage.h"
#include "tinyxml2.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    constexpr size_t MAX_STRINGS_BYTES = 64u << 20;

    std::string stringFilesLower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    std::string trim(const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace((unsigned char)s[a])) a++;
        while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
        return s.substr(a, b - a);
    }

    // the documents' translation languages, then `asked` restricts them
    std::vector<std::string> targetsOf(const std::vector<StringsExportDoc>& docs, const std::vector<std::string>& asked) {
        std::vector<std::string> out;
        for (const StringsExportDoc& d : docs) {
            for (const std::string& code : d.languages) {
                if (code.empty() || stringFilesLower(code) == stringFilesLower(d.sourceLanguage)) continue;
                if (!asked.empty() && std::none_of(asked.begin(), asked.end(), [&](const std::string& a) { return stringFilesLower(a) == stringFilesLower(code); })) continue;
                if (std::none_of(out.begin(), out.end(), [&](const std::string& o) { return stringFilesLower(o) == stringFilesLower(code); })) out.push_back(code);
            }
        }
        return out;
    }

    bool hasLanguage(const StringsExportDoc& doc, const std::string& code) {
        return std::any_of(doc.languages.begin(), doc.languages.end(), [&](const std::string& l) { return stringFilesLower(l) == stringFilesLower(code); });
    }

    const StringsExportUnit::Target* targetOf(const StringsExportUnit& unit, const std::string& code) {
        for (const auto& [language, target] : unit.targets) {
            if (stringFilesLower(language) == stringFilesLower(code)) return &target;
        }
        return nullptr;
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

std::string editor::exportStringsCsv(const std::vector<StringsExportDoc>& docs, const std::vector<std::string>& languages) {
    const bool multi = docs.size() > 1;
    std::set<std::string> sources;
    for (const StringsExportDoc& d : docs) sources.insert(d.sourceLanguage);
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
    for (const StringsExportDoc& d : docs) {
        for (const StringsExportUnit& unit : d.units) {
            std::vector<std::string> row;
            if (multi) row.push_back(d.file);
            row.push_back(unit.key);
            row.push_back(unit.context);
            row.push_back(unit.maxChars > 0 ? std::to_string(unit.maxChars) : std::string());
            row.push_back(unit.source);
            if (mixed) row.push_back(d.sourceLanguage);
            for (const std::string& code : targets) {
                const StringsExportUnit::Target* t = targetOf(unit, code);
                row.push_back(t ? t->text : std::string());
            }
            for (size_t i = 0; i < row.size(); i++) out += (i ? "," : "") + csvField(row[i]);
            out += "\r\n";
        }
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> editor::exportStringsXliff(const std::vector<StringsExportDoc>& docs,
                                                                           const std::vector<std::string>& languages) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const std::string& code : targetsOf(docs, languages)) {
        std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<xliff version=\"1.2\" xmlns=\"urn:oasis:names:tc:xliff:document:1.2\">\n";
        for (const StringsExportDoc& d : docs) {
            if (!hasLanguage(d, code)) continue;
            x += "  <file original=\"" + xmlEscape(d.file, true) + "\" source-language=\"" + xmlEscape(d.sourceLanguage, true) +
                 "\" target-language=\"" + xmlEscape(code, true) + "\" datatype=\"plaintext\">\n    <body>\n";
            for (const StringsExportUnit& unit : d.units) {
                x += "      <trans-unit id=\"" + xmlEscape(unit.key, true) + "\" resname=\"" + xmlEscape(unit.key, true) + "\"";
                if (unit.maxChars > 0) x += " maxwidth=\"" + std::to_string(unit.maxChars) + "\" size-unit=\"char\"";
                x += " xml:space=\"preserve\">\n        <source>" + xmlEscape(unit.source, false) + "</source>";
                if (const StringsExportUnit::Target* t = targetOf(unit, code)) {
                    x += "<target state=\"" + std::string(t->needsReview ? "needs-review-translation" : "translated") + "\">" + xmlEscape(t->text, false) + "</target>";
                }
                x += "\n";
                if (!unit.context.empty()) x += "        <note>" + xmlEscape(unit.context, false) + "</note>\n";
                x += "      </trans-unit>\n";
            }
            x += "    </body>\n  </file>\n";
        }
        x += "</xliff>\n";
        out.emplace_back(code, std::move(x));
    }
    return out;
}

// ==================================================================== parse

bool editor::parseStringsCsv(const std::string& source, StringsFile& out, std::string& error) {
    out = StringsFile();
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
        const std::string h = stringFilesLower(trim(header[c]));
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
        StringRow row;
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

bool editor::parseStringsXliff(const std::string& source, StringsFile& out, std::string& error) {
    out = StringsFile();
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
                StringRow row;
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
                    out.notes.push_back({"inline_markup", row.key, target, "the target has inline markup; strings are plain text (refused)", row.line});
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

bool editor::parseStrings(const std::string& text, const std::string& format, StringsFile& out, std::string& error) {
    std::string kind = format;
    if (kind.empty() || kind == "auto") {
        size_t i = text.rfind("\xEF\xBB\xBF", 0) == 0 ? 3 : 0;
        while (i < text.size() && std::isspace((unsigned char)text[i])) i++;
        kind = i < text.size() && text[i] == '<' ? "xliff" : "csv";
    }
    if (kind == "xliff") return parseStringsXliff(text, out, error);
    if (kind == "csv") return parseStringsCsv(text, out, error);
    error = "format is auto, csv or xliff";
    return false;
}

