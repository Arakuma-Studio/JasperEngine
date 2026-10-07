// ThorVG build configuration of the engine (meson's generated config.h; ours is hand-written).
// CPU renderer with the Lottie loader and what Lottie files embed (PNG/JPEG images, TTF/OTF
// text). The SVG loader is off: SVG goes through LunaSVG (docs/html-ui/svg-benchmark.md).
// Deliberately not defined: THORVG_THREAD_SUPPORT (no worker threads: deterministic output
// for golden images, no pthreads on Web), THORVG_FILE_IO_SUPPORT (all data comes through the
// engine's Data/ResourcePack), THORVG_LOTTIE_EXPRESSIONS_SUPPORT (needs a JavaScript engine),
// THORVG_PARTIAL_RENDER_SUPPORT, OpenMP, the GL/WG engines, SIMD (AVX2/NEON are compile-time
// choices; the engine targets baseline CPUs), THORVG_LOG_ENABLED.
#define THORVG_VERSION_STRING "1.2.0"
#define THORVG_CPU_ENGINE_SUPPORT 1
#define THORVG_LOTTIE_LOADER_SUPPORT 1
#define THORVG_PNG_LOADER_SUPPORT 1
#define THORVG_JPG_LOADER_SUPPORT 1
#define THORVG_SFNT_LOADER_SUPPORT 1
#define THORVG_TTF_LOADER_SUPPORT 1
#define THORVG_OTF_LOADER_SUPPORT 1
#define WIN32_LEAN_AND_MEAN 1
