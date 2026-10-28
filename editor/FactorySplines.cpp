// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The C++ the exported game builds its spline components from (S18): Spline (type, closed, up
// axis, the knots), Spline Extrude, Spline Instantiate and Spline Animate. Values away from
// their defaults only; the generated geometry is not exported, SplineSystem builds it at load.

#include "Factory.h"

#include "Catalog.h"

#include <sstream>

using namespace jasperengine;

// the Catalog's scalar properties of a component away from their defaults, one assignment
// each (indexed and Custom properties are written by their callers)
void editor::Factory::formatCatalogScalars(std::ostringstream& code, const std::string& ind, EntityRegistry* scene, Entity entity,
                                           ComponentType type, const std::string& variable,
                                           const std::unordered_map<Entity, std::string>* entityVarNames){
    for (const auto& [name, property] : Catalog::findEntityProperties(scene, entity, type)){
        if (name.find('[') != std::string::npos || name == "enabled" || !property.ref || !property.def){
            continue;
        }
        std::string text;
        switch (property.type){
            case PropertyType::Bool:
                if (*(const bool*)property.ref != *(const bool*)property.def) text = formatBool(*(const bool*)property.ref);
                break;
            case PropertyType::Float:
                if (*(const float*)property.ref != *(const float*)property.def) text = formatFloat(*(const float*)property.ref);
                break;
            case PropertyType::UInt:
                if (*(const unsigned int*)property.ref != *(const unsigned int*)property.def) text = std::to_string(*(const unsigned int*)property.ref) + "u";
                break;
            case PropertyType::Vector2:
                if (*(const Vector2*)property.ref != *(const Vector2*)property.def) text = formatVector2(*(const Vector2*)property.ref);
                break;
            case PropertyType::Vector3:
                if (*(const Vector3*)property.ref != *(const Vector3*)property.def) text = formatVector3(*(const Vector3*)property.ref);
                break;
            case PropertyType::Entity:
                if (*(const Entity*)property.ref != *(const Entity*)property.def) text = formatEntity(*(const Entity*)property.ref, entityVarNames);
                break;
            case PropertyType::Enum:
                if (*(const int*)property.ref != *(const int*)property.def){
                    // the enum's type is the member's: decltype keeps the generator free of enum names
                    text = "static_cast<decltype(" + variable + "." + name + ")>(" + std::to_string(*(const int*)property.ref) + ")";
                }
                break;
            default:
                break;
        }
        if (!text.empty()){
            code << ind << variable << "." << name << " = " << text << ";\n";
        }
    }
}

std::string editor::Factory::formatSplinePool(const std::string& ind, const std::string& variable, const char* pool,
                                          const std::vector<SplinePoolEntry>& entries){
    std::ostringstream code;
    for (const SplinePoolEntry& entry : entries){
        // the game loads by path (an AssetRef assigned a path)
        code << ind << variable << "." << pool << ".push_back({AssetRef(" << formatString(entry.asset.path) << "), "
             << formatFloat(entry.weight) << "});\n";
    }
    return code.str();
}

std::string editor::Factory::createSplineComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName,
                                                  std::string entityName, bool assignExisting,
                                                  const std::unordered_map<Entity, std::string>* entityVarNames){
    if (!scene->findComponent<SplineComponent>(entity)) return "";
    const SplineComponent& spline = scene->getComponent<SplineComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "SplineComponent spline;\n";
    static const char* types[] = {"spline::SplineType::Linear", "spline::SplineType::CatmullRom", "spline::SplineType::CubicBezier"};
    static const char* modes[] = {"spline::TangentMode::Mirrored", "spline::TangentMode::Aligned", "spline::TangentMode::Broken"};
    code << ind << "spline.spline.setType(" << types[static_cast<int>(spline.spline.type())] << ");\n";
    if (spline.spline.closed()) code << ind << "spline.spline.setClosed(true);\n";
    if (spline.spline.upAxis() != Vector3(0.0f, 1.0f, 0.0f)) code << ind << "spline.spline.setUpAxis(" << formatVector3(spline.spline.upAxis()) << ");\n";
    if (spline.spline.knotCount() > 0){
        code << ind << "spline.spline.setKnots({\n";
        for (const spline::SplineKnot& k : spline.spline.knots()){
            code << ind << "    {" << formatVector3(k.position) << ", " << formatVector3(k.tangentIn) << ", " << formatVector3(k.tangentOut) << ", "
                 << formatFloat(k.radius) << ", " << formatFloat(k.rollDeg) << ", " << modes[static_cast<int>(k.mode)] << "},\n";
        }
        code << ind << "});\n";
    }
    // the terrain settings: the editor's bake reads them; a game keeps them for its own scripts
    for (const auto& [name, property] : Catalog::findEntityProperties(scene, entity, ComponentType::SplineComponent)){
        if (name.compare(0, 8, "terrain.") != 0 || !property.ref || !property.def) continue;
        std::string text;
        if (property.type == PropertyType::Bool && *(const bool*)property.ref != *(const bool*)property.def) text = formatBool(*(const bool*)property.ref);
        else if (property.type == PropertyType::Float && *(const float*)property.ref != *(const float*)property.def) text = formatFloat(*(const float*)property.ref);
        else if (property.type == PropertyType::UInt && *(const unsigned int*)property.ref != *(const unsigned int*)property.def) text = std::to_string(*(const unsigned int*)property.ref) + "u";
        else if (property.type == PropertyType::Entity && *(const Entity*)property.ref != *(const Entity*)property.def) text = formatEntity(*(const Entity*)property.ref, entityVarNames);
        else if (property.type == PropertyType::Enum && *(const int*)property.ref != *(const int*)property.def)
            text = "static_cast<decltype(spline." + name + ")>(" + std::to_string(*(const int*)property.ref) + ")";
        if (!text.empty()) code << ind << "spline." << name << " = " << text << ";\n";
    }
    if (!spline.enabled) code << ind << "spline.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "SplineComponent", "spline", assignExisting);
    return code.str();
}

std::string editor::Factory::createSplineExtrudeComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName,
                                                         std::string entityName, bool assignExisting){
    if (!scene->findComponent<SplineExtrudeComponent>(entity)) return "";
    const SplineExtrudeComponent& extrude = scene->getComponent<SplineExtrudeComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "SplineExtrudeComponent splineExtrude;\n";
    formatCatalogScalars(code, ind, scene, entity, ComponentType::SplineExtrudeComponent, "splineExtrude", nullptr);
    for (const SplineProfilePoint& p : extrude.customProfile){
        code << ind << "splineExtrude.customProfile.push_back({" << formatVector2(p.position) << ", " << formatBool(p.hard) << ", "
             << formatBool(p.grounded) << "});\n";
    }
    if (!extrude.enabled) code << ind << "splineExtrude.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "SplineExtrudeComponent", "splineExtrude", assignExisting);
    return code.str();
}

std::string editor::Factory::createSplineInstantiateComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName,
                                                             std::string entityName, bool assignExisting){
    if (!scene->findComponent<SplineInstantiateComponent>(entity)) return "";
    const SplineInstantiateComponent& instantiate = scene->getComponent<SplineInstantiateComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "SplineInstantiateComponent splineInstantiate;\n";
    formatCatalogScalars(code, ind, scene, entity, ComponentType::SplineInstantiateComponent, "splineInstantiate", nullptr);
    code << formatSplinePool(ind, "splineInstantiate", "items", instantiate.items);
    code << formatSplinePool(ind, "splineInstantiate", "posts", instantiate.posts);
    code << formatSplinePool(ind, "splineInstantiate", "spans", instantiate.spans);
    code << formatSplinePool(ind, "splineInstantiate", "gates", instantiate.gates);
    code << formatSplinePool(ind, "splineInstantiate", "crests", instantiate.crests);
    code << formatSplinePool(ind, "splineInstantiate", "caps", instantiate.caps);
    for (const SplineSpanOverride& o : instantiate.spanOverrides){
        code << ind << "splineInstantiate.spanOverrides.push_back({" << o.knot << "u, " << o.span << "u, "
             << (o.kind == SplineSpanOverrideKind::Gate ? "SplineSpanOverrideKind::Gate" : "SplineSpanOverrideKind::Piece") << ", " << o.piece << "u});\n";
    }
    if (!instantiate.enabled) code << ind << "splineInstantiate.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "SplineInstantiateComponent", "splineInstantiate", assignExisting);
    return code.str();
}

std::string editor::Factory::createSplineAnimateComponent(int indentSpaces, EntityRegistry* scene, Entity entity, std::string sceneName,
                                                         std::string entityName, bool assignExisting,
                                                         const std::unordered_map<Entity, std::string>* entityVarNames){
    if (!scene->findComponent<SplineAnimateComponent>(entity)) return "";
    const SplineAnimateComponent& animate = scene->getComponent<SplineAnimateComponent>(entity);
    std::ostringstream code;
    const std::string ind = indentation(indentSpaces);
    code << ind << "SplineAnimateComponent splineAnimate;\n";
    formatCatalogScalars(code, ind, scene, entity, ComponentType::SplineAnimateComponent, "splineAnimate", entityVarNames);
    if (!animate.enabled) code << ind << "splineAnimate.enabled = false;\n";
    addComponentCode(code, ind, sceneName, entityName, entity, "SplineAnimateComponent", "splineAnimate", assignExisting);
    return code.str();
}
