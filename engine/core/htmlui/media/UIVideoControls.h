// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIVIDEOCONTROLS_H
#define UIVIDEOCONTROLS_H

#include "Export.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Types.h>

#include <string>

namespace Rml {
    class Event;
}

namespace jasperengine {

    class UIVideoElement;

    // The default controls of <video controls> (docs/html-ui/phase-6-video.md §3.10): non-DOM
    // children of the video (not in children, querySelector or the inspected tree), styled by the
    // user-agent stylesheet:
    //
    //   videocontrols.htmlui-video-controls [.htmlui-playing] [.htmlui-muted] [.htmlui-shown]
    //     videobutton.htmlui-video-play
    //     videotime                         "0:03 / 0:10"
    //     videotimeline > videotrack > videoprogress, videothumb
    //     videobutton.htmlui-video-mute
    //
    // The bar shows while the video is paused, hovered or focus-visible, and for 2.5 s of UI time
    // after the pointer moves over it while it plays (htmlui-shown; the sheet fades it).
    // Pointer: the play button and a click on the picture toggle playback, a press on the timeline
    // seeks and dragging it scrubs, the mute button toggles muted. Keys on the focused video (the
    // single focus stop): Space, Enter, K and gamepad A toggle; ArrowLeft/ArrowRight and the
    // gamepad's left/right seek -/+5 s, J/L -/+10 s; Home/End; M mutes. They are consumed (no
    // spatial navigation on left/right); up/down are left to navigation. A listener's
    // preventDefault disables them.
    // (RmlUi gives mousemove no default action and drag only one at its target: the controls
    // listen to them on the video and on the timeline)
    class JASPERENGINE_INTERNAL UIVideoControls : private Rml::EventListener {
    public:
        static constexpr double HIDE_AFTER = 2.5;     // seconds of UI time
        static constexpr double KEY_SEEK = 5.0;
        static constexpr double KEY_SEEK_LONG = 10.0;

        explicit UIVideoControls(UIVideoElement& video);
        ~UIVideoControls();
        UIVideoControls(const UIVideoControls&) = delete;
        UIVideoControls& operator=(const UIVideoControls&) = delete;

        void update();     // states, time text, progress (each update of the video)
        // the video is being destroyed: the listeners go, its children go with it (the bar is not removed)
        void forget();
        void layout();
        // the video's default action for an event on it or on the controls; true when used
        bool processDefaultAction(Rml::Event& event);

        // document px (absolute border boxes), for inspect_ui and tests
        struct Rects {
            bool visible = false;
            Rml::Rectanglef bar, play, time, timeline, mute;
        };
        Rects getRects() const;
        std::string getTimeText() const;
        bool isShown() const;

        // "0:03", "1:02:03" (HTML's media controls; NaN or negative: 0:00)
        static std::string formatTime(double seconds, bool hours);

    private:
        void ProcessEvent(Rml::Event& event) override;
        void detach();
        Rml::Element* make(Rml::Element* parent, const char* tag, const char* className, bool dom);
        bool inside(Rml::Element* element, Rml::Element* container) const;
        void seekToPointer(float x);
        void placeProgress(float fraction);
        void togglePlayback();
        void seekBy(double seconds);

        UIVideoElement& video;
        Rml::Element* bar = nullptr;
        Rml::Element* playButton = nullptr;
        Rml::Element* timeText = nullptr;
        Rml::Element* timeline = nullptr;
        Rml::Element* track = nullptr;
        Rml::Element* progress = nullptr;
        Rml::Element* thumb = nullptr;
        Rml::Element* muteButton = nullptr;
        std::string shownTime;
        float shownFraction = -1.0f;
        double lastPointerMove = -1e9;
        bool scrubbing = false;
        bool shown = false;
    };

}

#endif //UIVIDEOCONTROLS_H
