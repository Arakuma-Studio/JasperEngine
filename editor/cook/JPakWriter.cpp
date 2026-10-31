// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "JPakWriter.h"

#include "io/AtomicFile.h"
#include "io/JPak.h"
#include "util/Fnv.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    void put32(std::string& out, uint32_t v) {
        for (int i = 0; i < 4; i++) out.push_back(static_cast<char>(v >> (i * 8)));
    }
    void put64(std::string& out, uint64_t v) {
        for (int i = 0; i < 8; i++) out.push_back(static_cast<char>(v >> (i * 8)));
    }

}

bool editor::cook::writeJPak(const fs::path& root, const fs::path& output, const std::vector<std::string>& exclude, JPakStats& stats,
                             std::string& error) {
    const std::set<std::string> excluded(exclude.begin(), exclude.end());
    std::vector<std::pair<std::string, fs::path>> files;
    std::map<std::string, std::string> folded;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string relative = fs::relative(it->path(), root, ec).generic_string();
        if (excluded.count(relative)) continue;
        const std::string illegal = JPak::checkPath(relative);
        if (!illegal.empty()) {
            error = "content file \"" + relative + "\" cannot be packed: " + illegal;
            return false;
        }
        const std::string key = JPak::foldCase(relative);
        if (auto clash = folded.find(key); clash != folded.end()) {
            error = "content files \"" + clash->second + "\" and \"" + relative + "\" differ only in case (a pack needs them unique)";
            return false;
        }
        folded.emplace(key, relative);
        files.emplace_back(relative, it->path());
    }
    if (ec) {
        error = "cannot list " + root.string() + ": " + ec.message();
        return false;
    }
    std::sort(files.begin(), files.end());

    // the blobs first (their hashes go in the index), then the file in order
    std::vector<std::string> blobs;
    uint64_t indexBytes = 0;
    for (const auto& [relative, path] : files) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            error = "cannot read " + path.string();
            return false;
        }
        blobs.emplace_back((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        indexBytes += 4 + relative.size() + 24;
    }
    std::string pack(JPak::MAGIC, JPak::MAGIC + 8);
    put32(pack, JPak::VERSION);
    put32(pack, static_cast<uint32_t>(files.size()));
    put64(pack, indexBytes);
    uint64_t offset = JPak::HEADER_BYTES + indexBytes;
    std::vector<uint64_t> offsets;
    for (size_t i = 0; i < files.size(); i++) {
        offset = (offset + JPak::ALIGNMENT - 1) / JPak::ALIGNMENT * JPak::ALIGNMENT;
        offsets.push_back(offset);
        put32(pack, static_cast<uint32_t>(files[i].first.size()));
        pack += files[i].first;
        put64(pack, offset);
        put64(pack, blobs[i].size());
        put64(pack, fnv1a64(blobs[i].data(), blobs[i].size()));
        offset += blobs[i].size();
    }
    for (size_t i = 0; i < files.size(); i++) {
        pack.resize(static_cast<size_t>(offsets[i]), '\0');
        pack += blobs[i];
    }
    const io::WriteResult written = io::writeFileAtomic(output, pack);
    if (!written.ok()) {
        error = io::describeWriteError(written);
        return false;
    }
    stats.entries = static_cast<uint32_t>(files.size());
    stats.bytes = pack.size();
    return true;
}
