// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShippingCommands.h"

#include "cli/CommandLine.h"
#include "cook/RuntimeFieldsEmitter.h"
#include "io/AtomicFile.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

int ShippingCommands::runReflect(int argc, char** argv, const char* executableName) {
    std::string output;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--emit-runtime-fields" && i + 1 < argc) {
            output = CommandLine::absoluteArgPath(argv[++i]).string();
        } else if (arg == "-h" || arg == "--help") {
            std::printf("Usage: %s reflect --emit-runtime-fields <file>\n"
                        "Writes the cooked scenes' runtime field tables (engine/core/serialization/RuntimeFields.gen.inc)\n"
                        "from the editor's reflection tables.\n", executableName);
            return 0;
        } else {
            std::fprintf(stderr, "reflect: unknown argument %s (try --help)\n", arg.c_str());
            return 2;
        }
    }
    if (output.empty()) {
        std::fprintf(stderr, "reflect: --emit-runtime-fields <file> is required\n");
        return 2;
    }
    std::vector<std::string> errors;
    const std::string text = cook::emitRuntimeFields(errors);
    for (const std::string& error : errors) std::fprintf(stderr, "reflect: %s\n", error.c_str());
    if (!errors.empty()) return 1;
    const io::WriteResult written = io::writeFileAtomic(output, text);
    if (!written.ok()) {
        std::fprintf(stderr, "reflect: %s\n", io::describeWriteError(written).c_str());
        return 1;
    }
    std::printf("runtime_fields: %s\nbytes: %zu\n", output.c_str(), text.size());
    return 0;
}
