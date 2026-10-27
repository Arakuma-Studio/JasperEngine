// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 4.6, 4.6.1: the importers' texture reader)
// - Purpose: read the data textures of Houdini VAT 3 and OpenVAT exports (positions, rotations,
//   colours, lookup tables) as linear floats, exactly as stored: no colour management, no gamma
//   (PNG gAMA/sRGB/iCCP chunks are ignored), no premultiplication, no resizing.
// - Formats, recognised by content (EXR, PNG magic) or by the .tga extension:
//   EXR (tinyexr): single-part scanline or tiled images (level 0 of mipmapped/ripmapped ones),
//     HALF, FLOAT and UINT channels, every compression tinyexr reads (NONE, RLE, ZIPS, ZIP, PIZ,
//     PXR24, B44, B44A; not DWAA/DWAB, and not tinyexr's own ZFP extension, which is not built
//     in). Channels by name, matched on the part after the last '.' (so "Layer.R" is R),
//     case-insensitively: R, G, B, A, else Y (luminance, read as grey); a single channel with any
//     other name is read as grey. The data window is the image. HALF and FLOAT values are kept
//     bit-exact (half to float is exact), UINT values convert to float (exact up to 2^24).
//     Multi-part and deep images are refused.
//   PNG (stb_image): 8-bit (palette, 1/2/4-bit grey expanded to 8 bits as stb does) and 16-bit,
//     1-4 channels; 16-bit images keep their 16 bits (stb's 16-bit loader). Values normalise
//     exactly as float(v) / 255.0f or float(v) / 65535.0f.
//   TGA (stb_image): 8 bits per channel as stb decodes it (15/16-bit TGAs expand to 8 bits),
//     normalised as PNG 8-bit.
// - Layout of FloatImage::rgba: width * height texels of 4 floats, rows top to bottom as the image
//   is stored (EXR scanline 0 / the data window's first line first, whatever its line order; the
//   PNG's first row; a TGA's top row whatever its origin bit), no padding. RGB sources get alpha 1;
//   grey (g) gives (g, g, g, 1) and grey+alpha (g, a) gives (g, g, g, a); an EXR with only some of
//   R, G, B gives 0 for the missing ones (sourceChannels then counts the channels present).
// - sourceChannels: 1 grey, 2 grey+alpha, 3 RGB, 4 RGBA (for EXR: the channels used, 1..4).
//   sourceBits: 8 or 16 (PNG/TGA), 16 (EXR HALF) or 32 (EXR FLOAT or UINT; the widest channel).
//   sourceFloat: true for EXR HALF/FLOAT data, false for UINT, PNG and TGA.
// - raw8: for 8-bit sources (PNG 8-bit, TGA) the integer values 0..255 as RGBA8 (same expansion:
//   alpha 255 for RGB, (g, g, g, 255)/(g, g, g, a) for grey), width * height * 4 bytes, the same
//   row order; empty for 16-bit PNGs and EXRs. It is what lookup tables are read from (Houdini's
//   fluid lookup: raw bytes, never through the float normalisation).
// - writeExr: an EXR of the image (the inverse of readFloatImage for EXR): channels by
//   sourceChannels (1 Y, 2 Y+A, 3 R G B, 4 or 0 R G B A, stored alphabetically as EXR readers
//   expect), HALF (IEEE binary16 rounded to nearest, ties to even; past 65504 infinity) or
//   FLOAT, ZIP compression, scanline, increasing Y, data window = the image. The overload with
//   ExrWriteOptions picks the pixel type (also UINT: values rounded and clamped to 0..2^32-1),
//   the compression and tiles (tests and later tools).
// - Errors: false with a sentence in *error naming the file (missing file, unknown format,
//   truncated/corrupt data with tinyexr's or stb's reason, unsupported EXR feature); `out` is then
//   empty. Paths are UTF-8.

#ifndef FLOAT_IMAGE_H
#define FLOAT_IMAGE_H

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct FloatImage {
        int width = 0, height = 0;
        std::vector<float> rgba;
        int sourceChannels = 0;
        int sourceBits = 0;
        bool sourceFloat = false;
        // 8-bit sources only (see the contract): the stored integer values as RGBA8
        std::vector<uint8_t> raw8;
    };

    bool readFloatImage(const std::string& path, FloatImage& out, std::string* error);
    // the same from bytes in memory; `name` is the file name used for the format (the .tga
    // extension) and in error messages
    bool readFloatImageFromMemory(const uint8_t* bytes, size_t size, const std::string& name, FloatImage& out, std::string* error);

    struct ExrWriteOptions {
        enum class PixelType { Half, Float, Uint };
        enum class Compression { None, Rle, Zips, Zip, Piz, Pxr24, B44, B44a };
        PixelType pixelType = PixelType::Float;
        Compression compression = Compression::Zip;
        // tiled (one level) instead of scanlines, tileSize x tileSize tiles (at most the image size)
        bool tiled = false;
        int tileSize = 32;
    };

    bool writeExr(const std::string& path, const FloatImage& image, bool half, std::string* error);
    bool writeExr(const std::string& path, const FloatImage& image, const ExrWriteOptions& options, std::string* error);

} // namespace jasperengine::editor

#endif // FLOAT_IMAGE_H
