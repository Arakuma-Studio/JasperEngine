// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitlePreviewQueue.h"

#include "render/SystemRender.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <tuple>

using namespace jasperengine;
using namespace jasperengine::editor;

const char* editor::titlePreviewPriorityName(TitlePreviewPriority priority) {
    switch (priority) {
        case TitlePreviewPriority::Hover: return "hover";
        case TitlePreviewPriority::Layers: return "layers";
        case TitlePreviewPriority::Tiles: return "tiles";
        case TitlePreviewPriority::Thumbnails: return "thumbnails";
        case TitlePreviewPriority::Prefetch: return "prefetch";
    }
    return "prefetch";
}

bool TitlePreviewQueue::Item::visible() const {
    return std::any_of(visibility.begin(), visibility.end(), [](const auto& v) { return v.second; });
}

TitlePreviewQueue::TitlePreviewQueue(std::function<double()> clockMs, std::function<uint64_t()> contextId)
    : clock(std::move(clockMs)), context(std::move(contextId)) {
    if (!clock) {
        clock = [] {
            using namespace std::chrono;
            return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
        };
    }
    if (context) lastContext = context();
}

TitlePreviewQueue::~TitlePreviewQueue() = default;

TitlePreviewQueue& TitlePreviewQueue::editor() {
    static TitlePreviewQueue* queue = new TitlePreviewQueue({}, [] { return SystemRender::getContextId(); });
    return *queue;
}

uint64_t TitlePreviewQueue::request(TitlePreviewPriority priority, const std::string& key, TitlePreviewFactory factory,
                                    TitlePreviewOwner owner, bool visible) {
    const uint64_t id = nextRequest++;
    Item* item = nullptr;
    for (const auto& existing : items) {
        if (existing->key == key && !key.empty()) item = existing.get();
    }
    if (!item) {
        auto made = std::make_unique<Item>();
        made->priority = priority;
        made->key = key;
        made->factory = std::move(factory);
        made->order = nextOrder++;
        item = made.get();
        items.push_back(std::move(made));
    } else if (priority < item->priority) {
        item->priority = priority;
    }
    item->requests[id] = owner;
    item->visibility[id] = visible;
    byRequest[id] = item;
    refreshCounts();
    return id;
}

void TitlePreviewQueue::setVisible(uint64_t id, bool visible) {
    auto it = byRequest.find(id);
    if (it != byRequest.end()) it->second->visibility[id] = visible;
}

void TitlePreviewQueue::drop(Item* item) {
    items.erase(std::remove_if(items.begin(), items.end(), [item](const auto& i) { return i.get() == item; }), items.end());
}

void TitlePreviewQueue::cancel(uint64_t id) {
    auto it = byRequest.find(id);
    if (it == byRequest.end()) return;
    Item* item = it->second;
    item->requests.erase(id);
    item->visibility.erase(id);
    byRequest.erase(it);
    if (item->requests.empty()) drop(item);
    refreshCounts();
}

void TitlePreviewQueue::cancelAll(TitlePreviewOwner owner) {
    std::vector<uint64_t> ids;
    for (const auto& [id, item] : byRequest) {
        auto r = item->requests.find(id);
        if (r != item->requests.end() && r->second == owner) ids.push_back(id);
    }
    for (uint64_t id : ids) cancel(id);
}

TitlePreviewState TitlePreviewQueue::state(uint64_t id) const {
    auto it = byRequest.find(id);
    return it != byRequest.end() ? it->second->state : TitlePreviewState::Unknown;
}

TitlePreviewJob* TitlePreviewQueue::result(uint64_t id) const {
    auto it = byRequest.find(id);
    return it != byRequest.end() && it->second->state == TitlePreviewState::Done ? it->second->job.get() : nullptr;
}

void TitlePreviewQueue::deviceLost() {
    for (const auto& item : items) {
        item->job.reset();
        item->state = TitlePreviewState::Queued;
        counters.restarts++;
    }
    restFrames = 0;
    refreshCounts();
}

void TitlePreviewQueue::resetStats() {
    const size_t queued = counters.queued, done = counters.done;
    counters = TitlePreviewStats();
    counters.queued = queued;
    counters.done = done;
    recentPending.clear();
    recentWorking.clear();
}

void TitlePreviewQueue::refreshCounts() {
    counters.queued = 0;
    counters.done = 0;
    for (const auto& item : items) {
        if (item->state == TitlePreviewState::Queued || item->state == TitlePreviewState::Running) counters.queued++;
        else if (item->state == TitlePreviewState::Done) counters.done++;
    }
}

TitlePreviewQueue::Item* TitlePreviewQueue::pick() const {
    Item* best = nullptr;
    for (const auto& item : items) {
        if (item->state != TitlePreviewState::Queued && item->state != TitlePreviewState::Running) continue;
        if (!best) {
            best = item.get();
            continue;
        }
        const auto rank = [](const Item* i) { return std::make_tuple((int)i->priority, i->visible() ? 0 : 1, i->order); };
        if (rank(item.get()) < rank(best)) best = item.get();
    }
    return best;
}

void TitlePreviewQueue::charge(double ms) {
    if (ms <= 0) return;
    charged += ms;
    counters.sideWorkMs += ms;
}

void TitlePreviewQueue::Ring::add(double value) {
    if (values.size() < WINDOW) {
        values.push_back(value);
    } else {
        values[next] = value;
        next = (next + 1) % WINDOW;
    }
}

double TitlePreviewQueue::Ring::p95() const {
    if (values.empty()) return 0.0;
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    // nearest rank
    const size_t rank = (size_t)std::ceil(0.95 * (double)sorted.size());
    return sorted[std::clamp<size_t>(rank, 1, sorted.size()) - 1];
}

void TitlePreviewQueue::Ring::clear() {
    values.clear();
    next = 0;
}

void TitlePreviewQueue::runFrame(double budgetMs) {
    counters.frames++;
    counters.budgetMs = budgetMs;
    if (context) {
        const uint64_t now = context();
        if (now != lastContext) {
            lastContext = now;
            deviceLost();
        }
    }
    const bool pending = counters.queued > 0;
    // the side work charged since the last frame belongs to this one
    const double side = charged;
    charged = 0;
    double spent = 0;
    bool ran = false;
    bool ranAlone = false;
    TitlePreviewOverrun frame;
    if (restFrames > 0) {
        restFrames--;
        counters.restFrames++;
    } else {
        const double budget = budgetMs - side;
        const double start = clock();
        while (budget > 0) {
            Item* item = pick();
            if (!item) break;
            if (!item->job) {
                item->job = item->factory ? item->factory() : nullptr;
                if (!item->job) {
                    item->state = TitlePreviewState::Failed;
                    continue;
                }
            }
            // a step that does not fit what is left runs only as the frame's first work; one
            // predicted over the whole budget then runs alone
            const double cost = item->job->nextStepCostUs() / 1000.0;
            if (cost > budget - spent && (ran || side > 0)) break;
            ranAlone = cost > budgetMs;
            item->state = TitlePreviewState::Running;
            const double left = std::max(0.0, budget - spent);
            const double stepStart = clock();
            const bool finished = item->job->step((uint32_t)std::max(1.0, std::floor(left * 1000.0)));
            const double stepMs = clock() - stepStart;
            ran = true;
            counters.steps++;
            // the job's own longest piece, else the whole call (an upper bound)
            std::string label = item->job->longestStepLabel();
            double longest = item->job->longestStepMs();
            if (label.empty()) {
                label = item->key;
                longest = stepMs;
            }
            if (longest >= frame.longestMs) {
                frame.longestMs = longest;
                frame.longestLabel = label;
            }
            if (longest >= counters.worstStepMs) {
                counters.worstStepMs = longest;
                counters.worstStepLabel = label;
            }
            if (item->job->failed()) item->state = TitlePreviewState::Failed;
            else if (finished) item->state = TitlePreviewState::Done;
            spent = clock() - start;
            if (ranAlone || spent >= budget) break;
        }
    }
    const double total = side + spent;
    counters.lastFrameMs = total;
    if (ran || side > 0) {
        counters.workingFrames++;
        counters.maxFrameMs = std::max(counters.maxFrameMs, total);
        if (ranAlone) counters.aloneFrames++;
        if (total > budgetMs) {
            counters.overBudgetFrames++;
            frame.frame = counters.frames;
            frame.ms = total;
            frame.sideWorkMs = side;
            if (counters.overruns.size() >= OVERRUNS) counters.overruns.erase(counters.overruns.begin());
            counters.overruns.push_back(std::move(frame));
            // idle frames that bring the average back within the budget
            restFrames = std::max(restFrames, (int)std::ceil(total / std::max(budgetMs, 1e-6)) - 1);
        }
        recentWorking.add(total);
    }
    if (pending || ran || side > 0) recentPending.add(total);
    refreshCounts();
}
