// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "shadercompiler.h"

#include <cstdio>
#include <fstream>
#include <iterator>

using namespace shadercompiler;

bool shadercompiler::load_input(std::vector<input_t>& inputs, const args_t& args){
    for (stage_type_t stage : {STAGE_VERTEX, STAGE_FRAGMENT}){
        const std::string& filename = stage == STAGE_VERTEX ? args.vert_file : args.frag_file;
        if (filename.empty())
            continue;

        std::string buffer;
        if (args.useBuffers){
            auto it = args.fileBuffers.find(filename);
            if (it == args.fileBuffers.end())
                return false;
            buffer = it->second;
        }else{
            std::ifstream file(filename);
            if (!file.is_open()){
                fprintf(stderr, "Unable to open file: %s\n", filename.c_str());
                return false;
            }
            buffer.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }

        inputs.push_back({stage, filename, buffer});
    }

    return true;
}
