// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "log/LogSinks.h"

using namespace jasperengine;

CallbackSink::CallbackSink(std::string sinkName, Callback callback, Delivery delivery):
    sinkName(std::move(sinkName)), callback(std::move(callback)), mode(delivery) {
}

void CallbackSink::write(const LogRecord& record) {
    if (callback) callback(record);
}
