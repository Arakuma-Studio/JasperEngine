// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// game-ui-localization.md's test project (R7, R10, AC15): String Tables read as JSON
// (assets/l10n), the game language resolved from the device, and the device's languages changing
// while the game runs. It logs the resolved language ("[Localization] language de", the engine's
// line) and the message of ui.menu.play at start and after each change. The web build
// (WEB_PROJECT=localization tools/build.sh web, GATE=localization tools/build.sh web-gate) starts
// with navigator.languages ['de'] and dispatches languagechange with ['ja']; on Android
// (ANDROID_PROJECT=localization tools/build.sh android) tools/android-emulator-test.sh
// --locale-change ja-JP changes the system language while it runs.

#include "JasperEngine.h"

using namespace jasperengine;

Scene scene;

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(512, 256);
    Engine::setScene(&scene);

    LocalizationManifest manifest;
    manifest.defaultLanguage = "en";
    manifest.rememberLanguage = false;
    manifest.languages = {"en", "de", "ja"};
    manifest.names = {{"en", "English"}, {"de", "Deutsch"}, {"ja", "日本語"}};
    manifest.namespaces.push_back({"ui", "en", {{"en", "l10n/ui.stringtable"}, {"de", "l10n/ui.de.translation"},
                                                {"ja", "l10n/ui.ja.translation"}}});
    Localization::setManifest(manifest);
    Log::print("[gate] l10n %s", Localization::get("ui.menu.play").c_str());
    Localization::onLanguageChanged.add("gate", [](const std::string& code) {
        Log::print("[gate] l10n changed %s %s", code.c_str(), Localization::get("ui.menu.play").c_str());
    });
}
