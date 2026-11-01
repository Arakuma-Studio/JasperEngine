// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FieldEdit.h"

#include "command/CommandHistory.h"

using namespace jasperengine::editor;

namespace {

    struct Session {
        InteractiveEdit edit;
        const void* owner = nullptr;
    };

    Session& session() {
        static Session s;
        return s;
    }

    ui::BeginEditHandler& beginHandler() {
        static ui::BeginEditHandler handler = nullptr;
        return handler;
    }

}

void ui::setBeginEditHandler(BeginEditHandler handler) {
    beginHandler() = handler;
}

void ui::beginFieldEdit(CommandHistory* history, const char* label) {
    if (beginHandler()) beginHandler()(history, label);
}

void ui::FieldEditSession::begin(CommandHistory* history, std::string name, const void* owner) {
    Session& s = session();
    s.edit.commit();
    s.owner = nullptr;
    if (!history) return;
    s.edit = history->beginInteractive(std::move(name));
    s.owner = owner;
}

void ui::FieldEditSession::commit(const void* owner) {
    Session& s = session();
    if (s.owner != owner) return;
    s.edit.commit();
    s.owner = nullptr;
}

void ui::FieldEditSession::cancel(const void* owner) {
    Session& s = session();
    if (s.owner != owner) return;
    s.edit.cancel();
    s.owner = nullptr;
}

bool ui::FieldEditSession::isOpen() {
    Session& s = session();
    return s.owner && s.edit.isOpen();
}

bool ui::FieldEditSession::isOpenFor(const void* owner) {
    Session& s = session();
    return s.owner == owner && s.edit.isOpen();
}
