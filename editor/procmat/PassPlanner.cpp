// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PassPlanner.h"

#include "ProceduralMaterialKind.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::editor::procmat;

namespace {

    using OutputKey = std::pair<Generator*, int>;

    // an upper bound well past what any graph needs: a cut always shrinks a pass, so this only stops
    // a bug from looping forever
    constexpr int MAX_CUTS_PER_PASS = 256;

    int log2Of(int size) {
        int exponent = 0;
        while ((1 << exponent) < size) exponent++;
        return exponent;
    }

    // The shared Occlusion, Roughness and Metallic image (R14): the same defaults a bake would use
    const Json& ormBody() {
        static const Json body = Json::parse(R"json({
            "functions": [], "instance": [], "code": [], "parameters": [],
            "inputs": [
                {"name": "occlusion", "type": "f", "default": "1.0"},
                {"name": "roughness", "type": "f", "default": "1.0"},
                {"name": "metallic", "type": "f", "default": "0.0"}],
            "outputs": [{"type": "rgba", "expr": "vec4($occlusion($uv), $roughness($uv), $metallic($uv), 1.0)"}]})json");
        return body;
    }

    class Planner {
    public:
        Planner(Expander& expander, const PlanOptions& options, PassPlan& plan) : expander(expander), options(options), plan(plan) {}

        void addOutput(const std::string& name, const SourceRef& source) {
            if (!source.generator) throw ExpansionError("output " + name + " has nothing connected");
            const int pass = passFor(source);
            auto known = rootBuffers.find({source.generator, source.port});
            int buffer;
            if (known != rootBuffers.end()) {
                buffer = known->second;
            } else {
                buffer = (int)plan.buffers.size();
                plan.buffers.push_back({"output " + name, options.size, false});
                plan.steps.push_back({pass, buffer, options.size});
                rootBuffers[{source.generator, source.port}] = buffer;
            }
            plan.outputs.push_back({name, buffer});
            // a later pass can ask an earlier Buffer for levels, so this is only known now
            for (const auto& [texture, index] : buffers) plan.buffers[(size_t)index].mips = texture->needsMips();
        }

    private:
        // a Buffer holds colours: a signed distance or a 3D value through one would lose its meaning
        static bool isCuttable(Generator* consumer, int input) {
            const auto colour = [](const std::string& type) { return type == "f" || type == "rgb" || type == "rgba"; };
            auto* shader = dynamic_cast<ShaderNode*>(consumer);
            const std::optional<SourceRef> source = consumer->getSource(input);
            return shader && source && source->generator && !source->generator->isTrivial() && input < shader->inputCount() &&
                   colour(shader->inputDefinitions()[(size_t)input].value("type", std::string())) && colour(source->generator->outputType(source->port));
        }

        // Where the R6 rules put the Buffer of this generated text, if anywhere; every consumer of the
        // chosen output shares it, so one cut removes that output from the pass entirely
        std::optional<std::pair<Generator*, int>> chooseCut(const std::string& text, const PassBuilder& pass) {
            const PlanLimits& limits = options.limits;
            // a repeated output first: cutting it often brings the pass within its limits too
            for (const auto& [key, count] : pass.samples) {
                if (count < limits.repeatedSamples) continue;
                for (const PassBuilder::InlineEdge& edge : pass.edges) {
                    const std::optional<SourceRef> source = edge.consumer->getSource(edge.input);
                    if (source && source->generator == key.first && source->port == key.second && isCuttable(edge.consumer, edge.input)) {
                        return std::make_pair(edge.consumer, edge.input);
                    }
                }
            }
            for (const auto& [consumer, input] : pass.loopedInputs) {
                if (isCuttable(consumer, input)) return std::make_pair(consumer, input);
            }
            const size_t vec4s = (pass.values.size() + 3) / 4;
            enum class Over { None, Textures, Uniforms, Bytes } over = Over::None;
            if ((int)pass.textures.size() > limits.textures) over = Over::Textures;
            else if ((int)vec4s > limits.uniformVec4s) over = Over::Uniforms;
            else if (text.size() > limits.glslBytes) over = Over::Bytes;
            if (over == Over::None) return std::nullopt;
            // the input whose source holds nearest half of what is over: the pass and the new one then
            // both have room, and a long chain does not take a cut per node
            struct Load {
                size_t bytes = 0, floats = 0, textures = 0;
            };
            std::map<std::pair<Generator*, int>, Load> loads;
            for (const PassBuilder::InlineEdge& edge : pass.edges) {
                if (!isCuttable(edge.consumer, edge.input)) continue;
                Load& load = loads[{edge.consumer, edge.input}];
                load.bytes += edge.bytes;
                load.floats += edge.uniformFloats;
                load.textures += edge.textures;
            }
            const auto measure = [&](const Load& load) {
                return over == Over::Textures ? (double)load.textures : (over == Over::Uniforms ? (double)load.floats / 4.0 : (double)load.bytes);
            };
            const double half = (over == Over::Textures ? (double)pass.textures.size() : (over == Over::Uniforms ? (double)vec4s : (double)text.size())) / 2.0;
            std::optional<std::pair<Generator*, int>> best;
            double bestDistance = 0.0;
            for (const auto& [key, load] : loads) {
                if (measure(load) <= 0.0) continue;
                const double distance = std::fabs(measure(load) - half);
                if (!best || distance < bestDistance) {
                    best = key;
                    bestDistance = distance;
                }
            }
            return best;
        }

        void cut(Generator* consumer, int input) {
            const std::optional<SourceRef> source = consumer->getSource(input);
            if (!source || !source->generator) throw ExpansionError(consumer->name() + ": nothing to cut at this input");
            auto& shared = autoBuffers[{source->generator, source->port}];
            if (!shared) {
                expander.ownerNode = source->generator->graphNode;
                shared = expander.make<TextureSource>(source->generator->name() + "/auto", Json{{"size", log2Of(options.size)}});
                shared->setSource(0, *source);
            }
            consumer->setSource(input, SourceRef{shared, 0, false});
        }

        // The pass of one output, with any Buffer it needs inserted, its dependencies planned first
        int passFor(const SourceRef& root) {
            const OutputKey key{root.generator, root.port};
            auto known = passes.find(key);
            if (known != passes.end()) return known->second;
            if (root.graphInput || !root.generator) throw ExpansionError("a pass is wired to a graph's own input");

            std::string text;
            PassBuilder pass;
            for (int attempt = 0;; attempt++) {
                if (attempt > MAX_CUTS_PER_PASS) throw ExpansionError(root.generator->name() + ": too many Buffers were needed to fit one pass");
                pass = PassBuilder();
                try {
                    text = expander.shaderText(root, pass);
                } catch (const NeedsBufferError& error) {
                    cut(error.consumer, error.input);
                    continue;
                }
                const auto choice = chooseCut(text, pass);
                if (!choice) break;
                cut(choice->first, choice->second);
            }
            if ((int)pass.textures.size() > options.limits.textures) {
                throw ExpansionError(root.generator->name() + " samples " + std::to_string(pass.textures.size()) + " textures in one pass, over the limit of " + std::to_string(options.limits.textures));
            }
            if ((int)((pass.values.size() + 3) / 4) > PLAN_HARD_UNIFORM_VEC4S) {
                throw ExpansionError(root.generator->name() + " needs more uniforms in one pass than GL 4.1 guarantees");
            }

            PlannedPass planned;
            planned.label = root.generator->name();
            planned.nodes.assign(pass.nodes.begin(), pass.nodes.end());
            planned.fragment = std::move(text);
            for (double value : pass.values) planned.uniforms.values.push_back((float)value);
            const std::vector<PassBuilder::TextureUse> uses = pass.textures;
            // dependencies draw first: a Buffer's steps come before any step reading it
            for (const PassBuilder::TextureUse& use : uses) planned.textures.push_back({use.name, bufferFor(use.source)});
            const int index = (int)plan.passes.size();
            plan.passes.push_back(std::move(planned));
            passes[key] = index;
            return index;
        }

        int bufferFor(TextureSource* source) {
            auto known = buffers.find(source);
            if (known != buffers.end()) return known->second;
            const int buffer = (int)plan.buffers.size();
            buffers[source] = buffer;
            plan.buffers.push_back({source->name(), source->size(), false});

            const std::optional<SourceRef> init = source->getSource(0);
            if (init && init->generator) {
                const int pass = passFor(*init);
                plan.steps.push_back({pass, buffer, source->size()});
            } else {
                plan.steps.push_back({-1, buffer, source->size()});
            }
            if (source->kind() == TextureSource::Kind::Iterate) {
                if (const std::optional<SourceRef> loop = source->getSource(1); loop && loop->generator) {
                    // whatever the loop's own pass needed is redrawn with it each iteration: a Buffer
                    // inside the loop reads what the iteration before wrote
                    const size_t firstInLoop = plan.steps.size();
                    const int pass = passFor(*loop);
                    const std::vector<PlannedStep> inLoop(plan.steps.begin() + (std::ptrdiff_t)firstInLoop, plan.steps.end());
                    int size = source->size();
                    for (int iteration = 1; iteration <= source->iterations(); iteration++) {
                        if (source->shrink()) size = std::max(4, size >> 1);
                        if (iteration > 1) plan.steps.insert(plan.steps.end(), inLoop.begin(), inLoop.end());
                        plan.steps.push_back({pass, buffer, size});
                    }
                }
            }
            return buffer;
        }

        Expander& expander;
        const PlanOptions& options;
        PassPlan& plan;
        std::map<OutputKey, int> passes;
        std::map<OutputKey, int> rootBuffers;
        std::map<OutputKey, TextureSource*> autoBuffers;
        std::map<TextureSource*, int> buffers;
    };

    // S23's validation finds a cycle too, but a plan must not depend on the kind being registered, and
    // inlining a cycle would never end
    bool findCycle(const graph::Graph& file, std::string& node) {
        std::map<std::string, std::vector<std::string>> upstream;
        for (const graph::Edge& edge : file.edges) upstream[edge.to.node].push_back(edge.from.node);
        std::map<std::string, int> state;
        const std::function<bool(const std::string&)> visit = [&](const std::string& id) {
            int& mark = state[id];
            if (mark == 1) {
                node = id;
                return true;
            }
            if (mark == 2) return false;
            mark = 1;
            for (const std::string& next : upstream[id]) {
                if (visit(next)) return true;
            }
            state[id] = 2;
            return false;
        };
        for (const graph::Node& item : file.nodes) {
            if (visit(item.id)) return true;
        }
        return false;
    }

    int portIndex(const std::vector<graph::Port>& ports, graph::PortDirection direction, const std::string& id) {
        int index = 0;
        for (const graph::Port& port : ports) {
            if (port.direction != direction) continue;
            if (port.id == id) return index;
            index++;
        }
        return -1;
    }

    // a seed spreads over [0, 1) by the two most irrational steps, and zero stays zero
    double seedOffsetOf(double graphSeed, const graph::Node& node) {
        double own = 0.0;
        if (auto seed = node.params.find("seed"); seed != node.params.end() && seed->is_number()) own = seed->get<double>();
        const double mixed = graphSeed * 0.6180339887498949 + own * 0.7548776662466927;
        return mixed - std::floor(mixed);
    }

    const graph::Node* outputNode(const graph::Graph& file) {
        for (const graph::Node& node : file.nodes) {
            if (node.type == MATERIAL_OUTPUT_NODE) return &node;
        }
        return nullptr;
    }

}

bool PassPlan::sameStructure(const PassPlan& other) const {
    if (size != other.size || readBack != other.readBack || passes.size() != other.passes.size() || buffers.size() != other.buffers.size() ||
        steps.size() != other.steps.size() || outputs.size() != other.outputs.size()) {
        return false;
    }
    for (size_t i = 0; i < passes.size(); i++) {
        const PlannedPass& a = passes[i];
        const PlannedPass& b = other.passes[i];
        if (a.fragment != b.fragment || a.uniforms.values.size() != b.uniforms.values.size() || a.textures.size() != b.textures.size()) return false;
        for (size_t t = 0; t < a.textures.size(); t++) {
            if (a.textures[t].name != b.textures[t].name || a.textures[t].buffer != b.textures[t].buffer) return false;
        }
    }
    for (size_t i = 0; i < buffers.size(); i++) {
        if (buffers[i].size != other.buffers[i].size || buffers[i].mips != other.buffers[i].mips) return false;
    }
    for (size_t i = 0; i < steps.size(); i++) {
        if (steps[i].pass != other.steps[i].pass || steps[i].buffer != other.steps[i].buffer || steps[i].size != other.steps[i].size) return false;
    }
    for (size_t i = 0; i < outputs.size(); i++) {
        if (outputs[i].name != other.outputs[i].name || outputs[i].buffer != other.outputs[i].buffer) return false;
    }
    return true;
}

std::vector<PlanRoot> jasperengine::editor::procmat::materialOutputRoots(const graph::Graph& file) {
    std::vector<PlanRoot> roots;
    const graph::Node* output = outputNode(file);
    if (!output) return roots;
    const auto connected = [&](const char* port) {
        return std::any_of(file.edges.begin(), file.edges.end(), [&](const graph::Edge& edge) { return edge.to.node == output->id && edge.to.port == port; });
    };
    if (connected("base_color")) roots.push_back({"base_color", output->id, "base_color"});
    if (connected("normal")) roots.push_back({"normal", output->id, "normal"});
    if (connected("occlusion") || connected("roughness") || connected("metallic")) roots.push_back({"orm", output->id, "orm"});
    if (connected("emission")) roots.push_back({"emission", output->id, "emission"});
    if (connected("height")) roots.push_back({"height", output->id, "height"});
    return roots;
}

bool jasperengine::editor::procmat::planPasses(Expander& expander, const std::vector<PlanSource>& roots, const PlanOptions& options, PassPlan& plan, std::string& error) {
    plan = PassPlan();
    plan.size = options.size;
    plan.readBack = options.readBack;
    if (options.size < 1 || (options.size & (options.size - 1)) != 0) {
        error = "the size of a Procedural Material is a power of two";
        return false;
    }
    if (roots.empty()) {
        error = "nothing is connected to an output";
        return false;
    }
    try {
        Planner planner(expander, options, plan);
        for (const PlanSource& root : roots) planner.addOutput(root.name, root.source);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

bool jasperengine::editor::procmat::planGraph(const graph::Graph& file, const std::shared_ptr<const NodeLibrary>& library, const std::vector<PlanRoot>& requested,
                                              const PlanOptions& options, PassPlan& plan, std::string& error) {
    std::string cycleNode;
    if (findCycle(file, cycleNode)) {
        error = "node " + cycleNode + " is on a cycle";
        return false;
    }
    const std::vector<PlanRoot> roots = requested.empty() ? materialOutputRoots(file) : requested;
    if (roots.empty()) {
        error = "nothing is connected to the Material Output";
        return false;
    }
    Expander expander(library);
    expander.maxBufferSize = options.maxBufferSize;
    const Json& hostBuffer = hostBufferDescriptor();

    struct Built {
        const graph::Node* node = nullptr;
        NodeInstance instance;
        std::vector<graph::Port> ports;
    };
    std::map<std::string, Built> built;
    try {
        for (const graph::Node& node : file.nodes) {
            if (node.type == MATERIAL_OUTPUT_NODE) continue;
            expander.ownerNode = node.id;
            expander.seedOffset = seedOffsetOf(options.seed, node);
            Built item;
            item.node = &node;
            if (node.type == BUFFER_NODE) {
                const std::vector<std::string> problems = checkNodeParameters(hostBuffer, node, true);
                if (!problems.empty()) {
                    error = problems.front();
                    return false;
                }
                auto* buffer = expander.make<TextureSource>(node.id, generatorParameters(hostBuffer, node));
                item.instance.generator = buffer;
                item.instance.outputs.push_back(SourceRef{buffer, 0, false});
                InputTarget target;
                target.generator = buffer;
                target.port = 0;
                item.instance.inputs.push_back(std::move(target));
                graph::Port in, out;
                in.id = "in";
                in.direction = graph::PortDirection::In;
                out.id = "out";
                out.direction = graph::PortDirection::Out;
                item.ports = {in, out};
            } else {
                const Json* descriptor = library->descriptor(node.type);
                if (!descriptor || NodeLibrary::leftOutReason(node.type)) {
                    error = "node " + node.id + " has the type '" + node.type + "', which this build does not have";
                    return false;
                }
                const std::vector<std::string> problems = checkNodeParameters(*descriptor, node, true);
                if (!problems.empty()) {
                    error = problems.front();
                    return false;
                }
                const int genericSize = genericSizeOf(node);
                item.instance = expander.instantiate(node.type, generatorParameters(*descriptor, node), 0.0, genericSize);
                item.ports = nodePorts(*descriptor, genericSize);
            }
            built.emplace(node.id, std::move(item));
        }
        expander.seedOffset = 0.0;
        expander.ownerNode.clear();

        const auto sourceOf = [&](const graph::PortRef& from) -> std::optional<SourceRef> {
            auto owner = built.find(from.node);
            if (owner == built.end()) throw ExpansionError("an edge comes from node " + from.node + ", which it cannot use");
            const int port = portIndex(owner->second.ports, graph::PortDirection::Out, from.port);
            if (port < 0 || port >= (int)owner->second.instance.outputs.size()) {
                throw ExpansionError("node " + from.node + " has no output '" + from.port + "'");
            }
            return owner->second.instance.outputs[(size_t)port];
        };
        const graph::Node* output = outputNode(file);
        for (const graph::Edge& edge : file.edges) {
            if (output && edge.to.node == output->id) continue;
            auto target = built.find(edge.to.node);
            if (target == built.end()) throw ExpansionError("an edge goes to node " + edge.to.node + ", which it cannot use");
            const int port = portIndex(target->second.ports, graph::PortDirection::In, edge.to.port);
            if (port < 0 || port >= (int)target->second.instance.inputs.size()) {
                throw ExpansionError("node " + edge.to.node + " has no input '" + edge.to.port + "'");
            }
            const std::optional<SourceRef> source = sourceOf(edge.from);
            if (source) expander.connect(target->second.instance.inputs[(size_t)port], *source);
        }

        std::vector<PlanSource> sources;
        ShaderNode* orm = nullptr;
        const auto outputSource = [&](const std::string& port) -> std::optional<SourceRef> {
            for (const graph::Edge& edge : file.edges) {
                if (output && edge.to.node == output->id && edge.to.port == port) return sourceOf(edge.from);
            }
            return std::nullopt;
        };
        for (const PlanRoot& root : roots) {
            if (output && root.node == output->id && root.port == "orm") {
                if (!orm) {
                    expander.ownerNode = output->id;
                    orm = expander.make<ShaderNode>("orm", ormBody(), Json::object(), 0.0, 1);
                    expander.ownerNode.clear();
                    for (const char* port : {"occlusion", "roughness", "metallic"}) {
                        if (const std::optional<SourceRef> source = outputSource(port)) orm->setSource(orm->inputIndex(port), *source);
                    }
                }
                sources.push_back({root.name, SourceRef{orm, 0, false}});
                continue;
            }
            const std::optional<SourceRef> source = output && root.node == output->id ? outputSource(root.port) : sourceOf({root.node, root.port});
            if (!source) {
                error = "output " + root.name + " has nothing connected";
                return false;
            }
            sources.push_back({root.name, *source});
        }
        return planPasses(expander, sources, options, plan, error);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
