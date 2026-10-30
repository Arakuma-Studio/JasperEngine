// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The scalar image operations of the generator (internal: SpriteNormals.cpp and the tests).
// Float-exact ports of upstream field.rs / distance.rs / components.rs / color.rs: see the
// parity rules of the Phase 2 spec (no FMA contraction, evaluation order, float literals).

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace jasperengine::editor::spritenormals::detail {

    // How a field reads outside itself
    enum class Border { Zero, Wrap, Clamp };

    // A small pool for the work inside one large island (rows, columns, the inflate solver):
    // forRange splits [0, n) into chunks of `grain` run by the pool's threads and the caller, and
    // returns once every chunk ran (an exception of a chunk is rethrown there). Every user writes
    // disjoint outputs per index, or per-index partial sums added in index order afterwards, so
    // the bytes never depend on the number of threads. Not reentrant: one forRange at a time.
    class Workers {
    public:
        explicit Workers(int threads);   // threads - 1 helper threads (none for 1)
        ~Workers();
        Workers(const Workers&) = delete;
        Workers& operator=(const Workers&) = delete;
        int threads() const { return static_cast<int>(helpers.size()) + 1; }
        void forRange(int n, const std::function<void(int begin, int end)>& body, int grain = 1);

    private:
        void helperLoop();
        void runChunks();
        std::vector<std::thread> helpers;
        std::mutex mutex;
        std::condition_variable wake, finished;
        const std::function<void(int, int)>* job = nullptr;
        int jobSize = 0, jobGrain = 1;
        uint64_t serial = 0;
        int checkedIn = 0;
        int active = 0;
        bool stopping = false;
        std::atomic<int> nextChunk{0};
        std::exception_ptr failure;
    };
    // Fields smaller than this run on one thread (the pool's hand-over costs more)
    inline constexpr int64_t kParallelPixels = 65536;
    // forRange on `workers`, or the whole range on this thread without them
    void parallelFor(Workers* workers, int n, const std::function<void(int begin, int end)>& body, int grain = 1);

    struct Field {
        int width = 0;
        int height = 0;
        std::vector<float> data;    // row-major
        static Field zero(int width, int height);
    };

    // sRGB 8-bit -> linear, 256 entries (upstream color.rs)
    const float* srgbToLinearTable();
    // Oklab L of an sRGB colour (upstream lightness), and the full Oklab (L, a, b)
    float oklabL(uint8_t r, uint8_t g, uint8_t b);
    void oklab(uint8_t r, uint8_t g, uint8_t b, float& L, float& a, float& bb);

    // Exact Euclidean distance (pixels, centre to centre) from each mask pixel to the nearest
    // non-mask pixel; 0 on non-mask pixels; +INF where the field has none. Border::Zero: nothing
    // beyond the field (upstream); Wrap: toroidal; Clamp: outside counts as mask (Extend).
    Field edt(const std::vector<uint8_t>& mask, int width, int height, Border border = Border::Zero, Workers* workers = nullptr);

    // Separable Gaussian (upstream: sigma < 0.05 is the identity, radius ceil(3 sigma)) up to
    // kExactGaussianSigma; above, four box passes per axis of the same variance (a convolution
    // of the border-extended field, like the exact one; cost independent of sigma)
    // (`exactUpTo` lowers that limit: the generator's scaled items use 8)
    inline constexpr float kExactGaussianSigma = 16.0f;
    Field gaussian(const Field& input, float sigma, Border border = Border::Zero, Workers* workers = nullptr, float exactUpTo = kExactGaussianSigma);
    // gaussian(field) into field; above kExactGaussianSigma without a second field (the metrics)
    void gaussianInPlace(Field& field, float sigma, Border border = Border::Zero, Workers* workers = nullptr);
    // gaussian(input * weight) / max(gaussian(weight), 1e-8)
    Field maskedGaussian(const Field& input, const Field& weight, float sigma, Border border = Border::Zero, Workers* workers = nullptr,
                         float exactUpTo = kExactGaussianSigma);

    // The inflate profile's field: u solving -Laplacian(u) = 1 (5-point stencil, unit spacing) on
    // the mask pixels and u = 0 on the others; beyond the field Border::Zero holds u = 0, Wrap is
    // periodic, Clamp is a Neumann border (no flux). A conjugate gradient preconditioned by one
    // multigrid V-cycle (the iterate in double; a coarse cell is inside when all its children are), until the residual is 1e-4 of the right-hand side (at most
    // kPoissonMaxIterations: NotConverged, u the last iterate). Singular (u all 0) when no pixel
    // holds u at 0 (Wrap or Clamp with the whole field in the mask: no solution), Cancelled (u all
    // 0) when `cancel` was set meanwhile.
    inline constexpr int kPoissonMaxIterations = 80;
    enum class PoissonStatus { Solved, NotConverged, Singular, Cancelled };
    PoissonStatus poisson(const std::vector<uint8_t>& mask, int width, int height, Border border, std::vector<double>& u, Workers* workers = nullptr,
                 const std::atomic<bool>* cancel = nullptr, int* iterations = nullptr);
    // Normalized Sobel correlation at (x, y): +x slopes up to the right, +y downward. Border::Zero
    // requires (x, y) at least one pixel inside the field.
    void sobel(const Field& input, int x, int y, float& dx, float& dy, Border border = Border::Zero);

    // 8-connected components of `mask` (upstream order)
    struct Component {
        std::vector<int> pixels;    // indices y * width + x, BFS order
        int minX = 0, minY = 0, maxX = 0, maxY = 0;
    };
    std::vector<Component> components(const std::vector<uint8_t>& mask, int width, int height);
    // The same components, in the same order (by their first pixel, row-major), their pixels
    // row-major (two-pass union-find: several times faster on large islands; the generator's
    // results never depend on the order of an island's pixels)
    std::vector<Component> componentsRowMajor(const std::vector<uint8_t>& mask, int width, int height);

    // (128,128,255)-encoding of slopes (upstream slopes_to_normal + byte rounding)
    void encodeNormal(float dx, float dy, float strength, bool directx, uint8_t out[3]);
    // The OpenGL-convention unit normal encodeNormal encodes (the same float operations, before
    // its DirectX flip)
    void slopeNormal(float dx, float dy, float strength, float v[3]);
    // A unit normal (OpenGL convention) to bytes with encodeNormal's rounding; directx negates y
    void encodeUnit(const float v[3], bool directx, uint8_t out[3]);
}
