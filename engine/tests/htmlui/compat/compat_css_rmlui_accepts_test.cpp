// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The invariant of phase-2 §5.1: every RCSS the CSS rewriter produces for the golden cases
// (fork features off: the RmlUi of phase 2) is parsed by the real RmlUi without a warning or
// an error (the --html-unset sentinel excepted, which only appears at runtime).

#include "CssCaseRunner.h"
#include "CompatRuntimeStandIns.h"

#include "htmlui/compat/RmlLogTranslator.h"

#include "HtmlUI.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/StreamMemory.h>

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;
using namespace jasperengine::test::compat;

TEST(CompatCssRmlUiAccepts, EveryGoldenCase){
    ASSERT_TRUE(HtmlUI::acquire());
    registerCompatRuntimeStandIns();
    const std::string root = cssCaseRoot();
    const std::vector<std::string> cases = listCssCases(root);
    ASSERT_FALSE(cases.empty());
    for (const std::string& name : cases){
        const CssCaseResult off = runCssCase(root, name, ForkFeatures::none());
        size_t index = 0;
        for (const auto& sheet : off.sheets){
            if (sheet.first.empty())
                continue;
            RmlLogTranslator::Capture capture;
            {
                Rml::StreamMemory stream(reinterpret_cast<const Rml::byte*>(sheet.first.data()), sheet.first.size());
                stream.SetSourceURL("@htmlui/doc/0000000000000000/compat/css/" + name + "-" + std::to_string(index) + ".rcss");
                Rml::Factory::InstanceStyleSheetStream(&stream);
            }
            for (const RmlLogTranslator::Capture::Message& message : capture.messages()){
                if (message.severity == Severity::Info || RmlLogTranslator::isUnsetSentinel(message.text))
                    continue;
                ADD_FAILURE() << name << ": RmlUi: " << message.text << "\n" << sheet.first;
            }
            index++;
        }
    }
    Rml::Factory::ClearStyleSheetCache();
    HtmlUI::release();
}
