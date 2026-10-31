// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef RUNTIMESHADERCOMPILER_H
#define RUNTIMESHADERCOMPILER_H

#include "Export.h"
#include "ShaderBuildTypes.h"

#include <cstddef>
#include <string>
#include <vector>

namespace jasperengine {

    // Builds, in a game, the shader variants the build does not ship precompiled: a
    // mesh assembled by a script, a material or a property (vertex colors, normal map,
    // fog, skinning, instancing...) that only changes at runtime. ShaderPool asks it for
    // a variant once neither the embedded headers nor the shader directory have it.
    //
    // Variants compile on a worker thread (pipeline creation stays on the render
    // thread, in ShaderPool), so what draws with one is left out for the frames the
    // compile takes instead of stalling the game. Without threads (the web build) or
    // with asynchronous compilation turned off they compile on the main thread, one
    // per frame. Every compiled variant is kept in memory for the session and written
    // to a disk cache under the cache directory, keyed by the graphics backend, the
    // engine shader library and compiler, and the variant, so later runs load it
    // instead of compiling.
    //
    // Not used in the editor, which builds through its own ShaderPool builder.
    class JASPERENGINE_API RuntimeShaderCompiler {
    public:
        // The build carries the compiler (JASPERENGINE_RUNTIME_SHADERS) and its shader library
        static bool isSupported();

        // On by default where supported; off, a missing variant is reported and skipped
        static void setEnabled(bool enabled);
        static bool isEnabled();

        // Compile on a worker thread (default) or on the main thread, one per frame
        static void setAsync(bool async);
        static bool isAsync();

        // Main thread. Finished with the variant (from memory, the disk cache or a
        // compile that just ended), Loading while it compiles, Failed if it does not
        // compile (getError tells why). Never NotStarted.
        static ShaderBuildResult request(ShaderKey key);
        static std::string getError(ShaderKey key);

        // variants requested and not compiled yet, and those a compile settled this frame that
        // no request has taken yet (the next frame's draws take them)
        static size_t getPendingCount();

        // Engine frame start: the main-thread compile of the frame, and the file system
        // sync a web build needs to keep what the worker wrote
        static void beginFrame();

        // Stops the worker and forgets every variant (ShaderPool::clear)
        static void shutdown();

        // where the disk cache of this backend and shader library lives
        static std::string getCacheDirectory();
        // built-in variants in the disk cache (forked shaders are left out: their key
        // depends on the session), for ShaderPool::warmUpAllShaders
        static std::vector<ShaderKey> getCachedKeys();
    };

}

#endif // RUNTIMESHADERCOMPILER_H
