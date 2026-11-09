// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PsdBytes.h"

using namespace jasperengine::editor::psd;

std::u16string jasperengine::editor::psd::utf16Of(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        uint32_t cp = c;
        int extra = 0;
        if (c >= 0xF0) { cp = c & 0x07; extra = 3; }
        else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
        else if (c >= 0xC0) { cp = c & 0x1F; extra = 1; }
        else if (c >= 0x80) { cp = 0xFFFD; }
        i++;
        for (int k = 0; k < extra && i < s.size(); k++, i++) cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += (char16_t)(0xD800 + (cp >> 10));
            out += (char16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out += (char16_t)cp;
        }
    }
    return out;
}

void ByteWriter::pascal(const std::string& utf8, size_t pad) {
    std::string ascii;
    for (size_t i = 0; i < utf8.size() && ascii.size() < 255; i++) {
        const unsigned char c = (unsigned char)utf8[i];
        if (c < 0x80) {
            ascii += (char)c;
        } else if ((c & 0xC0) != 0x80) {
            ascii += '?';
        }
    }
    u8((uint8_t)ascii.size());
    bytes(reinterpret_cast<const uint8_t*>(ascii.data()), ascii.size());
    const size_t total = ascii.size() + 1;
    if (pad > 1 && total % pad) zeros(pad - total % pad);
}

void ByteWriter::unicode(const std::string& utf8, bool nulTerminated) {
    const std::u16string text = utf16Of(utf8);
    u32((uint32_t)(text.size() + (nulTerminated ? 1 : 0)));
    for (char16_t c : text) u16((uint16_t)c);
    if (nulTerminated) u16(0);
}
