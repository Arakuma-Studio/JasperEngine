// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

namespace jasperengine::editor{

    class CreateDirCmd: public Command{

    private:

        fs::path directory;

    public:
        CreateDirCmd(std::string dirName, std::string dirPath);

        bool execute() override;
        void undo() override;

    };

}
