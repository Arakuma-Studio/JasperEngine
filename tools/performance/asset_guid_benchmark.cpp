// Compile this harness with editor/util/AssetDatabase.cpp; see asset_guid_benchmark.sh.
#include "util/AssetDatabase.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using jasperengine::editor::AssetDatabase;

// The original parser is the compatibility oracle, including malformed input.
static std::string referenceGuid(const std::string& text) {
    for (size_t begin = 0; begin < text.size();) {
        size_t end = text.find('\n', begin);
        if (end == std::string::npos) end = text.size();
        if (text.compare(begin, 5, "guid:") == 0) {
            if (end > begin && text[end - 1] == '\r') --end;
            std::string value = text.substr(begin + 5, end - begin - 5);
            const size_t comment = value.find(" #");
            if (comment != std::string::npos) value.erase(comment);
            auto notSpace = [](unsigned char c) { return !std::isspace(c); };
            value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
            value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
            if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
                value = value.substr(1, value.size() - 2);
            }
            return value;
        }
        begin = end + 1;
    }
    return {};
}

static void writeFile(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file) throw std::runtime_error("Cannot write fixture");
}

static void verify(const fs::path& directory) {
    const fs::path path = directory / "test.prefab";
    std::vector<std::string> cases = {
        "", "guid:", "guid: one", "guid: one\r\n", "guid: \"one\" # comment\n",
        "guid: 'one'\n", "# comment\n---\nguid: one\n", " guid: nested\nguid: root\n",
        "guid: first\nguid: second\n", "guid: \t \r\n", "guid:one#not-comment\n",
        "guid: 'one # two'\n", "entities:\n  - guid: nested\n", "\xEF\xBB\xBFguid: bom\n",
        std::string("guid: a\0b\n", 10), std::string(131072, 'x') + "\nguid: last"
    };
    std::mt19937 random(1729);
    for (size_t i = 0; i < 2000; ++i) {
        std::string text;
        for (size_t line = 0, count = random() % 12; line < count; ++line) {
            if (random() % 4 == 0) text += "guid:";
            for (size_t j = 0, count = random() % 60; j < count; ++j) text += static_cast<char>(random() % 256);
            text += '\n';
        }
        cases.push_back(std::move(text));
    }
    for (const auto& text : cases) {
        writeFile(path, text);
        if (AssetDatabase::readGuid(path) != referenceGuid(text)) throw std::runtime_error("GUID compatibility failure");
    }
    if (!AssetDatabase::readGuid(directory / "missing.prefab").empty()) throw std::runtime_error("Missing file failure");
    std::cout << "PASS: " << cases.size() << " parser cases and missing file\n";
}

static void benchmark(const fs::path& directory, size_t bytes, bool guidFirst) {
    const std::string guid = "0123456789abcdef0123456789abcdef";
    std::string body;
    body.reserve(bytes);
    while (body.size() < bytes) body += "  - entity: example_prefab_entity_with_serialized_properties\n";
    const std::string text = guidFirst ? "guid: " + guid + "\nentities:\n" + body : "entities:\n" + body + "guid: " + guid + "\n";
    std::vector<fs::path> paths;
    for (size_t i = 0; i < 64; ++i) {
        paths.push_back(directory / (std::to_string(i) + ".prefab"));
        writeFile(paths.back(), text);
    }
    std::vector<double> samples;
    std::vector<double> cpuSamples;
    // Warm filesystem cache; measure seven scans of the same generated project.
    for (size_t round = 0; round < 8; ++round) {
        const auto start = std::chrono::steady_clock::now();
        const auto cpuStart = std::clock();
        for (const auto& path : paths) {
            if (AssetDatabase::readGuid(path) != guid) throw std::runtime_error("Benchmark GUID mismatch");
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const double cpuMs = 1000.0 * (std::clock() - cpuStart) / CLOCKS_PER_SEC;
        if (round) {
            samples.push_back(ms);
            cpuSamples.push_back(cpuMs);
        }
    }
    std::sort(samples.begin(), samples.end());
    std::sort(cpuSamples.begin(), cpuSamples.end());
    std::cout << "64 files," << bytes << " bytes body,guid " << (guidFirst ? "first" : "last")
              << ",median_ms=" << samples[3] << ",median_cpu_ms=" << cpuSamples[3] << '\n';
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const fs::path directory = argv[1];
    fs::create_directories(directory);
    verify(directory);
    benchmark(directory, 4096, true);
    benchmark(directory, 1024 * 1024, true);
    benchmark(directory, 1024 * 1024, false);
}
