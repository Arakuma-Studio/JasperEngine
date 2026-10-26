// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LogCapture.h"

using namespace jasperengine;
using namespace jasperengine::test;

namespace {
    size_t testStart = 0;
}

LogCapture::LogCapture(): start(TestSystem::get().getLogSequence()){
}

LogCapture::LogCapture(size_t start): start(start){
}

LogCapture LogCapture::sinceTestStart(){
    return LogCapture(testStart);
}

void LogCapture::markTestStart(){
    testStart = TestSystem::get().getLogSequence();
}

std::vector<TestSystem::LogEntry> LogCapture::entries() const{
    return TestSystem::get().getLogsSince(start);
}

std::vector<TestSystem::LogEntry> LogCapture::entries(int type, const std::string& text) const{
    std::vector<TestSystem::LogEntry> matching;
    for (const TestSystem::LogEntry& entry : entries()){
        if (entry.type == type && (text.empty() || entry.message.find(text) != std::string::npos))
            matching.push_back(entry);
    }
    return matching;
}

size_t LogCapture::count(int type, const std::string& text) const{
    return entries(type, text).size();
}

std::vector<TestSystem::LogEntry> LogCapture::errors() const{
    return entries(D_LOG_ERROR);
}

std::vector<TestSystem::LogEntry> LogCapture::warnings() const{
    return entries(D_LOG_WARN);
}

bool LogCapture::contains(const std::string& text) const{
    for (const TestSystem::LogEntry& entry : entries()){
        if (entry.message.find(text) != std::string::npos)
            return true;
    }
    return false;
}

std::string LogCapture::describeErrors() const{
    std::string text;
    for (const TestSystem::LogEntry& entry : errors()){
        text += "  " + entry.message;
        if (text.empty() || text.back() != '\n')
            text += '\n';
    }
    return text;
}
