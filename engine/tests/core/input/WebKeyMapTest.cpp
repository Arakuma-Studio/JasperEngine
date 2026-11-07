// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The web platform's key map (engine/platform/emscripten/WebKeyMap.h; platform-lifecycle spec
// D11): every KeyboardEvent.code the engine has a key for, the names older browsers used, and the
// legacy keyCode fallback of events without a code (Android soft keyboards, old browsers).

#include "WebKeyMap.h"

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>

using namespace jasperengine;

namespace {

    constexpr int LOCATION_STANDARD = 0;
    constexpr int LOCATION_LEFT = 1;
    constexpr int LOCATION_RIGHT = 2;
    constexpr int LOCATION_NUMPAD = 3;

}

TEST(WebKeyMap, ModernMetaAndContextMenu){
    EXPECT_EQ(web::keyFromCode("MetaLeft"), D_KEY_LEFT_SUPER);
    EXPECT_EQ(web::keyFromCode("MetaRight"), D_KEY_RIGHT_SUPER);
    EXPECT_EQ(web::keyFromCode("ContextMenu"), D_KEY_MENU);
    EXPECT_EQ(web::keyFromCode("IntlBackslash"), D_KEY_WORLD_2);
    EXPECT_EQ(web::keyFromCode("BracketRight"), D_KEY_RIGHT_BRACKET);
    EXPECT_EQ(web::keyFromCode("F13"), D_KEY_F13);
    EXPECT_EQ(web::keyFromCode("F24"), D_KEY_F24);
}

TEST(WebKeyMap, LegacyNamesStillMap){
    EXPECT_EQ(web::keyFromCode("OSLeft"), D_KEY_LEFT_SUPER);
    EXPECT_EQ(web::keyFromCode("OSRight"), D_KEY_RIGHT_SUPER);
    EXPECT_EQ(web::keyFromCode("Menu"), D_KEY_MENU);
    EXPECT_EQ(web::keyFromCode(""), 0);
    EXPECT_EQ(web::keyFromCode("Unidentified"), 0);
}

TEST(WebKeyMap, LegacyKeyCodes){
    EXPECT_EQ(web::keyFromKeyCode(8, LOCATION_STANDARD), D_KEY_BACKSPACE);
    EXPECT_EQ(web::keyFromKeyCode(46, LOCATION_STANDARD), D_KEY_DELETE);
    EXPECT_EQ(web::keyFromKeyCode(93, LOCATION_STANDARD), D_KEY_MENU);
    EXPECT_EQ(web::keyFromKeyCode(221, LOCATION_STANDARD), D_KEY_RIGHT_BRACKET);
    EXPECT_EQ(web::keyFromKeyCode(224, LOCATION_LEFT), D_KEY_LEFT_SUPER);
    // Firefox's own codes for ; = -
    EXPECT_EQ(web::keyFromKeyCode(59, LOCATION_STANDARD), D_KEY_SEMICOLON);
    EXPECT_EQ(web::keyFromKeyCode(61, LOCATION_STANDARD), D_KEY_EQUAL);
    EXPECT_EQ(web::keyFromKeyCode(173, LOCATION_STANDARD), D_KEY_MINUS);
    // AltGraph (Firefox on Linux) and the ISO <> key
    EXPECT_EQ(web::keyFromKeyCode(225, LOCATION_RIGHT), D_KEY_RIGHT_ALT);
    EXPECT_EQ(web::keyFromKeyCode(226, LOCATION_STANDARD), D_KEY_WORLD_2);
    for (int i = 0; i < 12; i++){
        EXPECT_EQ(web::keyFromKeyCode(124 + i, LOCATION_STANDARD), D_KEY_F13 + i) << "keyCode " << 124 + i;
    }
    EXPECT_EQ(web::keyFromKeyCode(0, LOCATION_STANDARD), 0);
}

TEST(WebKeyMap, LocationPicksRightModifiers){
    EXPECT_EQ(web::keyFromKeyCode(16, LOCATION_LEFT), D_KEY_LEFT_SHIFT);
    EXPECT_EQ(web::keyFromKeyCode(16, LOCATION_RIGHT), D_KEY_RIGHT_SHIFT);
    EXPECT_EQ(web::keyFromKeyCode(17, LOCATION_RIGHT), D_KEY_RIGHT_CONTROL);
    EXPECT_EQ(web::keyFromKeyCode(18, LOCATION_RIGHT), D_KEY_RIGHT_ALT);
    EXPECT_EQ(web::keyFromKeyCode(91, LOCATION_RIGHT), D_KEY_RIGHT_SUPER);
    EXPECT_EQ(web::keyFromKeyCode(224, LOCATION_RIGHT), D_KEY_RIGHT_SUPER);
    // Chrome and Safari on macOS: the right Command key is keyCode 93 on the right
    EXPECT_EQ(web::keyFromKeyCode(93, LOCATION_RIGHT), D_KEY_RIGHT_SUPER);
    EXPECT_EQ(web::keyFromKeyCode(13, LOCATION_STANDARD), D_KEY_ENTER);
    EXPECT_EQ(web::keyFromKeyCode(13, LOCATION_NUMPAD), D_KEY_KP_ENTER);
}

// The class of bug behind Doriax f9d271489 (BracketLeft listed twice, BracketRight never): every
// engine key a browser can report has a code name, and no name is listed twice
TEST(WebKeyMap, EveryEngineKeyHasACodeName){
    std::set<int> mapped;
    for (const web::KeyCodeName& entry : web::KEY_CODES){
        mapped.insert(entry.key);
    }
    // the printable keys and the function keys of Input.h (GLFW's values)
    std::set<int> engineKeys = {D_KEY_SPACE, D_KEY_APOSTROPHE, D_KEY_COMMA, D_KEY_MINUS, D_KEY_PERIOD, D_KEY_SLASH,
                                D_KEY_SEMICOLON, D_KEY_EQUAL, D_KEY_LEFT_BRACKET, D_KEY_BACKSLASH, D_KEY_RIGHT_BRACKET,
                                D_KEY_GRAVE_ACCENT, D_KEY_WORLD_2};
    for (int key = D_KEY_0; key <= D_KEY_9; key++) engineKeys.insert(key);
    for (int key = D_KEY_A; key <= D_KEY_Z; key++) engineKeys.insert(key);
    for (int key = D_KEY_ESCAPE; key <= D_KEY_END; key++) engineKeys.insert(key);
    for (int key = D_KEY_CAPS_LOCK; key <= D_KEY_PAUSE; key++) engineKeys.insert(key);
    // F25 has no KeyboardEvent.code
    for (int key = D_KEY_F1; key <= D_KEY_F24; key++) engineKeys.insert(key);
    for (int key = D_KEY_KP_0; key <= D_KEY_KP_EQUAL; key++) engineKeys.insert(key);
    for (int key = D_KEY_LEFT_SHIFT; key <= D_KEY_MENU; key++) engineKeys.insert(key);

    for (int key : engineKeys){
        EXPECT_TRUE(mapped.count(key)) << "no KeyboardEvent.code gives engine key " << key;
    }
}

TEST(WebKeyMap, NoCodeNameTwice){
    std::map<std::string, int> seen;
    for (const web::KeyCodeName& entry : web::KEY_CODES){
        EXPECT_TRUE(seen.emplace(std::string(entry.code), entry.key).second) << entry.code << " is listed twice";
    }
}
