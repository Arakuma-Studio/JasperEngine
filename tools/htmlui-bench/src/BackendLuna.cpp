// LunaSVG backend: Document::loadFromData + updateLayout, Document::render into a reused
// ARGB32-premultiplied Bitmap, Element::setAttribute for mutation.
#include "Backend.h"

#include <lunasvg.h>

#include <cmath>
#include <cstdio>

namespace {

class LunaBackend final : public SvgBackend {
public:
    const char* name() const override { return "luna"; }

    bool parse(const std::string& svg) override
    {
        doc = lunasvg::Document::loadFromData(svg.data(), svg.size());
        if (!doc) return false;
        // Layout is lazy in LunaSVG; force it so parse() ends with a render-ready document,
        // like ThorVG's loader which builds its scene during load.
        doc->updateLayout();
        return true;
    }

    float intrinsicWidth() const override { return doc ? doc->width() : 0.0f; }
    float intrinsicHeight() const override { return doc ? doc->height() : 0.0f; }

    bool prepare(int w, int h) override
    {
        if (bitmap.isNull() || bitmap.width() != w || bitmap.height() != h) bitmap = lunasvg::Bitmap(w, h);
        return !bitmap.isNull();
    }

    bool render() override
    {
        if (!doc || bitmap.isNull()) return false;
        float s = bitmap.width() / doc->width();
        bitmap.clear(0x00000000);
        doc->render(bitmap, lunasvg::Matrix::scaled(s, s));
        return true;
    }

    void readback(std::vector<uint8_t>& out) const override
    {
        int w = bitmap.width(), h = bitmap.height(), stride = bitmap.stride();
        out.resize(size_t(w) * h * 4);
        const uint8_t* src = bitmap.data();
        uint8_t* dst = out.data();
        // ARGB32 premultiplied in native byte order (B, G, R, A in memory on little endian).
        for (int y = 0; y < h; y++) {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(src + size_t(y) * stride);
            for (int x = 0; x < w; x++) {
                uint32_t p = row[x];
                *dst++ = uint8_t(p >> 16);
                *dst++ = uint8_t(p >> 8);
                *dst++ = uint8_t(p);
                *dst++ = uint8_t(p >> 24);
            }
        }
    }

    bool mutateFill(const std::string& id, uint32_t rgba) override
    {
        if (!doc) return false;
        auto element = doc->getElementById(id);
        if (!element) return false;
        char value[16];
        std::snprintf(value, sizeof(value), "#%06x", unsigned(rgba >> 8));
        element.setAttribute("fill", value);
        return render();
    }

    bool mutatesInPlace() const override { return true; }

    void reset() override { doc.reset(); }

private:
    std::unique_ptr<lunasvg::Document> doc;
    lunasvg::Bitmap bitmap;
};

} // namespace

void lunaGlobalInit(const BackendOptions& options)
{
    if (options.fontDir.empty()) return;
    // The resvg test suite asks for these families; LunaSVG also scans system fonts.
    struct Face { const char* family; bool bold; bool italic; const char* file; };
    static const Face faces[] = {
        {"Noto Sans", false, false, "NotoSans-Regular.ttf"},
        {"Noto Sans", true, false, "NotoSans-Bold.ttf"},
        {"Noto Sans", false, true, "NotoSans-Italic.ttf"},
        {"Noto Serif", false, false, "NotoSerif-Regular.ttf"},
        {"Noto Mono", false, false, "NotoMono-Regular.ttf"},
        {"Source Sans Pro", false, false, "SourceSansPro-Regular.ttf"},
        {"serif", false, false, "NotoSerif-Regular.ttf"},
        {"sans-serif", false, false, "NotoSans-Regular.ttf"},
        {"monospace", false, false, "NotoMono-Regular.ttf"},
    };
    for (const Face& f : faces) {
        std::string path = options.fontDir + "/" + f.file;
        lunasvg_add_font_face_from_file(f.family, f.bold, f.italic, path.c_str());
    }
}

std::unique_ptr<SvgBackend> createLunaBackend(const BackendOptions&)
{
    return std::make_unique<LunaBackend>();
}
