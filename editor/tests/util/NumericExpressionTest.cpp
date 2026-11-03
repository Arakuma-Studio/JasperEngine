// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Expressions in numeric fields (S23 R25, R26, R35): the grammar, units, relative ops, the
// multi-edit ramps, errors with their position, and the ImGui hook every field goes through.

#include "util/NumericExpression.h"

#include "imgui.h"
#include "imgui_internal.h"

#include "support/EditorTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

using namespace jasperengine::editor;

namespace {

    double eval(const std::string& text, double current = 0.0, NumericContext context = NumericContext()) {
        const NumericResult result = NumericExpression::evaluate(text, current, context);
        EXPECT_TRUE(result.ok) << text << ": " << result.error;
        return result.value;
    }

    NumericContext unitContext(NumericUnit unit) {
        NumericContext context;
        context.unit = unit;
        return context;
    }

}

TEST(NumericExpression, Arithmetic) {
    EXPECT_DOUBLE_EQ(eval("2+2"), 4.0);
    EXPECT_DOUBLE_EQ(eval("2^3^2"), 512.0) << "power is right-associative (TE_POW_FROM_RIGHT)";
    EXPECT_DOUBLE_EQ(eval("log(e)"), 1.0) << "log is natural (TE_NAT_LOG)";
    EXPECT_DOUBLE_EQ(eval("(1 + 2) * 3 / 4"), 2.25);
    EXPECT_NEAR(eval("tau / 2"), 3.14159265358979, 1e-12);
    EXPECT_DOUBLE_EQ(eval("min(3, 4) + max(1, 2)"), 5.0);
    EXPECT_DOUBLE_EQ(eval("clamp(12, 0, 10)"), 10.0);
    EXPECT_DOUBLE_EQ(eval("round(2.5)"), 3.0);
    EXPECT_DOUBLE_EQ(eval("lerp(10, 20, 0.25)"), 12.5);
    EXPECT_DOUBLE_EQ(eval("sqrt(16) + abs(-1)"), 5.0);
}

TEST(NumericExpression, Units) {
    EXPECT_DOUBLE_EQ(eval("90deg", 0, unitContext(NumericUnit::Degrees)), 90.0);
    EXPECT_DOUBLE_EQ(eval("90\xC2\xB0", 0, unitContext(NumericUnit::Degrees)), 90.0);
    EXPECT_NEAR(eval("pi rad", 0, unitContext(NumericUnit::Degrees)), 180.0, 1e-9);
    EXPECT_DOUBLE_EQ(eval("0.5turn", 0, unitContext(NumericUnit::Degrees)), 180.0);
    EXPECT_DOUBLE_EQ(eval("25cm", 0, unitContext(NumericUnit::Meters)), 0.25);
    EXPECT_DOUBLE_EQ(eval("1km + 20 m", 0, unitContext(NumericUnit::Meters)), 1020.0);
    EXPECT_NEAR(eval("2ft", 0, unitContext(NumericUnit::Meters)), 0.6096, 1e-12);
    EXPECT_DOUBLE_EQ(eval("250ms", 0, unitContext(NumericUnit::Seconds)), 0.25);
    EXPECT_DOUBLE_EQ(eval("2min", 0, unitContext(NumericUnit::Seconds)), 120.0);
    EXPECT_DOUBLE_EQ(eval("min(2, 3)", 0, unitContext(NumericUnit::Seconds)), 2.0) << "min( is the function";
    EXPECT_DOUBLE_EQ(eval("12px", 0, unitContext(NumericUnit::Pixels)), 12.0);

    const NumericResult none = NumericExpression::evaluate("5cm", 0.0, NumericContext());
    EXPECT_FALSE(none.ok) << "a unit on a field without one is an error";
    EXPECT_EQ(none.position, 2);
    EXPECT_FALSE(NumericExpression::evaluate("5cm", 0.0, unitContext(NumericUnit::Degrees)).ok);
}

TEST(NumericExpression, RelativeOps) {
    EXPECT_DOUBLE_EQ(eval("+=10", 5), 15.0);
    EXPECT_DOUBLE_EQ(eval("-=1", 5), 4.0);
    EXPECT_DOUBLE_EQ(eval("*=2", 5), 10.0);
    EXPECT_DOUBLE_EQ(eval("/=4", 5), 1.25);
    EXPECT_DOUBLE_EQ(eval("-5", 100), -5.0) << "absolute";
    EXPECT_DOUBLE_EQ(eval("+5", 100), 5.0) << "absolute";
    EXPECT_DOUBLE_EQ(eval("+= 10cm", 1, unitContext(NumericUnit::Meters)), 1.1);
    EXPECT_TRUE(NumericExpression::evaluate("+=1", 0).perObject);
    EXPECT_FALSE(NumericExpression::evaluate("1+1", 0).perObject);
    EXPECT_FALSE(NumericExpression::evaluate("/=0", 1).ok);
}

TEST(NumericExpression, IntegerRounding) {
    NumericContext integer;
    integer.integer = true;
    EXPECT_DOUBLE_EQ(eval("2.5", 0, integer), 3.0);
    EXPECT_DOUBLE_EQ(eval("-2.5", 0, integer), -3.0) << "half away from zero";
    EXPECT_DOUBLE_EQ(eval("7/2", 0, integer), 4.0);
    EXPECT_DOUBLE_EQ(eval("2.4", 0, integer), 2.0);
}

TEST(NumericExpression, RampOverTheSelection) {
    std::vector<double> values;
    ASSERT_TRUE(NumericExpression::evaluateForObjects("L(0,10)", {9, 9, 9, 9, 9}, NumericContext(), values));
    EXPECT_EQ(values, std::vector<double>({0, 2.5, 5, 7.5, 10}));
    ASSERT_TRUE(NumericExpression::evaluateForObjects("+=1", {1, 2, 3}, NumericContext(), values));
    EXPECT_EQ(values, std::vector<double>({2, 3, 4})) << "relative ops apply per object";
    EXPECT_TRUE(NumericExpression::evaluate("L(0,1)", 0).perObject);
}

TEST(NumericExpression, RandomWithinBoundsAndSeeded) {
    NumericContext context;
    context.seed = 42;
    std::vector<double> first, second;
    ASSERT_TRUE(NumericExpression::evaluateForObjects("R(-2, 3)", std::vector<double>(50, 0.0), context, first));
    ASSERT_TRUE(NumericExpression::evaluateForObjects("R(-2, 3)", std::vector<double>(50, 0.0), context, second));
    EXPECT_EQ(first, second) << "the same seed gives the same values";
    bool varied = false;
    for (double value : first) {
        EXPECT_GE(value, -2.0);
        EXPECT_LT(value, 3.0);
        varied = varied || value != first[0];
    }
    EXPECT_TRUE(varied);
    context.seed = 43;
    ASSERT_TRUE(NumericExpression::evaluateForObjects("R(-2, 3)", std::vector<double>(50, 0.0), context, second));
    EXPECT_NE(first, second);
}

TEST(NumericExpression, ErrorPositions) {
    const NumericResult unknown = NumericExpression::evaluate("2*pii", 0);
    EXPECT_FALSE(unknown.ok);
    EXPECT_EQ(unknown.error, "Unknown name 'pii' at 3");
    EXPECT_EQ(unknown.position, 3);
    const NumericResult syntax = NumericExpression::evaluate("2+*3", 0);
    EXPECT_FALSE(syntax.ok);
    EXPECT_GE(syntax.position, 3);
    EXPECT_NE(syntax.error.find("Syntax error"), std::string::npos);
    EXPECT_FALSE(NumericExpression::evaluate("", 0).ok);
    EXPECT_FALSE(NumericExpression::evaluate("sqrt(-1)", 0).ok) << "not a number";
}

TEST(NumericExpression, PlainNumbersAndDecimalComma) {
    EXPECT_TRUE(NumericExpression::isPlainNumber("12"));
    EXPECT_TRUE(NumericExpression::isPlainNumber(" -1.5e3 "));
    EXPECT_TRUE(NumericExpression::isPlainNumber("+.5"));
    EXPECT_FALSE(NumericExpression::isPlainNumber("1+1"));
    EXPECT_FALSE(NumericExpression::isPlainNumber("2e"));
    EXPECT_DOUBLE_EQ(eval("1,5"), 1.5) << "a decimal comma outside a call";
}

TEST(NumericExpression, EvaluationWithinBudget) {
    constexpr int runs = 20000;
    const auto start = std::chrono::steady_clock::now();
    double sum = 0.0;
    for (int i = 0; i < runs; i++) sum += NumericExpression::evaluate("clamp(2*pi rad, 0, 360) + 25", i, unitContext(NumericUnit::Degrees)).value;
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / runs;
    std::printf("[bench] numeric expression %.2f us (budget 10)\n", us);
    EXPECT_GT(sum, 0.0);
    // the budget is the benchmark run's (JASPER_TEST_PERF_SLACK on the benchmark server,
    // docs/testing.md): a shared builder's load, or a sanitizer, alone can miss it
    if (const double slack = jasperengine::editor::test::perfSlack(); slack > 0.0) {
        EXPECT_LE(us, 10.0 * slack) << "R35 (10 us)";
    }
}

// The hook of libs/imgui/JASPER_PATCHES.md, driven as Ctrl+click on a DragFloat drives it
TEST(ImGuiNumericHook, DragFloatCtrlClick) {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    NumericExpression::installImGuiHook();

    float value = 1.0f;
    // 200 characters: past ImGui's old 32-byte TempInputScalar buffer
    std::string expression = "1";
    while (expression.size() < 196) expression += "+1";
    expression += "+0";
    ASSERT_GE(expression.size(), 198u);
    EXPECT_TRUE(ImGui::DataTypeApplyFromText(expression.c_str(), ImGuiDataType_Float, &value, "%.3f"));
    EXPECT_FLOAT_EQ(value, 99.0f);

    // plain numbers keep ImGui's own parsing
    EXPECT_TRUE(ImGui::DataTypeApplyFromText("12.5", ImGuiDataType_Float, &value, "%.3f"));
    EXPECT_FLOAT_EQ(value, 12.5f);

    // an invalid expression leaves the value unchanged
    EXPECT_FALSE(ImGui::DataTypeApplyFromText("2*pii", ImGuiDataType_Float, &value, "%.3f"));
    EXPECT_FLOAT_EQ(value, 12.5f);

    // integer fields round and clamp to the type
    int count = 0;
    EXPECT_TRUE(ImGui::DataTypeApplyFromText("7/2", ImGuiDataType_S32, &count, "%d"));
    EXPECT_EQ(count, 4);
    ImU8 small = 0;
    EXPECT_TRUE(ImGui::DataTypeApplyFromText("2^10", ImGuiDataType_U8, &small, "%d"));
    EXPECT_EQ(small, 255);

    // a field's unit (the Inspector's NumericFieldScope)
    {
        NumericFieldScope scope(NumericUnit::Meters);
        EXPECT_TRUE(ImGui::DataTypeApplyFromText("25cm", ImGuiDataType_Float, &value, "%.3f"));
        EXPECT_FLOAT_EQ(value, 0.25f);
    }
    EXPECT_FALSE(ImGui::DataTypeApplyFromText("25cm", ImGuiDataType_Float, &value, "%.3f"));

    // a multi-edit relative op is handed to the caller for each object
    {
        NumericFieldScope scope(NumericUnit::None, 3);
        float shared = 2.0f;
        EXPECT_TRUE(ImGui::DataTypeApplyFromText("*=2", ImGuiDataType_Float, &shared, "%.3f"));
        std::string text;
        ASSERT_TRUE(NumericExpression::takeMultiEdit(text));
        EXPECT_EQ(text, "*=2");
        EXPECT_FALSE(NumericExpression::takeMultiEdit(text));
    }

    ImGui::SetDataTypeApplyFromTextHook(nullptr, nullptr);
    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
}
