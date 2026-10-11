#include "InlineBox.h"
#include "LayoutDetails.h"
#include "../../../Include/RmlUi/Core/Box.h"
#include "../../../Include/RmlUi/Core/Element.h"
#include "../../../Include/RmlUi/Core/FontMetrics.h"

namespace Rml {

static void ZeroBoxEdge(Box& box, BoxEdge edge)
{
	box.SetEdge(BoxArea::Padding, edge, 0.f);
	box.SetEdge(BoxArea::Border, edge, 0.f);
	box.SetEdge(BoxArea::Margin, edge, 0.f);
}

InlineLevelBox* InlineBoxBase::AddChild(UniquePtr<InlineLevelBox> child)
{
	auto result = child.get();
	children.push_back(std::move(child));
	return result;
}

void InlineBoxBase::GetStrut(float& out_total_height_above, float& out_total_depth_below) const
{
	const float line_height = GetElement()->GetLineHeight();

	// As Chromium (NGLineHeightMetrics::AddLeading): the whole-pixel ascent and descent, and the half-leading added to
	// the ascent rounded down; the baseline is then on a whole pixel of the line.
	float ascent, descent;
	LayoutDetails::GetIntegerAscentDescent(GetFontMetrics(), ascent, descent);
	const float half_leading = Math::RoundDown(0.5f * (line_height - (ascent + descent)));
	out_total_height_above = ascent + half_leading;
	out_total_depth_below = line_height - out_total_height_above;
}

String InlineBoxBase::DebugDumpTree(int depth) const
{
	String value = InlineLevelBox::DebugDumpTree(depth);
	for (auto&& child : children)
		value += child->DebugDumpTree(depth + 1);
	return value;
}

InlineBoxBase::InlineBoxBase(Element* element) : InlineLevelBox(element) {}

InlineBoxRoot::InlineBoxRoot(Element* element) : InlineBoxBase(element)
{
	float height_above_baseline, depth_below_baseline;
	GetStrut(height_above_baseline, depth_below_baseline);
	SetHeight(height_above_baseline, depth_below_baseline);
}

FragmentConstructor InlineBoxRoot::CreateFragment(InlineLayoutMode /*mode*/, float /*available_width*/, float /*right_spacing_width*/,
	bool /*first_box*/, LayoutOverflowHandle /*overflow_handle*/)
{
	RMLUI_ERROR;
	return {};
}

void InlineBoxRoot::Submit(const PlacedFragment& /*placed_fragment*/)
{
	RMLUI_ERROR;
}

InlineBox::InlineBox(const InlineLevelBox* parent, Element* element, const Box& _box) : InlineBoxBase(element), box(_box)
{
	RMLUI_ASSERT(box.GetSize().x < 0.f && box.GetSize().y < 0.f);

	const FontMetrics& font_metrics = GetFontMetrics();

	// The inline box content height does not depend on the 'line-height' property, only on the font, and is not exactly
	// specified by CSS (CSS 2.1 §10.6.1). As browsers do, it is the font's ascent plus descent in whole pixels
	// (LayoutDetails::GetIntegerAscentDescent): the content area of a span is as tall as Chromium's
	// (getBoundingClientRect, backgrounds).
	float rounded_ascent, rounded_descent;
	LayoutDetails::GetIntegerAscentDescent(font_metrics, rounded_ascent, rounded_descent);
	const float inner_height = rounded_ascent + rounded_descent;
	box.SetContent(Vector2f(-1.f, inner_height));

	float height_above_baseline, depth_below_baseline;
	GetStrut(height_above_baseline, depth_below_baseline);
	SetHeightAndVerticalAlignment(height_above_baseline, depth_below_baseline, parent);

	const float edge_left = box.GetCumulativeEdge(BoxArea::Padding, BoxEdge::Left);
	const float edge_right = box.GetCumulativeEdge(BoxArea::Padding, BoxEdge::Right);
	SetInlineBoxSpacing(edge_left, edge_right);

	// Vertically position the box so that its content box spans the font's (rounded) ascent above the baseline.
	baseline_to_border_height = rounded_ascent + box.GetEdge(BoxArea::Border, BoxEdge::Top) + box.GetEdge(BoxArea::Padding, BoxEdge::Top);
}

FragmentConstructor InlineBox::CreateFragment(InlineLayoutMode mode, float available_width, float right_spacing_width, bool /*first_box*/,
	LayoutOverflowHandle /*overflow_handle*/)
{
	if (mode != InlineLayoutMode::WrapAny || right_spacing_width <= available_width + GetSpacingLeft())
		return FragmentConstructor{FragmentType::InlineBox, -1.f, {}, {}};

	return {};
}

void InlineBox::Submit(const PlacedFragment& placed_fragment)
{
	Element* element = GetElement();
	RMLUI_ASSERT(element && element != placed_fragment.offset_parent);

	Box element_box = box;
	element_box.SetContent({placed_fragment.layout_width, element_box.GetSize().y});

	if (placed_fragment.split_left)
		ZeroBoxEdge(element_box, BoxEdge::Left);
	if (placed_fragment.split_right)
		ZeroBoxEdge(element_box, BoxEdge::Right);

	// In inline layout, fragments are positioned in terms of (x: margin edge, y: baseline), while element offsets are
	// specified relative to their border box. Thus, find the offset from the fragment position to the border edge.
	const Vector2f border_position =
		placed_fragment.position + Vector2f{element_box.GetEdge(BoxArea::Margin, BoxEdge::Left), -baseline_to_border_height};

	// We can determine the principal fragment based on its left split: Only the principal one has its left side intact,
	// subsequent fragments have their left side split.
	const bool principal_box = !placed_fragment.split_left;

	if (principal_box)
	{
		element_offset = border_position;
		element->SetOffset(border_position, placed_fragment.offset_parent);
		element->SetBox(element_box);
		SubmitElementOnLayout();
	}
	else
	{
		element->AddBox(element_box, border_position - element_offset);
	}
}

} // namespace Rml
