// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S90 R5: the C++ expander and tools/procmat/expand_nodes.py, the reference the compile sweep runs,
// write the same bytes and uniforms for every shader of the library. The reference's hashes are
// committed (ExpanderReference.txt, `expand_nodes.py --write-reference`), and
// tools/procmat/test_import_mm_nodes.py checks they are current.

#include "../../procmat/NodeLibrary.h"
#include "../../procmat/ProcMatExpander.h"
#include "ProcMatLibraryVariants.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

using namespace jasperengine::editor::procmat;
using namespace jasperengine::editor::procmat::test;

namespace {

    struct Row {
        std::string name;
        uint64_t text = 0;
        uint64_t uniforms = 0;
        std::string source;
    };

    std::shared_ptr<const NodeLibrary> repositoryLibrary() {
        std::string error;
        auto library = NodeLibrary::fromDirectory(std::filesystem::path(JASPER_TEST_REPO_DIR) / "editor/procmat/nodes", error);
        EXPECT_TRUE(library) << error;
        return library;
    }

    std::string slugOf(const std::string& text) {
        return std::regex_replace(text, std::regex("[^A-Za-z0-9_.=-]+"), "_");
    }

    Row rowOf(const std::string& name, const std::string& text, const PassBuilder& pass) {
        std::vector<float> floats;
        for (double value : pass.values) floats.push_back((float)value);
        return {slugOf(name), fnv1a(text.data(), text.size()), fnv1a(floats.data(), floats.size() * sizeof(float)), text};
    }

    void expandPasses(Expander& expander, const std::string& prefix, const SourceRef& root, std::vector<Row>& rows) {
        std::set<std::pair<Generator*, int>> done;
        std::vector<std::pair<std::string, SourceRef>> queue = {{"out", root}};
        while (!queue.empty()) {
            const auto [label, target] = queue.back();
            queue.pop_back();
            if (!done.insert({target.generator, target.port}).second) continue;
            PassBuilder pass;
            const std::string text = expander.shaderText(target, pass);
            rows.push_back(rowOf(prefix + "." + label, text, pass));
            for (const PassBuilder::TextureUse& use : pass.textures) {
                const std::string index = std::to_string(use.source->index());
                if (const auto source = use.source->getSource(0)) queue.push_back({"p" + index + "in", *source});
                if (use.source->kind() == TextureSource::Kind::Iterate) {
                    if (const auto loop = use.source->getSource(1)) queue.push_back({"p" + index + "loop", *loop});
                }
            }
        }
    }

    // Noise and blend nodes in one pass: a plausible graph of N nodes with no Buffer to cut it
    void expandChains(Expander& expander, std::vector<Row>& rows) {
        const char* noises[] = {"fbm4", "voronoi2", "perlin", "bricks3"};
        for (int size : {10, 30, 60, 120, 240}) {
            expander.reset();
            NodeInstance first = expander.instantiate("perlin", Json::object(), 0.1);
            SourceRef previous{first.generator, 0, false};
            int count = 1, step = 0;
            while (count < size) {
                NodeInstance noise = expander.instantiate(noises[step % 4], Json::object(), 0.01 * step);
                NodeInstance blend = expander.instantiate("blend2", Json::object(), 0.0, 1);
                auto* blendShader = static_cast<ShaderNode*>(blend.generator);
                blend.generator->setSource(blendShader->inputIndex("b"), previous);
                blend.generator->setSource(blendShader->inputIndex("l1"), SourceRef{noise.generator, 0, false});
                previous = SourceRef{blend.generator, 0, false};
                count += 2;
                step++;
            }
            PassBuilder pass;
            const std::string text = expander.shaderText(previous, pass);
            rows.push_back(rowOf("chain" + std::to_string(size), text, pass));
        }
    }

    std::vector<Row> expandLibrary(const std::shared_ptr<const NodeLibrary>& library) {
        std::vector<Row> rows;
        Expander expander(library);
        for (const auto& [nodeId, descriptor] : library->descriptors()) {
            for (const NodeVariant& variant : variantsOf(descriptor)) {
                expander.reset();
                NodeInstance instance = instantiateVariant(expander, nodeId, descriptor, variant);
                for (size_t port = 0; port < instance.outputs.size(); port++) {
                    const auto& output = instance.outputs[port];
                    if (!output || output->graphInput) continue;
                    expandPasses(expander, nodeId + "." + variant.name + ".o" + std::to_string(port), *output, rows);
                }
            }
        }
        expandChains(expander, rows);
        return rows;
    }

}

TEST(ProcMatExpander, AgreesWithThePythonReferenceOnTheLibrary) {
    const auto library = repositoryLibrary();
    ASSERT_TRUE(library);
    const std::vector<Row> rows = expandLibrary(library);

    std::ifstream file(std::filesystem::path(JASPER_TEST_REPO_DIR) / "editor/tests/procmat/ExpanderReference.txt");
    ASSERT_TRUE(file) << "no ExpanderReference.txt (tools/procmat/expand_nodes.py --write-reference)";
    std::vector<Row> reference;
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream fields(line);
        Row row;
        std::string text, uniforms;
        fields >> row.name >> text >> uniforms;
        row.text = std::stoull(text, nullptr, 16);
        row.uniforms = std::stoull(uniforms, nullptr, 16);
        reference.push_back(std::move(row));
    }
    ASSERT_GT(reference.size(), 1300u) << "the reference is cut short";
    EXPECT_EQ(rows.size(), reference.size());
    int differences = 0;
    for (size_t i = 0; i < std::min(rows.size(), reference.size()); i++) {
        if (rows[i].name != reference[i].name) {
            ADD_FAILURE() << "shader " << i << ": " << rows[i].name << " here, " << reference[i].name << " in the reference";
            break;
        }
        if (rows[i].text != reference[i].text) {
            if (differences++ < 5) {
                ADD_FAILURE() << rows[i].name << ": its text differs from the reference:\n" << rows[i].source;
            }
        } else if (rows[i].uniforms != reference[i].uniforms) {
            if (differences++ < 5) ADD_FAILURE() << rows[i].name << ": its uniforms differ from the reference";
        }
    }
    EXPECT_EQ(differences, 0) << "shaders that differ from the Python expander";
}

TEST(ProcMatExpander, ANodeTheLibraryLacksIsAnError) {
    const auto library = repositoryLibrary();
    ASSERT_TRUE(library);
    Expander expander(library);
    EXPECT_THROW(expander.instantiate("not_a_node", Json::object()), ExpansionError);
}

TEST(ProcMatExpander, AnInputReadAsATextureNeedsABuffer) {
    const auto library = repositoryLibrary();
    ASSERT_TRUE(library);
    std::string nodeId;
    for (const auto& [id, descriptor] : library->descriptors()) {
        const std::vector<bool> uses = usesTextureAttribute(descriptor, descriptor.value("inputs", Json::array()).size());
        if (std::find(uses.begin(), uses.end(), true) != uses.end() && descriptor.value("kind", std::string()) == "shader") {
            nodeId = id;
            break;
        }
    }
    ASSERT_FALSE(nodeId.empty()) << "the library has no node reading an input as a texture";
    Expander expander(library);
    NodeInstance node = expander.instantiate(nodeId, Json::object());
    NodeInstance source = expander.instantiate("perlin", Json::object());
    expander.connect(node.inputs.at(0), SourceRef{source.generator, 0, false});
    PassBuilder pass;
    EXPECT_THROW(expander.shaderText(*node.outputs.at(0), pass), ExpansionError);
}
