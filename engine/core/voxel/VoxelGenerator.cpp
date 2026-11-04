// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelGenerator.h"

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

using namespace jasperengine::voxel;

namespace {

    class EmptyBase final : public BaseSource {
    public:
        void evaluate(const Int3&, int, int16_t* sdf, uint16_t* material) const override {
            std::fill_n(sdf, BLOCK_SAMPLES, SDF_AIR);
            std::fill_n(material, BLOCK_SAMPLES, static_cast<uint16_t>(0));
        }
        bool evaluateUniform(const Int3&, int, int16_t& sdf, uint16_t& material) const override {
            sdf = SDF_AIR;
            material = 0;
            return true;
        }
    };

    class SolidGroundBase final : public BaseSource {
    public:
        SolidGroundBase(double heightVoxels, int slot): height(heightVoxels), value(packMaterial(slot, slot, 0)) {}
        void evaluate(const Int3& origin, int spacing, int16_t* sdf, uint16_t* material) const override {
            for (int z = 0; z < BLOCK_SIZE; z++) {
                for (int y = 0; y < BLOCK_SIZE; y++) {
                    const int16_t encoded = encodeSdf(static_cast<double>(origin.y + y * spacing) - height);
                    for (int x = 0; x < BLOCK_SIZE; x++) {
                        const int index = sampleIndex(x, y, z);
                        sdf[index] = encoded;
                        material[index] = value;
                    }
                }
            }
        }
        // R34's clip in its plainest form: beyond the band a block has one value
        bool evaluateUniform(const Int3& origin, int spacing, int16_t& sdf, uint16_t& material) const override {
            const double low = static_cast<double>(origin.y) - height;
            const double high = static_cast<double>(origin.y + (BLOCK_SIZE - 1) * spacing) - height;
            if (low >= SDF_BAND_VOXELS) {
                sdf = SDF_AIR;
            } else if (high <= -SDF_BAND_VOXELS) {
                sdf = -SDF_MAX;
            } else {
                return false;
            }
            material = value;
            return true;
        }

    private:
        double height;
        uint16_t value;
    };

    class TerrainBase final : public BaseSource {
    public:
        TerrainBase(std::shared_ptr<const TerrainSource> source, const TerrainBaseOptions& opts): terrain(std::move(source)), options(opts) {}
        void evaluate(const Int3& origin, int spacing, int16_t* sdf, uint16_t* material) const override {
            const double voxel = options.voxelSize;
            for (int z = 0; z < BLOCK_SIZE; z++) {
                for (int x = 0; x < BLOCK_SIZE; x++) {
                    const float mx = static_cast<float>(static_cast<double>(origin.x + x * spacing) * voxel);
                    const float mz = static_cast<float>(static_cast<double>(origin.z + z * spacing) * voxel);
                    const double h = terrain->height(mx, mz);
                    float dhdx = 0.0f, dhdz = 0.0f;
                    terrain->gradient(mx, mz, dhdx, dhdz);
                    const double c = slopeFactor(dhdx, dhdz);
                    const TerrainLayerSample layers = terrain->layers(mx, mz);
                    const uint16_t value = packMaterial(layers.slot, layers.second, layers.secondWeight);
                    for (int y = 0; y < BLOCK_SIZE; y++) {
                        const double my = static_cast<double>(origin.y + y * spacing) * voxel;
                        const int index = sampleIndex(x, y, z);
                        sdf[index] = encodeSdf((my - h + options.overlapDepth) * c / voxel);
                        material[index] = value;
                    }
                }
            }
        }

    private:
        std::shared_ptr<const TerrainSource> terrain;
        TerrainBaseOptions options;
    };

    class BlendedBase final : public BaseSource {
    public:
        BlendedBase(std::shared_ptr<const BaseSource> otherBase, std::shared_ptr<const BaseSource> terrainBase, const Int3& dimensions, double width)
            : other(std::move(otherBase)), terrain(std::move(terrainBase)), dim(dimensions), blendWidth(width) {}
        void evaluate(const Int3& origin, int spacing, int16_t* sdf, uint16_t* material) const override {
            int16_t otherSdf[BLOCK_SAMPLES], terrainSdf[BLOCK_SAMPLES];
            uint16_t otherMaterial[BLOCK_SAMPLES], terrainMaterial[BLOCK_SAMPLES];
            other->evaluate(origin, spacing, otherSdf, otherMaterial);
            terrain->evaluate(origin, spacing, terrainSdf, terrainMaterial);
            for (int z = 0; z < BLOCK_SIZE; z++) {
                const int pz = origin.z + z * spacing;
                const int edgeZ = std::min(pz, dim.z - 1 - pz);
                for (int y = 0; y < BLOCK_SIZE; y++) {
                    for (int x = 0; x < BLOCK_SIZE; x++) {
                        const int px = origin.x + x * spacing;
                        const int edge = std::min(std::min(px, dim.x - 1 - px), edgeZ);
                        double t = blendWidth > 0.0 ? 1.0 - static_cast<double>(edge) / blendWidth : 1.0;
                        t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
                        const double b = t * t * (3.0 - 2.0 * t);
                        const int index = sampleIndex(x, y, z);
                        sdf[index] = encodeSdf(decodeSdf(otherSdf[index]) * (1.0 - b) + decodeSdf(terrainSdf[index]) * b);
                        material[index] = b >= 0.5 ? terrainMaterial[index] : otherMaterial[index];
                    }
                }
            }
        }

    private:
        std::shared_ptr<const BaseSource> other;
        std::shared_ptr<const BaseSource> terrain;
        Int3 dim;
        double blendWidth;
    };

    class FieldBase final : public BaseSource {
    public:
        FieldBase(std::shared_ptr<const StampField> sourceField, double voxel, int slot): field(std::move(sourceField)), voxelSize(voxel), value(packMaterial(slot, slot, 0)) {}
        void evaluate(const Int3& origin, int spacing, int16_t* sdf, uint16_t* material) const override {
            const double toField = voxelSize / field->voxelSize;
            for (int z = 0; z < BLOCK_SIZE; z++) {
                for (int y = 0; y < BLOCK_SIZE; y++) {
                    for (int x = 0; x < BLOCK_SIZE; x++) {
                        const int index = sampleIndex(x, y, z);
                        const int fx = static_cast<int>(std::floor((origin.x + x * spacing) * toField + 0.5));
                        const int fy = static_cast<int>(std::floor((origin.y + y * spacing) * toField + 0.5));
                        const int fz = static_cast<int>(std::floor((origin.z + z * spacing) * toField + 0.5));
                        material[index] = value;
                        if (fx < 0 || fy < 0 || fz < 0 || fx >= field->size.x || fy >= field->size.y || fz >= field->size.z) {
                            sdf[index] = SDF_AIR;
                            continue;
                        }
                        const int16_t stored = field->sdf[static_cast<size_t>(fx) + static_cast<size_t>(field->size.x) * (static_cast<size_t>(fy) + static_cast<size_t>(field->size.y) * static_cast<size_t>(fz))];
                        sdf[index] = encodeSdf(decodeSdf(stored) * field->voxelSize / voxelSize);
                    }
                }
            }
        }

    private:
        std::shared_ptr<const StampField> field;
        double voxelSize;
        uint16_t value;
    };

    class CarvedBase final : public BaseSource {
    public:
        CarvedBase(std::shared_ptr<const BaseSource> otherBase, std::vector<CarveCapsule> list): other(std::move(otherBase)), capsules(std::move(list)) {}
        void evaluate(const Int3& origin, int spacing, int16_t* sdf, uint16_t* material) const override {
            other->evaluate(origin, spacing, sdf, material);
            for (const CarveCapsule& capsule : capsules) {
                const Vec3d axis{capsule.to.x - capsule.from.x, capsule.to.y - capsule.from.y, capsule.to.z - capsule.from.z};
                const double lengthSq = axis.x * axis.x + axis.y * axis.y + axis.z * axis.z;
                for (int z = 0; z < BLOCK_SIZE; z++) {
                    for (int y = 0; y < BLOCK_SIZE; y++) {
                        for (int x = 0; x < BLOCK_SIZE; x++) {
                            const double px = origin.x + x * spacing - capsule.from.x;
                            const double py = origin.y + y * spacing - capsule.from.y;
                            const double pz = origin.z + z * spacing - capsule.from.z;
                            double h = lengthSq > 0.0 ? (px * axis.x + py * axis.y + pz * axis.z) / lengthSq : 0.0;
                            h = h < 0.0 ? 0.0 : (h > 1.0 ? 1.0 : h);
                            const double dx = px - axis.x * h, dy = py - axis.y * h, dz = pz - axis.z * h;
                            const double distance = std::sqrt(dx * dx + dy * dy + dz * dz) - capsule.radius;
                            const int index = sampleIndex(x, y, z);
                            const double carved = -distance;
                            const double current = decodeSdf(sdf[index]);
                            if (carved > current) sdf[index] = encodeSdf(carved);
                        }
                    }
                }
            }
        }

    private:
        std::shared_ptr<const BaseSource> other;
        std::vector<CarveCapsule> capsules;
    };

}

double jasperengine::voxel::slopeFactor(double dhdx, double dhdz) {
    return 1.0 / std::sqrt(1.0 + dhdx * dhdx + dhdz * dhdz);
}

std::shared_ptr<const BaseSource> jasperengine::voxel::makeEmptyBase() {
    return std::make_shared<EmptyBase>();
}

std::shared_ptr<const BaseSource> jasperengine::voxel::makeSolidGroundBase(double heightMetres, double voxelSize, int slot) {
    return std::make_shared<SolidGroundBase>(heightMetres / voxelSize, slot);
}

std::shared_ptr<const BaseSource> jasperengine::voxel::makeTerrainBase(std::shared_ptr<const TerrainSource> terrain, const TerrainBaseOptions& options) {
    return std::make_shared<TerrainBase>(std::move(terrain), options);
}

std::shared_ptr<const BaseSource> jasperengine::voxel::makeBlendedBase(std::shared_ptr<const BaseSource> other, std::shared_ptr<const BaseSource> terrain, const Int3& dimensions, double blendWidthVoxels) {
    return std::make_shared<BlendedBase>(std::move(other), std::move(terrain), dimensions, blendWidthVoxels);
}

std::shared_ptr<const BaseSource> jasperengine::voxel::makeFieldBase(std::shared_ptr<const StampField> field, double voxelSize, int slot) {
    return std::make_shared<FieldBase>(std::move(field), voxelSize, slot);
}

std::shared_ptr<const BaseSource> jasperengine::voxel::makeCarvedBase(std::shared_ptr<const BaseSource> other, std::vector<CarveCapsule> capsules) {
    return std::make_shared<CarvedBase>(std::move(other), std::move(capsules));
}

std::string jasperengine::voxel::generatorToJson(const GeneratorDesc& desc) {
    // nlohmann's map keeps keys sorted, so this is canonical (R74)
    nlohmann::json out;
    out["kind"] = desc.kind;
    if (!desc.sourceGuid.empty()) out["source_guid"] = desc.sourceGuid;
    if (!desc.sourcePath.empty()) out["source_path"] = desc.sourcePath;
    if (desc.sourceHash) out["source_hash"] = desc.sourceHash;
    if (desc.seed) out["seed"] = desc.seed;
    if (desc.height != 0.0) out["height"] = desc.height;
    if (desc.graphScale != 1.0) out["graph_scale"] = desc.graphScale;
    if (desc.blendWidth != 0.0) out["blend_width"] = desc.blendWidth;
    if (desc.overlapDepth != 0.0) out["overlap_depth"] = desc.overlapDepth;
    if (!desc.preset.empty()) out["preset"] = desc.preset;
    if (!desc.otherKind.empty()) out["other_kind"] = desc.otherKind;
    if (!desc.otherGuid.empty()) out["other_guid"] = desc.otherGuid;
    if (!desc.otherPath.empty()) out["other_path"] = desc.otherPath;
    return out.dump();
}

bool jasperengine::voxel::generatorFromJson(const std::string& json, GeneratorDesc& desc) {
    if (json.empty()) {
        desc = GeneratorDesc{};
        return true;
    }
    const nlohmann::json in = nlohmann::json::parse(json, nullptr, false);
    if (in.is_discarded() || !in.is_object()) return false;
    desc.kind = in.value("kind", std::string("empty"));
    desc.sourceGuid = in.value("source_guid", std::string());
    desc.sourcePath = in.value("source_path", std::string());
    desc.sourceHash = in.value("source_hash", static_cast<uint64_t>(0));
    desc.seed = in.value("seed", static_cast<uint64_t>(0));
    desc.height = in.value("height", 0.0);
    desc.graphScale = in.value("graph_scale", 1.0);
    desc.blendWidth = in.value("blend_width", 0.0);
    desc.overlapDepth = in.value("overlap_depth", 0.0);
    desc.preset = in.value("preset", std::string());
    desc.otherKind = in.value("other_kind", std::string());
    desc.otherGuid = in.value("other_guid", std::string());
    desc.otherPath = in.value("other_path", std::string());
    return true;
}
