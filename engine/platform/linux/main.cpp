// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "JasperEngineLinux.h"

#include <cstdio>

int main(int argc, char* argv[]) {
    // The log goes to stdout: line by line even into a pipe (a launcher reading it, a
    // player killed before exit), instead of in 4 KB blocks lost at a crash
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    return JasperEngineLinux::init(argc, argv);
}
