// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UpstreamParticles.h"

#include "Stream.h"
#include "component/PointsComponent.h"
#include "component/SpriteComponent.h"
#include "stream/CurveCodec.h"
#include "util/Color.h"
#include "util/YamlNumbers.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::particles;
namespace up = jasperengine::editor::upstreamparticles;

namespace {

    constexpr float kRadToDeg = 57.29577951308232f;
    constexpr float kDegToRad = 0.017453292519943295f;

    // origin/main's enum names, by value (particles/ParticleFields.cpp there)
    const std::vector<std::string> kSimulationSpace = {"Local", "World"};
    const std::vector<std::string> kScalingMode = {"Hierarchy", "Local", "Shape"};
    const std::vector<std::string> kStopAction = {"None", "Disable", "Destroy", "Callback"};
    const std::vector<std::string> kEmitterVelocity = {"Transform", "Rigidbody"};
    const std::vector<std::string> kGravitySource = {"Physics3D", "Physics2D"};
    const std::vector<std::string> kShapeType = {"Sphere", "Hemisphere", "Cone", "ConeVolume", "Box", "BoxShell", "BoxEdge",
                                                 "Circle", "Edge", "Rectangle", "Donut"};
    const std::vector<std::string> kMultiMode = {"Random", "Loop", "PingPong", "BurstSpread"};
    const std::vector<std::string> kInheritMode = {"Initial", "Current"};
    const std::vector<std::string> kAnimationMode = {"Grid", "Sprites"};
    const std::vector<std::string> kAnimationType = {"WholeSheet", "SingleRow"};
    const std::vector<std::string> kRowMode = {"Custom", "Random"};
    const std::vector<std::string> kTimeMode = {"Lifetime", "Speed", "FPS"};
    const std::vector<std::string> kRenderMode = {"Billboard", "Stretch", "HorizontalBillboard", "VerticalBillboard", "Mesh"};
    const std::vector<std::string> kSortMode = {"None", "Distance", "OldestInFront", "YoungestInFront"};
    const std::vector<std::string> kRenderSpace = {"View", "World", "Local", "Facing", "Velocity"};

    struct Reader {
        std::vector<std::string>& notes;

        void note(const std::string& line){ notes.push_back(line); }

        float number(const YAML::Node& node, float fallback){
            if (!node || !node.IsScalar())
                return fallback;
            return editor::Stream::decodeFinite(node, fallback);
        }

        bool flag(const YAML::Node& node, bool fallback){
            if (!node || !node.IsScalar())
                return fallback;
            try {
                return editor::YamlNumbers::as<bool>(node);
            } catch (const YAML::Exception&) {
                return fallback;
            }
        }

        int integer(const YAML::Node& node, int fallback){
            if (!node || !node.IsScalar())
                return fallback;
            try {
                return editor::YamlNumbers::as<int>(node);
            } catch (const YAML::Exception&) {
                return (int)std::lround(number(node, (float)fallback));
            }
        }

        Vector3 vector3(const YAML::Node& node, const Vector3& fallback){
            return node ? editor::Stream::decodeFiniteVector3(node, fallback) : fallback;
        }

        Vector2 vector2(const YAML::Node& node, const Vector2& fallback){
            if (!node || !node.IsSequence() || node.size() < 2)
                return fallback;
            return Vector2(number(node[0], fallback.x), number(node[1], fallback.y));
        }

        // an enum by its name (or its value), `fallback` when neither
        int enumeration(const YAML::Node& node, const std::vector<std::string>& names, int fallback, const std::string& path){
            if (!node || !node.IsScalar())
                return fallback;
            const std::string text = node.Scalar();
            for (size_t i = 0; i < names.size(); i++){
                if (names[i] == text)
                    return (int)i;
            }
            int value = 0;
            if (YAML::convert<int>::decode(node, value) && value >= 0 && (size_t)value < names.size())
                return value;
            note(path + ": '" + text + "' is not one of its values: the default is kept");
            return fallback;
        }

        Curve curve(const YAML::Node& node, const std::string& path){
            Curve c;
            std::string reason;
            if (!editor::curvecodec::curveFromYaml(node, c, reason)){
                note(path + ": " + reason + " (a constant 1 curve is used)");
                return Curve::constant(1.0f);
            }
            return c;
        }

        Gradient gradient(const YAML::Node& node, const std::string& path){
            Gradient g;
            std::string reason;
            if (!editor::curvecodec::gradientFromYaml(node, g, reason)){
                note(path + ": " + reason + " (white is used)");
                return Gradient();
            }
            return g;
        }

        // a colour (hex: sRGB; [r, g, b(, a)]: linear) as sRGB and alpha
        bool color(const YAML::Node& node, Vector4& srgb){
            if (!node || !(node.IsScalar() || node.IsSequence()))
                return false;
            Gradient g;
            std::string reason;
            if (!editor::curvecodec::gradientFromYaml(node, g, reason) || g.colorKeys.empty())
                return false;
            const Vector3& c = g.colorKeys.front().color;
            srgb = Vector4(c.x, c.y, c.z, g.alphaKeys.empty() ? 1.0f : g.alphaKeys.front().alpha);
            return true;
        }

        Vector4 colorOr(const YAML::Node& node, const Vector4& fallback, const std::string& path){
            Vector4 c;
            if (color(node, c))
                return c;
            if (node)
                note(path + ": not a colour (white is used)");
            return fallback;
        }

        // A MinMaxCurve: a number, {mode: two_constants, min, max}, {mode: curve, multiplier,
        // curve} or {mode: two_curves, multiplier, min, max}; `scale` converts its unit (rad/s
        // to deg/s). An absent value is `fallback` (in S20's unit already).
        MinMaxCurve minMax(const YAML::Node& node, const MinMaxCurve& fallback, const std::string& path, float scale = 1.0f){
            if (!node || node.IsNull())
                return fallback;
            if (node.IsScalar())
                return MinMaxCurve(number(node, fallback.constant / (scale != 0.0f ? scale : 1.0f)) * scale);
            if (!node.IsMap()){
                note(path + ": not a value (the default is kept)");
                return fallback;
            }
            const std::string mode = node["mode"] && node["mode"].IsScalar() ? node["mode"].Scalar() : std::string("constant");
            if (mode == "two_constants")
                return MinMaxCurve::twoConstants(number(node["min"], 0.0f) * scale, number(node["max"], 0.0f) * scale);
            if (mode == "curve")
                return MinMaxCurve::fromCurve(curve(node["curve"], path + ".curve"), number(node["multiplier"], 1.0f) * scale);
            if (mode == "two_curves"){
                MinMaxCurve value = MinMaxCurve::fromCurve(curve(node["min"], path + ".min"), number(node["multiplier"], 1.0f) * scale);
                value.mode = CurveMode::TwoCurves;
                value.curveMax = curve(node["max"], path + ".max");
                return value;
            }
            if (mode != "constant")
                note(path + ": mode '" + mode + "' is not a curve mode (a constant is read)");
            const YAML::Node constant = node["value"] ? node["value"] : node["constant"];
            return MinMaxCurve(number(constant, fallback.constant / (scale != 0.0f ? scale : 1.0f)) * scale);
        }

        // A MinMaxGradient: a colour, {mode: two_colors, min, max}, {mode: gradient |
        // random_color, gradient} or {mode: two_gradients, min, max}
        MinMaxGradient minMaxGradient(const YAML::Node& node, const MinMaxGradient& fallback, const std::string& path){
            if (!node || node.IsNull())
                return fallback;
            Vector4 single;
            if ((node.IsScalar() || node.IsSequence()) && color(node, single))
                return MinMaxGradient(single);
            if (!node.IsMap()){
                note(path + ": not a colour (the default is kept)");
                return fallback;
            }
            const std::string mode = node["mode"] && node["mode"].IsScalar() ? node["mode"].Scalar() : std::string("color");
            const Vector4 white(1.0f, 1.0f, 1.0f, 1.0f);
            MinMaxGradient value;
            if (mode == "two_colors"){
                value.mode = GradientValueMode::TwoColors;
                value.color = colorOr(node["min"], white, path + ".min");
                value.colorMax = colorOr(node["max"], white, path + ".max");
            }else if (mode == "gradient" || mode == "random_color"){
                value.mode = mode == "gradient" ? GradientValueMode::Gradient : GradientValueMode::RandomColor;
                value.gradient = gradient(node["gradient"], path + ".gradient");
            }else if (mode == "two_gradients"){
                value.mode = GradientValueMode::TwoGradients;
                value.gradient = gradient(node["min"], path + ".min");
                value.gradientMax = gradient(node["max"], path + ".max");
            }else{
                if (mode != "color")
                    note(path + ": mode '" + mode + "' is not a gradient mode (a colour is read)");
                value = MinMaxGradient(colorOr(node["color"], fallback.color, path + ".color"));
            }
            return value;
        }
    };

    // a module of the system: an empty map when the file has none (a missing key of a const node
    // is an invalid node, which throws when it is looked into)
    YAML::Node moduleOf(const YAML::Node& system, const char* name){
        const YAML::Node module = system[name];
        if (module && module.IsMap()) return module;
        return YAML::Node(YAML::NodeType::Map);
    }

    ValueSpace space(int v){ return v == 1 ? ValueSpace::World : ValueSpace::Local; }

    ShapeMultiMode multiMode(int v){
        switch (v){
            case 1: return ShapeMultiMode::Loop;
            case 2: return ShapeMultiMode::PingPong;
            case 3: return ShapeMultiMode::BurstSpread;
            default: return ShapeMultiMode::Random;
        }
    }

    // 3x3 column-major products of the axis rotations
    void mul3(const float a[9], const float b[9], float out[9]){
        for (int c = 0; c < 3; c++){
            for (int r = 0; r < 3; r++){
                out[c * 3 + r] = a[0 * 3 + r] * b[c * 3 + 0] + a[1 * 3 + r] * b[c * 3 + 1] + a[2 * 3 + r] * b[c * 3 + 2];
            }
        }
    }

    void axis(int which, float degrees, float m[9]){
        const float a = degrees * kDegToRad, c = std::cos(a), s = std::sin(a);
        for (int i = 0; i < 9; i++) m[i] = (i % 4 == 0) ? 1.0f : 0.0f;
        if (which == 0){        // X: rows y, z
            m[4] = c; m[5] = s; m[7] = -s; m[8] = c;
        }else if (which == 1){  // Y: rows x, z
            m[0] = c; m[2] = -s; m[6] = s; m[8] = c;
        }else{                  // Z: rows x, y
            m[0] = c; m[1] = s; m[3] = -s; m[4] = c;
        }
    }

    float tidy(float degrees){
        // the decomposition's float noise (89.99999 for 90) rounded away
        const float rounded = std::round(degrees * 10000.0f) / 10000.0f;
        return rounded == 0.0f ? 0.0f : rounded;
    }

    MeshSource primitiveOfName(const std::string& mesh, bool& known){
        MeshSource source;
        source.type = MeshSourceType::Primitive;
        known = true;
        if (mesh == "quad" || mesh.empty()) source.primitive = PrimitiveMesh::Quad;
        else if (mesh == "cube") source.primitive = PrimitiveMesh::Cube;
        else if (mesh == "sphere") source.primitive = PrimitiveMesh::Sphere;
        else if (mesh == "cylinder") source.primitive = PrimitiveMesh::Cylinder;
        else if (mesh == "capsule") source.primitive = PrimitiveMesh::Capsule;
        else if (mesh == "plane") source.primitive = PrimitiveMesh::Plane;
        else {
            source.type = MeshSourceType::Model;
            source.model = mesh;
            known = false;
        }
        return source;
    }

    bool hasFeature(const std::vector<std::string>& features, const std::string& name){
        return std::find(features.begin(), features.end(), name) != features.end();
    }

    void setProperty(RendererModule& r, const std::string& name, const Vector4& value, int components){
        for (particles::ShaderProperty& p : r.shaderProperties){
            if (p.name == name){
                p.value = value;
                p.components = components;
                return;
            }
        }
        particles::ShaderProperty p;
        p.name = name;
        p.value = value;
        p.components = components;
        r.shaderProperties.push_back(p);
    }

    void addFeature(RendererModule& r, const std::string& name){
        if (!hasFeature(r.shaderFeatures, name))
            r.shaderFeatures.push_back(name);
    }

}

bool up::isVersion2(const YAML::Node& system){
    if (!system || !system.IsMap() || !system["version"] || !system["version"].IsScalar())
        return false;
    try {
        return editor::YamlNumbers::as<int>(system["version"]) >= 2 && !system["inlineEffect"] && !system["effect"];
    } catch (const YAML::Exception&) {
        return false;
    }
}

up::Target up::targetOf(const YAML::Node& components){
    Target target;
    if (!components || !components.IsMap())
        return target;
    if (const YAML::Node mesh = components["mesh"]; mesh && mesh.IsMap()){
        target.kind = Target::Kind::Mesh;
        const YAML::Node submeshes = mesh["submeshes"];
        if (submeshes && submeshes.IsSequence() && submeshes.size() > 0 && submeshes[0]["material"])
            target.material = editor::Stream::decodeMaterial(submeshes[0]["material"]);
        std::vector<std::string> ignored;
        Reader read{ignored};
        target.meshEnabled = read.flag(mesh["enabled"], true);
        target.castShadows = read.flag(mesh["castShadows"], true);
        target.receiveShadows = read.flag(mesh["receiveShadows"], true);
        if (const YAML::Node sprite = components["sprite"]){
            const SpriteComponent decoded = editor::Stream::decodeSpriteComponent(sprite);
            for (unsigned int i = 0; i < decoded.numFramesRect; i++){
                const Rect& r = decoded.framesRect[i].rect;
                target.frames.push_back(SpriteRect{r.getX(), r.getY(), r.getWidth(), r.getHeight()});
            }
        }
    }else if (const YAML::Node pointsNode = components["points"]; pointsNode && pointsNode.IsMap()){
        target.kind = Target::Kind::Points;
        const PointsComponent points = editor::Stream::decodePointsComponent(pointsNode);
        target.texture = points.texture.getPath();
        if (!points.texture.getPathGuid().isNull())
            target.textureGuid = points.texture.getPathGuid().str();
        for (unsigned int i = 0; i < points.numFramesRect; i++){
            const Rect& r = points.framesRect[i].rect;
            target.frames.push_back(SpriteRect{r.getX(), r.getY(), r.getWidth(), r.getHeight()});
        }
        target.pointsShader = points.customShader.path;
        target.pointsFeatures = points.shaderFeatures;
        for (const auto& u : points.shaderUniforms){
            particles::ShaderProperty p;
            p.name = u.first;
            p.value = u.second;
            p.components = 4;
            target.pointsProperties.push_back(p);
        }
        for (const auto& t : points.shaderTextures){
            particles::ShaderTexture texture;
            texture.name = t.first;
            texture.path = t.second.getPath();
            if (!t.second.getPathGuid().isNull())
                texture.guid = t.second.getPathGuid().str();
            target.pointsTextures.push_back(texture);
        }
        switch (points.blendMode){
            case MaterialBlendMode::AUTO: target.pointsBlend = -1; break;
            case MaterialBlendMode::NONE: target.pointsOpaque = true; break;
            case MaterialBlendMode::PREMULTIPLIED: target.pointsBlend = (int)ParticleBlend::Premultiplied; break;
            case MaterialBlendMode::ADDITIVE: target.pointsBlend = (int)ParticleBlend::Additive; break;
            case MaterialBlendMode::MULTIPLY: target.pointsBlend = (int)ParticleBlend::Multiply; break;
            default: target.pointsBlend = (int)ParticleBlend::Alpha; break;
        }
        target.pointsDepthWrite = points.depthWrite == MaterialDepthWrite::AUTO ? -1 : (points.depthWrite == MaterialDepthWrite::ENABLED ? 1 : 0);
    }
    return target;
}

Vector3 up::shapeRotation(const Vector3& e){
    if (e.x == 0.0f && e.y == 0.0f && e.z == 0.0f)
        return e;
    // origin/main: q = qz * qy * qx (X first)
    float rx[9], ry[9], rz[9], zy[9], m[9];
    axis(0, e.x, rx);
    axis(1, e.y, ry);
    axis(2, e.z, rz);
    mul3(rz, ry, zy);
    mul3(zy, rx, m);
    // S20: M = Ry(a) Rx(b) Rz(c); m[7] (row 1, column 2) = -sin(b)
    const float sb = std::clamp(-m[7], -1.0f, 1.0f);
    const float b = std::asin(sb);
    float a, c;
    if (std::fabs(sb) < 0.99999f){
        a = std::atan2(m[6], m[8]);
        c = std::atan2(m[1], m[4]);
    }else{
        // gimbal lock: the roll folded into the yaw
        c = 0.0f;
        a = std::atan2(-m[2], m[0]);
    }
    return Vector3(tidy(b * kRadToDeg), tidy(a * kRadToDeg), tidy(c * kRadToDeg));
}

bool up::applyRenderMode(int renderMode, int alignment, bool localSimulation, RendererModule& r){
    switch (renderMode){
        case 1: r.renderMode = RenderMode::StretchedBillboard; break;
        case 2: r.renderMode = RenderMode::HorizontalBillboard; break;
        case 3: r.renderMode = RenderMode::VerticalBillboard; break;
        case 4: r.renderMode = RenderMode::Mesh; break;
        default:
            switch (alignment){
                // World turns a Local system's particles with its emitter there
                case 1: r.renderMode = localSimulation ? RenderMode::LocalOriented : RenderMode::WorldOriented; break;
                case 2: r.renderMode = RenderMode::LocalOriented; break;
                case 3: r.renderMode = RenderMode::FacingCameraPosition; break;
                case 4: r.renderMode = RenderMode::VelocityAligned; break;
                default: r.renderMode = RenderMode::Billboard; break;
            }
            return true;
    }
    if (r.renderMode == RenderMode::Mesh){
        switch (alignment){
            // World there: the simulation's axes (a Local system's meshes turn with its emitter)
            case 1: r.meshAlignment = MeshAlignment::Simulation; break;
            case 2: r.meshAlignment = MeshAlignment::Local; break;
            case 3: r.meshAlignment = MeshAlignment::Facing; break;
            case 4: r.meshAlignment = MeshAlignment::Velocity; break;
            default: r.meshAlignment = MeshAlignment::View; break;
        }
        return true;
    }
    return alignment == 0;
}

MeshSource up::meshSourceOf(const std::string& name, bool& primitive){
    return primitiveOfName(name, primitive);
}

void up::applyMaterial(const Material& material, RendererModule& r, std::vector<std::string>& notes){
    const std::string shader = material.shader.path;
    if (shader != "library:unlit")
        notes.push_back("its material's shader " + (shader.empty() ? std::string("(the built-in Lit)") : "'" + shader + "'") +
                        " is not a particles shader: library:particles draws them with its base map and colour");
    // the base map: the library shader's u_baseTexture, else the material's base colour texture
    bool textured = false;
    for (const auto& t : material.shaderTextures){
        if (t.first == "u_baseTexture" && !t.second.getPath().empty()){
            r.texture = t.second.getPath();
            if (!t.second.getPathGuid().isNull())
                r.textureGuid = t.second.getPathGuid().str();
            textured = true;
        }else if (t.first != "u_baseTexture"){
            notes.push_back("its material's texture " + t.first + " has no place in a particle Renderer: left out");
        }
    }
    if (!textured && !material.baseColorTexture.getPath().empty()){
        r.texture = material.baseColorTexture.getPath();
        if (!material.baseColorTexture.getPathGuid().isNull())
            r.textureGuid = material.baseColorTexture.getPathGuid().str();
        textured = true;
    }
    // without a base map: the built-in soft particle (PARTICLE_TEXTURE), else a white quad
    if (!textured)
        r.untexturedShape = hasFeature(material.shaderFeatures, "PARTICLE_TEXTURE") ? UntexturedShape::Disc : UntexturedShape::Square;

    for (const auto& param : material.shaderParams){
        const std::string& name = param.first;
        const Vector4& v = param.second;
        if (name == "baseColor"){
            // linear RGBA, library:particles' tint (the same property type)
            if (v != Vector4(1.0f, 1.0f, 1.0f, 1.0f))
                setProperty(r, "tint", v, 4);
        }else if (name == "tiling"){
            r.textureTiling = v;
        }else if (name == "softness" || name == "nearFadeStart" || name == "nearFadeEnd"){
            setProperty(r, name, v, 1);
        }else if (name != "alphaCutoff" && name.rfind("stochastic", 0) != 0){
            notes.push_back("its material's parameter " + name + " has no place in a particle Renderer: left out");
        }
    }
    // the built-in Lit's colour (linear) where the library shader's is not
    if (shader != "library:unlit" && material.baseColorFactor != Vector4(1.0f, 1.0f, 1.0f, 1.0f))
        setProperty(r, "tint", material.baseColorFactor, 4);
    for (const std::string& feature : material.shaderFeatures){
        if (feature == "SOFT" || feature == "NEAR_FADE"){
            addFeature(r, feature);
        }else if (feature == "ALPHA_CLIP" || feature == "STOCHASTIC"){
            notes.push_back("its material's " + feature + " is not a particles shader feature: left out");
        }
    }

    switch (material.blendMode){
        case MaterialBlendMode::ADDITIVE: r.blend = ParticleBlend::Additive; break;
        case MaterialBlendMode::MULTIPLY: r.blend = ParticleBlend::Multiply; break;
        case MaterialBlendMode::PREMULTIPLIED: r.blend = ParticleBlend::Premultiplied; break;
        case MaterialBlendMode::NONE:
            r.blend = ParticleBlend::Alpha;
            r.depthWrite = true;
            notes.push_back("an opaque material draws alpha blended, with depth writes");
            break;
        default: r.blend = ParticleBlend::Alpha; break;
    }
    if (material.depthWrite == MaterialDepthWrite::ENABLED)
        r.depthWrite = true;
}

up::Result up::convert(const YAML::Node& node, const Target& target){
    Result result;
    Reader read{result.notes};
    ParticleEffectDocument& d = result.effect;

    d.useUnscaledTime = read.flag(node["useUnscaledTime"], false);

    // ---------------------------------------------------------------- main
    const YAML::Node main = moduleOf(node, "main");
    MainModule& m = d.main;
    d.duration = std::max(0.05f, read.number(main["duration"], 5.0f));
    d.looping = read.flag(main["looping"], true);
    d.prewarm = read.flag(main["prewarm"], false);
    d.startDelay = read.minMax(main["startDelay"], MinMaxCurve(0.0f), "main.startDelay");
    if (d.startDelay.usesCurve()){
        // a delay is picked once per play: its curve's start
        const float at0 = d.startDelay.curve.evaluate(0.0f) * d.startDelay.multiplier;
        d.startDelay = MinMaxCurve(at0);
        read.note("main.startDelay: a curve delay starts at its value at 0");
    }
    m.startLifetime = read.minMax(main["startLifetime"], MinMaxCurve(5.0f), "main.startLifetime");
    m.startSpeed = read.minMax(main["startSpeed"], MinMaxCurve(5.0f), "main.startSpeed");
    m.startSize3D = read.flag(main["startSize3D"], false);
    m.startSize = read.minMax(main["startSize"], MinMaxCurve(1.0f), "main.startSize");
    m.startSizeY = read.minMax(main["startSizeY"], MinMaxCurve(1.0f), "main.startSizeY");
    m.startSizeZ = read.minMax(main["startSizeZ"], MinMaxCurve(1.0f), "main.startSizeZ");
    m.startRotation3D = read.flag(main["startRotation3D"], false);
    m.startRotation = read.minMax(main["startRotation"], MinMaxCurve(0.0f), "main.startRotation", kRadToDeg);
    m.startRotationX = read.minMax(main["startRotationX"], MinMaxCurve(0.0f), "main.startRotationX", kRadToDeg);
    m.startRotationY = read.minMax(main["startRotationY"], MinMaxCurve(0.0f), "main.startRotationY", kRadToDeg);
    m.flipRotation = std::clamp(read.number(main["flipRotation"], 0.0f), 0.0f, 1.0f);
    m.startColor = read.minMaxGradient(main["startColor"], MinMaxGradient(), "main.startColor");
    m.gravityModifier = read.minMax(main["gravityModifier"], MinMaxCurve(0.0f), "main.gravityModifier");
    // over the emitter's time there (Unity's)
    if (m.gravityModifier.usesCurve())
        m.gravityModifier.driver = CurveDriver::EmitterTime;
    m.gravitySource = read.enumeration(main["gravitySource"], kGravitySource, 0, "main.gravitySource") == 1 ? GravitySource::Physics2D : GravitySource::Physics3D;
    d.simulationSpace = read.enumeration(main["simulationSpace"], kSimulationSpace, 0, "main.simulationSpace") == 1 ? SimulationSpace::World : SimulationSpace::Local;
    d.simulationSpeed = std::max(0.0f, read.number(main["simulationSpeed"], 1.0f));
    d.scalingMode = (ScalingMode)read.enumeration(main["scalingMode"], kScalingMode, 1, "main.scalingMode");
    result.playOnAwake = read.flag(main["playOnAwake"], true);
    d.maxParticles = (uint32_t)std::clamp(read.integer(main["maxParticles"], 1000), 0, (int)kMaxParticlesPerEmitter);
    d.emitterVelocityMode = read.enumeration(main["emitterVelocityMode"], kEmitterVelocity, 0, "main.emitterVelocityMode") == 1 ?
        EmitterVelocityMode::Rigidbody : EmitterVelocityMode::Transform;
    d.stopAction = (StopAction)read.enumeration(main["stopAction"], kStopAction, 0, "main.stopAction");
    if (!read.flag(main["autoRandomSeed"], true)){
        const int seed = read.integer(main["randomSeed"], 0);
        // 0 is "a new seed every play" here: a fixed seed 0 plays as seed 1
        result.seed = seed == 0 ? 1u : (uint32_t)seed;
    }

    // ---------------------------------------------------------------- emission
    const YAML::Node emission = moduleOf(node, "emission");
    EmissionModule& e = d.emission;
    e.enabled = read.flag(emission["enabled"], true);
    e.rateOverTime = read.minMax(emission["rateOverTime"], MinMaxCurve(10.0f), "emission.rateOverTime");
    e.rateOverDistance = read.minMax(emission["rateOverDistance"], MinMaxCurve(0.0f), "emission.rateOverDistance");
    if (const YAML::Node bursts = emission["bursts"]; bursts && bursts.IsSequence()){
        for (size_t i = 0; i < bursts.size(); i++){
            const YAML::Node b = bursts[i];
            if (!b.IsMap())
                continue;
            if (e.bursts.size() >= kMaxBursts){
                read.note("emission.bursts: more than " + std::to_string(kMaxBursts) + " bursts: the first " + std::to_string(kMaxBursts) + " are kept");
                break;
            }
            const std::string at = "emission.bursts[" + std::to_string(i) + "]";
            Burst burst;
            burst.time = std::max(0.0f, read.number(b["time"], 0.0f));
            burst.count = read.minMax(b["count"], MinMaxCurve(30.0f), at + ".count");
            burst.cycles = std::max(0, read.integer(b["cycles"], 1));
            burst.interval = std::max(0.0001f, read.number(b["interval"], 0.01f));
            burst.probability = std::clamp(read.number(b["probability"], 1.0f), 0.0f, 1.0f);
            e.bursts.push_back(burst);
        }
    }

    // ---------------------------------------------------------------- shape
    const YAML::Node shape = moduleOf(node, "shape");
    ShapeModule& s = d.shape;
    s.enabled = read.flag(shape["enabled"], true);
    switch (read.enumeration(shape["shapeType"], kShapeType, 2, "shape.shapeType")){
        case 0: s.shape = ShapeType::Sphere; s.emitFrom = ShapeEmitFrom::Volume; break;
        case 1: s.shape = ShapeType::Hemisphere; s.emitFrom = ShapeEmitFrom::Volume; break;
        case 3: s.shape = ShapeType::Cone; s.emitFrom = ShapeEmitFrom::Volume; break;
        case 4: s.shape = ShapeType::Box; s.emitFrom = ShapeEmitFrom::Volume; break;
        case 5: s.shape = ShapeType::Box; s.emitFrom = ShapeEmitFrom::Shell; break;
        case 6: s.shape = ShapeType::Box; s.emitFrom = ShapeEmitFrom::Edge; break;
        case 7: s.shape = ShapeType::Circle; s.emitFrom = ShapeEmitFrom::Volume; break;
        case 8: s.shape = ShapeType::Edge; s.emitFrom = ShapeEmitFrom::Volume; break;
        case 9: s.shape = ShapeType::Rectangle; s.emitFrom = ShapeEmitFrom::Volume; break;
        case 10: s.shape = ShapeType::Donut; s.emitFrom = ShapeEmitFrom::Volume; break;
        default: s.shape = ShapeType::Cone; s.emitFrom = ShapeEmitFrom::Base; break;
    }
    s.radius = read.number(shape["radius"], 1.0f);
    s.radiusThickness = std::clamp(read.number(shape["radiusThickness"], 1.0f), 0.0f, 1.0f);
    s.radiusMode = multiMode(read.enumeration(shape["radiusMode"], kMultiMode, 0, "shape.radiusMode"));
    s.radiusSpread = std::clamp(read.number(shape["radiusSpread"], 0.0f), 0.0f, 1.0f);
    s.radiusSpeed = read.minMax(shape["radiusSpeed"], MinMaxCurve(1.0f), "shape.radiusSpeed");
    s.angle = std::clamp(read.number(shape["angle"], 25.0f), 0.0f, 90.0f);
    s.length = std::max(0.0f, read.number(shape["length"], 5.0f));
    s.arc = std::clamp(read.number(shape["arc"], 360.0f), 0.0f, 360.0f);
    s.arcMode = multiMode(read.enumeration(shape["arcMode"], kMultiMode, 0, "shape.arcMode"));
    s.arcSpread = std::clamp(read.number(shape["arcSpread"], 0.0f), 0.0f, 1.0f);
    s.arcSpeed = read.minMax(shape["arcSpeed"], MinMaxCurve(1.0f), "shape.arcSpeed");
    s.boxThickness = read.vector3(shape["boxThickness"], Vector3(0.0f, 0.0f, 0.0f));
    s.donutRadius = read.number(shape["donutRadius"], 0.2f);
    s.position = read.vector3(shape["position"], Vector3(0.0f, 0.0f, 0.0f));
    s.rotation = shapeRotation(read.vector3(shape["rotation"], Vector3(0.0f, 0.0f, 0.0f)));
    s.scale = read.vector3(shape["scale"], Vector3(1.0f, 1.0f, 1.0f));
    s.alignToDirection = read.flag(shape["alignToDirection"], false);
    s.randomizeDirection = read.number(shape["randomDirectionAmount"], 0.0f);
    s.spherizeDirection = read.number(shape["sphericalDirectionAmount"], 0.0f);
    s.randomizePosition = read.number(shape["randomPositionAmount"], 0.0f);

    // ---------------------------------------------------------------- velocity over lifetime
    if (const YAML::Node vol = node["velocityOverLifetime"]){
        VelocityOverLifetimeModule& v = d.velocityOverLifetime;
        v.present = true;
        v.enabled = read.flag(vol["enabled"], false);
        v.x = read.minMax(vol["x"], MinMaxCurve(0.0f), "velocityOverLifetime.x");
        v.y = read.minMax(vol["y"], MinMaxCurve(0.0f), "velocityOverLifetime.y");
        v.z = read.minMax(vol["z"], MinMaxCurve(0.0f), "velocityOverLifetime.z");
        v.orbitalX = read.minMax(vol["orbitalX"], MinMaxCurve(0.0f), "velocityOverLifetime.orbitalX", kRadToDeg);
        v.orbitalY = read.minMax(vol["orbitalY"], MinMaxCurve(0.0f), "velocityOverLifetime.orbitalY", kRadToDeg);
        v.orbitalZ = read.minMax(vol["orbitalZ"], MinMaxCurve(0.0f), "velocityOverLifetime.orbitalZ", kRadToDeg);
        v.offsetX = read.minMax(vol["orbitalOffsetX"], MinMaxCurve(0.0f), "velocityOverLifetime.orbitalOffsetX");
        v.offsetY = read.minMax(vol["orbitalOffsetY"], MinMaxCurve(0.0f), "velocityOverLifetime.orbitalOffsetY");
        v.offsetZ = read.minMax(vol["orbitalOffsetZ"], MinMaxCurve(0.0f), "velocityOverLifetime.orbitalOffsetZ");
        v.radial = read.minMax(vol["radial"], MinMaxCurve(0.0f), "velocityOverLifetime.radial");
        v.speedModifier = read.minMax(vol["speedModifier"], MinMaxCurve(1.0f), "velocityOverLifetime.speedModifier");
        v.space = space(read.enumeration(vol["space"], kSimulationSpace, 0, "velocityOverLifetime.space"));
    }

    // ---------------------------------------------------------------- limit velocity
    if (const YAML::Node lim = node["limitVelocityOverLifetime"]){
        LimitVelocityModule& l = d.limitVelocity;
        l.present = true;
        l.enabled = read.flag(lim["enabled"], false);
        l.separateAxes = read.flag(lim["separateAxes"], false);
        l.limit = read.minMax(l.separateAxes ? lim["limitX"] : lim["limit"], MinMaxCurve(1.0f), "limitVelocityOverLifetime.limit");
        l.limitY = read.minMax(lim["limitY"], MinMaxCurve(1.0f), "limitVelocityOverLifetime.limitY");
        l.limitZ = read.minMax(lim["limitZ"], MinMaxCurve(1.0f), "limitVelocityOverLifetime.limitZ");
        if (l.separateAxes && lim["limit"])
            read.note("limitVelocityOverLifetime.limit: with separate axes the X limit is the limit (its speed limit left out)");
        else if (!l.separateAxes && lim["limitX"])
            read.note("limitVelocityOverLifetime.limitX: without separate axes the speed limit is the limit (its X limit left out)");
        l.space = space(read.enumeration(lim["space"], kSimulationSpace, 0, "limitVelocityOverLifetime.space"));
        l.dampen = std::clamp(read.number(lim["dampen"], 1.0f), 0.0f, 1.0f);
        l.drag = read.minMax(lim["drag"], MinMaxCurve(0.0f), "limitVelocityOverLifetime.drag");
        l.multiplyBySize = read.flag(lim["multiplyDragByParticleSize"], true);
        // origin/main's default is on
        l.multiplyByVelocity = read.flag(lim["multiplyDragByParticleVelocity"], true);
    }

    // ---------------------------------------------------------------- inherit velocity (Main here)
    if (const YAML::Node inherit = node["inheritVelocity"]){
        if (read.flag(inherit["enabled"], false)){
            m.inheritVelocity = read.minMax(inherit["curve"], MinMaxCurve(0.0f), "inheritVelocity.curve");
            m.inheritVelocityMode = read.enumeration(inherit["mode"], kInheritMode, 0, "inheritVelocity.mode") == 1 ?
                InheritVelocityMode::Current : InheritVelocityMode::Initial;
        }else if (inherit["curve"] || inherit["mode"]){
            read.note("inheritVelocity: its module was off: Main's Inherit Velocity stays 0");
        }
    }

    // ---------------------------------------------------------------- force over lifetime
    if (const YAML::Node force = node["forceOverLifetime"]){
        ForceOverLifetimeModule& f = d.forceOverLifetime;
        f.present = true;
        f.enabled = read.flag(force["enabled"], false);
        f.x = read.minMax(force["x"], MinMaxCurve(0.0f), "forceOverLifetime.x");
        f.y = read.minMax(force["y"], MinMaxCurve(0.0f), "forceOverLifetime.y");
        f.z = read.minMax(force["z"], MinMaxCurve(0.0f), "forceOverLifetime.z");
        f.space = space(read.enumeration(force["space"], kSimulationSpace, 0, "forceOverLifetime.space"));
        f.randomizePerTick = read.flag(force["randomized"], false);
    }

    // ---------------------------------------------------------------- colour
    if (const YAML::Node col = node["colorOverLifetime"]){
        d.colorOverLifetime.present = true;
        d.colorOverLifetime.enabled = read.flag(col["enabled"], false);
        d.colorOverLifetime.color = read.minMaxGradient(col["color"], MinMaxGradient(), "colorOverLifetime.color");
    }
    if (const YAML::Node col = node["colorBySpeed"]){
        d.colorBySpeed.present = true;
        d.colorBySpeed.enabled = read.flag(col["enabled"], false);
        d.colorBySpeed.color = read.minMaxGradient(col["color"], MinMaxGradient(), "colorBySpeed.color");
        d.colorBySpeed.speedRange = read.vector2(col["range"], Vector2(0.0f, 1.0f));
    }

    // ---------------------------------------------------------------- size
    const MinMaxCurve linear01 = MinMaxCurve::fromCurve(Curve::linear(0.0f, 1.0f));
    if (const YAML::Node size = node["sizeOverLifetime"]){
        SizeOverLifetimeModule& z = d.sizeOverLifetime;
        z.present = true;
        z.enabled = read.flag(size["enabled"], false);
        z.separateAxes = read.flag(size["separateAxes"], false);
        z.size = read.minMax(size["size"], linear01, "sizeOverLifetime.size");
        z.sizeY = read.minMax(size["y"], linear01, "sizeOverLifetime.y");
        z.sizeZ = read.minMax(size["z"], linear01, "sizeOverLifetime.z");
    }
    if (const YAML::Node size = node["sizeBySpeed"]){
        SizeBySpeedModule& z = d.sizeBySpeed;
        z.present = true;
        z.enabled = read.flag(size["enabled"], false);
        z.separateAxes = read.flag(size["separateAxes"], false);
        z.size = read.minMax(size["size"], linear01, "sizeBySpeed.size");
        z.sizeY = read.minMax(size["y"], linear01, "sizeBySpeed.y");
        z.sizeZ = read.minMax(size["z"], linear01, "sizeBySpeed.z");
        z.speedRange = read.vector2(size["range"], Vector2(0.0f, 1.0f));
    }

    // ---------------------------------------------------------------- rotation (rad/s there, deg/s here)
    if (const YAML::Node rot = node["rotationOverLifetime"]){
        RotationOverLifetimeModule& r = d.rotationOverLifetime;
        r.present = true;
        r.enabled = read.flag(rot["enabled"], false);
        r.separateAxes = read.flag(rot["separateAxes"], false);
        r.angularVelocityX = read.minMax(rot["x"], MinMaxCurve(0.0f), "rotationOverLifetime.x", kRadToDeg);
        r.angularVelocityY = read.minMax(rot["y"], MinMaxCurve(0.0f), "rotationOverLifetime.y", kRadToDeg);
        r.angularVelocity = read.minMax(rot["z"], MinMaxCurve(45.0f), "rotationOverLifetime.z", kRadToDeg);
    }
    if (const YAML::Node rot = node["rotationBySpeed"]){
        RotationBySpeedModule& r = d.rotationBySpeed;
        r.present = true;
        r.enabled = read.flag(rot["enabled"], false);
        r.separateAxes = read.flag(rot["separateAxes"], false);
        r.angularVelocityX = read.minMax(rot["x"], MinMaxCurve(0.0f), "rotationBySpeed.x", kRadToDeg);
        r.angularVelocityY = read.minMax(rot["y"], MinMaxCurve(0.0f), "rotationBySpeed.y", kRadToDeg);
        r.angularVelocity = read.minMax(rot["z"], MinMaxCurve(45.0f), "rotationBySpeed.z", kRadToDeg);
        r.speedRange = read.vector2(rot["range"], Vector2(0.0f, 1.0f));
    }

    // ---------------------------------------------------------------- noise
    if (const YAML::Node noise = node["noise"]){
        NoiseModule& n = d.noise;
        n.present = true;
        n.enabled = read.flag(noise["enabled"], false);
        n.separateAxes = read.flag(noise["separateAxes"], false);
        n.strength = read.minMax(noise["strength"], MinMaxCurve(1.0f), "noise.strength");
        n.strengthY = read.minMax(noise["strengthY"], MinMaxCurve(1.0f), "noise.strengthY");
        n.strengthZ = read.minMax(noise["strengthZ"], MinMaxCurve(1.0f), "noise.strengthZ");
        n.frequency = std::max(0.0001f, read.number(noise["frequency"], 0.5f));
        n.scrollSpeed = read.minMax(noise["scrollSpeed"], MinMaxCurve(0.0f), "noise.scrollSpeed");
        n.damping = read.flag(noise["damping"], true);
        n.octaves = std::clamp(read.integer(noise["octaves"], 1), 1, 4);
        n.octaveMultiplier = read.number(noise["octaveMultiplier"], 0.5f);
        n.octaveScale = read.number(noise["octaveScale"], 2.0f);
        n.positionAmount = read.minMax(noise["positionAmount"], MinMaxCurve(1.0f), "noise.positionAmount");
        // radians per second at amount 1 in both
        n.rotationAmount = read.minMax(noise["rotationAmount"], MinMaxCurve(0.0f), "noise.rotationAmount");
        n.sizeAmount = read.minMax(noise["sizeAmount"], MinMaxCurve(0.0f), "noise.sizeAmount");
    }

    // ---------------------------------------------------------------- texture sheet
    if (const YAML::Node sheet = node["textureSheetAnimation"]){
        TextureSheetAnimationModule& t = d.textureSheetAnimation;
        t.present = true;
        t.enabled = read.flag(sheet["enabled"], false);
        t.mode = read.enumeration(sheet["mode"], kAnimationMode, 0, "textureSheetAnimation.mode") == 1 ? TextureSheetMode::Sprites : TextureSheetMode::Grid;
        t.tilesX = std::max(1, read.integer(sheet["numTilesX"], 1));
        t.tilesY = std::max(1, read.integer(sheet["numTilesY"], 1));
        t.animation = read.enumeration(sheet["animation"], kAnimationType, 0, "textureSheetAnimation.animation") == 1 ?
            TextureSheetAnimation::SingleRow : TextureSheetAnimation::WholeSheet;
        t.rowMode = read.enumeration(sheet["rowMode"], kRowMode, 0, "textureSheetAnimation.rowMode") == 1 ? TextureSheetRowMode::Random : TextureSheetRowMode::Custom;
        t.rowIndex = std::max(0, read.integer(sheet["rowIndex"], 0));
        switch (read.enumeration(sheet["timeMode"], kTimeMode, 0, "textureSheetAnimation.timeMode")){
            case 1: t.timeMode = TextureSheetTimeMode::Speed; break;
            case 2: t.timeMode = TextureSheetTimeMode::FPS; break;
            default: t.timeMode = TextureSheetTimeMode::Lifetime; break;
        }
        t.frameOverTime = read.minMax(sheet["frameOverTime"], MinMaxCurve::fromCurve(Curve::linear(0.0f, 1.0f)), "textureSheetAnimation.frameOverTime");
        // origin/main's default is 30 frames per second
        t.fps = read.number(sheet["fps"], 30.0f);
        t.speedRange = read.vector2(sheet["speedRange"], Vector2(0.0f, 1.0f));
        t.startFrame = read.minMax(sheet["startFrame"], MinMaxCurve(0.0f), "textureSheetAnimation.startFrame");
        t.cycles = (float)std::max(1, read.integer(sheet["cycles"], 1));
        t.flipU = std::clamp(read.number(sheet["flipU"], 0.0f), 0.0f, 1.0f);
        t.flipV = std::clamp(read.number(sheet["flipV"], 0.0f), 0.0f, 1.0f);
        if (t.mode == TextureSheetMode::Sprites){
            // frame ids of the target's sprite frames: their rects
            if (const YAML::Node ids = sheet["sprites"]; ids && ids.IsSequence()){
                for (const YAML::Node& id : ids){
                    const int index = read.integer(id, -1);
                    if (index < 0 || (size_t)index >= target.frames.size()){
                        read.note("textureSheetAnimation.sprites: frame " + std::to_string(index) + " is not one of its target's frames: left out");
                        continue;
                    }
                    if (t.sprites.size() >= kMaxSpriteFrames){
                        read.note("textureSheetAnimation.sprites: more than 64 frames: the first 64 are kept");
                        break;
                    }
                    t.sprites.push_back(target.frames[(size_t)index]);
                }
            }
        }
    }

    // ---------------------------------------------------------------- renderer
    const YAML::Node rn = moduleOf(node, "renderer");
    RendererModule& r = d.renderer;
    const int renderMode = read.enumeration(rn["renderMode"], kRenderMode, 0, "renderer.renderMode");
    const int alignment = read.enumeration(rn["alignment"], kRenderSpace, 0, "renderer.alignment");
    if (!applyRenderMode(renderMode, alignment, d.simulationSpace == SimulationSpace::Local, r))
        read.note("renderer.alignment: a stretched, horizontal or vertical billboard has no alignment of its own: left out");
    if (r.renderMode == RenderMode::Mesh){
        const std::string mesh = rn["mesh"] && rn["mesh"].IsScalar() ? rn["mesh"].Scalar() : std::string("quad");
        if (target.kind == Target::Kind::Mesh){
            // the GameObject's own mesh (the one origin/main made from the name, or a model's)
            MeshSource self;
            self.type = MeshSourceType::GameObject;
            r.meshes = {self};
            result.drawsTargetMesh = true;
        }else{
            bool known = false;
            r.meshes = {meshSourceOf(mesh, known)};
            if (!known)
                read.note("renderer.mesh: '" + mesh + "' is read as a model path");
        }
    }
    r.cameraScale = read.number(rn["cameraVelocityScale"], 0.0f);
    r.speedScale = read.number(rn["velocityScale"], 0.0f);
    r.lengthScale = read.number(rn["lengthScale"], 2.0f);
    switch (read.enumeration(rn["sortMode"], kSortMode, 0, "renderer.sortMode")){
        case 1: r.sortMode = SortMode::ByDistance; break;
        case 2: r.sortMode = SortMode::OldestInFront; break;
        case 3: r.sortMode = SortMode::YoungestInFront; break;
        default: r.sortMode = SortMode::None; break;
    }
    r.sortingFudge = read.number(rn["sortingFudge"], 0.0f);
    r.sortPriority = read.integer(rn["sortPriority"], 0);
    r.minScreenSize = read.number(rn["minParticleSize"], 0.0f);
    r.maxScreenSize = read.number(rn["maxParticleSize"], 0.5f);
    r.pivot = read.vector3(rn["pivot"], Vector3(0.0f, 0.0f, 0.0f));
    r.flip = read.vector3(rn["flip"], Vector3(0.0f, 0.0f, 0.0f));

    // what its target drew them with
    if (target.kind == Target::Kind::Mesh){
        applyMaterial(target.material, r, result.notes);
        r.enabled = target.meshEnabled;
        r.receiveShadows = target.receiveShadows;
        if (target.castShadows)
            read.note("its MeshComponent cast shadows: particles do not");
    }else if (target.kind == Target::Kind::Points){
        r.texture = target.texture;
        r.textureGuid = target.textureGuid;
        // points without a texture drew squares, without a size cap but the device's
        r.untexturedShape = UntexturedShape::Square;
        r.maxScreenSize = std::max(r.maxScreenSize, 10.0f);
        r.shaderFeatures = target.pointsFeatures;
        r.shaderProperties = target.pointsProperties;
        r.shaderTextures = target.pointsTextures;
        if (!target.pointsShader.empty() && target.pointsShader != "library:particle")
            read.note("the custom points shader \"" + target.pointsShader + "\" is not a particles shader: library:particles draws them");
        if (target.pointsOpaque){
            r.blend = ParticleBlend::Alpha;
            read.note("an opaque Points blend draws alpha blended");
        }else{
            r.blend = target.pointsBlend < 0 ? ParticleBlend::Alpha : (ParticleBlend)target.pointsBlend;
        }
        r.depthWrite = target.pointsDepthWrite == 1;
    }else{
        read.note("its action's target draws nothing (no MeshComponent nor Points): the default particle look");
    }
    return result;
}
