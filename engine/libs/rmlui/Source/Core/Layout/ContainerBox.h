#pragma once

#include "../../../Include/RmlUi/Core/Box.h"
#include "../../../Include/RmlUi/Core/StyleTypes.h"
#include "../../../Include/RmlUi/Core/Types.h"
#include "LayoutBox.h"
#include <optional>

namespace Rml {

/**
    Abstraction for layout boxes that can act as a containing block.

    Implements functionality for catching overflow, and for handling positioned boxes that the box acts as a containing block for.
*/
class ContainerBox : public LayoutBox {
public:
	// Enable or disable scrollbars for the element we represent, preparing it for the first round of layouting, according to our properties.
	void ResetScrollbars(const Box& box);

	/// Checks if we have a new overflow on an auto-scrolling element. If so, our vertical scrollbar will be enabled and
	/// our block boxes will be destroyed. All content will need to be re-formatted.
	/// @param[in] visible_overflow_size The border-box overflow of our descendants, relative to our content area, propagated to ancestors.
	/// @param[in] scrollable_content_size The margin-box overflow of our direct children, relative to our content area.
	/// @param[in] box The box built for the element, possibly with a non-determinate height.
	/// @param[in] max_height Maximum height of the content area, if any.
	/// @returns True if no overflow occurred, false if it did.
	bool CatchOverflow(Vector2f visible_overflow_size, Vector2f scrollable_content_size, const Box& box, float max_height) const;

	// How an absolutely positioned box is placed in the area of its static position, per axis (flex containers: CSS
	// Flexbox §4.1, the box placed as a sole flex item).
	enum class StaticAlignment : uint8_t { Start, Center, End };
	struct StaticPositionArea {
		Rectanglef rectangle;                                                            // Relative to the static position's offset parent.
		StaticAlignment alignment[2] = {StaticAlignment::Start, StaticAlignment::Start}; // Horizontal, vertical.
		bool safe[2] = {false, false};                                                   // 'safe': an overflowing box falls back to the start.
	};

	// Adds an absolutely positioned element, to be formatted and positioned when closing this container, see 'ClosePositionedElements'.
	// @param[in] containing_block Its containing block when it is not our padding box (a grid area, the viewport), relative to our
	// border box: its size and insets resolve against it.
	// @param[in] static_position_area With an area, the static position is the box's position aligned in it (its size known once
	// formatted); 'static_position' is then the area's origin.
	void AddAbsoluteElement(Element* element, Vector2f static_position, Element* static_relative_offset_parent,
		std::optional<Rectanglef> containing_block = std::nullopt, std::optional<StaticPositionArea> static_position_area = std::nullopt);
	// Adds a relatively positioned element which we act as a containing block for.
	void AddRelativeElement(Element* element);
	// Moves the static positions of our absolutely positioned elements given relative to 'static_relative_offset_parent'.
	void ShiftStaticPositions(Element* static_relative_offset_parent, Vector2f delta);

	ContainerBox* GetParent() { return parent_container; }
	Element* GetElement() { return element; }

	bool IsScrollContainer() const;

	// Returns true if this box acts as a containing block for absolutely positioned descendants.
	bool IsAbsolutePositioningContainingBlock() const { return is_absolute_positioning_containing_block; }

protected:
	ContainerBox(Type type, Element* element, ContainerBox* parent_container);

	/// Set the box and scrollable area on our element, possibly catching any overflow.
	/// @param[in] visible_overflow_size The border-box overflow of our descendants, relative to our content area, propagated to ancestors.
	/// @param[in] scrollable_content_size The margin-box overflow of our direct children, relative to our content area.
	/// @param[in] box The box to be set on the element.
	/// @param[in] max_height Maximum height of the content area, if any.
	/// @returns True if no overflow occurred, false if it did.
	bool SubmitBox(Vector2f visible_overflow_size, Vector2f scrollable_content_size, const Box& box, float max_height);

	/// Formats, sizes, and positions all absolute elements whose containing block is this, and offsets relative elements.
	void ClosePositionedElements();

	// Set the element's baseline (proxy for private access to Element).
	void SetElementBaseline(float element_baseline);
	// Calls Element::OnLayout (proxy for private access to Element).
	void SubmitElementLayout();

	// The element this box represents, if any.
	Element* const element;

private:
	struct AbsoluteElement {
		Vector2f static_position;                   // The hypothetical position of the element as if it was placed in normal flow.
		Element* static_position_offset_parent;     // The element for which the static position is offset from.
		std::optional<Rectanglef> containing_block; // Set: the containing block, relative to our border box, instead of our padding box.
		std::optional<StaticPositionArea> static_position_area; // Set: the static position is aligned in this area.
	};
	using AbsoluteElementMap = SmallUnorderedMap<Element*, AbsoluteElement>;

	// Get the scrollable overflow size relative to the content area of this box.
	Vector2f GetScrollableOverflowContentSize(Vector2f visible_overflow_size, Vector2f scrollable_content_size,
		const Vector2f padding_bottom_right) const;

	AbsoluteElementMap absolute_elements; // List of absolutely positioned elements that we act as a containing block for.
	ElementList relative_elements;        // List of relatively positioned elements that we act as a containing block for.

	Style::Overflow overflow_x = Style::Overflow::Visible;
	Style::Overflow overflow_y = Style::Overflow::Visible;
	bool is_absolute_positioning_containing_block = false;

	ContainerBox* parent_container = nullptr;
};

/**
    The root of a tree of layout boxes, usually represents the root element or viewport.
*/
class RootBox final : public ContainerBox {
public:
	RootBox(Vector2f containing_block) : ContainerBox(Type::Root, nullptr, nullptr), box(containing_block) {}
	RootBox(const Box& box) : ContainerBox(Type::Root, nullptr, nullptr), box(box) {}

	const Box* GetIfBox() const override { return &box; }
	String DebugDumpTree(int depth) const override;

private:
	Box box;
};

/**
    A box where flexbox layout occurs.
*/
class FlexContainer final : public ContainerBox {
public:
	FlexContainer(Element* element, ContainerBox* parent_container);

	// Submits the formatted box to the flex container element, and propagates any uncaught overflow to this box.
	// @returns True if it succeeds, otherwise false if it needs to be formatted again because scrollbars were enabled.
	bool Close(const Vector2f visible_overflow_size, Vector2f scrollable_content_size, const Box& box, float element_baseline);

	float GetShrinkToFitWidth() const override;
	float GetMinContentWidth() const override;
	bool GetBaselineOfFirstLine(float& out_baseline) const override;

	const Box* GetIfBox() const override { return &box; }
	String DebugDumpTree(int depth) const override;

	Box& GetBox() { return box; }

	// The first baseline of the container (CSS Flexbox §8.5), from its border-box top.
	void SetFirstBaseline(bool has_baseline, float baseline)
	{
		has_first_baseline = has_baseline;
		first_baseline = baseline;
	}

private:
	Box box;
	bool has_first_baseline = false;
	float first_baseline = 0.f;
};

/**
    A box where table formatting occurs.

    As opposed to a flex container, the table wrapper cannot contain overflow or produce scrollbars.
*/
class TableWrapper final : public ContainerBox {
public:
	TableWrapper(Element* element, ContainerBox* parent_container);

	// Submits the formatted box to the table element, and propagates any uncaught overflow to this box.
	void Close(const Vector2f visible_overflow_size, const Box& box, float element_baseline);

	float GetShrinkToFitWidth() const override;

	const Box* GetIfBox() const override { return &box; }
	String DebugDumpTree(int depth) const override;

	Box& GetBox() { return box; }

private:
	Box box;
};

} // namespace Rml
