#include "CppBullet.h"

using namespace jasperengine;

int CppBullet::constructed = 0;
int CppBullet::destroyed = 0;

CppBullet::CppBullet(Scene* scene, Entity entity): ScriptBase(scene, entity) {
    constructed++;
}

CppBullet::~CppBullet() {
    destroyed++;
}
