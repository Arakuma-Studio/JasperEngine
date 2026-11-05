// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Read/Write (spec S53 R11): a game frees the CPU copy of a cooked model's mesh bytes once they are
// on the GPU, when the export found nothing at runtime that reads them (index.json keepCpuData
// false). Only the file's bytes go, and only once every mesh showing them has uploaded and nothing
// else holds the parse (a load in flight, a scene not shown yet). The meshes keep their GPU buffers
// and sizes; their ExternalBuffers show no bytes (Buffer::isDataReleased).
//
// A reader the export could not see stays correct: whoever reads a mesh's bytes calls ensure first,
// which reads the file again and keeps the bytes from then on (a parse is released at most once, so
// no buffer ever points at freed bytes). ModelPool::getGLTF does it for a later load of the file.
// The editor never releases, and a project with no released model pays one flag per frame.

#ifndef MODELCPUDATA_H
#define MODELCPUDATA_H

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tinygltf { class Model; }

namespace jasperengine {

    struct MeshComponent;
    class Scene;

    class JASPERENGINE_API ModelCpuData {
    public:
        // a load finished: a pooled parse whose index entry says nothing reads it (never in the editor)
        static void markReleasable(const std::shared_ptr<tinygltf::Model>& parse, const std::string& filename, const std::string& poolKey);

        // Once a frame while getPendingCount() is not 0, main thread: frees what has uploaded in
        // `scenes`, the engine's (`force`: now; else every 30th call)
        static void update(const std::vector<Scene*>& scenes, bool force = false);

        // The mesh's bytes back in place, from the file when they were released. False when the
        // file cannot be read (logged). Cheap when nothing of it was released.
        static bool ensure(const MeshComponent& mesh);
        static bool ensure(const std::shared_ptr<tinygltf::Model>& parse);

        // what update has freed and ensure has not brought back, in bytes
        static uint64_t getReleasedBytes();
        // parses waiting for their meshes to upload
        static size_t getPendingCount();

        // necessary for engine shutdown
        static void clear();
    };

}

#endif // MODELCPUDATA_H
