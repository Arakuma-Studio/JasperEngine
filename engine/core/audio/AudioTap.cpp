// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioTap.h"

#include <algorithm>
#include <utility>
#include <vector>

using namespace jasperengine;

namespace {

    struct TapClient {
        int id = 0;
        std::string name;
        AudioTap::Fn fn;
    };

    std::vector<TapClient>& tapClients() {
        static auto* clients = new std::vector<TapClient>();
        return *clients;
    }

    int nextTapClient = 1;

}

int AudioTap::subscribe(std::string name, Fn fn) {
    const int id = nextTapClient++;
    tapClients().push_back({id, std::move(name), std::move(fn)});
    return id;
}

void AudioTap::unsubscribe(int id) {
    std::vector<TapClient>& clients = tapClients();
    clients.erase(std::remove_if(clients.begin(), clients.end(), [id](const TapClient& client) { return client.id == id; }), clients.end());
}

void AudioTap::publish(const float* interleaved, uint32_t frames, uint64_t firstFrame) {
    for (const TapClient& client : tapClients()) client.fn(interleaved, frames, firstFrame);
}
