#include "LayoutEngine.h"
#include "../../../Include/RmlUi/Core/Element.h"
#include "../../../Include/RmlUi/Core/Log.h"
#include "../../../Include/RmlUi/Core/Math.h"
#include "../../../Include/RmlUi/Core/Profiling.h"
#include "ContainerBox.h"
#include "FormattingContext.h"
#include <string.h>

namespace Rml {

static LayoutMeasureCache* current_measure_cache = nullptr;

LayoutMeasureCache* LayoutMeasureCache::Get()
{
	return current_measure_cache;
}

LayoutMeasureCache::Scope::Scope() : cache(MakeUnique<LayoutMeasureCache>()), previous(current_measure_cache)
{
	current_measure_cache = cache.get();
}

LayoutMeasureCache::Scope::~Scope()
{
	current_measure_cache = previous;
}

LayoutMeasureCache::Key LayoutMeasureCache::MakeKey(Element* element, Kind kind, Vector2f containing_block, float width)
{
	auto Bits = [](float value) {
		uint32_t bits;
		memcpy(&bits, &value, sizeof(bits));
		return bits;
	};
	return Key{element, kind, Bits(containing_block.x), Bits(containing_block.y), Bits(width)};
}

size_t LayoutMeasureCache::KeyHash::operator()(const Key& key) const
{
	size_t seed = std::hash<const void*>()(key.element);
	for (uint32_t value : {uint32_t(key.kind), key.containing_block_x, key.containing_block_y, key.width})
		seed ^= std::hash<uint32_t>()(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
	return seed;
}

bool LayoutMeasureCache::Find(Element* element, Kind kind, Vector2f containing_block, float width, float& result) const
{
	const auto it = entries.find(MakeKey(element, kind, containing_block, width));
	if (it == entries.end())
		return false;
	result = it->second;
	return true;
}

void LayoutMeasureCache::Insert(Element* element, Kind kind, Vector2f containing_block, float width, float result)
{
	entries[MakeKey(element, kind, containing_block, width)] = result;
}

void LayoutEngine::FormatElement(Element* element, Vector2f containing_block)
{
	RMLUI_ASSERT(element && containing_block.x >= 0 && containing_block.y >= 0);

	// Intrinsic measurements are kept for the duration of this layout.
	LayoutMeasureCache::Scope measure_cache;

	RootBox root(containing_block);

	auto layout_box = FormattingContext::FormatIndependent(&root, element, nullptr, FormattingContextType::Block);
	if (!layout_box)
	{
		Log::Message(Log::LT_ERROR, "Error while formatting element: %s", element->GetAddress().c_str());
	}

	{
		RMLUI_ZoneScopedN("ClampScrollOffsetRecursive");
		// The size of the scrollable area might have changed, so clamp the scroll offset to avoid scrolling outside the
		// scrollable area. During layouting, we might be changing the scrollable overflow area of the element several
		// times, such as after enabling scrollbars. For this reason, we don't clamp the scroll offset during layouting,
		// as that could inadvertently clamp it to a temporary size. Now that we know the final layout, including the
		// size of each element's scrollable area, we can finally clamp the scroll offset.
		element->ClampScrollOffsetRecursive();
	}
}

void LayoutEngine::FormatLayoutBoundary(Element* element)
{
	RMLUI_ASSERT(element);
	RMLUI_ZoneScoped;

	LayoutMeasureCache::Scope measure_cache;

	// The containing block of its percentages (min-height and the like, resolved against the box's own size otherwise)
	// as its parent's formatting saw it: the parent's content box.
	Vector2f containing_block(0.f);
	if (Element* parent = element->GetParentNode())
		containing_block = Math::Max(parent->GetBox().GetSize(BoxArea::Content), Vector2f(0.f));
	RootBox root(containing_block);

	const Box box = element->GetBox();
	auto layout_box = FormattingContext::FormatIndependent(&root, element, &box, FormattingContextType::Block);
	if (!layout_box)
		Log::Message(Log::LT_ERROR, "Error while formatting layout boundary: %s", element->GetAddress().c_str());

	element->ClampScrollOffsetRecursive();
}

} // namespace Rml
