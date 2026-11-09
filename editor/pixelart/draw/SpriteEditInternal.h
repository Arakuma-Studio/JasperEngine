// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Shared by SpriteEdit*.cpp only: the batch's working state. Pixel ops draw on canvas-sized
// copies of the cels they touch (one per layer and image-holding frame, so a linked cel is edited
// once for every frame showing it); the copies go back into the document's cels when the batch
// ends, before a structure op and before anything reads the composite. A failing op throws
// OpError, which applyOps turns into the batch's problem (the working document is then dropped).

#pragma once

#include "SpriteEdit.h"

#include "json.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor::pixelart::draw::detail {

    namespace ase = aseprite;
    using Json = nlohmann::json;

    struct OpError {
        PixelProblem problem;
    };

    [[noreturn]] void fail(std::string kind, std::string field, std::string message, std::string fix);

    // A colour in the sprite's model: the index (indexed sprites) and its RGBA
    struct Ink {
        uint8_t index = 0;
        PixelRgba rgba{0, 0, 0, 0};
    };

    struct Canvas {
        int layer = 0;
        int srcFrame = 0;          // the frame whose cel holds the image (links followed), from 0
        PixelImage original;       // as it was when the batch (or the last structure op) first touched it
        PixelImage current;
        std::set<int> frames;      // the frames the ops addressed through it, from 0
    };

    class EditState {
    public:
        EditState(SpriteDoc& sprite, const EditRequest& request, const EditEnv& env, EditResult& result);

        SpriteDoc& sprite;
        ase::Document& doc;
        const EditEnv& env;
        EditResult& result;
        const EditRequest& request;

        int opIndex = -1;
        uint64_t opChanges = 0;    // pixels the running op changed

        // ---- addressing (R3)
        bool indexed() const { return doc.mode == ase::ColorMode::Indexed; }
        bool grayscale() const { return doc.mode == ase::ColorMode::Grayscale; }
        // the op's frames (its own `frame`, else the request's, else 1), from 0
        std::vector<int> frames(const Json& op, const char* field = "frame");
        std::vector<int> parseFrames(const Json& value, const std::string& field);
        int parseFrame(const Json& value, const std::string& field);   // one frame, from 0
        // the op's pixel layer (its own `layer`, else the request's, else the topmost visible pixel layer)
        int pixelLayer(const Json& op, const char* field = "layer");
        int parseLayer(const Json& value, const std::string& field, bool pixels);
        std::string frameRange() const;    // "frame is 1-8 or a tag (walk, idle) or \"all\""
        std::string layerList() const;     // "layers: Body (0), Arm (1)"

        Ink color(const Json& value, const std::string& field);
        Ink clearInk(int layer) const;
        Ink inkAt(const PixelImage& image, int x, int y) const;
        bool clearAt(const PixelImage& image, int x, int y) const;
        bool sameInk(const Ink& a, const Ink& b) const;
        // R18's similarity: indices with tolerance 0 on indexed sprites, else max |delta| over RGBA
        bool similar(const Ink& a, const Ink& b, int tolerance) const;
        Ink inkOfRgba(const PixelRgba& rgba, const std::string& field);   // exact (indexed: in the palette)
        Ink inkOfIndex(int index) const;
        std::vector<uint8_t> paletteRgba() const;

        // ---- the canvases
        Canvas& canvas(int layer, int frame);
        // a pixel write: inside the canvas and (unless the op ignores it) the selection
        bool writable(int x, int y) const;
        void put(Canvas& canvas, int x, int y, const Ink& ink);
        // writes without the selection test (transforms place what they took)
        void putRaw(Canvas& canvas, int x, int y, const Ink& ink);
        bool ignoreSelection = false;   // the running op's ignore_selection

        // ---- the selection (R14): a canvas mask for the rest of the batch
        std::vector<uint8_t> selection;
        bool hasSelection = false;
        bool selected(int x, int y) const;
        IntRect selectionBounds() const;

        // ---- symmetry (R15)
        bool mirrorX = false, mirrorY = false, diagXY = false, diagXMinusY = false;
        int axisX2 = 0, axisY2 = 0;     // twice the axes, in pixel-index coordinates
        bool symmetric() const { return mirrorX || mirrorY || diagXY || diagXMinusY; }
        // the copies of a position (Tools.gd get_mirrored_positions), without the original
        std::vector<IntPoint> mirrorsOf(IntPoint p) const;
        // the pixels plus every mirrored copy, each once
        std::vector<IntPoint> withMirrors(const std::vector<IntPoint>& pixels) const;

        // ---- composition and bookkeeping
        // the canvases back into the document's cels (they stay, unless `drop`, which records
        // their changes so far and forgets them: a structure op follows)
        void flush(bool drop);
        PixelImage composite(int frame, bool keepIndices);
        // after a structure op: re-addresses the changes recorded so far (false drops one)
        void remapRecorded(const std::function<bool(DirtyRect&)>& remap);
        void markStructure() { result.structureChanged = true; structureOps++; }
        void warn(std::string kind, std::string message);
        void finish();
        int structureOps = 0;

    private:
        std::map<std::pair<int, int>, Canvas> canvases;
        std::vector<DirtyRect> recorded;
        void writeBack(Canvas& canvas);
        void record(Canvas& canvas);
    };

    // field readers: a missing optional field gives the default; a wrong type or range fails
    // with bad_op_field naming the field and its range
    int intField(const Json& op, const char* name, int def, int lo, int hi);
    double numberField(const Json& op, const char* name, double def, double lo, double hi);
    bool boolField(const Json& op, const char* name, bool def);
    std::string enumField(const Json& op, const char* name, const std::string& def, const std::vector<std::string>& values);
    IntPoint pointValue(const Json& value, const std::string& field);
    std::vector<IntPoint> pointsField(const Json& op, const char* name, size_t minimum);
    IntRect rectValue(const Json& value, const std::string& field);
    std::optional<IntRect> rectField(const Json& op, const char* name);

    // the families
    void runPaintOp(EditState& state, const Json& op, const std::string& name);
    void runTransformOp(EditState& state, const Json& op, const std::string& name);
    void runStructureOp(EditState& state, const Json& op, const std::string& name);

}
