// Binary-size probe: ThorVG (software canvas) load + render at 64 px.
#include "common.h"

#include <thorvg.h>

int main(int argc, char** argv)
{
    std::string data;
    if (!readInput(argc, argv, data)) return 1;
    tvg::Initializer::init(0);
    std::vector<uint32_t> pixels(64 * 64);
    auto canvas = tvg::SwCanvas::gen();
    canvas->target(pixels.data(), 64, 64, 64, tvg::ColorSpace::ABGR8888);
    auto picture = tvg::Picture::gen();
    if (picture->load(data.data(), uint32_t(data.size()), "svg", nullptr, true) != tvg::Result::Success) return 1;
    picture->size(64, 64);
    canvas->add(picture);
    canvas->draw(true);
    canvas->sync();
    delete canvas;
    tvg::Initializer::term();
    return writeOutput(argv, pixels.data(), pixels.size() * 4);
}
