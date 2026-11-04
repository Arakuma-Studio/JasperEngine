// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PaletteQuantize.h"

#include "Log.h"
#include "jobs/JobSystem.h"

#include <algorithm>
#include <chrono>
#include <map>

using namespace jasperengine;
using namespace jasperengine::pixelart;

// file-local, inside the namespace: a unity build's other sources never see these names
namespace jasperengine::pixelart {
namespace {

    int quantizeCellOf(Rgba8 color, int cellsPerAxis) {
        const int shift = cellsPerAxis == 64 ? 2 : 3;
        const int r = color.r >> shift, g = color.g >> shift, b = color.b >> shift;
        return r + cellsPerAxis * (g + cellsPerAxis * b);
    }

    size_t quantizeTexel(int r, int g, int b, int cellsPerAxis, int width) {
        const int x = r + cellsPerAxis * (b % 8);
        const int y = g + cellsPerAxis * (b / 8);
        return ((size_t)y * (size_t)width + (size_t)x) * 4;
    }

    std::vector<uint16_t> visibleIndices(const Palette& palette) {
        std::vector<uint16_t> indices;
        for (int i = 0; i < palette.count() && i < Palette::kMaxColors; i++) {
            if (palette.colors[i].a != 0) indices.push_back((uint16_t)i);
        }
        return indices;
    }

}
}

Rgba8 QuantizeLut::pick(Rgba8 display, int ditherSize, int x, int y) const {
    if (cells.empty() || row.size() < 256 * 4) return display;
    const int shift = cellsPerAxis == 64 ? 2 : 3;
    const size_t at = quantizeTexel(display.r >> shift, display.g >> shift, display.b >> shift, cellsPerAxis, width);
    int index = cells[at];
    if (cells[at + 2] > 0 && ditherSize > 0 && bayerThreshold(ditherSize, x, y) < cells[at + 2] / (float)(ditherSize * ditherSize))
        index = cells[at + 1];
    return Rgba8{row[index * 4], row[index * 4 + 1], row[index * 4 + 2], display.a};
}

QuantizeLutJob::QuantizeLutJob(const Palette& palette, ColorMetric metric, int ditherSize, float ditherStrength, int cellsPerAxis)
    : matcher(palette.colors, metric, visibleIndices(palette)) {
    lut.cellsPerAxis = cellsPerAxis == 64 ? 64 : 32;
    lut.width = lut.cellsPerAxis * 8;
    lut.height = lut.cellsPerAxis * lut.cellsPerAxis / 8;
    lut.cells.assign((size_t)lut.width * lut.height * 4, 0);
    lut.row.assign(256 * 4, 0);
    for (int i = 0; i < palette.count() && i < 256; i++) {
        lut.row[i * 4] = palette.colors[i].r;
        lut.row[i * 4 + 1] = palette.colors[i].g;
        lut.row[i * 4 + 2] = palette.colors[i].b;
        lut.row[i * 4 + 3] = 255;
    }
    strength = std::clamp(ditherStrength, 0.0f, 1.0f);
    dither = ditherSize > 0 && strength > 0.0f;
    steps = ditherSize >= 8 ? 64 : (ditherSize >= 4 ? 16 : 4);
    total = lut.cellsPerAxis * lut.cellsPerAxis * lut.cellsPerAxis;

    // the identity: a cell holding a palette colour is that colour; two in one cell keep the first
    identity.assign((size_t)total, -1);
    std::map<int, std::vector<int>> shared;
    for (uint16_t index : matcher.candidates()) {
        const int cell = quantizeCellOf(palette.colors[index], lut.cellsPerAxis);
        if (identity[cell] < 0) {
            identity[cell] = index;
        } else if (palette.colors[identity[cell]].r != palette.colors[index].r || palette.colors[identity[cell]].g != palette.colors[index].g ||
                   palette.colors[identity[cell]].b != palette.colors[index].b) {
            std::vector<int>& members = shared[cell];
            if (members.empty()) members.push_back(identity[cell]);
            members.push_back(index);
        }
    }
    for (const auto& [cell, members] : shared) {
        std::string list;
        for (size_t m = 0; m < members.size(); m++) list += (m ? ", " : "") + toHex(palette.colors[members[m]]);
        lut.warnings.push_back({Diagnostic::Severity::Warning, "palette_cell_shared", "palette_precision",
                                "these palette colours share one cell of the table, so only the first passes through unchanged: " + list,
                                "set Precision to High, or move the colours apart"});
    }
    if (matcher.candidates().empty()) finished = true;
}

void QuantizeLutJob::bakeRange(int begin, int end) {
    const int n = lut.cellsPerAxis;
    const int span = 256 / n;
    for (int cell = begin; cell < end; cell++) {
        const int r = cell % n, g = (cell / n) % n, b = cell / (n * n);
        // the display colour at the cell's centre
        const Rgba8 centre{(uint8_t)(r * span + span / 2), (uint8_t)(g * span + span / 2), (uint8_t)(b * span + span / 2), 255};
        PaletteMatcher::Mix mix;
        if (identity[cell] >= 0) {
            mix.a = mix.b = (uint16_t)identity[cell];
        } else if (dither) {
            mix = matcher.ditherPair(centre, strength, steps);
        } else {
            mix.a = mix.b = (uint16_t)matcher.nearestUncached(centre).first;
        }
        const size_t at = quantizeTexel(r, g, b, n, lut.width);
        lut.cells[at] = (uint8_t)mix.a;
        lut.cells[at + 1] = (uint8_t)mix.b;
        lut.cells[at + 2] = mix.ratio16;
        lut.cells[at + 3] = 255;
    }
}

bool QuantizeLutJob::step(double budgetSeconds) {
    if (finished) return true;
    const auto start = std::chrono::steady_clock::now();
    while (next < total) {
        const int end = std::min(total, next + 256);
        bakeRange(next, end);
        next = end;
        if (budgetSeconds > 0.0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= budgetSeconds)
            return next >= total ? (finished = true) : false;
    }
    finished = true;
    return true;
}

void QuantizeLutJob::bakeParallel() {
    if (finished) return;
    jobs::parallelForChunked((size_t)next, (size_t)total, 1024, [this](size_t begin, size_t end) { bakeRange((int)begin, (int)end); });
    next = total;
    finished = true;
}

float QuantizeLutJob::progress() const {
    return total > 0 ? (float)next / (float)total : 1.0f;
}

std::unique_ptr<QuantizeLutJob> pixelart::startQuantizeBake(const Palette& palette, ColorMetric metric, int ditherSize, float strength,
                                                            int cellsPerAxis) {
    return std::make_unique<QuantizeLutJob>(palette, metric, ditherSize, strength, cellsPerAxis);
}

QuantizeLut pixelart::bakeQuantizeLut(const Palette& palette, ColorMetric metric, int ditherSize, float strength, int cellsPerAxis) {
    QuantizeLutJob job(palette, metric, ditherSize, strength, cellsPerAxis);
    job.step(0.0);
    return job.result();
}

PaletteQuantizer::~PaletteQuantizer() {
    release();
}

void PaletteQuantizer::release() {
    bake.reset();
    started = false;
    cellsTexture.destroyTexture();
    rowTexture.destroyTexture();
    hasTextures = false;
}

void PaletteQuantizer::adopt(const QuantizeLut& lut, int ditherSize) {
    for (const Diagnostic& warning : lut.warnings) Log::warn("Palette Quantize: %s", warning.message.c_str());
    std::vector<uint8_t> cells = lut.cells;
    std::vector<uint8_t> row = lut.row;
    void* cellsData[1] = {cells.data()};
    size_t cellsSize[1] = {cells.size()};
    void* rowData[1] = {row.data()};
    size_t rowSize[1] = {row.size()};
    cellsTexture.destroyTexture();
    rowTexture.destroyTexture();
    const bool made = cellsTexture.createTexture("palette quantize cells", lut.width, lut.height, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1,
                                                 cellsData, cellsSize, TextureFilter::NEAREST, TextureFilter::NEAREST,
                                                 TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE) &&
                      rowTexture.createTexture("palette quantize row", 256, 1, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, rowData, rowSize,
                                               TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE);
    hasTextures = made;
    readyCells = lut.cellsPerAxis;
    readyDither = ditherSize;
}

void PaletteQuantizer::update(const Request& request) {
    std::vector<Diagnostic> diagnostics;
    std::shared_ptr<const Palette> palette = request.palette.empty() ? nullptr : PaletteLibrary::get(request.palette, &diagnostics);
    const uint32_t version = PaletteLibrary::version(request.palette);
    if (!started || !(request == current) || version != currentVersion) {
        current = request;
        currentVersion = version;
        started = true;
        bake.reset();
        if (!palette) {
            for (const Diagnostic& diagnostic : diagnostics) {
                if (diagnostic.severity == Diagnostic::Severity::Error)
                    Log::warn("Palette Quantize: %s", diagnostic.message.c_str());
            }
            cellsTexture.destroyTexture();
            rowTexture.destroyTexture();
            hasTextures = false;
            return;
        }
        auto next = std::make_shared<Bake>();
        next->job = startQuantizeBake(*palette, request.metric, request.ditherSize, request.ditherSize > 0 ? request.strength : 0.0f,
                                      request.cellsPerAxis);
        bake = next;
        bakeDither = request.ditherSize;
        if (jobs::system().getWorkerCount() > 0) {
            // one job: the result does not depend on the thread count
            std::weak_ptr<Bake> weak = next;
            jobs::enqueueDetached([weak]() {
                if (std::shared_ptr<Bake> running = weak.lock()) {
                    running->job->bakeParallel();
                    running->done.store(true, std::memory_order_release);
                }
            }, jobs::Lane::Background, "palette-quantize");
        }
    }
    if (!bake) return;
    // without workers (the web build without threads): 8 ms of it a frame
    if (jobs::system().getWorkerCount() == 0 && !bake->done.load(std::memory_order_acquire)) {
        if (bake->job->step(0.008)) bake->done.store(true, std::memory_order_release);
    }
    if (bake->done.load(std::memory_order_acquire)) {
        adopt(bake->job->result(), bakeDither);
        bake.reset();
    }
}
