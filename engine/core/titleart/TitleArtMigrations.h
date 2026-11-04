// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R2, R63, §6.1 "Migration framework")
// - Purpose: the JSON migration framework of the Title Art formats (editor/StreamMigrations.h's
//   design for JSON): an ordered table of `vN -> vN+1` steps run on the parsed JSON before
//   decode, pure (no editor state), so the engine, the CLI and the tests run it. One table per
//   format: `.titleart` here; S84's looks and colour schemes make their own tables of the same
//   class.
// - Rules every step follows: adding an optional member, a layer type, an effect, a generator or
//   an enum value does not bump the version (defaults and unknown-data preservation cover it);
//   renaming, moving or re-typing a member, or changing what a value means, bumps the version,
//   appends one step and a fixture (engine/tests/core/titleart/fixtures/v<N>/). A member a step
//   introduces gets the value that keeps old files rendering as they did (fill the old
//   behaviour, not the new default). Every step is idempotent (tested: twice equals once) and
//   no step is ever removed. A renderer change without a format change bumps
//   TITLE_ART_BAKE_VERSION instead.
// - The `.titleart` v1 table is empty: v1 is the first version.
// - Thread-safety: the tables are built once; add() at startup or from a test.

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"

#include <string>
#include <vector>

namespace jasperengine::titleart {

    // what the steps of one run did (their notes go to the parse warnings)
    struct JASPERENGINE_API MigrationLog {
        std::vector<std::string> applied;   // "v1->v2 rename_rim_width" per step run
        std::vector<std::string> notes;     // a step's notes ("layers[0].rims: moved to style.rims")

        void note(const std::string& text) { notes.push_back(text); }
    };

    struct MigrationStep {
        int from = 0;
        int to = 0;                         // from + 1
        const char* name = "";
        void (*apply)(Json& root, MigrationLog& log) = nullptr;
    };

    class JASPERENGINE_API TitleJsonMigrations {
    public:
        // `format` names the table in messages; `firstVersion` is the oldest version there is
        explicit TitleJsonMigrations(std::string format, int firstVersion = 1);

        // appends a step; false (and nothing added) when it does not continue the chain (`from`
        // is not the last step's `to`, or the first step's `from` is not `firstVersion`), `to` is
        // not `from + 1`, or `apply` is null
        bool add(MigrationStep step);
        // tests: drops the last step (only steps a test added)
        void removeLast();

        // the newest version the steps reach (firstVersion when there are none)
        int latestVersion() const;
        const std::vector<MigrationStep>& steps() const { return chain; }
        const std::string& format() const { return formatName; }

        // runs the steps from `fromVersion` up to `toVersion` on `root` and sets its "version";
        // false with `error` when `fromVersion` is older than the table or a step is missing
        bool run(Json& root, int fromVersion, int toVersion, MigrationLog& log, std::string* error) const;

    private:
        std::string formatName;
        int first = 1;
        std::vector<MigrationStep> chain;
    };

    // the `.titleart` table (empty at v1; TITLE_ART_VERSION is its latest)
    JASPERENGINE_API TitleJsonMigrations& titleArtMigrations();

}
