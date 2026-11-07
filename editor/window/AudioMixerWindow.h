// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Window > Audio > Audio Mixer (audio-mixer spec 9): the project's .mixer (or one opened from the
// Project window) as Unity's Audio Mixer: the groups tree, snapshots and exposed parameters on
// the left, a channel strip per group (fader, M / S / B, live meter, effects). Every edit is one
// undo step written to the file (AudioMixerDocument); the project's mixer is heard as it changes,
// in Edit mode and in Play (Unity's "Edit in Play Mode" always on). A drag is heard live and
// written once, when it ends.

#include "audio/AudioMixerIO.h"

#include <filesystem>
#include <map>
#include <string>

namespace jasperengine::editor {

    class Project;

    class AudioMixerWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Audio Mixer";

        explicit AudioMixerWindow(Project* project);

        void show();
        // the project's mixer (empty path), or a .mixer file
        void open(const std::filesystem::path& mixer = {});
        void setOpen(bool open);
        bool isOpen() const;
        void resetForProjectSwitch();

    private:
        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;

        // the file shown; empty: the project's (it follows Project Settings > Audio)
        std::filesystem::path chosenFile;
        std::filesystem::path file;
        AudioMixerDef def;
        bool loaded = false;
        std::string loadError;
        std::string fileText;
        double lastCheck = -1.0;
        std::string error;

        // a drag in progress: heard live, written when it ends
        bool dragging = false;

        std::string selectedGroup;
        std::string renaming;            // "group:<id>", "snapshot:<name>", "exposed:<name>"
        char renameBuffer[64] = {0};
        bool renameFocus = false;
        std::string exposeTarget;
        char exposeName[64] = {0};
        bool openExposePopup = false;
        std::string pendingDeleteGroup;
        bool openDeletePopup = false;

        struct PeakHold {
            float db = -80.0f;
            double time = 0.0;
        };
        std::map<std::string, PeakHold> peaks;

        bool isProjectMixer() const;
        void resolveFile();
        void reload(bool force);
        // `next` as one undo step, written at the start of the next frame (the widgets drawn after
        // the edit still read the def of this one)
        bool commit(const AudioMixerDef& next);
        bool write(const AudioMixerDef& next);
        // a change in a drag: heard next frame, written once when the drag ends
        void preview(const AudioMixerDef& next);
        void finishDrag();
        void applyStaged();
        AudioMixerDef staged;
        bool hasStaged = false;
        bool stagedWrite = false;
        bool dragEnded = false;

        void drawToolbar();
        void drawSidebar();
        void drawGroupNode(const AudioMixerGroupDef& group);
        void drawSnapshots();
        void drawExposed();
        void drawStrips();
        void drawStrip(const AudioMixerGroupDef& group);
        void drawMeter(const std::string& groupId, float height);
        // the Master strip: LUFS against the target, the true-peak hold, the limiter's reduction
        void drawLoudness(const AudioMixerGroupDef& master);
        void drawEffect(const AudioMixerGroupDef& group, size_t index, float width);
        void drawPopups();
        // the right-click menu of a control: expose / unexpose the target
        void exposeMenu(const std::string& target, const std::string& suggestedName);
        std::string exposedName(const std::string& target) const;
    };

}
