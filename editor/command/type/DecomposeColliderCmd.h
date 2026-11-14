// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Decompose Into Convex Hulls (docs/specs/S96-physics-gaps.md REQ-20, D7): a Mesh collider of a
// Rigidbody turned into convex hull colliders a dynamic or kinematic body can move with. V-HACD
// runs on a background job; its result replaces the collider in one undo step, keeping its
// pose, material, trigger flag, density and receiver. An edit-time bake: players never run V-HACD.

#include "command/type/MultiPropertyCmd.h"
#include "component/RigidbodyComponent.h"
#include "physics/ConvexDecomposition.h"
#include "Project.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // The collider list with the decomposed one replaced: "Decompose Collider (Rock)"
    class DecomposeColliderCmd: public MultiPropertyCmd{
    private:
        Project* project;
        uint32_t sceneId;
        Entity entity;

    public:
        DecomposeColliderCmd(Project* project, uint32_t sceneId, Entity entity, std::vector<Collider3D> colliders);

        std::string describe() const override {
            return "Decompose Collider (" + CommandNames::entity(project, sceneId, entity) + ")";
        }
        const char* typeName() const override { return "DecomposeColliderCmd"; }
    };

    class ColliderDecomposition{
    public:
        struct Request{
            uint32_t sceneId = 0;
            Entity entity = NULL_ENTITY;
            size_t index = 0;
            DecompositionQuality quality = DecompositionQuality::MEDIUM;
            uint32_t maxHulls = 16;
        };

        struct Outcome{
            bool cancelled = false;
            // why it failed ("" when it did not)
            std::string error;
            size_t hulls = 0;
            // the undo step's name
            std::string undoName;
        };

        using Finished = std::function<void(const Outcome&)>;

        static constexpr uint32_t MAX_HULLS = 64;

        // Checks the collider (a Mesh one of a Rigidbody, its mesh loaded) and reads its triangles
        // on the main thread, then decomposes them on a job; `finished` runs on the main thread
        // after the replacement is in the scene's history (or with the error). Null and `error`
        // when it cannot start.
        static std::shared_ptr<ColliderDecomposition> start(Project* project, const Request& request, Finished finished, std::string& error);
        // The decomposition running for that collider, null when none (the Inspector's progress bar)
        static std::shared_ptr<ColliderDecomposition> find(uint32_t sceneId, Entity entity, size_t index);

        // 0 to 1, the V-HACD stage's share, and that stage's name
        double progress() const { return progressValue.load(); }
        std::string stage() const;
        void cancel() { cancelFlag.store(true); }

        // public for std::make_shared; start() makes them
        ColliderDecomposition() = default;

    private:
        Project* project = nullptr;
        Request request;
        Collider3D source;
        std::vector<Vector3> positions;
        std::vector<uint32_t> indices;
        Finished finished;

        std::atomic<bool> cancelFlag{false};
        std::atomic<double> progressValue{0.0};
        mutable std::mutex stageMutex;
        std::string stageName;

        std::vector<ConvexHullPoints> hulls;
        bool succeeded = false;
        std::string failure;

        void run();
        void land();
    };

}
