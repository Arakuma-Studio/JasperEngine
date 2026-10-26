#pragma once

#include "../../../Include/RmlUi/Core/Types.h"
#include <unordered_map>

namespace Rml {

/**
    See the CSS glossary for terms used in the layout engine:
    https://www.w3.org/TR/css-display-3/#glossary
 */
class LayoutEngine {
public:
	/// Formats the contents for a root-level element, usually a document, or a replaced element with custom formatting.
	/// @param[in] element The element to lay out.
	/// @param[in] containing_block The size of the containing block.
	static void FormatElement(Element* element, Vector2f containing_block);

	/// Fork (P7-L): formats a layout boundary (Element::SetLayoutBoundary) and its descendants again in the box it has,
	/// in place: its size and offset are its parent's, which did not change.
	static void FormatLayoutBoundary(Element* element);
};

/**
    The intrinsic measurements taken during one LayoutEngine::FormatElement call (LayoutDetails::GetMinContentWidth,
    GetMaxContentWidth, GetContentHeightForWidth), so that flex and grid measuring does not format the same element again
    for the same question: every measurement is a full format of the element. Keyed by element, kind, containing block
    and width; cleared when FormatElement starts. Outside of FormatElement nothing is cached.
 */
class LayoutMeasureCache {
public:
	enum class Kind : uint8_t {
		MinContentWidth,
		MaxContentWidth,
		HeightForWidth,
		// Content-based sizes (LayoutDetails::GetContentWidth, GetContentHeight; p3-layout).
		ContentMinWidth,
		ContentMaxWidth,
		ContentHeight,
	};

	/// The cache of the FormatElement call in progress, or nullptr.
	static LayoutMeasureCache* Get();

	bool Find(Element* element, Kind kind, Vector2f containing_block, float width, float& result) const;
	void Insert(Element* element, Kind kind, Vector2f containing_block, float width, float result);
	size_t Size() const { return entries.size(); }

	/// Makes a cache current for its lifetime (FormatElement), restoring the previous one after.
	class Scope {
	public:
		Scope();
		~Scope();
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;
		LayoutMeasureCache& GetCache() { return *cache; }

	private:
		UniquePtr<LayoutMeasureCache> cache;
		LayoutMeasureCache* previous;
	};

private:
	struct Key {
		Element* element;
		Kind kind;
		uint32_t containing_block_x, containing_block_y, width;
		bool operator==(const Key& other) const
		{
			return element == other.element && kind == other.kind && containing_block_x == other.containing_block_x &&
				containing_block_y == other.containing_block_y && width == other.width;
		}
	};
	struct KeyHash {
		size_t operator()(const Key& key) const;
	};
	static Key MakeKey(Element* element, Kind kind, Vector2f containing_block, float width);

	std::unordered_map<Key, float, KeyHash> entries;
};

} // namespace Rml
