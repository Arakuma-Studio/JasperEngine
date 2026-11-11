// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R47)
// - Purpose: the big-endian byte reading and writing of Photoshop files (PSD, PSB, ASL). The
//   reader bounds every read against the bytes left and remembers the first failure (with the
//   offset), so a parser of an untrusted file checks `ok()` instead of every call and never reads
//   outside its buffer; sub-readers limit a block to its declared length.
// - Thread-safety: values.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace jasperengine::editor::psd {

    class ByteReader {
    public:
        ByteReader() = default;
        ByteReader(const uint8_t* data, size_t size, size_t base = 0) : data(data), size(size), base(base) {}

        bool ok() const { return error.empty(); }
        const std::string& failure() const { return error; }
        // the absolute file offset of the next byte
        size_t offset() const { return base + pos; }
        size_t position() const { return pos; }
        size_t remaining() const { return pos <= size ? size - pos : 0; }
        bool atEnd() const { return remaining() == 0; }

        // records the first failure, naming the offset
        bool fail(const std::string& what) {
            if (error.empty()) error = what + " at offset " + std::to_string(offset());
            pos = size;
            return false;
        }
        bool need(size_t n, const char* what = "the file ends early") {
            if (!ok()) return false;
            if (n > remaining()) return fail(std::string(what) + " (" + std::to_string(n) + " bytes needed, " + std::to_string(remaining()) + " left)");
            return true;
        }

        uint8_t u8() {
            if (!need(1)) return 0;
            return data[pos++];
        }
        uint16_t u16() {
            if (!need(2)) return 0;
            const uint16_t v = (uint16_t)((data[pos] << 8) | data[pos + 1]);
            pos += 2;
            return v;
        }
        int16_t i16() { return (int16_t)u16(); }
        uint32_t u32() {
            if (!need(4)) return 0;
            const uint32_t v = ((uint32_t)data[pos] << 24) | ((uint32_t)data[pos + 1] << 16) | ((uint32_t)data[pos + 2] << 8) | data[pos + 3];
            pos += 4;
            return v;
        }
        int32_t i32() { return (int32_t)u32(); }
        uint64_t u64() {
            const uint64_t hi = u32();
            return (hi << 32) | u32();
        }
        int64_t i64() { return (int64_t)u64(); }
        double f64() {
            const uint64_t bits = u64();
            double v;
            std::memcpy(&v, &bits, sizeof(v));
            return v;
        }
        float f32() {
            const uint32_t bits = u32();
            float v;
            std::memcpy(&v, &bits, sizeof(v));
            return v;
        }
        // a length that must fit in what is left (lengths of blocks, strings, lists)
        uint64_t length(bool wide, const char* what) {
            const uint64_t n = wide ? u64() : u32();
            if (ok() && n > remaining()) {
                fail(std::string(what) + " length " + std::to_string(n) + " passes the end (" + std::to_string(remaining()) + " bytes left)");
                return 0;
            }
            return n;
        }
        std::string fourcc() {
            if (!need(4)) return std::string();
            std::string s(reinterpret_cast<const char*>(data + pos), 4);
            pos += 4;
            return s;
        }
        std::vector<uint8_t> bytes(size_t n) {
            if (!need(n)) return {};
            std::vector<uint8_t> out(data + pos, data + pos + n);
            pos += n;
            return out;
        }
        const uint8_t* view(size_t n) {
            if (!need(n)) return nullptr;
            const uint8_t* p = data + pos;
            pos += n;
            return p;
        }
        void skip(size_t n) {
            if (need(n)) pos += n;
        }
        // a sub-reader over the next `n` bytes (this reader moves past them)
        ByteReader sub(size_t n) {
            if (!need(n)) return ByteReader(nullptr, 0, offset());
            ByteReader r(data + pos, n, offset());
            pos += n;
            return r;
        }
        // Photoshop's Pascal string, padded so that length byte + text is a multiple of `pad`
        std::string pascal(size_t pad) {
            const size_t n = u8();
            std::string s;
            if (const uint8_t* p = view(n)) s.assign(reinterpret_cast<const char*>(p), n);
            const size_t total = n + 1;
            if (pad > 1 && total % pad) skip(pad - total % pad);
            return s;
        }
        // Photoshop's Unicode string (u32 count of UTF-16 code units), as UTF-8 without trailing NULs
        std::string unicode() {
            const uint32_t count = u32();
            if (ok() && (uint64_t)count * 2 > remaining()) {
                fail("a text of " + std::to_string(count) + " characters passes the end");
                return std::string();
            }
            std::string out;
            for (uint32_t i = 0; i < count && ok(); i++) {
                uint32_t c = u16();
                if (c >= 0xD800 && c <= 0xDBFF && i + 1 < count) {
                    const uint32_t low = u16();
                    i++;
                    c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                }
                if (c == 0) continue;
                appendUtf8(out, c);
            }
            return out;
        }

        static void appendUtf8(std::string& out, uint32_t c) {
            if (c < 0x80) {
                out += (char)c;
            } else if (c < 0x800) {
                out += (char)(0xC0 | (c >> 6));
                out += (char)(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                out += (char)(0xE0 | (c >> 12));
                out += (char)(0x80 | ((c >> 6) & 0x3F));
                out += (char)(0x80 | (c & 0x3F));
            } else {
                out += (char)(0xF0 | (c >> 18));
                out += (char)(0x80 | ((c >> 12) & 0x3F));
                out += (char)(0x80 | ((c >> 6) & 0x3F));
                out += (char)(0x80 | (c & 0x3F));
            }
        }

    private:
        const uint8_t* data = nullptr;
        size_t size = 0;
        size_t base = 0;
        size_t pos = 0;
        std::string error;
    };

    class ByteWriter {
    public:
        std::vector<uint8_t> out;

        size_t size() const { return out.size(); }
        void u8(uint8_t v) { out.push_back(v); }
        void u16(uint16_t v) {
            out.push_back((uint8_t)(v >> 8));
            out.push_back((uint8_t)v);
        }
        void i16(int16_t v) { u16((uint16_t)v); }
        void u32(uint32_t v) {
            for (int s = 24; s >= 0; s -= 8) out.push_back((uint8_t)(v >> s));
        }
        void i32(int32_t v) { u32((uint32_t)v); }
        void u64(uint64_t v) {
            u32((uint32_t)(v >> 32));
            u32((uint32_t)v);
        }
        void f64(double v) {
            uint64_t bits;
            std::memcpy(&bits, &v, sizeof(bits));
            u64(bits);
        }
        void fourcc(const char* s) { out.insert(out.end(), s, s + 4); }
        void bytes(const uint8_t* p, size_t n) { out.insert(out.end(), p, p + n); }
        void bytes(const std::vector<uint8_t>& v) { out.insert(out.end(), v.begin(), v.end()); }
        void zeros(size_t n) { out.insert(out.end(), n, 0); }
        void padTo(size_t multiple, size_t from = 0) {
            while ((out.size() - from) % multiple) out.push_back(0);
        }
        // a u32 placeholder; patch() writes the bytes since it as its value
        size_t placeholder32() {
            const size_t at = out.size();
            u32(0);
            return at;
        }
        void patch32(size_t at, uint32_t v) {
            for (int i = 0; i < 4; i++) out[at + i] = (uint8_t)(v >> (24 - 8 * i));
        }
        void patchLength(size_t at) { patch32(at, (uint32_t)(out.size() - at - 4)); }
        // Pascal string, ASCII with '?' for other characters, padded to `pad`
        void pascal(const std::string& utf8, size_t pad);
        // Unicode string (u32 count + UTF-16BE)
        void unicode(const std::string& utf8, bool nulTerminated = false);
    };

    // UTF-8 to UTF-16 code units
    std::u16string utf16Of(const std::string& utf8);

}
