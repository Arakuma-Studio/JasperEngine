// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SourceRefList (CompatTypes.h): the compact list the RCSS line maps and the RML source map
// are kept in holds exactly the entries the std::vector<SourceRef> it replaced held. Every
// map of every CSS case, HTML fixture and golden page is read back entry by entry (by index
// and in order), written again from a plain vector and compared.

#include "CssCaseRunner.h"

#include "htmlui/compat/DocumentLoader.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::test::compat;

namespace {

    // the list read in order, checked against the list read by index and written again
    std::vector<SourceRef> roundTrip(const SourceRefList& list, const std::string& what){
        std::vector<SourceRef> plain;
        for (const SourceRef& ref : list)
            plain.push_back(ref);
        EXPECT_EQ(plain.size(), list.size()) << what;
        for (size_t i = 0; i < plain.size() && i < list.size(); i++){
            if (!(list[i] == plain[i])){
                ADD_FAILURE() << what << ": entry " << i << " differs by index";
                break;
            }
        }
        const SourceRefList again(plain.begin(), plain.end());
        EXPECT_TRUE(again == list) << what;
        if (!plain.empty()){
            EXPECT_TRUE(list.back() == plain.back()) << what;
        }
        return plain;
    }

    FileAccess fixtureAccess(){
        FileAccess access;
        access.read = [](const std::string& path, std::string& content){ return readCaseFile(cssCaseRoot(), path, content); };
        access.exists = [](const std::string& path){
            std::error_code error;
            return std::filesystem::is_regular_file(cssCaseRoot() + "/" + path, error);
        };
        return access;
    }

}

TEST(CompatSourceRefList, HoldsWhatAVectorHolds){
    // every shape of entry: the same entry again, another file, offsets forward and back,
    // lengths under and over the short form, the largest values
    std::vector<SourceRef> refs = {SourceRef{}, SourceRef{}, SourceRef{1, 0, 0}, SourceRef{1, 0, 0}, SourceRef{1, 5, 30}, SourceRef{1, 5, 31},
                                   SourceRef{1, 4, 32}, SourceRef{2, 0xFFFFFFFFu, 0xFFFFFFFFu}, SourceRef{0xFFFFFFFFu, 0, 127}, SourceRef{0, 128, 128},
                                   SourceRef{0, 16383, 16384}, SourceRef{0, 16384, 2097151}, SourceRef{3, 2097152, 1}, SourceRef{}};
    uint32_t state = 12345;
    auto next = [&state](){
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    SourceRef last{1, 0, 0};
    for (int i = 0; i < 5000; i++){
        switch (next() % 8){
            case 0: break;                                              // the same again
            case 1: last.file = next() % 5; break;
            case 2: last.offset -= next() % 300; break;                 // back (wraps below zero too)
            case 3: last = SourceRef{next(), next() << 8, next()}; break;
            default: last.offset += next() % 200; last.length = next() % 90; break;
        }
        refs.push_back(last);
    }
    SourceRefList list;
    EXPECT_TRUE(list.empty());
    EXPECT_TRUE(list.begin() == list.end());
    for (size_t i = 0; i < refs.size(); i++){
        list.push_back(refs[i]);
        ASSERT_EQ(list.size(), i + 1);
        ASSERT_TRUE(list.back() == refs[i]);
    }
    const std::vector<SourceRef> plain = roundTrip(list, "generated entries");
    ASSERT_EQ(plain.size(), refs.size());
    for (size_t i = 0; i < refs.size(); i++)
        ASSERT_TRUE(plain[i] == refs[i]) << i;
    for (size_t i = 0; i < refs.size(); i += 7)
        ASSERT_TRUE(list[i] == refs[i]) << i;

    // appended as std::vector::insert at the end appends, and built from a list
    SourceRefList both = {SourceRef{9, 9, 9}};
    both.insert(both.end(), list.begin(), list.end());
    ASSERT_EQ(both.size(), refs.size() + 1);
    EXPECT_TRUE(both[0] == (SourceRef{9, 9, 9}));
    EXPECT_TRUE(both[refs.size()] == refs.back());
    SourceRefList copy = list;
    EXPECT_TRUE(copy == list);
    copy.push_back(SourceRef{});
    EXPECT_TRUE(copy != list);
    list.clear();
    EXPECT_EQ(list.size(), 0u);
    list.push_back(SourceRef{4, 4, 4});
    EXPECT_TRUE(list[0] == (SourceRef{4, 4, 4}));
}

TEST(CompatSourceRefList, EveryFixtureMapReadsBackTheSame){
    const std::string root = cssCaseRoot();
    size_t lines = 0, bytes = 0;
    for (const ForkFeatures& fork : {ForkFeatures::none(), ForkFeatures::present(), ForkFeatures::all()}){
        for (const std::string& name : listCssCases(root)){
            const CssCaseResult result = runCssCase(root, name, fork);
            for (const StylesheetOutput& sheet : result.result.sheets){
                lines += roundTrip(sheet.lines.lines, "compat/css/" + name).size();
                bytes += sheet.lines.lines.byteSize();
                // one entry for every line of the sheet
                if (sheet.kind != StylesheetInput::Kind::StyleAttribute){
                    EXPECT_EQ(sheet.lines.lines.size(), (size_t)std::count(sheet.rcss.begin(), sheet.rcss.end(), '\n')) << name;
                }
            }
            roundTrip(result.result.importantSheet.lines.lines, "compat/css/" + name + " (!important)");
        }
    }
    EXPECT_GT(lines, 5000u);
    // (a third of the 12 bytes an entry took, or less)
    EXPECT_LT(bytes, lines * 5);

    std::vector<std::string> paths;
    for (const char* directory : {"compat/pages", "compat/html"}){
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(root + "/" + directory, error)){
            if (entry.is_regular_file() && entry.path().extension() == ".html")
                paths.push_back(std::string(directory) + "/" + entry.path().filename().string());
        }
    }
    std::sort(paths.begin(), paths.end());
    ASSERT_GT(paths.size(), 20u);
    for (const std::string& path : paths){
        const std::shared_ptr<const CompatDocument> document = DocumentLoader::compile(path, CompatOptions(), fixtureAccess());
        const std::vector<SourceRef> elements = roundTrip(document->sourceMap.elementRefs, path + " elements");
        const std::vector<SourceRef> rmlLines = roundTrip(document->sourceMap.lineRefs, path + " lines");
        // RmlSourceMap::line and RcssLineMap::at answer from the same entries
        for (uint32_t line = 1; line <= rmlLines.size(); line += 3)
            ASSERT_TRUE(document->sourceMap.line(line) == rmlLines[line - 1]) << path;
        EXPECT_FALSE(document->sourceMap.line(0).valid());
        EXPECT_FALSE(document->sourceMap.line((uint32_t)rmlLines.size() + 1).valid());
        for (const StylesheetOutput* sheet : {&document->hints, &document->important})
            roundTrip(sheet->lines.lines, path + " generated sheet");
        for (const StylesheetOutput& sheet : document->stylesheets){
            const std::vector<SourceRef> sheetLines = roundTrip(sheet.lines.lines, path + " sheet");
            for (uint32_t line = 1; line <= sheetLines.size(); line += 5)
                ASSERT_TRUE(sheet.lines.at(line) == sheetLines[line - 1]) << path;
        }
    }
}
