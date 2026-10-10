// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_CASE_RUNNER_H
#define COMPAT_CSS_CASE_RUNNER_H

// The CSS rewrite cases of p2-css (fixtures/compat/css/<case>.css -> <case>.rcss,
// <case>.diag.txt, <case>.fork.rcss), shared by compat_css_golden_test.cpp and
// compat_css_rmlui_accepts_test.cpp. A case may start with a directive comment:
//   /* htmlui-test: kind=style-attribute root-id=app root-class=dark,theme root-attr=data-theme dir=rtl viewport=800x1200 touch */
// kind=style-attribute: every later non-empty line is one style="" attribute (baseOffset = the
// line); kind=inline: the sheet is an inline <style> of "ui/page.html". Files are read from
// the fixture root (asset-relative "compat/css/..."); a test import manifest maps the
// remote URLs the cases use to fixture files.

#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/ImportManifest.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/css/CssRewriter.h"

#include <algorithm>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace jasperengine::test::compat {

    using namespace jasperengine::htmlui::compat;

    struct CssCaseResult {
        std::string rcss;                         // the .rcss golden text
        std::string diagnostics;                  // the .diag.txt golden text
        std::vector<Diagnostic> list;
        RewriteResult result;
        std::shared_ptr<SourceFiles> files;
        // the sheets RmlUi must accept: (rcss, line map); style attributes are wrapped in a rule
        std::vector<std::pair<std::string, RcssLineMap>> sheets;
    };

    inline std::string cssCaseRoot(){
        return std::string(JASPER_HTMLUI_TEST_SOURCE_DIR) + "/fixtures";
    }

    inline bool readCaseFile(const std::string& root, const std::string& path, std::string& content){
        std::ifstream file(root + "/" + path, std::ios::binary);
        if (!file)
            return false;
        std::ostringstream stream;
        stream << file.rdbuf();
        content = stream.str();
        return true;
    }

    // the case names ("colors" for fixtures/compat/css/colors.css), sorted
    inline std::vector<std::string> listCssCases(const std::string& root){
        std::vector<std::string> names;
        if (DIR* dir = opendir((root + "/compat/css").c_str())){
            while (dirent* entry = readdir(dir)){
                const std::string name = entry->d_name;
                if (name.size() > 4 && name.compare(name.size() - 4, 4, ".css") == 0)
                    names.push_back(name.substr(0, name.size() - 4));
            }
            closedir(dir);
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    inline ImportManifest cssCaseManifest(){
        ImportManifest manifest;
        auto entry = [&](const char* url, const char* kind, const char* path){
            ImportEntry e;
            e.url = ImportManifest::normalizeUrl(url);
            e.kind = kind;
            e.path = path;
            e.files.push_back(path);
            e.licenseName = "unknown";
            e.source = "test";
            manifest.add(e);
        };
        entry("https://cdn.example.com/theme.css", "stylesheet", "compat/css/imports/remote.css");
        entry("https://fonts.googleapis.com/css2?family=Inter:wght@400;700", "google-fonts-css", "compat/css/imports/google-inter.css");
        entry("https://images.example.com/hero.png", "image", "compat/css/img/a.png");
        entry("https://fonts.gstatic.com/s/roboto/bold.ttf", "font", "fonts/Roboto-Bold.ttf");
        entry("https://images.example.com/gone.png", "image", "compat/css/img/missing.png");
        return manifest;
    }

    inline CssCaseResult runCssCase(const std::string& root, const std::string& name, const ForkFeatures& fork){
        CssCaseResult out;
        const std::string path = "compat/css/" + name + ".css";
        std::string text;
        readCaseFile(root, path, text);

        // directives
        std::string kind = "file";
        std::vector<std::pair<std::string, std::string>> rootAttributes;
        CompatOptions options;
        options.fork = fork;
        size_t bodyStart = 0;
        const std::string marker = "/* htmlui-test:";
        if (text.compare(0, marker.size(), marker) == 0){
            const size_t end = text.find("*/");
            std::istringstream directives(text.substr(marker.size(), end - marker.size()));
            std::string word;
            std::vector<std::string> attributeNames;
            while (directives >> word){
                const size_t eq = word.find('=');
                const std::string key = word.substr(0, eq);
                const std::string value = eq == std::string::npos ? std::string() : word.substr(eq + 1);
                if (key == "kind")
                    kind = value;
                else if (key == "root-id")
                    rootAttributes.emplace_back("id", value);
                else if (key == "root-class"){
                    std::string classes = value;
                    std::replace(classes.begin(), classes.end(), ',', ' ');
                    rootAttributes.emplace_back("class", classes);
                }else if (key == "root-attr"){
                    std::string names = value;
                    std::replace(names.begin(), names.end(), ',', ' ');
                    std::istringstream list(names);
                    std::string attribute;
                    while (list >> attribute)
                        rootAttributes.emplace_back(attribute, "");
                }else if (key == "dir")
                    rootAttributes.emplace_back("dir", value);
                else if (key == "viewport"){
                    const size_t x = value.find('x');
                    options.viewport.x = std::atoi(value.substr(0, x).c_str());
                    options.viewport.y = std::atoi(value.substr(x + 1).c_str());
                }else if (key == "touch")
                    options.touchDevice = true;
                else if (key == "dp")
                    options.dpRatio = (float)std::atof(value.c_str());
            }
            bodyStart = end + 2;
        }

        out.files = std::make_shared<SourceFiles>();
        SourceFiles& files = *out.files;
        const std::string registered = kind == "file" ? path : "ui/page.html";
        const uint32_t file = files.add(registered, text);
        DiagnosticSink sink(files);

        const ImportManifest manifest = cssCaseManifest();
        RewriteContext context;
        context.files.read = [root](const std::string& p, std::string& content){ return readCaseFile(root, p, content); };
        context.files.exists = [root](const std::string& p){
            std::ifstream f(root + "/" + p, std::ios::binary);
            return f.good();
        };
        context.manifest = &manifest;
        context.virtualDirectory = "@htmlui/doc/0000000000000000/";
        context.documentPath = kind == "file" ? std::string() : "ui/page.html";
        context.rootAttributes = rootAttributes;

        std::vector<StylesheetInput> inputs;
        if (kind == "style-attribute"){
            size_t lineStart = bodyStart;
            uint32_t loc = 1;
            while (lineStart < text.size()){
                size_t lineEnd = text.find('\n', lineStart);
                if (lineEnd == std::string::npos)
                    lineEnd = text.size();
                std::string_view line = std::string_view(text).substr(lineStart, lineEnd - lineStart);
                if (!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);
                if (!line.empty() && line.find_first_not_of(" \t") != std::string_view::npos){
                    StylesheetInput input;
                    input.kind = StylesheetInput::Kind::StyleAttribute;
                    input.file = file;
                    input.text = files.text(file).substr(lineStart, line.size());
                    input.baseOffset = (uint32_t)lineStart;
                    input.elementLoc = loc++;
                    inputs.push_back(input);
                }
                lineStart = lineEnd + 1;
            }
        }else{
            StylesheetInput input;
            input.kind = kind == "inline" ? StylesheetInput::Kind::Inline : StylesheetInput::Kind::File;
            input.file = file;
            input.text = files.text(file).substr(bodyStart);
            input.baseOffset = (uint32_t)bodyStart;
            inputs.push_back(input);
        }

        {
            CssRewriter rewriter(options, files, sink, context);
            out.result = rewriter.rewrite(Span<const StylesheetInput>(inputs));
        }
        std::ostringstream rcss;
        if (kind == "style-attribute"){
            std::ostringstream wrapped;
            for (const StylesheetOutput& sheet : out.result.sheets){
                rcss << sheet.rcss << "\n";
                wrapped << "div {\n" << sheet.rcss << "\n}\n";
            }
            RcssLineMap none;
            out.sheets.emplace_back(wrapped.str(), none);
        }else{
            for (const StylesheetOutput& sheet : out.result.sheets){
                rcss << sheet.rcss;
                out.sheets.emplace_back(sheet.rcss, sheet.lines);
            }
        }
        if (!out.result.importantSheet.rcss.empty()){
            rcss << "/* important */\n" << out.result.importantSheet.rcss;
            out.sheets.emplace_back(out.result.importantSheet.rcss, out.result.importantSheet.lines);
        }
        out.rcss = rcss.str();
        out.list = sink.take();
        std::ostringstream diagnostics;
        for (const Diagnostic& d : out.list)
            diagnostics << toText(d) << "\n";
        out.diagnostics = diagnostics.str();
        return out;
    }

}

#endif //COMPAT_CSS_CASE_RUNNER_H
