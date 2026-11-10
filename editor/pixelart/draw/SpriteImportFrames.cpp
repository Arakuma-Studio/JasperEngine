// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteImportFrames.h"

#include "miniz.h"
#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace jasperengine::editor::pixelart::draw {

    namespace {

        const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

        uint32_t readBe32(const uint8_t* p) {
            return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
        }

        uint16_t readBe16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

        void appendBe32(std::vector<uint8_t>& out, uint32_t v) {
            for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>(v >> s));
        }

        void appendChunk(std::vector<uint8_t>& out, const char* type, const uint8_t* data, size_t size) {
            appendBe32(out, static_cast<uint32_t>(size));
            const size_t at = out.size();
            out.insert(out.end(), type, type + 4);
            out.insert(out.end(), data, data + size);
            appendBe32(out, static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, out.data() + at, size + 4)));
        }

        struct ApngFrame {
            int w = 0, h = 0, x = 0, y = 0;
            int delayNum = 0, delayDen = 100;
            int dispose = 0, blend = 0;
            std::vector<uint8_t> idat;
        };

        uint16_t delayMs(int num, int den) {
            if (den <= 0) den = 100;
            const int ms = num * 1000 / den;
            return static_cast<uint16_t>(std::clamp(ms == 0 ? 100 : ms, 1, 65535));
        }

        bool decodeApng(const std::vector<uint8_t>& bytes, ImportedFrames& out, PixelProblem& problem) {
            std::vector<ApngFrame> frames;
            std::vector<std::vector<uint8_t>> shared;   // PLTE, tRNS and the like: every frame's own PNG repeats them
            std::vector<uint8_t> ihdr;
            ApngFrame* current = nullptr;
            bool animated = false;
            size_t pos = 8;
            while (pos + 12 <= bytes.size()) {
                const uint32_t length = readBe32(&bytes[pos]);
                if (length > bytes.size() - pos - 12) break;
                const char* type = reinterpret_cast<const char*>(&bytes[pos + 4]);
                const uint8_t* data = &bytes[pos + 8];
                if (std::memcmp(type, "IHDR", 4) == 0) {
                    ihdr.assign(data, data + length);
                } else if (std::memcmp(type, "acTL", 4) == 0) {
                    animated = true;
                } else if (std::memcmp(type, "fcTL", 4) == 0 && length >= 26) {
                    ApngFrame frame;
                    frame.w = static_cast<int>(readBe32(data + 4));
                    frame.h = static_cast<int>(readBe32(data + 8));
                    frame.x = static_cast<int>(readBe32(data + 12));
                    frame.y = static_cast<int>(readBe32(data + 16));
                    frame.delayNum = readBe16(data + 20);
                    frame.delayDen = readBe16(data + 22);
                    frame.dispose = data[24];
                    frame.blend = data[25];
                    frames.push_back(std::move(frame));
                    current = &frames.back();
                } else if (std::memcmp(type, "IDAT", 4) == 0) {
                    if (current) current->idat.insert(current->idat.end(), data, data + length);
                } else if (std::memcmp(type, "fdAT", 4) == 0 && length >= 4) {
                    if (current) current->idat.insert(current->idat.end(), data + 4, data + length);
                } else if (std::memcmp(type, "IEND", 4) != 0 && frames.empty()) {
                    shared.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(pos), bytes.begin() + static_cast<std::ptrdiff_t>(pos + 12 + length));
                }
                pos += 12 + length;
            }
            std::string error;
            std::optional<PixelImage> plain = PixelImage::decodePng(bytes, &error);
            if (!plain) {
                problem = {"not_a_sprite", "from", "the PNG does not decode: " + error, "pass a valid .png, .gif or animated PNG"};
                return false;
            }
            if (!animated || frames.empty()) {
                out.frames.push_back(std::move(*plain));
                out.durations.push_back(100);
                return true;
            }
            const int w = plain->width(), h = plain->height();
            PixelImage canvas(w, h);
            for (const ApngFrame& frame : frames) {
                std::vector<uint8_t> single(kSignature, kSignature + 8);
                std::vector<uint8_t> header = ihdr;
                for (int i = 0; i < 4; i++) {
                    header[static_cast<size_t>(i)] = static_cast<uint8_t>(uint32_t(frame.w) >> (24 - i * 8));
                    header[static_cast<size_t>(i + 4)] = static_cast<uint8_t>(uint32_t(frame.h) >> (24 - i * 8));
                }
                appendChunk(single, "IHDR", header.data(), header.size());
                for (const std::vector<uint8_t>& chunk : shared) single.insert(single.end(), chunk.begin(), chunk.end());
                appendChunk(single, "IDAT", frame.idat.data(), frame.idat.size());
                appendChunk(single, "IEND", nullptr, 0);
                std::optional<PixelImage> image = PixelImage::decodePng(single, &error);
                if (!image) {
                    problem = {"not_a_sprite", "from", "an APNG frame does not decode: " + error, "re-export the animation"};
                    return false;
                }
                const PixelImage before = canvas;
                if (frame.blend == 0) canvas.blit(*image, frame.x, frame.y);
                else canvas.blendOver(*image, frame.x, frame.y, 255);
                out.frames.push_back(canvas);
                out.durations.push_back(delayMs(frame.delayNum, frame.delayDen));
                if (frame.dispose == 1) {
                    canvas.blit(PixelImage(frame.w, frame.h), frame.x, frame.y);
                } else if (frame.dispose == 2) {
                    canvas = before;
                }
            }
            return true;
        }

        // R5 checked from the file's header (and its frame count, where it says one), before
        // decoding what could be gigabytes
        bool headerFits(const std::vector<uint8_t>& bytes, bool gif, PixelProblem& problem) {
            uint32_t w = 0, h = 0, frames = 1;
            if (gif) {
                if (bytes.size() < 13) return true;   // the decoder reports what is wrong
                w = static_cast<uint32_t>(bytes[6]) | (static_cast<uint32_t>(bytes[7]) << 8);
                h = static_cast<uint32_t>(bytes[8]) | (static_cast<uint32_t>(bytes[9]) << 8);
                // the frames: count the image descriptors, block by block
                size_t pos = 13;
                if (bytes[10] & 0x80) pos += 3u << ((bytes[10] & 7) + 1);
                frames = 0;
                while (pos < bytes.size()) {
                    const uint8_t tag = bytes[pos];
                    if (tag == 0x3B) break;
                    if (tag == 0x2C) {
                        if (pos + 10 > bytes.size()) break;
                        frames++;
                        const uint8_t flags = bytes[pos + 9];
                        pos += 10;
                        if (flags & 0x80) pos += 3u << ((flags & 7) + 1);
                        pos += 1;   // LZW minimum code size
                    } else if (tag == 0x21) {
                        pos += 2;
                    } else {
                        break;
                    }
                    while (pos < bytes.size() && bytes[pos] != 0) pos += bytes[pos] + 1u;
                    pos += 1;
                }
                if (frames == 0) frames = 1;
            } else {
                if (bytes.size() < 8 || std::memcmp(bytes.data(), kSignature, 8) != 0) return true;
                size_t pos = 8;
                while (pos + 12 <= bytes.size()) {
                    const uint32_t length = readBe32(&bytes[pos]);
                    if (length > bytes.size() - pos - 12) break;
                    const char* type = reinterpret_cast<const char*>(&bytes[pos + 4]);
                    if (std::memcmp(type, "IHDR", 4) == 0 && length >= 8) {
                        w = readBe32(&bytes[pos + 8]);
                        h = readBe32(&bytes[pos + 12]);
                    } else if (std::memcmp(type, "acTL", 4) == 0 && length >= 4) {
                        frames = readBe32(&bytes[pos + 8]);
                    } else if (std::memcmp(type, "IDAT", 4) == 0) {
                        break;
                    }
                    pos += 12 + length;
                }
            }
            if (w > 4096 || h > 4096) {
                problem = {"canvas_too_large", "from", "the image is " + std::to_string(w) + "x" + std::to_string(h) + ", above 4096x4096", "each side is 1-4096"};
                return false;
            }
            if (frames > 256) {
                problem = {"bad_op_field", "from", "the image has " + std::to_string(frames) + " frames, above 256", "frames is 1-256"};
                return false;
            }
            // what a decoder would hold while it composes the frames
            if (static_cast<uint64_t>(w) * h * frames * 4 > (uint64_t(1) << 30)) {
                problem = {"region_too_large", "from", "the frames would decode to more than 1 GiB", "use fewer or smaller frames"};
                return false;
            }
            return true;
        }

        bool decodeGif(const std::vector<uint8_t>& bytes, ImportedFrames& out, PixelProblem& problem) {
            int* delays = nullptr;
            int w = 0, h = 0, count = 0, comp = 0;
            stbi_uc* pixels = stbi_load_gif_from_memory(bytes.data(), static_cast<int>(bytes.size()), &delays, &w, &h, &count, &comp, 4);
            if (!pixels || w <= 0 || h <= 0 || count <= 0) {
                if (pixels) stbi_image_free(pixels);
                if (delays) stbi_image_free(delays);
                problem = {"not_a_sprite", "from", "the GIF does not decode", "pass a valid .gif, .png or animated PNG"};
                return false;
            }
            for (int f = 0; f < count; f++) {
                PixelImage frame(w, h);
                std::memcpy(frame.editPixels().data(), pixels + static_cast<size_t>(f) * w * h * 4, static_cast<size_t>(w) * h * 4);
                out.frames.push_back(std::move(frame));
                const int delay = delays ? delays[f] : 100;
                out.durations.push_back(static_cast<uint16_t>(std::clamp(delay <= 0 ? 100 : delay, 1, 65535)));
            }
            stbi_image_free(pixels);
            if (delays) stbi_image_free(delays);
            return true;
        }

    }

    bool readFramesFile(const std::filesystem::path& path, ImportedFrames& out, PixelProblem& problem) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            problem = {"sprite_missing", "from", "no file " + path.generic_string(), "pass a project-relative .png or .gif path"};
            return false;
        }
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string ext = path.extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        out = ImportedFrames();
        if ((ext == ".gif" || ext == ".png" || ext == ".apng") && !headerFits(bytes, ext == ".gif", problem)) return false;
        if (ext == ".gif") return decodeGif(bytes, out, problem);
        if (ext == ".png" || ext == ".apng") return decodeApng(bytes, out, problem);
        problem = {"not_a_sprite", "from", path.filename().generic_string() + " is not a .png, .apng or .gif", "pass a .png, .gif or animated PNG"};
        return false;
    }

}
