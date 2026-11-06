// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Window > Audio > Sound Browser (docs/specs/S37-sound-events.md R18): search, audition and drag the
// sounds of the project, the essentials and the starter library. The searching is S36's SoundSearch,
// the waveforms AudioWaveform's; the clip detail is the Inspector's (it follows the selection).
// Previews are editor voices outside the game's mix, none while a capture records.

#include "audio/SoundSearch.h"

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace SoLoud {
    class AudioSource;
}

namespace jasperengine::editor {

    class Project;

    class SoundBrowserWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Sound Browser";
        static constexpr const char* BUILTIN_PAYLOAD = "builtin_sounds";

        explicit SoundBrowserWindow(Project* project);
        ~SoundBrowserWindow();

        void show();
        void open();
        void setOpen(bool open);
        bool isOpen() const;
        bool isFocused() const;
        void resetForProjectSwitch();

        // the window's commands (soundBrowser.*)
        void focusSearch();
        void selectNext(int step);
        void playOrStop(bool matched);
        void useSelected();
        void selectAll();
        void stopOrClear();
        bool hasSelection() const;

    private:
        struct IndexJob {
            SoundSearch::Sources sources;
            std::vector<SoundSearch::Entry> entries;
            std::atomic<bool> cancel{false};
            std::atomic<bool> done{false};
        };

        struct WaveJob {
            std::string reference;
            std::filesystem::path cacheDirectory;
            std::shared_ptr<std::vector<float>> levels;
            std::atomic<bool> done{false};
        };

        struct PreviewJob {
            SoundSearch::Entry entry;
            std::filesystem::path assetsRoot;
            std::string bytes;
            std::string error;
            std::atomic<bool> done{false};
        };

        void startIndex();
        void runSearch();
        void drawToolbar();
        void drawCards();
        void drawCard(int index, float width, float height);
        void drawMiniWave(const SoundSearch::Entry& entry, float x, float y, float width, float height);
        void select(int index, bool additive);
        void startPreview(const SoundSearch::Entry& entry, double gainDb);
        void stopPreview();
        void updatePreview();
        void beginCardDrag(int index);
        const SoundSearch::Entry* selectedEntry() const;

        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;
        bool searchFocusRequested = false;
        bool focused = false;

        SoundSearch::Scope scope = SoundSearch::Scope::Project;
        std::shared_ptr<IndexJob> indexJob;
        std::vector<SoundSearch::Entry> indexEntries;
        bool indexStale = true;

        char searchText[256] = {0};
        std::string typeFilter;
        SoundSearch::Filters filters;
        std::string similarTo;
        bool resultsStale = true;
        std::vector<SoundSearch::Entry> results;
        size_t totalResults = 0;
        std::string searchError;
        SoundSearch::Hint hint;

        std::set<int> selection;
        int cursor = -1;
        bool scrollToCursor = false;

        std::shared_ptr<PreviewJob> previewJob;
        std::shared_ptr<SoLoud::AudioSource> previewSample;
        double previewGainDb = 0.0;
        unsigned int previewHandle = 0;
        std::string previewPath;

        std::filesystem::path cacheDirectory;
        std::shared_ptr<WaveJob> waveJob;
        std::map<std::string, std::shared_ptr<std::vector<float>>> builtinWaves;
    };

}
