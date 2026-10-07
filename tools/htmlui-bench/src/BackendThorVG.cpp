// ThorVG backend (software renderer): Picture::load(copy) builds the scene during load,
// SwCanvas renders into an ABGR8888 (R, G, B, A bytes on little endian, premultiplied) buffer.
// Mutation goes through Picture::paint(Accessor::id(id)) -> Shape::fill.
#include "Backend.h"

#include <thorvg.h>

#include <cstring>
#include <dirent.h>

namespace {

class ThorVGBackend final : public SvgBackend {
public:
    explicit ThorVGBackend(const BackendOptions& options) : accessible(options.accessible)
    {
        canvas = tvg::SwCanvas::gen(tvg::EngineOption::Default);
    }

    ~ThorVGBackend() override
    {
        reset();
        delete canvas;
    }

    const char* name() const override { return "thorvg"; }

    bool parse(const std::string& svg) override
    {
        reset();
        picture = tvg::Picture::gen();
        picture->accessible = accessible;
        // copy = true: with copy = false ThorVG caches the parsed document keyed by the data
        // address, so re-loading the same buffer would not parse again.
        if (picture->load(svg.data(), uint32_t(svg.size()), "svg", nullptr, true) != tvg::Result::Success) {
            tvg::Paint::rel(picture);
            picture = nullptr;
            return false;
        }
        picture->size(&iw, &ih);
        return iw > 0.0f && ih > 0.0f;
    }

    float intrinsicWidth() const override { return picture ? iw : 0.0f; }
    float intrinsicHeight() const override { return picture ? ih : 0.0f; }

    bool prepare(int w, int h) override
    {
        if (!picture) return false;
        if (w != tw || h != th) {
            buffer.assign(size_t(w) * h, 0);
            if (canvas->target(buffer.data(), uint32_t(w), uint32_t(w), uint32_t(h), tvg::ColorSpace::ABGR8888) != tvg::Result::Success) return false;
            tw = w;
            th = h;
        }
        float s = w / iw;
        picture->size(iw * s, ih * s);
        if (!added) {
            if (canvas->add(picture) != tvg::Result::Success) return false;
            added = true;
        }
        return true;
    }

    bool render() override
    {
        if (!picture || !added) return false;
        // Without a change ThorVG reuses the shapes' cached outlines/spans from the previous
        // frame (a no-change redraw is just compositing). Marking the picture's transform
        // dirty makes every shape re-transform, re-stroke and re-rasterize, which is the cost
        // of drawing the document for the first time at this size.
#ifndef THORVG_BENCH_CACHED_REDRAW // only for checking the method (docs/html-ui/svg-benchmark.md)
        picture->transform(identity);
#endif
        return draw();
    }

    void readback(std::vector<uint8_t>& out) const override
    {
        out.resize(buffer.size() * 4);
        std::memcpy(out.data(), buffer.data(), out.size());
    }

    bool mutateFill(const std::string& id, uint32_t rgba) override
    {
        if (!picture || !added) return false;
        auto paint = const_cast<tvg::Paint*>(picture->paint(tvg::Accessor::id(id.c_str())));
        if (!paint || paint->type() != tvg::Type::Shape) return false;
        auto shape = static_cast<tvg::Shape*>(paint);
        uint8_t alpha = 0;
        shape->fill(nullptr, nullptr, nullptr, &alpha);
        bool hadFill = alpha > 0 || shape->fill() != nullptr;
        shape->fill(uint8_t(rgba >> 24), uint8_t(rgba >> 16), uint8_t(rgba >> 8), uint8_t(rgba));
        // The software renderer builds a shape's fill spans only when it prepares the geometry
        // with a visible fill, so a colour change alone does not draw anything on a shape that
        // had no fill (every stroke-only icon path): invalidate its geometry too.
        if (!hadFill) shape->transform(shape->transform());
        // Picture::update skips the whole picture unless the picture itself is dirty, so a
        // change marked on an inner shape is never seen. Toggling the opacity is the cheapest
        // public way to dirty it: the other shapes only get a colour update (no re-raster),
        // the changed shape is re-prepared. That is the retained-scene benefit.
        picture->opacity(254);
        picture->opacity(255);
        return draw();
    }

    bool mutatesInPlace() const override { return true; }

    void reset() override
    {
        if (!picture) return;
        if (added) canvas->remove(picture); // releases the picture
        else tvg::Paint::rel(picture);
        picture = nullptr;
        added = false;
    }

private:
    bool draw()
    {
        if (canvas->update() != tvg::Result::Success) return false;
        if (canvas->draw(true) != tvg::Result::Success) return false;
        return canvas->sync() == tvg::Result::Success;
    }

    const tvg::Matrix identity = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    bool accessible;
    tvg::SwCanvas* canvas = nullptr;
    tvg::Picture* picture = nullptr;
    bool added = false;
    float iw = 0.0f, ih = 0.0f;
    int tw = 0, th = 0;
    std::vector<uint32_t> buffer;
};

} // namespace

void thorvgGlobalInit(const BackendOptions& options)
{
    tvg::Initializer::init(uint32_t(options.threads));
    if (options.fontDir.empty()) return;
    // ThorVG names a font after its file; SVG text falls back to any loaded font.
    if (DIR* dir = opendir(options.fontDir.c_str())) {
        while (dirent* e = readdir(dir)) {
            std::string n = e->d_name;
            if (n.size() > 4 && (n.compare(n.size() - 4, 4, ".ttf") == 0 || n.compare(n.size() - 4, 4, ".otf") == 0)) {
                tvg::Text::load((options.fontDir + "/" + n).c_str());
            }
        }
        closedir(dir);
    }
}

void thorvgGlobalTerm()
{
    tvg::Initializer::term();
}

std::unique_ptr<SvgBackend> createThorVGBackend(const BackendOptions& options)
{
    return std::make_unique<ThorVGBackend>(options);
}
