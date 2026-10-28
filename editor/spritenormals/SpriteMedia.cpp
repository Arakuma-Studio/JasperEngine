// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Sources of the sprite normal generator. Stills go through texsynth's stb_image loader; GIF and
// APNG have their own parsers here (stb's GIF loader restores instead of clearing on disposal 2,
// has no loop count and decodes every frame before a limit could apply), composited per the GIF89a
// and APNG specifications and packed like upstream's media.rs pack_frames.

#include "SpriteMedia.h"

#include "stb_image.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <new>
#include <numeric>
#include <system_error>

namespace fs = std::filesystem;

namespace jasperengine::editor::spritenormals {

    namespace {

        const char* const kFormatFix = "Use a PNG (or JPEG/TGA/BMP) image, a GIF or an APNG.";

        // "<name>: " in front of messages about a named file
        struct Errors {
            std::string name;
            std::vector<Problem>* problems = nullptr;

            bool fail(const std::string& kind, const std::string& message, const std::string& fix = "") const {
                problems->push_back({kind, "source", (name.empty() ? std::string() : name + ": ") + message, fix});
                return false;
            }
            bool invalid(const std::string& message) const { return fail("invalid_source", message, kFormatFix); }
        };

        std::string sizeText(int64_t w, int64_t h) { return std::to_string(w) + "x" + std::to_string(h); }

        uint32_t gcd32(uint32_t a, uint32_t b) {
            while (b) {
                const uint32_t t = a % b;
                a = b;
                b = t;
            }
            return a;
        }

        Delay reducedDelay(uint32_t numerator, uint32_t denominator) {
            Delay d;
            const uint32_t g = gcd32(numerator, denominator);
            d.numerator = g ? numerator / g : numerator;
            d.denominator = g ? denominator / g : denominator;
            if (numerator == 0) d.denominator = 1;
            return d;
        }

        const uint32_t* crcTable() {
            static const std::array<uint32_t, 256> table = [] {
                std::array<uint32_t, 256> t{};
                for (uint32_t n = 0; n < 256; n++) {
                    uint32_t c = n;
                    for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    t[n] = c;
                }
                return t;
            }();
            return table.data();
        }

        uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
            const uint32_t* table = crcTable();
            crc = ~crc;
            for (size_t i = 0; i < size; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            return ~crc;
        }

        inline uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
        inline uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
        inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

        void putBe32(std::vector<uint8_t>& out, uint32_t v) {
            out.push_back(static_cast<uint8_t>(v >> 24));
            out.push_back(static_cast<uint8_t>(v >> 16));
            out.push_back(static_cast<uint8_t>(v >> 8));
            out.push_back(static_cast<uint8_t>(v));
        }

        void putChunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, size_t size) {
            putBe32(out, static_cast<uint32_t>(size));
            const size_t start = out.size();
            out.insert(out.end(), type, type + 4);
            if (size) out.insert(out.end(), data, data + size);
            putBe32(out, crc32(out.data() + start, size + 4));
        }

        const uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

        bool isGif(const uint8_t* b, size_t n) { return n >= 6 && (std::memcmp(b, "GIF87a", 6) == 0 || std::memcmp(b, "GIF89a", 6) == 0); }
        bool isPng(const uint8_t* b, size_t n) { return n >= 8 && std::memcmp(b, kPngSignature, 8) == 0; }

        // A PNG with an acTL chunk before the first IDAT (without CRC checks: detection only)
        bool isApng(const uint8_t* b, size_t n) {
            if (!isPng(b, n)) return false;
            size_t pos = 8;
            while (pos + 8 <= n) {
                const uint32_t length = be32(b + pos);
                const uint8_t* type = b + pos + 4;
                if (std::memcmp(type, "acTL", 4) == 0) return true;
                if (std::memcmp(type, "IDAT", 4) == 0 || std::memcmp(type, "IEND", 4) == 0) return false;
                if (length > n - pos - 8) return false;
                pos += 12 + static_cast<size_t>(length);
            }
            return false;
        }

        bool checkCanvas(const Errors& e, const char* what, int64_t w, int64_t h) {
            if (w <= 0 || h <= 0) return e.invalid(std::string("the ") + what + " is " + sizeText(w, h) + " pixels");
            if (w * h > kMaxPixels) {
                return e.fail("too_large",
                              std::string("the ") + what + " is " + sizeText(w, h) + " (" + std::to_string(w * h) + " pixels); at most " +
                                  std::to_string(kMaxPixels) + " pixels (4096 x 4096) are allowed",
                              "Scale it down or split it.");
            }
            return true;
        }

        // The pixels of the atlas of `frames` frames (columns = ceil(sqrt(n)), rows = ceil(n / columns))
        int64_t atlasPixels(int64_t frames, int64_t framePixels) {
            int64_t c = 1;
            while (c * c < frames) c++;
            return framePixels * c * ((frames + c - 1) / c);
        }

        // Before decoding frame `decoded` (0-based): the frame count, the decoded pixels and the atlas
        bool checkFrameLimits(const Errors& e, int decoded, int64_t canvasPixels) {
            if (decoded >= kMaxFrames) {
                return e.fail("too_large",
                              "the animation has more than " + std::to_string(kMaxFrames) + " frames (frame " + std::to_string(decoded + 1) +
                                  " found); at most " + std::to_string(kMaxFrames) + " are allowed",
                              "Trim the animation.");
            }
            if (static_cast<int64_t>(decoded + 1) * canvasPixels > kMaxPixels) {
                return e.fail("too_large",
                              "frame " + std::to_string(decoded + 1) + " would bring the decoded frames to " +
                                  std::to_string(static_cast<int64_t>(decoded + 1) * canvasPixels) + " pixels; at most " +
                                  std::to_string(kMaxPixels) + " are allowed",
                              "Trim the animation or scale it down.");
            }
            // the atlas of the frames so far (unused cells included) must fit too: refused before
            // decoding the frame that would make it too big, not after all of them
            const int64_t atlas = atlasPixels(decoded + 1, canvasPixels);
            if (atlas > kMaxPixels) {
                return e.fail("too_large",
                              "with frame " + std::to_string(decoded + 1) + " the atlas would hold " + std::to_string(atlas) + " pixels; at most " +
                                  std::to_string(kMaxPixels) + " are allowed",
                              "Trim the animation or scale it down.");
            }
            return true;
        }

        bool finishAnimation(const Errors& e, SourceKind kind, int width, int height, std::vector<Image>& frames, std::vector<Delay>& delays,
                             int loopCount, Source& out) {
            if (frames.empty()) return e.invalid("the animation has no frame");
            std::string error;
            Source s;
            s.kind = kind;
            if (!packAtlas(frames, s.color, s.columns, s.rows, s.cells, error)) return e.fail("too_large", error, "Trim the animation or scale it down.");
            s.frameWidth = width;
            s.frameHeight = height;
            s.frameCount = static_cast<int>(frames.size());
            s.delays = std::move(delays);
            s.loopCount = loopCount;
            s.fileWidth = width;
            s.fileHeight = height;
            s.fileChannels = 4;
            s.fileBitDepth = out.fileBitDepth;
            s.sha1 = out.sha1;
            out = std::move(s);
            return true;
        }

        // ---------------------------------------------------------------- GIF

        struct GifFrameInfo {
            int left = 0, top = 0, width = 0, height = 0;
            bool interlaced = false;
            const uint8_t* palette = nullptr;   // local, else global, else null
            int paletteSize = 0;
            int disposal = 0;
            bool transparent = false;
            int transparentIndex = 0;
            int delayCs = 0;
        };

        // Walks the GIF block structure. For each image descriptor `onFrame(info, lzwStart)` gets
        // the position of its LZW minimum code size byte; it returns false to stop (an error was
        // reported). Sub-blocks are skipped here, never decoded.
        struct GifWalker {
            const uint8_t* b;
            size_t n;
            Errors e;
            int width = 0, height = 0;
            const uint8_t* globalPalette = nullptr;
            int globalSize = 0;
            bool loopFound = false;
            int loopCount = 1;

            bool header() {
                if (n < 13) return e.invalid("the GIF header is cut short");
                width = le16(b + 6);
                height = le16(b + 8);
                const uint8_t packed = b[10];
                size_t pos = 13;
                if (packed & 0x80) {
                    globalSize = 1 << ((packed & 7) + 1);
                    if (pos + 3 * static_cast<size_t>(globalSize) > n) return e.invalid("the GIF colour table is cut short");
                    globalPalette = b + pos;
                }
                return checkCanvas(e, "GIF canvas", width, height);
            }

            size_t firstBlock() const { return 13 + (globalPalette ? 3 * static_cast<size_t>(globalSize) : 0); }

            // Skips sub-blocks from `pos` (its size byte) to after the terminator; false when cut short
            bool skipSubBlocks(size_t& pos) const {
                while (pos < n) {
                    const uint8_t size = b[pos];
                    pos += 1;
                    if (size == 0) return true;
                    pos += size;
                }
                return false;
            }

            template <typename OnFrame>
            bool walk(OnFrame onFrame, int& frames) {
                frames = 0;
                size_t pos = firstBlock();
                GifFrameInfo pending;
                while (true) {
                    if (pos >= n) {
                        // a missing trailer after a frame is accepted, as browsers do
                        if (frames > 0) return true;
                        return e.invalid("the GIF ends before its first frame (offset " + std::to_string(pos) + ")");
                    }
                    const uint8_t introducer = b[pos];
                    if (introducer == 0x3B) {
                        if (frames == 0) return e.invalid("the GIF has no frame");
                        return true;
                    }
                    if (introducer == 0x21) {
                        if (pos + 2 > n) return frames > 0 ? true : e.invalid("the GIF is cut short (offset " + std::to_string(pos) + ")");
                        const uint8_t label = b[pos + 1];
                        size_t sub = pos + 2;
                        if (label == 0xF9 && sub + 5 <= n && b[sub] >= 4) {
                            const uint8_t packed = b[sub + 1];
                            pending.disposal = (packed >> 2) & 7;
                            pending.transparent = (packed & 1) != 0;
                            pending.delayCs = le16(b + sub + 2);
                            pending.transparentIndex = b[sub + 4];
                        } else if (label == 0xFF && sub + 12 <= n && b[sub] == 11 &&
                                   (std::memcmp(b + sub + 1, "NETSCAPE2.0", 11) == 0 || std::memcmp(b + sub + 1, "ANIMEXTS1.0", 11) == 0)) {
                            const size_t data = sub + 12;
                            if (data + 4 <= n && b[data] >= 3 && b[data + 1] == 1) {
                                loopFound = true;
                                loopCount = le16(b + data + 2);
                            }
                        }
                        if (!skipSubBlocks(sub)) {
                            if (frames > 0) return true;
                            return e.invalid("an extension block is cut short (offset " + std::to_string(pos) + ")");
                        }
                        pos = sub;
                        continue;
                    }
                    if (introducer == 0x2C) {
                        if (pos + 10 > n) return frames > 0 ? true : e.invalid("an image descriptor is cut short (offset " + std::to_string(pos) + ")");
                        GifFrameInfo info = pending;
                        pending = GifFrameInfo();
                        info.left = le16(b + pos + 1);
                        info.top = le16(b + pos + 3);
                        info.width = le16(b + pos + 5);
                        info.height = le16(b + pos + 7);
                        const uint8_t packed = b[pos + 9];
                        info.interlaced = (packed & 0x40) != 0;
                        size_t data = pos + 10;
                        if (packed & 0x80) {
                            info.paletteSize = 1 << ((packed & 7) + 1);
                            if (data + 3 * static_cast<size_t>(info.paletteSize) > n) {
                                return e.invalid("the colour table of frame " + std::to_string(frames + 1) + " is cut short (offset " + std::to_string(data) + ")");
                            }
                            info.palette = b + data;
                            data += 3 * static_cast<size_t>(info.paletteSize);
                        } else {
                            info.palette = globalPalette;
                            info.paletteSize = globalSize;
                        }
                        if (data >= n) return e.invalid("the data of frame " + std::to_string(frames + 1) + " is missing (offset " + std::to_string(data) + ")");
                        if (!onFrame(info, data, frames)) return false;
                        frames++;
                        size_t sub = data + 1;
                        if (!skipSubBlocks(sub)) {
                            // the last frame's data may run to the end of a file without trailer;
                            // onFrame has decoded (or counted) it already
                            return true;
                        }
                        pos = sub;
                        continue;
                    }
                    return e.invalid("unknown GIF block 0x" + [&] {
                        char h[8];
                        std::snprintf(h, sizeof(h), "%02x", introducer);
                        return std::string(h);
                    }() + " (offset " + std::to_string(pos) + ")");
                }
            }
        };

        // LZW-decodes the image data at `start` (minimum code size byte, then sub-blocks) of a
        // frame of `frameWidth` x `rowsInFile` pixels. Only the part inside the canvas is kept:
        // pixel x of the file's row r goes to visible[destRow[r] * visibleWidth + x] when x <
        // visibleWidth and destRow[r] >= 0 (the codes of the clipped pixels are still consumed).
        bool gifLzw(const uint8_t* b, size_t n, size_t start, int frameWidth, const std::vector<int>& destRow, int visibleWidth,
                    std::vector<uint8_t>& visible, const Errors& e, int frame) {
            const int minCodeSize = b[start];
            if (minCodeSize < 2 || minCodeSize > 8) {
                return e.invalid("frame " + std::to_string(frame) + " has an LZW code size of " + std::to_string(minCodeSize) +
                                 " (2..8 are valid; offset " + std::to_string(start) + ")");
            }
            const size_t count = static_cast<size_t>(frameWidth) * destRow.size();
            auto store = [&](size_t p, uint8_t value) {
                const size_t r = p / static_cast<size_t>(frameWidth), x = p % static_cast<size_t>(frameWidth);
                const int row = destRow[r];
                if (row >= 0 && x < static_cast<size_t>(visibleWidth)) visible[static_cast<size_t>(row) * visibleWidth + x] = value;
            };
            std::vector<uint16_t> prefix(4096), length(4096);
            std::vector<uint8_t> suffix(4096), first(4096);
            const int clear = 1 << minCodeSize, end = clear + 1;
            for (int c = 0; c < clear; c++) {
                prefix[static_cast<size_t>(c)] = 0;
                suffix[static_cast<size_t>(c)] = static_cast<uint8_t>(c);
                first[static_cast<size_t>(c)] = static_cast<uint8_t>(c);
                length[static_cast<size_t>(c)] = 1;
            }
            int codeSize = minCodeSize + 1, next = end + 1, previous = -1;
            size_t out = 0;
            // the bit reader over the sub-blocks
            size_t pos = start + 1, blockLeft = 0;
            uint32_t bits = 0;
            int bitCount = 0;
            bool dataEnded = false;
            auto readByte = [&](uint8_t& byte) {
                while (blockLeft == 0) {
                    if (pos >= n || b[pos] == 0) {
                        dataEnded = true;
                        return false;
                    }
                    blockLeft = b[pos++];
                }
                if (pos >= n) {
                    dataEnded = true;
                    return false;
                }
                byte = b[pos++];
                blockLeft--;
                return true;
            };
            while (out < count) {
                while (bitCount < codeSize) {
                    uint8_t byte;
                    if (!readByte(byte)) break;
                    bits |= uint32_t(byte) << bitCount;
                    bitCount += 8;
                }
                if (bitCount < codeSize) break;
                const int code = static_cast<int>(bits & ((1u << codeSize) - 1));
                bits >>= codeSize;
                bitCount -= codeSize;
                if (code == clear) {
                    codeSize = minCodeSize + 1;
                    next = end + 1;
                    previous = -1;
                    continue;
                }
                if (code == end) break;
                int emit;
                if (previous < 0) {
                    if (code >= clear) {
                        return e.invalid("frame " + std::to_string(frame) + " has a bad LZW code (offset " + std::to_string(pos) + ")");
                    }
                    emit = code;
                } else if (code < next) {
                    emit = code;
                    if (next < 4096) {
                        prefix[static_cast<size_t>(next)] = static_cast<uint16_t>(previous);
                        suffix[static_cast<size_t>(next)] = first[static_cast<size_t>(code)];
                        first[static_cast<size_t>(next)] = first[static_cast<size_t>(previous)];
                        length[static_cast<size_t>(next)] = static_cast<uint16_t>(length[static_cast<size_t>(previous)] + 1);
                        next++;
                    }
                } else if (code == next && next < 4096) {
                    prefix[static_cast<size_t>(next)] = static_cast<uint16_t>(previous);
                    suffix[static_cast<size_t>(next)] = first[static_cast<size_t>(previous)];
                    first[static_cast<size_t>(next)] = first[static_cast<size_t>(previous)];
                    length[static_cast<size_t>(next)] = static_cast<uint16_t>(length[static_cast<size_t>(previous)] + 1);
                    emit = next++;
                } else {
                    return e.invalid("frame " + std::to_string(frame) + " has a bad LZW code (offset " + std::to_string(pos) + ")");
                }
                // the string of `emit`, written back to front
                const size_t len = length[static_cast<size_t>(emit)];
                int c = emit;
                for (size_t k = len; k-- > 0;) {
                    if (out + k < count) store(out + k, suffix[static_cast<size_t>(c)]);
                    c = prefix[static_cast<size_t>(c)];
                }
                out += len;
                // the table grows to 12 bits; when full it stays full until a clear code (deferred clear)
                if (next == (1 << codeSize) && codeSize < 12) codeSize++;
                previous = code;
            }
            if (out < count) {
                (void)dataEnded;
                return e.invalid("LZW data ends early in frame " + std::to_string(frame) + " (offset " + std::to_string(pos) + ")");
            }
            return true;
        }

        // The LZW work of a frame is its descriptor's area, whatever the canvas clips: one frame
        // may not exceed kMaxPixels and all of them together kMaxGifDescriptorArea (frames inside
        // their canvas total at most kMaxPixels, the decoded-pixels limit), so a tiny canvas with
        // huge descriptors cannot make the decoder spin
        constexpr int64_t kMaxGifDescriptorArea = 2 * kMaxPixels;

        bool checkDescriptorArea(const Errors& e, const GifFrameInfo& f, int index, int64_t& total) {
            const int64_t area = static_cast<int64_t>(f.width) * f.height;
            if (area > kMaxPixels) {
                return e.fail("too_large",
                              "frame " + std::to_string(index + 1) + " is " + sizeText(f.width, f.height) + " (" + std::to_string(area) +
                                  " pixels); at most " + std::to_string(kMaxPixels) + " pixels are allowed",
                              "Re-export the GIF with frames no larger than its canvas.");
            }
            total += area;
            if (total > kMaxGifDescriptorArea) {
                return e.fail("too_large",
                              "the image descriptors of frames 1-" + std::to_string(index + 1) + " cover " + std::to_string(total) +
                                  " pixels; at most " + std::to_string(kMaxGifDescriptorArea) + " are decoded",
                              "Re-export the GIF with frames no larger than its canvas.");
            }
            return true;
        }

        bool loadGif(const uint8_t* b, size_t n, const Errors& e, Source& out) {
            GifWalker walker{b, n, e};
            if (!walker.header()) return false;
            const int w = walker.width, h = walker.height;
            const int64_t canvasPixels = static_cast<int64_t>(w) * h;
            std::vector<uint8_t> canvas(static_cast<size_t>(canvasPixels) * 4, 0), saved, indices;
            std::vector<Image> frames;
            std::vector<Delay> delays;
            int count = 0;
            int64_t descriptorArea = 0;
            const bool ok = walker.walk(
                [&](const GifFrameInfo& f, size_t lzw, int index) {
                    if (!checkFrameLimits(e, index, canvasPixels)) return false;
                    // the descriptor's own size bounds the decoding work: refused before anything is allocated
                    if (!checkDescriptorArea(e, f, index, descriptorArea)) return false;
                    // the visible part of the frame (clipped to the canvas) and where each row of the
                    // file goes: interlaced rows come in passes of 8/8/4/2 rows starting at 0/4/2/1
                    const int visibleWidth = std::max(0, std::min(f.width, w - f.left));
                    const int visibleHeight = std::max(0, std::min(f.height, h - f.top));
                    std::vector<int> destRow(static_cast<size_t>(f.height), -1);
                    {
                        int r = 0;
                        auto place = [&](int y) {
                            destRow[static_cast<size_t>(r++)] = y < visibleHeight ? y : -1;
                        };
                        if (f.interlaced) {
                            for (auto [startRow, step] : {std::pair{0, 8}, std::pair{4, 8}, std::pair{2, 4}, std::pair{1, 2}}) {
                                for (int y = startRow; y < f.height; y += step) place(y);
                            }
                        } else {
                            for (int y = 0; y < f.height; y++) place(y);
                        }
                    }
                    indices.assign(static_cast<size_t>(visibleWidth) * visibleHeight, 0);
                    if (!gifLzw(b, n, lzw, f.width, destRow, visibleWidth, indices, e, index + 1)) return false;
                    if (f.disposal == 3) saved = canvas;
                    for (int y = 0; y < visibleHeight; y++) {
                        for (int x = 0; x < visibleWidth; x++) {
                            const int index8 = indices[static_cast<size_t>(y) * visibleWidth + x];
                            if ((f.transparent && index8 == f.transparentIndex) || index8 >= f.paletteSize || !f.palette) continue;
                            uint8_t* p = canvas.data() + (static_cast<size_t>(f.top + y) * w + f.left + x) * 4;
                            p[0] = f.palette[3 * index8];
                            p[1] = f.palette[3 * index8 + 1];
                            p[2] = f.palette[3 * index8 + 2];
                            p[3] = 255;
                        }
                    }
                    Image frame = Image::make(w, h, 4, 8);
                    frame.data = canvas;
                    frames.push_back(std::move(frame));
                    delays.push_back(reducedDelay(static_cast<uint32_t>(f.delayCs) * 10u, 1));
                    // disposal before the next frame: 2 clears the frame's rectangle, 3 restores it
                    if (f.disposal == 2 || f.disposal == 3) {
                        for (int y = f.top; y < std::min(h, f.top + f.height); y++) {
                            for (int x = f.left; x < std::min(w, f.left + f.width); x++) {
                                const size_t i = (static_cast<size_t>(y) * w + x) * 4;
                                for (int k = 0; k < 4; k++) canvas[i + k] = f.disposal == 2 ? 0 : saved[i + k];
                            }
                        }
                    }
                    return true;
                },
                count);
            if (!ok) return false;
            out.fileBitDepth = 8;
            return finishAnimation(e, SourceKind::Gif, w, h, frames, delays, walker.loopFound ? walker.loopCount : 1, out);
        }

        // ---------------------------------------------------------------- APNG

        struct PngChunk {
            const uint8_t* type;
            const uint8_t* data;
            uint32_t length;
            size_t offset;
        };

        // More chunks than any real file has (a 64 MiB file could hold 5.6 million empty ones)
        constexpr size_t kMaxPngChunks = 1u << 20;

        // The chunks of a PNG up to IEND; checkCrc false for stills (stb ignores CRCs, so do we)
        bool pngChunks(const uint8_t* b, size_t n, const Errors& e, std::vector<PngChunk>& chunks, bool checkCrc = true) {
            size_t pos = 8;
            while (true) {
                if (chunks.size() >= kMaxPngChunks) {
                    return e.fail("too_large", "the PNG has more than " + std::to_string(kMaxPngChunks) + " chunks", "Re-export the image.");
                }
                if (pos + 12 > n) return e.invalid("the PNG is cut short (offset " + std::to_string(pos) + ")");
                const uint32_t length = be32(b + pos);
                if (length > n - pos - 12) return e.invalid("a PNG chunk runs past the end of the file (offset " + std::to_string(pos) + ")");
                PngChunk c{b + pos + 4, b + pos + 8, length, pos};
                if (checkCrc && crc32(b + pos + 4, static_cast<size_t>(length) + 4) != be32(b + pos + 8 + length)) {
                    return e.invalid("the CRC of the " + std::string(reinterpret_cast<const char*>(c.type), 4) + " chunk at offset " +
                                     std::to_string(pos) + " does not match");
                }
                chunks.push_back(c);
                pos += 12 + static_cast<size_t>(length);
                if (std::memcmp(c.type, "IEND", 4) == 0) return true;
            }
        }

        // The exact inflated size of PNG image data (filter bytes included) for its IHDR fields;
        // -1 for a colour type / bit depth PNG does not define
        int64_t pngRawSize(int64_t width, int64_t height, int bitDepth, int colorType, int interlace) {
            int channels;
            switch (colorType) {
                case 0: channels = 1; break;
                case 2: channels = 3; break;
                case 3: channels = 1; break;
                case 4: channels = 2; break;
                case 6: channels = 4; break;
                default: return -1;
            }
            if (bitDepth != 1 && bitDepth != 2 && bitDepth != 4 && bitDepth != 8 && bitDepth != 16) return -1;
            const int64_t bits = static_cast<int64_t>(channels) * bitDepth;
            auto pass = [&](int64_t w, int64_t h) { return w > 0 && h > 0 ? h * (1 + (w * bits + 7) / 8) : 0; };
            if (interlace != 1) return pass(width, height);
            int64_t total = 0;
            const int adam7[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
            for (const auto& a : adam7) {
                total += pass(width > a[0] ? (width - a[0] + a[2] - 1) / a[2] : 0, height > a[1] ? (height - a[1] + a[3] - 1) / a[3] : 0);
            }
            return total;
        }

        // A deflate bomb guard: the zlib data must inflate to at least the `expected` bytes its size
        // needs (extra bytes are ignored, as stb does) and at most expected + 1 MiB + expected / 8,
        // decoded into a buffer of that cap (stb's PNG loader would grow its buffer up to 4 GiB)
        bool checkInflatedSize(const uint8_t* data, size_t size, int64_t expected, const Errors& e, const std::string& what) {
            if (expected < 0) return e.invalid(what + " has an invalid colour type or bit depth");
            const int64_t cap = expected + (1 << 20) + expected / 8;
            if (cap > 0x7FFFFFFF || size > 0x7FFFFFFF) return e.fail("too_large", what + " is too large to decode", "Scale it down.");
            std::vector<char> buffer(static_cast<size_t>(cap));
            const int got = stbi_zlib_decode_buffer(buffer.data(), static_cast<int>(cap), reinterpret_cast<const char*>(data), static_cast<int>(size));
            if (got < expected) {
                return e.invalid(what + "'s image data is corrupt, ends before the " + std::to_string(expected) +
                                 " bytes its size needs or inflates past " + std::to_string(cap) + " bytes");
            }
            return true;
        }

        struct ApngFrame {
            int width = 0, height = 0, x = 0, y = 0;
            uint16_t delayNum = 0, delayDen = 0;
            int dispose = 0, blend = 0;
            std::vector<uint8_t> data;
        };

        struct ApngHeader {
            int width = 0, height = 0, bitDepth = 8, colorType = 6, interlace = 0;
            const PngChunk* ihdr = nullptr;
            const PngChunk* plte = nullptr;
            const PngChunk* trns = nullptr;
            uint32_t numFrames = 0, numPlays = 0;
            bool actl = false;
        };

        bool apngHeader(const std::vector<PngChunk>& chunks, const Errors& e, ApngHeader& h) {
            if (chunks.empty() || std::memcmp(chunks[0].type, "IHDR", 4) != 0 || chunks[0].length != 13) return e.invalid("the PNG does not start with IHDR");
            h.ihdr = &chunks[0];
            const uint8_t* d = chunks[0].data;
            h.width = static_cast<int>(std::min<uint32_t>(be32(d), 0x7FFFFFFF));
            h.height = static_cast<int>(std::min<uint32_t>(be32(d + 4), 0x7FFFFFFF));
            h.bitDepth = d[8];
            h.colorType = d[9];
            h.interlace = d[12];
            for (const PngChunk& c : chunks) {
                if (std::memcmp(c.type, "IDAT", 4) == 0) break;
                if (std::memcmp(c.type, "acTL", 4) == 0 && c.length == 8) {
                    h.actl = true;
                    h.numFrames = be32(c.data);
                    h.numPlays = be32(c.data + 4);
                }
                if (std::memcmp(c.type, "PLTE", 4) == 0) h.plte = &c;
                if (std::memcmp(c.type, "tRNS", 4) == 0) h.trns = &c;
            }
            if (!h.actl) return e.invalid("the APNG has no acTL chunk before its image data");
            if (h.numFrames == 0) return e.invalid("the APNG's acTL says 0 frames");
            if (h.numFrames > static_cast<uint32_t>(kMaxFrames)) {
                return e.fail("too_large",
                              "the APNG has " + std::to_string(h.numFrames) + " frames; at most " + std::to_string(kMaxFrames) + " are allowed",
                              "Trim the animation.");
            }
            if (!checkCanvas(e, "APNG canvas", h.width, h.height)) return false;
            if (static_cast<int64_t>(h.numFrames) * h.width * h.height > kMaxPixels) {
                return e.fail("too_large",
                              "the APNG's " + std::to_string(h.numFrames) + " frames of " + sizeText(h.width, h.height) + " hold " +
                                  std::to_string(static_cast<int64_t>(h.numFrames) * h.width * h.height) + " pixels; at most " +
                                  std::to_string(kMaxPixels) + " are allowed",
                              "Trim the animation or scale it down.");
            }
            const int64_t atlas = atlasPixels(h.numFrames, static_cast<int64_t>(h.width) * h.height);
            if (atlas > kMaxPixels) {
                return e.fail("too_large",
                              "the atlas of the APNG's " + std::to_string(h.numFrames) + " frames of " + sizeText(h.width, h.height) + " would hold " +
                                  std::to_string(atlas) + " pixels; at most " + std::to_string(kMaxPixels) + " are allowed",
                              "Trim the animation or scale it down.");
            }
            return true;
        }

        bool decodeApngFrame(const ApngHeader& h, const ApngFrame& f, const Errors& e, int index, Image& out) {
            if (!checkInflatedSize(f.data.data(), f.data.size(), pngRawSize(f.width, f.height, h.bitDepth, h.colorType, h.interlace), e,
                                   "frame " + std::to_string(index + 1) + " of the APNG")) {
                return false;
            }
            std::vector<uint8_t> png(kPngSignature, kPngSignature + 8);
            uint8_t ihdr[13];
            std::memcpy(ihdr, h.ihdr->data, 13);
            ihdr[0] = static_cast<uint8_t>(f.width >> 24);
            ihdr[1] = static_cast<uint8_t>(f.width >> 16);
            ihdr[2] = static_cast<uint8_t>(f.width >> 8);
            ihdr[3] = static_cast<uint8_t>(f.width);
            ihdr[4] = static_cast<uint8_t>(f.height >> 24);
            ihdr[5] = static_cast<uint8_t>(f.height >> 16);
            ihdr[6] = static_cast<uint8_t>(f.height >> 8);
            ihdr[7] = static_cast<uint8_t>(f.height);
            putChunk(png, "IHDR", ihdr, 13);
            if (h.plte) putChunk(png, "PLTE", h.plte->data, h.plte->length);
            if (h.trns) putChunk(png, "tRNS", h.trns->data, h.trns->length);
            putChunk(png, "IDAT", f.data.data(), f.data.size());
            putChunk(png, "IEND", nullptr, 0);
            texsynth::Image decoded;
            std::string error;
            if (!texsynth::loadImageFromMemory(png.data(), png.size(), decoded, error)) {
                return e.invalid("frame " + std::to_string(index + 1) + " of the APNG cannot be decoded (" + error + ")");
            }
            if (decoded.width != f.width || decoded.height != f.height) return e.invalid("frame " + std::to_string(index + 1) + " has the wrong size");
            out = texsynth::toRgba8(decoded);
            return true;
        }

        bool loadApng(const uint8_t* b, size_t n, const Errors& e, Source& out) {
            std::vector<PngChunk> chunks;
            if (!pngChunks(b, n, e, chunks)) return false;
            ApngHeader h;
            if (!apngHeader(chunks, e, h)) return false;
            const int64_t canvasPixels = static_cast<int64_t>(h.width) * h.height;

            // the frames: fcTL starts one; its data is the IDATs (frame 0 when the fcTL comes
            // before them) or the following fdATs
            std::vector<ApngFrame> list;
            bool sawIdat = false, idatIsFrame = false;
            int64_t lastSequence = -1;
            for (const PngChunk& c : chunks) {
                if (std::memcmp(c.type, "fcTL", 4) == 0) {
                    if (c.length != 26) return e.invalid("an fcTL chunk has length " + std::to_string(c.length) + " (offset " + std::to_string(c.offset) + ")");
                    const uint32_t sequence = be32(c.data);
                    if (static_cast<int64_t>(sequence) <= lastSequence) {
                        return e.invalid("APNG sequence numbers do not increase (" + std::to_string(sequence) + " after " + std::to_string(lastSequence) +
                                         ", offset " + std::to_string(c.offset) + ")");
                    }
                    lastSequence = sequence;
                    ApngFrame f;
                    const uint32_t fw = be32(c.data + 4), fh = be32(c.data + 8), fx = be32(c.data + 12), fy = be32(c.data + 16);
                    f.delayNum = be16(c.data + 20);
                    f.delayDen = be16(c.data + 22);
                    f.dispose = c.data[24];
                    f.blend = c.data[25];
                    const std::string where = "frame " + std::to_string(list.size() + 1) + " of the APNG";
                    if (fw == 0 || fh == 0 || fx > static_cast<uint32_t>(h.width) || fy > static_cast<uint32_t>(h.height) ||
                        fw > static_cast<uint32_t>(h.width) - fx || fh > static_cast<uint32_t>(h.height) - fy) {
                        return e.invalid(where + " (" + std::to_string(fx) + ", " + std::to_string(fy) + ", " + sizeText(fw, fh) +
                                         ") is not inside the " + sizeText(h.width, h.height) + " canvas");
                    }
                    if (list.empty() && (fx != 0 || fy != 0 || fw != static_cast<uint32_t>(h.width) || fh != static_cast<uint32_t>(h.height))) {
                        return e.invalid("the first frame of the APNG must cover the whole canvas");
                    }
                    if (f.dispose > 2 || f.blend > 1) return e.invalid(where + " has an unknown dispose or blend op");
                    f.width = static_cast<int>(fw);
                    f.height = static_cast<int>(fh);
                    f.x = static_cast<int>(fx);
                    f.y = static_cast<int>(fy);
                    if (!sawIdat && list.empty()) idatIsFrame = true;
                    if (list.size() >= h.numFrames) {
                        return e.invalid("the APNG has more fcTL chunks than the " + std::to_string(h.numFrames) + " frames its acTL declares");
                    }
                    list.push_back(std::move(f));
                } else if (std::memcmp(c.type, "IDAT", 4) == 0) {
                    sawIdat = true;
                    if (idatIsFrame && list.size() == 1) list[0].data.insert(list[0].data.end(), c.data, c.data + c.length);
                } else if (std::memcmp(c.type, "fdAT", 4) == 0) {
                    if (c.length < 4) return e.invalid("an fdAT chunk is too short (offset " + std::to_string(c.offset) + ")");
                    const uint32_t sequence = be32(c.data);
                    if (static_cast<int64_t>(sequence) <= lastSequence) {
                        return e.invalid("APNG sequence numbers do not increase (" + std::to_string(sequence) + " after " + std::to_string(lastSequence) +
                                         ", offset " + std::to_string(c.offset) + ")");
                    }
                    lastSequence = sequence;
                    if (list.empty() || (idatIsFrame && list.size() == 1)) {
                        return e.invalid("an fdAT chunk has no frame (offset " + std::to_string(c.offset) + ")");
                    }
                    list.back().data.insert(list.back().data.end(), c.data + 4, c.data + c.length);
                }
            }
            if (list.empty()) return e.invalid("the APNG has no frame");

            std::vector<uint8_t> canvas(static_cast<size_t>(canvasPixels) * 4, 0), saved;
            std::vector<Image> frames;
            std::vector<Delay> delays;
            for (size_t i = 0; i < list.size(); i++) {
                const ApngFrame& f = list[i];
                if (!checkFrameLimits(e, static_cast<int>(i), canvasPixels)) return false;
                if (f.data.empty()) return e.invalid("frame " + std::to_string(i + 1) + " of the APNG has no image data");
                // the previous frame's dispose op (2 on the first frame counts as 1)
                if (i > 0) {
                    const ApngFrame& p = list[i - 1];
                    const int dispose = (p.dispose == 2 && i == 1) ? 1 : p.dispose;
                    if (dispose != 0) {
                        for (int y = p.y; y < p.y + p.height; y++) {
                            uint8_t* row = canvas.data() + (static_cast<size_t>(y) * h.width + p.x) * 4;
                            if (dispose == 1) std::memset(row, 0, static_cast<size_t>(p.width) * 4);
                            else std::memcpy(row, saved.data() + (static_cast<size_t>(y) * h.width + p.x) * 4, static_cast<size_t>(p.width) * 4);
                        }
                    }
                }
                Image pixels;
                if (!decodeApngFrame(h, f, e, static_cast<int>(i), pixels)) return false;
                if (f.dispose == 2) saved = canvas;
                for (int y = 0; y < f.height; y++) {
                    for (int x = 0; x < f.width; x++) {
                        const uint8_t* s = pixels.pixel(x, y);
                        uint8_t* d = canvas.data() + (static_cast<size_t>(f.y + y) * h.width + f.x + x) * 4;
                        if (f.blend == 0) {
                            std::memcpy(d, s, 4);
                            continue;
                        }
                        const float sa = s[3], da = d[3];
                        const float outA = sa + da * (255.0f - sa) / 255.0f;
                        if (outA <= 0.0f) {
                            std::memset(d, 0, 4);
                            continue;
                        }
                        for (int k = 0; k < 3; k++) {
                            const float c = (s[k] * sa + d[k] * da * (255.0f - sa) / 255.0f) / outA;
                            d[k] = static_cast<uint8_t>(std::min(255.0f, std::round(c)));
                        }
                        d[3] = static_cast<uint8_t>(std::min(255.0f, std::round(outA)));
                    }
                }
                Image frame = Image::make(h.width, h.height, 4, 8);
                frame.data = canvas;
                frames.push_back(std::move(frame));
                delays.push_back(reducedDelay(static_cast<uint32_t>(f.delayNum) * 1000u, f.delayDen == 0 ? 100u : f.delayDen));
            }
            out.fileBitDepth = h.bitDepth == 16 ? 16 : 8;
            return finishAnimation(e, SourceKind::Apng, h.width, h.height, frames, delays, static_cast<int>(std::min<uint32_t>(h.numPlays, 0x7FFFFFFF)),
                                   out);
        }

        // ---------------------------------------------------------------- stills

        bool loadStill(const uint8_t* b, size_t n, const Errors& e, Source& out) {
            const int length = static_cast<int>(n);
            if (stbi_is_hdr_from_memory(b, length)) return e.invalid("HDR images are not supported");
            int w = 0, h = 0, comp = 0;
            if (!stbi_info_from_memory(b, length, &w, &h, &comp)) return e.invalid("not a readable image");
            if (!checkCanvas(e, "image", w, h)) return false;
            // PNG: the image data must inflate to its IHDR size (deflate bombs). The other formats
            // stb reads (JPEG, TGA, BMP, PSD, ...) decode into a buffer sized from their header,
            // which checkCanvas bounded above.
            if (isPng(b, n)) {
                std::vector<PngChunk> chunks;
                if (!pngChunks(b, n, e, chunks, false)) return false;
                if (chunks.empty() || std::memcmp(chunks[0].type, "IHDR", 4) != 0 || chunks[0].length != 13) return e.invalid("the PNG does not start with IHDR");
                const uint8_t* ihdr = chunks[0].data;
                std::vector<uint8_t> idat;
                for (const PngChunk& c : chunks) {
                    if (std::memcmp(c.type, "IDAT", 4) == 0) idat.insert(idat.end(), c.data, c.data + c.length);
                }
                if (!checkInflatedSize(idat.data(), idat.size(), pngRawSize(be32(ihdr), be32(ihdr + 4), ihdr[8], ihdr[9], ihdr[12]), e, "the PNG")) {
                    return false;
                }
            }
            texsynth::Image decoded;
            std::string error;
            if (!texsynth::loadImageFromMemory(b, n, decoded, error)) return e.invalid(error);
            Source s;
            s.kind = SourceKind::Image;
            s.color = texsynth::toRgba8(decoded);
            s.fileWidth = decoded.width;
            s.fileHeight = decoded.height;
            s.fileChannels = decoded.channels;
            s.fileBitDepth = decoded.bitDepth;
            s.sha1 = out.sha1;
            out = std::move(s);
            return true;
        }

        bool readFile(const fs::path& path, std::vector<uint8_t>& bytes, const Errors& e) {
            std::error_code ec;
            if (!fs::is_regular_file(path, ec)) return e.fail("invalid_source", "the file does not exist or is not a file", "Check the path.");
            const uintmax_t size = fs::file_size(path, ec);
            if (ec) return e.fail("invalid_source", "the file cannot be read (" + ec.message() + ")", "Check the path and its permissions.");
            if (size > static_cast<uintmax_t>(kMaxInputBytes)) {
                return e.fail("too_large",
                              "the file is " + std::to_string(size) + " bytes; at most " + std::to_string(kMaxInputBytes) + " (64 MiB) are read",
                              "Use a smaller file.");
            }
            std::ifstream in(path, std::ios::binary);
            if (!in) return e.fail("invalid_source", "the file cannot be opened", "Check the path and its permissions.");
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (bytes.size() != size) return e.fail("invalid_source", "the file could not be read whole", "Try again.");
            return true;
        }

        bool loadBytes(const uint8_t* bytes, size_t size, Source& out, const Errors& e, bool hash) {
            out = Source();
            if (!bytes || size == 0) return e.invalid("the file is empty");
            if (size > static_cast<size_t>(kMaxInputBytes)) {
                return e.fail("too_large",
                              "the file is " + std::to_string(size) + " bytes; at most " + std::to_string(kMaxInputBytes) + " (64 MiB) are read",
                              "Use a smaller file.");
            }
            if (hash) out.sha1 = texsynth::sha1Hex(bytes, size);
            if (isGif(bytes, size)) return loadGif(bytes, size, e, out);
            if (isApng(bytes, size)) return loadApng(bytes, size, e, out);
            return loadStill(bytes, size, e, out);
        }

        bool probeBytes(const uint8_t* b, size_t n, SourceInfo& info, const Errors& e) {
            info = SourceInfo();
            if (isGif(b, n)) {
                info.kind = SourceKind::Gif;
                GifWalker walker{b, n, e};
                if (!walker.header()) return false;
                info.width = walker.width;
                info.height = walker.height;
                const int64_t canvasPixels = static_cast<int64_t>(walker.width) * walker.height;
                int frames = 0;
                int64_t descriptorArea = 0;
                if (!walker.walk(
                        [&](const GifFrameInfo& f, size_t, int index) {
                            return checkFrameLimits(e, index, canvasPixels) && checkDescriptorArea(e, f, index, descriptorArea);
                        },
                        frames)) {
                    return false;
                }
                info.frameCount = frames;
                return true;
            }
            if (isApng(b, n)) {
                info.kind = SourceKind::Apng;
                std::vector<PngChunk> chunks;
                if (!pngChunks(b, n, e, chunks)) return false;
                ApngHeader h;
                if (!apngHeader(chunks, e, h)) return false;
                info.width = h.width;
                info.height = h.height;
                info.frameCount = static_cast<int>(h.numFrames);
                return true;
            }
            info.kind = SourceKind::Image;
            if (stbi_is_hdr_from_memory(b, static_cast<int>(n))) return e.invalid("HDR images are not supported");
            int comp = 0;
            if (!stbi_info_from_memory(b, static_cast<int>(n), &info.width, &info.height, &comp)) return e.invalid("not a readable image");
            return checkCanvas(e, "image", info.width, info.height);
        }
    }

    const char* toString(SourceKind kind) {
        switch (kind) {
            case SourceKind::Image: return "image";
            case SourceKind::Gif: return "gif";
            case SourceKind::Apng: return "apng";
        }
        return "image";
    }

    double Delay::ms() const { return denominator ? static_cast<double>(numerator) / denominator : 0.0; }

    int Delay::playbackMs() const {
        const double v = std::round(ms());
        return v < 10.0 ? 100 : static_cast<int>(std::min(v, 2147483647.0));
    }

    namespace {
        // The exception barrier of the public functions: they return false with a problem, never throw
        template <typename Work>
        bool guarded(const Errors& e, Work work) {
            try {
                return work();
            } catch (const std::bad_alloc&) {
                return e.fail("failed", "ran out of memory while reading the file", "Use a smaller file.");
            } catch (const std::exception& ex) {
                return e.fail("failed", std::string("reading the file failed (") + ex.what() + ")", "");
            } catch (...) {
                return e.fail("failed", "reading the file failed", "");
            }
        }

        bool packAtlasUnguarded(const std::vector<Image>& frames, Image& atlas, int& columns, int& rows, std::vector<Cell>& cells, std::string& error) {
            atlas = Image();
            cells.clear();
            columns = rows = 0;
            if (frames.empty()) {
                error = "There are no frames to pack.";
                return false;
            }
            const int w = frames[0].width, h = frames[0].height;
            for (const Image& f : frames) {
                if (f.width != w || f.height != h || f.channels != 4 || f.bitDepth != 8 || f.empty()) {
                    error = "The frames must all be RGBA8 images of the same size.";
                    return false;
                }
            }
            const int n = static_cast<int>(frames.size());
            int c = 1;
            while (c * c < n) c++;
            const int r = (n + c - 1) / c;
            const int64_t pixels = static_cast<int64_t>(w) * c * static_cast<int64_t>(h) * r;
            if (pixels > kMaxPixels) {
                error = "The atlas of " + std::to_string(n) + " frames of " + sizeText(w, h) + " would be " + sizeText(static_cast<int64_t>(w) * c, static_cast<int64_t>(h) * r) +
                        " (" + std::to_string(pixels) + " pixels); at most " + std::to_string(kMaxPixels) + " are allowed.";
                return false;
            }
            atlas = Image::make(w * c, h * r, 4, 8);
            for (int i = 0; i < n; i++) {
                const Cell cell{(i % c) * w, (i / c) * h, w, h};
                for (int y = 0; y < h; y++) std::memcpy(atlas.pixel(cell.x, cell.y + y), frames[static_cast<size_t>(i)].pixel(0, y), static_cast<size_t>(w) * 4);
                cells.push_back(cell);
            }
            columns = c;
            rows = r;
            return true;
        }
    }

    bool probeSource(const fs::path& path, SourceInfo& info, std::vector<Problem>& problems) {
        const Errors e{path.filename().u8string(), &problems};
        return guarded(e, [&] {
            std::vector<uint8_t> bytes;
            if (!readFile(path, bytes, e)) return false;
            return probeBytes(bytes.data(), bytes.size(), info, e);
        });
    }

    bool loadSource(const fs::path& path, Source& out, std::vector<Problem>& problems) {
        const Errors e{path.filename().u8string(), &problems};
        return guarded(e, [&] {
            std::vector<uint8_t> bytes;
            if (!readFile(path, bytes, e)) return false;
            return loadBytes(bytes.data(), bytes.size(), out, e, true);
        });
    }

    bool loadSourceFromMemory(const uint8_t* bytes, size_t size, Source& out, std::vector<Problem>& problems, bool hash) {
        const Errors e{"", &problems};
        return guarded(e, [&] { return loadBytes(bytes, size, out, e, hash); });
    }

    bool packAtlas(const std::vector<Image>& frames, Image& atlas, int& columns, int& rows, std::vector<Cell>& cells, std::string& error) {
        try {
            return packAtlasUnguarded(frames, atlas, columns, rows, cells, error);
        } catch (const std::bad_alloc&) {
            atlas = Image();
            error = "Out of memory while packing the atlas.";
            return false;
        }
    }

    bool encodePng(const Image& image, std::vector<uint8_t>& png, std::string& error) {
        png.clear();
        const bool rgba8 = image.channels == 4 && image.bitDepth == 8;
        const bool grey16 = image.channels == 1 && image.bitDepth == 16;
        if (image.empty() || (!rgba8 && !grey16) || image.data.size() != image.pixelCount() * image.bytesPerPixel()) {
            error = "Only 8-bit RGBA and 16-bit grey images are written (got " + texsynth::describeFormat(image) + ").";
            return false;
        }
        try {
            if (!texsynth::encodePng(image, png)) {
                error = "The PNG could not be encoded.";
                return false;
            }
        } catch (const std::bad_alloc&) {
            png.clear();
            error = "Out of memory while encoding the PNG.";
            return false;
        }
        return true;
    }
}
