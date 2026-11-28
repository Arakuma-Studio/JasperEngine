// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANDROID_TEXT_INPUT_H
#define ANDROID_TEXT_INPUT_H

// The text conversions of the Android soft keyboard (GameTextInput), the component InputField's
// diff and the keyboard a field asks for (editorInfoOf), without Android headers, so the core
// tests run them on any host. The document fields' diff is the core's TextInputSync.

#include "input/TextEntry.h"
#include "input/TextInputSync.h"
#include "util/StringUtils.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace jasperengine::android {

// GameTextInput text is Java's modified UTF-8 (NUL as C0 80, characters outside the BMP as
// two encoded surrogates) and its spans count UTF-16 units. Decodes to UTF-16 units; plain
// 4-byte UTF-8 is accepted too and malformed bytes become U+FFFD.
inline std::u16string decodeModifiedUtf8(const char* text, int32_t length) {
    std::u16string out;
    if (!text || length <= 0)
        return out;

    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(text);
    const size_t size = strnlen(text, (size_t)length);
    out.reserve(size);

    auto isCont = [&](size_t i) { return (bytes[i] & 0xC0) == 0x80; };

    size_t i = 0;
    while (i < size) {
        unsigned char b0 = bytes[i];
        if (b0 < 0x80) {
            out.push_back(b0);
            i += 1;
        } else if ((b0 & 0xE0) == 0xC0 && i + 1 < size && isCont(i + 1)) {
            out.push_back((char16_t)(((b0 & 0x1F) << 6) | (bytes[i + 1] & 0x3F)));
            i += 2;
        } else if ((b0 & 0xF0) == 0xE0 && i + 2 < size && isCont(i + 1) && isCont(i + 2)) {
            out.push_back((char16_t)(((b0 & 0x0F) << 12) | ((bytes[i + 1] & 0x3F) << 6) | (bytes[i + 2] & 0x3F)));
            i += 3;
        } else if ((b0 & 0xF8) == 0xF0 && i + 3 < size && isCont(i + 1) && isCont(i + 2) && isCont(i + 3)) {
            uint32_t cp = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(bytes[i + 1] & 0x3F) << 12) |
                          ((uint32_t)(bytes[i + 2] & 0x3F) << 6) | (uint32_t)(bytes[i + 3] & 0x3F);
            if (cp >= 0x10000 && cp <= 0x10FFFF) {
                cp -= 0x10000;
                out.push_back((char16_t)(0xD800 + (cp >> 10)));
                out.push_back((char16_t)(0xDC00 + (cp & 0x3FF)));
            } else {
                out.push_back(0xFFFD);
            }
            i += 4;
        } else {
            out.push_back(0xFFFD);
            i += 1;
        }
    }
    return out;
}

// UTF-16 units to code points: the core's, shared with the document text sync (TextInputSync)
using jasperengine::utf16ToCodepoints;
using jasperengine::codepointsToUtf8;

// Code points of a wide string. wchar_t is 32 bits on Android, but surrogate pairs that
// arrive as two units are combined too.
inline std::u32string wideToCodepoints(const std::wstring& text) {
    std::u32string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        uint32_t cp = (uint32_t)text[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size() &&
                (uint32_t)text[i + 1] >= 0xDC00 && (uint32_t)text[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t)text[i + 1] - 0xDC00);
            i++;
        } else if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
            cp = 0xFFFD;
        }
        out.push_back(cp);
    }
    return out;
}

// Java modified UTF-8 of code points, what GameTextInput hands to NewStringUTF;
// utf16Length receives the length in UTF-16 units
inline std::string codepointsToModifiedUtf8(const std::u32string& text, int32_t& utf16Length) {
    std::string out;
    utf16Length = 0;

    auto put3 = [&out](uint32_t unit) {
        out.push_back((char)(0xE0 | (unit >> 12)));
        out.push_back((char)(0x80 | ((unit >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (unit & 0x3F)));
    };

    for (char32_t c : text) {
        uint32_t cp = c;
        if (cp != 0 && cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            put3(cp);
        } else {
            cp -= 0x10000;
            put3(0xD800 + (cp >> 10));
            put3(0xDC00 + (cp & 0x3FF));
            utf16Length++;
        }
        utf16Length++;
    }
    return out;
}

// android.text.InputType and android.view.inputmethod.EditorInfo values
constexpr int32_t TYPE_CLASS_TEXT = 0x1;
constexpr int32_t TYPE_CLASS_NUMBER = 0x2;
constexpr int32_t TYPE_CLASS_PHONE = 0x3;
constexpr int32_t TYPE_TEXT_VARIATION_URI = 0x10;
constexpr int32_t TYPE_TEXT_VARIATION_EMAIL_ADDRESS = 0x20;
constexpr int32_t TYPE_TEXT_VARIATION_PASSWORD = 0x80;
constexpr int32_t TYPE_NUMBER_VARIATION_PASSWORD = 0x10;
constexpr int32_t TYPE_TEXT_FLAG_CAP_CHARACTERS = 0x1000;
constexpr int32_t TYPE_TEXT_FLAG_CAP_WORDS = 0x2000;
constexpr int32_t TYPE_TEXT_FLAG_CAP_SENTENCES = 0x4000;
constexpr int32_t TYPE_TEXT_FLAG_AUTO_CORRECT = 0x8000;
constexpr int32_t TYPE_TEXT_FLAG_MULTI_LINE = 0x20000;
constexpr int32_t TYPE_TEXT_FLAG_NO_SUGGESTIONS = 0x80000;
constexpr int32_t TYPE_NUMBER_FLAG_SIGNED = 0x1000;
constexpr int32_t TYPE_NUMBER_FLAG_DECIMAL = 0x2000;
constexpr int32_t IME_ACTION_UNSPECIFIED = 0;
constexpr int32_t IME_ACTION_NONE = 1;
constexpr int32_t IME_ACTION_GO = 2;
constexpr int32_t IME_ACTION_SEARCH = 3;
constexpr int32_t IME_ACTION_SEND = 4;
constexpr int32_t IME_ACTION_NEXT = 5;
constexpr int32_t IME_ACTION_DONE = 6;
constexpr int32_t IME_ACTION_PREVIOUS = 7;
constexpr int32_t IME_MASK_ACTION = 0xff;
constexpr int32_t IME_FLAG_NO_FULLSCREEN = 0x02000000;
constexpr int32_t IME_FLAG_NO_EXTRACT_UI = 0x10000000;

struct EditorInfo {
    int32_t inputType = TYPE_CLASS_TEXT;
    int32_t imeOptions = IME_FLAG_NO_FULLSCREEN | IME_FLAG_NO_EXTRACT_UI;
    bool operator==(const EditorInfo&) const = default;
};

// The EditorInfo of a text field's traits (text-entry spec D4): GameActivity_setImeEditorInfo's
// inputType and imeOptions. The keyboard never goes full screen over the game.
inline EditorInfo editorInfoOf(const jasperengine::TextEntryTraits& traits) {
    using jasperengine::KeyboardType;
    using jasperengine::EnterKey;
    using jasperengine::AutoCapitalize;
    EditorInfo info;
    switch (traits.keyboard) {
        case KeyboardType::Number:
            info.inputType = TYPE_CLASS_NUMBER | (traits.signedNumber ? TYPE_NUMBER_FLAG_SIGNED : 0);
            break;
        case KeyboardType::Decimal:
            info.inputType = TYPE_CLASS_NUMBER | TYPE_NUMBER_FLAG_DECIMAL | (traits.signedNumber ? TYPE_NUMBER_FLAG_SIGNED : 0);
            break;
        case KeyboardType::Phone:
            info.inputType = TYPE_CLASS_PHONE;
            break;
        case KeyboardType::Email:
            info.inputType = TYPE_CLASS_TEXT | TYPE_TEXT_VARIATION_EMAIL_ADDRESS;
            break;
        case KeyboardType::Url:
            info.inputType = TYPE_CLASS_TEXT | TYPE_TEXT_VARIATION_URI;
            break;
        case KeyboardType::Text: case KeyboardType::Search: case KeyboardType::None:
            info.inputType = TYPE_CLASS_TEXT;
            break;
    }
    const int32_t inputClass = info.inputType & 0xf;
    if (traits.secure) {
        // a password: the digits' variation on a number pad, else the text one
        if (inputClass == TYPE_CLASS_TEXT)
            info.inputType = TYPE_CLASS_TEXT | TYPE_TEXT_VARIATION_PASSWORD | TYPE_TEXT_FLAG_NO_SUGGESTIONS;
        else
            info.inputType = TYPE_CLASS_NUMBER | TYPE_NUMBER_VARIATION_PASSWORD;
    } else if (inputClass == TYPE_CLASS_TEXT) {
        switch (traits.capitalize) {
            case AutoCapitalize::Sentences: info.inputType |= TYPE_TEXT_FLAG_CAP_SENTENCES; break;
            case AutoCapitalize::Words: info.inputType |= TYPE_TEXT_FLAG_CAP_WORDS; break;
            case AutoCapitalize::Characters: info.inputType |= TYPE_TEXT_FLAG_CAP_CHARACTERS; break;
            case AutoCapitalize::Off: break;
        }
        if (traits.autocorrect)
            info.inputType |= TYPE_TEXT_FLAG_AUTO_CORRECT;
        if (traits.multiline)
            info.inputType |= TYPE_TEXT_FLAG_MULTI_LINE;
    }

    int32_t action = IME_ACTION_UNSPECIFIED;
    switch (traits.enterKey) {
        case EnterKey::Default: action = IME_ACTION_UNSPECIFIED; break;
        case EnterKey::Enter: action = IME_ACTION_NONE; break;
        case EnterKey::Done: action = IME_ACTION_DONE; break;
        case EnterKey::Go: action = IME_ACTION_GO; break;
        case EnterKey::Next: action = IME_ACTION_NEXT; break;
        case EnterKey::Previous: action = IME_ACTION_PREVIOUS; break;
        case EnterKey::Search: action = IME_ACTION_SEARCH; break;
        case EnterKey::Send: action = IME_ACTION_SEND; break;
    }
    info.imeOptions = action | IME_FLAG_NO_FULLSCREEN | IME_FLAG_NO_EXTRACT_UI;
    return info;
}

// The EditorInfo the IME of one activity has: set again (and the input restarted) only when a field
// asks for another, so moving between fields of one kind keeps the keyboard as it is
class ImeEditorInfoCache {
public:
    // true when `info` must go to the IME (GameActivity_setImeEditorInfo)
    bool changes(const EditorInfo& info) {
        if (set && info == current)
            return false;
        current = info;
        set = true;
        return true;
    }

private:
    // per activity (a NativeEngine member): a recreated activity's IME starts from its defaults
    EditorInfo current;
    bool set = false;
};

// The soft keyboard's action key (APP_CMD_EDITOR_ACTION) reaches the looper as a command, which a
// loop pass handles before the text GameActivity flagged in the same pass (textInputState). The
// IME sent that text first (Gboard finishes the composing word, then the action), so it goes to
// the field first and is not applied again later in the pass
template<typename ApplyText, typename Action>
void editorActionWithPendingText(bool& textPending, ApplyText&& applyText, Action&& action) {
    if (textPending) {
        textPending = false;
        applyText();
    }
    action();
}

// The action key an IME reports (an EditorInfo action) as the field's enter key
inline jasperengine::EnterKey enterKeyOfImeAction(int32_t action) {
    using jasperengine::EnterKey;
    switch (action & IME_MASK_ACTION) {
        case IME_ACTION_NONE: return EnterKey::Enter;
        case IME_ACTION_GO: return EnterKey::Go;
        case IME_ACTION_SEARCH: return EnterKey::Search;
        case IME_ACTION_SEND: return EnterKey::Send;
        case IME_ACTION_NEXT: return EnterKey::Next;
        case IME_ACTION_DONE: return EnterKey::Done;
        case IME_ACTION_PREVIOUS: return EnterKey::Previous;
        default: return EnterKey::Default;
    }
}

// What a text change the keyboard reports does to the component InputField: backspaces at
// the end of the text, then characters typed there
struct TextEdit {
    size_t backspaces = 0;
    std::u32string typed;
};

// The component InputField's view of the keyboard's text: the soft keyboard holds the text
// in UTF-16 units, the field in code points
class ComponentTextInput {
public:
    // the field's text when the keyboard opens (showSoftInput), or after a document field took
    // the keyboard's text
    void reset(const std::u32string& codepoints) {
        text.assign(codepoints.begin(), codepoints.end());
    }

    // the keyboard's new text: what to send to the field, by whole characters (a pair of
    // UTF-16 units is one character, typed once and erased with one backspace)
    TextEdit update(const std::u16string& units) {
        std::u32string next;
        std::vector<size_t> unitToCodepoint;
        utf16ToCodepoints(units, next, unitToCodepoint);
        size_t prefix = 0;
        while (prefix < text.size() && prefix < next.size() && text[prefix] == next[prefix]) {
            prefix++;
        }
        TextEdit edit;
        edit.backspaces = text.size() - prefix;
        edit.typed = next.substr(prefix);
        text = std::move(next);
        return edit;
    }

    const std::u32string& current() const {
        return text;
    }

private:
    std::u32string text;
};

}

#endif // ANDROID_TEXT_INPUT_H
