// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SourceFiles.h"

#include <algorithm>

using namespace jasperengine::htmlui::compat;

struct SourceFiles::File {
    std::string path;
    std::string text;
    SourceRef includedFrom;
    std::vector<uint32_t> lineStarts;   // byte offset of every line's first byte
};

namespace {

    const std::string emptyString;

    std::vector<uint32_t> computeLineStarts(const std::string& text){
        std::vector<uint32_t> starts;
        starts.push_back(0);
        const size_t size = text.size();
        for (size_t i = 0; i < size; i++){
            const char c = text[i];
            if (c == '\n'){
                starts.push_back((uint32_t)(i + 1));
            }else if (c == '\r'){
                // "\r\n" ends one line
                if (i + 1 < size && text[i + 1] == '\n')
                    i++;
                starts.push_back((uint32_t)(i + 1));
            }
        }
        return starts;
    }

    bool isLeadByte(unsigned char c){
        return (c & 0xC0) != 0x80;
    }

    bool endsWith(const std::string& text, const char* suffix){
        const size_t length = std::char_traits<char>::length(suffix);
        if (text.size() < length)
            return false;
        for (size_t i = 0; i < length; i++){
            char c = text[text.size() - length + i];
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
            if (c != suffix[i])
                return false;
        }
        return true;
    }

}

SourceFiles::SourceFiles() = default;
SourceFiles::~SourceFiles() = default;
SourceFiles::SourceFiles(SourceFiles&&) noexcept = default;
SourceFiles& SourceFiles::operator=(SourceFiles&&) noexcept = default;

SourceFiles::SourceFiles(const SourceFiles& other){
    *this = other;
}

SourceFiles& SourceFiles::operator=(const SourceFiles& other){
    if (this == &other)
        return *this;
    files.clear();
    files.reserve(other.files.size());
    for (const std::unique_ptr<File>& file : other.files)
        files.push_back(std::make_unique<File>(*file));
    return *this;
}

uint32_t SourceFiles::add(std::string path, std::string text, SourceRef includedFrom){
    if (const uint32_t existing = find(path))
        return existing;

    std::unique_ptr<File> file = std::make_unique<File>();
    file->path = std::move(path);
    file->text = std::move(text);
    file->includedFrom = includedFrom;
    file->lineStarts = computeLineStarts(file->text);
    files.push_back(std::move(file));
    return (uint32_t)files.size();
}

uint32_t SourceFiles::find(std::string_view path) const{
    for (size_t i = 0; i < files.size(); i++){
        if (files[i]->path == path)
            return (uint32_t)(i + 1);
    }
    return 0;
}

size_t SourceFiles::count() const{
    return files.size();
}

const std::string& SourceFiles::path(uint32_t file) const{
    if (file == 0 || file > files.size())
        return emptyString;
    return files[file - 1]->path;
}

std::string_view SourceFiles::text(uint32_t file) const{
    if (file == 0 || file > files.size())
        return std::string_view();
    return files[file - 1]->text;
}

SourcePos SourceFiles::position(uint32_t file, uint32_t offset) const{
    if (file == 0 || file > files.size())
        return SourcePos{};

    const File& source = *files[file - 1];
    if (offset > source.text.size())
        offset = (uint32_t)source.text.size();

    // the last line start <= offset
    const auto it = std::upper_bound(source.lineStarts.begin(), source.lineStarts.end(), offset);
    const size_t lineIndex = (size_t)(it - source.lineStarts.begin()) - 1;
    const uint32_t lineStart = source.lineStarts[lineIndex];

    uint32_t column = 1;
    for (uint32_t i = lineStart; i < offset; i++){
        if (isLeadByte((unsigned char)source.text[i]))
            column++;
    }

    return SourcePos{(uint32_t)lineIndex + 1, column};
}

SourceRef SourceFiles::includedFrom(uint32_t file) const{
    if (file == 0 || file > files.size())
        return SourceRef{};
    return files[file - 1]->includedFrom;
}

uint32_t SourceFiles::codePointLength(SourceRef ref) const{
    const std::string_view slice = this->slice(ref);
    uint32_t count = 0;
    for (char c : slice){
        if (isLeadByte((unsigned char)c))
            count++;
    }
    return count;
}

std::string_view SourceFiles::slice(SourceRef ref) const{
    const std::string_view source = text(ref.file);
    if (ref.offset >= source.size())
        return std::string_view();
    return source.substr(ref.offset, std::min<size_t>(ref.length, source.size() - ref.offset));
}

std::string SourceFiles::originText(uint32_t file) const{
    const SourceRef from = includedFrom(file);
    if (!from.valid())
        return std::string();

    const std::string& parent = path(from.file);
    const SourcePos pos = position(from.file, from.offset);
    const bool linked = endsWith(parent, ".html") || endsWith(parent, ".htm");
    return std::string(linked ? "linked by " : "imported by ") + parent + ":" + std::to_string(pos.line);
}

bool jasperengine::htmlui::compat::stripUtf8Bom(std::string& text){
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF){
        text.erase(0, 3);
        return true;
    }
    return false;
}
