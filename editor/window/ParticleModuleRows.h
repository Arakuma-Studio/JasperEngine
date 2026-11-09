// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The module rows of a particle effect (docs/specs/S20-particles.md R27, S91 R22), shared by the
// Inspector (a Particle System, a .particles asset) and the Particle Effect window. They come
// from the module registry's field tables (ParticleModules), so a field is declared once for the
// codec, MCP and these rows. A value that is not its default is marked; right-click on its label
// resets it. Each module's header has a help link and a menu: Reset, Copy Module, Paste Module
// (the clipboard's module, added when absent) and Paste Values (onto the same module, its
// enabled state kept). The rows draw (InspectorParticles.cpp); the document operations are here,
// without ImGui (ParticleModuleRows.cpp), so tests and tools take the same path.

#include "ecs/Entity.h"
#include "particles/ParticleEffect.h"
#include "particles/ParticleModules.h"

#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class CommandHistory;
    class Project;

    namespace particlerows {

        // What a frame of the rows did: a value changed (a step to push, merging) and an edit ended
        struct EffectRows {
            Project* project = nullptr;
            // where a field window's steps go (the scene's history, the project's for a file)
            CommandHistory* history = nullptr;
            std::set<std::string>* expanded = nullptr;
            // the rows' ImGui ids, and the UI probe's names ("<prefix>.<field path>")
            std::string prefix;
            bool changed = false;
            bool commit = false;
            // a curve field opened the Curve window this frame: its path and driver
            bool curveOpened = false;
            std::string curveField;
            particles::CurveDriver curveDriver = particles::CurveDriver::NormalizedAge;
            // S49: the grid a pixel particle draws on here (the Pixel Art module's status line), the
            // inspected system (Bake to Sprite Sheet), and a project step this edit wrote (a
            // preset's palette copy: linked to the edit's step)
            std::string gridStatus;
            uint32_t sceneId = 0;
            Entity entity = NULL_ENTITY;
            bool wroteProjectStep = false;
        };

        // The whole effect: Main always open, a foldout per present module, Add Module, the
        // exposed parameters' definitions. Edits go into `edited`.
        void drawEffect(EffectRows& rows, particles::ParticleEffectDocument& edited);
        void drawDiagnostics(const std::vector<particles::Diagnostic>& diagnostics);

        // ---- the document operations of the marks and menus
        // the fields that are not their defaults: "effect.<key>" for the effect's own, "<module>.<key>"
        std::set<std::string> nonDefaultFields(const particles::ParticleEffectDocument& document);
        // one of those back to its default; false for a path that names no such field
        bool resetField(particles::ParticleEffectDocument& document, const std::string& path);
        // every field of the module back to its default; present and enabled stay
        bool resetModule(particles::ParticleEffectDocument& document, particles::ModuleId module);
        // the module as clipboard text (JSON: {"particleModule": key, "values": {...}})
        std::string copyModule(const particles::ParticleEffectDocument& document, particles::ModuleId module);
        // the module the clipboard text holds, if any
        const particles::ModuleDesc* clipboardModule(const std::string& text);
        // Paste Module: the clipboard's module as copied (added when absent, its enabled state too).
        // Paste Values (valuesOnly): onto that module only when present, its enabled state kept.
        bool pasteModule(particles::ParticleEffectDocument& document, const std::string& text, bool valuesOnly, std::string* error = nullptr);
        // whether a curve's time axis is the emitter's (a field read over the emitter time, or the
        // Emitter Time driver): the Particle Effect window's playhead means something on it
        bool curveOverEmitterTime(const std::string& fieldPath, particles::CurveDriver driver);
        // the module's section of docs/editor/particle-effect.md
        std::string moduleHelpUrl(const particles::ModuleDesc& module);

    }

}
