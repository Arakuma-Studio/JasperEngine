// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Contact2D.h"

#include "box2d/box2d.h"

using namespace jasperengine;

Contact2D::Contact2D(Scene* scene, b2ContactData contact){
    this->scene = scene;
    this->contact = contact;
}

Contact2D::~Contact2D() = default;
Contact2D::Contact2D(const Contact2D& rhs) = default;
Contact2D& Contact2D::operator=(const Contact2D& rhs) = default;

b2ContactData Contact2D::getBox2DContact() const{
    return contact;
}

Manifold2D Contact2D::getManifold() const{
    return Manifold2D(scene, &contact.manifold);
}

Entity Contact2D::getBodyEntityA() const{
    return reinterpret_cast<uintptr_t>(b2Body_GetUserData(b2Shape_GetBody(contact.shapeIdA)));
}

Rigidbody2D Contact2D::getBodyA() const{
    return Rigidbody2D(scene, getBodyEntityA());
}

size_t Contact2D::getColliderIndexA() const{
    return reinterpret_cast<size_t>(b2Shape_GetUserData(contact.shapeIdA));
}

Entity Contact2D::getBodyEntityB() const{
    return reinterpret_cast<uintptr_t>(b2Body_GetUserData(b2Shape_GetBody(contact.shapeIdB)));
}

Rigidbody2D Contact2D::getBodyB() const{
    return Rigidbody2D(scene, getBodyEntityB());
}

size_t Contact2D::getColliderIndexB() const{
    return reinterpret_cast<size_t>(b2Shape_GetUserData(contact.shapeIdB));
}
