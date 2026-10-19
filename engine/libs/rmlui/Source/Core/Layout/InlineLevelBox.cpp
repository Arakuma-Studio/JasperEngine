#include "InlineLevelBox.h"
#include "../../../Include/RmlUi/Core/ComputedValues.h"
#include "../../../Include/RmlUi/Core/Core.h"
#include "../../../Include/RmlUi/Core/ElementText.h"
#include "../../../Include/RmlUi/Core/FontEngineInterface.h"
#include "../../../Include/RmlUi/Core/TextShapingContext.h"
#include "../LineClampEllipsis.h"
#include "../TextUnicode.h"
#include "LayoutDetails.h"
#include "LayoutPools.h"

namespace Rml {

void* InlineLevelBox::operator new(size_t size)
{
	return LayoutPools::AllocateLayoutChunk(size);
}

void InlineLevelBox::operator delete(void* chunk, size_t size)
{
	LayoutPools::DeallocateLayoutChunk(chunk, size);
}

InlineLevelBox::~InlineLevelBox() {}

void InlineLevelBox::SubmitElementOnLayout()
{
	element->OnLayout();
}

const FontMetrics& InlineLevelBox::GetFontMetrics() const
{
	if (FontFaceHandle handle = element->GetFontFaceHandle())
		return GetFontEngineInterface()->GetFontMetrics(handle);

	// If there is no font face defined then we provide zero'd out font metrics. This situation can affect the layout,
	// in particular in terms of inline box sizing and vertical alignment. Thus, this is potentially a situation where
	// we might want to log a warning. However, in many cases it will produce the same layout with or without the font,
	// so in that sense the warnings can produce false positives.
	//
	// For now, we wait until we try to actually place text before producing any warnings, since that is a clear
	// erroneous situation producing no text. See 'LogMissingFontFace' in ElementText.cpp, which also lists some
	// possible reasons for the missing font face.

	static const FontMetrics font_metrics = {};
	return font_metrics;
}

void InlineLevelBox::SetHeightAndVerticalAlignment(float _height_above_baseline, float _depth_below_baseline, const InlineLevelBox* parent)
{
	RMLUI_ASSERT(parent);
	using Style::VerticalAlign;

	SetHeight(_height_above_baseline, _depth_below_baseline);

	const Style::VerticalAlign vertical_align = element->GetComputedValues().vertical_align();
	vertical_align_type = vertical_align.type;

	// Determine the offset from the parent baseline.
	float parent_baseline_offset = 0.f; // The anchor on the parent, as an offset from its baseline.
	float self_baseline_offset = 0.f;   // The offset from the parent anchor to our baseline.

	switch (vertical_align.type)
	{
	case VerticalAlign::Baseline: parent_baseline_offset = 0.f; break;
	case VerticalAlign::Length: parent_baseline_offset = -vertical_align.value; break;
	// sub and super shift by the browsers' amounts (CSS leaves them open): a fifth / a third of the parent's font size, plus 1px
	case VerticalAlign::Sub: parent_baseline_offset = (1.f / 5.f) * (float)parent->GetFontMetrics().size + 1.f; break;
	case VerticalAlign::Super: parent_baseline_offset = (-1.f / 3.f) * (float)parent->GetFontMetrics().size - 1.f; break;
	case VerticalAlign::TextTop:
	{
		float parent_ascent, parent_descent;
		LayoutDetails::GetIntegerAscentDescent(parent->GetFontMetrics(), parent_ascent, parent_descent);
		parent_baseline_offset = -parent_ascent;
		self_baseline_offset = height_above_baseline;
		break;
	}
	case VerticalAlign::TextBottom:
	{
		float parent_ascent, parent_descent;
		LayoutDetails::GetIntegerAscentDescent(parent->GetFontMetrics(), parent_ascent, parent_descent);
		parent_baseline_offset = parent_descent;
		self_baseline_offset = -depth_below_baseline;
		break;
	}
	case VerticalAlign::Middle:
		parent_baseline_offset = -0.5f * parent->GetFontMetrics().x_height;
		self_baseline_offset = 0.5f * (height_above_baseline - depth_below_baseline);
		break;
	case VerticalAlign::Top:
	case VerticalAlign::Center:
	case VerticalAlign::Bottom:
		// These are relative to the line box and handled later.
		break;
	}

	vertical_offset_from_parent = parent_baseline_offset + self_baseline_offset;
}

void InlineLevelBox::SetHeight(float _height_above_baseline, float _depth_below_baseline)
{
	height_above_baseline = _height_above_baseline;
	depth_below_baseline = _depth_below_baseline;
}

void InlineLevelBox::SetInlineBoxSpacing(float _spacing_left, float _spacing_right)
{
	spacing_left = _spacing_left;
	spacing_right = _spacing_right;
}

String InlineLevelBox::DebugDumpTree(int depth) const
{
	String value = String(depth * 2, ' ') + DebugDumpNameValue() + " | " + LayoutDetails::GetDebugElementName(GetElement()) + '\n';
	return value;
}

InlineLevelBox_Atomic::InlineLevelBox_Atomic(const InlineLevelBox* parent, Element* element, const Box& box) : InlineLevelBox(element), box(box)
{
	RMLUI_ASSERT(parent && element);
	RMLUI_ASSERT(box.GetSize().x >= 0.f && box.GetSize().y >= 0.f);

	const float outer_height = box.GetSizeAcross(BoxDirection::Vertical, BoxArea::Margin);

	const float descent = GetElement()->GetBaseline();
	const float ascent = outer_height - descent;
	SetHeightAndVerticalAlignment(ascent, descent, parent);
}

FragmentConstructor InlineLevelBox_Atomic::CreateFragment(InlineLayoutMode mode, float available_width, float right_spacing_width, bool /*first_box*/,
	LayoutOverflowHandle /*overflow_handle*/)
{
	const float outer_width = box.GetSizeAcross(BoxDirection::Horizontal, BoxArea::Margin);

	if (mode != InlineLayoutMode::WrapAny || outer_width + right_spacing_width <= available_width)
		return FragmentConstructor{FragmentType::SizedBox, outer_width, {}, {}};

	return {};
}

void InlineLevelBox_Atomic::Submit(const PlacedFragment& placed_fragment)
{
	const Vector2f margin_position = {placed_fragment.position.x, placed_fragment.position.y - GetHeightAboveBaseline()};
	const Vector2f margin_edge = {box.GetEdge(BoxArea::Margin, BoxEdge::Left), box.GetEdge(BoxArea::Margin, BoxEdge::Top)};
	const Vector2f border_position = margin_position + margin_edge;

	GetElement()->SetOffset(border_position, placed_fragment.offset_parent);
	GetElement()->SetBox(box);
	// (SetBox shows it again: the line hides it each time it lays it out)
	if (placed_fragment.clamp_hidden)
		LineClampAccess::SetHidden(GetElement(), true);
	SubmitElementOnLayout();
}

float InlineLevelBox_Atomic::GetMarginLeft() const
{
	return box.GetEdge(BoxArea::Margin, BoxEdge::Left);
}

float InlineLevelBox_Atomic::GetBorderWidth() const
{
	return box.GetSize(BoxArea::Border).x;
}

static bool HasOutOfFlowDescendant(Element* element)
{
	for (int i = 0; i < element->GetNumChildren(); i++)
	{
		Element* child = element->GetChild(i);
		const Style::Position position = child->GetComputedValues().position();
		if (position == Style::Position::Absolute || position == Style::Position::Fixed || HasOutOfFlowDescendant(child))
			return true;
	}
	return false;
}

bool InlineLevelBox_Atomic::CanBeHiddenByEllipsis() const
{
	return !HasOutOfFlowDescendant(GetElement());
}

InlineLevelBox_Text::InlineLevelBox_Text(ElementText* element) : InlineLevelBox(element) {}

FragmentConstructor InlineLevelBox_Text::CreateFragment(InlineLayoutMode mode, float available_width, float right_spacing_width, bool first_box,
	LayoutOverflowHandle in_overflow_handle)
{
	ElementText* text_element = GetTextElement();

	const bool allow_empty = (mode == InlineLayoutMode::WrapAny);
	const bool decode_escape_characters = true;

	String line_contents;
	int line_begin = in_overflow_handle;
	int line_length = 0;
	float line_width = 0.f;
	bool overflow = !text_element->GenerateLine(line_contents, line_length, line_width, line_begin, available_width, right_spacing_width, first_box,
		decode_escape_characters, allow_empty);

	if (overflow && line_contents.empty())
		// We couldn't fit anything on this line.
		return {};

	LayoutOverflowHandle out_overflow_handle = {};
	if (overflow)
		out_overflow_handle = line_begin + line_length;

	LayoutFragmentHandle fragment_handle = (LayoutFragmentHandle)fragments.size();
	fragments.push_back(std::move(line_contents));

	return FragmentConstructor{FragmentType::TextRun, line_width, fragment_handle, out_overflow_handle};
}

void InlineLevelBox_Text::Submit(const PlacedFragment& placed_fragment)
{
	RMLUI_ASSERT((size_t)placed_fragment.handle < fragments.size());

	const int fragment_index = (int)placed_fragment.handle;
	const bool principal_box = (fragment_index == 0);

	ElementText* text_element = GetTextElement();
	Vector2f line_offset;

	if (principal_box)
	{
		element_offset = placed_fragment.position;
		text_element->SetOffset(placed_fragment.position, placed_fragment.offset_parent);
		text_element->ClearLines();
	}
	else
	{
		line_offset = placed_fragment.position - element_offset;
	}

	// Line clamping (fork, P6-L1): a run the ellipsis hides draws nothing; the one it follows draws it.
	if (placed_fragment.clamp_hidden)
		fragments[fragment_index].clear();
	if (placed_fragment.justification)
		text_element->AddLine(line_offset, std::move(fragments[fragment_index]), *placed_fragment.justification);
	else
		text_element->AddLine(line_offset, std::move(fragments[fragment_index]));
	if (placed_fragment.clamp_ellipsis)
		text_element->SetLineClampEllipsis(MakeUnique<LineClampEllipsis>(*placed_fragment.clamp_ellipsis));
}

float InlineLevelBox_Text::TruncateForEllipsis(LayoutFragmentHandle handle, float width, float space, bool line_rtl, bool first_child,
	Vector<JustificationSpacing>* justification, float& out_shift)
{
	RMLUI_ASSERT((size_t)handle < fragments.size());
	out_shift = 0.f;
	String& text = fragments[handle];
	ElementText* text_element = GetTextElement();
	const FontFaceHandle font_face_handle = text_element->GetFontFaceHandle();
	if (text.empty() || font_face_handle == 0)
		return first_child ? width : -1.f;

	// The part on the line's start side: a run's start in the line's direction, its end against it (the visual left of an
	// RTL run on an LTR line).
	const int run_direction = TextUnicode::FirstStrongDirection(text);
	const bool keep_start = (run_direction == 0 || (run_direction < 0) == line_rtl);

	// The places it can be cut, between clusters ('ends[i]': the byte length of the first i + 1 clusters).
	Vector<size_t> ends;
	{
		const char* p = text.data();
		const char* end = text.data() + text.size();
		Character previous = TextUnicode::DecodeUTF8(p, end);
		while (p < end)
		{
			const char* at = p;
			const Character c = TextUnicode::DecodeUTF8(p, end);
			if (!TextUnicode::ContinuesCluster(c) && previous != Character(0x200D))
				ends.push_back(size_t(at - text.data()));
			previous = c;
		}
		ends.push_back(text.size());
	}
	const int num_clusters = (int)ends.size();

	const auto& computed = text_element->GetComputedValues();
	TextShapingContext text_shaping_context{computed.language(), computed.direction(), computed.font_kerning(), computed.letter_spacing(),
		computed.word_spacing()};
	FontEngineInterface* font_engine_interface = GetFontEngineInterface();
	// The byte range of the part of 'count' clusters on the kept side.
	const auto part_begin = [&](int count) -> size_t {
		return (keep_start || count >= num_clusters ? 0 : ends[num_clusters - count - 1]);
	};
	const auto part_end = [&](int count) -> size_t { return (keep_start ? ends[count - 1] : text.size()); };
	// The justification space of the characters in a byte range, from its start (p6-justify: a justified line is cut with
	// the space around its characters, as Chromium cuts the expanded shape result).
	Vector<JustificationSpacing> part_spacing;
	const auto spacing_of = [&](size_t begin, size_t end) {
		part_spacing.clear();
		if (!justification)
			return;
		for (const JustificationSpacing& spacing : *justification)
		{
			if (spacing.offset >= (int)begin && spacing.offset < (int)end)
				part_spacing.push_back(JustificationSpacing{spacing.offset - (int)begin, spacing.before, spacing.after});
		}
	};
	const auto part_width = [&](int count) {
		if (count <= 0)
			return 0.f;
		const size_t begin = part_begin(count), end = part_end(count);
		spacing_of(begin, end);
		text_shaping_context.justification = Span<const JustificationSpacing>(part_spacing.data(), part_spacing.size());
		return (float)font_engine_interface->GetStringWidth(font_face_handle, StringView(text.data() + begin, text.data() + end),
			text_shaping_context);
	};

	// Chromium's ShapeResult::OffsetToFit: the most clusters whose width is within the space (widths grow with the count).
	int fit = 0;
	{
		int low = 1, high = num_clusters;
		while (low <= high)
		{
			const int middle = (low + high) / 2;
			if (part_width(middle) <= space)
			{
				fit = middle;
				low = middle + 1;
			}
			else
				high = middle - 1;
		}
	}
	if (fit == 0 || fit == num_clusters)
	{
		if (!first_child)
			return -1.f;
		// The first character of a line is clipped rather than ellipsized (CSS UI 4 §5.2): one is kept.
		fit = (fit == 0 ? 1 : num_clusters - 1);
		if (fit <= 0)
			fit = 1;
	}

	const float kept_width = part_width(fit);
	const size_t begin = part_begin(fit), end = part_end(fit);
	text = String(text.data() + begin, text.data() + end);
	if (justification)
		*justification = std::move(part_spacing);
	if (line_rtl)
		out_shift = width - kept_width;
	return kept_width;
}

String InlineLevelBox_Text::DebugDumpNameValue() const
{
	return "InlineLevelBox_Text";
}

ElementText* InlineLevelBox_Text::GetTextElement()
{
	return rmlui_static_cast<ElementText*>(GetElement());
}
} // namespace Rml
