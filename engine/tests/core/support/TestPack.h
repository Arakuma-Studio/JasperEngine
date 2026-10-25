// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TESTS_TESTPACK_H
#define TESTS_TESTPACK_H

// A resources.pak in the exporter's format (editor/Exporter.cpp writeResourcePack) holding
// one entry, so tests read through ResourcePack as an exported game does.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace testsupport {

    // the packed entry: an asset-relative path and its content
    inline const char* testPackPath(){ return "packed/hello.txt"; }
    inline const char* testPackContent(){ return "hello from the resource pack"; }

    inline void writeTestPack(const char* filename){
        const std::string key = std::string("assets/") + testPackPath();
        std::vector<unsigned char> body(testPackContent(), testPackContent() + std::string(testPackContent()).size());
        const uint8_t xorKey = 0x5A;
        const uint32_t shift = 3;
        // Exporter::obfuscate: rotate right, then XOR
        std::rotate(body.begin(), body.end() - (shift % body.size()), body.end());
        for (unsigned char& byte : body)
            byte ^= xorKey;

        std::vector<unsigned char> out = {'D', 'X', 'P', 'K', '1'};
        auto putU = [&out](uint64_t value, int bytes){
            for (int i = 0; i < bytes; i++)
                out.push_back((unsigned char)((value >> (8 * i)) & 0xff));
        };
        putU(1, 4);
        putU(key.size(), 2);
        out.insert(out.end(), key.begin(), key.end());
        const uint64_t offset = out.size() + 8 + 8 + 1 + 4;
        putU(offset, 8);
        putU(body.size(), 8);
        putU(xorKey, 1);
        putU(shift, 4);
        out.insert(out.end(), body.begin(), body.end());

        if (FILE* file = std::fopen(filename, "wb")){
            std::fwrite(out.data(), 1, out.size(), file);
            std::fclose(file);
        }
    }

}

#endif // TESTS_TESTPACK_H
