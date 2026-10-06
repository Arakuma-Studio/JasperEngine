// Binary-size probe: nanosvg parse + rasterize at 64 px.
#include "common.h"

#define NANOSVG_IMPLEMENTATION
#include <nanosvg.h>
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvgrast.h>

int main(int argc, char** argv)
{
    std::string data;
    if (!readInput(argc, argv, data)) return 1;
    NSVGimage* image = nsvgParse(&data[0], "px", 96.0f);
    if (!image) return 1;
    std::vector<unsigned char> pixels(64 * 64 * 4);
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    nsvgRasterize(rast, image, 0, 0, 64.0f / image->width, pixels.data(), 64, 64, 64 * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);
    return writeOutput(argv, pixels.data(), pixels.size());
}
