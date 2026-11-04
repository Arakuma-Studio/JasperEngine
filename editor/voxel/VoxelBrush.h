// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The grammar of a Voxel Volume's brush (docs/specs/T6-voxel-volumes.md R60, after T1 R55-R57): which
// operation a tool and its modifiers mean, the colour that says so, where a stroke's stamps fall, and
// an Alt press telling a pick from an orbit. No ImGui and no scene here, so the tests drive what the
// panel and the Scene view do.

#include "voxel/VoxelEdit.h"
#include "voxel/VoxelSample.h"
#include "voxel/VoxelStore.h"
#include "voxel/VoxelTypes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class CommandCatalog;

    enum class VoxelTool : uint8_t { Dig, Add, Smooth, Paint, Pick };
    enum class VoxelBrushShape : uint8_t { Sphere, Box, Capsule, Mesh, Stamp };

    struct VoxelBrushSettings {
        VoxelTool tool = VoxelTool::Dig;
        VoxelBrushShape shape = VoxelBrushShape::Sphere;
        float radius = 3.0f;      // metres; Box: half its side
        float blend = 0.5f;       // metres, smooth-CSG
        int material = 0;         // the slot Add and Paint write
        float weight = 1.0f;      // Paint's, absolute
        float falloff = 0.5f;     // the shape's edge softness for Smooth and Paint
        float amount = 0.5f;      // Smooth
        int iterations = 2;       // Smooth
        float stampScale = 1.0f;  // Mesh and Stamp, uniform
        // Mesh and Stamp: what the shape is, built once for each source (R36)
        std::shared_ptr<const voxel::StampField> field;
    };

    // R60's presets. They set the tool and the shape; the slot and any field stay as they are.
    struct VoxelBrushPreset {
        const char* name;
        const char* description;
        VoxelTool tool;
        VoxelBrushShape shape;
        float radius;
        float blend;
        float falloff;
        float amount;
        int iterations;
    };
    const std::vector<VoxelBrushPreset>& voxelBrushPresets();
    void applyVoxelBrushPreset(VoxelBrushSettings& settings, const VoxelBrushPreset& preset);

    // T1 R56 for these tools: Ctrl inverts Dig and Add, Shift smooths, and Shift wins over Ctrl, as on
    // a Terrain. Pick stays Pick, it writes nothing.
    VoxelTool voxelEffectiveTool(VoxelTool tool, bool ctrl, bool shift);

    // T1's Table 4 for the same meanings, linear RGB
    struct VoxelDecalColour {
        float r = 1.0f, g = 1.0f, b = 1.0f;
    };
    VoxelDecalColour voxelDecalColour(VoxelTool effective);

    // "Dig", "Add", ...
    const char* voxelToolName(VoxelTool tool);
    const char* voxelShapeName(VoxelBrushShape shape);
    // The undo step's name, "Dig Sphere"
    std::string voxelStrokeName(VoxelTool effective, VoxelBrushShape shape);

    // The one-line legend under the tool name (T1 R56), with the keys as they are bound now
    std::string voxelBrushLegend(const CommandCatalog& catalog);

    // T1's rule: stamps at arc-length multiples of this, whatever the frame rate
    double voxelStrokeSpacing(double radius, double voxelSize);

    // Where a stroke's stamps fall along its path: the start, then each multiple of the spacing. A path
    // cut into other segments gives the same stamps.
    class VoxelStrokeStamper {
    public:
        explicit VoxelStrokeStamper(double spacing): spacing(spacing) {}

        // The first point of a stroke, or the point it comes back at after the ray left the surface
        void start(const voxel::Vec3d& point, std::vector<voxel::Vec3d>& stamps);
        void moveTo(const voxel::Vec3d& point, std::vector<voxel::Vec3d>& stamps);
        // The ray left the surface: a return does not streak across the gap
        void lift() { anchored = false; }
        bool hasStarted() const { return anchored; }

    private:
        double spacing;
        voxel::Vec3d last;
        // how far past the last stamp the path has come
        double travelled = 0.0;
        bool anchored = false;
    };

    // The operation of one stamp at `center`. `end` is a capsule's other end.
    voxel::EditOp voxelStampOp(const VoxelBrushSettings& settings, VoxelTool effective, const voxel::Vec3d& center, const voxel::Vec3d& end);

    // An Alt press (T1 DC12): it starts nothing. Past the drag threshold it is an orbit, a release before
    // it is a pick. Modifiers read at the press hold for the gesture.
    class VoxelAltGesture {
    public:
        void press(float x, float y);
        // threshold: ImGui's MouseDragThreshold. True from the point the gesture is an orbit.
        bool move(float x, float y, float threshold);
        // True: this was a click, so pick. Ends the gesture.
        bool release();
        void cancel() { active = false; }
        bool isActive() const { return active; }
        bool isOrbit() const { return active && orbit; }

    private:
        float startX = 0.0f, startY = 0.0f;
        bool active = false;
        bool orbit = false;
    };

    // The decal in the volume's frame (R60): the ring draped over the surface around the hit and the
    // shape's wire preview, each a list of line segments, two points each
    struct VoxelDecal {
        std::vector<voxel::Vec3d> ring;
        std::vector<voxel::Vec3d> wire;
        VoxelDecalColour colour;
    };
    void buildVoxelDecal(voxel::Store& store, const VoxelBrushSettings& settings, VoxelTool effective, const voxel::RaycastHit& hit,
                         const voxel::Vec3d* capsuleStart, VoxelDecal& decal);

    // T1 R55: full until the pointer has rested 0.5 s, gone a moment later, gone at once while the camera moves
    float voxelDecalAlpha(double secondsSincePointerMoved, bool cameraMoving);

}
