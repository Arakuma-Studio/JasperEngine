// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The third-party notices every shipped build carries (docs/specs/S30-shipping.md R27):
// licenses/<library>-LICENSE (and the library's other licence and NOTICE files) for each library
// its player links, and THIRD-PARTY-NOTICES.txt from their NOTICE.md rows. engine/notices.json
// says which game links what; the names are tools/ci/check-notices.py's.

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::publish {

    struct NoticeLibrary {
        std::string name;
        std::string dir;      // repository-relative ("engine/libs/sokol")
        // always, editor, or the player features that link it (htmlui, physics2d, titleart...):
        // notices.json's `when`, one condition or a list of them (ThorVG ships with either feature)
        std::vector<std::string> when;
    };

    bool loadNoticeLibraries(const std::filesystem::path& noticesJson, std::vector<NoticeLibrary>& out, std::string& error);
    // the libraries a game with these features ships, by name
    std::vector<NoticeLibrary> shippedLibraries(const std::vector<NoticeLibrary>& all, const std::vector<std::string>& features);
    // a library directory's licence files then its NOTICE files (check-notices.py licence_files)
    std::vector<std::filesystem::path> licenceFiles(const std::filesystem::path& libraryDir);
    // "<library>-LICENSE" for the first, "<library>-<file name>" for the others
    std::string stagedNoticeName(const std::string& library, size_t index, const std::filesystem::path& file);
    // THIRD-PARTY-NOTICES.txt: each library's NOTICE.md row as a line
    std::string thirdPartyNotices(const std::string& noticeMd, const std::vector<NoticeLibrary>& libraries);

    // Where the editor finds engine/notices.json, NOTICE.md and the libraries' licence files (the
    // repository of a development editor, the install of a packaged one)
    struct NoticeSources {
        std::filesystem::path noticesJson;
        std::filesystem::path noticeMd;
        // the directory engine/notices.json's "dir" paths are relative to
        std::filesystem::path repositoryRoot;
    };
    bool findNoticeSources(NoticeSources& out, std::string& error);

    // a Compile build's notices (the player runtimes carry their own): the libraries of these
    // features, from the editor's sources, into <target>/licenses and <target>/THIRD-PARTY-NOTICES.txt
    bool stageCompileNotices(const std::filesystem::path& target, const std::vector<std::string>& features,
                             std::vector<std::string>& staged, std::string& error);

}
