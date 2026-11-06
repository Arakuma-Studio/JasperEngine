// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Launch.h"

#include "PlatformConfig.h"
#include "player/RunControl.h"
#include "device/Device.h"

#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::player;

namespace {
    GameConfigurator gameConfigurator = nullptr;
}

void jasperengine::player::setGameConfigurator(GameConfigurator configurator) {
    gameConfigurator = configurator;
}

bool jasperengine::player::hasGameConfigurator() {
    return gameConfigurator != nullptr;
}

int jasperengine::player::launch(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) args.emplace_back(argv[i]);
    return launch(args, gameConfigurator ? ParseMode::Strict : ParseMode::Lenient);
}

int jasperengine::player::launch(const std::vector<std::string>& args, ParseMode mode) {
    auto parsed = parse(args, mode);
    if (auto* error = std::get_if<ParseError>(&parsed)) {
        std::fprintf(stderr, "%s\n", error->message.c_str());
        return EXIT_BAD_ARGUMENTS;
    }
    const LaunchOptions& options = std::get<LaunchOptions>(parsed);
    RunControl::configure(options);

    PlatformConfig& config = platformConfig();
    if (!options.content.empty())
        config.contentRoot = options.content.string();

    if (gameConfigurator) {
        const int code = gameConfigurator(options);
        if (code >= 0) return code;
    }

    // the options go over the build's and the game's settings
    if (options.width > 0 && options.height > 0) {
        config.width = options.width;
        config.height = options.height;
    }
    if (options.headless) {
        config.headless = true;
        // a headless run goes as fast as it can: no vsync (R17)
        config.vsync = false;
        // a fullscreen or maximized window would take the screen's size, not the run's
        config.windowMode = 0;
    }
    if (options.headless || options.silentAudio || !options.audioCapture.empty())
        config.silentAudio = true;
    DeviceTier tier;
    if (!options.deviceTier.empty() && DeviceNames::tierFromName(options.deviceTier.c_str(), tier))
        Device::setTier(tier);
    return -1;
}
