// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What the Inspector's importers share (spec S53 R16, R17; S14's Texture Import Settings): the
// edits of a selection's import section before Apply (`touched` keys, `mixed` ones whose values
// differ between the selected files, shown with an em dash), the combo and checkbox rows, and the
// prompt for edits not applied yet ("Unapplied import settings for 'X'": Apply, Revert, Cancel)
// on a selection change, the editor's close and Play.

#pragma once

#include "imgui.h"
#include "yaml-cpp/yaml.h"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {

    namespace importer {
        // dotted keys of an import section ("lodCount", "platforms.mobile.override")
        YAML::Node get(const YAML::Node& root, const std::string& key);
        void set(YAML::Node& root, const std::string& key, const YAML::Node& value);   // a null value removes it
        void leaves(const YAML::Node& node, const std::string& prefix, std::set<std::string>& out);
        std::string dump(const YAML::Node& node);
    }

    // The working copy of the first selected file's section, with the edits of the selection
    class ImporterForm {
    public:
        YAML::Node working;
        std::set<std::string> touched;
        std::set<std::string> mixed;

        // the working section and the mixed keys of every selected file's section
        void reset(const std::vector<YAML::Node>& sections);
        YAML::Node value(const std::string& key) const { return importer::get(working, key); }
        void setValue(const std::string& key, const YAML::Node& value);
        // each selected section with the touched keys of the working one
        YAML::Node merged(const YAML::Node& section) const;

        // a two-column table row; true when it changed the value
        bool comboField(const char* label, const std::string& key, const std::vector<std::string>& names,
                        const std::vector<std::string>& labels, const std::string& shown, const char* tooltip);
        bool boolField(const char* label, const std::string& key, bool shown, const char* tooltip);
        bool intSlider(const char* label, const std::string& key, int shown, int low, int high, const char* tooltip);
        bool floatSlider(const char* label, const std::string& key, float shown, float low, float high, const char* format, const char* tooltip);
    };

    class ImporterPrompt {
    public:
        struct Pending {
            std::function<bool()> has;              // unapplied edits now
            std::function<std::string()> name;      // "Robot.glb", "3 models"
            std::function<void()> apply;
            std::function<void()> revert;
        };
        static void add(const void* owner, Pending pending);
        static void remove(const void* owner);
        static bool hasPending();

        // With unapplied edits: opens the prompt and returns true; `then` runs after Apply or
        // Revert, never after Cancel. Without: false (the caller goes on).
        static bool guard(std::function<void()> then);
        // the modal, once a frame (App)
        static void draw();
    };

}
