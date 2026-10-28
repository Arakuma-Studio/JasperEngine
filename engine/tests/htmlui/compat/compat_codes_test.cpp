// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Diagnostic identities and severities are public compatibility data (phase-2 Appendix F).

#include "htmlui/compat/DiagnosticCodes.h"
#include <gtest/gtest.h>

using namespace jasperengine::htmlui::compat;

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
