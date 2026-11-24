// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "WindowsResources.h"

#include "stb_image.h"
#include "stb_image_resize2.h"
#include "stb_image_write.h"

#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::platform;
namespace fs = std::filesystem;

namespace {

    void put16(std::vector<uint8_t>& out, uint16_t v) {
        out.push_back(static_cast<uint8_t>(v));
        out.push_back(static_cast<uint8_t>(v >> 8));
    }
    void put32(std::vector<uint8_t>& out, uint32_t v) {
        for (int i = 0; i < 4; i++) out.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    void pad32(std::vector<uint8_t>& out) {
        while (out.size() % 4) out.push_back(0);
    }
    void putWide(std::vector<uint8_t>& out, const std::string& text) {
        // the version strings are ASCII (UTF-8 above it is written as Latin-1 code units)
        for (unsigned char c : text) put16(out, c);
        put16(out, 0);
    }

    // a version block: wLength, wValueLength, wType, key, padding, value, padding, children
    struct Block {
        std::string key;
        bool text = false;
        std::vector<uint8_t> value;   // raw bytes (binary) or the UTF-16 text
        uint16_t valueLength = 0;     // bytes (binary) or characters with the terminator (text)
        std::vector<Block> children;

        std::vector<uint8_t> bytes() const {
            std::vector<uint8_t> out;
            put16(out, 0);
            put16(out, valueLength);
            put16(out, text ? 1 : 0);
            putWide(out, key);
            pad32(out);
            out.insert(out.end(), value.begin(), value.end());
            pad32(out);
            for (const Block& child : children) {
                const std::vector<uint8_t> inner = child.bytes();
                out.insert(out.end(), inner.begin(), inner.end());
                pad32(out);
            }
            const uint16_t length = static_cast<uint16_t>(out.size());
            out[0] = static_cast<uint8_t>(length);
            out[1] = static_cast<uint8_t>(length >> 8);
            return out;
        }
    };

    Block stringBlock(const std::string& key, const std::string& value) {
        Block block;
        block.key = key;
        block.text = true;
        putWide(block.value, value);
        block.valueLength = static_cast<uint16_t>(value.size() + 1);
        return block;
    }

    void versionParts(const std::string& version, uint16_t parts[4]) {
        for (int i = 0; i < 4; i++) parts[i] = 0;
        int index = 0;
        unsigned long value = 0;
        bool digits = false;
        for (char c : version + ".") {
            if (c >= '0' && c <= '9') {
                value = value * 10 + static_cast<unsigned long>(c - '0');
                digits = true;
            } else if (c == '.') {
                if (index < 4 && digits) parts[index] = static_cast<uint16_t>(value);
                index++;
                value = 0;
                digits = false;
            }
        }
    }

}

bool platform::canPatchExecutableResources() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

std::vector<uint8_t> platform::buildVersionResource(const WindowsVersionInfo& info) {
    uint16_t file[4], product[4];
    versionParts(info.fileVersion.empty() ? "1.0.0.0" : info.fileVersion, file);
    versionParts(info.productVersion.empty() ? info.fileVersion : info.productVersion, product);
    Block root;
    root.key = "VS_VERSION_INFO";
    // VS_FIXEDFILEINFO
    put32(root.value, 0xFEEF04BD);
    put32(root.value, 0x00010000);
    put32(root.value, (uint32_t(file[0]) << 16) | file[1]);
    put32(root.value, (uint32_t(file[2]) << 16) | file[3]);
    put32(root.value, (uint32_t(product[0]) << 16) | product[1]);
    put32(root.value, (uint32_t(product[2]) << 16) | product[3]);
    put32(root.value, 0x3F);         // file flags mask
    put32(root.value, 0);            // file flags
    put32(root.value, 0x00040004);   // VOS_NT_WINDOWS32
    put32(root.value, 0x1);          // VFT_APP
    put32(root.value, 0);
    put32(root.value, 0);
    put32(root.value, 0);
    root.valueLength = static_cast<uint16_t>(root.value.size());

    Block table;
    table.key = "040904b0";
    table.text = true;
    auto text = [](const uint16_t parts[4]) {
        return std::to_string(parts[0]) + "." + std::to_string(parts[1]) + "." + std::to_string(parts[2]) + "." + std::to_string(parts[3]);
    };
    if (!info.companyName.empty()) table.children.push_back(stringBlock("CompanyName", info.companyName));
    table.children.push_back(stringBlock("FileDescription", info.productName));
    table.children.push_back(stringBlock("FileVersion", text(file)));
    table.children.push_back(stringBlock("ProductName", info.productName));
    table.children.push_back(stringBlock("ProductVersion", text(product)));
    Block strings;
    strings.key = "StringFileInfo";
    strings.text = true;
    strings.children.push_back(table);
    Block translation;
    translation.key = "Translation";
    put16(translation.value, 0x0409);
    put16(translation.value, 0x04b0);
    translation.valueLength = 4;
    Block vars;
    vars.key = "VarFileInfo";
    vars.text = true;
    vars.children.push_back(translation);
    root.children = {strings, vars};
    return root.bytes();
}

bool platform::buildIconResources(const fs::path& png, std::vector<uint8_t>& group, std::vector<std::vector<uint8_t>>& icons, std::string& error) {
    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load(png.string().c_str(), &width, &height, &channels, 4);
    if (!pixels) {
        error = "cannot read the icon " + png.string();
        return false;
    }
    const int sizes[] = {16, 32, 48, 256};
    group.clear();
    icons.clear();
    put16(group, 0);
    put16(group, 1);   // icons
    put16(group, static_cast<uint16_t>(std::size(sizes)));
    for (size_t i = 0; i < std::size(sizes); i++) {
        const int size = sizes[i];
        std::vector<unsigned char> scaled(static_cast<size_t>(size) * size * 4);
        stbir_resize_uint8_srgb(pixels, width, height, 0, scaled.data(), size, size, 0, STBIR_RGBA);
        std::vector<uint8_t> encoded;
        stbi_write_png_to_func([](void* context, void* data, int length) {
            auto* out = static_cast<std::vector<uint8_t>*>(context);
            out->insert(out->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + length);
        }, &encoded, size, size, 4, scaled.data(), size * 4);
        // GRPICONDIRENTRY: width, height (0 = 256), colours, reserved, planes, bits, bytes, id
        group.push_back(static_cast<uint8_t>(size >= 256 ? 0 : size));
        group.push_back(static_cast<uint8_t>(size >= 256 ? 0 : size));
        group.push_back(0);
        group.push_back(0);
        put16(group, 1);
        put16(group, 32);
        put32(group, static_cast<uint32_t>(encoded.size()));
        put16(group, static_cast<uint16_t>(i + 1));
        icons.push_back(std::move(encoded));
    }
    stbi_image_free(pixels);
    return true;
}

bool platform::patchExecutableResources(const fs::path& exe, const fs::path& iconPng, const WindowsVersionInfo& info, std::string& error) {
#ifdef _WIN32
    std::vector<uint8_t> group;
    std::vector<std::vector<uint8_t>> icons;
    std::error_code ec;
    const bool icon = !iconPng.empty() && fs::is_regular_file(iconPng, ec);
    if (icon && !buildIconResources(iconPng, group, icons, error)) return false;
    const std::vector<uint8_t> version = buildVersionResource(info);
    HANDLE update = BeginUpdateResourceW(exe.wstring().c_str(), FALSE);
    if (!update) {
        error = "BeginUpdateResourceW failed on " + exe.string() + " (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    const WORD language = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
    // RT_* are MAKEINTRESOURCE ids, the narrow kind outside UNICODE builds: the same integer ids
    // as the wide ones
    const auto wideType = [](auto type) { return reinterpret_cast<LPCWSTR>(type); };
    bool ok = true;
    if (icon) {
        for (size_t i = 0; ok && i < icons.size(); i++)
            ok = UpdateResourceW(update, wideType(RT_ICON), MAKEINTRESOURCEW(i + 1), language, icons[i].data(), static_cast<DWORD>(icons[i].size()));
        ok = ok && UpdateResourceW(update, wideType(RT_GROUP_ICON), MAKEINTRESOURCEW(1), language, group.data(), static_cast<DWORD>(group.size()));
    }
    ok = ok && UpdateResourceW(update, wideType(RT_VERSION), MAKEINTRESOURCEW(1), MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                               const_cast<uint8_t*>(version.data()), static_cast<DWORD>(version.size()));
    if (!EndUpdateResourceW(update, ok ? FALSE : TRUE) || !ok) {
        error = "UpdateResourceW failed on " + exe.string() + " (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
#else
    (void)exe;
    (void)iconPng;
    (void)info;
    error = "Windows resources are patched on a Windows host only";
    return false;
#endif
}
