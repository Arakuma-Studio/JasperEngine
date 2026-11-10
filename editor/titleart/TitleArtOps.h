// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R9, R54, §6.2, §6.15 "Reserved op names")
// - Purpose: the one document operations API of Title Art. The Title Designer (S82), the
//   Inspector, MCP's edit_title and editor Lua (Editor.call("edit_title")) all edit documents
//   through apply(): a batch of ops (JSON objects with `op`) applied to a copy of the document,
//   validated, then given to TitleArtDocument::commit as one project undo step named by the ops
//   ("Set Rim Width", "Add Effect Outer Glow", "Edit Title Art (5 changes)").
// - All or nothing: any failing op leaves the result !ok with that op's index, its path, the
//   valid alternatives and the nearest of them; the input document is never changed (apply is
//   pure). Values are clamped by the schema (each clamp echoed in `changes`), `%` members echo
//   their pixel value.
// - Addressing (§6.2): layers by a path of names from the top level joined by '/', each followed
//   by "#N" for the Nth same-named sibling after the first (EntityPath's grammar: "Logo/Title",
//   "Spark#2"), or "id:<8 hex>"; a bare name not found at the top level matches a layer of that
//   name anywhere, and is refused listing the paths when several match. Fields by dot paths with
//   [i] indexes and map keys, with the shorthands of TitleArtIO.h's expandFieldShorthand.
// - Commit-time invariants (applyCommitInvariants, run after the ops of every batch): normalize,
//   then link sync (the first material of a link group that this batch changed, in layer then
//   part order, is copied to the others), then tree checks (a clip with no base cleared; dead
//   part keys dropped by normalize; masks, `follow` and exposed targets naming missing layers
//   reported). Editors never reason about links or tree shape.
// - Later specs' ops: the reserved names of §6.15 answer `not_available` naming their owner until
//   the owner registers a handler (registerOp, from its registration function), which then runs
//   inside the same batch with the same TitleOpContext helpers and the same all-or-nothing rule.
//   `create.from` importers (S87) register the same way (registerCreateImporter).
// - Thread-safety: main thread (the registry); apply() itself only touches its copy.

#pragma once

#include "titleart/TitleArtDoc.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::titleart {
    class AssetResolver;
}

namespace jasperengine::editor {

    using TitleJson = jasperengine::titleart::Json;

    // one field a batch changed (MCP's `changes`)
    struct TitleOpChange {
        std::string layer;                  // layer id ("" for document members)
        std::string path;                   // long form
        TitleJson from, to;
        bool clamped = false;
        std::string clampMessage;
        std::optional<double> px;           // `%` members: the value in canvas pixels at the layer's size
        std::string note;                   // structural changes ("removed mask 2 that named it")

        TitleJson toJson() const;
    };

    // a failure or a note of a batch (MCP's `problems`)
    struct TitleOpProblem {
        std::string severity = "error";
        std::string code;                   // bad_op, bad_path, bad_value, bad_layer, ambiguous, not_available, read_only ...
        int op = -1;                        // index in the batch
        std::string path;
        std::string message;
        std::string nearest;
        std::vector<std::string> validHere;
        TitleJson fix;                      // ops that would make it pass, when there is one

        TitleJson toJson() const;
    };

    // a file a batch writes besides the document (S84: fonts and environments imported into the
    // project, saved library items): `undoable` ones join the document's undo step, the others
    // (the personal library, outside the project) are written once the batch is committed
    struct TitleOpFile {
        std::filesystem::path path;         // absolute
        std::string content;
        bool undoable = true;
        bool remove = false;                // the file goes (a deleted library item)
    };

    struct TitleOpsOptions {
        uint64_t idSeed = 0;                // 0: random new ids (tests pass a seed for a fixed sequence)
        // where the document lives (absolute paths; empty for a document without a file): ops
        // that write files beside it (S87's import_svg, apply_ps_style) need them
        std::string documentPath;
        std::string projectPath;
        std::string assetsPath;
        // S87: the project's default language (add_language's source when it names none) and the
        // fonts that measure text (add_language's default fit box); null: the built-in face
        std::string defaultLanguage;
        const titleart::AssetResolver* assets = nullptr;
    };

    struct TitleOpsResult {
        bool ok = false;
        titleart::TitleArtDoc doc;          // the edited copy (normalized, invariants applied) when ok
        size_t applied = 0;
        int failedOp = -1;
        std::vector<TitleOpChange> changes;
        TitleJson newIds = TitleJson::object();     // op index -> the id(s) it made
        std::vector<TitleOpProblem> problems;       // the failure (when !ok) and the notes
        std::vector<TitleOpFile> files;             // the files the batch writes besides the document
        TitleJson details = TitleJson::object();    // op index -> what a later spec's op reports (S84's apply `changes`)
        std::string undoName;
        // what ops report beyond changes (import_strings' counts, apply_ps_style's summary), by op index
        TitleJson reports = TitleJson::object();
        // the error of a failed batch, ending "Nothing was applied."
        std::string error() const;
        TitleJson toJson() const;
    };

    class TitleOpContext;
    // an op of a later spec: false after ctx.fail(...) refuses the batch
    using TitleOpHandler = std::function<bool(TitleOpContext& ctx, const TitleJson& op)>;
    // runs after every op of a successful batch, before the commit invariants (S84: the scheme data
    // of exposed color_scheme parameters kept in `color_schemes`)
    using TitleBatchFinisher = std::function<void(TitleOpContext& ctx)>;

    // what an op handler works with: the batch's working copy and the helpers S80's own ops use
    class TitleOpContext {
    public:
        titleart::TitleArtDoc& doc;
        int opIndex = 0;

        TitleOpContext(titleart::TitleArtDoc& doc, titleart::TitleIdGenerator& ids, TitleOpsResult& result,
                       const TitleOpsOptions* options = nullptr);

        // the layer `op[member]` names (path, bare name or id:); null after fail() when it does
        // not resolve (or is missing while `required`)
        TitleJson* layer(const TitleJson& op, const char* member = "layer", bool required = true);
        // the layer a reference names, null with the problem when none or several
        TitleJson* resolveLayer(const std::string& ref, const std::string& member);
        // the path of a layer ("Logo/Title#1")
        std::string layerPath(const std::string& id) const;
        // writes one field (TitleArtIO's setTitleArtField) and records the change; false after fail()
        bool set(TitleJson* layer, const std::string& path, const TitleJson& value);
        std::string newId();
        void recordNewId(const std::string& key, const std::string& id);
        void change(TitleOpChange change);
        void note(const std::string& code, const std::string& message, const std::string& path = {});
        // records the failure of this op; always returns false
        bool fail(const std::string& code, const std::string& message, const std::string& path = {},
                  std::vector<std::string> validHere = {}, const std::string& nearest = {}, TitleJson fix = nullptr);
        // a member of `op` of the wrong type fails naming it
        bool requireString(const TitleJson& op, const char* member, std::string& out);

        titleart::TitleIdGenerator& ids;
        TitleOpsResult& result;
        // the batch's options (the document's paths), never null inside apply()
        const TitleOpsOptions* options = nullptr;
        // the label of the field the last set() wrote ("Rim Width", "Outer Glow Size"): a one-value
        // `set` names its undo step "Set <label>"
        std::string lastSetLabel;
        // a registered op's own undo step name ("Long Shadow", "Set Dimension 3D", "Apply Material Gold");
        // empty: its title-cased op name
        std::string undoName;
    };

    struct ReservedTitleOp {
        const char* name;
        const char* owner;      // "S84"
    };

    // `create.from` (S87's imports): a source file read into a new document at `documentPath`
    struct TitleCreateRequest {
        std::string sourcePath;             // absolute; may be outside the project (only read)
        std::string documentPath;           // the new document, absolute
        std::string projectPath;
        std::string assetsPath;
    };
    struct TitleCreateResult {
        titleart::TitleArtDoc doc;
        std::string kind;                   // pachislop, psd, svg
        std::vector<TitleOpFile> files;     // fonts, images, split SVGs written with the document
        TitleJson notes = TitleJson::array();   // the import report (S87 R31)
        std::vector<std::string> warnings;
    };
    // false with `code` and `error` (one sentence)
    using TitleCreateImporter = std::function<bool(const TitleCreateRequest& request, TitleCreateResult& out, std::string& code, std::string& error)>;
    // whether the first bytes of a file (and its lowercase extension) are an importer's kind
    using TitleCreateSniffer = std::function<bool(const std::string& head, const std::string& extension)>;

    class TitleArtOps {
    public:
        // applies `ops` (an array of op objects, or one op object) to a copy of `doc`
        static TitleOpsResult apply(const titleart::TitleArtDoc& doc, const TitleJson& ops, const TitleOpsOptions& options = {});

        // the ops S80 implements, in the order the agent docs list them
        static const std::vector<std::string>& s80Ops();
        // one of S80's ops inside a registered op's batch (apply_recipe adds its effect through
        // add_effect), its undo step name in `undoName`; false after ctx.fail()
        static bool runCoreOp(TitleOpContext& ctx, const TitleJson& op, std::string* undoName = nullptr);
        // the reserved names of §6.15 and their owners
        static const std::vector<ReservedTitleOp>& reservedOps();
        // the owner of a reserved name without a handler yet ("" when it is not reserved or has one)
        static std::string pendingOwner(const std::string& name);

        // a later spec's handler for `name` (a reserved name or a new one); false when the name is
        // S80's or already has a handler
        static bool registerOp(const std::string& name, TitleOpHandler handler);
        static bool unregisterOp(const std::string& name);
        // a later spec's step at the end of every batch (replaces one of the same name)
        static void registerBatchFinisher(const std::string& name, TitleBatchFinisher finisher);

        // S87's importers of `create.from` by kind ("pachislop", "psd", "svg"), each with the
        // sniffer that recognises its files by content (the first 4 KiB) and extension
        static bool registerCreateImporter(const std::string& kind, TitleCreateSniffer sniff, TitleCreateImporter importer);
        static bool unregisterCreateImporter(const std::string& kind);
        // the document to create from a source file, its kind detected by content; false with
        // `code` and `error` (`not_available` naming S87 when no importer knows the file)
        static bool createFrom(const TitleCreateRequest& request, TitleCreateResult& out, std::string& code, std::string& error);

        // normalize, link sync (against `before`, null for a new document), tree checks; notes go to
        // `result` (changes and problems)
        static void applyCommitInvariants(const titleart::TitleArtDoc* before, titleart::TitleArtDoc& after,
                                          titleart::TitleIdGenerator& ids, TitleOpsResult& result);
    };

}
