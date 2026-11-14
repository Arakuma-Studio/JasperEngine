// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ComponentManager.h"

#include <mutex>
#include <string>
#include <unordered_map>

using namespace jasperengine;

// One registry inside the library, so every module caches the same index for a type.
// Keyed by mangled name, not type_index: identity is then plain string equality.
size_t ComponentTypeRegistry::indexOf(const char* mangledName){
    static std::mutex mutex;
    static std::unordered_map<std::string, size_t> indices;
    static size_t next = 0;

    std::lock_guard<std::mutex> lock(mutex);

    auto it = indices.find(mangledName);
    if (it != indices.end()){
        return it->second;
    }

    indices.emplace(mangledName, next);
    return next++;
}

#if (defined(__GNUG__) || defined(__clang__)) && !defined(_MSC_VER)
#include <cxxabi.h>
#include <cstdlib>
#endif

std::string jasperengine::ecs::readableTypeName(const char* typeName){
    std::string name = typeName ? typeName : "";
#if (defined(__GNUG__) || defined(__clang__)) && !defined(_MSC_VER)
    int status = 0;
    char* demangled = abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &status);
    if (status == 0 && demangled){
        name = demangled;
    }
    std::free(demangled);
#endif
    // MSVC's and clang-cl's names are already readable: "class jasperengine::RenderSystem"
    for (const char* prefix : {"class ", "struct "}){
        const std::string text(prefix);
        if (name.compare(0, text.size(), text) == 0){
            name.erase(0, text.size());
        }
    }
    const size_t scope = name.rfind("::");
    if (scope != std::string::npos && name.find('<') == std::string::npos){
        name.erase(0, scope + 2);
    }
    return name;
}

void ComponentManager::notRegistered(){
    Log::error("Component not registered before use");
    throw std::out_of_range("Component not registered before use");
}
