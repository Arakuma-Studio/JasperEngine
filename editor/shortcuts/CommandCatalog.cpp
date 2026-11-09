// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CommandCatalog.h"

#include <algorithm>
#include <sstream>

using namespace jasperengine::editor;

namespace {

    const std::vector<Chord> noChords;

    bool contains(const std::vector<Chord>& chords, Chord chord) {
        return std::find(chords.begin(), chords.end(), chord) != chords.end();
    }

    // "|" would end a Markdown table cell
    std::string tableCell(const std::string& text) {
        std::string out;
        for (char c : text) {
            if (c == '|') out += "\\|";
            else out.push_back(c);
        }
        return out;
    }

}

const char* jasperengine::editor::shortcutContextName(ShortcutContext context) {
    switch (context) {
        case ShortcutContext::Global: return "Global";
        case ShortcutContext::Editor: return "Editor";
        case ShortcutContext::SceneView: return "Scene View";
        case ShortcutContext::Hierarchy: return "Hierarchy";
        case ShortcutContext::Project: return "Project";
        case ShortcutContext::Console: return "Console";
        case ShortcutContext::Animation: return "Animation";
        case ShortcutContext::PoseAnimation: return "Pose Animation";
        case ShortcutContext::Curve: return "Curve";
        case ShortcutContext::ColorPicker: return "Color Picker";
        case ShortcutContext::SoundBrowser: return "Sound Browser";
        case ShortcutContext::AudioCaptures: return "Audio Captures";
        case ShortcutContext::SpritePreview: return "Sprite Preview";
        case ShortcutContext::Palette: return "Palette";
        case ShortcutContext::PixelArtCheck: return "Pixel Art Check";
        case ShortcutContext::TilePalette: return "Tile Palette";
    }
    return "Editor";
}

const char* jasperengine::editor::shortcutContextKey(ShortcutContext context) {
    switch (context) {
        case ShortcutContext::Global: return "global";
        case ShortcutContext::Editor: return "editor";
        case ShortcutContext::SceneView: return "scene_view";
        case ShortcutContext::Hierarchy: return "hierarchy";
        case ShortcutContext::Project: return "project";
        case ShortcutContext::Console: return "console";
        case ShortcutContext::Animation: return "animation";
        case ShortcutContext::PoseAnimation: return "pose_animation";
        case ShortcutContext::Curve: return "curve";
        case ShortcutContext::ColorPicker: return "color_picker";
        case ShortcutContext::SoundBrowser: return "sound_browser";
        case ShortcutContext::AudioCaptures: return "audio_captures";
        case ShortcutContext::SpritePreview: return "sprite_preview";
        case ShortcutContext::Palette: return "palette";
        case ShortcutContext::PixelArtCheck: return "pixel_art_check";
        case ShortcutContext::TilePalette: return "tile_palette";
    }
    return "editor";
}

std::optional<ShortcutContext> jasperengine::editor::parseShortcutContext(std::string_view text) {
    for (uint8_t value = 0; value <= static_cast<uint8_t>(ShortcutContext::TilePalette); value++) {
        const auto context = static_cast<ShortcutContext>(value);
        if (text == shortcutContextName(context) || text == shortcutContextKey(context)) return context;
    }
    return std::nullopt;
}

bool jasperengine::editor::isBroadContext(ShortcutContext context) {
    return context == ShortcutContext::Global || context == ShortcutContext::Editor;
}

std::vector<ShortcutContext> jasperengine::editor::commandContexts(const CommandDef& def) {
    std::vector<ShortcutContext> contexts{def.context};
    for (ShortcutContext extra : def.alsoIn) {
        if (std::find(contexts.begin(), contexts.end(), extra) == contexts.end()) contexts.push_back(extra);
    }
    return contexts;
}

CommandCatalog& CommandCatalog::editor() {
    static CommandCatalog catalog;
    return catalog;
}

CommandCatalog::CommandCatalog() = default;

void CommandCatalog::setMac(bool value) {
    if (mac == value) return;
    mac = value;
    for (const CommandDef& def : defs) refreshEffective(def);
    revisionCounter++;
}

void CommandCatalog::rebuildIndex() {
    index.clear();
    for (size_t i = 0; i < defs.size(); i++) index[defs[i].id] = i;
}

size_t CommandCatalog::orderOf(std::string_view id) const {
    auto it = index.find(id);
    return it == index.end() ? defs.size() : it->second;
}

void CommandCatalog::add(CommandDef def) {
    auto it = index.find(def.id);
    const std::string id = def.id;
    if (it != index.end()) {
        defs[it->second] = std::move(def);
    } else {
        defs.push_back(std::move(def));
        index[id] = defs.size() - 1;
    }
    refreshEffective(defs[index[id]]);
    revisionCounter++;
}

void CommandCatalog::removeDynamic(std::string_view prefix) {
    const size_t before = defs.size();
    for (const CommandDef& def : defs) {
        if (def.dynamic && std::string_view(def.id).starts_with(prefix)) {
            effective.erase(def.id);
            menuItems.erase(def.id);
            if (auto s = stored.find(def.id); s != stored.end()) s->second.applied = false;
        }
    }
    defs.erase(std::remove_if(defs.begin(), defs.end(), [&](const CommandDef& def) {
        return def.dynamic && std::string_view(def.id).starts_with(prefix);
    }), defs.end());
    if (defs.size() != before) {
        rebuildIndex();
        revisionCounter++;
    }
}

const CommandDef* CommandCatalog::find(std::string_view id) const {
    auto it = index.find(id);
    return it == index.end() ? nullptr : &defs[it->second];
}

const CommandDef* CommandCatalog::findByMenuCommand(uint32_t menuCommand, std::string_view payload) const {
    const CommandDef* anyPayload = nullptr;
    for (const CommandDef& def : defs) {
        if (!def.menuCommand || *def.menuCommand != menuCommand) continue;
        if (def.menuPayload == payload) return &def;
        if (def.menuPayload.empty() && !anyPayload) anyPayload = &def;
    }
    return anyPayload;
}

std::vector<Chord> CommandCatalog::defaults(std::string_view id) const {
    const CommandDef* def = find(id);
    std::vector<Chord> chords;
    if (!def || def->readOnly) return chords;
    const Chord first = (mac && !def->macPrimary.empty()) ? def->macPrimary : def->primary;
    auto push = [&chords](Chord chord) {
        if (!chord.empty() && !contains(chords, chord)) chords.push_back(chord);
    };
    push(first);
    // macOS: the primary moves behind macPrimary rather than going away (Redo keeps Cmd+Y)
    if (mac && !def->macPrimary.empty()) push(def->primary);
    push(def->secondary);
    return chords;
}

void CommandCatalog::refreshEffective(const CommandDef& def) {
    std::vector<Chord> chords = defaults(def.id);
    if (!def.readOnly) {
        if (auto it = stored.find(def.id); it != stored.end()) {
            std::vector<Chord> parsed;
            bool valid = true;
            for (const std::string& text : it->second.chords) {
                std::optional<Chord> chord = Chord::parse(text);
                if (!chord) {
                    valid = false;
                    break;
                }
                if (!contains(parsed, *chord)) parsed.push_back(*chord);
            }
            it->second.applied = valid;
            if (valid) chords = std::move(parsed);
        }
    }
    effective[def.id] = std::move(chords);
}

std::span<const Chord> CommandCatalog::bindings(std::string_view id) const {
    auto it = effective.find(id);
    if (it == effective.end()) return noChords;
    return it->second;
}

bool CommandCatalog::isOverridden(std::string_view id) const {
    auto it = stored.find(std::string(id));
    return it != stored.end() && it->second.applied;
}

const CommandDef* CommandCatalog::findConflict(std::string_view id, Chord chord, bool* shadowed) const {
    if (shadowed) *shadowed = false;
    if (chord.empty()) return nullptr;
    const CommandDef* self = find(id);
    if (!self) return nullptr;
    const std::vector<ShortcutContext> mine = commandContexts(*self);
    const CommandDef* shadow = nullptr;
    for (const CommandDef& other : defs) {
        if (other.id == self->id || other.readOnly) continue;
        const std::span<const Chord> theirs = bindings(other.id);
        if (std::find(theirs.begin(), theirs.end(), chord) == theirs.end()) continue;
        const std::vector<ShortcutContext> contexts = commandContexts(other);
        bool same = false;
        bool broadOverPanel = false;
        for (ShortcutContext a : mine) {
            for (ShortcutContext b : contexts) {
                // Global and Editor reach the same windows: one context for collisions
                if (a == b || (isBroadContext(a) && isBroadContext(b))) same = true;
                else if (isBroadContext(a) != isBroadContext(b)) broadOverPanel = true;
            }
        }
        if (same) return &other;
        if (broadOverPanel && !shadow) shadow = &other;
    }
    if (shadow && shadowed) *shadowed = true;
    return shadow;
}

bool CommandCatalog::setBindings(std::string_view id, std::vector<Chord> chords, std::string* reason) {
    const CommandDef* def = find(id);
    if (!def) {
        if (reason) *reason = "no command " + std::string(id);
        return false;
    }
    if (def->readOnly) {
        if (reason) *reason = def->group + " / " + def->name + " is a reference row and cannot be rebound";
        return false;
    }
    std::vector<Chord> unique;
    for (Chord chord : chords) {
        if (chord.empty() || !Chord::isBindableKey(chord.key())) {
            if (reason) *reason = "not a bindable chord";
            return false;
        }
        if (!contains(unique, chord)) unique.push_back(chord);
    }
    if (unique == defaults(id)) {
        stored.erase(std::string(id));
    } else {
        StoredOverride& entry = stored[std::string(id)];
        entry.chords.clear();
        for (Chord chord : unique) entry.chords.push_back(chord.format(false));
        entry.applied = true;
    }
    effective[std::string(id)] = std::move(unique);
    revisionCounter++;
    return true;
}

void CommandCatalog::resetBindings(std::string_view id) {
    const CommandDef* def = find(id);
    if (!def) return;
    stored.erase(std::string(id));
    refreshEffective(*def);
    revisionCounter++;
}

void CommandCatalog::resetAll() {
    for (const CommandDef& def : defs) stored.erase(def.id);
    for (const CommandDef& def : defs) refreshEffective(def);
    revisionCounter++;
}

std::vector<CommandCatalog::Rejected> CommandCatalog::loadOverrides(const std::map<std::string, std::vector<std::string>>& overrides) {
    stored.clear();
    for (const auto& [id, chords] : overrides) {
        stored[id].chords = chords;
    }
    for (const CommandDef& def : defs) refreshEffective(def);
    revisionCounter++;

    std::vector<Rejected> rejected;
    for (const auto& [id, entry] : stored) {
        const CommandDef* def = find(id);
        if (!def) {
            // a project's script menus register when the project opens: theirs wait for it
            if (!std::string_view(id).starts_with("script.")) rejected.push_back({id, "no such command"});
            continue;
        }
        if (def->readOnly) {
            rejected.push_back({id, "a reference row cannot be rebound"});
            continue;
        }
        for (const std::string& text : entry.chords) {
            if (!Chord::parse(text)) {
                rejected.push_back({id, "\"" + text + "\" is not a chord"});
                break;
            }
        }
    }
    return rejected;
}

void CommandCatalog::applyStoredOverride(const std::string& id) {
    if (const CommandDef* def = find(id)) {
        refreshEffective(*def);
        revisionCounter++;
    }
}

std::vector<CommandCatalog::ResolvedConflict> CommandCatalog::resolveConflicts() {
    std::vector<ResolvedConflict> resolved;
    for (size_t i = 0; i < defs.size(); i++) {
        const CommandDef& def = defs[i];
        if (def.readOnly) continue;
        std::vector<Chord>& mine = effective[def.id];
        for (size_t c = 0; c < mine.size();) {
            const Chord chord = mine[c];
            bool shadowed = false;
            const CommandDef* other = findConflict(def.id, chord, &shadowed);
            if (!other || shadowed) {
                c++;
                continue;
            }
            const bool mineIsDefault = contains(defaults(def.id), chord);
            const bool theirsIsDefault = contains(defaults(other->id), chord);
            const bool otherWins = (theirsIsDefault != mineIsDefault) ? theirsIsDefault : orderOf(other->id) < i;
            if (otherWins) {
                resolved.push_back({other->id, def.id, chord});
                mine.erase(mine.begin() + c);
            } else {
                resolved.push_back({def.id, other->id, chord});
                std::vector<Chord>& theirs = effective[other->id];
                theirs.erase(std::remove(theirs.begin(), theirs.end(), chord), theirs.end());
                c++;
            }
        }
    }
    if (!resolved.empty()) revisionCounter++;
    return resolved;
}

std::string CommandCatalog::menuShortcutText(std::string_view id) const {
    const std::span<const Chord> chords = bindings(id);
    return chords.empty() ? std::string() : chords.front().format(mac);
}

std::string CommandCatalog::bindingsText(std::string_view id) const {
    const CommandDef* def = find(id);
    if (!def) return {};
    if (def->readOnly) return def->gesture;
    std::string text;
    for (Chord chord : bindings(id)) {
        if (!text.empty()) text += " / ";
        text += chord.format(mac);
    }
    return text;
}

std::string CommandCatalog::tooltip(std::string_view id, std::string_view name) const {
    const std::string shortcut = menuShortcutText(id);
    std::string text(name);
    if (!shortcut.empty()) text += " (" + shortcut + ")";
    return text;
}

void CommandCatalog::setMenuItem(std::string_view id, std::string path, Chord shown) {
    menuItems[std::string(id)] = {std::move(path), shown};
}

void CommandCatalog::clearMenuItems() {
    menuItems.clear();
}

std::string CommandCatalog::menuPath(std::string_view id) const {
    auto it = menuItems.find(id);
    return it == menuItems.end() ? std::string() : it->second.path;
}

Chord CommandCatalog::menuChord(std::string_view id) const {
    auto it = menuItems.find(id);
    return it == menuItems.end() ? Chord() : it->second.shown;
}

std::string CommandCatalog::markdownReference() const {
    // Defaults, formatted for every platform at once: the file is the same on each
    std::ostringstream out;
    out << "# Keyboard shortcuts\n\n"
        << "<!-- Generated from the editor's command catalog (CommandCatalog::markdownReference, also\n"
        << "     Help > Keyboard Shortcuts). Do not edit by hand: the unit test\n"
        << "     CommandCatalog.MarkdownReferenceMatchesDocs fails when this file is out of date. -->\n\n"
        << "The editor's default key bindings, by group. Rebind any of them in **Edit > Shortcuts...**; your\n"
        << "changes are kept in `shortcuts.yaml` in the editor's settings folder and apply to every project.\n"
        << "**Edit > Search All...** (Ctrl+K) finds any command by name, with its shortcut.\n\n"
        << "- On macOS, Ctrl is the Command key (Cmd) and Alt is Option.\n"
        << "- **Context** says where a key works: *Global* everywhere, the Code Editor and text fields included;\n"
        << "  *Editor* in every window but the Code Editor, and not while typing in a field; a window's name\n"
        << "  while that window has focus (the Scene View also while the mouse is over it).\n"
        << "- Letters follow the keyboard layout: on AZERTY the Hand Tool is the key labelled Q.\n";

    std::vector<std::string> groups;
    for (const CommandDef& def : defs) {
        if (def.dynamic) continue;
        if (std::find(groups.begin(), groups.end(), def.group) == groups.end()) groups.push_back(def.group);
    }
    for (const std::string& group : groups) {
        std::vector<const CommandDef*> rows;
        for (const CommandDef& def : defs) {
            if (def.dynamic || def.group != group) continue;
            const bool hasDefaults = !def.primary.empty() || !def.secondary.empty() || !def.macPrimary.empty();
            if (def.readOnly || hasDefaults) rows.push_back(&def);
        }
        if (rows.empty()) continue;
        out << "\n## " << group << "\n\n| Command | Shortcut | Context |\n|---|---|---|\n";
        for (const CommandDef* def : rows) {
            std::string keys;
            if (def->readOnly) {
                keys = def->gesture;
            } else {
                for (Chord chord : {def->primary, def->secondary}) {
                    if (chord.empty()) continue;
                    if (!keys.empty()) keys += " / ";
                    keys += chord.format(false);
                }
                if (!def->macPrimary.empty() && def->macPrimary != def->primary) {
                    keys += " (macOS: " + def->macPrimary.format(true) + ")";
                }
            }
            std::string context = shortcutContextName(def->context);
            for (ShortcutContext extra : def->alsoIn) {
                if (extra != def->context) context += ", " + std::string(shortcutContextName(extra));
            }
            out << "| " << tableCell(def->name) << " | " << tableCell(keys) << " | " << context << " |\n";
        }
    }
    return out.str();
}
