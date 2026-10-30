// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Pose animations, humanoid maps and animation clips (S05 R11, with upstream's animation clips
// spec 4.1 and pose animation phase 2): the open PoseAnimationDocuments of changed .poseanim /
// .humanoid files and the open AnimationClipDocuments of changed .anim files reload (a file that
// fails to load leaves the document at its last valid content), and AnimationClipPool drops the
// clips so the Animation Players using them reload, in edit mode and during Play. A changed
// model drops its sub-clips, their documents and the players using them the same way. The load
// errors and warnings of .anim and .poseanim files become problems ({path, line, message}).

#include "ReloadHandlers.h"

#include "Project.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimatorAssetDocument.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/AnimatorRetarget.h"
#include "pool/AnimatorAssetPool.h"
#include "pose/PoseAnimationDocument.h"
#include "pose/PoseAnimationIO.h"
#include "util/ProjectChecks.h"
#include "util/Util.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    // At most this many load warnings of one file become problems (then a count)
    constexpr size_t ANIMATION_WARNINGS_MAX = 8;

    // 1-based line of a JSON syntax error in `text`, 0 when it parses
    int jsonErrorLine(const std::string& text) {
        try {
            const Json parsed = Json::parse(text);
            (void)parsed;
        } catch (const Json::parse_error& e) {
            int line = 1;
            for (size_t i = 0; i < e.byte && i < text.size(); i++) {
                if (text[i] == '\n') line++;
            }
            return line;
        } catch (...) {
        }
        return 0;
    }

    // What fails to load in a changed .anim or .poseanim, then its warnings: the same reading
    // the documents and the players do
    void addAnimationFileProblems(Project& project, const fs::path& path, Json& problems) {
        const std::string pathText = path.string();
        const bool clip = Util::isAnimationClipFile(pathText);
        const bool animatorAsset = Util::isAnimatorAssetFile(pathText);
        if (!clip && !animatorAsset && !Util::isPoseAnimationFile(pathText)) return;
        std::error_code ec;
        std::string text;
        if (!fs::is_regular_file(path, ec) || !ProjectChecks::readFile(path, text)) return;
        const std::string relative = reload::rel(project, path);

        std::vector<std::string> warnings;
        if (animatorAsset) {
            // the Animator's assets: the reading AnimatorAssetPool does
            std::vector<ClipWarning> loadWarnings;
            anim::AnimatorParseError error;
            bool ok = false;
            if (Util::isAnimatorControllerFile(pathText)) {
                anim::ControllerParseResult parsed = anim::parseController(text);
                ok = parsed.ok;
                error = parsed.error;
                loadWarnings = parsed.warnings;
            } else if (Util::isAvatarMaskFile(pathText)) {
                anim::MaskParseResult parsed = anim::parseMask(text);
                ok = parsed.ok;
                error = parsed.error;
                loadWarnings = parsed.warnings;
            } else {
                anim::MontageParseResult parsed = anim::parseMontage(text);
                ok = parsed.ok;
                error = parsed.error;
                loadWarnings = parsed.warnings;
            }
            if (!ok) {
                Json problem = {{"path", relative}, {"message", "Not a valid file: " + error.message +
                                                                " (running Animators keep its last valid content)."}};
                if (error.line > 0) {
                    problem["line"] = error.line;
                    problem["col"] = error.column;
                }
                problems.push_back(problem);
                return;
            }
            for (const ClipWarning& warning : loadWarnings) {
                warnings.push_back(warning.path.empty() ? warning.message : warning.path + ": " + warning.message);
            }
        } else if (clip) {
            const anim::ClipParseResult parsed = anim::parseClip(text);
            if (!parsed.ok()) {
                Json problem = {{"path", relative}, {"message", "Not a valid animation clip: " + parsed.error.message +
                                                                " (an open clip keeps its last valid content)."}};
                if (parsed.error.line > 0) {
                    problem["line"] = parsed.error.line;
                    problem["col"] = parsed.error.column;
                }
                problems.push_back(problem);
                return;
            }
            for (const ClipWarning& warning : parsed.warnings) {
                warnings.push_back(warning.path.empty() ? warning.message : warning.path + ": " + warning.message);
            }
        } else {
            pose::PoseAnimationData data;
            std::string error;
            if (!pose::parsePoseAnimation(text, data, warnings, &error)) {
                Json problem = {{"path", relative}, {"message", "Not a valid pose animation: " + error +
                                                                " (an open pose animation keeps its last valid content)."}};
                if (const int line = jsonErrorLine(text)) problem["line"] = line;
                problems.push_back(problem);
                return;
            }
        }
        for (size_t i = 0; i < warnings.size() && i < ANIMATION_WARNINGS_MAX; i++) {
            problems.push_back({{"path", relative}, {"severity", "warning"}, {"message", warnings[i]}});
        }
        if (warnings.size() > ANIMATION_WARNINGS_MAX) {
            problems.push_back({{"path", relative}, {"severity", "warning"},
                                {"message", "(+" + std::to_string(warnings.size() - ANIMATION_WARNINGS_MAX) + " more warnings)"}});
        }
    }

}

void reload::reloadAnimations(Context& ctx, const std::vector<fs::path>& files, const std::vector<fs::path>& models, bool all) {
    Project* project = &ctx.project;
    if (!files.empty()) {
        PoseAnimationDocument::filesChanged(files);
    }
    std::vector<fs::path> clipSources = files;
    clipSources.insert(clipSources.end(), models.begin(), models.end());
    if (!clipSources.empty()) {
        AnimationClipDocument::filesChanged(clipSources, project);
    }
    // the Animator's assets: open documents reload, running Animators rebind (S21 R35); a changed
    // model or .humanoid drops the cached retargeting sources
    bool humanoidChanged = !models.empty();
    for (const fs::path& path : files) {
        const std::string text = path.string();
        if (Util::isAnimatorAssetFile(text)) AnimatorAssetPool::invalidate(text);
        if (path.extension() == ".humanoid") humanoidChanged = true;
    }
    if (!files.empty()) AnimatorAssetDocument::filesChanged(files);
    if (humanoidChanged) anim::clearRetargetSources();
    if (all) {
        PoseAnimationDocument::checkDiskChanges();
        AnimationClipDocument::checkDiskChanges(project);
    }
    for (const fs::path& path : files) {
        addAnimationFileProblems(ctx.project, path, ctx.result.problems);
    }
}
