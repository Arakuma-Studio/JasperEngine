// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef DIALOGUE_PROGRAM_H
#define DIALOGUE_PROGRAM_H

// Contract (docs/specs/dialogue-graphs.md R2, R3, R12, R16, D5, D6)
// - A `.dialogue` graph compiled for the runner: its nodes in id order with their outputs resolved
//   to node indexes (an unconnected output is -1: the conversation ends there), expressions
//   compiled once (DialogueExpression.h), every string in one pool (a node is ~100 bytes plus its
//   texts). Texts are the source texts; the runner looks the keys up through Localization first.
// - Keys (R3): a line `<ns>.<node>`, a variant `<ns>.<node>.<vid>`, an option `<ns>.<node>.<oid>`.
// - compile() never refuses a graph: a node whose expression does not parse gets an empty one (the
//   lint reports it, bad-expression); an unknown node type ends the conversation there.
// - Cooked (R16): an export ships each `.dialogue` as its compiled program's bytes under the same
//   path (cook()), which a game reads without parsing JSON or compiling (fromCooked(): bounds,
//   indexes and every expression's stack checked, so a damaged file is refused, never run). The
//   editor keeps the JSON; Programs reads either.
// - Programs: the runtime's cache of loaded dialogues by path (FileData rules), reloaded when a file
//   changes (the editor's hot reload in Play); listeners hear each reload.
// - Thread-safety: a compiled program is immutable; the cache belongs to the main thread.

#include "Export.h"
#include "dialogue/DialogueExpression.h"
#include "graph/Graph.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jasperengine::dialogue {

    inline constexpr const char* KIND_ID = "dialogue";
    inline constexpr const char* FILE_EXTENSION = ".dialogue";

    enum class NodeKind : uint8_t { Entry, Line, Branch, Set, Action, Jump, End, Unknown };
    enum class VariantMode : uint8_t { Random, Sequence, Once };

    struct StrRef {
        uint32_t offset = 0;
        uint32_t length = 0;
    };

    struct ProgramVariant {
        StrRef id;
        StrRef text;
    };

    struct ProgramOption {
        StrRef id;
        StrRef text;
        StrRef style;
        Expr condition;
        Expr action;
        int32_t target = -1;
        uint32_t tagsFirst = 0;
        uint16_t tagsCount = 0;
        bool once = false;
        bool disable = false;       // when_false: disable (shown greyed) instead of hide
    };

    struct ProgramNode {
        NodeKind kind = NodeKind::Unknown;
        VariantMode variantMode = VariantMode::Random;
        bool wait = false;          // an action node that waits for resume() whatever its handlers return
        int32_t next = -1;          // next / true
        int32_t alt = -1;           // a branch's false
        StrRef id;
        StrRef a;                   // entry name, line speaker, jump entry, end result
        StrRef b;                   // line mood, jump dialogue path
        StrRef text;                // line text
        StrRef voice;               // line voice asset path
        Expr expr;                  // branch condition, set assignments, action calls, line action
        uint32_t variantsFirst = 0;
        uint16_t variantsCount = 0;
        uint16_t optionsCount = 0;
        uint32_t optionsFirst = 0;
        uint32_t tagsFirst = 0;
        uint16_t tagsCount = 0;
    };

    class JASPERENGINE_API Program {
    public:
        // `path`: what the runtime loads it by (and what a jump names); `issues` gets compile problems
        static std::shared_ptr<const Program> compile(const graph::Graph& graph, const std::string& path, std::vector<graph::Issue>* issues = nullptr);
        // The cooked file: COOKED_MAGIC, a version, then every field (not the path, which is what the
        // game loads it by). Equal programs give equal bytes.
        std::string cook() const;
        static bool isCooked(std::string_view bytes);
        // null with `error` when the bytes are not a whole, consistent cooked program of this version
        static std::shared_ptr<const Program> fromCooked(std::string_view bytes, const std::string& path, std::string* error = nullptr);
        static constexpr char COOKED_MAGIC[4] = {'J', 'D', 'L', 'G'};
        static constexpr uint32_t COOKED_VERSION = 1;

        std::string_view str(StrRef ref) const { return std::string_view(strings).substr(ref.offset, ref.length); }
        std::string_view nodeId(int index) const { return str(nodes[static_cast<size_t>(index)].id); }
        // -1 when absent
        int findNode(std::string_view id) const;
        int findEntry(std::string_view name) const;
        // `start`, else the entry with the lowest id; -1 without entries
        int defaultEntry() const;
        // an entry's name or a node id (a jump's target); -1 when neither
        int findTarget(std::string_view entryOrNode) const;
        // a graph variable's default (Variables reads it on first read); null when not declared
        const Value* defaultOf(std::string_view name) const;
        bool usesVariable(std::string_view name) const;

        // the visit identity: the file's GUID, else its path
        const std::string& identity() const { return guid.empty() ? path : guid; }
        size_t memoryBytes() const;
        // the bytes of its texts (lines, variants, options): what the per-node budget leaves out
        size_t textBytes() const;

        std::string path;
        std::string guid;
        std::string ns;                     // metadata.namespace
        std::string sourceLanguage;         // metadata.source_language ("" = the project default)
        std::string strings;
        std::vector<ProgramNode> nodes;     // in id order
        std::vector<ProgramOption> options;
        std::vector<ProgramVariant> variants;
        std::vector<StrRef> tags;
        std::vector<std::pair<StrRef, int32_t>> entries;
        std::vector<std::pair<std::string, Value>> defaults;
        std::vector<std::string> variablesUsed;     // sorted
        ExprPool exprs;
    };

    // The keys and source texts of a dialogue's String Table entries, in node order (without the
    // namespace: "n7", "n7.v1", "n7.o2")
    struct SourceEntry {
        std::string key;
        std::string text;
        std::string nodeId;
    };
    JASPERENGINE_API std::vector<SourceEntry> sourceEntries(const graph::Graph& graph);
    // metadata.namespace, else a namespace made from the file's stem
    JASPERENGINE_API std::string namespaceOf(const graph::Graph& graph, std::string_view path);
    JASPERENGINE_API std::string namespaceFromStem(std::string_view stem);

    class JASPERENGINE_API Programs {
    public:
        using Listener = std::function<void(const std::shared_ptr<const Program>& program)>;

        // the dialogue at `path` (cached); null with `error` when it does not load
        static std::shared_ptr<const Program> load(const std::string& path, std::string* error = nullptr);
        // a compiled program from a file's bytes under that path, the JSON or the cooked form (tests,
        // the editor's walker)
        static std::shared_ptr<const Program> fromText(const std::string& path, std::string_view text, std::string* error = nullptr);
        // a file changed (an absolute or asset path): every cached program of it recompiles and the
        // listeners hear it; true when one was cached
        static bool fileChanged(const std::string& path);
        static int addListener(Listener listener);
        static void removeListener(int id);
        static void clear();
    };

}

#endif //DIALOGUE_PROGRAM_H
