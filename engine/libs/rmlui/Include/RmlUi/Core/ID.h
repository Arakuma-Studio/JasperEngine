#pragma once

#include <stdint.h>

namespace Rml {

enum class ShorthandId : uint8_t {
	Invalid,

	/*
	  The following values define the shorthand ids for the main stylesheet specification.
	  These values must not be used in places that have their own property specification,
	  such as decorators and font-effects.
	*/
	Margin,
	Padding,
	BorderWidth,
	BorderColor,
	BorderTop,
	BorderRight,
	BorderBottom,
	BorderLeft,
	Border,
	BorderRadius,
	Inset,
	Overflow,
	Background,
	Font,
	Gap,
	PerspectiveOrigin,
	TransformOrigin,
	Flex,
	FlexFlow,
	Nav,

	/*
	  Phase 3 shorthands (docs/html-ui/phase-3-fork-patches.md, Appendix B), one group per package. All of them are
	  declared here by groundwork so that the packages never edit this enum; each package registers its own group in
	  its section of StyleSheetSpecification::RegisterDefaultProperties. An id that is not registered yet is reserved:
	  it has no name and nothing parses into it.
	*/

	// Groundwork (p3-base): alignment
	PlaceItems,
	PlaceSelf,
	PlaceContent,

	// Values (p3-values): logical properties
	MarginInline,
	MarginInlineStart,
	MarginInlineEnd,
	MarginBlock,
	MarginBlockStart,
	MarginBlockEnd,
	PaddingInline,
	PaddingInlineStart,
	PaddingInlineEnd,
	PaddingBlock,
	PaddingBlockStart,
	PaddingBlockEnd,
	InsetInline,
	InsetInlineStart,
	InsetInlineEnd,
	InsetBlock,
	InsetBlockStart,
	InsetBlockEnd,
	BorderInline,
	BorderInlineWidth,
	BorderInlineStyle,
	BorderInlineColor,
	BorderInlineStart,
	BorderInlineStartWidth,
	BorderInlineStartStyle,
	BorderInlineStartColor,
	BorderInlineEnd,
	BorderInlineEndWidth,
	BorderInlineEndStyle,
	BorderInlineEndColor,
	BorderBlock,
	BorderBlockWidth,
	BorderBlockStyle,
	BorderBlockColor,
	BorderBlockStart,
	BorderBlockStartWidth,
	BorderBlockStartStyle,
	BorderBlockStartColor,
	BorderBlockEnd,
	BorderBlockEndWidth,
	BorderBlockEndStyle,
	BorderBlockEndColor,
	InlineSize,
	BlockSize,
	MinInlineSize,
	MaxInlineSize,
	MinBlockSize,
	MaxBlockSize,

	// Cascade (p3-cascade)
	All,
	ListStyle,

	// Paint (p3-paint)
	BorderStyle,
	BorderTopLeftRadius,
	BorderTopRightRadius,
	BorderBottomRightRadius,
	BorderBottomLeftRadius,
	Outline,
	BackgroundPosition,
	Mask,
	MaskPosition,
	ObjectPosition,

	// Text (p3-text)
	WhiteSpace,
	TextWrap,
	TextDecoration,
	WordWrap,

	// Animation (p3-animation)
	Transition,
	Animation,

	// Grid (p3-grid)
	GridRow,
	GridColumn,
	GridArea,
	GridTemplate,
	Grid,
	GridGap,
	GridRowGap,
	GridColumnGap,

	NumDefinedIds,
	FirstCustomId = NumDefinedIds,

	// The maximum number of IDs. This limits the number of possible custom IDs to MaxNumIds - FirstCustomId.
	MaxNumIds = 0xff
};

enum class PropertyId : uint16_t {
	Invalid,

	/*
	  The following values define the property ids for the main stylesheet specification.
	  These values must not be used in places that have their own property specification,
	  such as decorators and font-effects.
	*/
	MarginTop,
	MarginRight,
	MarginBottom,
	MarginLeft,
	PaddingTop,
	PaddingRight,
	PaddingBottom,
	PaddingLeft,
	BorderTopWidth,
	BorderRightWidth,
	BorderBottomWidth,
	BorderLeftWidth,
	BorderTopColor,
	BorderRightColor,
	BorderBottomColor,
	BorderLeftColor,
	BorderTopLeftRadius,
	BorderTopRightRadius,
	BorderBottomRightRadius,
	BorderBottomLeftRadius,
	Display,
	Position,
	Top,
	Right,
	Bottom,
	Left,
	Float,
	Clear,
	BoxSizing,
	ZIndex,
	Width,
	MinWidth,
	MaxWidth,
	Height,
	MinHeight,
	MaxHeight,
	LineHeight,
	VerticalAlign,
	OverflowX,
	OverflowY,
	Clip,
	Visibility,
	TextOverflow,
	BackgroundColor,
	Color,
	CaretColor,
	ImageColor,
	FontFamily,
	FontStyle,
	FontWeight,
	FontSize,
	FontKerning,
	LetterSpacing,
	TextAlign,
	TextDecoration,
	TextTransform,
	WhiteSpace,
	WordBreak,
	RowGap,
	ColumnGap,
	Cursor,
	Drag,
	TabIndex,
	ScrollbarMargin,
	OverscrollBehavior,

	Perspective,
	PerspectiveOriginX,
	PerspectiveOriginY,
	Transform,
	TransformOriginX,
	TransformOriginY,
	TransformOriginZ,

	Transition,
	Animation,

	Opacity,
	PointerEvents,
	Focus,

	Decorator,
	MaskImage,
	FontEffect,

	Filter,
	BackdropFilter,
	BoxShadow,

	FillImage,

	AlignContent,
	AlignItems,
	AlignSelf,
	FlexBasis,
	FlexDirection,
	FlexGrow,
	FlexShrink,
	FlexWrap,
	JustifyContent,

	NavUp,
	NavRight,
	NavDown,
	NavLeft,

	RmlUi_Language,
	RmlUi_Direction,

	/*
	  Phase 3 longhands (docs/html-ui/phase-3-fork-patches.md, Appendix B), one group per package. All of them are
	  declared here by groundwork so that the packages never edit this enum; each package registers its own group in
	  its section of StyleSheetSpecification::RegisterDefaultProperties (and adds its cases to the matching sections of
	  ElementStyle::ComputeValue and Element::OnPropertyChange). An id that is not registered yet is reserved: it has
	  no name and nothing parses into it.
	*/

	// Groundwork (p3-base)
	Order,
	JustifyItems,
	JustifySelf,
	WillChange,

	// Values (p3-values): new units and values only

	// Cascade (p3-cascade)
	Content,
	Quotes,
	CounterReset,
	CounterIncrement,
	CounterSet,
	ListStyleType,
	ListStylePosition,
	ListStyleImage,

	// Paint (p3-paint)
	BorderTopStyle,
	BorderRightStyle,
	BorderBottomStyle,
	BorderLeftStyle,
	RmlUi_BorderTopLeftRadiusX,
	RmlUi_BorderTopLeftRadiusY,
	RmlUi_BorderTopRightRadiusX,
	RmlUi_BorderTopRightRadiusY,
	RmlUi_BorderBottomRightRadiusX,
	RmlUi_BorderBottomRightRadiusY,
	RmlUi_BorderBottomLeftRadiusX,
	RmlUi_BorderBottomLeftRadiusY,
	OutlineWidth,
	OutlineStyle,
	OutlineColor,
	OutlineOffset,
	Isolation,
	BackgroundImage,
	BackgroundPositionX,
	BackgroundPositionY,
	BackgroundSize,
	BackgroundRepeat,
	BackgroundOrigin,
	BackgroundClip,
	BackgroundAttachment,
	MaskSize,
	MaskPositionX,
	MaskPositionY,
	MaskRepeat,
	ObjectFit,
	ObjectPositionX,
	ObjectPositionY,
	BorderImage,

	// Text (p3-text)
	WordSpacing,
	OverflowWrap,
	WhiteSpaceCollapse,
	TextWrapMode,
	TextDecorationColor,
	TextDecorationStyle,
	TextDecorationThickness,
	TextUnderlineOffset,
	TextShadow,
	UserSelect,
	AccentColor,

	// Layout (p3-layout)
	OverflowClipMargin,
	AspectRatio,
	ScrollbarGutter,
	FlexLineCount,

	// Animation (p3-animation)
	TransitionProperty,
	TransitionDuration,
	TransitionTimingFunction,
	TransitionDelay,
	TransitionBehavior,
	AnimationName,
	AnimationDuration,
	AnimationTimingFunction,
	AnimationDelay,
	AnimationIterationCount,
	AnimationDirection,
	AnimationFillMode,
	AnimationPlayState,
	AnimationComposition,
	Translate,
	Rotate,
	Scale,

	// Grid (p3-grid)
	GridTemplateColumns,
	GridTemplateRows,
	GridTemplateAreas,
	GridAutoColumns,
	GridAutoRows,
	GridAutoFlow,
	GridRowStart,
	GridRowEnd,
	GridColumnStart,
	GridColumnEnd,

	NumDefinedIds,
	FirstCustomId = NumDefinedIds,

	// The maximum number of IDs. This limits the number of possible custom IDs to MaxNumIds - FirstCustomId.
	MaxNumIds = 256
};

enum class MediaQueryId : uint8_t {
	Invalid,

	Width,
	MinWidth,
	MaxWidth,
	Height,
	MinHeight,
	MaxHeight,
	AspectRatio,
	MinAspectRatio,
	MaxAspectRatio,
	Resolution,
	MinResolution,
	MaxResolution,
	Orientation,
	Theme,

	NumDefinedIds
};

enum class FontFaceId : uint8_t {
	Invalid,

    // standard
	FontFamily,
	FontWeight,
	FontStyle,
	Src,

    // extended
    FallbackFace,
    FaceIndex,

	NumDefinedIds
};

enum class EventId : uint16_t {
	Invalid,

	// Core events
	Mousedown,
	Mousescroll,
	Mouseover,
	Mouseout,
	Focus,
	Blur,
	Keydown,
	Keyup,
	Textinput,
	Mouseup,
	Click,
	Dblclick,
	Load,
	Unload,
	Show,
	Hide,
	Mousemove,
	Dragmove,
	Drag,
	Dragstart,
	Dragover,
	Dragdrop,
	Dragout,
	Dragend,
	Handledrag,
	Resize,
	Scroll,
	Animationend,
	Transitionend,

	// Form control events
	Change,
	Submit,
	Tabchange,

	// CSS animation and transition events (phase 3, p3-animation)
	Animationstart,
	Animationiteration,
	Animationcancel,
	Transitionrun,
	Transitionstart,
	Transitioncancel,

	NumDefinedIds,

	// Custom IDs start here
	FirstCustomId = NumDefinedIds,

	// The maximum number of IDs. This limits the number of possible custom IDs to MaxNumIds - FirstCustomId.
	MaxNumIds = 0xffff
};

} // namespace Rml
