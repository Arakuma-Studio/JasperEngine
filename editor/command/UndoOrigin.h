// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>

namespace jasperengine::editor{

    // Who made an undo step: the user, an MCP agent, the Play Mode Changes review or the
    // editor itself
    enum class UndoOriginKind{
        User,
        Agent,
        PlayReview,
        System
    };

    struct UndoOrigin{
        UndoOriginKind kind = UndoOriginKind::User;
        std::string client;     // Agent: the MCP client's name ("Claude Code")
        std::string callId;     // Agent: the tools/call request id
        std::string tool;       // Agent: the tool that ran
        uint64_t call = 0;      // Agent: the call's serial, unique per editor session (0: none)

        bool sameSource(const UndoOrigin& other) const{
            return kind == other.kind && call == other.call;
        }
    };

    const char* undoOriginKindName(UndoOriginKind kind);

    // The origin of the commands pushed while it lives (main thread). Nested scopes stack:
    // the innermost wins, and the previous one comes back when it ends.
    class UndoOriginScope{
    private:
        UndoOrigin previous;

    public:
        explicit UndoOriginScope(UndoOrigin origin);
        ~UndoOriginScope();

        UndoOriginScope(const UndoOriginScope&) = delete;
        UndoOriginScope& operator=(const UndoOriginScope&) = delete;

        static const UndoOrigin& current();
    };

}
