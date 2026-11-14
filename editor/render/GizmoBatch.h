// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Scene view's gizmos of one kind (the lights' shapes, the bodies' wires, ...) for every
// GameObject of a view, drawn by one renderer, and its icons by one instanced sprite
// (docs/specs/S98-editor-tooling-gaps.md REQ-30, D9). A gizmo object per GameObject took an
// entity of the scene's system range (1..1000) each, so a large scene ran out of them; a kind
// takes one now, whatever the number of objects. A frame's update collects what it draws between
// begin() and the owner's flush; the renderer is written only when that changed.

#include "component/InstancedMeshComponent.h"
#include "component/LineRendererComponent.h"
#include "math/Matrix4.h"
#include "math/Vector3.h"
#include "math/Vector4.h"

#include <cstddef>
#include <vector>

namespace jasperengine{
    class LineRenderer;
    class Scene;
    class Sprite;
}

namespace jasperengine::editor{

    // lines kept by one object between frames (a cage that is rebuilt only when it changes),
    // appended to a batch every frame
    struct GizmoLineList{
        std::vector<LineData> lines;
        void clearLines() { lines.clear(); }
        void addLine(const Vector3& pointA, const Vector3& pointB, const Vector4& color){
            LineData line;
            line.pointA = pointA;
            line.pointB = pointB;
            line.colorA = color;
            line.colorB = color;
            lines.push_back(line);
        }
    };

    class GizmoLineBatch{
    public:
        // toolsLayer: drawn in the view's tools layer (over the geometry), else in the scene it
        // annotates (depth tested, marked as an editor overlay)
        explicit GizmoLineBatch(bool toolsLayer = false): toolsLayer(toolsLayer) {}
        GizmoLineBatch(const GizmoLineBatch&) = delete;
        GizmoLineBatch& operator=(const GizmoLineBatch&) = delete;

        bool isToolsLayer() const { return toolsLayer; }

        // the frame's collection starts empty
        void begin() { pending.clear(); }
        void addLine(const Vector3& pointA, const Vector3& pointB, const Vector4& color);
        void addLine(const Vector3& pointA, const Vector3& pointB, const Vector4& colorA, const Vector4& colorB);
        // lines made in an object's space, placed by its matrix
        void addLines(const std::vector<LineData>& lines, const Matrix4& toWorld);
        void append(const GizmoLineList& list) { pending.insert(pending.end(), list.lines.begin(), list.lines.end()); }
        size_t size() const { return pending.size(); }

        // the collected lines to the renderer (shown, written only when they changed), or the
        // renderer hidden when there are none
        void flush();
        void hide();

        LineRenderer* getRenderer() const { return renderer; }
        void setRenderer(LineRenderer* lines);
        // the renderer is gone or deleted: the next flush draws into a new one
        void forgetRenderer();

    private:
        bool toolsLayer;
        LineRenderer* renderer = nullptr;
        std::vector<LineData> pending;
        std::vector<LineData> shown;
    };

    enum class GizmoIcon{
        Sun = 0,
        Bulb,
        Spot,
        Camera,
        Sound,
        Count
    };

    class GizmoIconBatch{
    public:
        explicit GizmoIconBatch(bool billboard): billboard(billboard) {}
        GizmoIconBatch(const GizmoIconBatch&) = delete;
        GizmoIconBatch& operator=(const GizmoIconBatch&) = delete;

        bool isBillboard() const { return billboard; }

        void begin() { pending.clear(); }
        // an icon of `scale` times its 128 px at a world position
        void add(GizmoIcon icon, const Vector3& position, float scale);
        size_t size() const { return pending.size(); }

        void flush();
        void hide();

        Sprite* getSprite() const { return sprite; }
        // a new sprite gets the icon atlas, the instancing and (in 3D) the billboard
        void setSprite(Sprite* icons);
        void forgetSprite();

    private:
        bool billboard;
        Sprite* sprite = nullptr;
        std::vector<InstanceData> pending;
        std::vector<InstanceData> shown;
    };

}
