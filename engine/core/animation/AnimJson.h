// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (anim layers spec 5.12)
// - Purpose: the JSON plumbing the engine's JSON asset formats share (.anim, .mask,
//   .controller): the document parse with a line:col error, number and string writing in the
//   canonical forms, lenient number reads. Internal to engine/core/animation (not in
//   JasperEngine.h): it exposes nlohmann::ordered_json.
// - Thread-safety: pure functions.

#ifndef ANIM_JSON_H
#define ANIM_JSON_H

#include "animation/AnimationClipIO.h"
#include "animation/ClipTypes.h"
#include "json.hpp"

#include <string>
#include <vector>

namespace jasperengine::anim::json{

    using OJson = nlohmann::ordered_json;

    // `text` as a JSON document; false with `error` (line:col of the syntax error, "not valid
    // JSON: ...") when it is not one
    bool parseDocument(const std::string& text, OJson& root, ClipParseError& error);

    // the common header checks: an object whose "format" is `format` and whose "version" is a
    // whole number from 1 to `version`; false with `error.message` naming `what` ("an Avatar
    // Mask") or `kind` ("Avatar Mask": "unsupported Avatar Mask version 2") otherwise
    bool checkHeader(const OJson& root, const char* format, int version, const char* what, const char* kind, ClipParseError& error);

    // a JSON string literal
    std::string quote(const std::string& text);
    // %.6g, "-0" written as "0" (non-finite: "0")
    std::string number(float value);
    // seconds with at most 6 decimals, no trailing zeros
    std::string time(float value);
    // %.6g for messages
    std::string shortNumber(double value);
    // compact JSON (unknown keys written back)
    std::string compact(const OJson& value);

    // a JSON number as a finite float; false otherwise
    bool toFloat(const OJson& value, float& out);
    // a number, or true/false as 1/0
    bool toWeight(const OJson& value, float& out);

    void warn(std::vector<ClipWarning>* warnings, const std::string& path, const std::string& message);
    std::string member(const std::string& path, const std::string& name);
    std::string indexed(const std::string& path, const std::string& name, size_t index);

    // unknown top-level keys in file order (the last of a repeated name), as "  "key": value,\n"
    std::string unknownKeysText(const std::vector<std::pair<std::string, std::string>>& unknownKeys, const char* const* known, size_t knownCount);

    // a text starting (after a BOM and spaces) with "%YAML", or with `document` ("AvatarMask:") at
    // a line start: Unity's serialized form
    bool isUnityYaml(const std::string& text, const char* document);

}

#endif // ANIM_JSON_H
