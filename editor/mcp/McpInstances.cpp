// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpInstances.h"

#include "AppSettings.h"
#include "EditorVersion.h"
#include "io/AtomicFile.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace jasperengine::editor::mcp {

namespace {

// An editor that still runs. A killed editor its parent has not reaped yet is a zombie, which
// kill(pid, 0) still finds: on Linux its /proc state says so (state Z), and it serves nothing
bool editorAlive(int pid) {
    if (!io::isProcessRunning(pid)) return false;
#ifdef __linux__
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (std::getline(stat, line)) {
        // "<pid> (<comm>) <state> ...": comm may hold spaces and parentheses, the last ')' ends it
        const size_t close = line.rfind(')');
        if (close != std::string::npos && close + 2 < line.size() && line[close + 2] == 'Z') return false;
    }
#endif
    return true;
}

std::string utcNow() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

fs::path instanceFile(int pid) {
    return instancesDirectory() / (std::to_string(pid) + ".json");
}

std::string canonical(const std::string& path) {
    if (path.empty()) return {};
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::absolute(fs::path(path), ec), ec);
    if (ec) p = fs::absolute(fs::path(path)).lexically_normal();
    std::string text = p.generic_string();
    while (text.size() > 1 && text.back() == '/') text.pop_back();
#ifdef _WIN32
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return text;
}

} // namespace

Json InstanceInfo::toJson() const {
    return Json{
        {"pid", pid}, {"url", url}, {"port", port}, {"project", project}, {"cwd", cwd},
        {"settings_path", settingsPath}, {"token_required", tokenRequired}, {"version", version},
        {"started", started}
    };
}

bool InstanceInfo::fromJson(const Json& json, InstanceInfo& out) {
    if (!json.is_object() || !json.contains("pid") || !json["pid"].is_number_integer() ||
        !json.contains("port") || !json["port"].is_number_integer()) {
        return false;
    }
    out.pid = json["pid"].get<int>();
    out.port = json["port"].get<int>();
    out.url = json.value("url", "http://127.0.0.1:" + std::to_string(out.port) + "/mcp");
    out.project = json.value("project", std::string());
    out.cwd = json.value("cwd", std::string());
    out.settingsPath = json.value("settings_path", std::string());
    out.tokenRequired = json.value("token_required", false);
    out.version = json.value("version", std::string());
    out.started = json.value("started", std::string());
    return true;
}

fs::path instancesDirectory() {
    return AppSettings::getUserConfigDirectory() / "mcp" / "instances";
}

bool writeInstance(const InstanceInfo& info, std::string& error) {
    const fs::path dir = instancesDirectory();
    std::error_code ec;
    fs::create_directories(dir, ec);
    // only this user lists the editors (the files hold no secret, but name projects and ports)
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    const fs::path file = instanceFile(info.pid);
    const io::WriteResult result = io::writeFileAtomic(file, info.toJson().dump(2) + "\n", options);
    if (!result.ok()) {
        error = result.message;
        return false;
    }
    // a new file gets the default mode; a rewritten one keeps 0600
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    return true;
}

void removeInstance(int pid) {
    std::error_code ec;
    fs::remove(instanceFile(pid), ec);
}

std::vector<InstanceInfo> liveInstances() {
    std::vector<InstanceInfo> out;
    std::error_code ec;
    for (fs::directory_iterator it(instancesDirectory(), ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path path = it->path();
        if (path.extension() != ".json" || io::isTemporarySiblingName(path.filename().string())) continue;
        std::ifstream file(path, std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        file.close();
        InstanceInfo info;
        const Json json = Json::parse(text.str(), nullptr, false);
        if (!InstanceInfo::fromJson(json, info)) continue;   // being written, or not ours
        if (!editorAlive(info.pid)) {
            std::error_code removeError;
            fs::remove(path, removeError);
            continue;
        }
        out.push_back(info);
    }
    std::sort(out.begin(), out.end(), [](const InstanceInfo& a, const InstanceInfo& b) { return a.pid < b.pid; });
    return out;
}

bool sameProjectPath(const std::string& a, const std::string& b) {
    return !a.empty() && canonical(a) == canonical(b);
}

void InstanceRecord::httpStarted(int port, bool tokenRequired, const std::string& project) {
    info.pid = static_cast<int>(io::currentProcessId());
    info.port = port;
    info.url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    info.project = project;
    std::error_code ec;
    info.cwd = fs::current_path(ec).generic_string();
    info.settingsPath = (AppSettings::getConfigDirectory() / "settings.yaml").generic_string();
    info.tokenRequired = tokenRequired;
    info.version = JASPERENGINE_EDITOR_VERSION;
    info.started = utcNow();
    write();
}

void InstanceRecord::projectChanged(const std::string& project) {
    if (!active() || project == info.project) return;
    info.project = project;
    write();
}

void InstanceRecord::httpStopped() {
    if (!active()) return;
    removeInstance(info.pid);
    info = InstanceInfo();
}

void InstanceRecord::write() {
    std::string error;
    if (!writeInstance(info, error)) {
        std::fprintf(stderr, "[mcp] could not write the instance file %s: %s\n",
                     instanceFile(info.pid).generic_string().c_str(), error.c_str());
    }
}

}
