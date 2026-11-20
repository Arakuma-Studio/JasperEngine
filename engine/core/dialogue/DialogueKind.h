// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef DIALOGUE_KIND_H
#define DIALOGUE_KIND_H

// Contract (docs/specs/dialogue-graphs.md R1, R2, R11, R30, D1)
// - The `dialogue` graph kind (`.dialogue`, kindVersion 1, not acyclic, strictParams): node types
//   entry, line, branch, set, action, jump, end on `flow` ports (an output feeds at most one edge,
//   which the lint checks; inputs take many). A line's ports follow its options (o<id> each).
// - The lint (the kind's validate): no-entry, duplicate-entry, fan-out, bad-expression (with the
//   column), jump-target, bad-message (Errors); unreachable, no-exit, unknown-action-prefix,
//   unknown-condition-prefix, unknown-variable, unknown-argument, empty-line (Warnings);
//   duplicate-option-text (Info). What the project declares (prefixes, variables, other dialogues'
//   jumps and entries) comes from a LintContext the editor sets; without one, only the file is
//   known (prefix and variable checks then use the graph alone).
// - registerDialogueKind() registers the kind and the `.dialogue` reader of Localization (the
//   source texts as the dialogue's String Table), once; the engine calls it at start with the
//   module, the editor at start.
// - Thread-safety: registration is once; the context is swapped whole (main thread).

#include "Export.h"
#include "graph/GraphRegistry.h"

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::dialogue {

    struct LintContext {
        std::set<std::string> actionPrefixes;       // Project Settings > Dialogue; empty: no prefix check
        std::set<std::string> conditionPrefixes;
        std::set<std::string> variables;            // declared project-wide
        // a jump's target file (an asset {guid, path}'s path) and entry or node: "" when it exists, else why not
        std::function<std::string(const std::string& path, const std::string& target)> jumpTarget;
        // the entries or nodes of the dialogue with this GUID that other dialogues jump to
        std::function<std::set<std::string>(const std::string& guid)> jumpedInto;
    };

    JASPERENGINE_API graph::GraphKindDesc makeDialogueKind();
    JASPERENGINE_API void registerDialogueKind();
    JASPERENGINE_API void setLintContext(std::shared_ptr<const LintContext> context);
    // a context made at each lint (the editor's: the project's settings and dialogues as they are
    // now); a set context wins over it
    JASPERENGINE_API void setLintContextProvider(std::function<std::shared_ptr<const LintContext>()> provider);
    JASPERENGINE_API std::shared_ptr<const LintContext> lintContext();
    // the lint alone (validate() runs it after the generic checks)
    JASPERENGINE_API void lint(const graph::Graph& graph, std::vector<graph::Issue>& issues);
    // the argument names of an ICU message's top level and nested texts ({gold}, {n, plural, ...})
    JASPERENGINE_API std::vector<std::string> messageArguments(const std::string& message);
    // a line's ports for its options (the `line` type's dynamicPorts)
    JASPERENGINE_API std::vector<graph::Port> linePorts(const graph::Json& params);

}

#endif //DIALOGUE_KIND_H
