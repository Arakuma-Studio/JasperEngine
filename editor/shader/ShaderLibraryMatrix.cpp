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
        std::string label;
        std::function<void(SubmeshShaderInputs&, MeshShaderEnvironment&)> apply;
    };

    const std::vector<MeshCase>& meshCases(){
        static const std::vector<MeshCase> cases = [](){
            std::vector<MeshCase> list = {
                {"skinning", [](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.skinning = true; }},
                {"morph targets", [](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.morphTarget = true; i.morphNormal = true; }},
                {"instancing", [](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.instanced = true; i.instanceFade = true; }},
                {"vertex colour + uv2", [](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.vertexColor4 = true; i.texCoord2 = true; }},
                {"vertex colour vec3", [](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.vertexColor3 = true; }},
                {"texture rect", [](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.textureRect = true; }},
                {"no normals or uv", [](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.normal = false; i.tangent = false; i.texCoord1 = false; }},
                {"fog", [](SubmeshShaderInputs&, MeshShaderEnvironment& e){ e.fog = true; }},
                {"shadows", [](SubmeshShaderInputs&, MeshShaderEnvironment& e){ e.lights = true; e.shadows = true; }},
                // the High and Ultra tiers' filters (S10 'Shq': the Vogel disk, PCSS, the raw-depth view)
                {"shadows, high filters", [](SubmeshShaderInputs&, MeshShaderEnvironment& e){ e.lights = true; e.shadows = true; e.shadowsHQ = true; }},
                {"2d lights + shadows, sprite rect", [](SubmeshShaderInputs& i, MeshShaderEnvironment& e){ i.textureRect = true; e.lights2D = true; e.shadows2D = true; }},
                {"instanced sprite rect", [](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.textureRect = true; i.instanced = true; }},
                // a normal-mapped sprite under 2D lights (@require normalmap: the material's Normal
                // Texture through getNormal2D, the 2D tangent frame of includes/tangent_frame.glsl)
                {"2d lights + shadows, normal-mapped sprite", [](SubmeshShaderInputs& i, MeshShaderEnvironment& e){
                    i.normal = true; i.tangent = true; i.normalTexture = true; i.textureRect = true; i.vertexColor4 = true;
                    e.lights2D = true; e.shadows2D = true; }},
                {"post colour, 2d lights, normal-mapped instanced sprite", [](SubmeshShaderInputs& i, MeshShaderEnvironment& e){
                    i.normal = true; i.tangent = true; i.normalTexture = true; i.textureRect = true; i.instanced = true;
                    e.postColor = true; e.lights2D = true; }},
                // a Terrain's patches (no UV or tangent attributes: uv1 is the terrain's own)
                {"terrain", [](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.terrain = true; i.texCoord1 = false; i.tangent = false; }},
                {"terrain, fog, shadows", [](SubmeshShaderInputs& i, MeshShaderEnvironment& e){
                    i.terrain = true; i.texCoord1 = false; i.tangent = false; e.fog = true; e.lights = true; e.shadows = true; }},
                {"terrain, fog, shadows, high filters", [](SubmeshShaderInputs& i, MeshShaderEnvironment& e){
                    i.terrain = true; i.texCoord1 = false; i.tangent = false; e.fog = true; e.lights = true; e.shadows = true; e.shadowsHQ = true; }},
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
            // every case again bound to lattices (includes/lattice.glsl, never on a terrain), on a
            // mesh with vertex colours: its variants then read them in every pass for a mask
            const size_t unbound = list.size();
            for (size_t c = 0; c < unbound; c++){
                const MeshCase base = list[c];
                if (base.label.find("terrain") != std::string::npos)
                    continue;
                list.push_back({"lattice, " + base.label, [base](SubmeshShaderInputs& i, MeshShaderEnvironment& e){
                    base.apply(i, e);
                    i.lattice = true;
                    if (!i.vertexColor3)
                        i.vertexColor4 = true;
                }});
            }
            // played by a Vertex Animator (includes/vat.glsl, never on a terrain): Vertices and
            // Bones mode alone, instanced (i_vat0/i_vat1) with the fade and shadows, with vertex
            // colours, fog and a lattice after it, and with the post colour and 2D lights
            for (VatVariant vat : {VatVariant::VERTICES, VatVariant::BONES}){
                const std::string mode = (vat == VatVariant::BONES) ? "vat bones" : "vat vertices";
                list.push_back({mode, [vat](SubmeshShaderInputs& i, MeshShaderEnvironment&){ i.vat = vat; }});
                list.push_back({mode + ", instancing, shadows", [vat](SubmeshShaderInputs& i, MeshShaderEnvironment& e){
                    i.vat = vat; i.instanced = true; i.instanceFade = true; e.lights = true; e.shadows = true; }});
                list.push_back({mode + ", vertex colour + uv2, fog, lattice", [vat](SubmeshShaderInputs& i, MeshShaderEnvironment& e){
                    i.vat = vat; i.vertexColor4 = true; i.texCoord2 = true; e.fog = true; i.lattice = true; }});
                list.push_back({mode + ", post colour, 2d lights + shadows, instanced", [vat](SubmeshShaderInputs& i, MeshShaderEnvironment& e){
                    i.vat = vat; i.instanced = true; e.postColor = true; e.lights2D = true; e.shadows2D = true; }});
            }
            return list;
        }();
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
    std::set<std::tuple<int, uint64_t, std::string>> seen;
    auto add = [&](const std::string& label, const std::string& reference, ShaderType type, uint64_t properties){
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
        // take them; with the High filters too), and also without scene lights (its unlit
        // fallback)
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
                for (int shadows = 0; shadows < 3; shadows++){
                    if (!lit && (capacity != 8 || shadows))
                        continue;
                    MeshShaderEnvironment lightingEnvironment = environment;
                    lightingEnvironment.lightCapacity = capacity;
                    lightingEnvironment.shadows = shadows != 0;
                    lightingEnvironment.shadowsHQ = shadows == 2;
                    const std::string label = modeLabel + (lit ? ", " + std::to_string(capacity) + " lights" +
                                                           (shadows == 2 ? ", shadows (high filters)" : (shadows ? ", shadows" : "")) : "");
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
                        // the Water Excluders and Wakes over the most a lit water binds (its textures' budget)
                        if (manifest.getPossibleRequire() & (ShaderRequire_WaterExclusion | ShaderRequire_WaterWake)){
                            MeshShaderEnvironment waterEnvironment = lightingEnvironment;
                            waterEnvironment.fog = true;
                            waterEnvironment.postColor = true;
                            waterEnvironment.waterExclusion = true;
                            waterEnvironment.waterWake = true;
                            addMesh(label + ", fog, post colour, water excluders and wakes, all features", referenceOf(allOn), inputs, waterEnvironment);
                        }
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
    }else if (type == ShaderType::POSTPROCESS){
        // a post-process pass (phase 4): one variant per feature set, no blend or colour bit
        for (const auto& features : featureSets)
            add("pass [" + features.first + "]", ShaderReference::getVariantReference(base, features.second), type, 0);
    }else{
        // the variants RenderSystem loads a component of the type with
        std::vector<std::pair<std::string, uint64_t>> properties;
        if (type == ShaderType::UI){
            properties = {{"plain", ShaderPool::getUIProperties(false, false, false, true)},
                          {"texture", ShaderPool::getUIProperties(true, false, false, true)},
                          {"font atlas", ShaderPool::getUIProperties(false, true, false, false)}};
        }else if (type == ShaderType::POINTS){
            properties = {{"plain", ShaderPool::getPointsProperties(false, false, true, false)},
                          {"texture", ShaderPool::getPointsProperties(true, false, true, false)},
                          {"texture rect", ShaderPool::getPointsProperties(true, false, true, true)}};
        }else if (type == ShaderType::LINES){
            // the hairline, the ribbon of thick lines and trails, its pixel-width form
            properties = {{"ribbon", ShaderPool::getLinesProperties(false, true, true)},
                          {"hairline", ShaderPool::getLinesProperties(false, true)},
                          {"ribbon pixel width", ShaderPool::getLinesProperties(false, true, true, false, true)}};
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
        const uint64_t postColor = ShaderPool::getPostColorProperty(type);
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

void editor::ShaderLibraryMatrix::compileVariants(const std::vector<Variant>& variants, const std::vector<ShaderBackend>& backends,
                                                  const std::string& file, Result& result, std::vector<ShaderData>* reflections){
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
                        result.problems.push_back({file, 0, true, message});
                }
                if (reflections && variant.type != ShaderType::DEPTH && backend == backends.front())
                    reflections->push_back(data);
                continue;
            }

            result.failed++;
            const std::string where = " (" + variant.label + ", " + ShaderPool::getShaderTypeName(variant.type, true) +
                                      " " + variantName + ", " + backendToken(backend) + ")";
            bool located = false;
            for (const auto& entry : ProjectChecks::parseShaderLog(log)){
                const std::string source = entry.value("source", std::string());
                const std::string sourceFile = ShaderVariantCompiler::getLibrarySources().count(source) ? "engine/shaderlib/" + source : source;
                const std::string message = entry.value("message", std::string()) + where;
                if (reported.insert(sourceFile + ":" + std::to_string(entry.value("line", 0)) + ":" + message).second)
                    result.problems.push_back({sourceFile, entry.value("line", 0), entry.value("severity", std::string("error")) == "error", message});
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
                result.problems.push_back({file, 0, true, error + (first.empty() ? "" : ": " + first) + where});
            }
        }
    }
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
    compileVariants(variants, backends, fragFile, result, &reflections);

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

std::vector<editor::ShaderLibraryMatrix::Variant> editor::ShaderLibraryMatrix::buildBuiltinLitVariants(){
    std::vector<Variant> variants;
    std::set<std::pair<uint32_t, std::string>> seen;
    auto add = [&](const std::string& label, const SubmeshShaderVariant& variant){
        const std::string reference = ShaderReference::withFeatures(ShaderReference::BUILTIN_LIT, variant.litDefines);
        if (!variant.litDefines.empty() && seen.insert({variant.properties, reference}).second)
            variants.push_back({label + " [" + featureLabel(variant.litDefines) + "]", reference, ShaderType::MESH, variant.properties});
    };
    // a map's presence is all a variant knows of it
    auto map = [](const char* path){ return Texture(path); };
    auto lobeMaterial = [&](PbrLobe lobe, bool withMap){
        Material material;
        MaterialPbrExtensions& pbr = material.pbr.edit();
        MaterialPbr::lobeEnabled(pbr, lobe) = true;
        if (lobe == PbrLobe::VOLUME)
            pbr.transmission.enabled = true;
        if (withMap){
            for (int m = 0; m < (int)PbrMap::COUNT; m++){
                if (MaterialPbr::getMapLobe((PbrMap)m) == (lobe == PbrLobe::VOLUME ? PbrLobe::TRANSMISSION : lobe))
                    MaterialPbr::getMap(pbr, (PbrMap)m) = map("matrix.png");
            }
        }
        return material;
    };
    // the scene with the most fragment textures a lit mesh binds
    MeshShaderEnvironment worst;
    worst.lights = true;
    worst.shadows = true;
    worst.ibl = true;
    worst.ssao = true;
    worst.postColor = true;
    worst.fog = true;
    SubmeshShaderInputs mesh = baseMeshInputs(nullptr);
    mesh.baseColorTexture = mesh.metallicRoughnessTexture = mesh.normalTexture = mesh.occlusionTexture = mesh.emissiveTexture = true;

    for (const PbrLobeInfo& lobe : MaterialPbr::getLobes()){
        if (lobe.lobe == PbrLobe::HEX)
            continue;
        for (MaterialLighting lighting : {MaterialLighting::PIXEL, MaterialLighting::MIXED}){
            for (int withMap = 0; withMap < 2; withMap++){
                const Material material = lobeMaterial(lobe.lobe, withMap != 0);
                SubmeshShaderInputs inputs = mesh;
                inputs.lighting = lighting;
                inputs.material = &material;
                inputs.pbrTransmission = material.pbr->transmission.enabled;
                add(std::string(lobe.label) + (lighting == MaterialLighting::MIXED ? ", mixed" : ", pixel") + (withMap ? ", map" : ""),
                    RenderSystem::getSubmeshShaderVariant(worst, inputs));
            }
        }
    }

    // every lobe and every map (three bound, by priority), with and without the post colour and
    // with 2D shadows (one map slot less)
    Material all;
    {
        MaterialPbrExtensions& pbr = all.pbr.edit();
        for (const PbrLobeInfo& lobe : MaterialPbr::getLobes())
            MaterialPbr::lobeEnabled(pbr, lobe.lobe) = true;
        for (int m = 0; m < (int)PbrMap::COUNT; m++)
            MaterialPbr::getMap(pbr, (PbrMap)m) = map("matrix.png");
    }
    {
        SubmeshShaderInputs inputs = mesh;
        inputs.lighting = MaterialLighting::PIXEL;
        inputs.material = &all;
        inputs.pbrTransmission = true;
        add("all lobes, 3 maps", RenderSystem::getSubmeshShaderVariant(worst, inputs));
        MeshShaderEnvironment lights2D = worst;
        lights2D.lights2D = true;
        lights2D.shadows2D = true;
        // (transmission takes no 2D lighting: the variant is the one above)
        add("all lobes, 3 maps, 2d shadows", RenderSystem::getSubmeshShaderVariant(lights2D, inputs));
        // the lobes without transmission, which keeps SSAO
        Material noTransmission = all;
        noTransmission.pbr.edit().transmission.enabled = false;
        noTransmission.pbr.edit().volume.enabled = false;
        inputs.material = &noTransmission;
        inputs.pbrTransmission = false;
        add("all lobes but transmission, 3 maps", RenderSystem::getSubmeshShaderVariant(worst, inputs));
        add("all lobes but transmission, 3 maps, 2d shadows", RenderSystem::getSubmeshShaderVariant(lights2D, inputs));
        inputs.texCoord2 = true;
        add("all lobes but transmission, 3 maps, 2d shadows, uv2", RenderSystem::getSubmeshShaderVariant(lights2D, inputs));
        inputs.texCoord2 = false;
        // the mobile Reduced tier
        MeshShaderEnvironment reduced = worst;
        reduced.pbrTier = PbrTier::REDUCED;
        inputs.material = &all;
        inputs.pbrTransmission = true;
        add("all lobes, 3 maps, reduced tier", RenderSystem::getSubmeshShaderVariant(reduced, inputs));
        // a second UV set (the slots' UV sets come from texCoordSets)
        inputs.texCoord2 = true;
        add("all lobes, 3 maps, uv2", RenderSystem::getSubmeshShaderVariant(worst, inputs));
    }

    // hex tiling, unlit and lit (with its maps on both UV sets)
    Material hex;
    hex.pbr.edit().hex.enabled = true;
    {
        SubmeshShaderInputs inputs = mesh;
        inputs.material = &hex;
        add("hex tiling, unlit", RenderSystem::getSubmeshShaderVariant(MeshShaderEnvironment(), inputs));
        inputs.lighting = MaterialLighting::PIXEL;
        add("hex tiling, pixel", RenderSystem::getSubmeshShaderVariant(worst, inputs));
        inputs.texCoord2 = true;
        add("hex tiling, pixel, uv2", RenderSystem::getSubmeshShaderVariant(worst, inputs));
        Material hexLobes = all;
        inputs.material = &hexLobes;
        inputs.pbrTransmission = true;
        add("hex tiling, all lobes", RenderSystem::getSubmeshShaderVariant(worst, inputs));
    }

    // a painted Terrain with hex tiled layers, lit and unlit
    {
        SubmeshShaderInputs terrain = baseMeshInputs(nullptr);
        terrain.terrain = true;
        terrain.terrainBlendMaps = true;
        terrain.terrainSurface = true;
        terrain.terrainHex = true;
        terrain.texCoord1 = false;
        terrain.tangent = false;
        terrain.baseColorTexture = true;
        add("terrain hex tiling, pixel", RenderSystem::getSubmeshShaderVariant(worst, terrain));
        add("terrain hex tiling, unlit", RenderSystem::getSubmeshShaderVariant(MeshShaderEnvironment(), terrain));
        MeshShaderEnvironment high = worst;
        high.litQualityHigh = true;
        add("terrain hex tiling, high quality", RenderSystem::getSubmeshShaderVariant(high, terrain));
        // with Stochastic Tiling layers beside the hex ones (TERRAIN_HEX_STOCHASTIC)
        terrain.terrainHexStochastic = true;
        add("terrain hex tiling and stochastic tiling, pixel", RenderSystem::getSubmeshShaderVariant(worst, terrain));
    }

    // the High Lit Quality on the base variants (with lobes too)
    MeshShaderEnvironment high = worst;
    high.litQualityHigh = true;
    for (MaterialLighting lighting : {MaterialLighting::PIXEL, MaterialLighting::MIXED}){
        SubmeshShaderInputs inputs = mesh;
        inputs.lighting = lighting;
        add(std::string("high quality, ") + (lighting == MaterialLighting::PIXEL ? "pixel" : "mixed"), RenderSystem::getSubmeshShaderVariant(high, inputs));
        MeshShaderEnvironment plain;
        plain.lights = true;
        plain.litQualityHigh = true;
        add(std::string("high quality, plain scene, ") + (lighting == MaterialLighting::PIXEL ? "pixel" : "mixed"),
            RenderSystem::getSubmeshShaderVariant(plain, inputs));
        SubmeshShaderInputs noUV = baseMeshInputs(nullptr);
        noUV.texCoord1 = false;
        noUV.tangent = false;
        noUV.lighting = lighting;
        add(std::string("high quality, no uv, ") + (lighting == MaterialLighting::PIXEL ? "pixel" : "mixed"), RenderSystem::getSubmeshShaderVariant(high, noUV));
        inputs.material = &all;
        inputs.pbrTransmission = true;
        add(std::string("high quality, all lobes, ") + (lighting == MaterialLighting::PIXEL ? "pixel" : "mixed"), RenderSystem::getSubmeshShaderVariant(high, inputs));
    }
    // IBL alone (no lights): the lobes' environment path
    {
        MeshShaderEnvironment ibl;
        ibl.ibl = true;
        SubmeshShaderInputs inputs = mesh;
        inputs.material = &all;
        inputs.pbrTransmission = true;
        add("all lobes, IBL only", RenderSystem::getSubmeshShaderVariant(ibl, inputs));
        MeshShaderEnvironment ambient;
        ambient.lights = true;
        add("all lobes, scene ambient", RenderSystem::getSubmeshShaderVariant(ambient, inputs));
    }
    return variants;
}

editor::ShaderLibraryMatrix::Result editor::ShaderLibraryMatrix::runBuiltinLit(const std::vector<ShaderBackend>& backends){
    Result result;
    result.shader = ShaderReference::BUILTIN_LIT;
    const std::vector<Variant> variants = buildBuiltinLitVariants();
    result.variants = variants.size();
    compileVariants(variants, backends, "engine/shaderlib/mesh.frag", result, nullptr);
    return result;
}
