// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The `title-library` GPU subcommands' environment for the headless editors they run (S84 Deviation 31).

#include <gtest/gtest.h>

#include "titleart/library/TitleLibraryCliTools.h"

#include <filesystem>

using namespace jasperengine::editor::titlelibrary;
namespace fs = std::filesystem;

// S84 Deviation 31: the GPU subcommands' editors keep llvmpipe's compiled shaders under HOME (Mesa
// writes them under the passwd entry's home, which a container's HOME override does not reach)
TEST(TitleLibraryCli, MesaShaderCacheFollowsHome) {
    EXPECT_EQ(fs::path(mesaShaderCacheDir(nullptr, nullptr, "/src/build/.cache/home")), fs::path("/src/build/.cache/home/.cache/mesa_shader_cache"));
    EXPECT_EQ(mesaShaderCacheDir("/shared/mesa", nullptr, "/home/u"), "") << "the caller's MESA_SHADER_CACHE_DIR wins";
    EXPECT_EQ(mesaShaderCacheDir(nullptr, "/home/u/.xdg-cache", "/home/u"), "") << "Mesa follows XDG_CACHE_HOME itself";
    EXPECT_EQ(fs::path(mesaShaderCacheDir("", "", "/home/u")), fs::path("/home/u/.cache/mesa_shader_cache")) << "empty values are unset";
    EXPECT_EQ(mesaShaderCacheDir(nullptr, nullptr, nullptr), "");
}
