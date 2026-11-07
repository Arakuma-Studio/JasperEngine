// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PanelClaims.h"

#include <array>
#include <optional>
#include <utility>

using namespace jasperengine::editor;

namespace {

    struct EditCommand {
        const char* id;
        EditVerb verb;
    };

    constexpr std::array<EditCommand, 9> kEditCommands = {{
        {"edit.delete", EditVerb::Delete},
        {"edit.duplicate", EditVerb::Duplicate},
        {"edit.frameSelected", EditVerb::Frame},
        {"edit.copy", EditVerb::Copy},
        {"edit.cut", EditVerb::Cut},
        {"edit.paste", EditVerb::Paste},
        {"edit.pasteAsChild", EditVerb::PasteAsChild},
        {"edit.selectAll", EditVerb::SelectAll},
        {"edit.rename", EditVerb::Rename},
    }};

    std::optional<EditVerb> verbOf(std::string_view commandId) {
        for (const EditCommand& command : kEditCommands) {
            if (commandId == command.id) return command.verb;
        }
        return std::nullopt;
    }

}

PanelClaims& PanelClaims::editor() {
    static PanelClaims claims(CommandCatalog::editor());
    return claims;
}

PanelClaims::PanelClaims(CommandCatalog& catalog) : catalog(catalog) {
}

void PanelClaims::add(PanelClaim claim) {
    for (PanelClaim& existing : claims) {
        if (existing.panel == claim.panel) {
            existing = std::move(claim);
            return;
        }
    }
    claims.push_back(std::move(claim));
}

void PanelClaims::setTyping(std::function<bool()> value) {
    typing = std::move(value);
}

void PanelClaims::bindEditCommands() {
    for (const EditCommand& command : kEditCommands) {
        const CommandDef* existing = catalog.find(command.id);
        if (!existing) continue;
        CommandDef def = *existing;
        const EditVerb verb = command.verb;
        def.enabled = [this, verb, original = def.enabled]() {
            if (current >= 0) return !(typing && typing()) && canRun(handlerFor(verb));
            return !original || original();
        };
        // a panel with its own command for the verb has its own keys: this command's key is quiet
        def.keyGate = [this, verb, original = def.keyGate]() {
            if (current < 0) return !original || original();
            const ClaimHandler* handler = handlerFor(verb);
            return handler && handler->command.empty();
        };
        def.run = [this, verb, original = def.run](const CommandArgs& args) {
            if (current >= 0) runVerb(verb, args.source);
            else if (original) original(args);
        };
        catalog.add(std::move(def));
    }
}

void PanelClaims::refresh() {
    current = -1;
    for (size_t i = 0; i < claims.size(); i++) {
        if (claims[i].focused && claims[i].focused()) {
            current = static_cast<int>(i);
            return;
        }
    }
}

const PanelClaim* PanelClaims::active() const {
    return current >= 0 ? &claims[static_cast<size_t>(current)] : nullptr;
}

const ClaimHandler* PanelClaims::handlerFor(EditVerb verb) const {
    const PanelClaim* claim = active();
    if (!claim) return nullptr;
    const auto it = claim->handlers.find(verb);
    return it == claim->handlers.end() ? nullptr : &it->second;
}

bool PanelClaims::canRun(const ClaimHandler* handler) const {
    if (!handler) return false;
    if (handler->command.empty()) return !handler->enabled || handler->enabled();
    const CommandDef* def = catalog.find(handler->command);
    return def && def->run && (!def->enabled || def->enabled());
}

void PanelClaims::runVerb(EditVerb verb, CommandSource source) {
    const ClaimHandler* handler = handlerFor(verb);
    if (!canRun(handler)) return;
    if (handler->command.empty()) {
        if (handler->run) handler->run();
        return;
    }
    const CommandDef* def = catalog.find(handler->command);
    const auto run = def->run;
    run(CommandArgs{source, def->menuPayload});
}

bool PanelClaims::route(std::string_view commandId, CommandSource source) {
    if (current < 0) return false;
    const std::optional<EditVerb> verb = verbOf(commandId);
    if (!verb) return false;
    runVerb(*verb, source);
    return true;
}

std::string PanelClaims::shownCommand(std::string_view commandId) const {
    const std::optional<EditVerb> verb = verbOf(commandId);
    const ClaimHandler* handler = verb ? handlerFor(*verb) : nullptr;
    return handler && !handler->command.empty() ? handler->command : std::string(commandId);
}

std::vector<Chord> PanelClaims::nativeMenuChords() const {
    std::vector<Chord> chords;
    for (const EditCommand& command : kEditCommands) {
        const ClaimHandler* handler = handlerFor(command.verb);
        if (!handler || handler->command.empty() || (typing && typing()) || !canRun(handler)) continue;
        const std::span<const Chord> bound = catalog.bindings(handler->command);
        if (!bound.empty()) chords.push_back(bound.front());
    }
    return chords;
}
