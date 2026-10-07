// SVG benchmark: one interface over LunaSVG, ThorVG (software renderer) and nanosvg.
//
// Every backend renders with a uniform scale s = width / intrinsicWidth into a buffer it owns
// (the library's native pixel format), so the timed part is only the library's own work:
// canvas clear + full draw. readback() converts that buffer to RGBA8 premultiplied (the format
// the engine uploads) and is timed separately.
#ifndef HTMLUI_BENCH_BACKEND_H
#define HTMLUI_BENCH_BACKEND_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SvgBackend {
    virtual ~SvgBackend() = default;

    virtual const char* name() const = 0;

    // Parses the document and builds whatever the library needs before the first render
    // (LunaSVG layout, ThorVG scene). Replaces any previously loaded document.
    virtual bool parse(const std::string& svg) = 0;

    // Intrinsic size of the loaded document in CSS px (0 when nothing is loaded).
    virtual float intrinsicWidth() const = 0;
    virtual float intrinsicHeight() const = 0;

    // Prepares a w x h target. Called once per size before render(); not timed.
    virtual bool prepare(int w, int h) = 0;

    // Clears the target and draws the whole document scaled by w / intrinsicWidth.
    // Must redraw everything even when nothing changed since the last call.
    virtual bool render() = 0;

    // Copies the last render to tightly packed RGBA8, premultiplied alpha.
    virtual void readback(std::vector<uint8_t>& rgbaPremultiplied) const = 0;

    // Retained-scene access: sets the fill of the element with this id and re-renders
    // with render(). Returns false when the element is not found or the library cannot
    // modify a loaded document (nanosvg re-parses the modified source text instead).
    virtual bool mutateFill(const std::string& id, uint32_t rgba) = 0;

    // True when mutateFill() changes the loaded document without re-parsing.
    virtual bool mutatesInPlace() const = 0;

    // Frees the document (the target stays).
    virtual void reset() = 0;
};

struct BackendOptions {
    int threads = 0;             // ThorVG task-scheduler threads (0 = single-threaded)
    std::string fontDir;         // fonts for <text> (LunaSVG and ThorVG), empty = none
    bool accessible = false;     // ThorVG Picture::accessible (id map, used for mutation)
};

std::unique_ptr<SvgBackend> createLunaBackend(const BackendOptions& options);
std::unique_ptr<SvgBackend> createThorVGBackend(const BackendOptions& options);
std::unique_ptr<SvgBackend> createNanoSVGBackend(const BackendOptions& options);

// Process-wide setup/teardown for libraries with global state (ThorVG initializer, fonts).
void thorvgGlobalInit(const BackendOptions& options);
void thorvgGlobalTerm();
void lunaGlobalInit(const BackendOptions& options);

#endif // HTMLUI_BENCH_BACKEND_H
