// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S17's test project (AC16): two HTML UI examples on a phone's or a browser's path (GLES3 /
// WebGL2), with their Lua scripts: server_browser (a <table-view> of 10,000 rows from a data model)
// and inventory (HTML5 drag and drop, rich tooltips). The files are links to
// docs/mcp/guide/html-ui-examples. Once both documents are loaded and their views filled it logs
// what they show and how many problems the documents reported; a browser
// (WEB_PROJECT=htmlui-widgets tools/build.sh web, GATE=htmlui-widgets tools/build.sh web-gate) and
// an Android emulator (ANDROID_PROJECT=htmlui-widgets tools/build.sh android, then
// tools/android-emulator-test.sh with android-expect.txt) check the lines.

#include "JasperEngine.h"
#include "component/ScriptComponent.h"
#include "object/ui/UIItemsView.h"
#include "script/LuaBinding.h"

using namespace jasperengine;

Scene scene;
int frames = 0;

namespace {

    UIDocument* browser = nullptr;
    UIDocument* inventory = nullptr;

    UIDocument* addDocument(const std::string& html, const std::string& lua, const std::string& className){
        UIDocument* document = new UIDocument(&scene);
        document->setFilename(html);
        ScriptEntry entry;
        entry.type = ScriptType::LUA;
        entry.path = lua;
        entry.className = className;
        entry.enabled = true;
        ScriptComponent scripts;
        scripts.scripts.push_back(entry);
        document->addComponent<ScriptComponent>(scripts);
        return document;
    }

    // the problems a document reported: errors and warnings
    int problems(const UIDocument& document){
        int count = 0;
        for (const htmlui::compat::Diagnostic& diagnostic : document.getDiagnostics())
            count += diagnostic.severity != htmlui::compat::Severity::Info ? 1 : 0;
        return count;
    }

}

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(1280, 720);
    Engine::setScene(&scene);
    scene.setBackgroundColor(0.1f, 0.1f, 0.12f);

    browser = addDocument("server_browser/server_browser.html", "ServerBrowserUI.lua", "ServerBrowserUI");
    inventory = addDocument("inventory/inventory.html", "InventoryUI.lua", "InventoryUI");
    LuaBinding::initializeLuaScripts(&scene);

    // logged once both documents show their rows and slots, or after 600 frames as they are then
    Engine::onUpdate.add("htmluiWidgetsTest", [](){
        if (frames < 0)
            return;
        frames++;
        UIItemsView servers = browser->querySelector("table-view").getItemsView();
        const int slots = (int)inventory->querySelectorAll(".grid > .slot").size();
        const bool ready = browser->isLoaded() && inventory->isLoaded() && servers.isValid() && servers.getCount() > 0 &&
                           servers.getVisibleRange().first > 0 && slots > 0;
        if (!ready && frames < 600)
            return;
        frames = -1;
        Log::print("[widgets] server_browser rows %d problems %d shown %d..%d", servers.isValid() ? servers.getCount() : 0,
                   problems(*browser), servers.getVisibleRange().first, servers.getVisibleRange().second);
        Log::print("[widgets] inventory slots %d problems %d", slots, problems(*inventory));
    });
}
