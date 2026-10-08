// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The diagnostic code registry (phase-2 Appendix F): sorted, unique, complete rows, and a
// scan of the compat and editor importer sources: every literal "HTMLUI-AREA-NNNN" and every
// codes::Name constant they use is registered.

#include "CompatTestCommon.h"

#include "htmlui/compat/DiagnosticCodes.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <regex>
#include <set>

using namespace jasperengine::htmlui::compat;
namespace fs = std::filesystem;

namespace {

    // "unsupported-property" -> "UnsupportedProperty"
    std::string constantName(const char* kebab){
        std::string name;
        bool upper = true;
        for (const char* c = kebab; *c; c++){
            if (*c == '-'){
                upper = true;
                continue;
            }
            name += upper ? (char)std::toupper((unsigned char)*c) : *c;
            upper = false;
        }
        return name;
    }

    std::vector<fs::path> sourceFiles(const fs::path& root){
        std::vector<fs::path> files;
        std::error_code ec;
        if (!fs::exists(root, ec))
            return files;
        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root, ec)){
            const std::string extension = entry.path().extension().string();
            if (entry.is_regular_file() && (extension == ".cpp" || extension == ".h" || extension == ".inl"))
                files.push_back(entry.path());
        }
        return files;
    }

    std::string readAll(const fs::path& path){
        std::ifstream file(path, std::ios::binary);
        std::ostringstream stream;
        stream << file.rdbuf();
        return stream.str();
    }

}

TEST(CompatCodes, RegistryIsSortedUniqueAndComplete){
    const Span<const DiagCodeInfo> codes = diagnosticCodes();
    ASSERT_GT(codes.size(), 80u);
    std::set<std::string> names;
    std::set<std::string> ids;
    for (size_t i = 0; i < codes.size(); i++){
        const DiagCodeInfo& info = codes[i];
        if (i > 0){
            EXPECT_TRUE(codes[i - 1].code < info.code) << info.code.id() << " is not after " << codes[i - 1].code.id();
        }
        EXPECT_TRUE(ids.insert(info.code.id()).second) << "duplicate " << info.code.id();
        EXPECT_TRUE(names.insert(info.name).second) << "duplicate name " << info.name;
        EXPECT_GT(info.code.number, 0);
        EXPECT_TRUE(std::regex_match(std::string(info.name), std::regex("[a-z0-9]+(-[a-z0-9]+)*"))) << info.name;
        EXPECT_GT(std::strlen(info.title), 0u) << info.name;
        EXPECT_GT(std::strlen(info.help), 40u) << info.name;
    }
}

TEST(CompatCodes, NumbersAreDenseWithinEachArea){
    // codes are never renumbered: each area counts 1, 2, 3, ... (a removed situation keeps its row)
    DiagArea area = DiagArea::GEN;
    uint16_t expected = 1;
    for (const DiagCodeInfo& info : diagnosticCodes()){
        if (info.code.area != area){
            area = info.code.area;
            expected = 1;
        }
        EXPECT_EQ(info.code.number, expected) << info.code.id();
        expected++;
    }
}

TEST(CompatCodes, AppendixFSeverities){
    // a sample of Appendix F, including "by case" rows
    struct Expected { const char* id; const char* name; Severity severity; bool byCase; };
    const Expected expected[] = {
        {"HTMLUI-GEN-0001", "file-not-found", Severity::Error, false},
        {"HTMLUI-GEN-0002", "diagnostics-truncated", Severity::Info, false},
        {"HTMLUI-GEN-0006", "rmlui-message", Severity::Warning, false},
        {"HTMLUI-HTML-0004", "unsupported-attribute", Severity::Warning, true},
        {"HTMLUI-HTML-0006", "javascript", Severity::Error, false},
        {"HTMLUI-CSS-0002", "unsupported-property", Severity::Warning, true},
        {"HTMLUI-CSS-0007", "fork-fallback", Severity::Warning, true},
        {"HTMLUI-CSS-0023", "unknown-font-family", Severity::Info, false},
        {"HTMLUI-SEL-0003", "pseudo-element-pending", Severity::Warning, false},
        {"HTMLUI-AT-0007", "import-problem", Severity::Warning, true},
        {"HTMLUI-IMP-0001", "not-imported", Severity::Error, false},
        {"HTMLUI-SVG-0003", "svg-internal-css", Severity::Info, false},
    };
    for (const Expected& e : expected){
        const DiagCodeInfo* byId = findDiagnosticCode(e.id);
        ASSERT_NE(byId, nullptr) << e.id;
        EXPECT_STREQ(byId->name, e.name);
        EXPECT_EQ(byId->severity, e.severity) << e.id;
        EXPECT_EQ(byId->severityByCase, e.byCase) << e.id;
        EXPECT_EQ(findDiagnosticCode(e.name), byId);
        EXPECT_EQ(findDiagnosticCode(byId->code), byId);
    }
    EXPECT_NE(findDiagnosticCode("htmlui-css-0004"), nullptr);
    EXPECT_EQ(findDiagnosticCode("HTMLUI-CSS-0999"), nullptr);
    EXPECT_EQ(findDiagnosticCode("no-such-code"), nullptr);
    EXPECT_EQ(findDiagnosticCode(DiagCode{DiagArea::CSS, 999}), nullptr);
}

TEST(CompatCodes, SourcesUseOnlyRegisteredCodes){
    const fs::path root = fs::path(JASPER_HTMLUI_TEST_SOURCE_DIR) / ".." / ".." / "..";
    const std::vector<fs::path> roots = {root / "engine" / "core" / "htmlui" / "compat", root / "editor" / "htmlui"};

    std::set<std::string> registeredIds;
    std::set<std::string> registeredConstants;
    for (const DiagCodeInfo& info : diagnosticCodes()){
        registeredIds.insert(info.code.id());
        registeredConstants.insert(constantName(info.name));
    }

    // every constant of DiagnosticCodes.h is a registry row
    const std::string header = readAll(roots[0] / "DiagnosticCodes.h");
    const std::regex declared(R"(inline constexpr DiagCode (\w+)\{DiagArea::(\w+), (\d+)\})");
    size_t constants = 0;
    for (auto it = std::sregex_iterator(header.begin(), header.end(), declared); it != std::sregex_iterator(); ++it){
        constants++;
        const std::string name = (*it)[1];
        EXPECT_TRUE(registeredConstants.count(name)) << "constant " << name << " has no registry row";
        char id[32];
        std::snprintf(id, sizeof(id), "HTMLUI-%s-%04d", (*it)[2].str().c_str(), std::stoi((*it)[3]));
        EXPECT_TRUE(registeredIds.count(id)) << "constant " << name << " = " << id << " is not registered";
        const DiagCodeInfo* info = findDiagnosticCode(id);
        if (info)
            EXPECT_EQ(constantName(info->name), name) << id;
    }
    EXPECT_EQ(constants, diagnosticCodes().size());

    const std::regex literal(R"(HTMLUI-([A-Z]+)-(\d{4}))");
    const std::regex constant(R"(codes::(\w+))");
    size_t scanned = 0;
    for (const fs::path& directory : roots){
        for (const fs::path& file : sourceFiles(directory)){
            scanned++;
            const std::string text = readAll(file);
            for (auto it = std::sregex_iterator(text.begin(), text.end(), literal); it != std::sregex_iterator(); ++it){
                const std::string id = (*it)[0];
                // the documented form "HTMLUI-<AREA>-NNNN" is not an id
                EXPECT_TRUE(registeredIds.count(id)) << file.string() << ": " << id << " is not registered";
            }
            for (auto it = std::sregex_iterator(text.begin(), text.end(), constant); it != std::sregex_iterator(); ++it){
                const std::string name = (*it)[1];
                EXPECT_TRUE(registeredConstants.count(name)) << file.string() << ": codes::" << name << " is not registered";
            }
        }
    }
    EXPECT_GT(scanned, 20u);
}
