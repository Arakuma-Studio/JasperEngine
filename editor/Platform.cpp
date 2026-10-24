// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Platform.h"
#include "Out.h"
#include "Backend.h"
#include "EditorHost.h"
#include "imgui.h"

#include <cstdarg>
#include <cstdio>
#include <string>

using namespace jasperengine;

int editor::Platform::width = 0;
int editor::Platform::height = 0;

editor::Platform::Platform(Project* project) : System() {
    this->project = project;
}

bool editor::Platform::setSizes(int width, int height){
    if (editor::Platform::width != width || editor::Platform::height != height){
        editor::Platform::width = width;
        editor::Platform::height = height;

        return true;
    }

    return false;
}

int editor::Platform::getScreenWidth(){
    return width;
}

int editor::Platform::getScreenHeight(){
    return height;
}

std::string editor::Platform::getAssetPath(){
    return project->getAssetsPath().string();
}

std::string editor::Platform::getLuaPath(){
    return project->getLuaPath().string();
}

std::string editor::Platform::getShaderPath(){
    // Compiled shaders are a build output, not a referenced asset
    return (project->getProjectPath() / "shaders").string();
}

std::string editor::Platform::getUserDataPath(){
    return project->getUserDataPath().string();
}

std::string editor::Platform::getImportPath(){
    return project->getImportPath().string();
}

sg_environment editor::Platform::getSokolEnvironment(){
    #if defined(SOKOL_VULKAN) || defined(SOKOL_METAL)
    return Backend::getSokolEnvironment();
    #else
    return System::getSokolEnvironment();
    #endif
}

sg_swapchain editor::Platform::getSokolSwapchain(){
    #if defined(SOKOL_VULKAN) || defined(SOKOL_METAL)
    return Backend::getSokolSwapchain();
    #else
    return System::getSokolSwapchain();
    #endif
}

bool editor::Platform::isTouchDevice(){
    return project->isSimulateTouch();
}

void editor::Platform::setClipboardText(const std::string& utf8){
    if (ImGui::GetCurrentContext()){
        ImGui::SetClipboardText(utf8.c_str());
    }else{
        System::setClipboardText(utf8);
    }
}

std::string editor::Platform::getClipboardText(){
    if (ImGui::GetCurrentContext()){
        const char* text = ImGui::GetClipboardText();
        return text ? std::string(text) : std::string();
    }
    return System::getClipboardText();
}

void editor::Platform::setMouseMode(MouseMode mode){
    Backend::setMouseMode(mode);
}

void editor::Platform::quit(){
    // stops play instead of closing the editor, deferred to not stop inside a script call
    editor::getEditorHost().enqueueMainThreadTask([this]() {
        project->stopActivePlay();
    });
}

