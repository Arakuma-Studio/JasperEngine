// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Helpers of the scripting tests (phase 1 §3.9): a scene with one UIDocument, the engine's Lua
// state with the document as the global `doc`, and Lua chunks that assert in Lua.

#ifndef HTMLUI_TEST_SCRIPT_COMMON_H
#define HTMLUI_TEST_SCRIPT_COMMON_H

#include "TestDocumentCommon.h"

#include "object/ui/HtmlUIEventListener.h"
#include "object/ui/HtmlUIScriptInternal.h"
#include "script/LuaBinding.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace jasperengine { namespace test {

    class ScriptTest : public ::testing::Test {
    protected:
        // the document file to load in SetUp ("" = none)
        virtual const char* documentFile() const { return "p1/script_dom.html"; }

        void SetUp() override{
            ASSERT_TRUE(GpuEnvironment::ensure()) << "no headless GPU context";
            GpuEnvironment::setView(640, 360, 640, 360, Scaling::FITWIDTH);
            L = LuaBinding::getLuaState();
            ASSERT_NE(L, nullptr);
            fixture.reset(new SceneFixture());
            document = &fixture->add<UIDocument>();
            if (documentFile()[0]){
                ASSERT_TRUE(document->load(documentFile()));
            }
            setDocumentGlobal();
            TestSystem::get().clearLogs();
        }

        void TearDown() override{
            // Lua handles of this test go first, then the scene with its documents
            lua(R"(doc = nil; result = nil; model = nil; el = nil; log = nil; collectgarbage())");
            fixture.reset();
            lua("collectgarbage()");
            EXPECT_EQ(HtmlUIScriptState::liveLuaRefs(), 0u);
            EXPECT_EQ(HtmlUIElementRegistry::poolSize(), 0u);
            EXPECT_EQ(HtmlUI::getReferenceCount(), 0);
        }

        void setDocumentGlobal(){
            if (!luabridge::push(L, *document))
                lua_pushnil(L);
            lua_setglobal(L, "doc");
        }

        // runs a chunk; "" on success, else the error message
        std::string lua(const std::string& code){
            if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
                std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
                lua_pop(L, 1);
                return error;
            }
            return std::string();
        }

        std::string luaString(const std::string& expression){
            lua("result = tostring(" + expression + ")");
            lua_getglobal(L, "result");
            std::string value = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
            lua_pop(L, 1);
            return value;
        }

        Scene& scene(){
            return fixture->getScene();
        }

        Rml::Context* context(){
            return rmlContext(scene());
        }

        void update(){
            updateDocuments(scene());
        }

        lua_State* L = nullptr;
        std::unique_ptr<SceneFixture> fixture;
        UIDocument* document = nullptr;
    };

} }

// a Lua chunk must run without error
#define EXPECT_LUA(code) EXPECT_EQ(lua(code), "")

#endif //HTMLUI_TEST_SCRIPT_COMMON_H
