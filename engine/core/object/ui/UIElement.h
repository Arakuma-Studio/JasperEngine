// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIELEMENT_H
#define UIELEMENT_H

#include "Export.h"
#include "math/Rect.h"
#include "math/Vector3.h"
#include "UIValue.h"
#include "object/ParticleSystem.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// RmlUi stays out of the engine API (swappable core): forward declarations only
namespace Rml {
    class Element;
}

namespace jasperengine{

    class UIEvent;
    class UIItemsView;        // UIItemsView.h (S17)
    struct UIElementHandle;   // UIElementInternal.h: holds Rml::ObserverPtr<Rml::Element>
    struct UIElementAccess;   // UIElementInternal.h: engine-internal access to the RmlUi element

    // getBoundingClientRect, document px
    struct UIRect { float x, y, width, height, top, left, right, bottom; };

    // An effect an element uses (phase 5, getEffects): the property it is in ("background-image",
    // "mask-image", "filter", "backdrop-filter"), its 1-based index there, the effect's name, its
    // form ("paint" or "filter") and every parameter as canonical CSS text (universal ones too)
    struct UIEffectInfo {
        std::string property;
        int index = 0;
        std::string name;
        std::string form;
        std::vector<std::pair<std::string, std::string>> params;
        // phase 6: a stateful effect's simulation (docs/html-ui/phase-6-stateful-effects.md 3.10):
        // its state images' bytes, steps since its reset, its simulation time (s), and whether it
        // is paused (not drawn last frame)
        bool stateful = false;
        size_t stateBytes = 0;
        int64_t steps = 0;
        double simTime = 0.0;
        bool paused = false;
    };

    // An injection into an element's stateful effects (phase 6, UIElement::effectSplat): x, y and
    // radius in the effect area's CSS px, a velocity (dx, dy) in px/s, a CSS colour, a strength;
    // effect names one effect ("" every stateful effect of the element)
    struct UIEffectSplat {
        float x = 0.f, y = 0.f;
        float radius = 24.f;
        float dx = 0.f, dy = 0.f;
        std::string color = "#ffffff";
        float strength = 1.f;
        std::string effect;
        // with a pinned effect clock: the effect time (s) the splat is logged at (< 0: the pinned
        // time), replayed at the step whose interval holds it
        double time = -1.0;
    };

    // Phase-0 members and the phase-1 DOM API are defined in UIElement.cpp (p1-lua), the
    // Lottie control by p1-vector (htmlui/vector/UIElementLottie.cpp). On an invalid handle
    // every C++ method returns a default and logs a warning (once per method).

    // How a tooltip set by setTooltip shows (S17 R15): on a side of the element ("top", "bottom",
    // "left", "right", or "auto": the side with the most room), after `delay` seconds (the
    // document's otherwise), with an arrow pointing at the element; an interactive one stays while
    // the pointer is on it (it takes the pointer)
    struct UITooltipOptions {
        std::string placement = "top";
        std::optional<float> delay;
        bool arrow = false;
        bool interactive = false;
    };

    // HTMLMediaElement's MediaError (phase 6): code 1 aborted, 2 network, 3 decode, 4 source not supported
    struct UIMediaError {
        int code = 0;
        std::string message;
    };

    // An element of an HTML/CSS document (UIDocument), with browser names. A value type holding
    // a weak reference: isValid() is false once the element or its document is gone.
    class JASPERENGINE_API UIElement {
    public:
        UIElement();                                // invalid handle
        bool isValid() const;                       // false once the element or document is gone
        bool operator==(const UIElement&) const;

        // --- phase 0 ---

        std::string getId() const;
        std::string getTagName() const;             // lower case
        std::string getTextContent() const;         // concatenated text of descendants
        void setTextContent(const std::string& text);   // replaces children with escaped text
        std::string getInnerHTML() const;           // RML of the children (HTML after phase 2)
        void setInnerHTML(const std::string& markup);

        bool hasAttribute(const std::string& name) const;
        std::string getAttribute(const std::string& name) const;
        void setAttribute(const std::string& name, const std::string& value);
        void removeAttribute(const std::string& name);

        bool hasClass(const std::string& name) const;
        void setClass(const std::string& name, bool active);
        bool setStyleProperty(const std::string& name, const std::string& value);   // false + warning if RmlUi rejects it
        void removeStyleProperty(const std::string& name);

        Rect getBoundingBox() const;                // border box, canvas px, top-left origin
        bool isPseudoClassSet(const std::string& pseudoClass) const;   // "hover", "active", "focus"

        // --- phase 1 (p1-lua) ---

        // identity / tree

        void setId(const std::string& id);
        std::string getClassName() const; void setClassName(const std::string& className);
        UIElement getParentElement() const;
        std::vector<UIElement> getChildren() const;   // element children only
        UIElement getFirstElementChild() const; UIElement getLastElementChild() const;
        UIElement getNextElementSibling() const; UIElement getPreviousElementSibling() const;
        int getChildElementCount() const;
        UIElement querySelector(const std::string& selector) const;
        std::vector<UIElement> querySelectorAll(const std::string& selector) const;
        UIElement closest(const std::string& selector) const; bool matches(const std::string& selector) const;
        bool contains(const UIElement& other) const;
        // mutation

        UIElement appendChild(const UIElement& child);              // moves; returns child
        UIElement insertBefore(const UIElement& child, const UIElement& reference);
        UIElement removeChild(const UIElement& child);              // child stays usable (detached pool)
        UIElement replaceChild(const UIElement& newChild, const UIElement& oldChild);
        void remove();
        UIElement cloneNode(bool deep) const;
        std::map<std::string, std::string> getAttributes() const;   // name -> value

        // classList / style / dataset

        void addClass(const std::string& name); void removeClass(const std::string& name);
        bool toggleClass(const std::string& name); bool toggleClass(const std::string& name, bool force);
        bool containsClass(const std::string& name) const;
        std::string getStyle(const std::string& property) const;   // computed value as CSS text
        void setStyle(const std::string& property, const std::string& value);   // inline; "" removes
        void removeStyle(const std::string& property);
        std::string getDataset(const std::string& camelKey) const; void setDataset(const std::string& camelKey, const std::string& value);
        // form controls

        std::string getValue() const; void setValue(const std::string& value);
        bool getChecked() const; void setChecked(bool checked); bool getDisabled() const; void setDisabled(bool disabled);
        // focus / interaction / scroll / geometry

        void focus(); void blur(); void click();
        // DOM pointer capture (ui-multitouch): the element gets that pointer's move, up and cancel
        // events until released or the pointer is up; pointerId as event.pointerId (1 the mouse,
        // fingerId + 2 a finger). false when the pointer is not pressed (set) or not captured here (release)
        bool setPointerCapture(int pointerId); bool releasePointerCapture(int pointerId); bool hasPointerCapture(int pointerId) const;
        void scrollIntoView(bool alignToTop = true);
        void scrollTo(float x, float y, bool smooth = false); void scrollBy(float dx, float dy, bool smooth = false);
        float getScrollTop() const; void setScrollTop(float value); float getScrollLeft() const; void setScrollLeft(float value);
        float getScrollWidth() const; float getScrollHeight() const;
        float getClientWidth() const; float getClientHeight() const;
        float getOffsetWidth() const; float getOffsetHeight() const; float getOffsetLeft() const; float getOffsetTop() const;
        UIRect getBoundingClientRect() const;       // border box in document px, through the transforms of the last drawn frame

        // events (C++)

        uint64_t addEventListener(const std::string& type, std::function<void(UIEvent&)> fn, bool capture = false);
        void removeEventListener(uint64_t listenerId);
        bool dispatchEvent(const std::string& type, const UIValue& detail = UIValue(), bool bubbles = true); // false if prevented

        // --- phase 2 (p2-html, hook H5): HTML documents (loaded through the compat layer) ---
        // getTagName gives the DOM tagName there (upper case for HTML: "DIV", <body> -> "BODY");
        // the engine's own data-htmlui-* attributes and htmlui-* classes are not visible to
        // scripts; selectors, innerHTML and style values are HTML/CSS and are rewritten

        std::string getLocalName() const;           // "div", "body", "template" (lower case)
        std::string getOuterHTML() const;
        void setOuterHTML(const std::string& html); // replaces the element (it becomes detached)
        // position: "beforebegin", "afterbegin", "beforeend", "afterend"
        void insertAdjacentHTML(const std::string& position, const std::string& html);
        // <template>: a detached fragment element holding a fresh copy of its content
        // (appendChild/insertBefore of it move its children, like a DocumentFragment)
        UIElement getContent() const;
        // <dialog>: show/showModal set `open` (no modal backdrop), close removes it and dispatches `close`
        void show(); void showModal(); void close(const std::string& returnValue = std::string());
        std::string getCssText() const;             // the inline declarations
        void setCssText(const std::string& css);    // replaces them

        // --- phase 5 (p5-infra): the UI effects the element uses (CSS effect()), in property order ---

        std::vector<UIEffectInfo> getEffects() const;
        // --- phase 6: stateful effects. A splat queued on the element's simulations (consumed by
        // their next step); the number that took it. effectReset restarts them ("" all); how many
        int effectSplat(const UIEffectSplat& splat);
        int effectReset(const std::string& effect = std::string());
        // Agent determinism (MCP set_ui_effect_time): a scripted pointer path for the element's
        // stateful effects ("" all): each point x, y in the element's border-box CSS px and z the
        // effect time s. A pinned clock's replay drags the pointer along it (down from the first
        // point's time to the last's); empty clears it. effectClearSplats drops the queued or
        // logged splats. Both return how many simulations took it
        int effectPointerPath(const std::vector<Vector3>& path, const std::string& effect = std::string());
        int effectClearSplats(const std::string& effect = std::string());

        // --- S17: widgets (htmlui/widgets/UIElementWidgets.cpp) ---

        // <list-view>, <grid-view>, <tree-view>, <table-view>: the view (invalid on other elements)
        UIItemsView getItemsView() const;
        // the content becomes the rendered Markdown (any element; HTML in it is shown as text)
        void setMarkdown(const std::string& markdown);
        // a rich tooltip, in place of the title attribute: a text, a clone of an element, or a
        // text a function gives on each show; clearTooltip removes it
        void setTooltip(const std::string& text, const UITooltipOptions& options = UITooltipOptions());
        void setTooltip(const UIElement& content, const UITooltipOptions& options = UITooltipOptions());
        void setTooltip(std::function<std::string()> text, const UITooltipOptions& options = UITooltipOptions());
        void clearTooltip();

        // --- phase 1 (p1-vector): <lottie> control; logs an error on other elements ---

        void play(); void pause(); void stop(); void seek(float seconds); void setFrame(float frame);
        // <particle-system> (ui-scaling-text): play/pause/stop/isPlaying above, its live particles
        // gone, and the ParticleSystem of its hidden entity (every particle API works). Before the
        // prefab is made (document scripts, onLoad, edit mode) or when its src failed there is none:
        // hasParticleSystem() is false and getParticleSystem() an empty handle (Lua: nil)
        void clear();
        bool hasParticleSystem() const;
        ParticleSystem getParticleSystem() const;
        // text-effect: the effect of this element (or the one its text inherits) from the start
        void restartTextEffect();
        void setSpeed(float speed); void setDirection(int direction); void setLoop(bool loop); void setLoopCount(int count);
        void setSegment(float startFrame, float endFrame); void setMarker(const std::string& marker);
        bool isPlaying() const; float getCurrentFrame() const; float getTotalFrames() const; float getDuration() const;

        // --- phase 6 (p6-video): <video>, HTMLMediaElement's API (htmlui/media/UIElementVideo.cpp);
        // play(), pause(), setLoop(), isPlaying() and getDuration() above serve <video> too. Logs an
        // error on other elements (once per method and tag), as the Lottie control does ---

        // (one declarator each: MSVC refuses the later declarators of one declaration in a dllexport
        // class, C2487)
        static constexpr int HAVE_NOTHING = 0;
        static constexpr int HAVE_METADATA = 1;
        static constexpr int HAVE_CURRENT_DATA = 2;
        static constexpr int HAVE_FUTURE_DATA = 3;
        static constexpr int HAVE_ENOUGH_DATA = 4;
        static constexpr int NETWORK_EMPTY = 0;
        static constexpr int NETWORK_IDLE = 1;
        static constexpr int NETWORK_LOADING = 2;
        static constexpr int NETWORK_NO_SOURCE = 3;
        static constexpr int MEDIA_ERR_ABORTED = 1;
        static constexpr int MEDIA_ERR_NETWORK = 2;
        static constexpr int MEDIA_ERR_DECODE = 3;
        static constexpr int MEDIA_ERR_SRC_NOT_SUPPORTED = 4;

        void load();
        std::string canPlayType(const std::string& type) const;     // "probably", "maybe" or ""
        void fastSeek(double seconds);
        double getCurrentTime() const; void setCurrentTime(double seconds);
        double getMediaDuration() const;       // NaN before the metadata (a <lottie>: its duration)
        bool isPaused() const; bool isEnded() const; bool isSeeking() const;
        bool getLoop() const;                  // (setLoop above; a <lottie>: loops forever)
        bool getAutoplay() const; void setAutoplay(bool autoplay);
        bool getControls() const; void setControls(bool controls);
        bool isMuted() const; void setMuted(bool muted);
        bool getDefaultMuted() const; void setDefaultMuted(bool muted);
        float getVolume() const; void setVolume(float volume);                       // [0, 1]
        double getPlaybackRate() const; void setPlaybackRate(double rate);           // 0 or [1/16, 16]
        double getDefaultPlaybackRate() const; void setDefaultPlaybackRate(double rate);
        bool getPreservesPitch() const; void setPreservesPitch(bool preserve);        // false: the pitch follows the rate
        int getReadyState() const; int getNetworkState() const;
        int getVideoWidth() const; int getVideoHeight() const;
        std::string getCurrentSrc() const;
        // what decodes it: "webm" (AV1/Opus in software), "ffmpeg", "mediacodec", "avfoundation",
        // "mediafoundation", "browser" ("" before a source opened)
        std::string getVideoBackend() const;
        std::string getPoster() const; void setPoster(const std::string& poster);
        std::string getPreload() const; void setPreload(const std::string& preload);
        std::optional<UIMediaError> getMediaError() const;

    private:
        friend class UIDocumentSystem;
        friend struct UIElementAccess;
        // a weak reference to element (invalid UIElement for nullptr)
        static UIElement wrap(Rml::Element* element);
        Rml::Element* get() const;   // nullptr once invalid

        std::shared_ptr<UIElementHandle> handle;   // phase 0's handle, plus p1-lua's detached-pool token
    };

}

#endif //UIELEMENT_H
