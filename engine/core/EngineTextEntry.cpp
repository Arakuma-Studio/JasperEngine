// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The soft keyboard's side of text entry (text-entry spec part A): a platform keyboard's text
// snapshots diffed into the field's edits, and its action key.

#include "Engine.h"

#include "Input.h"
#include "Scene.h"
// a build without the HTML UI (JASPERENGINE_HTMLUI 0) has neither RmlUi nor these headers' sources
#if JASPERENGINE_HTMLUI
#include "htmlui/RmlTextInputHandler.h"
#include "htmlui/TextEntryTraits.h"
#endif
#include "input/InputScheme.h"
#include "input/TextEntry.h"
#include "input/TextInputSync.h"
#include "subsystem/UISystem.h"
#include "util/StringUtils.h"

#include <algorithm>
#include <vector>

using namespace jasperengine;

namespace {

    // what the soft keyboard holds, as the engine has it (one keyboard for the app)
    TextInputSync keyboardSync;

    void pressSyntheticKey(int key){
        // the soft keyboard's edits are made by the engine: never input scheme evidence
        Engine::setInputSource(InputSourceTag::Synthetic);
        Engine::systemKeyDown(key, false, 0);
        Engine::setInputSource(InputSourceTag::Synthetic);
        Engine::systemKeyUp(key, false, 0);
    }

    // the edits go through the focused field's own input path
    class FieldEdits : public TextInputSync::Output {
    public:
        void moveCaret(int steps) override{
            for (; steps > 0; steps--)
                pressSyntheticKey(D_KEY_RIGHT);
            for (; steps < 0; steps++)
                pressSyntheticKey(D_KEY_LEFT);
        }
        void backspace() override{
            pressSyntheticKey(D_KEY_BACKSPACE);
        }
        void insertText(const std::string& utf8) override{
            Engine::systemTextInput(utf8);
        }
        void compose(const std::string& utf8, int cursor) override{
            Engine::systemImeComposition(utf8, cursor);
        }
        void cancelComposition() override{
            Engine::systemImeCancel();
        }
    };

}

void Engine::systemTextInputState(const std::u16string& units, int selectionStart, int selectionEnd,
                                  int composingStart, int composingEnd){
    FieldEdits edits;
    keyboardSync.update(units, selectionStart, selectionEnd, composingStart, composingEnd, edits);

#if JASPERENGINE_HTMLUI
    // the keyboard's own selection, in code points: what the resync compares the field with
    std::u32string text;
    std::vector<size_t> unitToCodepoint;
    utf16ToCodepoints(units, text, unitToCodepoint);
    const int unitCount = (int)units.size();
    auto codepointAt = [&](int unit){
        return (int)unitToCodepoint[unit < 0 ? unitCount : std::min(unit, unitCount)];
    };
    RmlTextInputHandler::noteKeyboardText(codepointsToUtf8(text, 0, text.size()), codepointAt(selectionStart), codepointAt(selectionEnd));
#endif
}

void Engine::resetTextInputState(const std::string& utf8, size_t caret){
    bool hadInvalid = false;
    const std::vector<uint32_t> codepoints = StringUtils::decodeUtf8ToCodepoints(utf8, hadInvalid);
    keyboardSync.reset(std::u32string(codepoints.begin(), codepoints.end()), caret);
}

void Engine::systemEditorAction(EnterKey key){
    // Enter on the focused field, with its hint (documents: htmlui::runEnterKeyAction; a component
    // InputField submits on the Enter character)
#if JASPERENGINE_HTMLUI
    const htmlui::ActionKeyScope scope(key);
#else
    (void)key;
#endif
    // a document field takes the key alone: its character would reach the game once the field blurred
    const bool documentField = wantsTextComposition();
    setInputSource(InputSourceTag::Synthetic);
    systemKeyDown(D_KEY_ENTER, false, 0);
    if (!documentField)
        systemCharInput('\r');
    setInputSource(InputSourceTag::Synthetic);
    systemKeyUp(D_KEY_ENTER, false, 0);
}

bool Engine::getTextEntryFocus(TextEntryTraits& traits, bool& keyboardOpen){
#if JASPERENGINE_HTMLUI
    if (RmlTextInputHandler::getActive()){
        traits = RmlTextInputHandler::getKeyboardTraits();
        keyboardOpen = RmlTextInputHandler::isKeyboardShown();
        return true;
    }
#endif
    const std::vector<Scene*> scenes = getInputScenes(false);
    for (auto it = scenes.rbegin(); it != scenes.rend(); ++it){
        if ((*it)->getSystem<UISystem>()->getTypingInputFieldTraits(traits)){
            keyboardOpen = traits.keyboard != KeyboardType::None;
            return true;
        }
    }
    return false;
}
