// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "json.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class CommandLine {
    public:
        // Runs the project export subcommand.
        //
        // `argc` and `argv` should already be shifted past the `export`
        // subcommand token (i.e. argv[0] is the subcommand itself).
        // `executableName` is used only for help and error output.
        static int runExportCommand(int argc, char** argv, const char* executableName);

        // Runs the standalone shader generation subcommand.
        static int runShadersCommand(int argc, char** argv, const char* executableName);

        // Developer check: dumps every scene's GameObjects of a project (see CommandLine.cpp)
        static int runSceneDumpCommand(int argc, char** argv, const char* executableName);

        // Asset identity: assets check (missing references, JSON) and assets migrate (GUIDs written)
        static int runAssetsCommand(int argc, char** argv, const char* executableName);

        static int runBenchmarkHelp(const char* executableName);

        // Cooks a project's textures into its texture cache and reports them, or decodes a KTX2
        // to PNG (editor/texture/TextureCli.h)
        static int runCookTexturesCommand(int argc, char** argv, const char* executableName);

        // Checks a project's Lua scripts, shader forks, C++ scripts and HTML/CSS UI files (see docs/mcp/checks.md)
        static int runCheckCommand(int argc, char** argv, const char* executableName);
        // procedural-material bake when no editor could start (S90 R39): bake_procedural_material's arguments
        // (`toolArgs`) baked in this process on the CPU tier, no window, no device. 0 baked, 1 failed.
        static int bakeProceduralMaterialOnCpu(int argc, char** argv, const std::filesystem::path& project, const nlohmann::json& toolArgs,
                                               const std::string& commandName);

        // Prints the HTML/CSS compatibility support tables as JSON (developer: docs generation)
        static int runUiSupportCommand(int argc, char** argv, const char* executableName);

        // Lists the HTML UI effects (built-in and a project's), as text or JSON; --validate compiles them
        static int runUiEffectsCommand(int argc, char** argv, const char* executableName);

        // Imports the remote resources of HTML UI documents (editor/htmlui/HtmlUiImportCli.h)
        static int runImportUiCommand(int argc, char** argv, const char* executableName);

        // Synthesizes a texture from example images, no window (editor/texsynth/SynthesisCli.h)
        static int runSynthesizeTextureCommand(int argc, char** argv, const char* executableName);

        // Places GameObjects with an edit-time physics simulation, no window (editor/physics/PlacementCli.h)
        static int runPlacePhysicsCommand(int argc, char** argv, const char* executableName);

        // Bakes or imports a Vertex Animation (.vat + .vat.glb), no window (editor/vat/VatCli.h)
        static int runBakeVatCommand(int argc, char** argv, const char* executableName);

        // Imports Unity Particle Systems (.prefab / .unity) as engine prefabs, no window (editor/unity/UnityParticleImport.h)
        static int runImportUnityParticlesCommand(int argc, char** argv, const char* executableName);

        // Generates sprite normal maps (sprites, sheets, GIF/APNG, tiles), no window (editor/spritenormals/SpriteNormalsCli.h)
        static int runGenerateSpriteNormalsCommand(int argc, char** argv, const char* executableName);

        // Prints the top level usage, or reports an unrecognized argument.
        static int runHelpCommand(int argc, char** argv, const char* executableName);

        // The working directory the editor was started in, read once (main() reads it first,
        // before anything runs). Relative path arguments are the caller's: relative to it.
        static const std::filesystem::path& invocationDirectory();

        // A path argument (--out, --ops, --doc, ...) as an absolute, normalized path: a relative
        // one is relative to invocationDirectory(); symlinks and "." / ".." resolved, no trailing
        // separator. Empty stays empty.
        static std::filesystem::path absoluteArgPath(const std::filesystem::path& path);

        // A --project argument (a project directory or its project.yaml) as the absolute,
        // normalized project directory (absoluteArgPath)
        static std::filesystem::path projectArgPath(const std::filesystem::path& path);

        // The profiler's options (--profile, --profile-capture <path>, --profile-frames <n>, also
        // with "="), anywhere on the command line: main() removes them before the subcommands
        // parse theirs, and the engine gets them at start (App::engineInit); the capture path is
        // made absolute. False with a message on stderr for a flag without its value.
        static bool takeProfilerFlags(std::vector<char*>& args);
        static const std::vector<std::string>& takenProfilerFlags();
    };

}
