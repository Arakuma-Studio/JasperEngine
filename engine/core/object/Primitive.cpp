// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Primitive.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"

using namespace jasperengine;

Primitive::Primitive(Scene* scene): Mesh(scene){
}

Primitive::Primitive(Scene* scene, Entity entity): Mesh(scene, entity){
}

Primitive::~Primitive(){
}

Primitive::Primitive(const Primitive& rhs) = default;
Primitive& Primitive::operator=(const Primitive& rhs) = default;
Primitive::Primitive(Primitive&& rhs) noexcept = default;
Primitive& Primitive::operator=(Primitive&& rhs) noexcept = default;

void Primitive::createPlane(float width, float depth){
    scene->getSystem<MeshSystem>()->createPlane(getComponent<MeshComponent>(), width, depth);
}

void Primitive::createPlane(float width, float depth, unsigned int tiles){
    scene->getSystem<MeshSystem>()->createPlane(getComponent<MeshComponent>(), width, depth, tiles);
}

void Primitive::createWall(float width, float height){
    scene->getSystem<MeshSystem>()->createWall(getComponent<MeshComponent>(), width, height);
}

void Primitive::createWall(float width, float height, unsigned int tiles){
    scene->getSystem<MeshSystem>()->createWall(getComponent<MeshComponent>(), width, height, tiles);
}

void Primitive::createBox(float width, float height, float depth){
    scene->getSystem<MeshSystem>()->createBox(getComponent<MeshComponent>(), width, height, depth);
}

void Primitive::createBox(float width, float height, float depth, unsigned int tiles){
    scene->getSystem<MeshSystem>()->createBox(getComponent<MeshComponent>(), width, height, depth, tiles);
}

void Primitive::createSphere(float radius){
    scene->getSystem<MeshSystem>()->createSphere(getComponent<MeshComponent>(), radius);
}

void Primitive::createSphere(float radius, unsigned int slices, unsigned int stacks){
    scene->getSystem<MeshSystem>()->createSphere(getComponent<MeshComponent>(), radius, slices, stacks);
}

void Primitive::createCylinder(float radius, float height){
    scene->getSystem<MeshSystem>()->createCylinder(getComponent<MeshComponent>(), radius, radius, height);
}

void Primitive::createCylinder(float baseRadius, float topRadius, float height){
    scene->getSystem<MeshSystem>()->createCylinder(getComponent<MeshComponent>(), baseRadius, topRadius, height);
}

void Primitive::createCylinder(float radius, float height, unsigned int slices, unsigned int stacks){
    scene->getSystem<MeshSystem>()->createCylinder(getComponent<MeshComponent>(), radius, radius, height, slices, stacks);
}

void Primitive::createCylinder(float baseRadius, float topRadius, float height, unsigned int slices, unsigned int stacks){
    scene->getSystem<MeshSystem>()->createCylinder(getComponent<MeshComponent>(), baseRadius, topRadius, height, slices, stacks);
}

void Primitive::createCapsule(float radius, float height){
    scene->getSystem<MeshSystem>()->createCapsule(getComponent<MeshComponent>(), radius, radius, height);
}

void Primitive::createCapsule(float baseRadius, float topRadius, float height){
    scene->getSystem<MeshSystem>()->createCapsule(getComponent<MeshComponent>(), baseRadius, topRadius, height);
}

void Primitive::createCapsule(float radius, float height, unsigned int slices, unsigned int stacks){
    scene->getSystem<MeshSystem>()->createCapsule(getComponent<MeshComponent>(), radius, radius, height, slices, stacks);
}

void Primitive::createCapsule(float baseRadius, float topRadius, float height, unsigned int slices, unsigned int stacks){
    scene->getSystem<MeshSystem>()->createCapsule(getComponent<MeshComponent>(), baseRadius, topRadius, height, slices, stacks);
}

void Primitive::createTorus(float radius, float ringRadius){
    scene->getSystem<MeshSystem>()->createTorus(getComponent<MeshComponent>(), radius, ringRadius);
}

void Primitive::createTorus(float radius, float ringRadius, unsigned int sides, unsigned int rings){
    scene->getSystem<MeshSystem>()->createTorus(getComponent<MeshComponent>(), radius, ringRadius, sides, rings);
}