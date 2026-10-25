// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MODELPOOL_H
#define MODELPOOL_H

#include "Export.h"
#include <map>
#include <memory>
#include <cstdint>
#include <string>

namespace tinygltf { class Model; }

namespace jasperengine{

    struct ObjModelData;

    typedef std::map< std::string, std::shared_ptr<tinygltf::Model> > models_t;
    typedef std::map< std::string, std::shared_ptr<ObjModelData> > obj_models_t;

    class JASPERENGINE_API ModelPool{
    private:
        static models_t& getMap();
        static obj_models_t& getObjMap();
        static std::map<std::string, uint32_t>& getGenerations();

    public:
        static std::shared_ptr<tinygltf::Model> getGLTF(const std::string& id);
        static std::shared_ptr<tinygltf::Model> addGLTF(const std::string& id, std::shared_ptr<tinygltf::Model> model);
        static std::shared_ptr<ObjModelData> getObj(const std::string& id);
        static std::shared_ptr<ObjModelData> addObj(const std::string& id, std::shared_ptr<ObjModelData> model);
        static void removeGLTF(const std::string& id);
        static void removeObj(const std::string& id);
        // Drops the entry even while others hold it, so the next load parses the file again
        static void invalidate(const std::string& id);

        // Hot reload (S05 R13): the entry becomes the new parse and the id's generation goes
        // up (one counter for both formats). Main thread.
        static void replaceGLTF(const std::string& id, std::shared_ptr<tinygltf::Model> model);
        static void replaceObj(const std::string& id, std::shared_ptr<ObjModelData> model);
        static uint32_t generation(const std::string& id);

        // necessary for engine shutdown
        static void clear();
        // removes only entries no one else references (safe while other scenes are alive)
        static void clearUnused();
    };
}

#endif /* MODELPOOL_H */
