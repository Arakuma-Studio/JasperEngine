// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Layered PSD import (docs/specs/S94-rendering-anim-gaps.md F48, AC45): a document written by the
// Title Art PSD writer (three pixel layers, two of them in a group, and a hidden one) gives three
// sprites named after the layers and keyed by their layer ids, packed trimmed into a page, and a
// prefab with the group as a parent, each Sprite at its layer's place (within 0.5 px) and the
// stack order as Order in Layer. A reimport of the edited document (a layer renamed, one added on
// top) keeps each layer's key and prefab GameObject id.

#include <gtest/gtest.h>

#include "texture/PsdLayerImport.h"
#include "titleart/interchange/psd/PsdWriter.h"

#include "Catalog.h"

#include <cmath>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    constexpr int kDocW = 64, kDocH = 48;

    psd::PsdWriteLayer solidLayer(const std::string& name, int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
        psd::PsdWriteLayer layer;
        layer.name = name;
        layer.x = x;
        layer.y = y;
        layer.width = w;
        layer.height = h;
        layer.rgba.resize(static_cast<size_t>(w) * h * 4);
        for (size_t i = 0; i < layer.rgba.size(); i += 4) {
            layer.rgba[i] = r;
            layer.rgba[i + 1] = g;
            layer.rgba[i + 2] = b;
            layer.rgba[i + 3] = 255;
        }
        return layer;
    }

    // bottom first: Body, then the group Head (Face, Hat), then a hidden layer
    psd::PsdWriteDocument character(const std::string& hatName, bool extraOnTop, bool psb = false) {
        psd::PsdWriteDocument doc;
        doc.psb = psb;
        doc.width = kDocW;
        doc.height = kDocH;
        doc.layers.push_back(solidLayer("Body", 20, 20, 24, 24, 200, 40, 40));
        psd::PsdWriteLayer head;
        head.name = "Head";
        head.group = true;
        head.blendKey = "pass";
        head.children.push_back(solidLayer("Face", 24, 4, 16, 16, 240, 200, 150));
        head.children.push_back(solidLayer(hatName, 22, 0, 20, 6, 30, 30, 200));
        doc.layers.push_back(head);
        psd::PsdWriteLayer hidden = solidLayer("Ghost", 0, 0, 8, 8, 0, 255, 0);
        hidden.visible = false;
        doc.layers.push_back(hidden);
        if (extraOnTop) doc.layers.push_back(solidLayer("Sword", 50, 30, 6, 16, 180, 180, 190));
        doc.merged.assign(static_cast<size_t>(kDocW) * kDocH * 4, 0);
        return doc;
    }

    std::vector<uint8_t> bytesOf(const psd::PsdWriteDocument& doc) {
        std::vector<uint8_t> bytes;
        std::string error;
        EXPECT_TRUE(psd::writePsd(doc, bytes, error)) << error;
        return bytes;
    }

    // every GameObject of the prefab by name: its entity id, parent name and components
    struct PrefabObject {
        Entity entity = 0;
        std::string parent;
        YAML::Node components;
    };
    void collect(const YAML::Node& node, const std::string& parent, std::map<std::string, PrefabObject>& out) {
        PrefabObject object;
        object.entity = node["entity"].as<Entity>();
        object.parent = parent;
        object.components = node["components"];
        const std::string name = node["name"].as<std::string>();
        out[name] = object;
        if (node["children"]) {
            for (const YAML::Node& child : node["children"]) collect(child, name, out);
        }
    }

    const PsdLayerNode* findNode(const std::vector<PsdLayerNode>& nodes, const std::string& name) {
        for (const PsdLayerNode& node : nodes) {
            if (node.name == name) return &node;
            if (const PsdLayerNode* found = findNode(node.children, name)) return found;
        }
        return nullptr;
    }

}

TEST(PsdLayerImportTest, LayersBecomeSprites) {
    const PsdLayerImportResult result = importPsdLayers(bytesOf(character("Hat", false)), "art/hero.psd", 1, {});
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.width, kDocW);
    EXPECT_EQ(result.height, kDocH);
    ASSERT_EQ(result.sheet.frames.size(), 3u) << "three visible pixel layers; the hidden one and the group are no sprites";
    ASSERT_EQ(result.pages.size(), result.sheet.pages.size());
    std::map<std::string, const SpriteFrame*> frames;
    for (const SpriteFrame& frame : result.sheet.frames) frames[frame.name] = &frame;
    ASSERT_TRUE(frames.count("Body") && frames.count("Face") && frames.count("Hat"));
    EXPECT_EQ(frames["Body"]->sourceSize.x, 24.0f);
    EXPECT_EQ(frames["Hat"]->sourceSize.y, 6.0f);
    for (const SpriteFrame& frame : result.sheet.frames) {
        EXPECT_EQ(frame.key.rfind("layer:", 0), 0u) << frame.key;
        // the page holds the layer's colour where the frame is
        const editor::pixelart::PixelImage& page = result.pages[frame.page];
        const editor::pixelart::PixelRgba pixel = page.at(static_cast<int>(frame.rect.getX()) + 1, static_cast<int>(frame.rect.getY()) + 1);
        EXPECT_EQ(pixel[3], 255) << frame.name;
    }
    EXPECT_EQ(result.pages[frames["Body"]->page].at(static_cast<int>(frames["Body"]->rect.getX()) + 2, static_cast<int>(frames["Body"]->rect.getY()) + 2)[0], 200);

    // the tree: Hat above Face in the group, the group above Body; layer centres from the document's
    const PsdLayerNode* body = findNode(result.roots, "Body");
    const PsdLayerNode* hat = findNode(result.roots, "Hat");
    ASSERT_TRUE(body && hat);
    EXPECT_NEAR(body->x, (32.0f - kDocW / 2.0f), 0.5f);
    EXPECT_NEAR(body->y, (kDocH / 2.0f - 32.0f), 0.5f);
    EXPECT_NEAR(hat->x, (32.0f - kDocW / 2.0f), 0.5f);
    EXPECT_NEAR(hat->y, (kDocH / 2.0f - 3.0f), 0.5f);
    EXPECT_LT(body->order, hat->order) << "stack order is Order in Layer";

    const YAML::Node prefab = psdLayerPrefab(result, "art/hero.psd", Guid(), Guid::derive(Guid(), "hero"), false);
    EXPECT_EQ(prefab["type"].as<std::string>(), "Prefab");
    std::map<std::string, PrefabObject> objects;
    collect(prefab["members"][0], "", objects);
    ASSERT_TRUE(objects.count("Head") && objects.count("Face") && objects.count("Hat") && objects.count("Body"));
    EXPECT_FALSE(objects.count("Ghost"));
    EXPECT_EQ(objects["Face"].parent, "Head");
    EXPECT_EQ(objects["Hat"].parent, "Head");
    EXPECT_EQ(objects["Body"].parent, "hero");
    const std::string spriteKey = Catalog::getComponentName(ComponentType::SpriteComponent, true);
    const std::string meshKey = Catalog::getComponentName(ComponentType::MeshComponent, true);
    const std::string transformKey = Catalog::getComponentName(ComponentType::Transform, true);
    const YAML::Node hatSprite = objects["Hat"].components[spriteKey];
    ASSERT_TRUE(hatSprite) << YAML::Dump(objects["Hat"].components);
    EXPECT_NE(YAML::Dump(hatSprite).find("art/hero.psd"), std::string::npos);
    EXPECT_EQ(hatSprite["frameName"].as<std::string>(), "Hat");
    const YAML::Node hatPosition = objects["Hat"].components[transformKey]["position"];
    EXPECT_NEAR(hatPosition[0].as<float>(), hat->x, 0.5f);
    EXPECT_NEAR(hatPosition[1].as<float>(), hat->y, 0.5f);
    const int bodyOrder = objects["Body"].components[meshKey]["sortingOrder"] ? objects["Body"].components[meshKey]["sortingOrder"].as<int>() : 0;
    ASSERT_TRUE(objects["Hat"].components[meshKey]["sortingOrder"]) << YAML::Dump(objects["Hat"].components[meshKey]);
    EXPECT_GT(objects["Hat"].components[meshKey]["sortingOrder"].as<int>(), bodyOrder);

    // a reimport: Hat renamed Cap, a Sword added on top
    const PsdLayerImportResult again = importPsdLayers(bytesOf(character("Cap", true)), "art/hero.psd", 1, {});
    ASSERT_TRUE(again.ok) << again.error;
    ASSERT_EQ(again.sheet.frames.size(), 4u);
    std::map<std::string, std::string> keys, keysAgain;
    for (const SpriteFrame& frame : result.sheet.frames) keys[frame.name] = frame.key;
    for (const SpriteFrame& frame : again.sheet.frames) keysAgain[frame.name] = frame.key;
    EXPECT_EQ(keysAgain["Body"], keys["Body"]);
    EXPECT_EQ(keysAgain["Face"], keys["Face"]);
    EXPECT_EQ(keysAgain["Cap"], keys["Hat"]) << "a renamed layer keeps its sprite's key";
    std::map<std::string, PrefabObject> objectsAgain;
    collect(psdLayerPrefab(again, "art/hero.psd", Guid(), Guid::derive(Guid(), "hero"), false)["members"][0], "", objectsAgain);
    EXPECT_EQ(objectsAgain["Cap"].entity, objects["Hat"].entity) << "the renamed layer keeps its GameObject id";
    EXPECT_EQ(objectsAgain["Body"].entity, objects["Body"].entity);
}

TEST(PsdLayerImportTest, PsbLayersBecomeTheSameSprites) {
    // a Large Document (version 2) of the same layers imports as the PSD does
    const PsdLayerImportResult psdResult = importPsdLayers(bytesOf(character("Hat", false)), "art/hero.psb", 1, {});
    const std::vector<uint8_t> bytes = bytesOf(character("Hat", false, true));
    ASSERT_GE(bytes.size(), 6u);
    EXPECT_EQ(bytes[5], 2) << "the writer made a PSB";
    const PsdLayerImportResult psbResult = importPsdLayers(bytes, "art/hero.psb", 1, {});
    ASSERT_TRUE(psbResult.ok) << psbResult.error;
    ASSERT_EQ(psbResult.sheet.frames.size(), psdResult.sheet.frames.size());
    for (size_t i = 0; i < psbResult.sheet.frames.size(); i++) {
        EXPECT_EQ(psbResult.sheet.frames[i].name, psdResult.sheet.frames[i].name);
        EXPECT_EQ(psbResult.sheet.frames[i].key, psdResult.sheet.frames[i].key);
        EXPECT_EQ(psbResult.sheet.frames[i].sourceSize.x, psdResult.sheet.frames[i].sourceSize.x);
    }
    ASSERT_EQ(psbResult.pages.size(), psdResult.pages.size());
    EXPECT_TRUE(psbResult.pages[0] == psdResult.pages[0]) << "the same pixels";
}

TEST(PsdLayerImportTest, CharacterRigBindsLayersToBones) {
    // the Hat bound at 100% to one bone, the Body to another: the prefab's skeleton, each Sprite Skin bound
    sprite::SpriteSkinSet skins;
    auto quad = [](float w, float h, const std::string& bone, Vector2 head, Vector2 tail) {
        auto skin = std::make_shared<SpriteSkinGeometry>();
        skin->bones.push_back({bone, -1, head, tail});
        skin->vertices = {Vector2(0, 0), Vector2(w, 0), Vector2(w, h), Vector2(0, h)};
        skin->indices = {0, 1, 2, 0, 2, 3};
        SpriteVertexWeights rigid;
        rigid.weights[0] = 1.0f;
        skin->weights.assign(4, rigid);
        return skin;
    };
    skins.sprites["Hat"] = quad(20, 6, "hat", Vector2(10, 6), Vector2(10, 0));
    skins.sprites["Body"] = quad(24, 24, "spine", Vector2(12, 24), Vector2(12, 0));
    const PsdLayerImportResult result = importPsdLayers(bytesOf(character("Hat", false)), "art/hero.psd", 1, skins);
    ASSERT_TRUE(result.ok) << result.error;
    std::map<std::string, PrefabObject> objects;
    collect(psdLayerPrefab(result, "art/hero.psd", Guid(), Guid::derive(Guid(), "hero"), true)["members"][0], "", objects);
    ASSERT_TRUE(objects.count("hat") && objects.count("spine")) << "the bones are GameObjects of the prefab";
    const std::string skinKey = Catalog::getComponentName(ComponentType::SpriteSkinComponent, true);
    const YAML::Node hatSkin = objects["Hat"].components[skinKey];
    ASSERT_TRUE(hatSkin) << YAML::Dump(objects["Hat"].components);
    ASSERT_TRUE(hatSkin["bones"]);
    EXPECT_EQ(hatSkin["bones"][0].as<Entity>(), objects["hat"].entity);
    EXPECT_FALSE(objects["Face"].components[skinKey]) << "a layer without a skin has no Sprite Skin";
    // the hat bone's head at the hat layer's bottom centre, in units from the document's centre
    const YAML::Node position = objects["hat"].components[Catalog::getComponentName(ComponentType::Transform, true)]["position"];
    EXPECT_NEAR(position[0].as<float>(), 32.0f - kDocW / 2.0f, 0.5f);
    EXPECT_NEAR(position[1].as<float>(), kDocH / 2.0f - 6.0f, 0.5f);
}
