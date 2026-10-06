// Shared I/O of the binary-size probes: read the file named by argv[1], write the pixels to
// argv[2] (so the compiler cannot drop the rendering).
#ifndef HTMLUI_BENCH_SIZE_COMMON_H
#define HTMLUI_BENCH_SIZE_COMMON_H

#include <cstdio>
#include <string>
#include <vector>

static bool readInput(int argc, char** argv, std::string& data)
{
    if (argc < 3) return false;
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    std::fclose(f);
    return true;
}

static int writeOutput(char** argv, const void* pixels, size_t bytes)
{
    FILE* f = std::fopen(argv[2], "wb");
    if (!f) return 1;
    std::fwrite(pixels, 1, bytes, f);
    std::fclose(f);
    return 0;
}

#endif
