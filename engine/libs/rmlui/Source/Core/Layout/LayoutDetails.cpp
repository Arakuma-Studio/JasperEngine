#include "LayoutDetails.h"
#include "../../../Include/RmlUi/Core/FontMetrics.h"
#include "../../../Include/RmlUi/Core/ComputedValues.h"
#include "../../../Include/RmlUi/Core/Element.h"
#include "../../../Include/RmlUi/Core/ElementDocument.h"
#include "../../../Include/RmlUi/Core/ElementScroll.h"
#include "../../../Include/RmlUi/Core/ElementText.h"
#include "../../../Include/RmlUi/Core/Math.h"
#include "../../../Include/RmlUi/Core/Profiling.h"
#include "../../../Include/RmlUi/Core/StringUtilities.h"
#include "BlockContainer.h"
#include "ContainerBox.h"
#include "FormattingContext.h"
#include "LayoutEngine.h"
#include "ReplacedFormattingContext.h"
#include <algorithm>
#include <float.h>

namespace Rml {

bool LayoutDetails::IsCollapsibleWhiteSpaceText(Element* element)
{
	ElementText* text = rmlui_dynamic_cast<ElementText*>(element);
	if (!text)
		return false;
	const Style::WhiteSpace white_space = element->GetComputedValues().white_space();
	if (white_space == Style::WhiteSpace::Pre || white_space == Style::WhiteSpace::Prewrap)
		return false;
	const String& value = text->GetText();
	return std::all_of(value.begin(), value.end(), &StringUtilities::IsWhitespace);
}

static Vector<UniquePtr<LayoutChildList>> layout_child_list_pool;
static size_t layout_child_lists_borrowed = 0;

ScopedLayoutChildList::ScopedLayoutChildList()
{
	if (layout_child_lists_borrowed == layout_child_list_pool.size())
		layout_child_list_pool.push_back(MakeUnique<LayoutChildList>());
	list = layout_child_list_pool[layout_child_lists_borrowed++].get();
	list->clear();
}

ScopedLayoutChildList::~ScopedLayoutChildList()
{
	RMLUI_ASSERT(layout_child_lists_borrowed > 0 && layout_child_list_pool[layout_child_lists_borrowed - 1].get() == list);
	layout_child_lists_borrowed--;
}

void LayoutDetails::AppendLayoutChildren(Element* parent, LayoutChildList& children)
{
	// Reads the children and their 'display: contents' flag directly: this runs for every box of every layout.
	auto Append = [&children](Element* child) {
		if (child->display_contents)
			AppendLayoutChildren(child, children);
		else
			children.push_back(child);
	};

	if (Element* marker = parent->GetPseudoElement(PseudoElementType::Marker))
		Append(marker);
	if (Element* before = parent->GetPseudoElement(PseudoElementType::Before))
		Append(before);
	const size_t num_dom_children = parent->children.size() - (size_t)parent->num_non_dom_children;
	for (size_t i = 0; i < num_dom_children; i++)
		Append(parent->children[i].get());
	if (Element* after = parent->GetPseudoElement(PseudoElementType::After))
		Append(after);
}

void LayoutDetails::GetLayoutChildren(Element* parent, LayoutChildList& children, bool apply_order)
{
	children.clear();
	AppendLayoutChildren(parent, children);

	if (apply_order)
	{
		const bool any_order = std::any_of(children.begin(), children.end(), [](Element* child) { return child->GetComputedValues().order() != 0; });
		if (any_order)
		{
			std::stable_sort(children.begin(), children.end(),
				[](Element* a, Element* b) { return a->GetComputedValues().order() < b->GetComputedValues().order(); });
		}
	}
}

namespace {
	// The element whose sizing keywords are being resolved: measuring it builds its box again, where they count as 'auto'.
	const Element* resolving_sizing_keywords_of = nullptr;
} // namespace

// Convert width or height of a border box to the width or height of its corresponding content box.
static inline float BorderSizeToContentSize(float border_size, float border_padding_edges_size)
{
	if (border_size < 0.0f || border_size >= FLT_MAX)
		return border_size;

	return Math::Max(0.0f, border_size - border_padding_edges_size);
}

void LayoutDetails::BuildBox(Box& box, Vector2f containing_block, Element* element, BuildBoxMode box_context)
{
	if (!element)
	{
		box.SetContent(containing_block);
		return;
	}

	const ComputedValues& computed = element->GetComputedValues();

	// Calculate the padding area.
	box.SetEdge(BoxArea::Padding, BoxEdge::Top, Math::Max(0.0f, ResolveValue(computed.padding_top(), containing_block.x)));
	box.SetEdge(BoxArea::Padding, BoxEdge::Right, Math::Max(0.0f, ResolveValue(computed.padding_right(), containing_block.x)));
	box.SetEdge(BoxArea::Padding, BoxEdge::Bottom, Math::Max(0.0f, ResolveValue(computed.padding_bottom(), containing_block.x)));
	box.SetEdge(BoxArea::Padding, BoxEdge::Left, Math::Max(0.0f, ResolveValue(computed.padding_left(), containing_block.x)));

	// Calculate the border area.
	box.SetEdge(BoxArea::Border, BoxEdge::Top, Math::Max(0.0f, computed.border_top_width()));
	box.SetEdge(BoxArea::Border, BoxEdge::Right, Math::Max(0.0f, computed.border_right_width()));
	box.SetEdge(BoxArea::Border, BoxEdge::Bottom, Math::Max(0.0f, computed.border_bottom_width()));
	box.SetEdge(BoxArea::Border, BoxEdge::Left, Math::Max(0.0f, computed.border_left_width()));

	// Prepare sizing of the content area.
	Vector2f content_area(-1, -1);
	Vector2f min_size = Vector2f(0, 0);
	Vector2f max_size = Vector2f(FLT_MAX, FLT_MAX);

	// Intrinsic size for replaced elements.
	Vector2f intrinsic_size(-1, -1);
	float intrinsic_ratio = -1;

	const bool replaced_element = element->GetIntrinsicDimensions(intrinsic_size, intrinsic_ratio);

	// Calculate the content area and constraints. 'auto' width and height are handled later.
	// For inline non-replaced elements, width and height are ignored, so we can skip the calculations.
	if (box_context == BuildBoxMode::Block || box_context == BuildBoxMode::UnalignedBlock || replaced_element)
	{
		content_area.x = ResolveValueOr(computed.width(), containing_block.x, -1.f);
		content_area.y = ResolveValueOr(computed.height(), containing_block.y, -1.f);

		min_size = Vector2f{
			ResolveValueOr(computed.min_width(), containing_block.x, 0.f),
			ResolveValueOr(computed.min_height(), containing_block.y, 0.f),
		};
		max_size = Vector2f{
			ResolveValueOr(computed.max_width(), containing_block.x, FLT_MAX),
			ResolveValueOr(computed.max_height(), containing_block.y, FLT_MAX),
		};

		// The sizing keywords (CSS Sizing 3, 4), in the units of the box-sizing box like the values above.
		if (!replaced_element && element != resolving_sizing_keywords_of &&
			(computed.width_keyword() != Style::SizingKeyword::None || computed.height_keyword() != Style::SizingKeyword::None ||
				computed.min_width_keyword() != Style::SizingKeyword::None || computed.max_width_keyword() != Style::SizingKeyword::None ||
				computed.min_height_keyword() != Style::SizingKeyword::None || computed.max_height_keyword() != Style::SizingKeyword::None))
			ResolveSizingKeywords(box, computed, element, containing_block, content_area, min_size, max_size);

		// Adjust sizes for the given box sizing model.
		if (computed.box_sizing() == Style::BoxSizing::BorderBox)
		{
			const float border_padding_width = box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding);
			const float border_padding_height = box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border, BoxArea::Padding);

			min_size.x = BorderSizeToContentSize(min_size.x, border_padding_width);
			max_size.x = BorderSizeToContentSize(max_size.x, border_padding_width);
			content_area.x = BorderSizeToContentSize(content_area.x, border_padding_width);

			min_size.y = BorderSizeToContentSize(min_size.y, border_padding_height);
			max_size.y = BorderSizeToContentSize(max_size.y, border_padding_height);
			content_area.y = BorderSizeToContentSize(content_area.y, border_padding_height);
		}

		max_size = Math::Max(min_size, max_size);

		if (content_area.x >= 0)
			content_area.x = Math::Clamp(content_area.x, min_size.x, max_size.x);
		if (content_area.y >= 0)
			content_area.y = Math::Clamp(content_area.y, min_size.y, max_size.y);

		// 'aspect-ratio' other than 'auto' replaces the natural ratio of a replaced element ('auto && <ratio>': without one).
		if (replaced_element && computed.aspect_ratio() > 0.f && (!computed.aspect_ratio_auto() || intrinsic_ratio <= 0.f))
		{
			intrinsic_ratio = computed.aspect_ratio();
			if (computed.box_sizing() == Style::BoxSizing::BorderBox)
			{
				// (the ratio holds between the border-box sizes)
				const Vector2f edges = {box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding),
					box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border, BoxArea::Padding)};
				if (content_area.x >= 0.f && content_area.y < 0.f)
					content_area.y = Math::Max((content_area.x + edges.x) / intrinsic_ratio - edges.y, 0.f);
				else if (content_area.y >= 0.f && content_area.x < 0.f)
					content_area.x = Math::Max((content_area.y + edges.y) * intrinsic_ratio - edges.x, 0.f);
			}
		}

		if (replaced_element)
			content_area = CalculateSizeForReplacedElement(content_area, min_size, max_size, intrinsic_size, intrinsic_ratio);
		else if (box_context == BuildBoxMode::Block && computed.aspect_ratio() > 0.f)
			ApplyAspectRatio(box, computed, content_area, min_size, max_size);
	}

	box.SetContent(content_area);

	// Evaluate the margins, and width and height if they are auto.
	BuildBoxSizeAndMargins(box, min_size, max_size, containing_block, element, box_context, replaced_element);
}

void LayoutDetails::GetMinMaxWidth(float& min_width, float& max_width, const ComputedValues& computed, const Box& box, float containing_block_width)
{
	min_width = ResolveValueOr(computed.min_width(), containing_block_width, 0.f);
	max_width = ResolveValueOr(computed.max_width(), containing_block_width, FLT_MAX);

	if (computed.box_sizing() == Style::BoxSizing::BorderBox)
	{
		const float border_padding_width = box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding);
		min_width = BorderSizeToContentSize(min_width, border_padding_width);
		max_width = BorderSizeToContentSize(max_width, border_padding_width);
	}
}

void LayoutDetails::GetMinMaxHeight(float& min_height, float& max_height, const ComputedValues& computed, const Box& box,
	float containing_block_height)
{
	min_height = ResolveValueOr(computed.min_height(), containing_block_height, 0.f);
	max_height = ResolveValueOr(computed.max_height(), containing_block_height, FLT_MAX);

	if (computed.box_sizing() == Style::BoxSizing::BorderBox)
	{
		const float border_padding_height = box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border, BoxArea::Padding);
		min_height = BorderSizeToContentSize(min_height, border_padding_height);
		max_height = BorderSizeToContentSize(max_height, border_padding_height);
	}
}

void LayoutDetails::GetDefiniteMinMaxHeight(float& min_height, float& max_height, const ComputedValues& computed, const Box& box,
	float containing_block_height)
{
	const float box_height = box.GetSize().y;
	if (box_height < 0)
	{
		GetMinMaxHeight(min_height, max_height, computed, box, containing_block_height);
	}
	else
	{
		min_height = box_height;
		max_height = box_height;
	}
}

ContainingBlock LayoutDetails::GetContainingBlock(ContainerBox* parent_container, const Style::Position position)
{
	RMLUI_ASSERT(parent_container);
	using Style::Position;

	ContainerBox* container = parent_container;
	BoxArea area = BoxArea::Content;

	// For absolutely positioned boxes we look for the first positioned ancestor.
	if (position == Position::Absolute)
	{
		area = BoxArea::Padding;

		while (container->GetParent() && !container->IsAbsolutePositioningContainingBlock())
			container = container->GetParent();
	}
	// For fixed boxes, the first ancestor that is a containing block for them (a transform, filter, ...). Without one the
	// outermost box (the document) holds them, with the viewport as their containing block (ContainerBox::AddAbsoluteElement,
	// read back from the element by the overload below).
	else if (position == Position::Fixed)
	{
		area = BoxArea::Padding;

		while (
			container->GetParent() && container->GetParent()->GetType() != LayoutBox::Type::Root && !container->IsFixedPositioningContainingBlock())
			container = container->GetParent();
	}

	const Box* box = container->GetIfBox();
	if (!box)
	{
		RMLUI_ERROR;
		return {container, {}};
	}

	Vector2f containing_block = box->GetSize(area);

	// A height given by a flex container that is not definite: percentages of it behave as 'auto'.
	if (area == BoxArea::Content && container->IsHeightIndefinite())
		containing_block.y = -1.f;

	{
		// The scrollbars take their space from the box: in-flow boxes do not overlap their parent's scrollbars, and the
		// containing block of absolutely positioned boxes is the padding box without them (CSS Overflow 3 §3.2).
		if (Element* element = container->GetElement())
		{
			ElementScroll* element_scroll = element->GetElementScroll();
			if (containing_block.x >= 0.f)
				containing_block.x = Math::Max(containing_block.x - element_scroll->GetScrollbarSize(ElementScroll::VERTICAL), 0.f);
			if (containing_block.y >= 0.f)
				containing_block.y = Math::Max(containing_block.y - element_scroll->GetScrollbarSize(ElementScroll::HORIZONTAL), 0.f);
		}
	}

	return {container, containing_block};
}

ContainingBlock LayoutDetails::GetContainingBlock(ContainerBox* parent_container, Element* element)
{
	ContainingBlock containing_block = GetContainingBlock(parent_container, element->GetPosition());
	if (const Rectanglef* containing_block_override = element->GetContainingBlockOverride())
		containing_block.size = containing_block_override->Size();
	return containing_block;
}

void LayoutDetails::BuildBoxSizeAndMargins(Box& box, Vector2f min_size, Vector2f max_size, Vector2f containing_block, Element* element,
	BuildBoxMode box_context, bool replaced_element)
{
	const ComputedValues& computed = element->GetComputedValues();

	if (box_context == BuildBoxMode::Inline || box_context == BuildBoxMode::UnalignedBlock)
	{
		// For inline elements, their calculations are straightforward. No worrying about auto margins and dimensions, etc.
		// Evaluate the margins. Any declared as 'auto' will resolve to 0.
		box.SetEdge(BoxArea::Margin, BoxEdge::Top, ResolveValue(computed.margin_top(), containing_block.x));
		box.SetEdge(BoxArea::Margin, BoxEdge::Right, ResolveValue(computed.margin_right(), containing_block.x));
		box.SetEdge(BoxArea::Margin, BoxEdge::Bottom, ResolveValue(computed.margin_bottom(), containing_block.x));
		box.SetEdge(BoxArea::Margin, BoxEdge::Left, ResolveValue(computed.margin_left(), containing_block.x));
	}
	else
	{
		// The element is block, so we need to run the box through the ringer to potentially evaluate auto margins and dimensions.
		if (computed.position() == Style::Position::Absolute || computed.position() == Style::Position::Fixed)
		{
			BuildAbsoluteBoxWidth(box, computed, min_size.x, max_size.x, containing_block, element, replaced_element);
			BuildAbsoluteBoxHeight(box, computed, min_size.y, max_size.y, containing_block, element, replaced_element);
		}
		else
		{
			BuildBoxWidth(box, computed, min_size.x, max_size.x, containing_block, element, replaced_element);
			ApplyAspectRatioToHeight(box, computed, min_size.y, max_size.y, replaced_element);
			BuildBoxHeight(box, computed, min_size.y, max_size.y, containing_block);
		}
	}
}

void LayoutDetails::ApplyAspectRatio(const Box& box, const ComputedValues& computed, Vector2f& content_area, Vector2f& min_size, Vector2f& max_size)
{
	// The ratio holds between the sizes of the box-sizing box; here sizes are content sizes.
	const float ratio = computed.aspect_ratio();
	const bool border_box = (computed.box_sizing() == Style::BoxSizing::BorderBox);
	const Vector2f edges = border_box ? Vector2f{box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding),
											box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border, BoxArea::Padding)}
									  : Vector2f(0.f);
	auto width_from_height = [&](float height) { return Math::Max((height + edges.y) * ratio - edges.x, 0.f); };

	if (content_area.y >= 0.f && content_area.x < 0.f)
	{
		// A definite height and an automatic width: the width follows the height.
		content_area.x = Math::Clamp(width_from_height(content_area.y), min_size.x, max_size.x);
	}
	else if (content_area.x < 0.f && content_area.y < 0.f)
	{
		// Both automatic: the width is resolved as usual (its height follows it, ApplyAspectRatioToHeight), within the min
		// and max heights transferred through the ratio (CSS Sizing 4 §5.1).
		if (max_size.y < FLT_MAX)
			max_size.x = Math::Min(max_size.x, Math::Max(width_from_height(max_size.y), min_size.x));
		if (min_size.y > 0.f)
			min_size.x = Math::Min(Math::Max(min_size.x, width_from_height(min_size.y)), max_size.x);
	}
}

void LayoutDetails::ApplyAspectRatioToHeight(Box& box, const ComputedValues& computed, float min_height, float max_height, bool replaced_element)
{
	const float ratio = computed.aspect_ratio();
	Vector2f content_area = box.GetSize();
	if (replaced_element || ratio <= 0.f || content_area.x < 0.f || content_area.y >= 0.f)
		return;
	const bool border_box = (computed.box_sizing() == Style::BoxSizing::BorderBox);
	const Vector2f edges = border_box ? Vector2f{box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding),
											box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border, BoxArea::Padding)}
									  : Vector2f(0.f);
	content_area.y = Math::Clamp(Math::Max((content_area.x + edges.x) / ratio - edges.y, 0.f), min_height, max_height);
	box.SetContent(content_area);
}

bool LayoutDetails::IsAspectRatioHeightMinimum(Element* element, const Box& box, float containing_block_height, float& out_max_height)
{
	const ComputedValues& computed = element->GetComputedValues();
	if (computed.aspect_ratio() <= 0.f || box.GetSize().y < 0.f || computed.height().type != Style::Height::Auto ||
		computed.height_keyword() != Style::SizingKeyword::None || computed.min_height().type != Style::MinHeight::Auto ||
		computed.min_height_keyword() != Style::SizingKeyword::None || computed.overflow_x() != Style::Overflow::Visible ||
		computed.overflow_y() != Style::Overflow::Visible || computed.position() == Style::Position::Absolute ||
		computed.position() == Style::Position::Fixed || element->IsReplaced())
		return false;
	float min_height = 0.f;
	GetMinMaxHeight(min_height, out_max_height, computed, box, containing_block_height);
	return true;
}

void LayoutDetails::ResolveSizingKeywords(const Box& box, const ComputedValues& computed, Element* element, Vector2f containing_block,
	Vector2f& content_area, Vector2f& min_size, Vector2f& max_size)
{
	using Style::SizingKeyword;
	const Element* previous = resolving_sizing_keywords_of;
	resolving_sizing_keywords_of = element;

	const bool border_box = (computed.box_sizing() == Style::BoxSizing::BorderBox);
	const Vector2f edges = {box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding),
		box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border, BoxArea::Padding)};
	// The space beside the margins (the 'stretch' size and the fit-content available space), border-box.
	const float margins_x = ResolveValue(computed.margin_left(), containing_block.x) + ResolveValue(computed.margin_right(), containing_block.x);
	const float margins_y = ResolveValue(computed.margin_top(), containing_block.x) + ResolveValue(computed.margin_bottom(), containing_block.x);
	const float available_x = (containing_block.x >= 0.f ? Math::Max(containing_block.x - margins_x, 0.f) : -1.f);
	const float available_y = (containing_block.y >= 0.f ? Math::Max(containing_block.y - margins_y, 0.f) : -1.f);
	const bool absolutely_positioned = (computed.position() == Style::Position::Absolute || computed.position() == Style::Position::Fixed);

	// A border-box width to the units of the values (the box-sizing box).
	auto to_box_x = [&](float border_width) { return border_box ? border_width : Math::Max(border_width - edges.x, 0.f); };
	auto to_box_y = [&](float border_height) { return border_box ? border_height : Math::Max(border_height - edges.y, 0.f); };

	// A width keyword's value, or -1 for 'auto' (the fit-content and stretch widths of absolutely positioned boxes take
	// their insets: BuildAbsoluteBoxWidth).
	auto width_of = [&](SizingKeyword keyword, Style::LengthPercentage argument) -> float {
		switch (keyword)
		{
		case SizingKeyword::MinContent: return to_box_x(GetContentWidth(element, AvailableSpace::MakeMinContent(), containing_block));
		case SizingKeyword::MaxContent: return to_box_x(GetContentWidth(element, AvailableSpace::MakeMaxContent(), containing_block));
		case SizingKeyword::FitContent:
			if (absolutely_positioned || available_x < 0.f)
				return -1.f;
			return to_box_x(GetContentWidth(element, AvailableSpace::MakeDefinite(available_x), containing_block));
		case SizingKeyword::FitContentLength:
		{
			// fit-content(<length-percentage>): the argument as the available space of the box-sizing box.
			if (argument.type == Style::LengthPercentage::Percentage && containing_block.x < 0.f)
				return -1.f;
			const float limit = ResolveValue(argument, containing_block.x) + (border_box ? 0.f : edges.x);
			return to_box_x(GetContentWidth(element, AvailableSpace::MakeDefinite(Math::Max(limit, 0.f)), containing_block));
		}
		case SizingKeyword::Stretch: return (absolutely_positioned || available_x < 0.f) ? -1.f : to_box_x(available_x);
		default: break;
		}
		return -1.f;
	};
	// A height keyword's value: the content-based ones are the automatic height in the block axis.
	auto height_of = [&](SizingKeyword keyword) -> float {
		if (keyword == SizingKeyword::Stretch && !absolutely_positioned && available_y >= 0.f)
			return to_box_y(available_y);
		return -1.f;
	};

	if (computed.width_keyword() != SizingKeyword::None)
		content_area.x = width_of(computed.width_keyword(), computed.fit_content_width());
	if (computed.height_keyword() != SizingKeyword::None)
		content_area.y = height_of(computed.height_keyword());
	if (computed.min_width_keyword() != SizingKeyword::None)
		min_size.x = Math::Max(width_of(computed.min_width_keyword(), {}), 0.f);
	if (computed.max_width_keyword() != SizingKeyword::None)
	{
		const float width = width_of(computed.max_width_keyword(), {});
		max_size.x = (width >= 0.f ? width : FLT_MAX);
	}
	if (computed.min_height_keyword() != SizingKeyword::None)
		min_size.y = Math::Max(height_of(computed.min_height_keyword()), 0.f);
	if (computed.max_height_keyword() != SizingKeyword::None)
	{
		const float height = height_of(computed.max_height_keyword());
		max_size.y = (height >= 0.f ? height : FLT_MAX);
	}

	resolving_sizing_keywords_of = previous;
}

namespace {
	// While the min-content width of an element is measured, the shrink-to-fit width of its descendants (floats,
	// inline-blocks) is their min-content width: CSS's fit-content width, min(max-content, max(min-content, available)),
	// with nothing available. Other layout keeps RmlUi's shrink-to-fit width.
	bool measuring_min_content = false;

	class MinContentScope {
	public:
		explicit MinContentScope(bool value) : previous(measuring_min_content) { measuring_min_content = value; }
		~MinContentScope() { measuring_min_content = previous; }
		MinContentScope(const MinContentScope&) = delete;
		MinContentScope& operator=(const MinContentScope&) = delete;

	private:
		bool previous;
	};
} // namespace

bool LayoutDetails::IsMeasuringMinContent()
{
	return measuring_min_content;
}

float LayoutDetails::GetMinContentWidth(Element* element, Vector2f containing_block)
{
	RMLUI_ASSERT(element);
	LayoutMeasureCache* cache = LayoutMeasureCache::Get();
	float result = 0.f;
	if (cache && cache->Find(element, LayoutMeasureCache::Kind::MinContentWidth, containing_block, 0.f, result))
		return result;

	MinContentScope scope(true);
	Box box;
	LayoutDetails::BuildBox(box, containing_block, element, BuildBoxMode::UnalignedBlock);
	const float edges = box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding);

	float content_width = 0.f;
	const Style::Display display = element->GetDisplay();
	if (box.GetSize().x >= 0.f)
	{
		content_width = box.GetSize().x;
	}
	else if (display != Style::Display::Table && display != Style::Display::InlineTable)
	{
		// Format with no width to give, every line then holds one unbreakable piece of content.
		box.SetContent({0.f, box.GetSize().y});
		RootBox root(Math::Max(containing_block, Vector2f(0.f)));
		if (UniquePtr<LayoutBox> layout_box = FormattingContext::FormatIndependent(&root, element, &box, FormattingContextType::Block))
			content_width = layout_box->GetMinContentWidth();
	}

	result = content_width + edges;
	if (cache)
		cache->Insert(element, LayoutMeasureCache::Kind::MinContentWidth, containing_block, 0.f, result);
	return result;
}

float LayoutDetails::GetMaxContentWidth(Element* element, Vector2f containing_block)
{
	RMLUI_ASSERT(element);
	LayoutMeasureCache* cache = LayoutMeasureCache::Get();
	float result = 0.f;
	if (cache && cache->Find(element, LayoutMeasureCache::Kind::MaxContentWidth, containing_block, 0.f, result))
		return result;

	MinContentScope scope(false);
	Box box;
	LayoutDetails::BuildBox(box, containing_block, element, BuildBoxMode::UnalignedBlock);
	const float edges = box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding);

	float content_width = 0.f;
	const Style::Display display = element->GetDisplay();
	if (box.GetSize().x >= 0.f)
	{
		content_width = box.GetSize().x;
	}
	else if (display != Style::Display::Table && display != Style::Display::InlineTable)
	{
		// Practically unconstrained, as GetShrinkToFitWidth.
		box.SetContent({Math::Max(containing_block.x, 0.f) + 10000.f, box.GetSize().y});
		RootBox root(Math::Max(containing_block, Vector2f(0.f)));
		if (UniquePtr<LayoutBox> layout_box = FormattingContext::FormatIndependent(&root, element, &box, FormattingContextType::Block))
			content_width = layout_box->GetShrinkToFitWidth();
	}

	result = content_width + edges;
	if (cache)
		cache->Insert(element, LayoutMeasureCache::Kind::MaxContentWidth, containing_block, 0.f, result);
	return result;
}

float LayoutDetails::GetContentHeightForWidth(Element* element, float border_box_width, Vector2f containing_block)
{
	RMLUI_ASSERT(element);
	LayoutMeasureCache* cache = LayoutMeasureCache::Get();
	float result = 0.f;
	if (cache && cache->Find(element, LayoutMeasureCache::Kind::HeightForWidth, containing_block, border_box_width, result))
		return result;

	MinContentScope scope(false);
	Box box;
	LayoutDetails::BuildBox(box, containing_block, element, BuildBoxMode::UnalignedBlock);
	const float edges = box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding);
	const float content_width = Math::Max(0.f, border_box_width - edges);
	const float content_height =
		(element->IsReplaced() ? GetReplacedContentHeightForWidth(element, box, content_width, containing_block) : box.GetSize().y);
	box.SetContent({content_width, content_height});

	RootBox root(Math::Max(containing_block, Vector2f(0.f)));
	if (FormattingContext::FormatIndependent(&root, element, &box, FormattingContextType::Block))
		result = element->GetBox().GetSize(BoxArea::Border).y;

	if (cache)
		cache->Insert(element, LayoutMeasureCache::Kind::HeightForWidth, containing_block, border_box_width, result);
	return result;
}

float LayoutDetails::GetReplacedContentHeightForWidth(Element* element, const Box& box, float content_width, Vector2f containing_block)
{
	const ComputedValues& computed = element->GetComputedValues();
	if (ResolveValueOr(computed.height(), containing_block.y, -1.f) >= 0.f || content_width < 0.f)
		return box.GetSize().y;

	Vector2f intrinsic_size(-1.f);
	float ratio = -1.f;
	element->GetIntrinsicDimensions(intrinsic_size, ratio);
	float height = 0.f;
	if (computed.aspect_ratio() > 0.f && (!computed.aspect_ratio_auto() || ratio <= 0.f))
	{
		// 'aspect-ratio' holds between the box-sizing boxes.
		Vector2f edges;
		if (computed.box_sizing() == Style::BoxSizing::BorderBox)
			edges = {box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding),
				box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border, BoxArea::Padding)};
		height = Math::Max((content_width + edges.x) / computed.aspect_ratio() - edges.y, 0.f);
	}
	else if (ratio > 0.f)
		height = content_width / ratio;
	else
		return box.GetSize().y;

	float min_height = 0.f, max_height = FLT_MAX;
	GetMinMaxHeight(min_height, max_height, computed, box, containing_block.y);
	return Math::Clamp(height, min_height, Math::Max(min_height, max_height));
}

float LayoutDetails::GetShrinkToFitWidth(Element* element, Vector2f containing_block, float available_width)
{
	RMLUI_ASSERT(element);

	// @performance Can we lay out the elements directly using a fit-content size mode, instead of fetching the
	// shrink-to-fit width first? Use a non-definite placeholder for the box content width, and available width as a
	// maximum constraint.
	Box box;
	float min_height, max_height;
	LayoutDetails::BuildBox(box, containing_block, element, BuildBoxMode::UnalignedBlock);
	LayoutDetails::GetDefiniteMinMaxHeight(min_height, max_height, element->GetComputedValues(), box, containing_block.y);

	if (box.GetSize().x >= 0.f)
	{
		return box.GetSize().x;
	}

	// Currently we don't support shrink-to-fit width for tables. Just return a zero-sized width.
	const Style::Display display = element->GetDisplay();
	if (display == Style::Display::Table || display == Style::Display::InlineTable)
	{
		return 0.f;
	}

	// Inside a min-content measurement: the min-content width (see measuring_min_content).
	if (measuring_min_content)
		return Math::Max(0.f,
			GetMinContentWidth(element, containing_block) - box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding));

	// Use a large size for the box content width, so that it is practically unconstrained. This makes the formatting
	// procedure act as if under a maximum content constraint. Children with percentage sizing values may be scaled
	// based on this width (such as 'width' or 'margin'), if so, the layout is considered undefined like in CSS 2.
	const float max_content_constraint_width = containing_block.x + 10000.f;
	box.SetContent({max_content_constraint_width, box.GetSize().y});

	// First, format the element under the above generated box. Then we ask the resulting box for its shrink-to-fit
	// width. For block containers, this is essentially its largest line or child box.
	// @performance. Some formatting can be simplified, e.g. absolute elements do not contribute to the shrink-to-fit
	// width. Also, children of elements with a fixed width and height don't need to be formatted further.
	RootBox root(Math::Max(containing_block, Vector2f(0.f)));
	UniquePtr<LayoutBox> layout_box = FormattingContext::FormatIndependent(&root, element, &box, FormattingContextType::Block);

	float shrink_to_fit_width = layout_box->GetShrinkToFitWidth();
	if (available_width < 0.f && containing_block.x >= 0)
		available_width = Math::Max(0.f, containing_block.x - box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Margin, BoxArea::Padding));
	// The space of a vertical scrollbar that is always there is part of the width.
	if (element->GetComputedValues().overflow_y() == Style::Overflow::Scroll)
		shrink_to_fit_width += element->GetElementScroll()->GetScrollbarSize(ElementScroll::VERTICAL);

	if (available_width >= 0.f && shrink_to_fit_width > available_width)
	{
		// fit-content: not narrower than the min-content width (CSS 2.1 §10.3.5).
		const float min_content_width =
			GetMinContentWidth(element, containing_block) - box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding);
		shrink_to_fit_width = Math::Max(available_width, Math::Min(min_content_width, shrink_to_fit_width));
	}
	return shrink_to_fit_width;
}

namespace {
	// The element whose content width GetContentWidth measures (its definite 'width' is then ignored by its layout box).
	const Element* measuring_content_of = nullptr;

	class MeasuringContentScope {
	public:
		explicit MeasuringContentScope(const Element* element) : previous(measuring_content_of) { measuring_content_of = element; }
		~MeasuringContentScope() { measuring_content_of = previous; }
		MeasuringContentScope(const MeasuringContentScope&) = delete;
		MeasuringContentScope& operator=(const MeasuringContentScope&) = delete;

	private:
		const Element* previous;
	};
} // namespace

bool LayoutDetails::IsMeasuringContentOf(const Element* element)
{
	return element == measuring_content_of;
}

static const Element* indefinite_height_of = nullptr;

LayoutDetails::IndefiniteHeightScope::IndefiniteHeightScope(const Element* element, bool indefinite) : previous(indefinite_height_of)
{
	indefinite_height_of = (indefinite ? element : nullptr);
}

LayoutDetails::IndefiniteHeightScope::~IndefiniteHeightScope()
{
	indefinite_height_of = previous;
}

bool LayoutDetails::IsHeightIndefinite(const Element* element)
{
	return element == indefinite_height_of;
}

namespace {
	// The 'justify-self' of a block-level box ('auto': its parent's 'justify-items'), as the justify-items keywords.
	Style::JustifyItems ResolveJustifySelf(Element* element, bool& safe)
	{
		const ComputedValues& computed = element->GetComputedValues();
		const Style::JustifySelf justify_self = computed.justify_self();
		if (justify_self != Style::JustifySelf::Auto)
		{
			safe = (computed.justify_self_overflow() == Style::OverflowAlignment::Safe);
			return static_cast<Style::JustifyItems>(static_cast<int>(justify_self) - 1);
		}
		if (Element* parent = element->GetParentNode())
		{
			const ComputedValues& parent_computed = parent->GetComputedValues();
			safe = (parent_computed.justify_items_overflow() == Style::OverflowAlignment::Safe);
			return parent_computed.justify_items();
		}
		safe = false;
		return Style::JustifyItems::Legacy;
	}
} // namespace

bool LayoutDetails::IsJustifiedFitContent(Element* element)
{
	using Style::JustifyItems;
	bool safe = false;
	switch (ResolveJustifySelf(element, safe))
	{
	case JustifyItems::FlexStart:
	case JustifyItems::FlexEnd:
	case JustifyItems::Center:
	case JustifyItems::Start:
	case JustifyItems::End:
	case JustifyItems::SelfStart:
	case JustifyItems::SelfEnd:
	case JustifyItems::Left:
	case JustifyItems::Right: return true;
	default: break;
	}
	return false;
}

float LayoutDetails::GetJustifySelfOffset(Element* element, float free_space)
{
	using Style::JustifyItems;
	const ComputedValues& computed = element->GetComputedValues();
	if (computed.margin_left().type == Style::Margin::Auto || computed.margin_right().type == Style::Margin::Auto)
		return 0.f;

	bool safe = false;
	JustifyItems justify = ResolveJustifySelf(element, safe);
	if (safe && free_space < 0.f)
		return 0.f;
	// 'justify-items: legacy' of the parent: its legacy 'text-align' value (-webkit-left, -webkit-right, -webkit-center)
	// aligns its block-level children, as in browsers.
	if (justify == JustifyItems::Legacy && computed.justify_self() == Style::JustifySelf::Auto)
	{
		if (Element* parent = element->GetParentNode())
		{
			switch (parent->GetComputedValues().text_align_legacy())
			{
			case Style::TextAlignLegacy::Center: justify = JustifyItems::Center; break;
			case Style::TextAlignLegacy::Right: justify = JustifyItems::Right; break;
			default: break;
			}
		}
	}
	switch (justify)
	{
	case JustifyItems::Center: return 0.5f * free_space;
	case JustifyItems::FlexEnd:
	case JustifyItems::End:
	case JustifyItems::SelfEnd:
	case JustifyItems::Right: return free_space;
	default: break;
	}
	return 0.f;
}

bool LayoutDetails::HasDefiniteHeight(Element* element)
{
	const ComputedValues& computed = element->GetComputedValues();
	if (computed.height().type != Style::Height::Auto)
		return true;
	if ((computed.position() == Style::Position::Absolute || computed.position() == Style::Position::Fixed) &&
		computed.top().type != Style::Top::Auto && computed.bottom().type != Style::Bottom::Auto)
		return true;
	Element* parent = element->GetParentNode();
	if (!parent)
		return true;
	const Style::Display parent_display = parent->GetDisplay();
	return parent_display == Style::Display::Flex || parent_display == Style::Display::InlineFlex || parent_display == Style::Display::Grid ||
		parent_display == Style::Display::InlineGrid;
}

float LayoutDetails::GetIntrinsicEdgesWidth(const LayoutBox& layout_box)
{
	const Box* box = layout_box.GetIfBox();
	if (!box)
		return 0.f;

	Element* element = nullptr;
	switch (layout_box.GetType())
	{
	case LayoutBox::Type::BlockContainer: element = static_cast<const BlockContainer&>(layout_box).GetElement(); break;
	case LayoutBox::Type::FlexContainer:
	case LayoutBox::Type::TableWrapper: element = const_cast<ContainerBox&>(static_cast<const ContainerBox&>(layout_box)).GetElement(); break;
	case LayoutBox::Type::Replaced: element = static_cast<const ReplacedBox&>(layout_box).GetElement(); break;
	default: break;
	}
	if (!element)
		return box->GetSizeAcross(BoxDirection::Horizontal, BoxArea::Margin, BoxArea::Padding);

	const ComputedValues& computed = element->GetComputedValues();
	return ResolveValue(computed.margin_left(), 0.f) + ResolveValue(computed.margin_right(), 0.f) +
		Math::Max(0.f, ResolveValue(computed.padding_left(), 0.f)) + Math::Max(0.f, ResolveValue(computed.padding_right(), 0.f)) +
		box->GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Border);
}

void LayoutDetails::ShiftContentOffsets(Element* container, float delta)
{
	auto shift = [container, delta](Element* parent, auto& shift_ref) -> void {
		const int num_children = parent->GetNumChildren(true);
		for (int i = 0; i < num_children; i++)
		{
			Element* child = parent->GetChild(i);
			if (child->GetOffsetParent() == container)
			{
				child->relative_offset_base.y += delta;
				child->DirtyAbsoluteOffset();
			}
			// Inline elements (and 'display: contents') place their content against the container.
			const Style::Display display = child->GetDisplay();
			if (display == Style::Display::Inline || display == Style::Display::Contents)
				shift_ref(child, shift_ref);
		}
	};
	shift(container, shift);
}

float LayoutDetails::GetContentWidth(Element* element, AvailableSpace available_width, Vector2f containing_block, float known_height)
{
	RMLUI_ASSERT(element);
	if (available_width.IsDefinite())
	{
		// fit-content: min(max-content, max(min-content, available)), both already within the min/max-width.
		const float max_content = GetContentWidth(element, AvailableSpace::MakeMaxContent(), containing_block, known_height);
		if (max_content <= available_width.value)
			return max_content;
		const float min_content = GetContentWidth(element, AvailableSpace::MakeMinContent(), containing_block, known_height);
		return Math::Max(min_content, available_width.value);
	}

	const bool min_content = (available_width.type == AvailableSpace::MinContent);
	const LayoutMeasureCache::Kind kind = (min_content ? LayoutMeasureCache::Kind::ContentMinWidth : LayoutMeasureCache::Kind::ContentMaxWidth);
	LayoutMeasureCache* cache = LayoutMeasureCache::Get();
	float result = 0.f;
	if (cache && cache->Find(element, kind, containing_block, known_height, result))
		return result;

	MinContentScope scope(min_content);
	Box box;
	LayoutDetails::BuildBox(box, containing_block, element, BuildBoxMode::UnalignedBlock);
	const float edges_x = box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding);
	const float edges_y = box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border, BoxArea::Padding);
	const ComputedValues& computed = element->GetComputedValues();

	Vector2f intrinsic_size(-1.f);
	float intrinsic_ratio = -1.f;
	float content_width = 0.f;
	if (element->GetIntrinsicDimensions(intrinsic_size, intrinsic_ratio))
	{
		// A replaced element: its natural width, or its known height through its natural ratio.
		const float content_height = (known_height >= 0.f ? Math::Max(known_height - edges_y, 0.f) : intrinsic_size.y);
		if (intrinsic_ratio > 0.f && known_height >= 0.f)
			content_width = content_height * intrinsic_ratio;
		else if (intrinsic_size.x >= 0.f)
			content_width = intrinsic_size.x;
		else if (intrinsic_ratio > 0.f && content_height >= 0.f)
			content_width = content_height * intrinsic_ratio;
		else
			content_width = 300.f;
	}
	else if (known_height >= 0.f && computed.aspect_ratio() > 0.f)
	{
		// The preferred aspect ratio (of the box given by box-sizing) transfers the height.
		if (computed.box_sizing() == Style::BoxSizing::BorderBox)
			content_width = Math::Max(known_height * computed.aspect_ratio() - edges_x, 0.f);
		else
			content_width = Math::Max(known_height - edges_y, 0.f) * computed.aspect_ratio();
	}
	else if (computed.display() != Style::Display::Table && computed.display() != Style::Display::InlineTable)
	{
		box.SetContent({min_content ? 0.f : Math::Max(containing_block.x, 0.f) + 10000.f, box.GetSize().y});
		MeasuringContentScope content_scope(element);
		RootBox root(Math::Max(containing_block, Vector2f(0.f)));
		if (UniquePtr<LayoutBox> layout_box = FormattingContext::FormatIndependent(&root, element, &box, FormattingContextType::Block))
			content_width = (min_content ? layout_box->GetMinContentWidth() : layout_box->GetShrinkToFitWidth());
		// The space of a vertical scrollbar that is always there is part of the width.
		if (computed.overflow_y() == Style::Overflow::Scroll)
			content_width += element->GetElementScroll()->GetScrollbarSize(ElementScroll::VERTICAL);
	}

	// The size of the content: the element's own min-width and max-width do not apply (CSS Flexbox 9.2: the flex base size
	// is not clamped; callers clamp where CSS does).
	result = content_width + edges_x;

	if (cache)
		cache->Insert(element, kind, containing_block, known_height, result);
	return result;
}

float LayoutDetails::GetContentHeight(Element* element, float border_box_width, Vector2f containing_block)
{
	RMLUI_ASSERT(element);
	LayoutMeasureCache* cache = LayoutMeasureCache::Get();
	float result = 0.f;
	if (cache && cache->Find(element, LayoutMeasureCache::Kind::ContentHeight, containing_block, border_box_width, result))
		return result;

	MinContentScope scope(false);
	Box box;
	LayoutDetails::BuildBox(box, containing_block, element, BuildBoxMode::UnalignedBlock);
	const float edges_x = box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding);
	const float edges_y = box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border, BoxArea::Padding);
	const float content_width = Math::Max(0.f, border_box_width - edges_x);
	const ComputedValues& computed = element->GetComputedValues();

	Vector2f intrinsic_size(-1.f);
	float intrinsic_ratio = -1.f;
	if (element->GetIntrinsicDimensions(intrinsic_size, intrinsic_ratio))
	{
		float content_height = 150.f;
		if (intrinsic_ratio > 0.f)
			content_height = content_width / intrinsic_ratio;
		else if (intrinsic_size.y >= 0.f)
			content_height = intrinsic_size.y;
		result = content_height + edges_y;
	}
	else if (computed.aspect_ratio() > 0.f)
	{
		float content_height;
		if (computed.box_sizing() == Style::BoxSizing::BorderBox)
			content_height = Math::Max(border_box_width / computed.aspect_ratio() - edges_y, 0.f);
		else
			content_height = content_width / computed.aspect_ratio();
		result = content_height + edges_y;
	}
	else
	{
		// Formatted without its own min-height and max-height (see BlockFormattingContext::Format).
		box.SetContent({content_width, -1.f});
		MeasuringContentScope content_scope(element);
		RootBox root(Math::Max(containing_block, Vector2f(0.f)));
		if (FormattingContext::FormatIndependent(&root, element, &box, FormattingContextType::Block))
			result = element->GetBox().GetSize(BoxArea::Border).y;
	}

	if (cache)
		cache->Insert(element, LayoutMeasureCache::Kind::ContentHeight, containing_block, border_box_width, result);
	return result;
}

ComputedAxisSize LayoutDetails::BuildComputedHorizontalSize(const ComputedValues& computed)
{
	return ComputedAxisSize{computed.width(), computed.min_width(), computed.max_width(), computed.padding_left(), computed.padding_right(),
		computed.margin_left(), computed.margin_right(), computed.border_left_width(), computed.border_right_width(), computed.box_sizing()};
}

ComputedAxisSize LayoutDetails::BuildComputedVerticalSize(const ComputedValues& computed)
{
	return ComputedAxisSize{computed.height(), computed.min_height(), computed.max_height(), computed.padding_top(), computed.padding_bottom(),
		computed.margin_top(), computed.margin_bottom(), computed.border_top_width(), computed.border_bottom_width(), computed.box_sizing()};
}

void LayoutDetails::GetEdgeSizes(float& margin_a, float& margin_b, float& padding_border_a, float& padding_border_b,
	const ComputedAxisSize& computed_size, const float base_value)
{
	margin_a = ResolveValue(computed_size.margin_a, base_value);
	margin_b = ResolveValue(computed_size.margin_b, base_value);

	padding_border_a = Math::Max(0.0f, ResolveValue(computed_size.padding_a, base_value)) + Math::Max(0.0f, computed_size.border_a);
	padding_border_b = Math::Max(0.0f, ResolveValue(computed_size.padding_b, base_value)) + Math::Max(0.0f, computed_size.border_b);
}

String LayoutDetails::GetDebugElementName(Element* element)
{
	if (!element)
		return "nullptr";
	if (!element->GetId().empty())
		return '#' + element->GetId();
	if (auto element_text = rmlui_dynamic_cast<ElementText*>(element))
		return '\"' + StringUtilities::StripWhitespace(element_text->GetText()).substr(0, 20) + '\"';
	return element->GetAddress(false, false);
}

Vector2f LayoutDetails::CalculateSizeForReplacedElement(const Vector2f specified_content_size, const Vector2f min_size, const Vector2f max_size,
	const Vector2f intrinsic_size, const float intrinsic_ratio)
{
	// Start with the element's specified width and height. If any of them are auto, use the element's intrinsic
	// dimensions and ratio to find a suitable content size.
	Vector2f content_size = specified_content_size;

	const bool auto_width = (content_size.x < 0);
	const bool auto_height = (content_size.y < 0);

	if (auto_width)
		content_size.x = intrinsic_size.x;

	if (auto_height)
		content_size.y = intrinsic_size.y;

	// Use a fallback size if we still couldn't determine the size.
	if (content_size.x < 0)
		content_size.x = 300;
	if (content_size.y < 0)
		content_size.y = 150;

	// Resolve the size constraints.
	const float min_width = min_size.x;
	const float max_width = max_size.x;
	const float min_height = min_size.y;
	const float max_height = max_size.y;

	// If we have an intrinsic ratio and one of the dimensions is 'auto', then scale it such that the ratio is preserved.
	if (intrinsic_ratio > 0)
	{
		if (auto_width && !auto_height)
		{
			content_size.x = content_size.y * intrinsic_ratio;
		}
		else if (auto_height && !auto_width)
		{
			content_size.y = content_size.x / intrinsic_ratio;
		}
		else if (auto_width && auto_height)
		{
			// If both width and height are auto, try to preserve the ratio under the respective min/max constraints.
			const float w = content_size.x;
			const float h = content_size.y;

			if ((w < min_width && h > max_height) || (w > max_width && h < min_height))
			{
				// Cannot preserve aspect ratio, let it be clamped.
			}
			else if (w < min_width && h < min_height)
			{
				// Increase the size such that both min-constraints are respected. The non-scaled axis will
				// be clamped below, preserving the aspect ratio.
				if (min_width <= min_height * intrinsic_ratio)
					content_size.x = min_height * intrinsic_ratio;
				else
					content_size.y = min_width / intrinsic_ratio;
			}
			else if (w > max_width && h > max_height)
			{
				// Shrink the size such that both max-constraints are respected. The non-scaled axis will
				// be clamped below, preserving the aspect ratio.
				if (max_width <= max_height * intrinsic_ratio)
					content_size.y = max_width / intrinsic_ratio;
				else
					content_size.x = max_height * intrinsic_ratio;
			}
			else
			{
				// Single constraint violations.
				if (w < min_width)
					content_size.y = min_width / intrinsic_ratio;
				else if (w > max_width)
					content_size.y = max_width / intrinsic_ratio;
				else if (h < min_height)
					content_size.x = min_height * intrinsic_ratio;
				else if (h > max_height)
					content_size.x = max_height * intrinsic_ratio;
			}
		}
	}

	content_size.x = Math::Clamp(content_size.x, min_width, max_width);
	content_size.y = Math::Clamp(content_size.y, min_height, max_height);

	return content_size;
}

void LayoutDetails::BuildBoxWidth(Box& box, const ComputedValues& computed, float min_width, float max_width, Vector2f containing_block,
	Element* element, bool replaced_element, float override_shrink_to_fit_width)
{
	RMLUI_ZoneScoped;

	Vector2f content_area = box.GetSize();

	// Determine if the element has automatic margins.
	bool margins_auto[2];
	int num_auto_margins = 0;

	for (int i = 0; i < 2; ++i)
	{
		const Style::Margin margin_value = (i == 0 ? computed.margin_left() : computed.margin_right());
		if (margin_value.type == Style::Margin::Auto)
		{
			margins_auto[i] = true;
			num_auto_margins++;
			box.SetEdge(BoxArea::Margin, i == 0 ? BoxEdge::Left : BoxEdge::Right, 0);
		}
		else
		{
			margins_auto[i] = false;
			box.SetEdge(BoxArea::Margin, i == 0 ? BoxEdge::Left : BoxEdge::Right, ResolveValue(margin_value, containing_block.x));
		}
	}

	const bool absolutely_positioned = (computed.position() == Style::Position::Absolute || computed.position() == Style::Position::Fixed);
	const bool inset_auto = (computed.left().type == Style::Left::Auto || computed.right().type == Style::Right::Auto);
	const bool width_auto = (content_area.x < 0);

	auto GetInsetWidth = [&] {
		// For absolutely positioned elements (and only those), the 'left' and 'right' values are part of the box's width constraint.
		if (absolutely_positioned)
			return ResolveValue(computed.left(), containing_block.x) + ResolveValue(computed.right(), containing_block.x);
		return 0.f;
	};

	// A button in normal flow is as wide as its content with an auto width, a block-level one too (HTML's button layout: the
	// fit-content inline size; Chromium), its auto margins taking the rest; as a flex or grid item it is sized as any item
	// (fork, P4-G12).
	bool button_fit_content = false;
	if (width_auto && !absolutely_positioned && !replaced_element && element->HasButtonLayout())
	{
		Element* parent = element->GetParentNode();
		const Style::Display parent_display = (parent ? parent->GetDisplay() : Style::Display::Block);
		button_fit_content = !(parent_display == Style::Display::Flex || parent_display == Style::Display::InlineFlex ||
			parent_display == Style::Display::Grid || parent_display == Style::Display::InlineGrid);
	}

	// If the width is set to auto, we need to calculate the width.
	if (width_auto)
	{
		// Apply the shrink-to-fit algorithm here to find the width of the element.
		// See CSS 2.1 section 10.3.7 for when this should be applied.
		const bool shrink_to_fit = !replaced_element &&
			((computed.float_() != Style::Float::None) || (absolutely_positioned && inset_auto) ||
				(computed.display() == Style::Display::InlineBlock || computed.display() == Style::Display::InlineFlex ||
					computed.display() == Style::Display::InlineGrid) ||
				(!absolutely_positioned && IsJustifiedFitContent(element)) || button_fit_content);

		if (!shrink_to_fit)
		{
			// The width is set to whatever remains of the containing block.
			content_area.x = containing_block.x - (GetInsetWidth() + box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Margin, BoxArea::Padding));
			content_area.x = Math::Max(0.0f, content_area.x);
		}
		else if (override_shrink_to_fit_width >= 0)
		{
			content_area.x = override_shrink_to_fit_width;
		}
		else
		{
			content_area.x = GetShrinkToFitWidth(element, containing_block);
		}
	}
	// Otherwise, the margins that are set to auto will pick up the remaining width of the containing block (also beside a
	// fit-content block-level button).
	if ((!width_auto || (button_fit_content && computed.display() != Style::Display::InlineBlock && computed.display() != Style::Display::InlineFlex &&
							computed.display() != Style::Display::InlineGrid && computed.float_() == Style::Float::None)) &&
		num_auto_margins > 0)
	{
		box.SetContent({content_area.x, box.GetSize().y});
		float margin =
			(containing_block.x - (GetInsetWidth() + box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Margin))) / float(num_auto_margins);
		// Over-constrained: 'auto' margins are treated as 0 in normal flow (CSS 2.1 §10.3.3).
		if (!absolutely_positioned)
			margin = Math::Max(margin, 0.f);

		if (margins_auto[0])
			box.SetEdge(BoxArea::Margin, BoxEdge::Left, margin);
		if (margins_auto[1])
			box.SetEdge(BoxArea::Margin, BoxEdge::Right, margin);
	}

	// Clamp the calculated width; if the width is changed by the clamp, then the margins need to be recalculated if
	// they were set to auto.
	const float clamped_width = Math::Clamp(content_area.x, min_width, max_width);
	if (clamped_width != content_area.x)
	{
		content_area.x = clamped_width;
		box.SetContent(content_area);

		if (num_auto_margins > 0)
			BuildBoxWidth(box, computed, min_width, max_width, containing_block, element, replaced_element, clamped_width);
	}
	else
		box.SetContent(content_area);
}

namespace {
	// One axis of an absolutely positioned box (CSS Position 3).
	struct AbsoluteAxis {
		float containing_block = 0.f;
		bool has_inset_start = false, has_inset_end = false;
		float inset_start = 0.f, inset_end = 0.f;
		bool margin_start_auto = false, margin_end_auto = false;
		float margin_start = 0.f, margin_end = 0.f; // Non-auto values.
		// 'auto' margins take the free space only between two insets (CSS); a document (RmlUi's root of a document, laid
		// out as absolutely positioned) centers by them without insets, as the root element does in CSS.
		bool auto_margins_without_insets = false;

		// The inset-modified containing block: its insets (a lone or two 'auto' insets resolve to 0; an over-constrained
		// pair shrinks the weaker, end, inset).
		float GetInsetModifiedSize() const
		{
			const float start = (has_inset_start ? inset_start : 0.f);
			const float end = containing_block - (has_inset_end ? inset_end : 0.f);
			return Math::Max(end - start, 0.f);
		}
		float GetStretchFitSize() const { return Math::Max(GetInsetModifiedSize() - margin_start - margin_end, 0.f); }
		// The margins for a margin box of the given border-box size: 'auto' margins take the free space between two insets.
		void ResolveMargins(float border_size, bool inline_axis, float& out_start, float& out_end) const
		{
			out_start = margin_start;
			out_end = margin_end;
			if (((!has_inset_start || !has_inset_end) && !auto_margins_without_insets) || (!margin_start_auto && !margin_end_auto))
				return;
			const float free_space = GetInsetModifiedSize() - border_size;
			if (margin_start_auto && margin_end_auto)
			{
				// In the inline axis, negative free space goes to the end margin.
				if (free_space < 0.f && inline_axis)
				{
					out_start = 0.f;
					out_end = free_space;
				}
				else
					out_start = out_end = 0.5f * free_space;
			}
			else if (margin_start_auto)
				out_start = free_space - margin_end;
			else
				out_end = free_space - margin_start;
		}
	};

	AbsoluteAxis MakeAbsoluteAxis(Style::LengthPercentageAuto inset_start, Style::LengthPercentageAuto inset_end, Style::Margin margin_start,
		Style::Margin margin_end, float containing_block, float margin_basis)
	{
		AbsoluteAxis axis;
		axis.containing_block = containing_block;
		axis.has_inset_start = (inset_start.type != Style::LengthPercentageAuto::Auto);
		axis.has_inset_end = (inset_end.type != Style::LengthPercentageAuto::Auto);
		axis.inset_start = (axis.has_inset_start ? ResolveValue(inset_start, containing_block) : 0.f);
		axis.inset_end = (axis.has_inset_end ? ResolveValue(inset_end, containing_block) : 0.f);
		axis.margin_start_auto = (margin_start.type == Style::Margin::Auto);
		axis.margin_end_auto = (margin_end.type == Style::Margin::Auto);
		axis.margin_start = (axis.margin_start_auto ? 0.f : ResolveValue(margin_start, margin_basis));
		axis.margin_end = (axis.margin_end_auto ? 0.f : ResolveValue(margin_end, margin_basis));
		return axis;
	}
} // namespace

void LayoutDetails::BuildAbsoluteBoxWidth(Box& box, const ComputedValues& computed, float min_width, float max_width, Vector2f containing_block,
	Element* element, bool replaced_element)
{
	AbsoluteAxis axis =
		MakeAbsoluteAxis(computed.left(), computed.right(), computed.margin_left(), computed.margin_right(), containing_block.x, containing_block.x);
	axis.auto_margins_without_insets = (element && element == static_cast<Element*>(element->GetOwnerDocument()));
	const float edges = box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding);
	const bool table = (computed.display() == Style::Display::Table || computed.display() == Style::Display::InlineTable);

	Vector2f content_area = box.GetSize();
	const float ratio = (replaced_element ? 0.f : computed.aspect_ratio());
	if (content_area.x < 0.f && content_area.y < 0.f && ratio > 0.f && containing_block.y >= 0.f && !(axis.has_inset_start && axis.has_inset_end) &&
		computed.width_keyword() == Style::SizingKeyword::None && computed.height_keyword() == Style::SizingKeyword::None && !table)
	{
		// An automatic height that 'top' and 'bottom' make stretch-fit is definite, and transfers as a definite height does.
		const AbsoluteAxis block_axis = MakeAbsoluteAxis(computed.top(), computed.bottom(), computed.margin_top(), computed.margin_bottom(),
			containing_block.y, containing_block.x);
		if (block_axis.has_inset_start && block_axis.has_inset_end)
		{
			float min_height = 0.f, max_height = FLT_MAX;
			GetMinMaxHeight(min_height, max_height, computed, box, containing_block.y);
			const float edges_y = box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border, BoxArea::Padding);
			content_area.y = Math::Clamp(Math::Max(block_axis.GetStretchFitSize() - edges_y, 0.f), min_height, max_height);
		}
	}
	if (content_area.x < 0.f && content_area.y >= 0.f && ratio > 0.f)
	{
		// A definite height transfers through the aspect ratio (of the box-sizing box).
		const float edges_y = box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border, BoxArea::Padding);
		if (computed.box_sizing() == Style::BoxSizing::BorderBox)
			content_area.x = Math::Max((content_area.y + edges_y) * ratio - edges, 0.f);
		else
			content_area.x = content_area.y * ratio;
	}
	if (content_area.x < 0.f)
	{
		const float available = Math::Max(axis.GetStretchFitSize() - edges, 0.f);
		const bool keyword = (computed.width_keyword() != Style::SizingKeyword::None);
		if ((axis.has_inset_start && axis.has_inset_end && !replaced_element && !table && !keyword) ||
			computed.width_keyword() == Style::SizingKeyword::Stretch)
			content_area.x = available;                                                 // stretch-fit
		else
			content_area.x = GetShrinkToFitWidth(element, containing_block, available); // fit-content
	}
	content_area.x = Math::Max(Math::Clamp(content_area.x, min_width, max_width), 0.f);
	box.SetContent(content_area);

	float margin_left = 0.f, margin_right = 0.f;
	axis.ResolveMargins(content_area.x + edges, true, margin_left, margin_right);
	box.SetEdge(BoxArea::Margin, BoxEdge::Left, margin_left);
	box.SetEdge(BoxArea::Margin, BoxEdge::Right, margin_right);
}

void LayoutDetails::BuildAbsoluteBoxHeight(Box& box, const ComputedValues& computed, float min_height, float max_height, Vector2f containing_block,
	Element* element, bool replaced_element)
{
	// Percentage margins refer to the containing block's width (CSS 2.1 §8.3).
	AbsoluteAxis axis =
		MakeAbsoluteAxis(computed.top(), computed.bottom(), computed.margin_top(), computed.margin_bottom(), containing_block.y, containing_block.x);
	axis.auto_margins_without_insets = (element && element == static_cast<Element*>(element->GetOwnerDocument()));
	const float edges = box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Border, BoxArea::Padding);
	const bool table = (computed.display() == Style::Display::Table || computed.display() == Style::Display::InlineTable);

	Vector2f content_area = box.GetSize();
	const float ratio = (replaced_element ? 0.f : computed.aspect_ratio());
	if (content_area.y < 0.f && ratio > 0.f && content_area.x >= 0.f)
	{
		// The width (stretch-fit, fit-content or definite) transfers through the aspect ratio: the block axis depends on it.
		const float edges_x = box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Border, BoxArea::Padding);
		if (computed.box_sizing() == Style::BoxSizing::BorderBox)
			content_area.y = Math::Max((content_area.x + edges_x) / ratio - edges, 0.f);
		else
			content_area.y = content_area.x / ratio;
	}
	const bool keyword = (computed.height_keyword() != Style::SizingKeyword::None);
	// A document that scrolls (overflow-y 'auto', 'scroll' or 'hidden') is the viewport's scroll container (CSS Overflow 3
	// §3.3, the root's overflow is the viewport's): with an automatic height it is as tall as the viewport, its content
	// scrolls in it (it would otherwise grow with its content and never scroll).
	if (content_area.y < 0.f && containing_block.y >= 0.f && !keyword && axis.auto_margins_without_insets &&
		LayoutDetails::IsScrollContainer(Style::Overflow::Visible, computed.overflow_y()))
		content_area.y = Math::Max(containing_block.y - edges - axis.margin_start - axis.margin_end, 0.f);
	if (content_area.y < 0.f && containing_block.y >= 0.f &&
		((axis.has_inset_start && axis.has_inset_end && !replaced_element && !table && !keyword) ||
			computed.height_keyword() == Style::SizingKeyword::Stretch))
		content_area.y = Math::Max(axis.GetStretchFitSize() - edges, 0.f); // stretch-fit
	if (content_area.y >= 0.f)
		content_area.y = Math::Max(Math::Clamp(content_area.y, min_height, max_height), 0.f);
	box.SetContent(content_area);

	// With a content-based height the margins are resolved once the height is known (they need two insets and an 'auto'
	// margin, and then the height is stretch-fit, so it is known here).
	float margin_top = axis.margin_start, margin_bottom = axis.margin_end;
	if (content_area.y >= 0.f)
		axis.ResolveMargins(content_area.y + edges, false, margin_top, margin_bottom);
	box.SetEdge(BoxArea::Margin, BoxEdge::Top, margin_top);
	box.SetEdge(BoxArea::Margin, BoxEdge::Bottom, margin_bottom);
}

void LayoutDetails::BuildBoxHeight(Box& box, const ComputedValues& computed, float min_height, float max_height, Vector2f containing_block)
{
	const float containing_block_height = containing_block.y;
	RMLUI_ZoneScoped;

	Vector2f content_area = box.GetSize();

	// Determine if the element has automatic margins.
	bool margins_auto[2];
	int num_auto_margins = 0;

	for (int i = 0; i < 2; ++i)
	{
		const Style::Margin margin_value = (i == 0 ? computed.margin_top() : computed.margin_bottom());
		if (margin_value.type == Style::Margin::Auto)
		{
			margins_auto[i] = true;
			num_auto_margins++;
			box.SetEdge(BoxArea::Margin, i == 0 ? BoxEdge::Top : BoxEdge::Bottom, 0);
		}
		else
		{
			margins_auto[i] = false;
			// Percentage margins refer to the containing block's width, vertical ones too (CSS 2.1 §8.3).
			box.SetEdge(BoxArea::Margin, i == 0 ? BoxEdge::Top : BoxEdge::Bottom, ResolveValue(margin_value, containing_block.x));
		}
	}

	const bool absolutely_positioned = (computed.position() == Style::Position::Absolute || computed.position() == Style::Position::Fixed);
	const bool inset_auto = (computed.top().type == Style::Top::Auto || computed.bottom().type == Style::Bottom::Auto);
	const bool height_auto = (content_area.y < 0);

	auto GetInsetHeight = [&] {
		// For absolutely positioned elements (and only those), the 'top' and 'bottom' values are part of the box's height constraint.
		if (absolutely_positioned)
			return ResolveValue(computed.top(), containing_block_height) + ResolveValue(computed.bottom(), containing_block_height);
		return 0.f;
	};

	// If the height is set to auto, we need to calculate the height.
	if (height_auto)
	{
		// If the height is set to auto for a box in normal flow, the height is set to -1, representing indefinite height.
		content_area.y = -1;

		// But if we are dealing with an absolutely positioned element we need to consider if the top and bottom
		// properties are set, since the height can be affected.
		if (absolutely_positioned && !inset_auto)
		{
			// The height is set to whatever remains of the containing block.
			content_area.y =
				containing_block_height - (GetInsetHeight() + box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Margin, BoxArea::Padding));
			content_area.y = Math::Max(0.0f, content_area.y);
		}
	}
	// Otherwise, the margins that are set to auto will pick up the remaining height of the containing block: for absolutely
	// positioned boxes only, 'auto' vertical margins are 0 in normal flow (CSS 2.1 §10.6.3).
	else if (num_auto_margins > 0 && absolutely_positioned)
	{
		const float margin =
			(containing_block_height - (GetInsetHeight() + box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Margin))) / float(num_auto_margins);

		if (margins_auto[0])
			box.SetEdge(BoxArea::Margin, BoxEdge::Top, margin);
		if (margins_auto[1])
			box.SetEdge(BoxArea::Margin, BoxEdge::Bottom, margin);
	}

	if (content_area.y >= 0)
	{
		// Clamp the calculated height; if the height is changed by the clamp, then the margins need to be recalculated if
		// they were set to auto.
		float clamped_height = Math::Clamp(content_area.y, min_height, max_height);
		if (clamped_height != content_area.y)
		{
			content_area.y = clamped_height;
			box.SetContent(content_area);

			if (num_auto_margins > 0)
				BuildBoxHeight(box, computed, min_height, max_height, containing_block);

			return;
		}
	}

	box.SetContent(content_area);
}

} // namespace Rml
