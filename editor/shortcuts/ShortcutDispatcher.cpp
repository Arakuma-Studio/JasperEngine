// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShortcutDispatcher.h"

#include <algorithm>

using namespace jasperengine::editor;

bool ImGuiShortcutKeys::pressed(ImGuiKey key, bool repeat) const {
    return ImGui::IsKeyPressed(key, repeat);
}

bool ImGuiShortcutKeys::down(ImGuiKey key) const {
    return ImGui::IsKeyDown(key);
}

ImGuiKeyChord ImGuiShortcutKeys::mods() const {
    return ImGui::GetIO().KeyMods;
}

ShortcutDispatcher::ShortcutDispatcher(CommandCatalog& catalog) : catalog(catalog) {
}

void ShortcutDispatcher::setCaptureActive(bool active) {
    capturing = active;
}

void ShortcutDispatcher::noteCaptureConsumedKey(ImGuiKey key) {
    consumedKey = key;
}

bool ShortcutDispatcher::contextActive(ShortcutContext context, const ShortcutState& state) const {
    switch (context) {
        case ShortcutContext::Global:
            return true;
        case ShortcutContext::Editor:
            return !state.codeEditorFocused;
        default:
            return std::find(state.panels.begin(), state.panels.end(), context) != state.panels.end();
    }
}

bool ShortcutDispatcher::mayFire(const CommandDef& def, Chord chord, const ShortcutState& state, bool ignoreEnabled) const {
    if (def.readOnly || !def.run) return false;
    bool active = false;
    for (ShortcutContext context : commandContexts(def)) {
        active = active || contextActive(context, state);
    }
    if (!active) return false;
    if (chord.isTypingChord() && (state.wantTextInput || state.codeEditorFocused || state.gameHasKeyboard)) return false;
    if (def.context == ShortcutContext::Editor && state.wantTextInput) return false;
    if (state.nativeMenuOwnsKeys && (catalog.menuChord(def.id) == chord ||
        std::find(state.nativeMenuChords.begin(), state.nativeMenuChords.end(), chord) != state.nativeMenuChords.end())) {
        return false;
    }
    if (def.keyGate && !def.keyGate()) return false;
    if (!ignoreEnabled && def.enabled && !def.enabled()) return false;
    return true;
}

const CommandDef* ShortcutDispatcher::resolve(Chord chord, const ShortcutState& state) const {
    return resolveCommand(chord, state, false);
}

const CommandDef* ShortcutDispatcher::resolveIgnoringEnabled(Chord chord, const ShortcutState& state) const {
    return resolveCommand(chord, state, true);
}

const CommandDef* ShortcutDispatcher::resolveCommand(Chord chord, const ShortcutState& state, bool ignoreEnabled) const {
    if (chord.empty() || capturing || state.popupOpen) return nullptr;
    // the armed brush first, then panel contexts, then Editor, then Global; catalog order within each
    const CommandDef* best = nullptr;
    int bestRank = 3;
    for (const CommandDef& def : catalog.commands()) {
        const std::span<const Chord> chords = catalog.bindings(def.id);
        if (std::find(chords.begin(), chords.end(), chord) == chords.end()) continue;
        int rank = 3;
        for (ShortcutContext context : commandContexts(def)) {
            if (!contextActive(context, state)) continue;
            // an armed brush outranks the panels (T1 R57, T6 R60): its keys shadow theirs only while armed
            const int contextRank = context == ShortcutContext::Global ? 2 : (context == ShortcutContext::Editor ? 1 : (context == ShortcutContext::VoxelBrush || context == ShortcutContext::TerrainBrush ? -1 : 0));
            rank = std::min(rank, contextRank);
        }
        if (rank >= bestRank) continue;
        if (!mayFire(def, chord, state, ignoreEnabled)) continue;
        best = &def;
        bestRank = rank;
    }
    return best;
}

std::vector<std::string> ShortcutDispatcher::process(const ShortcutKeys& keys, const ShortcutState& state) {
    std::vector<std::string> fired;
    if (consumedKey != ImGuiKey_None) {
        // the captured key is still held (or repeating): it belongs to the capture
        if (keys.down(consumedKey)) return fired;
        consumedKey = ImGuiKey_None;
    }
    if (capturing || state.popupOpen) return fired;

    const ImGuiKeyChord mods = keys.mods();
    // each chord bound anywhere, once: the first command of the best context runs
    std::vector<Chord> seen;
    std::vector<std::pair<std::string, CommandArgs>> toRun;
    std::vector<std::pair<std::string, std::string>> disabledReasons;
    for (const CommandDef& def : catalog.commands()) {
        for (Chord chord : catalog.bindings(def.id)) {
            if (chord.mods() != mods) continue;
            if (std::find(seen.begin(), seen.end(), chord) != seen.end()) continue;
            seen.push_back(chord);
            // the cheap test first: most frames press nothing
            if (!keys.pressed(chord.key(), true)) continue;
            const CommandDef* target = resolve(chord, state);
            if (!target) {
                // pressed for a command that cannot run now: say why (once per press)
                if (disabledHandler && keys.pressed(chord.key(), false)) {
                    const CommandDef* disabled = resolveIgnoringEnabled(chord, state);
                    if (disabled && disabled->disabledReason) {
                        const std::string reason = disabled->disabledReason();
                        if (!reason.empty()) disabledReasons.push_back({disabled->id, reason});
                    }
                }
                continue;
            }
            if (!keys.pressed(chord.key(), target->repeat)) continue;
            toRun.push_back({target->id, CommandArgs{CommandSource::Keyboard, target->menuPayload}});
        }
    }
    // run after the walk: a command may change the catalog (a layout, a rebinding)
    for (const auto& [id, args] : toRun) {
        if (const CommandDef* def = catalog.find(id)) {
            const auto run = def->run;
            fired.push_back(id);
            run(args);
        }
    }
    for (const auto& [id, reason] : disabledReasons) {
        if (const CommandDef* def = catalog.find(id)) {
            disabledHandler(*def, reason);
        }
    }
    return fired;
}
