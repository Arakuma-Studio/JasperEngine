// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TestPaths.h"

#include <cstdlib>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {
    TestPaths paths;
}

void jasperengine::test::setTestPaths(const TestPaths& newPaths){
    paths = newPaths;
}

const TestPaths& jasperengine::test::testPaths(){
    return paths;
}

bool jasperengine::test::envFlag(const char* name){
    const char* value = std::getenv(name);
    return value && std::strcmp(value, "1") == 0;
}
