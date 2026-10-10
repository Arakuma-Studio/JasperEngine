// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The lexbor source-offset patch (engine/libs/lexbor/PATCHES.md) against the html5lib
// tree-construction corpus (fixtures/compat/html5lib, MIT): in every document input, every
// element made from a start tag points at `<` + its name, each source start tag gives at most
// one element, implied elements (and adoption-agency clones) have no offset, attributes point at
// their names (phase-2 §3.7.1, §5.2).

#include "CompatTestCommon.h"

#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/html/HtmlParser.h"

#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <set>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::test::compat;

namespace {

    struct Html5libCase {
        std::string file;
        int number = 0;
        std::string data;
        bool fragment = false;
        bool scriptOn = false;
    };

    std::vector<Html5libCase> loadCases(){
        std::vector<Html5libCase> cases;
        std::vector<std::string> names;
        for (const auto& entry : std::filesystem::directory_iterator(fixturePath("compat/html5lib"))){
            if (entry.path().extension() == ".dat")
                names.push_back(entry.path().filename().string());
        }
        std::sort(names.begin(), names.end());
        for (const std::string& name : names){
            std::string text;
            if (!readFixture("compat/html5lib/" + name, text))
                continue;
            // sections: "#data", "#errors", "#document-fragment", "#script-on", "#document"
            std::vector<std::string> lines;
            size_t start = 0;
            for (size_t i = 0; i <= text.size(); i++){
                if (i == text.size() || text[i] == '\n'){
                    lines.push_back(text.substr(start, i - start));
                    start = i + 1;
                }
            }
            int number = 0;
            for (size_t i = 0; i < lines.size(); i++){
                if (lines[i] != "#data")
                    continue;
                Html5libCase c;
                c.file = name;
                c.number = ++number;
                size_t j = i + 1;
                std::string data;
                bool first = true;
                for (; j < lines.size() && lines[j] != "#errors" && lines[j] != "#new-errors"; j++){
                    if (!first)
                        data += '\n';
                    data += lines[j];
                    first = false;
                }
                c.data = data;
                for (; j < lines.size() && lines[j] != "#data"; j++){
                    if (lines[j] == "#document-fragment")
                        c.fragment = true;
                    if (lines[j] == "#script-on")
                        c.scriptOn = true;
                }
                cases.push_back(c);
                i = j - 1;
            }
        }
        return cases;
    }

    bool equalsNoCase(std::string_view a, std::string_view b){
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); i++){
            if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
                return false;
        }
        return true;
    }

    // elements the tree builder makes without a token of their own
    bool mayBeImplied(const HtmlNode& node){
        static const std::set<std::string> implied = {
            "html", "head", "body", "tbody", "tr", "colgroup", "p",
            // reconstructed active formatting elements and adoption-agency clones
            "a", "b", "big", "code", "em", "font", "i", "nobr", "s", "small", "strike", "strong", "tt", "u",
        };
        return node.ns == Namespace::Html && implied.count(node.tag) > 0;
    }

    struct Counts {
        size_t elements = 0;
        size_t located = 0;
        size_t attributes = 0;
    };

    void check(const HtmlNode& node, const SourceFiles& files, const Html5libCase& c, std::set<uint32_t>& seen, Counts& counts){
        for (const std::unique_ptr<HtmlNode>& owned : node.children){
            const HtmlNode& child = *owned;
            if (child.kind != HtmlNode::Kind::Element)
                continue;
            counts.elements++;
            if (child.ref.valid()){
                counts.located++;
                const std::string_view tag = files.slice(child.ref);
                // "<" + the name as written (SVG/MathML names are adjusted in case: compared without case)
                // (two tokens make an element of another name: `</br>` a <br>, `<image>` an <img>)
                const size_t nameStart = (tag.size() > 1 && tag[1] == '/') ? 2 : 1;
                const std::string expected = (child.tag == "img" && equalsNoCase(tag.substr(nameStart, 5), "image")) ? "image" : child.tag;
                const std::string written = (tag.size() > expected.size() + nameStart) ? std::string(tag.substr(nameStart, expected.size())) : std::string();
                EXPECT_TRUE(!tag.empty() && tag[0] == '<' && equalsNoCase(written, expected) && (nameStart == 1 || child.tag == "br"))
                    << c.file << " #" << c.number << ": <" << child.tag << "> points at '" << tag << "' in: " << c.data;
                EXPECT_TRUE(seen.insert(child.ref.offset).second)
                    << c.file << " #" << c.number << ": two elements share the start tag at " << child.ref.offset;
                for (const HtmlAttr& attribute : child.attrs){
                    if (!attribute.nameRef.valid())
                        continue;
                    counts.attributes++;
                    const std::string_view name = files.slice(attribute.nameRef);
                    // xlink:href and the like keep their prefix; the adjusted SVG names differ in case only
                    EXPECT_TRUE(equalsNoCase(name, attribute.name))
                        << c.file << " #" << c.number << ": attribute " << attribute.name << " points at '" << name << "'";
                }
            }else{
                EXPECT_TRUE(mayBeImplied(child)) << c.file << " #" << c.number << ": <" << child.tag << "> has no offset in: " << c.data;
            }
            check(child, files, c, seen, counts);
            if (child.templateContent)
                check(*child.templateContent, files, c, seen, counts);
        }
    }

}

TEST(CompatHtml5lib, StartTagOffsets){
    const std::vector<Html5libCase> cases = loadCases();
    ASSERT_GT(cases.size(), 1500u) << "the html5lib corpus is missing";

    Counts counts;
    size_t documents = 0;
    for (const Html5libCase& c : cases){
        if (c.fragment || c.scriptOn)
            continue;   // fragment contexts and scripting-on parses are not what the engine does
        documents++;
        SourceFiles files;
        const uint32_t file = files.add(c.file, c.data);
        DiagnosticSink sink(files);
        HtmlDocument document = HtmlParser::parseDocument(file, files.text(file), &sink);
        ASSERT_TRUE(document.root) << c.file << " #" << c.number;
        std::set<uint32_t> seen;
        // the root itself
        if (document.root->ref.valid()){
            counts.located++;
            EXPECT_TRUE(equalsNoCase(files.slice(document.root->ref).substr(1, 4), "html")) << c.file << " #" << c.number;
            seen.insert(document.root->ref.offset);
        }
        counts.elements++;
        check(*document.root, files, c, seen, counts);
    }
    EXPECT_GT(documents, 1400u);
    EXPECT_GT(counts.located, 3000u);
    EXPECT_GT(counts.attributes, 400u);
    std::printf("[html5lib] %zu documents, %zu elements, %zu from start tags, %zu attributes located\n",
                documents, counts.elements, counts.located, counts.attributes);
}

TEST(CompatHtml5lib, FragmentsParse){
    // fragment inputs parse in body context without crashing (innerHTML)
    size_t parsed = 0;
    for (const Html5libCase& c : loadCases()){
        if (!c.fragment)
            continue;
        SourceFiles files;
        const uint32_t file = files.add(c.file, c.data);
        HtmlDocument document = HtmlParser::parseFragment(file, files.text(file), 0, nullptr);
        ASSERT_TRUE(document.root);
        parsed++;
    }
    EXPECT_GT(parsed, 100u);
}
