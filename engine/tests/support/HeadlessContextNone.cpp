// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// No headless device here (Windows, a shared engine, macOS on OpenGL): GpuEnvironment::ensure()
// fails with its reason before a GPU suite gets to these, which only let those suites link

#include "HeadlessContext.h"
#include "Readback.h"

namespace jasperengine { namespace test {

    bool HeadlessContext::create(int, int){ return false; }
    void HeadlessContext::destroy(){}
    bool HeadlessContext::isCreated(){ return false; }
    const char* HeadlessContext::getBackendName(){ return "none"; }
    const char* HeadlessContext::getRendererName(){ return ""; }

    bool readTexture(TextureRender&, int, int, std::vector<uint8_t>&){ return false; }
    bool readTexturePixels(TextureRender&, int, int, int, std::vector<uint8_t>&){ return false; }
    bool readTextureRaw(TextureRender&, int, int, ColorFormat, std::vector<uint8_t>&){ return false; }
    bool readTextureFloat(TextureRender&, int, int, std::vector<float>&){ return false; }
    bool finishGpu(){ return false; }

} }
