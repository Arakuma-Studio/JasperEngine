// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleMigrationReport.h"

#include "Out.h"

#include <algorithm>

using namespace jasperengine::editor;

std::mutex& ParticleMigrationReport::mutex() {
    static std::mutex instance;
    return instance;
}

std::vector<ParticleMigrationReport::Entry>& ParticleMigrationReport::list() {
    static std::vector<Entry> instance;
    return instance;
}

void ParticleMigrationReport::record(const std::string& document, const std::vector<std::string>& lines) {
    std::lock_guard<std::mutex> lock(mutex());
    for (const std::string& line : lines) {
        if (line.rfind("Particle System of ", 0) != 0) continue;
        // a document read again (a reload, a prefab used twice) reports once
        const bool known = std::any_of(list().begin(), list().end(), [&](const Entry& e) { return e.document == document && e.line == line; });
        if (known) continue;
        list().push_back({document, line});
        Out::info(document + ": " + line);
    }
}

std::vector<ParticleMigrationReport::Entry> ParticleMigrationReport::entries() {
    std::lock_guard<std::mutex> lock(mutex());
    return list();
}

void ParticleMigrationReport::clear() {
    std::lock_guard<std::mutex> lock(mutex());
    list().clear();
}
