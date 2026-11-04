// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Materials and lighting (docs/specs/S11-materials-lighting.md R21): the built-in Lit variant a
// submesh draws with (its extended lobes, the extension maps it binds and leaves out, Lit
// Quality) and a light's photometric unit, converted so it stays as bright. The lobes
// themselves are material values (create_material / update_material pbr, set_properties
// submeshes[i].material.pbr.<lobe>.<field>), documented in jasper://docs/materials.
#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"

#include "Project.h"
#include "command/type/MultiPropertyCmd.h"
#include "component/LightComponent.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderManifest.h"
#include "subsystem/RenderSystem.h"
#include "texture/MaterialPbr.h"
#include "util/LightPhotometry.h"

#include <set>

namespace jasperengine::editor::mcp {

namespace {

// the distinct builtin:lit variants the open scenes' meshes draw with (a project past
// MaterialPbr::VARIANT_WARNING compiles and ships many)
size_t litVariantCount(Project* project){
    std::set<std::pair<uint64_t, uint64_t>> variants;
    for (SceneProject* sceneProject : project->getScenesAndPrefabStages()){
        if (!sceneProject->scene) continue;
        auto meshes = sceneProject->scene->getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); i++){
            const MeshComponent& mesh = meshes->getComponentFromIndex(i);
            for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
                if (mesh.submeshes[s].litDefineBits != 0)
                    variants.insert({mesh.submeshes[s].litDefineBits, mesh.submeshes[s].shaderProperties});
            }
        }
    }
    return variants.size();
}

void getMaterialVariantTool(const CallPtr& call){
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* sceneProject = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!sceneProject){
        call->fail(error);
        return;
    }
    const Entity entity = McpRefs::resolveEntity(sceneProject, args.contains("entity") ? args["entity"] : Json(), error);
    if (entity == NULL_ENTITY){
        call->fail(error);
        return;
    }
    Scene* scene = sceneProject->scene;
    const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
    if (!mesh || mesh->numSubmeshes == 0){
        call->fail(McpRefs::entityPath(sceneProject, entity) + " has no Mesh component with submeshes.");
        return;
    }
    const int submesh = args.value("submesh", 0);
    if (submesh < 0 || (unsigned int)submesh >= mesh->numSubmeshes){
        call->fail("submesh must be 0.." + std::to_string(mesh->numSubmeshes - 1) + " for " + McpRefs::entityPath(sceneProject, entity) + ".");
        return;
    }
    const Submesh& sub = mesh->submeshes[submesh];
    const bool terrain = scene->findComponent<TerrainComponent>(entity) != nullptr;

    // as loaded (the defines the draw asked for) and as the material asks now (the next load)
    const std::vector<std::string> loaded = MaterialPbr::getDefinesFromBits(sub.litDefineBits);
    const PbrVariant asked = terrain ? PbrVariant() : RenderSystem::getLoadedPbrVariant(sub, MaterialPbr::getRuntimeTier());
    const std::string& sceneMeshShader = scene->getDefaultCustomShader(ShaderType::MESH);
    const std::string reference = RenderSystem::getSubmeshShaderReference(*mesh, sub, sceneMeshShader, loaded);
    const uint16_t customId = reference.empty() ? 0 : ShaderPool::registerCustomShader(reference);
    const bool failed = !loaded.empty() && sub.customShaderId == 0 &&
                        ShaderPool::isShaderBuildFailed(ShaderType::MESH, sub.shaderProperties, customId);
    // drawn with the variant it asked for (a plain one stands in while it builds, or when it failed)
    const bool ready = mesh->loaded && !mesh->needReload && !mesh->needVariantReload && sub.customShaderId == customId;

    Json maps = Json::array();
    for (PbrMap map : asked.maps) maps.push_back(MaterialPbr::getMapName(map));
    const int budget = MaterialPbr::MAX_EXTENSION_MAPS - (((sub.shaderProperties & (1u << 23)) != 0) ? 1 : 0);
    const PbrVariant mobile = terrain ? PbrVariant() : RenderSystem::getLoadedPbrVariant(sub, MaterialPbr::resolveTier(true, project->getMobilePbrTier()));

    Json data = {
        {"reference", reference},
        {"basename", loaded.empty() ? std::string() : MaterialPbr::getVariantBasename(loaded)},
        {"defines", loaded},
        {"material_defines", asked.defines},
        {"customId", customId},
        {"ready", ready},
        // why it is not: the mesh not loaded or waiting for a reload, or drawing another variant
        {"state", {{"loaded", mesh->loaded}, {"needReload", mesh->needReload}, {"needVariantReload", mesh->needVariantReload},
                   {"drawnCustomId", sub.customShaderId}}},
        {"failed", failed},
        {"extensionMaps", {{"used", maps}, {"max", budget}, {"dropped", asked.droppedMaps}}},
        {"mobileStripped", mobile.strippedMaps},
        {"litQuality", scene->getLitQuality() == LitQuality::HIGH ? "high" : "standard"},
        {"terrain", terrain},
        {"projectLitVariants", litVariantCount(project)}
    };
    std::string text = McpRefs::entityPath(sceneProject, entity) + " submesh " + std::to_string(submesh) + ": " +
                       (reference.empty() ? std::string("builtin:lit (no extended lobes)") : reference) +
                       (failed ? " (failed to build: the plain variant draws; see read_console)" : ready ? " (ready)" : " (compiling)") +
                       "; extension maps " + std::to_string(asked.maps.size()) + " / " + std::to_string(budget) + ".";
    if (!asked.droppedMaps.empty()){
        text += " Dropped (over the map budget, their factors draw):";
        for (const std::string& map : asked.droppedMaps) text += " " + map;
        text += ".";
    }
    if (litVariantCount(project) > (size_t)MaterialPbr::VARIANT_WARNING)
        text += " The open scenes draw " + std::to_string(litVariantCount(project)) + " builtin:lit variants (more than " +
                std::to_string(MaterialPbr::VARIANT_WARNING) + "): each is a compile and an exported shader per backend.";
    call->ok(text, data);
}

void setLightUnitTool(const CallPtr& call){
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* sceneProject = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!sceneProject){
        call->fail(error);
        return;
    }
    std::vector<Entity> entities;
    if (!McpRefs::resolveEntityArgs(sceneProject, args, entities, error)){
        call->fail(error);
        return;
    }
    int unit = 0;
    if (!McpValues::enumFromJson(args.contains("unit") ? args["unit"] : Json(), *McpValues::enumTable("light_unit"), unit, error)){
        call->fail("unit: " + error);
        return;
    }
    const bool preserve = args.value("preserve_brightness", true);
    const float ev100 = sceneProject->scene->getPhysicalLightEV100();

    auto* multi = new MultiPropertyCmd();
    Json changed = Json::array();
    for (Entity entity : entities){
        const LightComponent* light = sceneProject->scene->findComponent<LightComponent>(entity);
        if (!light){
            delete multi;
            call->fail(McpRefs::entityPath(sceneProject, entity) + " has no Light component.");
            return;
        }
        LightComponent converted = *light;
        LightPhotometry::setIntensityUnit(converted, (LightUnit)unit, preserve, ev100);
        multi->addPropertyCmd<int>(project, sceneProject->id, entity, ComponentType::LightComponent, "intensityUnit", (int)converted.intensityUnit);
        multi->addPropertyCmd<float>(project, sceneProject->id, entity, ComponentType::LightComponent, "intensity", converted.intensity);
        changed.push_back({{"entity", McpRefs::entityJson(sceneProject, entity)},
                           {"unit", LightPhotometry::getUnitName(converted.intensityUnit)},
                           {"intensity", McpValues::number(converted.intensity)},
                           {"engineIntensity", McpValues::number(LightPhotometry::toEngineIntensity(converted.intensity, converted.intensityUnit, ev100))}});
    }
    if (!runSceneCommand(sceneProject->id, multi, error, "The lights already use that unit.")){
        call->fail(error);
        return;
    }
    call->ok("Set the unit of " + std::to_string(entities.size()) + " light(s)" + (preserve ? ", as bright as before" : "") +
             " (EV100 " + Json(McpValues::number(ev100)).dump() + "; a unit a type cannot take becomes its neighbour: lux and candela).",
             Json{{"lights", changed}, {"ev100", McpValues::number(ev100)}});
}

}

void registerMaterialsLightingTools(McpRegistry& registry){
    using namespace schema;
    {
        ToolDef tool;
        tool.name = "get_material_variant";
        tool.title = "Get Material Variant";
        tool.description =
            "The built-in Lit variant a mesh submesh draws with: its builtin:lit reference and defines (extended lobes "
            "PBR_*, hex tiling, USE_LIT_QUALITY), customId, whether it is built (ready) or still compiling, the extension "
            "maps it binds (at most 3) and the ones dropped over that budget, what the mobile tier strips, the scene's Lit "
            "Quality. Lobes: jasper://docs/materials.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"submesh", withDefault(integer("", 0, 1024), 0)}
        }, {"entity"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getMaterialVariantTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "set_light_unit";
        tool.title = "Set Light Unit";
        tool.description =
            "Set the intensity unit of lights: engine, lux (directional), candela or lumen (point, spot). With "
            "preserve_brightness (default) the intensity is converted so the light stays as bright, through the scene's "
            "physical_light_ev100 (one undo step).";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"entities", array(entityRef(), "")},
            {"unit", stringEnum("", {"engine", "lux", "candela", "lumen"})},
            {"preserve_brightness", withDefault(boolean(""), true)}
        }, {"unit"});
        tool.handler = setLightUnitTool;
        registry.addTool(std::move(tool));
    }
}

}
