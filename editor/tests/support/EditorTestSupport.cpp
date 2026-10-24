// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EditorTestSupport.h"

#include "DocumentFile.h"
#include "Engine.h"
#include "Stream.h"
#include "System.h"

#include <fstream>
#include <iterator>
#include <mutex>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;

void editor::test::ensureEngine() {
    // engine/tests/support/TestMain.cpp starts the engine before any test, with the support
    // library's TestSystem (headless, logs captured); a second Engine::systemInit with another
    // System would leave that capture reading the wrong object
}

void editor::test::releaseCameraFramebuffers(Scene* scene) {
    if (!scene)
        return;
    ComponentArray<CameraComponent>* cameras = scene->getComponentArray<CameraComponent>();
    for (size_t i = 0; i < cameras->size(); i++) {
        CameraComponent& camera = cameras->getComponentFromIndex(i);
        delete camera.framebuffer;
        camera.framebuffer = nullptr;
    }
}

fs::path editor::test::fixture(const std::string& relative) {
    return fs::path(JASPER_TEST_FIXTURES_DIR) / relative;
}

std::string editor::test::readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

editor::test::DecodedScene::~DecodedScene() {
    if (scene) {
        releaseCameraFramebuffers(scene->scene);
        delete scene->scene;
        scene->scene = nullptr;
    }
}

namespace {

    editor::test::DecodedScene decodeNode(YAML::Node root, const std::string& name, LoadReport report) {
        editor::test::DecodedScene decoded;
        decoded.scene = std::make_unique<SceneProject>();
        decoded.report = std::move(report);
        {
            ScopedLoadReport scope(decoded.report);
            Stream::decodeSceneProject(decoded.scene.get(), root, true);
            Stream::decodeSceneProjectEntities(nullptr, decoded.scene.get(), root);
        }
        (void)name;
        return decoded;
    }

}

editor::test::DecodedScene editor::test::decodeSceneFile(const fs::path& path) {
    ensureEngine();
    LoadReport report;
    report.documentPath = path.filename().string();
    YAML::Node root;
    std::string sha1;
    {
        ScopedLoadReport scope(report);
        DocumentFile document = DocumentFiles::read(path, DocumentKind::Scene);
        root = document.root;
        sha1 = document.sha1;
    }
    DecodedScene decoded = decodeNode(root, report.documentPath, std::move(report));
    decoded.sha1 = sha1;
    return decoded;
}

editor::test::DecodedScene editor::test::decodeSceneText(const std::string& text, const std::string& name) {
    ensureEngine();
    LoadReport report;
    report.documentPath = name;
    YAML::Node root;
    {
        ScopedLoadReport scope(report);
        root = DocumentFiles::parse(text, name);
        DocumentFiles::recordVersionIssues(DocumentKind::Scene, StreamMigrations::migrate(DocumentKind::Scene, root));
    }
    DecodedScene decoded = decodeNode(root, name, std::move(report));
    decoded.sha1 = DocumentFiles::sha1(text);
    return decoded;
}

YAML::Node editor::test::encodeScene(const DecodedScene& decoded) {
    return Stream::encodeSceneProject(nullptr, decoded.scene.get());
}

std::string editor::test::encodeSceneText(const DecodedScene& decoded) {
    return DocumentFiles::text(encodeScene(decoded));
}

bool editor::test::structurallyEqual(const YAML::Node& a, const YAML::Node& b, std::string* why) {
    auto fail = [why](const std::string& text) {
        if (why) *why = text;
        return false;
    };
    if (a.Type() != b.Type()) {
        return fail("type differs: " + YAML::Dump(a) + " vs " + YAML::Dump(b));
    }
    if (a.IsScalar()) {
        return a.Scalar() == b.Scalar() ? true : fail("\"" + a.Scalar() + "\" vs \"" + b.Scalar() + "\"");
    }
    if (a.IsSequence()) {
        if (a.size() != b.size()) return fail("sequence sizes " + std::to_string(a.size()) + " vs " + std::to_string(b.size()));
        for (size_t i = 0; i < a.size(); i++) {
            std::string inner;
            if (!structurallyEqual(a[i], b[i], &inner)) return fail("[" + std::to_string(i) + "] " + inner);
        }
        return true;
    }
    if (a.IsMap()) {
        if (a.size() != b.size()) {
            std::string keys;
            for (auto it = a.begin(); it != a.end(); ++it) if (!b[it->first.Scalar()]) keys += " -" + it->first.Scalar();
            for (auto it = b.begin(); it != b.end(); ++it) if (!a[it->first.Scalar()]) keys += " +" + it->first.Scalar();
            return fail("map keys differ:" + keys);
        }
        for (auto it = a.begin(); it != a.end(); ++it) {
            const std::string key = it->first.Scalar();
            const YAML::Node other = b[key];
            if (!other) return fail("missing key " + key);
            std::string inner;
            if (!structurallyEqual(it->second, other, &inner)) return fail(key + ": " + inner);
        }
        return true;
    }
    return true;
}

YAML::Node editor::test::withoutKey(const YAML::Node& map, const std::string& key) {
    YAML::Node copy(YAML::NodeType::Map);
    for (auto it = map.begin(); it != map.end(); ++it) {
        if (it->first.Scalar() != key) copy[it->first] = it->second;
    }
    return copy;
}
