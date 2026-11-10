// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "noise/NoiseSwitch.h"

#if JASPERENGINE_NOISE_ENABLED

#include "noise/NoiseDetMath.h"
#include "noise/NoiseNodes.h"
#include "noise/NoisePlan.h"
#include "terrain/HeightfieldErosion.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <queue>
#include <unordered_map>

namespace jasperengine { namespace noise {

    namespace {

        using graph::Graph;
        using graph::Issue;
        using graph::Json;
        using graph::Node;
        using graph::Severity;

        constexpr int kValueX = 0, kValueY = 1, kValueZ = 2, kValueU = 3, kValueV = 4, kFirstValue = 5;

        // One thing a node's output port holds: a field, or a coord's three fields
        struct PortValue {
            std::string port;
            int v[3] = {-1, -1, -1};
            bool coord = false;
        };

        struct InEdge {
            std::string port;
            int fromNode = -1;
            std::string fromPort;
        };

        void addIssue(std::vector<Issue>& issues, Severity severity, const char* code, std::string message, std::string node = {}) {
            Issue issue;
            issue.severity = severity;
            issue.code = code;
            issue.nodeId = std::move(node);
            issue.message = std::move(message);
            issues.push_back(std::move(issue));
        }

        bool hasError(const std::vector<Issue>& issues) {
            return std::any_of(issues.begin(), issues.end(), [](const Issue& issue) { return issue.severity == Severity::Error; });
        }

        Basis basisOf(const std::string& name) {
            if (name == "simplex") return Basis::Simplex;
            if (name == "value") return Basis::Value;
            if (name == "cellular") return Basis::Cellular;
            return Basis::Perlin;
        }

        int dimensionsOf(const Node& node) {
            return paramString(node, "dimensions", "2d") == "3d" ? 3 : 2;
        }

        MathKind mathKindOf(const std::string& name) {
            static const std::pair<const char*, MathKind> kinds[] = {
                {"add", MathKind::Add}, {"subtract", MathKind::Subtract}, {"multiply", MathKind::Multiply}, {"divide", MathKind::Divide},
                {"min", MathKind::Min}, {"max", MathKind::Max}, {"power", MathKind::Power}, {"modulo", MathKind::Modulo},
                {"abs", MathKind::Abs}, {"negate", MathKind::Negate}, {"one_minus", MathKind::OneMinus}, {"sqrt", MathKind::Sqrt},
                {"floor", MathKind::Floor}, {"frac", MathKind::Frac}, {"sin", MathKind::Sin}, {"cos", MathKind::Cos}};
            for (const auto& [id, kind] : kinds) {
                if (name == id) return kind;
            }
            return MathKind::Add;
        }

        bool mathIsBinary(MathKind kind) {
            return kind == MathKind::Add || kind == MathKind::Subtract || kind == MathKind::Multiply || kind == MathKind::Divide ||
                   kind == MathKind::Min || kind == MathKind::Max || kind == MathKind::Power || kind == MathKind::Modulo;
        }

        float degreesTan(float degrees) {
            return detmath::DetMath<Lane1>::tan(degrees * (detmath::kPi / 180.0f));
        }

        // S20's curve: Hermite between keys, a null tangent is a step; 256 samples, linear between (R9)
        std::vector<float> curveLut(const Json& keys, float& t0, float& scale) {
            std::vector<float> lut(256, 0.0f);
            const auto numberAt = [](const Json& key, size_t i, float fallback) {
                return key.size() > i && key[i].is_number() ? static_cast<float>(key[i].get<double>()) : fallback;
            };
            const size_t count = keys.size();
            t0 = numberAt(keys[0], 0, 0.0f);
            const float t1 = numberAt(keys[count - 1], 0, t0);
            scale = t1 > t0 ? 255.0f / (t1 - t0) : 0.0f;
            size_t segment = 0;
            for (int i = 0; i < 256; i++) {
                const float t = scale > 0.0f ? t0 + (t1 - t0) * (static_cast<float>(i) / 255.0f) : t0;
                while (segment + 2 < count && numberAt(keys[segment + 1], 0, 0.0f) <= t) segment++;
                const Json& a = keys[segment];
                const Json& b = keys[std::min(segment + 1, count - 1)];
                const float ta = numberAt(a, 0, 0.0f), tb = numberAt(b, 0, 0.0f);
                const float va = numberAt(a, 1, 0.0f), vb = numberAt(b, 1, 0.0f);
                const float span = tb - ta;
                const bool step = (a.size() > 3 && a[3].is_null()) || (b.size() > 2 && b[2].is_null());
                if (span <= 0.0f || step) {
                    lut[static_cast<size_t>(i)] = t >= tb && span >= 0.0f && count > 1 && i == 255 ? vb : va;
                    continue;
                }
                const float s = (t - ta) / span;
                const float m0 = numberAt(a, 3, 0.0f) * span;
                const float m1 = numberAt(b, 2, 0.0f) * span;
                const float s2 = s * s, s3 = s2 * s;
                lut[static_cast<size_t>(i)] = (2.0f * s3 - 3.0f * s2 + 1.0f) * va + (s3 - 2.0f * s2 + s) * m0 + (-2.0f * s3 + 3.0f * s2) * vb + (s3 - s2) * m1;
            }
            return lut;
        }

        struct Compiler {
            const Graph& graph;
            const CompileOptions& options;
            CompileResult& result;
            std::shared_ptr<const graph::GraphKindDesc> kind;
            PlanData plan;
            std::vector<std::vector<InEdge>> inEdges;
            std::vector<std::vector<PortValue>> outPorts;
            std::vector<int> valueProducer;     // op; -1 for the domain's own values
            std::vector<char> valueConstant;
            std::vector<float> constants;
            std::vector<Interval> valueRanges;
            std::vector<char> nodeFolded;
            std::vector<std::pair<int, std::string>> missingInputs;
            std::vector<int> outputNodes;

            Compiler(const Graph& g, const CompileOptions& o, CompileResult& r) : graph(g), options(o), result(r) {}

            int newValue() {
                valueProducer.push_back(-1);
                valueConstant.push_back(0);
                constants.push_back(0.0f);
                valueRanges.push_back(Interval{});
                return static_cast<int>(valueProducer.size()) - 1;
            }

            const InEdge* edgeInto(int node, const char* port) const {
                for (const InEdge& edge : inEdges[static_cast<size_t>(node)]) {
                    if (edge.port == port) return &edge;
                }
                return nullptr;
            }

            const PortValue* portOf(int node, const std::string& port) const {
                for (const PortValue& value : outPorts[static_cast<size_t>(node)]) {
                    if (value.port == port) return &value;
                }
                return nullptr;
            }

            void setField(int node, const char* port, int value) {
                PortValue out;
                out.port = port;
                out.v[0] = value;
                outPorts[static_cast<size_t>(node)].push_back(out);
            }

            void setCoord(int node, const char* port, int x, int y, int z) {
                PortValue out;
                out.port = port;
                out.v[0] = x;
                out.v[1] = y;
                out.v[2] = z;
                out.coord = true;
                outPorts[static_cast<size_t>(node)].push_back(out);
            }

            // Pushes an op with a new output per entry of outputs, working out its range and whether it
            // needs sanitising (R6)
            int pushOp(PlanOp op, int node, int outputs) {
                op.node = node;
                for (int k = 0; k < outputs; k++) op.outValue[k] = newValue();
                const int index = static_cast<int>(plan.ops.size());
                rangeOfOp(op, plan, valueRanges);
                for (int k = 0; k < outputs; k++) {
                    const Interval& range = valueRanges[static_cast<size_t>(op.outValue[k])];
                    if (!(std::isfinite(range.min) && std::isfinite(range.max))) op.sanitize = true;
                    valueProducer[static_cast<size_t>(op.outValue[k])] = index;
                }
                plan.ops.push_back(op);
                return index;
            }

            int constantValue(float value, int node) {
                PlanOp op;
                op.kind = OpKind::Fill;
                op.f[0] = value;
                const int index = pushOp(op, node, 1);
                const int v = plan.ops[static_cast<size_t>(index)].outValue[0];
                valueConstant[static_cast<size_t>(v)] = 1;
                constants[static_cast<size_t>(v)] = value;
                return v;
            }

            // A field input: its edge, else a same-named parameter (R10), else 0 with a required-input error if it is used
            int fieldInput(int nodeIndex, const char* port, const char* param, double fallback) {
                const Node& node = graph.nodes[static_cast<size_t>(nodeIndex)];
                if (const InEdge* edge = edgeInto(nodeIndex, port)) {
                    if (const PortValue* value = portOf(edge->fromNode, edge->fromPort)) {
                        if (!value->coord) return value->v[0];
                    }
                }
                if (!param) missingInputs.emplace_back(nodeIndex, port);
                return constantValue(static_cast<float>(param ? paramNumber(node, param, fallback) : fallback), nodeIndex);
            }

            // Unconnected means position (R10)
            void coordInput(int nodeIndex, int out[3]) {
                out[0] = kValueX;
                out[1] = kValueY;
                out[2] = kValueZ;
                if (const InEdge* edge = edgeInto(nodeIndex, "coord")) {
                    if (const PortValue* value = portOf(edge->fromNode, edge->fromPort)) {
                        if (value->coord) {
                            out[0] = value->v[0];
                            out[1] = value->v[1];
                            out[2] = value->v[2];
                        }
                    }
                }
            }

            // A point op with constant inputs folds to its value at compile time (R18)
            int foldable(PlanOp op, int nodeIndex) {
                bool allConstant = true;
                for (int k = 0; k < 4; k++) {
                    if (op.inValue[k] >= 0 && !valueConstant[static_cast<size_t>(op.inValue[k])]) allConstant = false;
                }
                const int index = pushOp(op, nodeIndex, 1);
                const int v = plan.ops[static_cast<size_t>(index)].outValue[0];
                if (!allConstant) return v;
                PlanOp folded = plan.ops[static_cast<size_t>(index)];
                const float value = foldOp(folded, plan, constants.data());
                // Its range is the fill's from here on, so what reads it can fold or prove too
                PlanOp fill;
                fill.kind = OpKind::Fill;
                fill.f[0] = value;
                fill.node = nodeIndex;
                fill.outValue[0] = v;
                plan.ops[static_cast<size_t>(index)] = fill;
                valueConstant[static_cast<size_t>(v)] = 1;
                constants[static_cast<size_t>(v)] = value;
                valueRanges[static_cast<size_t>(v)] = Interval{value, value};
                nodeFolded[static_cast<size_t>(nodeIndex)] = 1;
                return v;
            }

            FractalParams fractalOf(const Node& node) {
                FractalParams fractal;
                fractal.basis.basis = basisOf(paramString(node, "basis", "perlin"));
                fractal.basis.dimensions = dimensionsOf(node);
                fractal.basis.frequency = static_cast<float>(paramNumber(node, "frequency", 4.0));
                fractal.octaves = std::clamp(static_cast<int>(paramNumber(node, "octaves", 6.0)), 1, 12);
                fractal.lacunarity = static_cast<float>(paramNumber(node, "lacunarity", 2.0));
                fractal.gain = static_cast<float>(paramNumber(node, "gain", 0.5));
                return fractal;
            }

            uint32_t seedOffsetOf(const Node& node) {
                return static_cast<uint32_t>(static_cast<int64_t>(paramNumber(node, "seed_offset", 0.0)));
            }

            void emitNode(int nodeIndex) {
                const Node& node = graph.nodes[static_cast<size_t>(nodeIndex)];
                const std::string& type = node.type;
                if (type == "position") {
                    setCoord(nodeIndex, "out", kValueX, kValueY, kValueZ);
                    const char* ports[3] = {"x", "y", "z"};
                    for (int k = 0; k < 3; k++) {
                        PlanOp op;
                        op.kind = OpKind::PositionField;
                        op.inValue[0] = kValueX + k;
                        op.sanitize = true;
                        const int index = pushOp(op, nodeIndex, 1);
                        setField(nodeIndex, ports[k], plan.ops[static_cast<size_t>(index)].outValue[0]);
                    }
                } else if (type == "terrain_uv") {
                    setField(nodeIndex, "u", kValueU);
                    setField(nodeIndex, "v", kValueV);
                } else if (type == "constant") {
                    setField(nodeIndex, "out", constantValue(static_cast<float>(paramNumber(node, "value", 0.0)), nodeIndex));
                    nodeFolded[static_cast<size_t>(nodeIndex)] = 1;
                } else if (type == "variable") {
                    emitVariable(nodeIndex);
                } else if (type == "terrain_height") {
                    PlanOp op;
                    op.kind = OpKind::TerrainHeight;
                    const int index = pushOp(op, nodeIndex, 1);
                    setField(nodeIndex, "out", plan.ops[static_cast<size_t>(index)].outValue[0]);
                } else if (type == "perlin" || type == "simplex" || type == "value" || type == "cellular") {
                    PlanOp op;
                    op.kind = OpKind::Noise;
                    BasisParams& basis = op.fractal.basis;
                    basis.basis = basisOf(type);
                    basis.dimensions = dimensionsOf(node);
                    basis.frequency = static_cast<float>(paramNumber(node, "frequency", 4.0));
                    if (type == "cellular") {
                        const std::string metric = paramString(node, "metric", "euclidean");
                        basis.metric = metric == "manhattan" ? CellularMetric::Manhattan : (metric == "chebyshev" ? CellularMetric::Chebyshev : CellularMetric::Euclidean);
                        const std::string ret = paramString(node, "return", "f1");
                        basis.cellReturn = ret == "f2" ? CellularReturn::F2 : (ret == "f2_minus_f1" ? CellularReturn::F2MinusF1 : (ret == "cell_value" ? CellularReturn::CellValue : CellularReturn::F1));
                        basis.jitter = std::clamp(static_cast<float>(paramNumber(node, "jitter", 1.0)), 0.0f, 1.0f);
                    }
                    op.seedOffset = seedOffsetOf(node);
                    coordInput(nodeIndex, op.inValue);
                    const int index = pushOp(op, nodeIndex, 1);
                    setField(nodeIndex, "out", plan.ops[static_cast<size_t>(index)].outValue[0]);
                } else if (type == "fbm" || type == "ridged") {
                    PlanOp op;
                    op.kind = type == "fbm" ? OpKind::Fbm : OpKind::Ridged;
                    op.fractal = fractalOf(node);
                    op.seedOffset = seedOffsetOf(node);
                    coordInput(nodeIndex, op.inValue);
                    const int index = pushOp(op, nodeIndex, 1);
                    setField(nodeIndex, "out", plan.ops[static_cast<size_t>(index)].outValue[0]);
                } else if (type == "domain_warp") {
                    PlanOp op;
                    op.kind = OpKind::DomainWarp;
                    op.fractal = fractalOf(node);
                    op.fractal.octaves = std::clamp(static_cast<int>(paramNumber(node, "octaves", 3.0)), 1, 12);
                    op.f[0] = static_cast<float>(paramNumber(node, "amplitude", 0.1));
                    op.seedOffset = seedOffsetOf(node);
                    op.i[0] = op.fractal.basis.dimensions;
                    coordInput(nodeIndex, op.inValue);
                    const int coordY = op.inValue[1];
                    if (const InEdge* edge = edgeInto(nodeIndex, "strength")) {
                        if (const PortValue* value = portOf(edge->fromNode, edge->fromPort)) op.inValue[3] = value->v[0];
                    }
                    const bool threeD = op.i[0] == 3;
                    const int index = pushOp(op, nodeIndex, 3);
                    const PlanOp& pushed = plan.ops[static_cast<size_t>(index)];
                    setCoord(nodeIndex, "out", pushed.outValue[0], threeD ? pushed.outValue[1] : coordY, pushed.outValue[2]);
                    // 2D leaves y alone: its output is the input's y, with no op and no copy
                    if (!threeD) plan.ops[static_cast<size_t>(index)].outValue[1] = -1;
                } else if (type == "domain_transform") {
                    PlanOp op;
                    op.kind = OpKind::DomainTransform;
                    const auto vec = [&](const char* name, int offset, float fallback) {
                        auto it = node.params.find(name);
                        for (int k = 0; k < 3; k++) {
                            float value = fallback;
                            if (it != node.params.end()) {
                                if (it->is_array() && it->size() > static_cast<size_t>(k) && (*it)[static_cast<size_t>(k)].is_number()) value = static_cast<float>((*it)[static_cast<size_t>(k)].get<double>());
                                else if (it->is_number()) value = static_cast<float>(it->get<double>());
                            }
                            op.f[offset + k] = value;
                        }
                    };
                    vec("scale", 0, 1.0f);
                    vec("offset", 3, 0.0f);
                    const float radians = static_cast<float>(paramNumber(node, "rotation_y", 0.0)) * (detmath::kPi / 180.0f);
                    op.f[6] = detmath::DetMath<Lane1>::cos(radians);
                    op.f[7] = detmath::DetMath<Lane1>::sin(radians);
                    coordInput(nodeIndex, op.inValue);
                    const int index = pushOp(op, nodeIndex, 3);
                    const PlanOp& pushed = plan.ops[static_cast<size_t>(index)];
                    setCoord(nodeIndex, "out", pushed.outValue[0], pushed.outValue[1], pushed.outValue[2]);
                } else if (type == "scale_offset") {
                    PlanOp op;
                    op.kind = OpKind::ScaleOffset;
                    op.f[0] = static_cast<float>(paramNumber(node, "scale", 1.0));
                    op.f[1] = static_cast<float>(paramNumber(node, "offset", 0.0));
                    op.inValue[0] = fieldInput(nodeIndex, "in", nullptr, 0.0);
                    setField(nodeIndex, "out", foldable(op, nodeIndex));
                } else if (type == "math") {
                    PlanOp op;
                    op.kind = OpKind::MathOp;
                    op.math = mathKindOf(paramString(node, "op", "add"));
                    op.inValue[0] = fieldInput(nodeIndex, "a", nullptr, 0.0);
                    if (mathIsBinary(op.math)) op.inValue[1] = fieldInput(nodeIndex, "b", "b", 0.0);
                    setField(nodeIndex, "out", foldable(op, nodeIndex));
                } else if (type == "clamp") {
                    PlanOp op;
                    op.kind = OpKind::Clamp;
                    op.f[0] = static_cast<float>(paramNumber(node, "min", 0.0));
                    op.f[1] = static_cast<float>(paramNumber(node, "max", 1.0));
                    op.inValue[0] = fieldInput(nodeIndex, "in", nullptr, 0.0);
                    setField(nodeIndex, "out", foldable(op, nodeIndex));
                } else if (type == "remap") {
                    PlanOp op;
                    op.kind = OpKind::Remap;
                    const float fromMin = static_cast<float>(paramNumber(node, "from_min", -1.0));
                    const float fromMax = static_cast<float>(paramNumber(node, "from_max", 1.0));
                    const float toMin = static_cast<float>(paramNumber(node, "to_min", 0.0));
                    const float toMax = static_cast<float>(paramNumber(node, "to_max", 1.0));
                    op.f[0] = fromMin;
                    op.f[1] = fromMax != fromMin ? 1.0f / (fromMax - fromMin) : 0.0f;
                    op.f[2] = toMin;
                    op.f[3] = toMax - toMin;
                    op.f[5] = toMax;
                    op.i[0] = paramBool(node, "clamp", false) ? 1 : 0;
                    op.inValue[0] = fieldInput(nodeIndex, "in", nullptr, 0.0);
                    setField(nodeIndex, "out", foldable(op, nodeIndex));
                } else if (type == "curve") {
                    PlanOp op;
                    op.kind = OpKind::Curve;
                    auto keys = node.params.find("keys");
                    static const Json fallback = Json::array({Json::array({0.0, 0.0, 1.0, 1.0}), Json::array({1.0, 1.0, 1.0, 1.0})});
                    plan.luts.push_back(curveLut(keys != node.params.end() && keys->is_array() && !keys->empty() ? *keys : fallback, op.f[0], op.f[1]));
                    op.lut = static_cast<int>(plan.luts.size()) - 1;
                    op.inValue[0] = fieldInput(nodeIndex, "in", nullptr, 0.0);
                    setField(nodeIndex, "out", foldable(op, nodeIndex));
                } else if (type == "terrace") {
                    PlanOp op;
                    op.kind = OpKind::Terrace;
                    op.f[0] = static_cast<float>(std::clamp(static_cast<int>(paramNumber(node, "steps", 8.0)), 2, 64) - 1);
                    op.f[1] = std::clamp(static_cast<float>(paramNumber(node, "smoothness", 0.2)), 0.0f, 1.0f);
                    op.inValue[0] = fieldInput(nodeIndex, "in", nullptr, 0.0);
                    setField(nodeIndex, "out", foldable(op, nodeIndex));
                } else if (type == "select") {
                    const float threshold = static_cast<float>(paramNumber(node, "threshold", 0.5));
                    const float falloff = std::max(static_cast<float>(paramNumber(node, "falloff", 0.0)), 0.0f);
                    const int a = fieldInput(nodeIndex, "a", nullptr, 0.0);
                    const int b = fieldInput(nodeIndex, "b", nullptr, 0.0);
                    const int mask = fieldInput(nodeIndex, "mask", nullptr, 0.0);
                    const Interval range = valueRanges[static_cast<size_t>(mask)];
                    // A proven mask keeps one branch; the other becomes dead (R18)
                    if (range.max < threshold - falloff) { aliasNode(nodeIndex, a); return; }
                    if (range.min >= threshold + falloff) { aliasNode(nodeIndex, b); return; }
                    PlanOp op;
                    op.kind = OpKind::Select;
                    op.f[0] = threshold;
                    op.f[1] = falloff;
                    op.inValue[0] = a;
                    op.inValue[1] = b;
                    op.inValue[2] = mask;
                    setField(nodeIndex, "out", foldable(op, nodeIndex));
                } else if (type == "blend") {
                    const int a = fieldInput(nodeIndex, "a", nullptr, 0.0);
                    const int b = fieldInput(nodeIndex, "b", nullptr, 0.0);
                    const int weight = fieldInput(nodeIndex, "weight", "weight", 0.5);
                    const Interval range = valueRanges[static_cast<size_t>(weight)];
                    if (range.max <= 0.0f) { aliasNode(nodeIndex, a); return; }
                    if (range.min >= 1.0f) { aliasNode(nodeIndex, b); return; }
                    PlanOp op;
                    op.kind = OpKind::Blend;
                    op.inValue[0] = a;
                    op.inValue[1] = b;
                    op.inValue[2] = weight;
                    setField(nodeIndex, "out", foldable(op, nodeIndex));
                } else if (type == "height_mask") {
                    PlanOp op;
                    op.kind = OpKind::HeightMask;
                    op.f[0] = static_cast<float>(paramNumber(node, "min", 0.0));
                    op.f[1] = static_cast<float>(paramNumber(node, "max", 1.0));
                    op.f[2] = std::max(static_cast<float>(paramNumber(node, "falloff", 0.0)), 0.0f);
                    op.inValue[0] = fieldInput(nodeIndex, "in", nullptr, 0.0);
                    setField(nodeIndex, "out", foldable(op, nodeIndex));
                } else if (type == "slope_mask") {
                    PlanOp op;
                    op.kind = OpKind::SlopeMask;
                    op.stage = true;
                    const float minAngle = static_cast<float>(paramNumber(node, "min_angle", 0.0));
                    const float maxAngle = static_cast<float>(paramNumber(node, "max_angle", 45.0));
                    const float falloff = std::max(static_cast<float>(paramNumber(node, "falloff", 5.0)), 0.0f);
                    op.f[0] = degreesTan(std::max(minAngle - falloff, 0.0f));
                    op.f[1] = degreesTan(std::max(minAngle, 0.0f));
                    op.f[2] = degreesTan(std::min(maxAngle, 89.9f));
                    op.f[3] = degreesTan(std::min(maxAngle + falloff, 89.9f));
                    // Nothing at or under 0 degrees, nothing past a max of 90
                    op.i[0] = minAngle <= 0.0f ? 1 : 0;
                    op.i[1] = maxAngle >= 90.0f ? 1 : 0;
                    op.inValue[0] = fieldInput(nodeIndex, "in", nullptr, 0.0);
                    const int index = pushOp(op, nodeIndex, 1);
                    setField(nodeIndex, "out", plan.ops[static_cast<size_t>(index)].outValue[0]);
                } else if (type == "hydraulic_erosion") {
                    PlanOp op;
                    op.kind = OpKind::Hydraulic;
                    op.stage = true;
                    op.f[0] = std::clamp(static_cast<float>(paramNumber(node, "droplets_per_texel", 0.25)), 0.0f, 4.0f);
                    op.f[1] = std::clamp(static_cast<float>(paramNumber(node, "inertia", 0.05)), 0.0f, 1.0f);
                    op.f[2] = static_cast<float>(paramNumber(node, "capacity", 4.0));
                    op.f[3] = static_cast<float>(paramNumber(node, "min_slope", 0.01));
                    op.f[4] = static_cast<float>(paramNumber(node, "erode_rate", 0.3));
                    op.f[5] = static_cast<float>(paramNumber(node, "deposit_rate", 0.3));
                    op.f[6] = static_cast<float>(paramNumber(node, "evaporation", 0.02));
                    op.f[7] = static_cast<float>(paramNumber(node, "gravity", 4.0));
                    op.i[0] = std::clamp(static_cast<int>(paramNumber(node, "lifetime", 48.0)), 8, 128);
                    op.i[1] = std::clamp(static_cast<int>(paramNumber(node, "radius", 2.0)), 1, 4);
                    op.seedOffset = seedOffsetOf(node);
                    op.inValue[0] = fieldInput(nodeIndex, "height", nullptr, 0.0);
                    if (const InEdge* edge = edgeInto(nodeIndex, "hardness")) {
                        if (const PortValue* value = portOf(edge->fromNode, edge->fromPort)) op.inValue[1] = value->v[0];
                    }
                    // Droplets only move what is there, and a deposit fills a hollow: the input's span past
                    // either end is more than any real run reaches, and the stage clamps to it (R19)
                    const Interval in = valueRanges[static_cast<size_t>(op.inValue[0])];
                    const float margin = std::isfinite(in.max - in.min) ? std::max(in.max - in.min, 0.1f) : 0.0f;
                    op.f[8] = std::isfinite(in.min) ? in.min - margin : -std::numeric_limits<float>::max();
                    op.f[9] = std::isfinite(in.max) ? in.max + margin : std::numeric_limits<float>::max();
                    const int index = pushOp(op, nodeIndex, 3);
                    const PlanOp& pushed = plan.ops[static_cast<size_t>(index)];
                    setField(nodeIndex, "out", pushed.outValue[0]);
                    setField(nodeIndex, "flow", pushed.outValue[1]);
                    setField(nodeIndex, "sediment", pushed.outValue[2]);
                } else if (type == "thermal_erosion") {
                    PlanOp op;
                    op.kind = OpKind::Thermal;
                    op.stage = true;
                    op.i[0] = std::clamp(static_cast<int>(paramNumber(node, "iterations", 8.0)), 1, 64);
                    op.f[0] = std::clamp(static_cast<float>(paramNumber(node, "talus_angle", 35.0)), 0.0f, 89.0f);
                    op.f[1] = std::clamp(static_cast<float>(paramNumber(node, "rate", 0.5)), 0.0f, 1.0f);
                    op.inValue[0] = fieldInput(nodeIndex, "height", nullptr, 0.0);
                    const int index = pushOp(op, nodeIndex, 1);
                    setField(nodeIndex, "out", plan.ops[static_cast<size_t>(index)].outValue[0]);
                } else if (type == "output_height") {
                    OutputInfo info;
                    info.kind = OutputKind::Height;
                    info.name = "height";
                    PlanOp op = outputOp(nodeIndex, "in", info);
                    if (paramString(node, "unit", "normalized") == "metres") {
                        op.f[0] = 1.0f;
                        op.f[1] = options.maxHeight > 0.0f ? options.maxHeight : 1.0f;
                    }
                    pushOutput(op, nodeIndex);
                } else if (type == "output_layer") {
                    OutputInfo info;
                    info.kind = OutputKind::Layer;
                    info.index = static_cast<int>(paramNumber(node, "layer", 0.0));
                    info.name = "layer:" + std::to_string(info.index);
                    info.optional = paramBool(node, "optional", false);
                    pushOutput(outputOp(nodeIndex, "weight", info), nodeIndex);
                } else if (type == "output_density") {
                    OutputInfo info;
                    info.kind = OutputKind::Density;
                    info.index = static_cast<int>(paramNumber(node, "foliage_layer", 0.0));
                    info.name = "density:" + std::to_string(info.index);
                    info.optional = paramBool(node, "optional", false);
                    pushOutput(outputOp(nodeIndex, "density", info), nodeIndex);
                } else if (type == "output_value") {
                    OutputInfo info;
                    info.kind = OutputKind::Value;
                    info.name = paramString(node, "name", "value");
                    pushOutput(outputOp(nodeIndex, "in", info), nodeIndex);
                }
            }

            void aliasNode(int nodeIndex, int value) {
                setField(nodeIndex, "out", value);
                nodeFolded[static_cast<size_t>(nodeIndex)] = 1;
            }

            void emitVariable(int nodeIndex) {
                const Node& node = graph.nodes[static_cast<size_t>(nodeIndex)];
                const std::string name = paramString(node, "name", "");
                const graph::Variable* variable = graph.findVariable(name);
                float value = 0.0f;
                bool fixed = !variable || !variable->exposed;
                if (variable && variable->defaultValue.is_number()) value = static_cast<float>(variable->defaultValue.get<double>());
                for (const auto& [setName, setValue] : options.variables) {
                    if (setName == name) {
                        value = setValue;
                        fixed = true;
                    }
                }
                if (fixed) {
                    setField(nodeIndex, "out", constantValue(std::isfinite(value) ? value : 0.0f, nodeIndex));
                    nodeFolded[static_cast<size_t>(nodeIndex)] = 1;
                    return;
                }
                PlanVariable slot;
                slot.name = name;
                slot.value = value;
                slot.min = -std::numeric_limits<float>::max();
                slot.max = std::numeric_limits<float>::max();
                auto lo = variable->passthrough.find("min");
                auto hi = variable->passthrough.find("max");
                if (lo != variable->passthrough.end() && lo->is_number()) slot.min = static_cast<float>(lo->get<double>());
                if (hi != variable->passthrough.end() && hi->is_number()) slot.max = static_cast<float>(hi->get<double>());
                plan.variables.push_back(slot);
                PlanOp op;
                op.kind = OpKind::Variable;
                op.i[0] = static_cast<int>(plan.variables.size()) - 1;
                const int index = pushOp(op, nodeIndex, 1);
                setField(nodeIndex, "out", plan.ops[static_cast<size_t>(index)].outValue[0]);
            }

            PlanOp outputOp(int nodeIndex, const char* port, OutputInfo info) {
                PlanOp op;
                op.kind = OpKind::Output;
                info.nodeId = graph.nodes[static_cast<size_t>(nodeIndex)].id;
                op.inValue[0] = fieldInput(nodeIndex, port, nullptr, 0.0);
                op.i[0] = static_cast<int>(plan.outputs.size());
                PlanOutput output;
                output.info = std::move(info);
                plan.outputs.push_back(std::move(output));
                return op;
            }

            void pushOutput(PlanOp op, int nodeIndex) {
                const int index = pushOp(op, nodeIndex, 1);
                PlanOutput& output = plan.outputs[static_cast<size_t>(plan.ops[static_cast<size_t>(index)].i[0])];
                output.value = plan.ops[static_cast<size_t>(index)].outValue[0];
                output.op = index;
                outputNodes.push_back(nodeIndex);
            }

            void addPreviewOutput(int nodeIndex, const std::string& port, int value) {
                OutputInfo info;
                info.kind = OutputKind::NodePreview;
                info.nodeId = graph.nodes[static_cast<size_t>(nodeIndex)].id;
                info.name = "node:" + info.nodeId + ":" + port;
                PlanOp op;
                op.kind = OpKind::Output;
                op.inValue[0] = value;
                op.i[0] = static_cast<int>(plan.outputs.size());
                PlanOutput output;
                output.info = std::move(info);
                plan.outputs.push_back(std::move(output));
                const int index = pushOp(op, nodeIndex, 1);
                PlanOutput& back = plan.outputs.back();
                back.value = plan.ops[static_cast<size_t>(index)].outValue[0];
                back.op = index;
            }

            std::vector<int> topoOrder() {
                const size_t count = graph.nodes.size();
                std::unordered_map<std::string_view, int> indexOf;
                indexOf.reserve(count);
                for (size_t i = 0; i < count; i++) indexOf.emplace(graph.nodes[i].id, static_cast<int>(i));
                inEdges.assign(count, {});
                outPorts.assign(count, {});
                std::vector<std::vector<int>> readers(count);
                std::vector<int> pending(count, 0);
                for (const graph::Edge& edge : graph.edges) {
                    auto from = indexOf.find(edge.from.node);
                    auto to = indexOf.find(edge.to.node);
                    if (from == indexOf.end() || to == indexOf.end()) continue;
                    InEdge in;
                    in.port = edge.to.port;
                    in.fromNode = from->second;
                    in.fromPort = edge.from.port;
                    inEdges[static_cast<size_t>(to->second)].push_back(std::move(in));
                    readers[static_cast<size_t>(from->second)].push_back(to->second);
                    pending[static_cast<size_t>(to->second)]++;
                }
                // File order among what is ready: the plan is a function of the file, not of a hash map
                std::priority_queue<int, std::vector<int>, std::greater<int>> ready;
                for (size_t i = 0; i < count; i++) {
                    if (pending[i] == 0) ready.push(static_cast<int>(i));
                }
                std::vector<int> order;
                order.reserve(count);
                while (!ready.empty()) {
                    const int next = ready.top();
                    ready.pop();
                    order.push_back(next);
                    for (int reader : readers[static_cast<size_t>(next)]) {
                        if (--pending[static_cast<size_t>(reader)] == 0) ready.push(reader);
                    }
                }
                return order;
            }

            std::vector<char> liveOps(bool includePreview) {
                std::vector<char> live(plan.ops.size(), 0);
                std::vector<int> stack;
                for (const PlanOutput& output : plan.outputs) {
                    if (includePreview || output.info.kind != OutputKind::NodePreview) stack.push_back(output.op);
                }
                while (!stack.empty()) {
                    const int index = stack.back();
                    stack.pop_back();
                    if (live[static_cast<size_t>(index)]) continue;
                    live[static_cast<size_t>(index)] = 1;
                    const PlanOp& op = plan.ops[static_cast<size_t>(index)];
                    for (int k = 0; k < 4; k++) {
                        if (op.inValue[k] < 0) continue;
                        const int producer = valueProducer[static_cast<size_t>(op.inValue[k])];
                        if (producer >= 0 && !live[static_cast<size_t>(producer)]) stack.push_back(producer);
                    }
                }
                return live;
            }

            // Levels, segments, padding and buffer numbers over what is live (R17, R22)
            void schedule(const std::vector<char>& live) {
                std::vector<PlanOp> ordered;
                const size_t count = plan.ops.size();
                std::vector<int> level(count, 0);
                std::vector<int> pad(count, 0);
                std::vector<int> valuePad(valueProducer.size(), 0);
                const auto stagePadding = [](const PlanOp& op) {
                    switch (op.kind) {
                        case OpKind::SlopeMask: return 1;
                        case OpKind::Thermal: return op.i[0];
                        case OpKind::Hydraulic: return op.i[0] + op.i[1] + 2;
                        default: return 0;
                    }
                };
                for (size_t index = 0; index < count; index++) {
                    if (!live[index]) continue;
                    const PlanOp& op = plan.ops[index];
                    int l = 0, p = 0;
                    for (int k = 0; k < 4; k++) {
                        if (op.inValue[k] < 0) continue;
                        const int producer = valueProducer[static_cast<size_t>(op.inValue[k])];
                        p = std::max(p, valuePad[static_cast<size_t>(op.inValue[k])]);
                        if (producer < 0) continue;
                        const PlanOp& from = plan.ops[static_cast<size_t>(producer)];
                        l = std::max(l, level[static_cast<size_t>(producer)] + (from.stage ? 1 : 0));
                    }
                    level[index] = l;
                    p += stagePadding(op);
                    pad[index] = p;
                    for (int k = 0; k < 3; k++) {
                        if (op.outValue[k] >= 0) valuePad[static_cast<size_t>(op.outValue[k])] = p;
                    }
                }
                std::vector<int> order;
                order.reserve(count);
                for (size_t index = 0; index < count; index++) {
                    if (live[index]) order.push_back(static_cast<int>(index));
                }
                std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
                    if (level[static_cast<size_t>(a)] != level[static_cast<size_t>(b)]) return level[static_cast<size_t>(a)] < level[static_cast<size_t>(b)];
                    return plan.ops[static_cast<size_t>(a)].stage < plan.ops[static_cast<size_t>(b)].stage;
                });
                std::vector<int> newIndex(count, -1);
                ordered.reserve(order.size());
                for (int index : order) {
                    newIndex[static_cast<size_t>(index)] = static_cast<int>(ordered.size());
                    ordered.push_back(plan.ops[static_cast<size_t>(index)]);
                    ordered.back().level = level[static_cast<size_t>(index)];
                }
                for (int& producer : valueProducer) {
                    if (producer >= 0) producer = newIndex[static_cast<size_t>(producer)];
                }
                int globalPad = 0;
                for (PlanOutput& output : plan.outputs) {
                    output.pad = pad[static_cast<size_t>(output.op)];
                    output.op = newIndex[static_cast<size_t>(output.op)];
                    globalPad = std::max(globalPad, output.pad);
                }
                plan.globalPad = globalPad;
                plan.ops = std::move(ordered);

                // Segments: a run of point ops, or one stage
                plan.opSegment.assign(plan.ops.size(), 0);
                plan.segments.clear();
                for (size_t i = 0; i < plan.ops.size(); i++) {
                    const bool stage = plan.ops[i].stage;
                    if (plan.segments.empty() || stage || plan.segments.back().stage) {
                        PlanSegment segment;
                        segment.begin = static_cast<uint32_t>(i);
                        segment.stage = stage;
                        plan.segments.push_back(segment);
                    }
                    plan.segments.back().end = static_cast<uint32_t>(i) + 1;
                    plan.opSegment[i] = static_cast<int>(plan.segments.size()) - 1;
                }

                // Which values must live across chunks
                const size_t values = valueProducer.size();
                std::vector<int> lastReaderOp(values, -1), lastReaderSegment(values, -1);
                std::vector<char> grid(values, 0);
                for (size_t i = 0; i < plan.ops.size(); i++) {
                    const PlanOp& op = plan.ops[i];
                    for (int k = 0; k < 4; k++) {
                        const int v = op.inValue[k];
                        if (v < 0) continue;
                        lastReaderOp[static_cast<size_t>(v)] = static_cast<int>(i);
                        lastReaderSegment[static_cast<size_t>(v)] = plan.opSegment[i];
                        if (op.stage) grid[static_cast<size_t>(v)] = 1;
                        const int producer = valueProducer[static_cast<size_t>(v)];
                        if (producer >= 0 && plan.opSegment[static_cast<size_t>(producer)] != plan.opSegment[i]) grid[static_cast<size_t>(v)] = 1;
                    }
                    if (op.stage) {
                        for (int k = 0; k < 3; k++) {
                            if (op.outValue[k] >= 0) grid[static_cast<size_t>(op.outValue[k])] = 1;
                        }
                    }
                }

                // Scratch slots: reused once a value's last reader has run (R17). The output is
                // allocated before the inputs are released, so no op's input and output share a slot.
                std::vector<int> slotOf(values, -1);
                std::vector<int> freeSlots;
                int slots = 0;
                std::vector<int> expiring;
                for (size_t i = 0; i < plan.ops.size(); i++) {
                    const PlanOp& op = plan.ops[i];
                    if (op.stage) continue;
                    for (int k = 0; k < 3; k++) {
                        const int v = op.outValue[k];
                        if (v < 0 || grid[static_cast<size_t>(v)] || op.kind == OpKind::Output) continue;
                        if (freeSlots.empty()) {
                            slotOf[static_cast<size_t>(v)] = slots++;
                        } else {
                            slotOf[static_cast<size_t>(v)] = freeSlots.back();
                            freeSlots.pop_back();
                        }
                        // Never read: free at once
                        if (lastReaderOp[static_cast<size_t>(v)] < static_cast<int>(i)) expiring.push_back(v);
                    }
                    for (int k = 0; k < 4; k++) {
                        const int v = op.inValue[k];
                        if (v < 0 || grid[static_cast<size_t>(v)] || valueProducer[static_cast<size_t>(v)] < 0) continue;
                        if (lastReaderOp[static_cast<size_t>(v)] == static_cast<int>(i) && slotOf[static_cast<size_t>(v)] >= 0) {
                            bool already = false;
                            for (int e : expiring) already = already || e == v;
                            if (!already) expiring.push_back(v);
                        }
                    }
                    for (int v : expiring) freeSlots.push_back(slotOf[static_cast<size_t>(v)]);
                    expiring.clear();
                }
                plan.slotCount = slots;
                plan.firstGrid = kFirstSlot + slots;

                std::vector<int> gridOf(values, -1);
                plan.gridLastSegment.clear();
                const auto gridNumber = [&](int v) {
                    if (gridOf[static_cast<size_t>(v)] < 0) {
                        gridOf[static_cast<size_t>(v)] = static_cast<int>(plan.gridLastSegment.size());
                        const int producer = valueProducer[static_cast<size_t>(v)];
                        plan.gridLastSegment.push_back(std::max(lastReaderSegment[static_cast<size_t>(v)], producer >= 0 ? plan.opSegment[static_cast<size_t>(producer)] : 0));
                    }
                    return gridOf[static_cast<size_t>(v)];
                };
                const auto bufferOf = [&](int v) -> int32_t {
                    if (v < kFirstValue) return v;      // the domain's own arrays share their numbers
                    if (grid[static_cast<size_t>(v)]) return plan.firstGrid + gridNumber(v);
                    return kFirstSlot + slotOf[static_cast<size_t>(v)];
                };
                for (PlanOp& op : plan.ops) {
                    for (int k = 0; k < 4; k++) op.in[k] = op.inValue[k] >= 0 ? bufferOf(op.inValue[k]) : -1;
                    if (op.kind == OpKind::Output) continue;
                    for (int k = 0; k < 3; k++) op.out[k] = op.outValue[k] >= 0 ? bufferOf(op.outValue[k]) : -1;
                }
                plan.gridCount = static_cast<int>(plan.gridLastSegment.size());
                plan.bufferTotal = plan.firstGrid + plan.gridCount;
            }

            void outputLists() {
                for (PlanOutput& output : plan.outputs) {
                    std::vector<char> needed(plan.ops.size(), 0);
                    std::vector<int> stack{output.op};
                    while (!stack.empty()) {
                        const int index = stack.back();
                        stack.pop_back();
                        if (needed[static_cast<size_t>(index)]) continue;
                        needed[static_cast<size_t>(index)] = 1;
                        const PlanOp& op = plan.ops[static_cast<size_t>(index)];
                        for (int k = 0; k < 4; k++) {
                            if (op.inValue[k] < 0) continue;
                            const int producer = valueProducer[static_cast<size_t>(op.inValue[k])];
                            if (producer >= 0) stack.push_back(producer);
                        }
                    }
                    output.ops.clear();
                    output.usesStage = false;
                    for (size_t i = 0; i < plan.ops.size(); i++) {
                        if (needed[i]) {
                            output.ops.push_back(static_cast<uint32_t>(i));
                            output.usesStage = output.usesStage || plan.ops[i].stage;
                        }
                    }
                    plan.outputInfos.push_back(output.info);
                }
            }

            void run() {
                kind = graph::GraphRegistry::get().kind(kNoiseKindId);
                result.issues = graph::validate(graph);
                if (graph.kind != kNoiseKindId) {
                    addIssue(result.issues, Severity::Error, "wrong-kind", "this is a '" + graph.kind + "' graph, not a Noise Graph");
                }
                if (options.resolution > 0) {
                    result.issues.erase(std::remove_if(result.issues.begin(), result.issues.end(), [](const Issue& issue) { return issue.code == "erosion-budget"; }), result.issues.end());
                    appendErosionBudgetIssues(graph, options.resolution, result.issues);
                }
                for (Issue& issue : result.issues) {
                    // R14: an unknown type is kept in the file, and nothing can run through it
                    if (issue.code == "unknown-node-type") issue.severity = Severity::Error;
                }
                if (hasError(result.issues)) return;

                nodeFolded.assign(graph.nodes.size(), 0);
                for (int k = 0; k < kFirstValue; k++) newValue();
                valueRanges[kValueX] = valueRanges[kValueY] = valueRanges[kValueZ] = Interval{-std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
                valueRanges[kValueU] = valueRanges[kValueV] = Interval{0.0f, 1.0f};

                const std::vector<int> order = topoOrder();
                for (int nodeIndex : order) emitNode(nodeIndex);

                const std::vector<char> liveReal = liveOps(false);
                std::vector<char> nodeLive(graph.nodes.size(), 0);
                for (size_t i = 0; i < plan.ops.size(); i++) {
                    if (liveReal[i]) nodeLive[static_cast<size_t>(plan.ops[i].node)] = 1;
                }
                // Nodes with no op of their own (a position, an alias) are live through what reads them
                std::vector<char> nodeReached = nodeLive;
                for (size_t r = order.size(); r-- > 0;) {
                    const int nodeIndex = order[r];
                    if (!nodeReached[static_cast<size_t>(nodeIndex)]) continue;
                    for (const InEdge& edge : inEdges[static_cast<size_t>(nodeIndex)]) {
                        // Past a proven select or blend, only the kept branch's value was read, so it is the op's life that counts
                        const graph::Node& reader = graph.nodes[static_cast<size_t>(nodeIndex)];
                        const bool provenNode = (reader.type == "select" || reader.type == "blend") && !nodeLive[static_cast<size_t>(nodeIndex)];
                        if (provenNode) {
                            const PortValue* kept = portOf(nodeIndex, "out");
                            const PortValue* from = portOf(edge.fromNode, edge.fromPort);
                            if (!kept || !from || kept->v[0] != from->v[0]) continue;
                        }
                        nodeReached[static_cast<size_t>(edge.fromNode)] = 1;
                    }
                }

                for (size_t i = 0; i < graph.nodes.size(); i++) {
                    const Node& node = graph.nodes[i];
                    CompiledNodeInfo info;
                    info.id = node.id;
                    info.type = node.type;
                    info.dead = !nodeReached[i];
                    info.folded = nodeFolded[i] != 0;
                    if (const PortValue* first = outPorts[i].empty() ? nullptr : &outPorts[i][0]) info.range = valueRanges[static_cast<size_t>(first->v[0])];
                    plan.nodes.push_back(std::move(info));
                    if (!nodeReached[i]) result.deadNodes.push_back(node.id);
                    else if (nodeFolded[i]) result.foldedNodes.push_back(node.id);
                }
                for (const auto& [nodeIndex, port] : missingInputs) {
                    if (!nodeReached[static_cast<size_t>(nodeIndex)]) continue;
                    const std::string& nodeId = graph.nodes[static_cast<size_t>(nodeIndex)].id;
                    bool found = false;
                    for (Issue& issue : result.issues) {
                        if (issue.code == "missing-required-input" && issue.nodeId == nodeId && issue.portId == port) {
                            issue.severity = Severity::Error;
                            found = true;
                        }
                    }
                    if (!found) addIssue(result.issues, Severity::Error, "missing-required-input", "input '" + port + "' is not connected", nodeId);
                }
                for (size_t i = 0; i < plan.ops.size(); i++) {
                    if (!liveReal[i]) continue;
                    const PlanOp& op = plan.ops[i];
                    const Node& node = graph.nodes[static_cast<size_t>(op.node)];
                    const bool gridOnly = isGridOnlyType(node.type);
                    const bool bakeOnly = isBakeOnlyType(node.type);
                    plan.gridOnly = plan.gridOnly || gridOnly;
                    plan.bakeOnly = plan.bakeOnly || bakeOnly;
                    plan.readsHeight = plan.readsHeight || node.type == "terrain_height";
                    if (options.forbidGridOnly && gridOnly) {
                        addIssue(result.issues, Severity::Error, "grid-only-node", "'" + node.type + "' needs a whole grid: it cannot run at single points", node.id);
                    }
                    if (options.forbidBakeOnly && bakeOnly) {
                        addIssue(result.issues, Severity::Error, "bake-only-node", "'" + node.type + "' only runs in the editor's bake: a game cannot load it", node.id);
                    }
                }
                if (hasError(result.issues)) return;

                if (options.previewNodes) {
                    for (int nodeIndex : order) {
                        const std::vector<PortValue> ports = outPorts[static_cast<size_t>(nodeIndex)];
                        for (const PortValue& port : ports) {
                            if (port.coord) {
                                addPreviewOutput(nodeIndex, port.port + ":x", port.v[0]);
                                addPreviewOutput(nodeIndex, port.port + ":z", port.v[2]);
                            } else {
                                addPreviewOutput(nodeIndex, port.port, port.v[0]);
                            }
                        }
                    }
                }

                const std::vector<char> live = liveOps(true);
                schedule(live);
                outputLists();
                plan.ranges = valueRanges;
                plan.readsPosition = false;
                plan.readsUv = false;
                for (const PlanOp& op : plan.ops) {
                    for (int k = 0; k < 4; k++) {
                        if (op.in[k] >= kBufX && op.in[k] <= kBufZ) plan.readsPosition = true;
                        if (op.in[k] == kBufU || op.in[k] == kBufV) plan.readsUv = true;
                    }
                    plan.readsHeight = plan.readsHeight || op.kind == OpKind::TerrainHeight;
                }

                result.opCount = plan.ops.size();
                result.bufferCount = static_cast<size_t>(plan.slotCount + kFirstSlot);
                result.graph = std::make_shared<const CompiledGraph>(std::make_unique<const PlanData>(std::move(plan)));
            }
        };

    }

    CompileResult compile(const graph::Graph& graph, const CompileOptions& options) {
        registerNoiseKind();
        CompileResult result;
        Compiler compiler(graph, options, result);
        compiler.run();
        return result;
    }

    void registerNoiseKind() {
        static std::once_flag once;
        std::call_once(once, [] { graph::GraphRegistry::get().registerKind(makeNoiseKindDesc()); });
    }

}}

#endif // JASPERENGINE_NOISE_ENABLED
