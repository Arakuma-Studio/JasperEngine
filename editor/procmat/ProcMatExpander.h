// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PROCMAT_EXPANDER_H
#define PROCMAT_EXPANDER_H

// The code generator of S90 R5: node descriptors become fragment shaders by Material Maker's macro
// semantics (gen_shader.gd as a behaviour reference), one shader per pass, with stable graph-local
// names (o<n>) and functions resolved by id. tools/procmat/expand_nodes.py is the reference the
// compile sweep runs: ProcMatExpanderTest pins both to the same bytes over the whole library, so a
// change here is a change there. Deliberate differences from Material Maker are in the spec's
// Deviations ("Rules the C++ expander must keep").

#include "NodeLibrary.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jasperengine::editor::procmat {

    class ExpansionError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    class Generator;

    // An input is read as .texture or .size and its source is inlined: only a Buffer has those (R6)
    class NeedsBufferError : public ExpansionError {
    public:
        NeedsBufferError(const std::string& message, Generator* consumer, int input)
            : ExpansionError(message), consumer(consumer), input(input) {}
        Generator* consumer;
        int input;
    };

    class Expander;

    // Which coordinates a generator was already expanded at, per function scope: a value computed
    // once is reused, and the input functions see their caller's names through the parent.
    class ExpansionContext {
    public:
        explicit ExpansionContext(ExpansionContext* parent = nullptr) : parent(parent) {}

        bool hasVariant(const Generator* generator) const;
        void touch(const Generator* generator);
        // the variant's index, -1 the first time (and it is then recorded)
        int getVariant(const Generator* generator, const std::string& variant);

    private:
        ExpansionContext* parent;
        std::unordered_map<const Generator*, std::vector<std::string>> variants;
    };

    struct ExpandedCode {
        std::set<std::string> functions;
        std::string defs;
        std::string code;
        std::map<std::string, std::string> outputValues;
        std::string outputType;
        std::shared_ptr<ExpandedCode> alias;

        void merge(const ExpandedCode& other);
    };

    // A place an output feeds: a generator's input, or an interface (group) of a graph node
    struct SourceRef {
        Generator* generator = nullptr;
        int port = 0;
        // a graph's input wired straight to its output: nothing to expand
        bool graphInput = false;
    };

    class TextureSource;

    // The state of one generated shader: its uniform block, textures and what was inlined in it,
    // which the planner reads to decide where to cut (R6).
    class PassBuilder {
    public:
        struct TextureUse {
            std::string name;
            TextureSource* source = nullptr;
            int port = 0;
        };

        // An input whose source was inlined here, with what the source cost in this pass
        struct InlineEdge {
            Generator* consumer = nullptr;
            int input = 0;
            size_t bytes = 0;
            size_t uniformFloats = 0;
            size_t textures = 0;
        };

        std::vector<double> values;       // the block's floats, four to a vec4
        std::vector<TextureUse> textures;
        std::vector<InlineEdge> edges;
        // distinct coordinates each output was expanded at
        std::map<std::pair<const Generator*, int>, int> samples;
        // inputs a consumer's function calls inside a loop of its own
        std::set<std::pair<Generator*, int>> loopedInputs;
        // the graph nodes whose code this pass holds
        std::set<std::string> nodes;

        std::string floatSlot(const std::string& key, double value);
        std::string vec4Slot(const std::string& key, const double value[4]);
        std::string texture(const std::string& name, TextureSource* source, int port);

    private:
        std::unordered_map<std::string, std::string> slots;
    };

    class Generator {
    public:
        Generator(Expander& expander, std::string name);
        virtual ~Generator() = default;

        std::optional<SourceRef> getSource(int index) const;
        void setSource(int index, const SourceRef& source);
        void clearSource(int index);

        virtual std::string outputType(int port) const = 0;
        // the Buffer attributes .texture and .size read; false for anything inlined
        virtual bool getOutputAttribute(int port, const std::string& attribute, std::string& value);
        // what a Buffer in front of it would not save anything for
        virtual bool isTrivial() const { return false; }

        ExpandedCode shaderCode(const std::string& uv, int port, ExpansionContext& context);
        virtual ExpandedCode generate(const std::string& uv, int port, ExpansionContext& context) = 0;

        int index() const { return nodeIndex; }
        const std::string& genName() const { return genname; }
        const std::string& name() const { return label; }
        // the graph node this belongs to, for what a report names
        std::string graphNode;

    protected:
        Expander& expander;

    private:
        int nodeIndex;
        std::string genname;
        std::string label;
        std::map<int, SourceRef> sources;
    };

    // A Buffer as its consumers see it: a texture sample, where inlining stops
    class TextureSource : public Generator {
    public:
        enum class Kind { Buffer, Iterate };

        TextureSource(Expander& expander, std::string name, Json parameters, Kind kind = Kind::Buffer);

        std::string outputType(int port) const override;
        bool getOutputAttribute(int port, const std::string& attribute, std::string& value) override;
        bool isTrivial() const override { return true; }
        ExpandedCode generate(const std::string& uv, int port, ExpansionContext& context) override;

        Kind kind() const { return passKind; }
        bool isGreyscale() const;
        int size() const { return bufferSize; }
        int iterations() const;
        bool shrink() const;
        // a consumer samples it with a level of detail: box-filtered mips (R11)
        bool needsMips() const { return mips; }
        void setNeedsMips() { mips = true; }

    private:
        std::string textureName(int port) const;

        Json parameters;
        Kind passKind;
        bool filter;
        int bufferSize;
        bool mips = false;
    };

    class SwitchNode : public Generator {
    public:
        SwitchNode(Expander& expander, std::string name, Json parameters);

        std::string outputType(int port) const override;
        ExpandedCode generate(const std::string& uv, int port, ExpansionContext& context) override;
        int outputCount() const;
        int inputCount() const;

    private:
        std::optional<SourceRef> forward(int port) const;
        Json parameters;
    };

    class ShaderNode : public Generator {
    public:
        ShaderNode(Expander& expander, std::string name, const Json& body, const Json& parameters, double seed, int genericSize);

        std::string outputType(int port) const override;
        ExpandedCode generate(const std::string& uv, int port, ExpansionContext& context) override;

        int inputIndex(const std::string& name) const;
        int inputCount() const { return (int)inputs.size(); }
        int outputCount() const { return (int)outputs.size(); }
        const std::vector<Json>& inputDefinitions() const { return inputs; }
        const std::vector<Json>& outputDefinitions() const { return outputs; }

        // a graph's parameters with `named: true`: name -> (slot key, value)
        std::vector<std::pair<std::string, std::pair<std::string, double>>> namedParameters;

    private:
        struct Variable {
            bool isInput = false;
            int index = 0;
            std::string text;
        };
        using Variables = std::unordered_map<std::string, Variable>;

        void processParameters(ExpandedCode& rv, Variables& variables, bool declare);
        std::string randomMacros(std::string text);
        std::string gradient(const std::string& prefix, const Json& value, ExpandedCode& rv, bool declare);
        std::string curve(const std::string& prefix, const Json& value, ExpandedCode& rv, bool declare);
        void pixels(const std::string& prefix, const Json& value, ExpandedCode& rv, bool declare, std::string& function, std::string& size);
        void generateInputFunction(int index, const Json& definition, ExpandedCode& rv, ExpansionContext& context);
        std::string replaceInput(int index, const std::string& suffix, const std::string& parameters, Variables& variables, ExpandedCode& rv, ExpansionContext& context);
        std::string replaceVariables(std::string text, Variables& variables, ExpandedCode& rv, ExpansionContext& context);

        int genericSize;
        double seed;
        std::vector<std::string> functions;
        std::string instance;
        std::string code;
        std::vector<Json> inputs;
        std::vector<Json> outputs;
        std::vector<Json> parameterDefs;
        Json parameters;
    };

    // A node once instantiated: its generator when it is one, and its ports. An input of a graph
    // node forwards to every inner input wired to it.
    struct InputTarget {
        Generator* generator = nullptr;
        int port = 0;
        std::vector<InputTarget> group;
        bool isGroup = false;
    };

    struct NodeInstance {
        Generator* generator = nullptr;
        std::vector<std::optional<SourceRef>> outputs;
        std::vector<InputTarget> inputs;
    };

    class Expander {
    public:
        explicit Expander(std::shared_ptr<const NodeLibrary> library);

        // Forgets every generator, so names start again from o1
        void reset();

        NodeInstance instantiate(const std::string& descriptorId, const Json& parameters, double seed = 0.0, int genericSize = 1);
        void connect(const InputTarget& target, const SourceRef& source);
        // A generator made by a caller (a planner's Buffer, a test's probe)
        template<class T, class... Args>
        T* make(Args&&... args) {
            auto owned = std::make_unique<T>(*this, std::forward<Args>(args)...);
            T* raw = owned.get();
            generators.push_back(std::move(owned));
            return raw;
        }

        // The whole fragment shader of one output, filling `pass`
        std::string shaderText(const SourceRef& root, PassBuilder& pass);

        int nextIndex() { return ++counter; }
        void noteFunction(const Generator* generator, const std::string& function);
        const std::set<std::string>* extraFunctions(const Generator* generator) const;
        const NodeLibrary& nodes() const { return *library; }
        const Json& portType(const std::string& name) const;
        PassBuilder& currentPass() const;

        // the largest size of a Buffer, 0 for none: a preview's cap on what a graph asks for
        int maxBufferSize = 0;
        // the graph node being instantiated: what its generators report as theirs
        std::string ownerNode;
        // added to the seed of the shaders instantiated from now on: a graph's reroll reaches the ones
        // inside a library graph too, as a uniform, so it costs no compile
        double seedOffset = 0.0;

    private:
        NodeInstance instantiateGraph(const std::string& owner, const Json& graph, const Json& parameters);
        NodeInstance childEntry(Generator* generator);
        std::vector<std::string> functionClosure(const std::set<std::string>& ids) const;

        std::shared_ptr<const NodeLibrary> library;
        std::vector<std::unique_ptr<Generator>> generators;
        std::unordered_map<const Generator*, std::set<std::string>> extra;
        int counter = 0;
        PassBuilder* pass = nullptr;
    };

    // The names ports of a node type go by, in port order: what edges name in a file
    std::vector<Json> expandedInputs(const Json& descriptor, int genericSize);
    std::vector<Json> expandedOutputs(const Json& descriptor, int genericSize);
    std::vector<Json> expandedParameters(const Json& descriptor, int genericSize);
    bool isGenericDescriptor(const Json& descriptor);
    std::vector<std::string> outputPortIds(const std::vector<Json>& outputs);

}

#endif
