// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AUDIOCAPTURESWINDOW_H
#define AUDIOCAPTURESWINDOW_H

#include "audio/AudioAnalysis.h"
#include "texture/Texture.h"

#include "json.hpp"

#include <memory>
#include <string>
#include <vector>

namespace SoLoud {
    class Wav;
}

namespace jasperengine::editor {

    class Project;

    // Window > Audio > Captures (docs/specs/S35-agent-audio.md R30): a person's view of the sessions a
    // Play captured, with the moments an agent's check asked them to listen to.
    class AudioCapturesWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Audio Captures";

        explicit AudioCapturesWindow(Project* project);

        void show();
        void open();
        bool isOpen() const;
        bool isFocused() const;
        void resetForProjectSwitch();

        // the commands (context AudioCaptures)
        void playMoment();
        void stopMoment();
        void selectMoment(int direction);

    private:
        struct RenderJob;
        struct AudioJob;

        void refreshSessions();
        void selectSession(int index);
        void requestRender();
        void pollJobs();
        void drawSessions();
        void drawTimeline();
        void drawChecks();
        void drawListen();
        void startMoment(double seconds);

        Project* project;
        bool windowOpen = false;
        bool focused = false;

        std::vector<AudioAnalysis::Capture> sessions;
        std::vector<std::string> verdicts;
        double refreshedAt = -1000.0;
        int selected = -1;

        nlohmann::json check;
        int moment = -1;

        double sessionSeconds = 0.0;
        double viewStart = 0.0;
        double viewEnd = 0.0;
        int renderWidth = 0;
        double renderedStart = -1.0;
        double renderedEnd = -1.0;
        int renderedWidth = 0;
        std::shared_ptr<RenderJob> renderJob;
        bool renderAgain = false;
        Texture timeline;
        std::shared_ptr<RenderJob> timelinePixels;
        std::vector<Texture> retired;
        int textureGeneration = 0;
        int timelineWidth = 0;
        int timelineHeight = 0;

        std::shared_ptr<AudioJob> audioJob;
        double pendingMoment = -1.0;
        std::string loadedSession;
        std::shared_ptr<SoLoud::Wav> wav;
        std::shared_ptr<std::string> wavBytes;
        unsigned int previewHandle = 0;
    };

}

#endif //AUDIOCAPTURESWINDOW_H
