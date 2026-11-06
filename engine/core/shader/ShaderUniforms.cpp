// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShaderUniforms.h"

#include "Log.h"
#include "shader/ShaderGlobals.h"
#include "shader/ShaderManifest.h"

#include <algorithm>
#include <cstring>

using namespace jasperengine;

bool ShaderUniforms::isReserved(const std::string& name){
    return name == "time" || name == "resolution" || name == "gameTime";
}

bool ShaderUniforms::isEditable(const ShaderUniform& uniform){
    if (uniform.arrayCount > 1)
        return false;

    switch (uniform.type){
        case ShaderUniformType::FLOAT:
        case ShaderUniformType::FLOAT2:
        case ShaderUniformType::FLOAT3:
        case ShaderUniformType::FLOAT4:
        case ShaderUniformType::INT:
        case ShaderUniformType::INT2:
        case ShaderUniformType::INT3:
        case ShaderUniformType::INT4:
            return true;
        default:
            return false;
    }
}

bool ShaderUniforms::set(ShaderUniformValues& values, const std::string& name, const Vector4& value){
    if (isReserved(name)){
        Log::error("Shader uniform '%s' is written by the engine and cannot be set", name.c_str());
        return false;
    }

    for (size_t i = 0; i < values.size(); i++){
        if (values[i].first == name){
            values[i].second = value;
            return true;
        }
    }
    values.push_back({name, value});
    return true;
}

Vector4 ShaderUniforms::get(const ShaderUniformValues& values, const std::string& name){
    for (size_t i = 0; i < values.size(); i++){
        if (values[i].first == name)
            return values[i].second;
    }
    return Vector4::ZERO;
}

bool ShaderUniforms::has(const ShaderUniformValues& values, const std::string& name){
    for (size_t i = 0; i < values.size(); i++){
        if (values[i].first == name)
            return true;
    }
    return false;
}

bool ShaderUniforms::remove(ShaderUniformValues& values, const std::string& name){
    for (size_t i = 0; i < values.size(); i++){
        if (values[i].first == name){
            values.erase(values.begin() + i);
            return true;
        }
    }
    return false;
}

bool CustomUniformBlock::resolve(ShaderData& shaderData, const std::string& blockName){
    clear();

    unsigned int sizeBytes = 0;
    const std::vector<ShaderUniform>* reflected = shaderData.getUniformBlockMembers(blockName, sizeBytes);
    if (!reflected || sizeBytes == 0)
        return false;

    slot = shaderData.getUniformBlockIndexByName(blockName);
    // the backend declares the block rounded up to 16 bytes
    data.assign(((sizeBytes + 15) / 16) * 16, 0);

    bool hasFloat = false;
    bool hasInt = false;
    members.reserve(reflected->size());
    for (size_t m = 0; m < reflected->size(); m++){
        ShaderUniform uniform = (*reflected)[m];
        uniform.name = ShaderData::getUniformShortName(uniform.name);
        if (uniform.name == "time"){
            timeMember = (int)members.size();
        }else if (uniform.name == "gameTime"){
            gameTimeMember = (int)members.size();
        }else if (uniform.name == "resolution"){
            resolutionMember = (int)members.size();
        }
        if (uniform.type >= ShaderUniformType::INT && uniform.type <= ShaderUniformType::INT4){
            hasInt = true;
        }else{
            hasFloat = true;
        }
        members.push_back(uniform);
    }
    mixedTypes = hasInt && hasFloat;

    return true;
}

void CustomUniformBlock::clear(){
    slot = -1;
    members.clear();
    data.clear();
    timeMember = -1;
    gameTimeMember = -1;
    resolutionMember = -1;
    mixedTypes = false;
}

void CustomUniformBlock::writeValues(const ShaderUniformValues& values){
    for (size_t m = 0; m < members.size(); m++){
        if ((int)m == timeMember || (int)m == gameTimeMember || (int)m == resolutionMember)
            continue;
        // unset (or removed) members read zero
        writeUniform(data, members[m], ShaderUniforms::get(values, members[m].name));
    }
}

void CustomUniformBlock::writeValues(const ShaderUniformValues& values, const ShaderUniformValues* fallbackValues, const ShaderManifest* manifest,
                                     const ShaderGlobalBindings* bindings){
    for (size_t m = 0; m < members.size(); m++){
        if ((int)m == timeMember || (int)m == gameTimeMember || (int)m == resolutionMember)
            continue;
        const std::string& name = members[m].name;
        Vector4 value = Vector4::ZERO;
        const std::string global = bindings ? ShaderGlobals::getBinding(*bindings, name) : std::string();
        if (!global.empty()){
            // the global's current value; zero while it does not exist (or is a texture)
            ShaderGlobals::getBoundValue(global, value);
        }else if (ShaderUniforms::has(values, name)){
            value = ShaderUniforms::get(values, name);
        }else if (fallbackValues && ShaderUniforms::has(*fallbackValues, name)){
            value = ShaderUniforms::get(*fallbackValues, name);
        }else if (manifest){
            manifest->getDefault(name, value);
        }
        writeUniform(data, members[m], value);
    }
}

void CustomUniformBlock::writeTime(float seconds){
    if (timeMember != -1)
        writeUniform(data, members[timeMember], Vector4(seconds, seconds, seconds, seconds));
}

void CustomUniformBlock::writeGameTime(float seconds){
    if (gameTimeMember != -1)
        writeUniform(data, members[gameTimeMember], Vector4(seconds, seconds, seconds, seconds));
}

void CustomUniformBlock::writeResolution(float width, float height){
    if (resolutionMember != -1){
        float invWidth = width > 0.0f ? 1.0f / width : 0.0f;
        float invHeight = height > 0.0f ? 1.0f / height : 0.0f;
        writeUniform(data, members[resolutionMember], Vector4(width, height, invWidth, invHeight));
    }
}

void CustomUniformBlock::writeUniform(std::vector<uint8_t>& block, const ShaderUniform& uniform, const Vector4& value){
    if (uniform.arrayCount > 1)
        return;

    float components[4] = {value.x, value.y, value.z, value.w};

    int count = 0;
    bool isInt = false;
    switch (uniform.type){
        case ShaderUniformType::FLOAT:  count = 1; break;
        case ShaderUniformType::FLOAT2: count = 2; break;
        case ShaderUniformType::FLOAT3: count = 3; break;
        case ShaderUniformType::FLOAT4: count = 4; break;
        case ShaderUniformType::INT:    count = 1; isInt = true; break;
        case ShaderUniformType::INT2:   count = 2; isInt = true; break;
        case ShaderUniformType::INT3:   count = 3; isInt = true; break;
        case ShaderUniformType::INT4:   count = 4; isInt = true; break;
        default: return;
    }

    if ((size_t)(uniform.offset + count * 4) > block.size())
        return;

    for (int i = 0; i < count; i++){
        if (isInt){
            int intValue = (int)components[i];
            memcpy(block.data() + uniform.offset + i * 4, &intValue, 4);
        }else{
            memcpy(block.data() + uniform.offset + i * 4, &components[i], 4);
        }
    }
}

void CustomUniformBlock::writeUniformArray(std::vector<uint8_t>& block, const ShaderUniform& uniform, const Vector4* values, size_t count){
    int components = 0;
    bool isInt = false;
    switch (uniform.type){
        case ShaderUniformType::FLOAT:  components = 1; break;
        case ShaderUniformType::FLOAT2: components = 2; break;
        case ShaderUniformType::FLOAT3: components = 3; break;
        case ShaderUniformType::FLOAT4: components = 4; break;
        case ShaderUniformType::INT:    components = 1; isInt = true; break;
        case ShaderUniformType::INT2:   components = 2; isInt = true; break;
        case ShaderUniformType::INT3:   components = 3; isInt = true; break;
        case ShaderUniformType::INT4:   components = 4; isInt = true; break;
        default: return;
    }
    const size_t elements = std::min<size_t>(count, uniform.arrayCount > 0 ? uniform.arrayCount : 1);
    for (size_t e = 0; e < elements; e++){
        const size_t offset = uniform.offset + e * 16;
        if (offset + components * 4 > block.size())
            return;
        const float c[4] = {values[e].x, values[e].y, values[e].z, values[e].w};
        for (int i = 0; i < components; i++){
            if (isInt){
                const int intValue = (int)c[i];
                memcpy(block.data() + offset + i * 4, &intValue, 4);
            }else{
                memcpy(block.data() + offset + i * 4, &c[i], 4);
            }
        }
    }
}
