// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The device commands of Build And Run and Patch And Run (docs/specs/build-and-run-on-device.md
// R3, R8, R16). Every process is an argument vector; the only shell is the device's own, which
// `adb shell` hands its one command string to, and `run-as <package> sh -c` parses once more, so a
// path in it is quoted twice, by the one helper here.

#include "player/ContentManifest.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor::platform::devicecmd {

    // A POSIX sh word: 'text' with every ' as '\'' (spaces, quotes, $, `, \ and non-ASCII bytes
    // pass through unchanged)
    std::string shellQuote(const std::string& word);
    // words quoted and joined by spaces
    std::string shellJoin(const std::vector<std::string>& words);

    // adb -s <serial> <args...>
    std::vector<std::string> adb(const std::string& adbPath, const std::string& serial, const std::vector<std::string>& args);
    // adb -s <serial> shell "run-as <package> sh -c '<script>'": the script runs as the app, in its
    // data folder (files/ is its internal files folder)
    std::vector<std::string> runAs(const std::string& adbPath, const std::string& serial, const std::string& package, const std::string& script);

    // the app's development content folder, relative to its data folder
    std::string developmentFolder();
    // where a patch is pushed before the app copies it: /data/local/tmp/jasper/<package>
    std::string remoteRoot(const std::string& package);
    std::string remotePatchDir(const std::string& package, int patch);

    // The script run as the app for a patch pushed to `pushedDir` (<pushedDir>/files/... and
    // <pushedDir>/content-manifest.json): a first patch empties the folder; the files are copied,
    // the ones the new content no longer has deleted, and the manifest copied last, each step only
    // when the one before succeeded
    std::string patchScript(const std::string& pushedDir, const std::vector<std::string>& deleted, bool first);
    // the script that empties the development folder (a full install)
    std::string clearScript();

    // What a patch sends: the files whose SHA-256 or size differ from what the device runs (all of
    // them on the first patch, which mirrors the content) and the ones to delete. content-manifest.json
    // is not among them: it is written last, always.
    struct PatchPlan {
        std::vector<std::string> push;
        std::vector<std::string> remove;
        uint64_t bytes = 0;
        bool first = false;
    };
    PatchPlan planPatch(const std::map<std::string, player::ManifestFile>& onDevice, const std::map<std::string, player::ManifestFile>& next,
                        bool first);

    // the player options as a web page's URL query (LaunchOptions::argsFromQuery reads it back):
    // "--fixed-fps 60 --after-ready" -> "fixed-fps=60&after-ready"; the game's own arguments
    // after "--" have no query form and are left out
    std::string queryFromArgs(const std::vector<std::string>& args);

}
