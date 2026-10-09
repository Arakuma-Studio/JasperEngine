// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// p2-css golden cases (phase-2 §4 p2-css, §5.1): every fixtures/compat/css/<case>.css is
// rewritten with every fork feature off (<case>.rcss, <case>.diag.txt) and with every one on
// (<case>.fork.rcss when the output differs). Regenerate with JASPER_HTMLUI_UPDATE_GOLDEN=1
// and review every diff.

#include "CssCaseRunner.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::test::compat;

namespace {

    bool updateMode(){
        const char* value = std::getenv("JASPER_HTMLUI_UPDATE_GOLDEN");
        return value && std::strcmp(value, "1") == 0;
    }

    void writeFile(const std::string& path, const std::string& content){
        std::ofstream file(path, std::ios::binary);
        file << content;
    }

    void compare(const std::string& root, const std::string& relative, const std::string& actual){
        const std::string path = root + "/" + relative;
        if (updateMode()){
            writeFile(path, actual);
            return;
        }
        std::string expected;
        ASSERT_TRUE(readCaseFile(root, relative, expected)) << relative << " is missing; create it with JASPER_HTMLUI_UPDATE_GOLDEN=1";
        EXPECT_EQ(expected, actual) << relative;
    }

    // the fallback diagnostics a fork feature removes
    bool isFallbackCode(DiagCode code){
        return code == codes::ForkFallback || code == codes::PseudoElementPending || code == codes::IsWhereExpanded ||
               code == codes::EasingApproximated || code == codes::StepsApproximated;
    }

}

TEST(CompatCssGolden, EveryCase){
    const std::string root = cssCaseRoot();
    const std::vector<std::string> cases = listCssCases(root);
    ASSERT_GE(cases.size(), 80u) << "the p2-css cases are missing";
    for (const std::string& name : cases){
        SCOPED_TRACE(name);
        const CssCaseResult off = runCssCase(root, name, ForkFeatures::none());
        compare(root, "compat/css/" + name + ".rcss", off.rcss);
        compare(root, "compat/css/" + name + ".diag.txt", off.diagnostics);

        const CssCaseResult on = runCssCase(root, name, ForkFeatures::all());
        const std::string forkPath = "compat/css/" + name + ".fork.rcss";
        std::string existing;
        const bool hasFork = readCaseFile(root, forkPath, existing);
        if (on.rcss != off.rcss){
            compare(root, forkPath, on.rcss);
        }else if (updateMode()){
            if (hasFork)
                std::remove((root + "/" + forkPath).c_str());
        }else{
            EXPECT_FALSE(hasFork) << forkPath << " exists but the fork output equals the .rcss";
        }
        // with every feature present, no fallback is reported
        for (const Diagnostic& d : on.list)
            EXPECT_FALSE(isFallbackCode(d.code)) << toText(d);
    }
}

TEST(CompatCssGolden, ReportedLocationsAreInTheCaseFile){
    const std::string root = cssCaseRoot();
    for (const std::string& name : listCssCases(root)){
        const CssCaseResult off = runCssCase(root, name, ForkFeatures::none());
        for (const Diagnostic& d : off.list){
            if (d.file.empty())
                continue;
            EXPECT_GT(d.line, 0u) << name << ": " << toText(d);
        }
    }
}
