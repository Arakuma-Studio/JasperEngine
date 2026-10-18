#include "BlockFormattingContext.h"
#include "../../../Include/RmlUi/Core/ComputedValues.h"
#include "../../../Include/RmlUi/Core/Element.h"
#include "../../../Include/RmlUi/Core/ElementText.h"
#include "../../../Include/RmlUi/Core/Profiling.h"
#include "../../../Include/RmlUi/Core/PropertyDefinition.h"
#include "../../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "../../../Include/RmlUi/Core/SystemInterface.h"
#include "BlockContainer.h"
#include "FloatedBoxSpace.h"
#include "LayoutDetails.h"
#include <float.h>

namespace Rml {

// Table elements should be handled within FormatElementTable, log a warning when it seems like we're encountering table parts in the wild.
static void LogUnexpectedFlowElement(Element* element, Style::Display display)
{
	RMLUI_ASSERT(element);
	String value = "*unknown";
	StyleSheetSpecification::GetPropertySpecification().GetProperty(PropertyId::Display)->GetValue(value, Property(display));

	Log::Message(Log::LT_WARNING, "Element has a display type '%s' which cannot be located in normal flow layout. Element will not be formatted: %s",
		value.c_str(), element->GetAddress().c_str());
}

#ifdef RMLUI_DEBUG
static bool g_debug_dumping_layout_tree = false;
struct DebugDumpLayoutTree {
	Element* element;
	BlockContainer* block_box;
	bool is_printing_tree_root = false;

	DebugDumpLayoutTree(Element* element, BlockContainer* block_box) : element(element), block_box(block_box)
	{
		// When an element with this ID is encountered, dump the formatted layout tree (including for all descendant formatting contexts).
		static const String debug_trigger_id = "rmlui-debug-layout";
		is_printing_tree_root = element->HasAttribute(debug_trigger_id);
		if (is_printing_tree_root)
			g_debug_dumping_layout_tree = true;
	}
	~DebugDumpLayoutTree()
	{
		if (g_debug_dumping_layout_tree)
		{
			const String header = ":: " + LayoutDetails::GetDebugElementName(element) + " ::\n";
			const String layout_tree = header + block_box->DumpLayoutTree();
			if (SystemInterface* system_interface = GetSystemInterface())
				system_interface->LogMessage(Log::LT_INFO, layout_tree);

			if (is_printing_tree_root)
				g_debug_dumping_layout_tree = false;
		}
	}
};
#else
struct DebugDumpLayoutTree {
	DebugDumpLayoutTree(Element* /*element*/, BlockContainer* /*block_box*/) {}
};
#endif

enum class OuterDisplayType { BlockLevel, InlineLevel, Invalid };

static OuterDisplayType GetOuterDisplayType(Style::Display display)
{
	switch (display)
	{
	case Style::Display::Block:
	case Style::Display::FlowRoot:
	case Style::Display::Flex:
	case Style::Display::Table:
	case Style::Display::Grid:
	case Style::Display::ListItem: return OuterDisplayType::BlockLevel;

	case Style::Display::Inline:
	case Style::Display::InlineBlock:
	case Style::Display::InlineFlex:
	case Style::Display::InlineTable:
	case Style::Display::InlineGrid: return OuterDisplayType::InlineLevel;

	case Style::Display::TableRow:
	case Style::Display::TableRowGroup:
	case Style::Display::TableColumn:
	case Style::Display::TableColumnGroup:
	case Style::Display::TableCell:
	case Style::Display::Contents: // Generates no box: replaced by its children (LayoutDetails::GetLayoutChildren).
	case Style::Display::WebkitBox: // Never computed: resolved by ElementStyle::ComputeValues (fork, P6-L1).
	case Style::Display::WebkitInlineBox:
	case Style::Display::None: break;
	}

	return OuterDisplayType::Invalid;
}

UniquePtr<LayoutBox> BlockFormattingContext::Format(ContainerBox* parent_container, Element* element, const Box* override_initial_box)
{
	RMLUI_ASSERT(parent_container && element);

#ifdef RMLUI_TRACY_PROFILING
	RMLUI_ZoneScopedC(0xB22222);
	auto name = CreateString("%s %x", element->GetAddress(false, false).c_str(), element);
	RMLUI_ZoneName(name.c_str(), name.size());
#endif

	const Vector2f containing_block = LayoutDetails::GetContainingBlock(parent_container, element).size;

	Box box;
	if (override_initial_box)
		box = *override_initial_box;
	else
		LayoutDetails::BuildBox(box, containing_block, element);

	float min_height, max_height;
	LayoutDetails::GetDefiniteMinMaxHeight(min_height, max_height, element->GetComputedValues(), box, containing_block.y);
	// Its content height alone while it is measured (LayoutDetails::GetContentHeight).
	if (box.GetSize().y < 0.f && LayoutDetails::IsMeasuringContentOf(element))
	{
		min_height = 0.f;
		max_height = FLT_MAX;
	}

	UniquePtr<BlockContainer> container = MakeUnique<BlockContainer>(parent_container, nullptr, element, box, min_height, max_height);
	container->SetHeightIndefinite(override_initial_box && LayoutDetails::IsHeightIndefinite(element));
	float content_max_height = 0.f;
	if (!override_initial_box && element->GetComputedValues().aspect_ratio() > 0.f &&
		LayoutDetails::IsAspectRatioHeightMinimum(element, box, containing_block.y, content_max_height))
		container->SetHeightGrowsWithContent(content_max_height);

	DebugDumpLayoutTree debug_dump_tree(element, container.get());

	container->ResetScrollbars(box);

	// Format the element's children. In rare cases, it is possible that we need three iterations: Once to enable the
	// horizontal scrollbar, then to enable the vertical scrollbar, and finally to format with both scrollbars enabled. A
	// line-clamp container whose content ends at its clamp line takes one more, without clamping (fork, P6-L1).
	for (int layout_iteration = 0; layout_iteration < 4; layout_iteration++)
	{
		bool all_children_formatted = true;
		if (rmlui_dynamic_cast<ElementText*>(element))
		{
			// A run of text formatted on its own, as a flex container does with each of its children: CSS wraps it in an
			// anonymous block box (an anonymous flex item, CSS Flexbox §4), here the block box holds the text as its
			// inline content.
			all_children_formatted = FormatInlineBox(container.get(), element);
		}
		if (all_children_formatted)
			all_children_formatted = FormatChildren(container.get(), element);

		if (all_children_formatted && container->Close(nullptr))
			// Success, break out of the loop.
			break;

		// Otherwise, restart formatting now that one or both scrollbars have been enabled.
		container->ResetContents();
	}

	return container;
}

bool BlockFormattingContext::FormatBlockBox(BlockContainer* parent_container, Element* element)
{
	RMLUI_ZoneScopedC(0x2F4F4F);
	const Vector2f containing_block = LayoutDetails::GetContainingBlock(parent_container, element->GetPosition()).size;

	Box box;
	LayoutDetails::BuildBox(box, containing_block, element);
	float min_height, max_height;
	LayoutDetails::GetDefiniteMinMaxHeight(min_height, max_height, element->GetComputedValues(), box, containing_block.y);

	BlockContainer* container = parent_container->OpenBlockBox(element, box, min_height, max_height);
	if (!container)
		return false;
	float content_max_height = 0.f;
	if (element->GetComputedValues().aspect_ratio() > 0.f &&
		LayoutDetails::IsAspectRatioHeightMinimum(element, box, containing_block.y, content_max_height))
		container->SetHeightGrowsWithContent(content_max_height);

	// Format our children. This may result in scrollbars being added to our formatting context root, then we need to
	// bail out and restart formatting for the current block formatting context.
	if (!FormatChildren(container, element))
		return false;

	if (!container->Close(parent_container))
		return false;

	return true;
}

bool BlockFormattingContext::FormatInlineBox(BlockContainer* parent_container, Element* element)
{
	RMLUI_ZoneScopedC(0x3F6F6F);
	const Vector2f containing_block = LayoutDetails::GetContainingBlock(parent_container, element->GetPosition()).size;

	Box box;
	LayoutDetails::BuildBox(box, containing_block, element, BuildBoxMode::Inline);
	auto inline_box_handle = parent_container->AddInlineElement(element, box);

	// Format the element's children.
	if (!FormatChildren(parent_container, element))
		return false;

	parent_container->CloseInlineElement(inline_box_handle);

	return true;
}

bool BlockFormattingContext::FormatChildren(BlockContainer* container, Element* element)
{
	ScopedLayoutChildList children;
	LayoutDetails::GetLayoutChildren(element, *children, false);
	for (size_t i = 0; i < children->size(); i++)
	{
		if (!FormatBlockContainerChild(container, *children, i))
			return false;
	}
	return true;
}

bool BlockFormattingContext::FormatBlockContainerChild(BlockContainer* parent_container, const LayoutChildList& siblings, size_t index)
{
	Element* element = siblings[index];
#ifdef RMLUI_TRACY_PROFILING
	RMLUI_ZoneScoped;
	auto name = CreateString(">%s %x", element->GetAddress(false, false).c_str(), element);
	RMLUI_ZoneName(name.c_str(), name.size());
#endif

	// Check for special formatting tags.
	if (element->GetTagName() == "br")
	{
		parent_container->AddBreak();
		return true;
	}

	// White space that starts or ends a line collapses away (CSS Text 3 §4.1.1): between blocks, and after the last
	// inline-level box of a container, it makes no box.
	if (LayoutDetails::IsCollapsibleWhiteSpaceText(element))
	{
		if (!parent_container->HasOpenInlineContent())
			return true;
		bool inline_follows = false;
		for (size_t sibling_index = index + 1; sibling_index < siblings.size(); sibling_index++)
		{
			Element* sibling = siblings[sibling_index];
			const ComputedValues& sibling_computed = sibling->GetComputedValues();
			if (sibling_computed.display() == Style::Display::None || sibling_computed.position() == Style::Position::Absolute ||
				sibling_computed.position() == Style::Position::Fixed || LayoutDetails::IsCollapsibleWhiteSpaceText(sibling))
				continue;
			inline_follows = (GetOuterDisplayType(sibling_computed.display()) == OuterDisplayType::InlineLevel &&
				sibling_computed.float_() == Style::Float::None);
			break;
		}
		if (!inline_follows)
			return true;
	}

	auto& computed = element->GetComputedValues();
	const Style::Display display = computed.display();

	// Don't lay this element out if it is set to a display type of none.
	if (display == Style::Display::None)
		return true;

	// Check for absolutely positioned elements: they are removed from the flow and added to the box representing their
	// containing block, to be layed out and positioned once that box has been closed and sized.
	const Style::Position position_property = computed.position();
	if (position_property == Style::Position::Absolute || position_property == Style::Position::Fixed)
	{
		const Vector2f static_position = parent_container->GetOpenStaticPosition(display) - parent_container->GetPosition();
		ContainingBlock containing_block = LayoutDetails::GetContainingBlock(parent_container, position_property);
		containing_block.container->AddAbsoluteElement(element, static_position, parent_container->GetElement());
		return true;
	}

	const OuterDisplayType outer_display = GetOuterDisplayType(display);
	if (outer_display == OuterDisplayType::Invalid)
	{
		LogUnexpectedFlowElement(element, display);
		return true;
	}

	// If the element creates an independent formatting context, then format it accordingly.
	if (UniquePtr<LayoutBox> layout_box = FormattingContext::FormatIndependent(parent_container, element, nullptr, FormattingContextType::None))
	{
		// If the element is floating, we remove it from the flow.
		if (computed.float_() != Style::Float::None)
		{
			parent_container->AddFloatElement(element, layout_box->GetVisibleOverflowSize());
		}
		// Otherwise, check if we have a sized block-level box.
		else if (layout_box && outer_display == OuterDisplayType::BlockLevel)
		{
			// A box that establishes a new formatting context does not overlap the floats of ours (CSS 2.1 §9.5): it goes
			// beside them, narrowed if its width is automatic, or below them.
			BlockContainer::FloatFit float_fit;
			float fitted_width = 0.f;
			const bool fitted = parent_container->FitBesideFloats(element, element->GetBox(), float_fit, fitted_width);
			if (fitted && fitted_width != element->GetBox().GetSize().x)
			{
				Box box;
				LayoutDetails::BuildBox(box, LayoutDetails::GetContainingBlock(parent_container, position_property).size, element);
				box.SetContent({fitted_width, box.GetSize().y});
				layout_box = FormattingContext::FormatIndependent(parent_container, element, &box, FormattingContextType::None);
				if (!layout_box)
					return false;
			}
			if (!parent_container->AddBlockLevelBox(std::move(layout_box), element, element->GetBox(), fitted ? &float_fit : nullptr))
				return false;
		}
		// Nope, then this must be an inline-level box.
		else
		{
			RMLUI_ASSERT(outer_display == OuterDisplayType::InlineLevel);
			auto inline_box_handle = parent_container->AddInlineElement(element, element->GetBox());
			parent_container->CloseInlineElement(inline_box_handle);
		}

		return true;
	}

	// The element is an in-flow box participating in this same block formatting context.
	switch (display)
	{
	case Style::Display::Block:
	case Style::Display::ListItem: // (its ::marker comes with the list-item package)
	case Style::Display::Grid:     // (until grid formatting contexts)
		return FormatBlockBox(parent_container, element);
	case Style::Display::Inline: return FormatInlineBox(parent_container, element);
	default:
		RMLUI_ERROR; // Should have been handled above.
		break;
	}

	return true;
}

} // namespace Rml
