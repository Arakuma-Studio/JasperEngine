// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_EFFECTS_EFFECTREGISTRY_H
#define HTMLUI_EFFECTS_EFFECTREGISTRY_H

#include "EffectTypes.h"
#include "shader/ShaderBuildTypes.h"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui {

    // A built-in effect's manifest as the build carries it (BuiltinEffectManifests.gen.cpp,
    // written by tools/htmlui/gen_effect_manifests.py): the #version line, the manifest block
    // and the main include line of engine/shaderlib/effects/<name>.frag.
    struct BuiltinEffectSource {
        const char* path;       // "effects/<name>.frag"
        const char* manifest;
    };
    JASPERENGINE_API const BuiltinEffectSource* builtinEffectSources(size_t& count);

    // Every effect the game can use (phase-5 spec 3.6): the built-in ones of the engine library
    // and the project effects CSS declares with @effect. Names are global to the process; the
    // first declaration of a project effect wins. initialise() registers the RmlUi side (the
    // typed jfx-* decorator and filter instancers, their property parsers, the runtime plugin
    // and the fallback `effect` instancers); the manifests themselves are read on first use,
    // so the compatibility layer and the editor's checks use the registry without RmlUi.
    class JASPERENGINE_API EffectRegistry {
    public:
        using FileReader = std::function<bool(const std::string& assetPath, std::string& content)>;

        static EffectRegistry& get();

        // after Rml::Initialise (HtmlUI::acquire); idempotent
        void initialise();
        // after Rml::Shutdown (HtmlUI::release): RmlUi forgot the instancers and parsers. The
        // manifests and the project declarations stay
        void shutdown();
        bool isInitialised() const;

        const EffectManifest* find(std::string_view name) const;
        // builtin + project, sorted by name (internal passes are never listed)
        std::vector<const EffectManifest*> all() const;
        std::vector<const EffectManifest*> builtins() const;

        // @effect <name> { src: url(<fragPath>) }: reads the .frag (asset-relative) through
        // `read` (default: the assets), parses and checks its manifest, registers the effect's
        // instancers and its custom shader. Errors: effect-declare-conflict (a built-in name, or
        // another src), effect-file-missing, effect-manifest (at the .frag's line, also when the
        // manifest's @effect is not `name`). A repeated identical declaration is fine; one whose
        // earlier src no longer exists replaces it (p5-tooling: a .frag renamed and the CSS following).
        bool declareProjectEffect(const std::string& name, const std::string& fragPath, const std::string& declaredIn, int line,
                                  std::vector<EffectDiagnostic>& diags, const FileReader& read = FileReader());
        // A project file changed (hot reload): a declared effect's .frag is read again, its
        // instancers registered again (the old ones kept until shutdown: RmlUi may still use
        // them) and its shader variants dropped so they compile again. True when the file is a
        // declared effect, or one of its include files (only its variants are dropped then). A file
        // that is gone or whose manifest no longer parses forgets the
        // declaration (p5-tooling): the documents reloading next declare it again and report
        // why it fails (effect-file-missing, effect-manifest at the .frag line), and declare it
        // once it is back or fixed. UIDocumentSystem::reloadChangedFiles calls it for every
        // changed .frag a document reads, before the reload.
        bool onProjectFileChanged(const std::string& assetPath);
        // Forgets every declared project effect whose file no longer exists (renamed or deleted
        // while no document that reads it was loaded: a stopped play scene's) and drops the
        // compatibility layer's cached compiles, so documents declare their effects again;
        // UIDocumentSystem::reloadChangedFiles calls it first. The count forgotten
        size_t forgetMissingProjectEffects();
        // Called (outside the registry's lock) after onProjectFileChanged dropped a project
        // effect's variants: the editor drops its compiled cache of project shaders there
        using ProjectEffectListener = std::function<void(const std::string& assetPath)>;
        void setProjectEffectListener(ProjectEffectListener listener);
        // the files a declared project effect's includes resolve to or were looked for at (asset
        // paths, without the .frag): what a document declaring it depends on besides the .frag,
        // so an include edited reloads the documents and the effect (p5-tooling)
        std::vector<std::string> includeFilesOf(const std::string& effectName) const;
        // the declared project effects are forgotten (another project, tests)
        void clearProjectEffects();
        // Counts the changes of the declared project effects (declared from another file, read
        // again, forgotten; not a declaration repeated): elements keep the decorators and filters
        // of an effect value whose text does not change, so a document restyled across such a
        // change is loaded again in full (UIDocumentSystem::reloadChangedFiles; p5-tooling)
        uint64_t projectGeneration() const;

        // the variant of an effect's form: ShaderType::UI_EFFECT, bit 0 'Flt', the effect in
        // the custom-shader id (re-registered when ShaderPool's custom ids were reset)
        ShaderKey shaderKey(const EffectManifest& m, bool filterForm) const;
        // the key of the internal bright pass (@effects/_bright, filter form)
        ShaderKey brightPassKey() const;

        // for an unknown name: names within an edit distance of 3, and the effects whose
        // @suggest-for lists it (nearest first)
        std::vector<std::string> suggestions(std::string_view unknownName) const;
        // the effect of the other kind made for the same thing (sparkle -> glints): one that
        // lists the other's name in @suggest-for, or that the other lists; "" when none
        std::string counterpart(const EffectManifest& m) const;

        // what parsing the built-in manifests reported (empty unless the table is broken)
        const std::vector<EffectDiagnostic>& builtinDiagnostics() const;
        // manifests and instancers kept alive after a project effect was declared again
        size_t graveyardSize() const;
        // the project effect declared for a .frag, or nullptr (custom shader name resolver)
        const EffectManifest* findByCustomShader(const std::string& customShaderName) const;

    private:
        EffectRegistry();
        ~EffectRegistry();
        EffectRegistry(const EffectRegistry&) = delete;
        EffectRegistry& operator=(const EffectRegistry&) = delete;

        struct ProjectEffect {
            std::unique_ptr<EffectManifest> manifest;
            std::string fragPath;
            std::string declaredIn;
            int line = 0;
            std::string source;
            // the files its includes resolve to or were looked for at (asset paths, the compiler's
            // resolution: the .frag's directory, then the asset root): documents depend on them
            std::vector<std::string> includes;
        };
        std::vector<std::string> includeFilesOf(const std::string& fragPath, const FileReader& read) const;

        void ensureBuiltins() const;
        void registerRml(const EffectManifest& m);
        void installShaderResolvers();
        bool loadProjectManifest(const std::string& name, const std::string& fragPath, const std::string& source,
                                 std::vector<EffectDiagnostic>& diags, std::unique_ptr<EffectManifest>& out) const;

        ProjectEffectListener projectEffectListener;
        mutable std::recursive_mutex mutex;
        mutable bool builtinsLoaded = false;
        mutable std::vector<std::unique_ptr<EffectManifest>> builtinList;
        mutable std::vector<EffectDiagnostic> builtinDiags;
        std::map<std::string, ProjectEffect> projects;
        std::vector<std::unique_ptr<EffectManifest>> graveyard;
        // the custom shader id of each effect (key: custom name), valid for one generation
        mutable std::map<std::string, uint16_t> customIds;
        mutable uint32_t customIdGeneration = 0;
        uint64_t projectChanges = 0;
        bool initialised = false;
    };

}

#endif //HTMLUI_EFFECTS_EFFECTREGISTRY_H
