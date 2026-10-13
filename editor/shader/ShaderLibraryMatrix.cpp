// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShaderLibraryMatrix.h"

#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"
#include "subsystem/RenderSystem.h"
#include "util/ProjectChecks.h"

#include <algorithm>
#include <functional>
#include <set>
#include <sstream>
#include <tuple>

using namespace jasperengine;

namespace {

    // Applies a lighting mode to the inputs and scene of a mesh variant: the material's
    // lighting, and for a lit mode a scene with lights (the variant otherwise draws unlit).
    // False when this build cannot express the mode (reported and skipped).
    bool applyLightingMode(ShaderLightingMode mode, SubmeshShaderInputs& inputs, MeshShaderEnvironment& environment){
        switch (mode){
            case ShaderLightingMode::NONE:
                inputs.lighting = MaterialLighting::NONE;
                return true;
            case ShaderLightingMode::PIXEL:
                inputs.lighting = MaterialLighting::PIXEL;
                environment.lights = true;
                return true;
            case ShaderLightingMode::VERTEX:
                inputs.lighting = MaterialLighting::VERTEX;
                environment.lights = true;
                return true;
            case ShaderLightingMode::MIXED:
                inputs.lighting = MaterialLighting::MIXED;
                environment.lights = true;
                return true;
        }
        return false;
    }

    // the per-object light capacity tiers a lit variant compiles at (RenderSystem::getLightCapacity)
    const int lightCapacities[] = {8, 4, 16};

    // a mesh with normals, tangents and one UV set, in a scene without lights
    SubmeshShaderInputs baseMeshInputs(const std::shared_ptr<const ShaderManifest>& manifest){
        SubmeshShaderInputs inputs;
        inputs.texCoord1 = true;
        inputs.normal = true;
        inputs.tangent = true;
        inputs.manifest = manifest;
        inputs.depthProgram = manifest && manifest->depth;
        return inputs;
    }

    struct MeshCase{
        const char* label;
        std::function<void(SubmeshShaderInputs&, MeshShaderEnvironment&)> apply;
    };

    const std::vector<MeshCase>& meshCases(){
        static const std::vector<MeshCase> cases = {
            {"skinning", [](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.skinning = true; }},
            {"morph targets", [](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.morphTarget = true; i.morphNormal = true; }},
            {"instancing", [](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.instanced = true; i.instanceFade = true; }},
            {"vertex colour + uv2", [](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.vertexColor4 = true; i.texCoord2 = true; }},
            {"vertex colour vec3", [](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.vertexColor3 = true; }},
            {"texture rect", [](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.textureRect = true; }},
            {"no normals or uv", [](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.normal = false; i.tangent = false; i.texCoord1 = false; }},
            {"fog", [](SubmeshShaderInputs&, MeshShaderEnvironment& e){ e.fog = true; }},
            {"shadows", [](SubmeshShaderInputs&, MeshShaderEnvironment& e){ e.lights = true; e.shadows = true; }},
            {"2d lights + shadows, sprite rect", [](SubmeshShaderInputs& i, MeshShaderEnvironment& e){ i.textureRect = true; e.lights2D = true; e.shadows2D = true; }},
            {"instanced sprite rect", [](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.textureRect = true; i.instanced = true; }},
            // a Terrain's patches (no UV or tangent attributes: uv1 is the terrain's own)
            {"terrain", [](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.terrain = true; i.texCoord1 = false; i.tangent = false; }},
            {"terrain, fog, shadows", [](SubmeshShaderInputs& i, MeshShaderEnvironment& e){
                i.terrain = true; i.texCoord1 = false; i.tangent = false; e.fog = true; e.lights = true; e.shadows = true; }},
            // the post-processing colour (phase 4, USE_POST_COLOR): its parameters in the frame
            // block or a block of its own, and its LUT texture, next to everything else
            {"post colour", [](SubmeshShaderInputs&, MeshShaderEnvironment& e){ e.postColor = true; }},
            {"post colour, fog, shadows", [](SubmeshShaderInputs&, MeshShaderEnvironment& e){
                e.postColor = true; e.fog = true; e.lights = true; e.shadows = true; }},
            {"post colour, 2d lights + shadows, sprite rect", [](SubmeshShaderInputs& i, MeshShaderEnvironment& e){
                e.postColor = true; i.textureRect = true; e.lights2D = true; e.shadows2D = true; }},
            {"post colour, instanced, skinning", [](SubmeshShaderInputs& i, MeshShaderEnvironment& e){
                e.postColor = true; i.instanced = true; i.instanceFade = true; i.skinning = true; }},
            {"post colour, terrain, fog, shadows", [](SubmeshShaderInputs& i, MeshShaderEnvironment& e){
                e.postColor = true; i.terrain = true; i.texCoord1 = false; i.tangent = false; e.fog = true; e.lights = true; e.shadows = true; }},
        };
        return cases;
    }

    std::string featureLabel(const std::vector<std::string>& defines){
        if (defines.empty())
            return "no features";
        std::string label;
        for (const std::string& define : defines)
            label += (label.empty() ? "" : ",") + define;
        return label;
    }

    const char* backendToken(ShaderBackend backend){
        static std::string token;
        token = ShaderPool::getShaderBackendCliToken(backend);
        return token.c_str();
    }

}

std::vector<editor::ShaderLibraryMatrix::Variant> editor::ShaderLibraryMatrix::buildVariants(const std::string& name, const ShaderManifest& manifest){
    const std::string base = std::string(ShaderReference::LIBRARY_PREFIX) + name;
    std::shared_ptr<const ShaderManifest> shared = ShaderManifests::get(base);

    std::vector<Variant> variants;
    std::set<std::tuple<int, uint32_t, std::string>> seen;
    auto add = [&](const std::string& label, const std::string& reference, ShaderType type, uint32_t properties){
        if (seen.insert({static_cast<int>(type), properties, reference}).second)
            variants.push_back({label, reference, type, properties});
    };

    // the feature combinations: the defaults, each feature alone, all on, each keyword value
    std::vector<std::pair<std::string, std::vector<std::string>>> featureSets;
    featureSets.push_back({"defaults", {}});
    for (const ShaderManifestFeature& feature : manifest.features){
        std::vector<std::string> list{feature.define};
        for (const ShaderManifestFeature& other : manifest.features){
            if (other.define != feature.define)
                list.push_back("!" + other.define);
        }
        featureSets.push_back({"feature " + feature.define, list});
        // a default-on feature turned off (with a single feature no other set does it)
        if (feature.defaultOn)
            featureSets.push_back({"feature " + feature.define + " off", {"!" + feature.define}});
    }
    if (manifest.features.size() > 1){
        std::vector<std::string> all;
        for (const ShaderManifestFeature& feature : manifest.features)
            all.push_back(feature.define);
        featureSets.push_back({"all features", all});
    }
    for (const ShaderManifestKeywords& group : manifest.keywords){
        for (const std::string& value : group.values){
            featureSets.push_back({"keyword " + group.group + "_" + value, {group.group + "_" + value}});
            // a keyword may only matter with a feature (a second layer's combine mode)
            if (!manifest.features.empty()){
                std::vector<std::string> list{group.group + "_" + value};
                for (const ShaderManifestFeature& feature : manifest.features)
                    list.push_back(feature.define);
                featureSets.push_back({"all features, keyword " + group.group + "_" + value, list});
            }
        }
    }
    // each keyword value with every feature on (a keyword often changes what the features read)
    if (!manifest.features.empty()){
        for (const ShaderManifestKeywords& group : manifest.keywords){
            for (const std::string& value : group.values){
                std::vector<std::string> list{group.group + "_" + value};
                for (const ShaderManifestFeature& feature : manifest.features)
                    list.push_back(feature.define);
                featureSets.push_back({"keyword " + group.group + "_" + value + ", all features", list});
            }
        }
    }

    // a feature list as a colour variant is keyed (with the BLEND_* define of the blend its
    // @state resolves to for those features, or of an explicit blend) and as the depth
    // program is (features only)
    auto referenceOf = [&](const std::vector<std::string>& list, MaterialBlendMode blend = MaterialBlendMode::AUTO){
        return ShaderReference::getColorVariantReference(base, list, blend);
    };
    auto depthReferenceOf = [&](const std::string& colorReference){
        std::vector<std::string> defines = ShaderReference::getFeatures(colorReference);
        defines.erase(std::remove_if(defines.begin(), defines.end(), ShaderReference::isBlendDefine), defines.end());
        return ShaderReference::withFeatures(base, defines);
    };
    std::vector<std::string> allOn;
    for (const ShaderManifestFeature& feature : manifest.features)
        allOn.push_back(feature.define);
    const MaterialBlendMode blendModes[] = {MaterialBlendMode::NONE, MaterialBlendMode::ALPHA, MaterialBlendMode::PREMULTIPLIED,
                                            MaterialBlendMode::ADDITIVE, MaterialBlendMode::MULTIPLY};
    auto blendLabel = [](MaterialBlendMode blend){
        return std::string(ShaderReference::getBlendDefine(blend));
    };

    const ShaderType type = manifest.hasType ? manifest.type : ShaderType::MESH;
    if (type == ShaderType::MESH){
        auto addMesh = [&](const std::string& label, const std::string& reference, SubmeshShaderInputs inputs, MeshShaderEnvironment environment){
            const std::string depthReference = depthReferenceOf(reference);
            // the features decide the feature= @require inputs
            inputs.features = ShaderReference::getFeatures(depthReference);
            const SubmeshShaderVariant variant = RenderSystem::getSubmeshShaderVariant(environment, inputs);
            add(label + " [" + featureLabel(ShaderReference::getFeatures(reference)) + "]", reference, ShaderType::MESH, variant.properties);
            if (manifest.depth){
                // the depth program, as the shadow and depth pre-passes draw it
                environment.depthPrepass = true;
                const SubmeshShaderVariant depth = RenderSystem::getSubmeshShaderVariant(environment, inputs);
                add(label + " depth [" + featureLabel(inputs.features) + "]", depthReference, ShaderType::DEPTH, depth.depthProperties);
            }
        };

        // the lighting modes it supports, the first (default) with every feature set; a lit
        // mode at each light capacity tier, with and without shadows (only pixel lights
        // take them), and also without scene lights (its unlit fallback)
        std::vector<ShaderLightingMode> modes = manifest.lighting;
        if (modes.empty())
            modes.push_back(ShaderLightingMode::NONE);
        for (ShaderLightingMode mode : modes){
            SubmeshShaderInputs inputs = baseMeshInputs(shared);
            MeshShaderEnvironment environment;
            if (!applyLightingMode(mode, inputs, environment))
                continue; // reported by run()
            const std::string modeLabel = "lighting " + ShaderManifest::getLightingModeName(mode);
            const bool lit = mode != ShaderLightingMode::NONE;

            for (int capacity : lightCapacities){
                for (int shadows = 0; shadows < 2; shadows++){
                    if (!lit && (capacity != 8 || shadows))
                        continue;
                    MeshShaderEnvironment lightingEnvironment = environment;
                    lightingEnvironment.lightCapacity = capacity;
                    lightingEnvironment.shadows = shadows != 0;
                    const std::string label = modeLabel + (lit ? ", " + std::to_string(capacity) + " lights" + (shadows ? ", shadows" : "") : "");
                    if (mode == modes.front() && capacity == 8 && !shadows){
                        for (const auto& features : featureSets)
                            addMesh(label + ", " + features.first, referenceOf(features.second), inputs, lightingEnvironment);
                        // each blend mode (the BLEND_* define output.glsl routes the fog by),
                        // with fog, with the default features and with all of them
                        MeshShaderEnvironment fogEnvironment = lightingEnvironment;
                        fogEnvironment.fog = true;
                        for (MaterialBlendMode blend : blendModes){
                            addMesh(label + ", fog, " + blendLabel(blend), referenceOf({}, blend), inputs, fogEnvironment);
                            if (!allOn.empty())
                                addMesh(label + ", fog, " + blendLabel(blend) + ", all features", referenceOf(allOn, blend), inputs, fogEnvironment);
                        }
                        // the same with the post-processing colour (output.glsl encodes by the blend)
                        MeshShaderEnvironment postEnvironment = fogEnvironment;
                        postEnvironment.postColor = true;
                        for (MaterialBlendMode blend : blendModes){
                            addMesh(label + ", fog, post colour, " + blendLabel(blend), referenceOf({}, blend), inputs, postEnvironment);
                            if (!allOn.empty())
                                addMesh(label + ", fog, post colour, " + blendLabel(blend) + ", all features", referenceOf(allOn, blend), inputs, postEnvironment);
                        }
                    }else{
                        addMesh(label, referenceOf({}), inputs, lightingEnvironment);
                        addMesh(label + ", all features", referenceOf(allOn), inputs, lightingEnvironment);
                        if (capacity == 8){
                            // the other keyword values in this mode, with every feature on
                            for (const ShaderManifestKeywords& group : manifest.keywords){
                                for (size_t v = 1; v < group.values.size(); v++){
                                    std::vector<std::string> list = allOn;
                                    list.push_back(group.group + "_" + group.values[v]);
                                    addMesh(label + ", keyword " + list.back() + ", all features", referenceOf(list), inputs, lightingEnvironment);
                                }
                            }
                        }
                    }
                }
            }
            if (lit){
                MeshShaderEnvironment unlitEnvironment;
                addMesh(modeLabel + ", no scene lights", referenceOf({}), inputs, unlitEnvironment);
                addMesh(modeLabel + ", no scene lights, all features", referenceOf(allOn), inputs, unlitEnvironment);
            }

            // the mesh variants, with the default features and with all of them
            for (const MeshCase& meshCase : meshCases()){
                SubmeshShaderInputs caseInputs = inputs;
                MeshShaderEnvironment caseEnvironment = environment;
                meshCase.apply(caseInputs, caseEnvironment);
                addMesh(modeLabel + ", " + meshCase.label, referenceOf({}), caseInputs, caseEnvironment);
                if (!allOn.empty())
                    addMesh(modeLabel + ", " + meshCase.label, referenceOf(allOn), caseInputs, caseEnvironment);
            }
        }
    }else{
        // the variants RenderSystem loads a component of the type with
        std::vector<std::pair<std::string, uint32_t>> properties;
        if (type == ShaderType::UI){
            properties = {{"plain", ShaderPool::getUIProperties(false, false, false, true)},
                          {"texture", ShaderPool::getUIProperties(true, false, false, true)},
                          {"font atlas", ShaderPool::getUIProperties(false, true, false, false)}};
        }else if (type == ShaderType::POINTS){
            properties = {{"plain", ShaderPool::getPointsProperties(false, false, true, false)},
                          {"texture", ShaderPool::getPointsProperties(true, false, true, false)},
                          {"texture rect", ShaderPool::getPointsProperties(true, false, true, true)}};
        }else if (type == ShaderType::LINES){
            properties = {{"plain", ShaderPool::getLinesProperties(false, true)}};
        }else{
            properties = {{"sky", 0}};
        }
        for (const auto& features : featureSets){
            for (const auto& variant : properties)
                add(variant.first + " [" + features.first + "]", referenceOf(features.second), type, variant.second);
        }
        // each blend mode a component can set (the BLEND_* define)
        for (MaterialBlendMode blend : blendModes){
            add(properties.front().first + " [" + blendLabel(blend) + "]", referenceOf({}, blend), type, properties.front().second);
            if (!allOn.empty())
                add(properties.front().first + " [" + blendLabel(blend) + ", all features]", referenceOf(allOn, blend), type, properties.front().second);
        }
        // the post-processing colour (phase 4): every variant with it, each blend mode with all features
        const uint32_t postColor = ShaderPool::getPostColorProperty(type);
        if (postColor){
            for (const auto& variant : properties){
                add(variant.first + " [post colour]", referenceOf({}), type, variant.second | postColor);
                if (!allOn.empty())
                    add(variant.first + " [post colour, all features]", referenceOf(allOn), type, variant.second | postColor);
            }
            for (MaterialBlendMode blend : blendModes){
                add(properties.front().first + " [post colour, " + blendLabel(blend) + "]", referenceOf({}, blend), type, properties.front().second | postColor);
                if (!allOn.empty())
                    add(properties.front().first + " [post colour, " + blendLabel(blend) + ", all features]", referenceOf(allOn, blend),
                        type, properties.front().second | postColor);
            }
        }
    }

    return variants;
}

editor::ShaderLibraryMatrix::Result editor::ShaderLibraryMatrix::run(const std::string& name, const std::vector<ShaderBackend>& backends){
    Result result;
    const std::string base = std::string(ShaderReference::LIBRARY_PREFIX) + name;
    result.shader = base;
    const std::string fragFile = "engine/shaderlib/library/" + name + ".frag";

    std::shared_ptr<const ShaderManifest> cached = ShaderManifests::get(base);
    if (!cached){
        result.failed++;
        result.problems.push_back({fragFile, 0, true, "unknown library shader '" + name + "'"});
        return result;
    }
    ShaderManifest manifest = *cached;
    if (!manifest.present){
        result.problems.push_back({fragFile, 1, true, "manifest: a library shader needs a manifest (// @shader ..., // @name ...)"});
    }

    // lighting modes this build cannot compile yet
    for (ShaderLightingMode mode : manifest.lighting){
        SubmeshShaderInputs inputs;
        MeshShaderEnvironment environment;
        if (!applyLightingMode(mode, inputs, environment))
            result.problems.push_back({fragFile, 1, false, "lighting mode '" + ShaderManifest::getLightingModeName(mode) + "' is not in the compile matrix of this build"});
    }

    const std::vector<Variant> variants = buildVariants(name, manifest);
    result.variants = variants.size();

    // the color variants that compiled, for the manifest's rows
    std::vector<ShaderData> reflections;
    std::set<std::string> reported;

    for (const Variant& variant : variants){
        const uint16_t customId = ShaderPool::registerCustomShader(variant.reference);
        const ShaderKey key = ShaderPool::getShaderKey(variant.type, variant.properties, customId);
        const CustomShaderSources sources = ShaderVariantCompiler::resolveCustomShaderSources(variant.reference, nullptr, variant.type);
        const std::string variantName = ShaderPool::getShaderStr(variant.type, variant.properties, customId);

        for (ShaderBackend backend : backends){
            ShaderData data;
            std::string error;
            std::string log;
            if (ShaderVariantCompiler::compile(key, backend, &sources, data, &error, &log)){
                result.compiled++;
                // the per-stage budget every backend has (a GLES3 device: 16 texture units)
                const std::string over = ShaderVariantCompiler::checkResourceBudget(data);
                if (!over.empty()){
                    result.failed++;
                    const std::string message = "over the resource budget: " + over + " (" + variant.label + ", " +
                        ShaderPool::getShaderTypeName(variant.type, true) + " " + variantName + ", " + backendToken(backend) + ")";
                    if (reported.insert(message).second)
                        result.problems.push_back({fragFile, 0, true, message});
                }
                if (variant.type != ShaderType::DEPTH && backend == backends.front())
                    reflections.push_back(data);
                continue;
            }

            result.failed++;
            const std::string where = " (" + variant.label + ", " + ShaderPool::getShaderTypeName(variant.type, true) +
                                      " " + variantName + ", " + backendToken(backend) + ")";
            bool located = false;
            for (const auto& entry : ProjectChecks::parseShaderLog(log)){
                const std::string source = entry.value("source", std::string());
                const std::string file = ShaderVariantCompiler::getLibrarySources().count(source) ? "engine/shaderlib/" + source : source;
                const std::string message = entry.value("message", std::string()) + where;
                if (reported.insert(file + ":" + std::to_string(entry.value("line", 0)) + ":" + message).second)
                    result.problems.push_back({file, entry.value("line", 0), entry.value("severity", std::string("error")) == "error", message});
                located = true;
            }
            if (!located){
                std::string first;
                std::istringstream lines(log);
                std::string line;
                while (first.empty() && std::getline(lines, line)){
                    if (line.find_first_not_of(" \t\r") != std::string::npos)
                        first = line;
                }
                result.problems.push_back({fragFile, 0, true, error + (first.empty() ? "" : ": " + first) + where});
            }
        }
    }

    // the manifest: its syntax, and its rows against every variant that compiled
    if (manifest.present){
        std::vector<const ShaderData*> pointers;
        for (const ShaderData& data : reflections)
            pointers.push_back(&data);
        if (!pointers.empty())
            manifest.validateReflection(pointers, fragFile);
        for (const ShaderManifestDiagnostic& d : manifest.diagnostics)
            result.problems.push_back({d.file, d.line, d.error, "manifest: " + d.message});
    }

    return result;
}
