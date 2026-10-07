// nanosvg backend (the engine's copy, engine/libs/nanosvg): nsvgParse on a copy of the text
// (it parses in place), nsvgRasterize into straight-alpha RGBA. Mutation = edit the source
// text and parse again (nanosvg has no document API).
#include "Backend.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#define NANOSVG_IMPLEMENTATION
#include <nanosvg.h>
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvgrast.h>

namespace {

class NanoSVGBackend final : public SvgBackend {
public:
    NanoSVGBackend() { rast = nsvgCreateRasterizer(); }

    ~NanoSVGBackend() override
    {
        reset();
        nsvgDeleteRasterizer(rast);
    }

    const char* name() const override { return "nanosvg"; }

    bool parse(const std::string& svg) override
    {
        reset();
        source = svg;
        return parseText(source);
    }

    float intrinsicWidth() const override { return image ? image->width : 0.0f; }
    float intrinsicHeight() const override { return image ? image->height : 0.0f; }

    bool prepare(int w, int h) override
    {
        tw = w;
        th = h;
        pixels.assign(size_t(w) * h * 4, 0);
        return image != nullptr;
    }

    bool render() override
    {
        if (!image || tw <= 0) return false;
        // nsvgRasterize clears the destination itself.
        nsvgRasterize(rast, image, 0.0f, 0.0f, tw / image->width, pixels.data(), tw, th, tw * 4);
        return true;
    }

    void readback(std::vector<uint8_t>& out) const override
    {
        out.resize(pixels.size());
        // nanosvg un-premultiplies its output; premultiply back.
        for (size_t i = 0; i < pixels.size(); i += 4) {
            unsigned a = pixels[i + 3];
            out[i + 0] = uint8_t((pixels[i + 0] * a + 127) / 255);
            out[i + 1] = uint8_t((pixels[i + 1] * a + 127) / 255);
            out[i + 2] = uint8_t((pixels[i + 2] * a + 127) / 255);
            out[i + 3] = uint8_t(a);
        }
    }

    bool mutateFill(const std::string& id, uint32_t rgba) override
    {
        if (!image) return false;
        // Append fill="..." at the end of the element's start tag (attributes are applied in
        // order, so it wins over an existing fill), then parse the whole document again.
        std::string key = "id=\"" + id + "\"";
        size_t at = source.find(key);
        if (at == std::string::npos) return false;
        size_t end = source.find('>', at);
        if (end == std::string::npos) return false;
        if (end > 0 && source[end - 1] == '/') end--;
        char attr[32];
        std::snprintf(attr, sizeof(attr), " fill=\"#%06x\"", unsigned(rgba >> 8));
        std::string text = source;
        text.insert(end, attr);
        nsvgDelete(image);
        image = nullptr;
        return parseText(text) && render();
    }

    bool mutatesInPlace() const override { return false; }

    void reset() override
    {
        if (image) nsvgDelete(image);
        image = nullptr;
    }

private:
    bool parseText(const std::string& text)
    {
        scratch.assign(text.begin(), text.end());
        scratch.push_back('\0');
        image = nsvgParse(scratch.data(), "px", 96.0f);
        return image && image->width > 0.0f && image->height > 0.0f;
    }

    NSVGrasterizer* rast = nullptr;
    NSVGimage* image = nullptr;
    std::string source; // the unmodified document; mutations start from it
    std::vector<char> scratch;
    std::vector<uint8_t> pixels;
    int tw = 0, th = 0;
};

} // namespace

std::unique_ptr<SvgBackend> createNanoSVGBackend(const BackendOptions&)
{
    return std::make_unique<NanoSVGBackend>();
}
