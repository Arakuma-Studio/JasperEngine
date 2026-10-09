// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Data binding (phase 1 §3.9.4) from Lua tables (proxies) and C++ (UIDataModel): data-for,
// {{ }}, two-way data-value/data-checked with onChange, data-event-*, fixed top-level keys,
// removal, and createDataModel after the document loaded.

#include "TestScriptCommon.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Input.h>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class ScriptDataModelTest : public ScriptTest {
    protected:
        // the model must exist before the document is parsed: each test loads it
        const char* documentFile() const override { return ""; }

        void loadModelDocument(){
            ASSERT_TRUE(document->load("p1/script_model.rml"));
            update();
        }

        // the rows data-for made (RmlUi keeps the data-for element itself, hidden, as the template)
        std::vector<UIElement> rows(){
            std::vector<UIElement> result;
            for (const UIElement& row : document->getElementById("rows").getChildren()){
                if (!row.hasAttribute("data-for"))
                    result.push_back(row);
            }
            return result;
        }

        std::vector<std::string> rowTexts(){
            std::vector<std::string> texts;
            for (const UIElement& row : rows()){
                texts.push_back(row.getTextContent());
            }
            return texts;
        }

        // at the end of the field's text
        void typeInto(const char* id, const std::string& text){
            document->getElementById(id).focus();
            context()->ProcessKeyDown(Rml::Input::KI_END, 0);
            context()->ProcessKeyUp(Rml::Input::KI_END, 0);
            context()->ProcessTextInput(text);
            update();
        }
    };

    const char* createShop = R"(
        bought = {}
        model = doc:createDataModel("shop", {
            gold = 10,
            items = { {name = "Sword", price = 5}, {name = "Shield", price = 8} },
            text = "abc",
            flag = false,
            buy = function(event, index)
                assert(event.type == "click", event.type)
                bought[#bought + 1] = index
            end,
        })
    )";

}

TEST_F(ScriptDataModelTest, LuaBindingAndProxies){
    ASSERT_EQ(lua(createShop), "");
    loadModelDocument();
    EXPECT_EQ(document->getElementById("gold").getTextContent(), "10");
    EXPECT_EQ(rowTexts(), (std::vector<std::string>{"Sword: 5", "Shield: 8"}));

    EXPECT_LUA(R"(
        assert(model.gold == 10 and model.text == "abc" and model.flag == false)
        assert(#model.items == 2 and model.items[1].name == "Sword" and model.items[2].price == 8)
        assert(model.items[3] == nil)
        assert(model:getName() == "shop" and model:isValid())
        assert(model:get("items[1].name") == "Shield")
        local keys = {}
        for k, v in pairs(model) do keys[#keys + 1] = k end
        assert(table.concat(keys, ",") == "flag,gold,items,text", table.concat(keys, ","))
        local names = {}
        for i, item in ipairs(model.items) do names[i] = item.name end
        assert(table.concat(names, ",") == "Sword,Shield")
        for k, v in pairs(model.items[1]) do names[#names + 1] = k end

        model.gold = 20
        table.insert(model.items, {name = "Bow", price = 3})
        model.items[2].name = "Big Shield"
        assert(model.items == model.items and tostring(model.items) == "data model 'shop' 'items'")
    )");
    update();
    EXPECT_EQ(document->getElementById("gold").getTextContent(), "20");
    EXPECT_EQ(rowTexts(), (std::vector<std::string>{"Sword: 5", "Big Shield: 8", "Bow: 3"}));

    EXPECT_LUA(R"(
        table.remove(model.items, 1)
        model.items[#model.items + 1] = {name = "Axe", price = 4}
        model:set("gold", 7)
        assert(model.gold == 7)
        local copy = model.items[1]
        model.items = {copy, {name = "Gem", price = 99}}
    )");
    update();
    EXPECT_EQ(document->getElementById("gold").getTextContent(), "7");
    EXPECT_EQ(rowTexts(), (std::vector<std::string>{"Big Shield: 8", "Gem: 99"}));
}

TEST_F(ScriptDataModelTest, LuaTwoWayAndEvents){
    ASSERT_EQ(lua(createShop), "");
    loadModelDocument();
    EXPECT_LUA(R"(
        changes = {}
        changeId = model:onChange("text", function(value) changes[#changes + 1] = value end)
        model:onChange("flag", function(value) changes[#changes + 1] = tostring(value) end)
    )");

    // data-value: typing writes the model; onChange runs after the update
    typeInto("name", "d");
    EXPECT_LUA(R"(
        assert(model.text == "abcd", model.text)
        assert(#changes == 1 and changes[1] == "abcd", tostring(changes[1]))
    )");
    // data-checked
    document->getElementById("flag").click();
    update();
    EXPECT_LUA(R"(
        assert(model.flag == true)
        assert(changes[2] == "true")
        model:removeOnChange(changeId)
    )");
    typeInto("name", "e");
    EXPECT_LUA("assert(#changes == 2 and model.text == 'abcde')");

    // data-event-click="buy(i)": the 0-based index of the row
    rows()[1].click();
    rows()[0].click();
    EXPECT_LUA(R"(assert(#bought == 2 and bought[1] == 1 and bought[2] == 0, #bought))");

    // writes from Lua keep the type the model gave (a number stays a number)
    EXPECT_LUA("model.gold = 3; assert(math.type(model.gold) == 'integer')");
}

TEST_F(ScriptDataModelTest, LuaErrors){
    ASSERT_EQ(lua(createShop), "");
    loadModelDocument();

    std::string error = lua("model.silver = 1");
    EXPECT_NE(error.find("has no variable 'silver'"), std::string::npos) << error;
    error = lua("model.items[1].onBuy = function() end");
    EXPECT_NE(error.find("functions are event callbacks"), std::string::npos) << error;
    error = lua("model.items[5] = {}");
    EXPECT_NE(error.find("out of range"), std::string::npos) << error;
    error = lua(R"(doc:createDataModel("shop", {}))");
    EXPECT_NE(error.find("already exists"), std::string::npos) << error;
    error = lua(R"(doc:createDataModel("other", {get = 1}))");
    EXPECT_NE(error.find("data model method"), std::string::npos) << error;
    error = lua(R"(doc:createDataModel("other", {[1] = 1}))");
    EXPECT_NE(error.find("must be a string"), std::string::npos) << error;
    error = lua(R"(local t = {}; t.self = t; doc:createDataModel("cyclic", {t = t}))");
    EXPECT_NE(error.find("too deep"), std::string::npos) << error;

    EXPECT_LUA(R"(
        assert(doc:getDataModel("shop") == model)
        assert(doc:getDataModel("nothing") == nil)
        local items = model.items
        doc:removeDataModel("shop")
        assert(model:isValid() == false)
        assert(doc:getDataModel("shop") == nil)
        result = items
    )");
    error = lua("return model.gold");
    EXPECT_NE(error.find("data model 'shop' was removed"), std::string::npos) << error;
    error = lua("model.gold = 1");
    EXPECT_NE(error.find("data model 'shop' was removed"), std::string::npos) << error;
    error = lua("return #result + result[1].price");
    EXPECT_NE(error.find("data model 'shop' was removed"), std::string::npos) << error;
    TestSystem::get().clearLogs();
}

TEST_F(ScriptDataModelTest, CreateAfterLoadReloadsTheDocument){
    ASSERT_TRUE(document->load("p1/script_model.rml"));
    update();
    // no model yet: an empty placeholder binds data-model="shop" without errors
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
    UIElement before = document->getElementById("gold");
    ASSERT_TRUE(before.isValid());
    EXPECT_EQ(before.getTextContent(), "");

    ASSERT_EQ(lua(createShop), "");
    update();
    EXPECT_FALSE(before.isValid()) << "handles obtained before createDataModel are invalid";
    EXPECT_EQ(document->getElementById("gold").getTextContent(), "10");
    EXPECT_EQ(rowTexts(), (std::vector<std::string>{"Sword: 5", "Shield: 8"}));
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);

    // a model the document does not use: no reload
    UIElement gold = document->getElementById("gold");
    EXPECT_LUA(R"(doc:createDataModel("unused", {a = 1}))");
    EXPECT_TRUE(gold.isValid());
    EXPECT_EQ(documentSystem(scene()).getDataModelNames(document->getEntity()), (std::vector<std::string>{"shop", "unused"}));
}

TEST_F(ScriptDataModelTest, CppApi){
    UIValue initial = UIValue::object();
    initial["gold"] = 10;
    initial["items"] = UIValue::array();
    UIValue sword = UIValue::object();
    sword["name"] = "Sword";
    sword["price"] = 5;
    initial["items"].push(sword);
    initial["text"] = "abc";
    initial["flag"] = false;

    UIDataModel model = document->createDataModel("shop", initial);
    ASSERT_TRUE(model.isValid());
    EXPECT_EQ(model.getName(), "shop");
    std::vector<int> bought;
    model.bindEvent("buy", [&](UIEvent& event, const std::vector<UIValue>& args){
        EXPECT_EQ(event.getType(), "click");
        ASSERT_EQ(args.size(), 1u);
        bought.push_back((int)args[0].asNumber());
    });
    std::vector<std::string> changes;
    uint64_t id = model.onChange("text", [&](const UIValue& value){ changes.push_back(value.asString()); });
    EXPECT_NE(id, 0u);

    loadModelDocument();
    EXPECT_EQ(rowTexts(), (std::vector<std::string>{"Sword: 5"}));
    EXPECT_EQ(model.get("items[0].name").asString(), "Sword");
    EXPECT_TRUE(model.get("items[3].name").isNil());

    UIValue shield = UIValue::object();
    shield["name"] = "Shield";
    shield["price"] = 8;
    model.set("items[1]", shield);
    model.set("items[0].price", 6);
    model.set("gold", 11);
    update();
    EXPECT_EQ(rowTexts(), (std::vector<std::string>{"Sword: 6", "Shield: 8"}));
    EXPECT_EQ(document->getElementById("gold").getTextContent(), "11");

    // a missing top-level key and a broken path are errors; nothing changes
    TestSystem::get().clearLogs();
    model.set("silver", 1);
    model.set("items[7].name", "x");
    model.set("items..name", "x");
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 3u);
    TestSystem::get().clearLogs();

    typeInto("name", "d");
    EXPECT_EQ(model.get("text").asString(), "abcd");
    EXPECT_EQ(changes, (std::vector<std::string>{"abcd"}));
    model.removeOnChange(id);
    typeInto("name", "e");
    EXPECT_EQ(changes.size(), 1u);

    rows()[1].click();
    EXPECT_EQ(bought, (std::vector<int>{1}));

    // dirty: a value changed through get/set elsewhere shows after dirty()
    model.dirty("gold");
    model.dirtyAll();

    EXPECT_TRUE(document->getDataModel("shop").isValid());
    EXPECT_FALSE(document->getDataModel("missing").isValid());
    document->removeDataModel("shop");
    EXPECT_FALSE(model.isValid());
    EXPECT_TRUE(model.get("gold").isNil());
    model.set("gold", 1);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "data model 'shop' was removed"), 1u);
    TestSystem::get().clearLogs();
}

TEST_F(ScriptDataModelTest, ModelsSurviveTheContextAndGoWithTheComponent){
    ASSERT_EQ(lua(createShop), "");
    loadModelDocument();
    // a reload keeps the model
    document->reload();
    update();
    EXPECT_EQ(document->getElementById("gold").getTextContent(), "10");
    EXPECT_LUA("assert(model:isValid())");

    // removing the component removes the models it created
    document->removeComponent<UIDocumentComponent>();
    update();
    EXPECT_LUA("assert(not model:isValid())");
}

TEST_F(ScriptDataModelTest, ValueTree){
    UIValue value;
    EXPECT_TRUE(value.isNil());
    value["a"] = 1;
    value["b"]["c"] = "x";
    value["list"].push(true);
    value["list"][2] = 2.5;
    EXPECT_EQ(value.getType(), UIValue::Type::OBJECT);
    EXPECT_EQ(value.keys(), (std::vector<std::string>{"a", "b", "list"}));
    EXPECT_EQ(value["list"].size(), 3u);
    EXPECT_TRUE(value["list"][1].isNil());
    EXPECT_DOUBLE_EQ(value["list"][2].asNumber(), 2.5);
    EXPECT_EQ(value.find("b")->find("c")->asString(), "x");
    EXPECT_EQ(value.find("zzz"), nullptr);
    value["list"].erase(1);
    EXPECT_EQ(value["list"].size(), 2u);
    value.erase("a");
    EXPECT_EQ(value.keys(), (std::vector<std::string>{"b", "list"}));
    EXPECT_TRUE(UIValue("3.5").asNumber() == 3.5);
    EXPECT_TRUE(UIValue(2).asBool());
    EXPECT_EQ(UIValue(2).asString(), "");
    EXPECT_TRUE(UIValue(std::string("s")) == UIValue("s"));
    EXPECT_FALSE(UIValue(1) == UIValue(true));
}
