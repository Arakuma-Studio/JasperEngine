// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CookStream.h"

using namespace jasperengine;
using namespace jasperengine::cook;

const std::string Reader::emptyString;

uint32_t StringTable::add(const std::string& text) {
    auto it = index.find(text);
    if (it != index.end()) return it->second;
    const uint32_t id = static_cast<uint32_t>(list.size());
    list.push_back(text);
    index.emplace(text, id);
    return id;
}

void StringTable::clear() {
    list.clear();
    index.clear();
}

Writer::Writer(StringTable& strings, std::function<uint32_t(Entity)> entities): strings(strings), entities(std::move(entities)) {}

void Writer::u32(uint32_t value) {
    for (int i = 0; i < 4; i++) bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

void Writer::u64(uint64_t value) {
    for (int i = 0; i < 8; i++) bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

void Writer::varint(uint64_t value) {
    while (value >= 0x80) {
        bytes.push_back(static_cast<uint8_t>(value | 0x80));
        value >>= 7;
    }
    bytes.push_back(static_cast<uint8_t>(value));
}

void Writer::svarint(int64_t value) {
    varint((static_cast<uint64_t>(value) << 1) ^ static_cast<uint64_t>(value >> 63));
}

void Writer::f32(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    u32(bits);
}

void Writer::f64(double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    u64(bits);
}

void Writer::string(const std::string& value) {
    varint(strings.add(value));
}

void Writer::blob(const void* data, size_t size) {
    varint(size);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    bytes.insert(bytes.end(), p, p + size);
}

void Writer::entity(Entity value) {
    varint(value == NULL_ENTITY ? 0 : entities(value));
}

size_t Writer::beginLength() {
    const size_t at = bytes.size();
    u32(0);
    return at;
}

void Writer::endLength(size_t at) {
    const uint32_t length = static_cast<uint32_t>(bytes.size() - at - 4);
    for (int i = 0; i < 4; i++) bytes[at + i] = static_cast<uint8_t>(length >> (i * 8));
}

Reader::Reader(std::span<const uint8_t> data, const std::vector<std::string>& strings, std::function<Entity(uint32_t)> entities):
    bytes(data), strings(&strings), entities(std::move(entities)) {}

void Reader::fail(const std::string& why) {
    if (!failure) {
        failure = true;
        message = why;
    }
    position = bytes.size();
}

bool Reader::take(size_t count) {
    if (failure) return false;
    if (count > bytes.size() - position) {
        fail("truncated (" + std::to_string(count) + " bytes asked at " + std::to_string(position) + " of " + std::to_string(bytes.size()) + ")");
        return false;
    }
    return true;
}

uint8_t Reader::u8() {
    if (!take(1)) return 0;
    return bytes[position++];
}

uint32_t Reader::u32() {
    if (!take(4)) return 0;
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) value |= static_cast<uint32_t>(bytes[position + i]) << (i * 8);
    position += 4;
    return value;
}

uint64_t Reader::u64() {
    if (!take(8)) return 0;
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) value |= static_cast<uint64_t>(bytes[position + i]) << (i * 8);
    position += 8;
    return value;
}

uint64_t Reader::varint() {
    uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (!take(1)) return 0;
        const uint8_t byte = bytes[position++];
        value |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if (!(byte & 0x80)) return value;
    }
    fail("varint longer than 64 bits");
    return 0;
}

int64_t Reader::svarint() {
    const uint64_t raw = varint();
    return static_cast<int64_t>(raw >> 1) ^ -static_cast<int64_t>(raw & 1);
}

float Reader::f32() {
    const uint32_t bits = u32();
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

double Reader::f64() {
    const uint64_t bits = u64();
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

const std::string& Reader::string() {
    const uint64_t index = varint();
    if (failure) return emptyString;
    if (index >= strings->size()) {
        fail("string index " + std::to_string(index) + " outside the string table");
        return emptyString;
    }
    return (*strings)[static_cast<size_t>(index)];
}

std::span<const uint8_t> Reader::blob() {
    const uint64_t size = varint();
    if (!take(static_cast<size_t>(size))) return {};
    std::span<const uint8_t> out = bytes.subspan(position, static_cast<size_t>(size));
    position += static_cast<size_t>(size);
    return out;
}

Entity Reader::entity() {
    const uint64_t ref = varint();
    if (failure || ref == 0) return NULL_ENTITY;
    return entities(static_cast<uint32_t>(ref));
}

Reader Reader::record() {
    const uint32_t length = u32();
    if (!take(length)) return Reader({}, *strings, entities);
    Reader inner(bytes.subspan(position, length), *strings, entities);
    position += length;
    return inner;
}

void Reader::skip(size_t count) {
    if (take(count)) position += count;
}
