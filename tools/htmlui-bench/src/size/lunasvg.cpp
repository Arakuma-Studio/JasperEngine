// Binary-size probe: LunaSVG load + render at 64 px.
#include "common.h"

#include <lunasvg.h>

int main(int argc, char** argv)
{
    std::string data;
    if (!readInput(argc, argv, data)) return 1;
    auto doc = lunasvg::Document::loadFromData(data);
    if (!doc) return 1;
    lunasvg::Bitmap bitmap = doc->renderToBitmap(64, 64);
    return writeOutput(argv, bitmap.data(), size_t(bitmap.stride()) * bitmap.height());
}
