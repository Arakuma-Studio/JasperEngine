// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The shipping commands of the command line (docs/specs/S30-shipping.md R31), headless: they
// print stable `key: value` lines (or --json) and exit non-zero on failure.
//
//   build    --project P --platform <windows|linux|macos|android|ios|web> --out D [...]
//   players  list | install <zip> | download <id> | remove <id> [--players-dir D]
//   publish  init | prepare | verify | steam-vdf | steam-upload | deck-deploy | deck-rollback | deck-smoke
//   reflect  --emit-runtime-fields <file>

namespace jasperengine::editor {

    class ShippingCommands {
    public:
        // argv[0] is the subcommand itself
        static int runBuild(int argc, char** argv, const char* executableName);
        static int runPlayers(int argc, char** argv, const char* executableName);
        static int runPublish(int argc, char** argv, const char* executableName);
        static int runReflect(int argc, char** argv, const char* executableName);
    };

}
