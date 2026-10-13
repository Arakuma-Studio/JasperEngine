#include "WillChange.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"

namespace Rml {
namespace WillChange {

	uint8_t ParseHints(const String& value)
	{
		struct NameHints {
			const char* name;
			uint8_t hints;
		};
		constexpr uint8_t Contains = StackingContext | FixedContainingBlock | AbsoluteContainingBlock;
		// Chromium 153 (ComputedStyle::HasWillChange*Hint, measured): the properties whose non-initial values create a
		// stacking context, and those that also make a containing block for positioned boxes.
		static const NameHints names[] = {
			{"transform", Contains},
			{"-webkit-transform", Contains},
			{"translate", Contains},
			{"rotate", Contains},
			{"scale", Contains},
			{"perspective", Contains},
			{"-webkit-perspective", Contains},
			{"transform-style", Contains},
			{"offset-path", Contains},
			{"offset-position", Contains},
			{"filter", Contains},
			{"-webkit-filter", Contains},
			{"backdrop-filter", Contains},
			{"contain", Contains},
			{"position", StackingContext | AbsoluteContainingBlock},
			{"opacity", StackingContext},
			{"mask", StackingContext},
			{"mask-image", StackingContext},
			{"-webkit-mask", StackingContext},
			{"-webkit-mask-image", StackingContext},
			{"-webkit-mask-box-image", StackingContext},
			{"clip-path", StackingContext},
			{"-webkit-clip-path", StackingContext},
			{"isolation", StackingContext},
			{"mix-blend-mode", StackingContext},
			{"view-transition-name", StackingContext},
		};

		uint8_t hints = 0;
		StringList items;
		StringUtilities::ExpandString(items, value, ',');
		for (const String& item : items)
		{
			const String name = StringUtilities::ToLower(StringUtilities::StripWhitespace(item));
			for (const NameHints& entry : names)
			{
				if (name == entry.name)
				{
					hints |= entry.hints;
					break;
				}
			}
		}
		return hints;
	}

	bool IsFixedPositioningContainingBlock(const Style::ComputedValues& computed)
	{
		return computed.has_local_transform() || computed.has_local_perspective() || computed.has_filter() || computed.has_backdrop_filter() ||
			(computed.will_change() & FixedContainingBlock) != 0;
	}

	bool IsAbsolutePositioningContainingBlock(const Style::ComputedValues& computed)
	{
		return computed.position() != Style::Position::Static || IsFixedPositioningContainingBlock(computed) ||
			(computed.will_change() & AbsoluteContainingBlock) != 0;
	}

} // namespace WillChange
} // namespace Rml
