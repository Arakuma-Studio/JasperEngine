// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUiImportQueue.h"
#include "util/SafeWrite.h"

#include "Out.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor::htmlui;
using jasperengine::editor::Out;
using jasperengine::htmlui::compat::ImportManifest;

ImportQueue::ImportQueue(Runner runner): runner(runner ? std::move(runner) : Runner(&ImportQueue::runImport)) {}

ImportQueue::~ImportQueue(){
    stop();
}

void ImportQueue::stop(){
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
        queue.clear();
    }
    wake.notify_all();
    if (worker.joinable())
        worker.join();
}

ImportedResource ImportQueue::runImport(const ImportJob& job){
    RemoteImporter importer(job.target);
    importer.setProgress([](const std::string& line){ Out::info("%s", line.c_str()); });
    ImportedResource result = importer.import(job.url, job.kind, job.location);
    if (HttpClient::cancelled())
        return result;   // editor shutdown: nothing is logged any more
    for (const compat::Diagnostic& diagnostic : importer.diagnostics()){
        const std::string text = compat::toText(diagnostic);
        switch (diagnostic.severity){
            case compat::Severity::Error: Out::error("[HTML UI] %s", text.c_str()); break;
            case compat::Severity::Warning: Out::warning("[HTML UI] %s", text.c_str()); break;
            case compat::Severity::Info: Out::info("[HTML UI] %s", text.c_str()); break;
        }
    }
    return result;
}

bool ImportQueue::enqueue(ImportJob job, bool manual){
    const std::string key = ImportManifest::normalizeUrl(job.url);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping)
            return false;
        const auto it = states.find(key);
        if (it != states.end()){
            const ImportStatus::State state = it->second.state;
            if (state == ImportStatus::State::Queued || state == ImportStatus::State::Importing){
                // one import per URL; remember who else waits for it
                for (ImportJob& queued : queue){
                    if (ImportManifest::normalizeUrl(queued.url) != key)
                        continue;
                    for (const std::string& document : job.documents){
                        if (std::find(queued.documents.begin(), queued.documents.end(), document) == queued.documents.end())
                            queued.documents.push_back(document);
                    }
                }
                for (const std::string& document : job.documents){
                    if (std::find(finished.documents.begin(), finished.documents.end(), document) == finished.documents.end())
                        finished.documents.push_back(document);
                }
                return false;
            }
            if (!manual)
                return false;   // imported or failed in this session
        }
        states[key] = ImportStatus{ImportStatus::State::Queued, ""};
        queue.push_back(std::move(job));
        if (!worker.joinable())
            worker = std::thread(&ImportQueue::work, this);
    }
    wake.notify_one();
    return true;
}

std::optional<ImportStatus> ImportQueue::status(const std::string& url) const{
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = states.find(ImportManifest::normalizeUrl(url));
    if (it == states.end())
        return std::nullopt;
    return it->second;
}

bool ImportQueue::busy() const{
    std::lock_guard<std::mutex> lock(mutex);
    return !queue.empty() || !running.empty();
}

size_t ImportQueue::pending() const{
    std::lock_guard<std::mutex> lock(mutex);
    return queue.size() + (running.empty() ? 0 : 1);
}

bool ImportQueue::takeFinishedBatch(Batch& batch){
    std::lock_guard<std::mutex> lock(mutex);
    if (!batchReady)
        return false;
    batch = std::move(finished);
    finished = Batch();
    batchReady = false;
    return true;
}

void ImportQueue::waitIdle(){
    std::unique_lock<std::mutex> lock(mutex);
    idle.wait(lock, [this]{ return (queue.empty() && running.empty()) || stopping; });
}

void ImportQueue::clearSession(){
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = states.begin(); it != states.end();){
        if (it->second.state == ImportStatus::State::Imported || it->second.state == ImportStatus::State::Failed)
            it = states.erase(it);
        else
            ++it;
    }
}

void ImportQueue::work(){
    std::unique_lock<std::mutex> lock(mutex);
    while (true){
        wake.wait(lock, [this]{ return stopping || !queue.empty(); });
        if (stopping)
            break;
        ImportJob job = std::move(queue.front());
        queue.pop_front();
        const std::string key = ImportManifest::normalizeUrl(job.url);
        running = key;
        states[key].state = ImportStatus::State::Importing;
        lock.unlock();

        ImportedResource result;
        {
            // the import's files beneath the project (util/SafeWrite.h: no link on the way
            // followed), for the GUI's automatic imports too
            jasperengine::editor::safewrite::Scope scope(job.target.projectRoot);
            result = runner(job);
        }

        lock.lock();
        ImportStatus& status = states[key];
        status.state = result.ok ? ImportStatus::State::Imported : ImportStatus::State::Failed;
        status.error = result.ok ? std::string() : result.error;
        running.clear();
        finished.results.push_back(std::move(result));
        for (const std::string& document : job.documents){
            if (std::find(finished.documents.begin(), finished.documents.end(), document) == finished.documents.end())
                finished.documents.push_back(document);
        }
        if (queue.empty()){
            batchReady = true;
            idle.notify_all();
        }
    }
    idle.notify_all();
}
