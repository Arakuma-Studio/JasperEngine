// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/RcssWriter.h"
#include "htmlui/compat/Arena.h"

#include <cstring>
#include "htmlui/compat/LiteralCompare.h"

using namespace jasperengine::htmlui::compat;

std::string RcssMediaCondition::text() const{
    if (!raw.empty())
        return raw;
    std::string result = negated ? "not " : "";
    for (size_t i = 0; i < features.size(); i++){
        if (i > 0)
            result += " and ";
        result += '(';
        result += features[i];
        result += ')';
    }
    return result;
}

RcssWriter::RcssWriter(Mode mode): mode(mode) {
}

namespace {

    // the largest output buffer a writer of the thread left (its pages mapped already): the
    // next writer starts in it
    struct SpareOutput {
        std::unique_ptr<char[]> data;
        size_t capacity = 0;
    };
    thread_local SpareOutput tSpareOutput;

}

RcssWriter::Output::~Output(){
    // (kept within kSpareKeptBytes, with the session lists)
    if (capacity_ > tSpareOutput.capacity && SpareStorage::keep(tSpareOutput.capacity, capacity_)){
        tSpareOutput.data = std::move(data_);
        tSpareOutput.capacity = capacity_;
    }
}

void RcssWriter::Output::grow(size_t needed){
    if (capacity_ == 0 && tSpareOutput.capacity >= needed){
        SpareStorage::take(tSpareOutput.capacity);
        data_ = std::move(tSpareOutput.data);
        capacity_ = tSpareOutput.capacity;
        tSpareOutput.capacity = 0;
        return;
    }
    size_t capacity = capacity_ ? capacity_ * 2 : 4096;
    while (capacity < needed)
        capacity *= 2;
    std::unique_ptr<char[]> data(new char[capacity]);
    if (size_ > 0)
        std::memcpy(data.get(), data_.get(), size_);
    data_ = std::move(data);
    capacity_ = capacity;
}

void RcssWriter::line(std::string_view text, SourceRef ref){
    char* to = output.extend(text.size() + 1);
    std::memcpy(to, text.data(), text.size());
    to[text.size()] = '\n';
    map.lines.push_back(ref);
}

void RcssWriter::beginMedia(const RcssMediaCondition& condition, SourceRef ref){
    if (mode != Mode::Sheet)
        return;
    if (ruleOpen)
        endRule();
    if (mediaOpen)
        endMedia();
    line("@media " + condition.text() + " {", ref);
    mediaOpen = true;
    mediaRef = ref;
}

void RcssWriter::endMedia(){
    if (!mediaOpen)
        return;
    if (ruleOpen)
        endRule();
    line("}", mediaRef);
    mediaOpen = false;
}

void RcssWriter::beginRule(std::string_view selectors, SourceRef ref){
    if (mode != Mode::Sheet)
        return;
    if (ruleOpen)
        endRule();
    char* to = output.extend(selectors.size() + 3);
    std::memcpy(to, selectors.data(), selectors.size());
    std::memcpy(to + selectors.size(), " {\n", 3);
    map.lines.push_back(ref);
    ruleOpen = true;
    ruleRef = ref;
}

void RcssWriter::endRule(){
    if (!ruleOpen)
        return;
    line("}", ruleRef);
    ruleOpen = false;
}

void RcssWriter::declaration(std::string_view name, std::string_view value, SourceRef ref){
    if (mode == Mode::DeclarationList){
        if (!output.empty())
            output.push(' ');
    }
    // "name: value;" and the line end written in one growth of the output
    const bool sheet = mode != Mode::DeclarationList;
    char* to = output.extend(name.size() + value.size() + (sheet ? 4 : 3));
    std::memcpy(to, name.data(), name.size());
    to += name.size();
    *to++ = ':';
    *to++ = ' ';
    std::memcpy(to, value.data(), value.size());
    to += value.size();
    *to++ = ';';
    if (!sheet){
        if (map.lines.empty())
            map.lines.push_back(ref);
        return;
    }
    *to = '\n';
    map.lines.push_back(ref);
}

void RcssWriter::atRule(std::string_view text, SourceRef ref){
    if (mode != Mode::Sheet)
        return;
    if (ruleOpen)
        endRule();
    size_t start = 0;
    while (start <= text.size()){
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view piece = text.substr(start, end - start);
        if (!piece.empty() && piece.back() == '\r')
            piece.remove_suffix(1);
        if (!piece.empty() || end < text.size())
            line(piece, ref);
        start = end + 1;
    }
}

uint32_t RcssWriter::lineCount() const{
    return (uint32_t)map.lines.size();
}

std::string RcssWriter::take(RcssLineMap& lines){
    endRule();
    endMedia();
    std::string result = output.take();
    lines = std::move(map);
    map.lines.clear();
    return result;
}

std::string RcssWriter::take(){
    RcssLineMap ignored;
    return take(ignored);
}
