// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EditorModal.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace jasperengine::editor;

EditorDialogValue EditorDialogValue::ofText(std::string value) {
    EditorDialogValue out;
    out.kind = Kind::Text;
    out.text = std::move(value);
    return out;
}

EditorDialogValue EditorDialogValue::ofBool(bool value) {
    EditorDialogValue out;
    out.kind = Kind::Bool;
    out.flag = value;
    return out;
}

EditorDialogValue EditorDialogValue::ofNumber(double value) {
    EditorDialogValue out;
    out.kind = Kind::Number;
    out.number = value;
    return out;
}

namespace {

    // until the frame that takes an answer's values has run
    constexpr int NO_PRESS_FRAME = INT_MAX;

    struct Choice {
        std::string choice;
        std::string label;
        bool closeButton = false;   // the title bar's close button (p_open), not a button of its own
        bool disabled = false;
    };

    struct Entry {
        ImGuiID window = 0;
        std::string title;
        std::string message;
        std::string error;
        std::vector<Choice> choices;
        std::vector<EditorDialogField> fields;
        bool hasCloseButton = false;
        EditorModal::Role role = EditorModal::Role::Dialog;
    };

    struct State {
        std::vector<Entry> drawn;   // this frame's modals, in drawing order
        std::vector<std::string> openTitles;    // the last complete frame's modals
        bool hasTop = false;
        Entry top;                  // the last complete frame's top-most modal
        // an agent's answer to `top`, for its next drawing
        ImGuiID pendingWindow = 0;
        std::string pendingChoice;
        std::map<std::string, EditorDialogValue> pendingValues;
        bool pendingDrawn = false;  // the modal has been drawn since the answer: its values have gone in
        int pressFrame = 0;         // the frame of its button: the one after the values, whose checks ran
        std::string answerError;
        int unsettledFrames = 0;
    };

    State& state() {
        static State instance;
        return instance;
    }

    // The words of a window name or button label: before its ImGui id suffix ("Prefabs##PrefabsModal",
    // "###mixer_bus_refs"), without a leading icon (Font Awesome glyphs are in the Private Use Area,
    // U+E000-U+F8FF: EE xx xx, or EF 80-A3 xx in UTF-8)
    std::string titleOf(const char* name) {
        std::string title = name ? name : "";
        const size_t id = title.find("##");
        if (id != std::string::npos) title.resize(id);
        size_t start = 0;
        while (start + 2 < title.size()) {
            const unsigned char lead = static_cast<unsigned char>(title[start]);
            const unsigned char next = static_cast<unsigned char>(title[start + 1]);
            if (lead != 0xEE && !(lead == 0xEF && next >= 0x80 && next <= 0xA3)) break;
            start += 3;
            while (start < title.size() && title[start] == ' ') start++;
        }
        return title.substr(start);
    }

    const Choice* findChoice(const Entry& entry, const std::string& choice) {
        for (const Choice& c : entry.choices) {
            if (c.choice == choice) return &c;
        }
        return nullptr;
    }

    const EditorDialogField* findField(const Entry& entry, const std::string& name) {
        for (const EditorDialogField& f : entry.fields) {
            if (f.name == name) return &f;
        }
        return nullptr;
    }

    std::string joined(const std::vector<std::string>& words) {
        std::string out;
        for (const std::string& word : words) out += (out.empty() ? "" : ", ") + word;
        return out;
    }

    // The modal whose body is being drawn (child windows inside it included)
    Entry* current() {
        ImGuiWindow* window = ImGui::GetCurrentWindowRead();
        if (!window) return nullptr;
        const ImGuiID root = window->RootWindow ? window->RootWindow->ID : window->ID;
        std::vector<Entry>& drawn = state().drawn;
        for (auto it = drawn.rbegin(); it != drawn.rend(); ++it) {
            if (it->window == root) return &*it;
        }
        return nullptr;
    }

    bool pressDue(const State& s) {
        return ImGui::GetFrameCount() >= s.pressFrame;
    }

    bool takePending(ImGuiID window, const std::string& choice) {
        State& s = state();
        if (s.pendingChoice.empty() || s.pendingWindow != window || s.pendingChoice != choice || !pressDue(s)) return false;
        s.pendingChoice.clear();
        if (s.pendingValues.empty()) s.pendingWindow = 0;
        return true;
    }

    // The agent's value of this field of the current modal, for the widget about to draw it; registers
    // the field. A widget with the caret keeps its own copy of the text, so it lets go first.
    const EditorDialogValue* takeValue(Entry& entry, const char* name, const char* type, const EditorDialogValue& value,
                                       std::vector<std::string> options, EditorDialogValue& taken) {
        if (!findField(entry, name)) entry.fields.push_back({name, type, value, std::move(options)});
        State& s = state();
        if (s.pendingWindow != entry.window) return nullptr;
        auto found = s.pendingValues.find(name);
        if (found == s.pendingValues.end()) return nullptr;
        taken = std::move(found->second);
        s.pendingValues.erase(found);
        if (s.pendingValues.empty() && s.pendingChoice.empty()) s.pendingWindow = 0;
        if (ImGui::GetActiveID() != 0) ImGui::ClearActiveID();
        return &taken;
    }

    void setField(Entry& entry, const char* name, const EditorDialogValue& value) {
        for (EditorDialogField& f : entry.fields) {
            if (f.name == name) f.value = value;
        }
    }

    bool textField(const char* name, const char* type, std::string& value) {
        Entry* entry = current();
        if (!entry) return false;
        EditorDialogValue taken;
        const bool wrote = takeValue(*entry, name, type, EditorDialogValue::ofText(value), {}, taken) != nullptr;
        if (wrote) {
            value = taken.text;
            setField(*entry, name, taken);
        }
        return wrote;
    }

    bool bufferField(const char* name, const char* type, char* buffer, size_t size) {
        std::string value = buffer;
        if (!textField(name, type, value)) return false;
        std::snprintf(buffer, size, "%s", value.c_str());
        return true;
    }

    std::string editorModalLowered(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    const char* kindName(EditorDialogValue::Kind kind) {
        switch (kind) {
            case EditorDialogValue::Kind::Text: return "a string";
            case EditorDialogValue::Kind::Bool: return "true or false";
            case EditorDialogValue::Kind::Number: return "a number";
            case EditorDialogValue::Kind::None: break;
        }
        return "nothing";
    }

    // A value this field can take; empty when so, or what is wrong with it
    std::string valueProblem(const EditorDialogField& field, const EditorDialogValue& value) {
        EditorDialogValue::Kind wanted = EditorDialogValue::Kind::Text;
        if (field.type == "bool") wanted = EditorDialogValue::Kind::Bool;
        if (field.type == "int" || field.type == "float") wanted = EditorDialogValue::Kind::Number;
        if (value.kind != wanted) {
            return "Field " + field.name + " takes " + kindName(wanted) + " (" + field.type + "), not " + kindName(value.kind) + ".";
        }
        if (field.type == "int" && value.number != std::floor(value.number)) {
            return "Field " + field.name + " takes a whole number.";
        }
        if (field.type == "enum" && std::find(field.options.begin(), field.options.end(), value.text) == field.options.end()) {
            return "Field " + field.name + " takes one of: " + joined(field.options) + ".";
        }
        return {};
    }

}

bool EditorModal::begin(const char* name, bool* p_open, ImGuiWindowFlags flags, Role role) {
    State& s = state();
    // An agent's cancel on a modal closed by its title bar: ImGui's own close path (Begin sees the
    // close button pressed, BeginPopupModal closes the popup and answers false)
    if (p_open && *p_open && s.hasTop && s.pendingChoice == "cancel" && pressDue(s)) {
        const Choice* cancel = findChoice(s.top, "cancel");
        ImGuiWindow* window = ImGui::FindWindowByName(name);
        if (cancel && cancel->closeButton && window && window->ID == s.top.window && takePending(window->ID, "cancel")) {
            *p_open = false;
        }
    }
    if (!ImGui::BeginPopupModal(name, p_open, flags)) {
        return false;
    }
    Entry entry;
    entry.window = ImGui::GetCurrentWindow()->ID;
    if (s.pendingWindow == entry.window && !s.pendingDrawn) {
        s.pendingDrawn = true;
        // no values to wait for: this frame's button
        if (s.pressFrame == 0) s.pressFrame = ImGui::GetFrameCount();
    }
    entry.title = titleOf(name);
    entry.hasCloseButton = p_open != nullptr;
    entry.role = role;
    s.drawn.push_back(std::move(entry));
    return true;
}

void EditorModal::message(const std::string& text) {
    if (Entry* entry = current()) entry->message = text;
}

void EditorModal::error(const std::string& text) {
    if (Entry* entry = current()) entry->error = text;
}

bool EditorModal::button(const char* label, const char* choice, const ImVec2& size) {
    bool pressed = ImGui::Button(label, size);
    Entry* entry = current();
    if (!entry) {
        return pressed;
    }
    const bool disabled = (ImGui::GetCurrentContext()->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0;
    if (!findChoice(*entry, choice)) {
        entry->choices.push_back({choice, titleOf(label), false, disabled});
    }
    // a disabled button is no answer (nothing a click could do)
    if (!disabled && !pressed && takePending(entry->window, choice)) {
        pressed = true;
    }
    return pressed;
}

std::string EditorModal::choiceOf(const std::string& label) {
    std::string out;
    for (const char c : editorModalLowered(titleOf(label.c_str()))) {
        if (std::isalnum(static_cast<unsigned char>(c))) out += c;
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

bool EditorModal::field(const char* name, std::string& value) {
    return textField(name, "text", value);
}

bool EditorModal::field(const char* name, char* buffer, size_t size) {
    return bufferField(name, "text", buffer, size);
}

bool EditorModal::field(const char* name, bool& value) {
    Entry* entry = current();
    if (!entry) return false;
    EditorDialogValue taken;
    if (!takeValue(*entry, name, "bool", EditorDialogValue::ofBool(value), {}, taken)) return false;
    value = taken.flag;
    setField(*entry, name, taken);
    return true;
}

bool EditorModal::field(const char* name, int& value) {
    Entry* entry = current();
    if (!entry) return false;
    EditorDialogValue taken;
    if (!takeValue(*entry, name, "int", EditorDialogValue::ofNumber(value), {}, taken)) return false;
    value = static_cast<int>(taken.number);
    setField(*entry, name, taken);
    return true;
}

bool EditorModal::field(const char* name, float& value) {
    Entry* entry = current();
    if (!entry) return false;
    EditorDialogValue taken;
    if (!takeValue(*entry, name, "float", EditorDialogValue::ofNumber(value), {}, taken)) return false;
    value = static_cast<float>(taken.number);
    setField(*entry, name, taken);
    return true;
}

bool EditorModal::pathField(const char* name, std::string& value) {
    return textField(name, "path", value);
}

bool EditorModal::pathField(const char* name, char* buffer, size_t size) {
    return bufferField(name, "path", buffer, size);
}

bool EditorModal::enumField(const char* name, int& index, const std::vector<std::string>& options) {
    Entry* entry = current();
    if (!entry) return false;
    const std::string shown = index >= 0 && index < static_cast<int>(options.size()) ? options[index] : std::string();
    EditorDialogValue taken;
    if (!takeValue(*entry, name, "enum", EditorDialogValue::ofText(shown), options, taken)) return false;
    index = static_cast<int>(std::find(options.begin(), options.end(), taken.text) - options.begin());
    setField(*entry, name, taken);
    return true;
}

bool EditorModal::enumField(const char* name, std::string& value, const std::vector<std::string>& options) {
    Entry* entry = current();
    if (!entry) return false;
    EditorDialogValue taken;
    if (!takeValue(*entry, name, "enum", EditorDialogValue::ofText(value), options, taken)) return false;
    value = taken.text;
    setField(*entry, name, taken);
    return true;
}

bool EditorModal::inputText(const char* label, const char* name, char* buffer, size_t size, ImGuiInputTextFlags flags,
                            ImGuiInputTextCallback callback, void* userData) {
    const bool wrote = field(name, buffer, size);
    const bool edited = ImGui::InputText(label, buffer, size, flags, callback, userData);
    return edited || (wrote && !(flags & ImGuiInputTextFlags_EnterReturnsTrue));
}

bool EditorModal::inputTextWithHint(const char* label, const char* name, const char* hint, char* buffer, size_t size, ImGuiInputTextFlags flags) {
    const bool wrote = field(name, buffer, size);
    const bool edited = ImGui::InputTextWithHint(label, hint, buffer, size, flags);
    return edited || (wrote && !(flags & ImGuiInputTextFlags_EnterReturnsTrue));
}

bool EditorModal::checkbox(const char* label, const char* name, bool* value) {
    const bool wrote = field(name, *value);
    return ImGui::Checkbox(label, value) || wrote;
}

bool EditorModal::combo(const char* label, const char* name, int* index, const std::vector<std::string>& options) {
    const bool wrote = enumField(name, *index, options);
    const bool picked = ImGui::Combo(label, index, [](void* data, int i) {
        return (*static_cast<const std::vector<std::string>*>(data))[i].c_str();
    }, const_cast<std::vector<std::string>*>(&options), static_cast<int>(options.size()));
    return picked || wrote;
}

bool EditorModal::inputInt(const char* label, const char* name, int* value, int step, int stepFast) {
    const bool wrote = field(name, *value);
    return ImGui::InputInt(label, value, step, stepFast) || wrote;
}

bool EditorModal::inputFloat(const char* label, const char* name, float* value, float step, float stepFast, const char* format) {
    const bool wrote = field(name, *value);
    return ImGui::InputFloat(label, value, step, stepFast, format) || wrote;
}

bool EditorModal::dragInt(const char* label, const char* name, int* value, float speed, int min, int max) {
    const bool wrote = field(name, *value);
    return ImGui::DragInt(label, value, speed, min, max) || wrote;
}

void EditorModal::endFrame() {
    State& s = state();
    s.hasTop = false;
    s.top = Entry();
    s.openTitles.clear();
    for (const Entry& entry : s.drawn) s.openTitles.push_back(entry.title);

    // top-most first; a progress modal has nothing to answer, so the one under it is reported
    ImGuiContext& g = *ImGui::GetCurrentContext();
    bool unsettled = false;
    for (int n = g.OpenPopupStack.Size - 1; n >= 0; n--) {
        ImGuiWindow* window = g.OpenPopupStack[n].Window;
        if (!window) {
            unsettled = true;
            continue;
        }
        // (not Hidden: an auto-sized window is Hidden in its first frame, while it already draws its body)
        if (s.hasTop || !(window->Flags & ImGuiWindowFlags_Modal) || !window->Active) continue;
        auto found = std::find_if(s.drawn.begin(), s.drawn.end(), [window](const Entry& e) { return e.window == window->ID; });
        if (found != s.drawn.end() && found->role == Role::Progress) continue;
        if (found != s.drawn.end()) {
            s.top = *found;
        } else {
            // drawn without EditorModal: still reported (a human must close it), with no choices
            s.top.window = window->ID;
            s.top.title = titleOf(window->Name);
        }
        if (s.top.hasCloseButton && !findChoice(s.top, "cancel")) {
            s.top.choices.push_back({"cancel", "close the window", true});
        }
        s.hasTop = true;
    }
    s.unsettledFrames = unsettled ? s.unsettledFrames + 1 : 0;
    s.drawn.clear();

    if (s.pendingWindow == 0) return;
    if (!s.hasTop || s.top.window != s.pendingWindow) {
        // the modal went away: nothing left to apply
        s.pendingChoice.clear();
        s.pendingValues.clear();
        s.pendingWindow = 0;
        return;
    }
    if (!s.pendingDrawn) return;
    const int frame = ImGui::GetFrameCount();
    // the frame that took the values has run its checks on them: the button is the next frame's
    if (s.pressFrame == NO_PRESS_FRAME) {
        s.pressFrame = frame + 1;
        // a value that found no field in the body just drawn
        if (!s.pendingValues.empty()) {
            std::vector<std::string> names;
            for (const auto& [name, value] : s.pendingValues) names.push_back(name);
            s.answerError = "\"" + s.top.title + "\" no longer shows the field(s) " + joined(names) + ": not set.";
            s.pendingValues.clear();
        }
    }
    // an answer is pressed by its frame or the next one; a modal gone by then, or a button greyed out
    // by then, drops it
    if (!s.pendingChoice.empty() && frame > s.pressFrame) {
        const Choice* choice = findChoice(s.top, s.pendingChoice);
        const std::string label = choice ? choice->label : s.pendingChoice;
        if (s.answerError.empty()) {
            s.answerError = "\"" + s.top.title + "\" did not take " + s.pendingChoice;
            if (choice && choice->disabled) s.answerError += ": " + label + " is greyed out";
            s.answerError += s.top.error.empty() ? "." : ": " + s.top.error;
        }
        s.pendingChoice.clear();
    }
    if (s.pendingValues.empty() && s.pendingChoice.empty()) s.pendingWindow = 0;
}

bool EditorModal::wantsFrame() {
    const State& s = state();
    return s.pendingWindow != 0 || settling();
}

bool EditorModal::settling() {
    // a popup nothing begins stays on ImGui's stack: two frames is all any of ours takes
    const int frames = state().unsettledFrames;
    return frames > 0 && frames <= 2;
}

bool EditorModal::topModal(EditorDialogInfo& out) {
    const State& s = state();
    if (!s.hasTop) return false;
    out = EditorDialogInfo();
    out.kind = "modal";
    out.title = s.top.title;
    out.message = s.top.message;
    out.error = s.top.error;
    out.fields = s.top.fields;
    out.blocksEdits = s.top.role != Role::MergesEdits;
    bool renamed = false;
    for (const Choice& c : s.top.choices) {
        if (c.disabled) {
            out.disabledChoices.push_back(c.choice);
        } else {
            out.choices.push_back(c.choice);
        }
        if (editorModalLowered(c.label) != c.choice) renamed = true;
    }
    // what each choice presses, when a label is not the choice's own word (as the three-button alerts say)
    if (renamed) {
        std::string legend;
        for (const Choice& c : s.top.choices) legend += (legend.empty() ? "" : ", ") + c.choice + " = " + c.label;
        out.message += (out.message.empty() ? "[" : " [") + legend + "]";
    }
    return true;
}

bool EditorModal::answer(const std::string& choice, const std::map<std::string, EditorDialogValue>& values, std::string& error) {
    State& s = state();
    if (!s.hasTop) {
        error = "No dialog is open.";
        return false;
    }
    std::vector<std::string> names;
    for (const EditorDialogField& f : s.top.fields) names.push_back(f.name);
    for (const auto& [name, value] : values) {
        const EditorDialogField* known = findField(s.top, name);
        if (!known) {
            std::vector<std::string> choices;
            for (const Choice& c : s.top.choices) choices.push_back(c.choice);
            error = "\"" + s.top.title + "\" has no field " + name + (names.empty() ? ". Answer it with choice: " + joined(choices) + "." : ". Its fields: " + joined(names) + ".");
            return false;
        }
        error = valueProblem(*known, value);
        if (!error.empty()) return false;
    }
    if (!choice.empty()) {
        const Choice* found = findChoice(s.top, choice);
        if (!found) {
            std::vector<std::string> valid;
            for (const Choice& c : s.top.choices) valid.push_back(c.choice);
            error = "\"" + s.top.title + "\" has no " + choice + " answer" + (valid.empty() ? "." : ". Choose one of: " + joined(valid) + ".");
            return false;
        }
        // new values may enable it; without any it would stay greyed out
        if (found->disabled && values.empty()) {
            error = "\"" + s.top.title + "\": " + found->label + " is greyed out" + (s.top.error.empty() ? "." : ": " + s.top.error);
            return false;
        }
    }
    s.pendingWindow = s.top.window;
    s.pendingChoice = choice;
    s.pendingValues = values;
    s.pendingDrawn = false;
    s.pressFrame = values.empty() ? 0 : NO_PRESS_FRAME;
    s.answerError.clear();
    return true;
}

bool EditorModal::answerSettled() {
    return state().pendingWindow == 0;
}

std::string EditorModal::takeAnswerError() {
    std::string out;
    out.swap(state().answerError);
    return out;
}

bool EditorModal::isOpen(const std::string& title) {
    const std::vector<std::string>& titles = state().openTitles;
    return std::find(titles.begin(), titles.end(), title) != titles.end();
}

void EditorModal::reset() {
    state() = State();
}
