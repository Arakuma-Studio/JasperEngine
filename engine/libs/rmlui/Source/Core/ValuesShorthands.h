#pragma once

#include "../../Include/RmlUi/Core/PropertySpecification.h"

namespace Rml {

struct ShorthandDefinition;

// The Custom shorthands of the values package (phase 3, docs/html-ui/phase-3-fork-patches.md 3.3), registered by
// StyleSheetSpecification in its values section. Each sets every longhand of its definition, the omitted ones to their
// initial values, and fails without changing anything on an invalid value.

// font: [<style> || <variant> || <weight> || <stretch>]? <size> [/ <line-height>]? <family>, in ShorthandFont.cpp. The
// variant and the stretch take 'normal' only; system fonts (caption, menu, ...) are not supported.
bool ParseFontShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification);

} // namespace Rml
