#pragma once

#include "Types.h"

namespace Rml {
namespace Style {

	struct LengthPercentageAuto {
		enum Type : uint8_t {
			Auto,
			Length,
			Percentage,
#ifdef RMLUI_MATH_EXPRESSIONS
			Calculation,
#endif
		} type = Length;
		float value = 0;
#ifdef RMLUI_MATH_EXPRESSIONS
		CalculationPtr calculation;
#endif
		LengthPercentageAuto() {}
		LengthPercentageAuto(Type type, float value = 0) : type(type), value(value) {}
#ifdef RMLUI_MATH_EXPRESSIONS
		explicit LengthPercentageAuto(CalculationPtr calculation) : type(Calculation), calculation(std::move(calculation)) {}
#endif
	};
	struct LengthPercentage {
		enum Type : uint8_t {
			Length,
			Percentage,
#ifdef RMLUI_MATH_EXPRESSIONS
			Calculation,
#endif
		} type = Length;
		float value = 0;
#ifdef RMLUI_MATH_EXPRESSIONS
		CalculationPtr calculation;
#endif
		LengthPercentage() {}
		LengthPercentage(Type type, float value = 0) : type(type), value(value) {}
#ifdef RMLUI_MATH_EXPRESSIONS
		explicit LengthPercentage(CalculationPtr calculation) : type(Calculation), calculation(std::move(calculation)) {}
#endif
	};

	struct NumberAuto {
		enum Type : uint8_t { Auto, Number } type = Number;
		float value = 0;
		NumberAuto() {}
		NumberAuto(Type type, float value = 0) : type(type), value(value) {}
	};

	using Margin = LengthPercentageAuto;
	using Padding = LengthPercentage;

	enum class Display : uint8_t {
		None,
		Block,
		Inline,
		InlineBlock,
		FlowRoot,
		Flex,
		InlineFlex,
		Table,
		InlineTable,
		TableRow,
		TableRowGroup,
		TableColumn,
		TableColumnGroup,
		TableCell,
		// Phase 3: 'grid' and 'inline-grid' are grid containers (GridFormattingContext, p3-grid); until its ::marker,
		// 'list-item' is laid out as a block (p3-cascade); 'contents' generates no box, its children are laid out in its
		// place (LayoutDetails::GetLayoutChildren).
		Grid,
		InlineGrid,
		ListItem,
		Contents,
	};
	// 'sticky' is laid out as 'relative' until sticky offsets (p3-layout).
	enum class Position : uint8_t { Static, Relative, Absolute, Fixed, Sticky };

	using Top = LengthPercentageAuto;
	using Right = LengthPercentageAuto;
	using Bottom = LengthPercentageAuto;
	using Left = LengthPercentageAuto;

	enum class Float : uint8_t { None, Left, Right };
	enum class Clear : uint8_t { None, Left, Right, Both };

	enum class BoxSizing : uint8_t { ContentBox, BorderBox };

	using ZIndex = NumberAuto;

	using Width = LengthPercentageAuto;
	using MinWidth = LengthPercentageAuto; // 'auto' (the initial value) is 0 outside flex and grid items.
	using MaxWidth = LengthPercentage; // 'none' keyword converted to FLT_MAX length.

	using Height = LengthPercentageAuto;
	using MinHeight = LengthPercentageAuto; // 'auto' (the initial value) is 0 outside flex and grid items.
	using MaxHeight = LengthPercentage; // 'none' keyword converted to FLT_MAX length.

	struct LineHeight {
		float value = 16.f * 1.2f; // The computed value (length)
		enum InheritType : uint8_t { Number, Length } inherit_type = Number;
		float inherit_value = 1.2f;
		LineHeight() {}
		LineHeight(float value, InheritType inherit_type, float inherit_value) :
			value(value), inherit_type(inherit_type), inherit_value(inherit_value)
		{}
	};
	struct VerticalAlign {
		enum Type : uint8_t { Baseline, Middle, Sub, Super, TextTop, TextBottom, Top, Center, Bottom, Length } type;
		float value; // For length type
		VerticalAlign(Type type = Baseline) : type(type), value(0) {}
		VerticalAlign(float value) : type(Length), value(value) {}
		VerticalAlign(Type type, float value) : type(type), value(value) {}
	};

	// 'clip' (phase 3, p3-layout) clips like 'hidden' without being a scroll container.
	enum class Overflow : uint8_t { Visible, Hidden, Auto, Scroll, Clip };
	struct Clip {
	private:
		// Here, 'value' is encoded with Auto <=> 0, None <=> -1, Always <=> -2. A value > 0 means the given number.
		int8_t value = 0;

	public:
		// The Type enum must instead correspond to the keywords in StyleSheetSpec.
		enum class Type : uint8_t { Auto, None, Always, Number };
		Clip() {}
		Clip(Type type, int8_t number = 0) : value(type == Type::Auto ? 0 : (type == Type::None ? -1 : (type == Type::Always ? -2 : number))) {}
		int GetNumber() const { return value < 0 ? 0 : value; }
		Type GetType() const { return value == 0 ? Type::Auto : (value == -1 ? Type::None : (value == -2 ? Type::Always : Type::Number)); }
		bool operator==(Type type) const { return GetType() == type; }
	};

	enum class Visibility : uint8_t { Visible, Hidden };
	enum class TextOverflow : uint8_t { Clip, Ellipsis, String };

	enum class FontStyle : uint8_t { Normal, Italic };
	enum class FontWeight : uint16_t { Auto = 0, Normal = 400, Bold = 700 }; // Any definite value in the range [1,1000] is valid.
	enum class FontKerning : uint8_t { Auto, Normal, None };

	// 'start' and 'end' are computed as such and resolve against 'direction' (ComputedValues::text_align() returns the
	// resolved value; phase 3, p3-text).
	enum class TextAlign : uint8_t { Left, Right, Center, Justify, Start, End };
	// The lines of 'text-decoration-line': a combination of flags (phase 3, p3-text).
	enum class TextDecoration : uint8_t { None = 0, Underline = 1, Overline = 2, LineThrough = 4 };
	enum class TextTransform : uint8_t { None, Capitalize, Uppercase, Lowercase };
	// The 'white-space' keywords of CSS 2.1. Phase 3 (p3-text) computes 'white-space-collapse' and 'text-wrap-mode', of
	// which 'white-space' is the shorthand; ComputedValues::white_space() maps them to the nearest of these.
	enum class WhiteSpace : uint8_t { Normal, Pre, Nowrap, Prewrap, Preline };
	enum class WordBreak : uint8_t { Normal, BreakAll, BreakWord, KeepAll };

	enum class Drag : uint8_t { None, Drag, DragDrop, Block, Clone };
	enum class TabIndex : uint8_t { None, Auto };
	enum class Focus : uint8_t { None, Auto };
	enum class OverscrollBehavior : uint8_t { Auto, Contain };
	enum class PointerEvents : uint8_t { None, Auto };

	using PerspectiveOrigin = LengthPercentage;
	using TransformOrigin = LengthPercentage;

	enum class OriginX : uint8_t { Left, Center, Right };
	enum class OriginY : uint8_t { Top, Center, Bottom };

	// CSS Box Alignment 3 (phase 3 groundwork): the values after the 6.3 ones are appended, so that 'AlignSelf' stays 'AlignItems'
	// shifted by one ('auto' first), as 'JustifySelf' is 'JustifyItems'. 'Baseline' is 'first baseline'. How flex and grid
	// lay them out is theirs (p3-layout, p3-grid); until then flex reads them as their nearest 6.3 value.
	enum class AlignContent : uint8_t { FlexStart, FlexEnd, Center, SpaceBetween, SpaceAround, SpaceEvenly, Stretch, Normal, Start, End };
	enum class AlignItems : uint8_t { FlexStart, FlexEnd, Center, Baseline, Stretch, Normal, Start, End, SelfStart, SelfEnd, LastBaseline };
	enum class AlignSelf : uint8_t { Auto, FlexStart, FlexEnd, Center, Baseline, Stretch, Normal, Start, End, SelfStart, SelfEnd, LastBaseline };
	enum class JustifyItems : uint8_t {
		FlexStart,
		FlexEnd,
		Center,
		Baseline,
		Stretch,
		Normal,
		Start,
		End,
		SelfStart,
		SelfEnd,
		LastBaseline,
		Left,
		Right,
		Legacy, // The initial value; behaves as 'normal'.
	};
	enum class JustifySelf : uint8_t {
		Auto,
		FlexStart,
		FlexEnd,
		Center,
		Baseline,
		Stretch,
		Normal,
		Start,
		End,
		SelfStart,
		SelfEnd,
		LastBaseline,
		Left,
		Right,
	};
	// The 'safe' and 'unsafe' prefixes of a positional alignment value.
	enum class OverflowAlignment : uint8_t { Default, Safe, Unsafe };
	using FlexBasis = LengthPercentageAuto;
	enum class FlexDirection : uint8_t { Row, RowReverse, Column, ColumnReverse };
	// 'balance' and 'wrap-reverse balance' (CSS Flexbox 2, phase 3, p3-layout) wrap with lines balanced.
	enum class FlexWrap : uint8_t { Nowrap, Wrap, WrapReverse, Balance, BalanceReverse };
	enum class JustifyContent : uint8_t {
		FlexStart,
		FlexEnd,
		Center,
		SpaceBetween,
		SpaceAround,
		SpaceEvenly,
		Stretch,
		Normal,
		Start,
		End,
		Left,
		Right
	};

	enum class Nav : uint8_t { None, Auto, Horizontal, Vertical, TreeOrder };

	enum class Direction : uint8_t { Auto, Ltr, Rtl };

	/*
	  Phase 3 sections (docs/html-ui/phase-3-fork-patches.md 3.0, 3.1.3): each package adds its style types in its own
	  section below, and only there.
	*/

	// -- Groundwork (p3-base) --

	// -- Values (p3-values) --

	// -- Cascade (p3-cascade) --

	// -- Paint (p3-paint) --

	// -- Text (p3-text) --
	enum class WhiteSpaceCollapse : uint8_t { Collapse, Preserve, PreserveBreaks, PreserveSpaces, BreakSpaces };
	enum class TextWrapMode : uint8_t { Wrap, Nowrap };
	enum class OverflowWrap : uint8_t { Normal, Anywhere, BreakWord };
	// 'wavy' is drawn as 'solid' (phase-3 spec 2.2).
	enum class TextDecorationStyle : uint8_t { Solid, Double, Dotted, Dashed, Wavy };
	// 'text' and 'all' select nothing outside text inputs (phase-3 spec 2.2); 'none' disables selection in text inputs.
	enum class UserSelect : uint8_t { Auto, Text, None, Contain, All };

	inline TextDecoration operator|(TextDecoration a, TextDecoration b) { return TextDecoration(uint8_t(a) | uint8_t(b)); }
	inline bool HasTextDecoration(TextDecoration lines, TextDecoration line) { return (uint8_t(lines) & uint8_t(line)) != 0; }

	// 'start' and 'end' against the direction ('auto' reads as 'ltr').
	inline TextAlign ResolveTextAlign(TextAlign align, Direction direction)
	{
		if (align == TextAlign::Start)
			return direction == Direction::Rtl ? TextAlign::Right : TextAlign::Left;
		if (align == TextAlign::End)
			return direction == Direction::Rtl ? TextAlign::Left : TextAlign::Right;
		return align;
	}

	// The CSS 2.1 'white-space' keyword nearest to the longhands: spaces preserved or not, lines wrapped or not.
	inline WhiteSpace ToWhiteSpace(WhiteSpaceCollapse collapse, TextWrapMode wrap_mode)
	{
		const bool wrap = (wrap_mode == TextWrapMode::Wrap);
		switch (collapse)
		{
		case WhiteSpaceCollapse::Collapse: return wrap ? WhiteSpace::Normal : WhiteSpace::Nowrap;
		case WhiteSpaceCollapse::PreserveBreaks: return wrap ? WhiteSpace::Preline : WhiteSpace::Nowrap;
		case WhiteSpaceCollapse::Preserve:
		case WhiteSpaceCollapse::PreserveSpaces:
		case WhiteSpaceCollapse::BreakSpaces: break;
		}
		return wrap ? WhiteSpace::Prewrap : WhiteSpace::Pre;
	}

	// -- Layout (p3-layout) --

	// The CSS Sizing keywords of 'width', 'height', 'min-*', 'max-*' and 'flex-basis' ('content' is flex-basis only;
	// 'fit-content(<length-percentage>)' is width and height only). These properties compute to 'auto' ('none' for max-*)
	// when a keyword is set, so code that does not know them sees an automatic size; the layout reads the keyword apart
	// (ComputedValues::width_keyword() ...). The values follow the properties' keyword lists ('auto' = None).
	enum class SizingKeyword : uint8_t { None, MinContent, MaxContent, FitContent, Stretch, Content, FitContentLength };
	enum class ScrollbarGutter : uint8_t { Auto, Stable, StableBothEdges };

	// -- Animation (p3-animation) --

	// -- Grid (p3-grid) --
	// 'grid-auto-flow': the auto-placement direction, and whether it fills holes ('dense').
	enum class GridAutoFlow : uint8_t { Row, Column, RowDense, ColumnDense };

	class ComputedValues;

} // namespace Style

using ComputedValues = Style::ComputedValues;

} // namespace Rml
