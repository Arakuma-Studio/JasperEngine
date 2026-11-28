// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TestPaths.h"

#include <cstdlib>
#include <cstring>

using namespace jasperengine;

namespace jasperengine::test {

namespace {
    TestPaths paths;
}

void setTestPaths(const TestPaths& newPaths){
    paths = newPaths;
}

const TestPaths& testPaths(){
    return paths;
}

bool envFlag(const char* name){
    const char* value = std::getenv(name);
    return value && std::strcmp(value, "1") == 0;
}

}  // namespace jasperengine::test
