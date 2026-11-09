// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PROCMAT_SWEEP_GLPASS_H
#define PROCMAT_SWEEP_GLPASS_H

#include <cstdint>
#include <string>
#include <vector>

// One fullscreen fragment pass on a surfaceless GL 4.1 context, read back as floats: the software
// tier of S90 measured with no window and no engine. Linux and Mesa only, like this tool.
class GlPass {
public:
    struct Timings {
        double compileLinkMs = 0.0;
        double firstDrawMs = 0.0;
        double drawMs = 0.0;
    };

    bool create(std::string& error);
    std::string renderer() const;

    bool render(const std::string& vertex, const std::string& fragment, const std::vector<float>& uniforms, int textureSize,
                const std::vector<float>& textureRgba, int size, std::vector<float>& out, Timings& timings, std::string& error);
};

#endif
