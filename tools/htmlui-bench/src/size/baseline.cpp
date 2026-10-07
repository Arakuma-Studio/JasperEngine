// Binary-size baseline: same I/O as the library probes, no SVG library.
#include "common.h"

int main(int argc, char** argv)
{
    std::string data;
    if (!readInput(argc, argv, data)) return 1;
    std::vector<unsigned char> pixels(64 * 64 * 4, (unsigned char)data.size());
    return writeOutput(argv, pixels.data(), pixels.size());
}
