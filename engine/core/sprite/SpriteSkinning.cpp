// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteSkinning.h"

#include "component/MeshComponent.h"
#include "component/SpriteSkinComponent.h"
#include "component/Transform.h"
#include "registry/EntityRegistry.h"
#include "math/Quaternion.h"
#include "util/Angle.h"

#include <algorithm>
#include <cmath>

namespace jasperengine{

    Vector2 spriteFrameLocalPoint(const SpriteFrame& frame, const SpriteFrameGeometryInput& input, Vector2 pixel){
        // a rect of no size at the pixel: its corner is where the quad draws that pixel
        const Rect at = spriteFrameLocalRect(frame, input, Rect(pixel.x, pixel.y, 0.0f, 0.0f));
        return Vector2(at.getX(), at.getY());
    }

    namespace{

        // the bone's head and its direction angle (radians) in the Sprite's local space
        void boneLocal(const SpriteFrame& frame, const SpriteSkinGeometry& skin, const SpriteFrameGeometryInput& input, int bone,
                       Vector2& head, float& angle){
            const SpriteBone& b = skin.bones[static_cast<size_t>(bone)];
            head = spriteFrameLocalPoint(frame, input, b.head);
            const Vector2 tail = spriteFrameLocalPoint(frame, input, b.tail);
            const Vector2 d = tail - head;
            angle = (d.x == 0.0f && d.y == 0.0f) ? 0.0f : std::atan2(d.y, d.x);
        }

        // as a Transform composes its model matrix (its rotation a quaternion about z), so a bone
        // GameObject at its rest pose gives an identity palette entry
        Matrix4 bindOf(const Vector2& head, float angle){
            return Matrix4::composeTRS(Vector3(head.x, head.y, 0.0f), Quaternion(0.0f, 0.0f, Angle::radToDefault(angle)), Vector3(1.0f, 1.0f, 1.0f));
        }

    }

    Matrix4 spriteBoneBindMatrix(const SpriteFrame& frame, const SpriteSkinGeometry& skin, const SpriteFrameGeometryInput& input, int bone){
        if (bone < 0 || bone >= static_cast<int>(skin.bones.size())) return Matrix4();
        Vector2 head;
        float angle = 0.0f;
        boneLocal(frame, skin, input, bone, head, angle);
        return bindOf(head, angle);
    }

    SpriteBoneRest spriteBoneRestTransform(const SpriteFrame& frame, const SpriteSkinGeometry& skin, const SpriteFrameGeometryInput& input, int bone){
        SpriteBoneRest rest;
        if (bone < 0 || bone >= static_cast<int>(skin.bones.size())) return rest;
        Vector2 head;
        float angle = 0.0f;
        boneLocal(frame, skin, input, bone, head, angle);
        const int parent = skin.bones[static_cast<size_t>(bone)].parent;
        if (parent < 0 || parent >= bone){
            rest.position = Vector3(head.x, head.y, 0.0f);
            rest.rotationZ = Angle::radToDefault(angle);
            return rest;
        }
        Vector2 parentHead;
        float parentAngle = 0.0f;
        boneLocal(frame, skin, input, parent, parentHead, parentAngle);
        // the head in the parent bone's frame
        const Vector2 d = head - parentHead;
        const float c = std::cos(-parentAngle), s = std::sin(-parentAngle);
        rest.position = Vector3(d.x * c - d.y * s, d.x * s + d.y * c, 0.0f);
        rest.rotationZ = Angle::radToDefault(angle - parentAngle);
        return rest;
    }

    SpriteSkinnedVertices spriteSkinnedVertices(const SpriteFrame& frame, const SpriteSkinGeometry& skin, const SpriteFrameGeometryInput& input){
        SpriteSkinnedVertices out;
        const float ox = frame.offset.x, oy = frame.offset.y;
        const float rw = frame.rect.getWidth() > 0.0f ? frame.rect.getWidth() : 1.0f;
        const float rh = frame.rect.getHeight() > 0.0f ? frame.rect.getHeight() : 1.0f;
        out.positions.reserve(skin.vertices.size());
        out.uvs.reserve(skin.vertices.size());
        for (const Vector2& v : skin.vertices){
            // inside the packed pixels: a vertex past them would sample the page's neighbours
            const float u = std::clamp((v.x - ox) / rw, 0.0f, 1.0f);
            const float t = std::clamp((v.y - oy) / rh, 0.0f, 1.0f);
            const Vector2 local = spriteFrameLocalPoint(frame, input, Vector2(ox + u * rw, oy + t * rh));
            out.positions.emplace_back(local.x, local.y, 0.0f);
            out.uvs.emplace_back(u, t);
        }
        const size_t bones = std::max<size_t>(skin.bones.size(), 1);
        out.inverseBindMatrices.resize(bones);
        out.boneBounds.assign(bones, AABB());
        for (size_t b = 0; b < skin.bones.size(); b++){
            out.inverseBindMatrices[b] = spriteBoneBindMatrix(frame, skin, input, static_cast<int>(b)).inverse();
        }
        // a vertex bounds every bone that moves it, in that bone's bind space (RenderSystem poses
        // each box by its bone's palette entry)
        for (size_t v = 0; v < out.positions.size(); v++){
            if (v >= skin.weights.size()){
                out.boneBounds[0].merge(out.inverseBindMatrices[0] * out.positions[v]);
                continue;
            }
            const SpriteVertexWeights& w = skin.weights[v];
            for (int k = 0; k < SPRITE_SKIN_MAX_INFLUENCES; k++){
                if (w.weights[k] <= 0.0f || w.bones[k] >= bones) continue;
                out.boneBounds[w.bones[k]].merge(out.inverseBindMatrices[w.bones[k]] * out.positions[v]);
            }
        }
        return out;
    }

    void updateSpriteSkinBones(EntityRegistry* registry){
        if (!registry->isComponentRegistered<SpriteSkinComponent>()) return;
        ComponentArray<SpriteSkinComponent>* skins = registry->getComponentArray<SpriteSkinComponent>();
        for (int i = 0; i < static_cast<int>(skins->size()); i++){
            SpriteSkinComponent& skin = skins->getComponentFromIndex(i);
            if (!skin.builtSkinned || skin.inverseBindMatrices.empty()) continue;
            const Entity entity = skins->getEntity(i);
            MeshComponent* mesh = registry->findComponent<MeshComponent>(entity);
            Transform* transform = registry->findComponent<Transform>(entity);
            if (!mesh || !transform) continue;

            bool dirty = skin.paletteDirty || transform->needUpdate || skin.paletteBones != skin.bones;
            for (size_t b = 0; b < skin.bones.size() && !dirty; b++){
                const Transform* bone = registry->isEntityCreated(skin.bones[b]) ? registry->findComponent<Transform>(skin.bones[b]) : nullptr;
                dirty = bone && bone->needUpdate;
            }
            if (!dirty) continue;
            const Matrix4 inverseSprite = transform->modelMatrix.inverse();
            // scale 0 makes it NaN: posing the bounds with it trips setExtents (as the model skins)
            if (!inverseSprite.isValid()) continue;

            // Flip X: the mirror on the left too (the inverse binds carry the right one)
            const Matrix4 front = skin.builtFlipX ? Matrix4::scaleMatrix(Vector3(-1.0f, 1.0f, 1.0f)) * inverseSprite : inverseSprite;
            const size_t count = skin.inverseBindMatrices.size();
            if (mesh->bonesMatrix.stored() != count) mesh->bonesMatrix.resize(count);
            for (size_t b = 0; b < count; b++){
                const Transform* bone = b < skin.bones.size() && registry->isEntityCreated(skin.bones[b])
                                        ? registry->findComponent<Transform>(skin.bones[b]) : nullptr;
                // a bone without its GameObject stays where it was bound
                mesh->bonesMatrix[b] = bone ? front * bone->modelMatrix * skin.inverseBindMatrices[b] : Matrix4();
            }
            skin.paletteDirty = false;
            skin.paletteBones = skin.bones;
            mesh->needUpdateBones = true;
            mesh->needUpdateAABB = true;
        }
    }

}
