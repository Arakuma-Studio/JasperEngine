// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DeviceCommands.h"

#include "player/DevelopmentContent.h"

#include <cstdio>

using namespace jasperengine;

namespace jasperengine::editor::platform {

std::string devicecmd::shellQuote(const std::string& word) {
    std::string quoted = "'";
    for (char c : word) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    quoted += "'";
    return quoted;
}

std::string devicecmd::shellJoin(const std::vector<std::string>& words) {
    std::string joined;
    for (const std::string& word : words) joined += (joined.empty() ? "" : " ") + shellQuote(word);
    return joined;
}

std::vector<std::string> devicecmd::adb(const std::string& adbPath, const std::string& serial, const std::vector<std::string>& args) {
    std::vector<std::string> argv = {adbPath, "-s", serial};
    argv.insert(argv.end(), args.begin(), args.end());
    return argv;
}

std::vector<std::string> devicecmd::runAs(const std::string& adbPath, const std::string& serial, const std::string& package,
                                          const std::string& script) {
    // one string for the device's shell: run-as, the package and sh -c with the script, each a word
    return adb(adbPath, serial, {"shell", "run-as " + shellQuote(package) + " sh -c " + shellQuote(script)});
}

std::string devicecmd::developmentFolder() {
    return std::string("files/") + player::DEVELOPMENT_CONTENT_DIR;
}

std::string devicecmd::remoteRoot(const std::string& package) {
    return "/data/local/tmp/jasper/" + package;
}

std::string devicecmd::remotePatchDir(const std::string& package, int patch) {
    return remoteRoot(package) + "/patch-" + std::to_string(patch);
}

std::string devicecmd::patchScript(const std::string& pushedDir, const std::vector<std::string>& deleted, bool first) {
    const std::string folder = developmentFolder();
    std::string script;
    if (first) script += "rm -rf " + shellQuote(folder) + " && ";
    script += "mkdir -p " + shellQuote(folder);
    // an empty files/ is not pushed (a patch that only deletes)
    script += " && { [ ! -d " + shellQuote(pushedDir + "/files") + " ] || cp -R " + shellQuote(pushedDir + "/files/.") + " " +
              shellQuote(folder + "/") + "; }";
    if (!deleted.empty()) {
        script += " && rm -f";
        for (const std::string& file : deleted) script += " " + shellQuote(folder + "/" + file);
    }
    script += " && cp " + shellQuote(pushedDir + "/content-manifest.json") + " " + shellQuote(folder + "/content-manifest.json");
    return script;
}

std::string devicecmd::clearScript() {
    return "rm -rf " + shellQuote(developmentFolder());
}

devicecmd::PatchPlan devicecmd::planPatch(const std::map<std::string, player::ManifestFile>& onDevice,
                                          const std::map<std::string, player::ManifestFile>& next, bool first) {
    PatchPlan plan;
    plan.first = first;
    plan.push.reserve(first ? next.size() : 16);
    // both maps are sorted: one pass over each
    auto device = onDevice.begin();
    for (const auto& [path, file] : next) {
        if (path == "content-manifest.json") continue;
        while (!first && device != onDevice.end() && device->first < path) {
            if (device->first != "content-manifest.json") plan.remove.push_back(device->first);
            ++device;
        }
        const bool same = !first && device != onDevice.end() && device->first == path && device->second.sha256 == file.sha256 &&
                          device->second.size == file.size;
        if (!first && device != onDevice.end() && device->first == path) ++device;
        if (same) continue;
        plan.push.push_back(path);
        plan.bytes += file.size;
    }
    for (; !first && device != onDevice.end(); ++device)
        if (device->first != "content-manifest.json") plan.remove.push_back(device->first);
    return plan;
}

std::string devicecmd::queryFromArgs(const std::vector<std::string>& args) {
    auto encode = [](const std::string& text) {
        std::string out;
        for (unsigned char c : text) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '/') {
                out += static_cast<char>(c);
            } else {
                char hex[4];
                std::snprintf(hex, sizeof(hex), "%%%02X", c);
                out += hex;
            }
        }
        return out;
    };
    std::string query;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& arg = args[i];
        if (arg == "--") break;
        if (arg.rfind("--", 0) != 0) continue;
        std::string item = arg.substr(2);
        std::string value;
        const size_t equals = item.find('=');
        if (equals != std::string::npos) {
            value = item.substr(equals + 1);
            item = item.substr(0, equals);
        } else if (i + 1 < args.size() && args[i + 1].rfind("--", 0) != 0) {
            value = args[++i];
        }
        query += (query.empty() ? "" : "&") + encode(item) + (value.empty() && equals == std::string::npos ? std::string() : "=" + encode(value));
    }
    return query;
}

}  // namespace jasperengine::editor::platform
