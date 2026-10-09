// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The property rewrite registry (phase-2 §3.5, §4 p2-css): every RewriteId a table row uses
// has a registered function and every registered function is used by a row.

#include "htmlui/compat/SupportTables.h"
#include "htmlui/compat/css/props/Rewriters.h"

#include <gtest/gtest.h>

#include <set>

using namespace jasperengine::htmlui::compat;

TEST(CompatRewriteRegistry, EveryUsedIdHasAFunctionAndEveryFunctionIsUsed){
    std::set<RewriteId> used;
    for (const PropertySupport& row : propertyTable()){
        if (row.rewrite != RewriteId::None)
            used.insert(row.rewrite);
    }
    std::set<RewriteId> registered;
    for (const RewriteRegistration& r : rewriteRegistry()){
        EXPECT_NE(r.function, nullptr) << rewriteIdName(r.id);
        EXPECT_TRUE(registered.insert(r.id).second) << "registered twice: " << rewriteIdName(r.id);
    }
    for (RewriteId id : used)
        EXPECT_TRUE(registered.count(id)) << "no rewrite for RewriteId::" << rewriteIdName(id);
    for (RewriteId id : registered)
        EXPECT_TRUE(used.count(id)) << "RewriteId::" << rewriteIdName(id) << " has a function but no table row";
    for (size_t i = 1; i < (size_t)RewriteId::Count; i++)
        EXPECT_NE(findRewrite((RewriteId)i), nullptr) << rewriteIdName((RewriteId)i);
    EXPECT_EQ(findRewrite(RewriteId::None), nullptr);
}

TEST(CompatRewriteRegistry, VarFamilies){
    for (const char* family : {"decorator", "mask", "font-effect", "transition", "animation", "font-family", "filter", "easing"}){
        const VarFamily* f = findVarFamily(family);
        ASSERT_NE(f, nullptr) << family;
        EXPECT_STREQ(f->name, family);
        EXPECT_NE(f->classify, nullptr);
        EXPECT_NE(f->transform, nullptr);
    }
    EXPECT_EQ(findVarFamily("nope"), nullptr);
    EXPECT_EQ(varFamilyOf("background"), "decorator");
    EXPECT_EQ(varFamilyOf("text-shadow"), "font-effect");
    EXPECT_EQ(varFamilyOf("width"), "");
}
