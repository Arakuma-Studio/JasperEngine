// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The .navmesh format (docs/specs/S19-navigation.md R24, R39; AC26): a round trip answers the
// same queries; a newer, damaged or truncated file is refused with an error, never a crash.

#include "NavTestUtil.h"

#include "component/NavMeshLinkComponent.h"
#include "navigation/NavMesh.h"

#include <cstring>

using namespace jasperengine;
using namespace jasperengine::navtest;

namespace {

    class NavAssetTest : public NavSceneTest {};

    std::vector<uint8_t> bakedFile(NavSceneTest& test, Scene& scene, NavigationSystem& nav, bool carving) {
        plane(scene, Vector3(), 30.0f, 30.0f);
        box(scene, Vector3(0.0f, 1.0f, 0.0f), Vector3(4.0f, 2.0f, 8.0f));
        const Entity linkEntity = transformEntity(scene, Vector3(-8.0f, 0.0f, 8.0f), NULL_ENTITY, "Link");
        NavMeshLinkComponent link;
        link.startPoint = Vector3(0.0f, 0.0f, 0.0f);
        link.endPoint = Vector3(3.0f, 0.0f, 0.0f);
        link.costModifier = 4.0f;
        scene.addComponent<NavMeshLinkComponent>(linkEntity, link);
        const Entity surface = transformEntity(scene, Vector3(), NULL_ENTITY, "Surface");
        addSurface(scene, surface, 0.25f, 32).runtimeCarving = carving;
        resolveTransforms(scene);
        const NavBakeResult result = nav.bakeNow(surface, NavBakeMode::FULL);
        EXPECT_TRUE(result.ok) << result.error;
        (void)test;
        return nav.getNavMeshFile(surface);
    }

    void sameQueries(const nav::NavMesh& a, const nav::NavMesh& b) {
        const nav::NavFilter filter = nav::NavFilter::fromSettings(nav::ALL_AREAS);
        const Vector3 extents(1.0f, 2.0f, 1.0f);
        const Vector3 points[][2] = {{{-12, 0, -12}, {12, 0, 12}}, {{-12, 0, 0}, {12, 0, 0}}, {{-8, 0, 8}, {-5, 0, 8}}};
        for (const auto& pair : points) {
            const nav::NavPath pa = a.findPath(pair[0], pair[1], filter, extents);
            const nav::NavPath pb = b.findPath(pair[0], pair[1], filter, extents);
            EXPECT_EQ(pa.status, pb.status);
            ASSERT_EQ(pa.corners.size(), pb.corners.size());
            for (size_t i = 0; i < pa.corners.size(); i++)
                EXPECT_EQ(pa.corners[i], pb.corners[i]);
            const nav::NavHit sa = a.samplePosition(pair[0] + Vector3(0.3f, 1.0f, 0.2f), 2.0f, filter);
            const nav::NavHit sb = b.samplePosition(pair[0] + Vector3(0.3f, 1.0f, 0.2f), 2.0f, filter);
            EXPECT_EQ(sa.position, sb.position);
            const nav::NavHit ra = a.raycast(pair[0], pair[1], filter, extents);
            const nav::NavHit rb = b.raycast(pair[0], pair[1], filter, extents);
            EXPECT_EQ(ra.hit, rb.hit);
            EXPECT_EQ(ra.position, rb.position);
        }
    }

}

TEST_F(NavAssetTest, RoundTrip){
    for (bool carving : {false, true}) {
        scene = std::make_unique<Scene>();
        test::disableSubSystems(*scene);
        nav = scene->getSystem<NavigationSystem>().get();
        const std::vector<uint8_t> bytes = bakedFile(*this, *scene, *nav, carving);
        ASSERT_GT(bytes.size(), 64u);
        ASSERT_EQ(std::memcmp(bytes.data(), "JNAV", 4), 0);
        nav::NavMeshData data;
        std::string error;
        ASSERT_TRUE(nav::NavMeshFile::read(bytes.data(), bytes.size(), data, error)) << error;
        EXPECT_EQ(data.header.config.carving, carving);
        EXPECT_EQ(data.header.linkAreaCosts.size(), 1u);
        // the bytes again, the same queries again
        EXPECT_EQ(nav::NavMeshFile::write(data), bytes);
        nav::NavMesh a, b;
        nav::NavMeshData copy = data;
        ASSERT_TRUE(a.create(std::move(data), false, error)) << error;
        ASSERT_TRUE(b.create(std::move(copy), true, error)) << error;
        sameQueries(a, b);
        EXPECT_EQ(a.isCarving(), carving);
        EXPECT_GT(a.getPolyCount(), 0);
        EXPECT_EQ(a.getPolyCount(), b.getPolyCount());
    }
}

TEST_F(NavAssetTest, RejectNewerVersion){
    const std::vector<uint8_t> bytes = bakedFile(*this, *scene, *nav, false);
    nav::NavMeshData data;
    std::string error;
    std::vector<uint8_t> newer = bytes;
    newer[4] = (uint8_t)(nav::NavMeshFile::VERSION + 1);
    EXPECT_FALSE(nav::NavMeshFile::read(newer.data(), newer.size(), data, error));
    EXPECT_NE(error.find("newer"), std::string::npos) << error;

    // damaged anywhere, truncated at any length: refused, no crash
    for (size_t offset = 8; offset < bytes.size(); offset += std::max<size_t>(1, bytes.size() / 97)) {
        std::vector<uint8_t> damaged = bytes;
        damaged[offset] ^= 0xA5;
        EXPECT_FALSE(nav::NavMeshFile::read(damaged.data(), damaged.size(), data, error)) << offset;
    }
    for (size_t length = 0; length < bytes.size(); length += std::max<size_t>(1, bytes.size() / 61))
        EXPECT_FALSE(nav::NavMeshFile::read(bytes.data(), length, data, error)) << length;
    EXPECT_FALSE(nav::NavMeshFile::read(reinterpret_cast<const uint8_t*>("JNGRxxxxxxxxxxxxxxxx"), 20, data, error));

    // a surface whose asset is refused reports Not baked
    const Entity surface = transformEntity(*scene, Vector3(), NULL_ENTITY, "Broken");
    addSurface(*scene, surface).navMeshData = AssetRef("navigation/missing.navmesh");
    EXPECT_EQ(nav->getNavMesh(surface), nullptr);
    const NavSurfaceStatus status = nav->getSurfaceStatus(surface);
    EXPECT_EQ(status.state, NavBakeState::NOT_BAKED);
    EXPECT_FALSE(status.loadError.empty());
}

TEST_F(NavAssetTest, TileDataValidated){
    // a well-formed file carrying a tile Detour could not hold safely is refused at create
    const std::vector<uint8_t> bytes = bakedFile(*this, *scene, *nav, false);
    nav::NavMeshData data;
    std::string error;
    ASSERT_TRUE(nav::NavMeshFile::read(bytes.data(), bytes.size(), data, error)) << error;
    ASSERT_FALSE(data.tiles.empty());
    data.tiles[0].data.resize(data.tiles[0].data.size() - 4);
    nav::NavMesh mesh;
    EXPECT_FALSE(mesh.create(std::move(data), false, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(mesh.isValid());
}
