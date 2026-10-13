// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// checkStylesheet, cssPropertyName and the RmlUi verification (p2-core). checkDocument,
// locateElement and collectRemoteReferences are in CompatApiDocument.cpp (p2-html).

#include "CompatApi.h"

#include "ImportManifest.h"
#include "RmlLogTranslator.h"
#include "SourceFiles.h"
#include "SupportTables.h"
#include "VirtualFiles.h"
#include "css/CssRewriter.h"

#include "HtmlUI.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/StreamMemory.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <string_view>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;

namespace {

    bool isNameByte(unsigned char c){
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c >= 0x80 || c == '\\';
    }

    // The custom property names of CSS text, comments and strings skipped: `--name:` declares
    // a name, every other `--name` reads one (var(--name), fallbacks included).
    void scanCustomProperties(std::string_view css, std::set<std::string>* declared, std::set<std::string>* used){
        size_t i = 0;
        while (i < css.size()){
            const char c = css[i];
            if (c == '/' && i + 1 < css.size() && css[i + 1] == '*'){
                const size_t end = css.find("*/", i + 2);
                i = end == std::string_view::npos ? css.size() : end + 2;
                continue;
            }
            if (c == '"' || c == '\''){
                size_t j = i + 1;
                while (j < css.size() && css[j] != c && css[j] != '\n')
                    j += css[j] == '\\' ? 2 : 1;
                i = j + 1;
                continue;
            }
            const bool start = c == '-' && i + 1 < css.size() && css[i + 1] == '-' && (i == 0 || !isNameByte((unsigned char)css[i - 1]));
            if (!start){
                i++;
                continue;
            }
            size_t j = i + 2;
            while (j < css.size() && isNameByte((unsigned char)css[j]))
                j++;
            const std::string name(css.substr(i, j - i));
            size_t k = j;
            while (k < css.size() && (css[k] == ' ' || css[k] == '\t' || css[k] == '\n' || css[k] == '\r' || css[k] == '\f'))
                k++;
            if (name.size() > 2){
                if (k < css.size() && css[k] == ':'){
                    if (declared)
                        declared->insert(name);
                }else if (used){
                    used->insert(name);
                }
            }
            i = j;
        }
    }

    // A diagnostic's text: `length` code points from a 1-based line and column (the rest of the
    // line when the length is unknown).
    std::string_view diagnosticText(std::string_view text, uint32_t line, uint32_t column, uint32_t length){
        size_t offset = 0;
        for (uint32_t l = 1; l < line && offset < text.size(); l++){
            const size_t end = text.find('\n', offset);
            if (end == std::string_view::npos)
                return std::string_view();
            offset = end + 1;
        }
        // columns and lengths count code points (UTF-8 lead bytes)
        auto advance = [&text](size_t at, uint32_t codePoints){
            while (at < text.size() && codePoints > 0){
                at++;
                while (at < text.size() && ((unsigned char)text[at] & 0xC0) == 0x80)
                    at++;
                codePoints--;
            }
            return at;
        };
        const size_t begin = advance(offset, column > 0 ? column - 1 : 0);
        size_t end = length > 0 ? advance(begin, length) : text.find('\n', begin);
        if (end == std::string_view::npos)
            end = text.size();
        return text.substr(begin, end - begin);
    }

    bool endsWithNoCase(const std::string& text, const char* suffix){
        const size_t length = std::strlen(suffix);
        if (text.size() < length)
            return false;
        for (size_t i = 0; i < length; i++){
            char c = text[text.size() - length + i];
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
            if (c != suffix[i])
                return false;
        }
        return true;
    }

    std::string normalizeAssetPath(std::string path){
        for (char& c : path){
            if (c == '\\')
                c = '/';
        }
        if (path.compare(0, 8, "asset://") == 0)
            path.erase(0, 8);
        while (!path.empty() && path[0] == '/')
            path.erase(0, 1);
        return path;
    }

    // RmlUi stays initialised for the duration of a verification
    class RmlScope {
    public:
        RmlScope(): acquired(!HtmlUI::isInitialized() && HtmlUI::acquire()) {}
        ~RmlScope(){
            if (acquired)
                HtmlUI::release();
        }
        bool ready() const { return HtmlUI::isInitialized(); }

    private:
        bool acquired;
    };

}

std::vector<Diagnostic> jasperengine::htmlui::compat::verifyRcssWithRmlUi(const std::string& rcss, const std::string& virtualPath,
                                                                          const RcssLineMap& lines, const SourceFiles& files){
    std::vector<Diagnostic> diagnostics;
    if (rcss.empty() || Rml::GetSystemInterface() == nullptr)
        return diagnostics;

    RmlLogTranslator::Capture capture;
    {
        Rml::StreamMemory stream(reinterpret_cast<const Rml::byte*>(rcss.data()), rcss.size());
        stream.SetSourceURL(virtualPath);
        Rml::Factory::InstanceStyleSheetStream(&stream);
    }

    const RmlLogTranslator::LineMapping mapping{virtualPath, 1, &lines.lines};
    for (const RmlLogTranslator::Capture::Message& message : capture.messages()){
        if (message.severity == Severity::Info)
            continue;
        std::optional<Diagnostic> mapped = RmlLogTranslator::translate(message.text, files, Span<const RmlLogTranslator::LineMapping>(&mapping, 1));
        if (!mapped){
            Diagnostic unmapped;
            unmapped.code = codes::RmluiMessage;
            unmapped.severity = Severity::Warning;
            unmapped.message = "RmlUi: " + message.text;
            unmapped.origin = "RmlUi";
            mapped = unmapped;
        }
        diagnostics.push_back(std::move(*mapped));
    }
    return diagnostics;
}

CheckReport jasperengine::htmlui::compat::checkStylesheet(const std::string& cssPath, const CheckOptions& options){
    CheckReport report;
    const std::string path = normalizeAssetPath(cssPath);

    SourceFiles files;
    DiagnosticSink sink(files, options.compat.diagnosticLimit);

    std::string text;
    if (!readAssetFile(options.files, path, text)){
        Diagnostic missing;
        missing.code = codes::FileNotFound;
        missing.severity = Severity::Error;
        missing.message = "stylesheet '" + path + "' not found";
        missing.file = path;
        missing.subject = path;
        sink.report(missing);
        report.diagnostics = sink.take();
        return report;
    }
    report.files.push_back(path);

    stripUtf8Bom(text);
    const std::vector<uint32_t> invalid = sanitizeUtf8(text);
    const uint32_t file = files.add(path, std::move(text));
    if (!invalid.empty()){
        sink.report(codes::InvalidUtf8, SourceRef{file, invalid[0], 3},
                    std::to_string(invalid.size()) + " invalid UTF-8 sequence(s) replaced by U+FFFD", "save the file as UTF-8");
    }

    // checks outside a loaded project read the manifest through their file access
    ImportManifest manifest;
    RewriteContext context;
    context.files = options.files;
    if (options.files.read){
        manifest = ImportManifest::load(options.files);
        context.manifest = &manifest;
    }
    context.virtualDirectory = std::string(VirtualFiles::kPrefix) + "check/";
    context.documentPath = path;

    RewriteResult result;
    {
        CssRewriter rewriter(options.compat, files, sink, context);
        StylesheetInput input;
        input.kind = StylesheetInput::Kind::File;
        input.file = file;
        input.text = files.text(file);
        result = rewriter.rewrite(Span<const StylesheetInput>(&input, 1));
    }

    std::vector<Diagnostic> verified;
    if (options.verifyWithRmlUi && !result.sheets.empty()){
        RmlScope rml;
        if (rml.ready()){
            const StylesheetOutput& sheet = result.sheets[0];
            const std::string virtualPath = sheet.virtualPath.empty() ? context.virtualDirectory + path + ".rcss" : sheet.virtualPath;
            verified = verifyRcssWithRmlUi(sheet.rcss, virtualPath, sheet.lines, files);
        }
    }

    report.diagnostics = sink.take();
    report.diagnostics.insert(report.diagnostics.end(), verified.begin(), verified.end());

    // A stylesheet on its own does not know the custom properties its documents define (a
    // style="--accent: ..." attribute, another sheet): a declaration that needs the value of a
    // variable the sheet does not declare is not judged here (p4-gaps). With its documents
    // (checkDocument, check --ui on the sheet with the documents that link it) it is.
    std::set<std::string> declared;
    for (uint32_t id = 1; id <= (uint32_t)files.count(); id++)
        scanCustomProperties(files.text(id), &declared, nullptr);
    report.diagnostics.erase(std::remove_if(report.diagnostics.begin(), report.diagnostics.end(), [&](const Diagnostic& diagnostic){
        if (diagnostic.code != codes::VarUntyped)
            return false;
        const uint32_t id = files.find(diagnostic.file);
        if (id == 0)
            return false;
        std::set<std::string> used;
        scanCustomProperties(diagnosticText(files.text(id), diagnostic.line, diagnostic.column, diagnostic.length), nullptr, &used);
        return std::any_of(used.begin(), used.end(), [&declared](const std::string& name){ return !declared.count(name); });
    }), report.diagnostics.end());
    sortDiagnostics(report.diagnostics);

    for (const std::string& dependency : result.dependencies){
        if (std::find(report.files.begin(), report.files.end(), dependency) == report.files.end())
            report.files.push_back(dependency);
    }
    report.remoteRefs = std::move(result.remoteRefs);
    return report;
}

std::vector<RemoteReference> jasperengine::htmlui::compat::collectRemoteReferences(const std::string& htmlPath){
    return collectRemoteReferences(htmlPath, FileAccess());
}

std::string jasperengine::htmlui::compat::cssPropertyName(std::string_view rcssProperty){
    // a CSS property the engine writes under its own name
    if (const PropertySupport* own = findProperty(rcssProperty)){
        if (std::strcmp(own->spec, "rmlui") != 0)
            return std::string(rcssProperty);
    }

    const PropertySupport* exact = nullptr;
    const PropertySupport* prefix = nullptr;
    for (const PropertySupport& row : propertyTable()){
        const std::string_view output(row.output);
        if (output.empty())
            continue;
        if (output == rcssProperty){
            if (!exact || std::strlen(row.name) < std::strlen(exact->name))
                exact = &row;
        }else if (output.size() > rcssProperty.size() && output.substr(0, rcssProperty.size()) == rcssProperty &&
                  (output[rcssProperty.size()] == ' ' || output[rcssProperty.size()] == ';' || output[rcssProperty.size()] == ',' ||
                   output[rcssProperty.size()] == ':')){
            if (!prefix || std::strlen(row.name) < std::strlen(prefix->name))
                prefix = &row;
        }
    }
    if (exact)
        return exact->name;
    if (prefix)
        return prefix->name;
    return std::string(rcssProperty);
}
