#pragma once

#include "Animation.h"
#include "Element.h"
#include "RenderBox.h"
#include "StyleTypes.h"
#include "Types.h"
#include <cfloat>

namespace Rml {
namespace Style {

#ifdef RMLUI_MATH_EXPRESSIONS
	struct ComputedCalculationStore;
#endif

	/*
	    A computed value is a value resolved as far as possible :before: introducing layouting. See CSS specs for details of each property.

	    Note: Enums and default values must correspond to the keywords and defaults in `StyleSheetSpecification.cpp`.
	*/

	struct CommonValues {
		CommonValues() :
			display(Display::Inline), position(Position::Static), float_(Float::None), clear(Clear::None), overflow_x(Overflow::Visible),
			overflow_y(Overflow::Visible),

			has_decorator(false), box_sizing(BoxSizing::ContentBox),

			width_type(LengthPercentageAuto::Auto), height_type(LengthPercentageAuto::Auto),

			margin_top_type(LengthPercentageAuto::Length), margin_right_type(LengthPercentageAuto::Length),
			margin_bottom_type(LengthPercentageAuto::Length), margin_left_type(LengthPercentageAuto::Length),

			padding_top_type(LengthPercentage::Length), padding_right_type(LengthPercentage::Length), padding_bottom_type(LengthPercentage::Length),
			padding_left_type(LengthPercentage::Length),

			top_type(LengthPercentageAuto::Auto), right_type(LengthPercentageAuto::Auto), bottom_type(LengthPercentageAuto::Auto),
			left_type(LengthPercentageAuto::Auto), z_index_type(NumberAuto::Auto), border_top_style(BorderStyle::None),
			border_right_style(BorderStyle::None), border_bottom_style(BorderStyle::None), border_left_style(BorderStyle::None)
		{}

		Display display : 5;
		Position position : 3;

		Float float_ : 2;
		Clear clear : 2;

		Overflow overflow_x : 3, overflow_y : 3;

		bool has_decorator : 1;
		BoxSizing box_sizing : 1;

		LengthPercentageAuto::Type width_type : 2, height_type : 2;

		LengthPercentageAuto::Type margin_top_type : 2, margin_right_type : 2, margin_bottom_type : 2, margin_left_type : 2;
#ifdef RMLUI_MATH_EXPRESSIONS
		LengthPercentage::Type padding_top_type : 2, padding_right_type : 2, padding_bottom_type : 2, padding_left_type : 2;
#else
		LengthPercentage::Type padding_top_type : 1, padding_right_type : 1, padding_bottom_type : 1, padding_left_type : 1;
#endif
		LengthPercentageAuto::Type top_type : 2, right_type : 2, bottom_type : 2, left_type : 2;

		NumberAuto::Type z_index_type : 1;

		BorderStyle border_top_style : 4, border_right_style : 4, border_bottom_style : 4, border_left_style : 4;

		float width_value = 0;
		float height_value = 0;

		float margin_top_value = 0;
		float margin_right_value = 0;
		float margin_bottom_value = 0;
		float margin_left_value = 0;

		float padding_top_value = 0;
		float padding_right_value = 0;
		float padding_bottom_value = 0;
		float padding_left_value = 0;

		float top_value = 0;
		float right_value = 0;
		float bottom_value = 0;
		float left_value = 0;

		float z_index_value = 0;

		// The declared widths ('medium' initially, snapped to whole pixels); a side whose style is none or hidden computes
		// its width to 0 (the accessors).
		float border_top_width = 3.f, border_right_width = 3.f, border_bottom_width = 3.f, border_left_width = 3.f;

		// The initial value is 'currentcolor': an element's border colors not declared follow its color (ElementStyle::ComputeValues).
		Colourb border_top_color{0, 0, 0}, border_right_color{0, 0, 0}, border_bottom_color{0, 0, 0}, border_left_color{0, 0, 0};

		Colourb background_color = Colourb(0, 0, 0, 0);
	};

	struct InheritedValues {
		InheritedValues() :
			font_weight(FontWeight::Normal), font_kerning(FontKerning::Auto), has_letter_spacing(0), font_style(FontStyle::Normal),
			has_font_effect(false), pointer_events(PointerEvents::Auto), focus(Focus::Auto), text_align(TextAlign::Start),
			text_align_legacy(TextAlignLegacy::None), text_decoration(TextDecoration::None), text_transform(TextTransform::None),
			white_space_collapse(WhiteSpaceCollapse::Collapse), text_wrap_mode(TextWrapMode::Wrap), word_break(WordBreak::Normal),
			direction(Direction::Auto), visibility(Visibility::Visible), line_height_inherit_type(LineHeight::Normal)
		{}

		// Font face used to render text and resolve ex properties. Does not represent a true property
		// like most computed values, but placed here as it is used and inherited in a similar manner.
		FontFaceHandle font_face_handle = 0;

		float font_size = 16.f;

		Colourb color = Colourb(0, 0, 0);

		FontWeight font_weight : 10;
		FontKerning font_kerning : 2;
		uint16_t has_letter_spacing : 1;

		FontStyle font_style : 1;
		bool has_font_effect : 1;
		PointerEvents pointer_events : 1;
		Focus focus : 1;

		TextAlign text_align : 3;
		TextAlignLegacy text_align_legacy : 2;
		TextDecoration text_decoration : 3;
		TextTransform text_transform : 2;
		WhiteSpaceCollapse white_space_collapse : 3;
		TextWrapMode text_wrap_mode : 1;
		WordBreak word_break : 2;

		Direction direction : 2;

		Visibility visibility : 1;

		LineHeight::InheritType line_height_inherit_type : 2;
		float line_height = 16.f * 1.2f;
		float line_height_inherit = 1.2f;

		String language = "";

		// -- Text (p3-text) --
		OverflowWrap overflow_wrap = OverflowWrap::Normal;
		bool has_text_shadow = false;
		float letter_spacing = 0.f; // px
		float word_spacing = 0.f;   // px
	};

	struct RareValues {
		RareValues() :
			min_width_type(LengthPercentageAuto::Auto), max_width_type(LengthPercentage::Length), min_height_type(LengthPercentageAuto::Auto),
			max_height_type(LengthPercentage::Length),

			perspective_origin_x_type(LengthPercentage::Percentage), perspective_origin_y_type(LengthPercentage::Percentage),
			transform_origin_x_type(LengthPercentage::Percentage), transform_origin_y_type(LengthPercentage::Percentage), has_local_transform(false),
			has_local_perspective(false),

			flex_basis_type(LengthPercentageAuto::Auto), row_gap_type(LengthPercentage::Length), column_gap_type(LengthPercentage::Length),

			vertical_align_type(VerticalAlign::Baseline), drag(Drag::None), tab_index(TabIndex::None), overscroll_behavior(OverscrollBehavior::Auto),

			has_mask_image(false), has_filter(false), has_backdrop_filter(false), has_box_shadow(false), text_overflow(TextOverflow::Clip)
		{}

#ifdef RMLUI_MATH_EXPRESSIONS
		LengthPercentageAuto::Type min_width_type : 2;
		LengthPercentage::Type max_width_type : 2;
		LengthPercentageAuto::Type min_height_type : 2;
		LengthPercentage::Type max_height_type : 2;

		LengthPercentage::Type perspective_origin_x_type : 2, perspective_origin_y_type : 2;
		LengthPercentage::Type transform_origin_x_type : 2, transform_origin_y_type : 2;
#else
		LengthPercentageAuto::Type min_width_type : 2;
		LengthPercentage::Type max_width_type : 1;
		LengthPercentageAuto::Type min_height_type : 2;
		LengthPercentage::Type max_height_type : 1;

		LengthPercentage::Type perspective_origin_x_type : 1, perspective_origin_y_type : 1;
		LengthPercentage::Type transform_origin_x_type : 1, transform_origin_y_type : 1;
#endif
		bool has_local_transform : 1, has_local_perspective : 1;

		LengthPercentageAuto::Type flex_basis_type : 2;
#ifdef RMLUI_MATH_EXPRESSIONS
		LengthPercentage::Type row_gap_type : 2, column_gap_type : 2;
#else
		LengthPercentage::Type row_gap_type : 1, column_gap_type : 1;
#endif

		VerticalAlign::Type vertical_align_type : 4;
		Drag drag : 3;
		TabIndex tab_index : 1;
		OverscrollBehavior overscroll_behavior : 1;

		bool has_mask_image : 1;
		bool has_filter : 1;
		bool has_backdrop_filter : 1;
		bool has_box_shadow : 1;

		TextOverflow text_overflow : 2;

		Clip clip;

		float min_width = 0, max_width = FLT_MAX;
		float min_height = 0, max_height = FLT_MAX;
		float vertical_align_length = 0;

		float perspective = 0;
		float perspective_origin_x = 50.f;
		float perspective_origin_y = 50.f;

		float transform_origin_x = 50.f;
		float transform_origin_y = 50.f;
		float transform_origin_z = 0.0f;

		float flex_basis = 0;
		float row_gap = 0, column_gap = 0;

		Colourb image_color = Colourb(255, 255, 255);
		float scrollbar_margin = 0.f;

		/*
		  Phase 3 sections (docs/html-ui/phase-3-fork-patches.md 3.0, 3.1.3): each package adds its computed fields in its
		  own section below, and only there (the matching accessors go into the same section of ComputedValues).
		*/

		// -- Groundwork (p3-base) --
		int order = 0;
		uint8_t will_change = 0; // the hints of 'will-change' (WillChange::Hint bits, Source/Core/WillChange.h)
		// The element declares justify-items, justify-self, align-content (ElementStyle::ComputeValues): their accessors look
		// the declaration up only then (block layout asks for them for every box).
		bool has_justify_items = false, has_justify_self = false, has_align_content = false;

		// -- Values (p3-values) --

		// -- Cascade (p3-cascade) --

		// -- Paint (p3-paint) --
		// Border radii: the horizontal and the vertical radius of the corners top-left, top-right, bottom-right and
		// bottom-left (index 2 * corner + axis). A length in px, a percentage (of the border box's width or height) or
		// a calc() of both (Element::GetRenderBox resolves and scales them all).
		LengthPercentage border_radius_values[8];

		// The element's opacity (CSS: not inherited): below 1 the element and its descendants are drawn as a group
		// (Element::GetDrawOpacity, ElementEffects).
		float opacity = 1.f;
		Isolation isolation = Isolation::Auto;
		bool has_background_image = false; // a background layer with an image (ElementBackgroundLayers)

		// object-fit and object-position (replaced elements: ComputeObjectFitRect in ObjectFit.h).
		ObjectFit object_fit = ObjectFit::Fill;
		LengthPercentage object_position_x = LengthPercentage(LengthPercentage::Percentage, 50.f);
		LengthPercentage object_position_y = LengthPercentage(LengthPercentage::Percentage, 50.f);

		// Outline: the width as declared (the computed width is 0 when the style is none), the colour unless it is
		// 'currentcolor' (the initial value, and 'invert').
		BorderStyle outline_style = BorderStyle::None;
		bool outline_color_current = true;
		Colourb outline_color{0, 0, 0};
		float outline_width = 3.f;
		float outline_offset = 0.f;

		// -- Text (p3-text) --

		// -- Layout (p3-layout) --
		// Sizing keywords (the property itself computes to 'auto' or 'none'), and the argument of fit-content(<length-percentage>).
		SizingKeyword width_keyword = SizingKeyword::None, height_keyword = SizingKeyword::None;
		SizingKeyword min_width_keyword = SizingKeyword::None, min_height_keyword = SizingKeyword::None;
		SizingKeyword max_width_keyword = SizingKeyword::None, max_height_keyword = SizingKeyword::None;
		SizingKeyword flex_basis_keyword = SizingKeyword::None;
		LengthPercentage::Type fit_content_width_type = LengthPercentage::Length, fit_content_height_type = LengthPercentage::Length;
		ScrollbarGutter scrollbar_gutter = ScrollbarGutter::Auto;
		bool aspect_ratio_auto = true;   // 'auto' (alone, or with a ratio: 'auto && <ratio>')
		float aspect_ratio = 0.f;        // width / height; 0 without a ratio
		float fit_content_width = 0.f, fit_content_height = 0.f;
		float overflow_clip_margin = 0.f;

		// -- Animation (p3-animation) --
		// The individual transform properties are not 'none'.
		bool has_translate = false, has_rotate = false, has_scale = false;

		// -- Grid (p3-grid) --
	};

	class ComputedValues : NonCopyMoveable {
	public:
#ifdef RMLUI_MATH_EXPRESSIONS
		explicit ComputedValues(Element* element);
		~ComputedValues();
#else
		explicit ComputedValues(Element* element) : element(element) {}
#endif

		// clang-format off

		// -- Common --
		LengthPercentageAuto width()               const { return GetLengthPercentageAuto(common.width_type, common.width_value, ComputedCalculationSlot::Width); }
		LengthPercentageAuto height()              const { return GetLengthPercentageAuto(common.height_type, common.height_value, ComputedCalculationSlot::Height); }
		LengthPercentageAuto margin_top()          const { return GetLengthPercentageAuto(common.margin_top_type, common.margin_top_value, ComputedCalculationSlot::MarginTop); }
		LengthPercentageAuto margin_right()        const { return GetLengthPercentageAuto(common.margin_right_type, common.margin_right_value, ComputedCalculationSlot::MarginRight); }
		LengthPercentageAuto margin_bottom()       const { return GetLengthPercentageAuto(common.margin_bottom_type, common.margin_bottom_value, ComputedCalculationSlot::MarginBottom); }
		LengthPercentageAuto margin_left()         const { return GetLengthPercentageAuto(common.margin_left_type, common.margin_left_value, ComputedCalculationSlot::MarginLeft); }
		LengthPercentage     padding_top()         const { return GetLengthPercentage(common.padding_top_type, common.padding_top_value, ComputedCalculationSlot::PaddingTop); }
		LengthPercentage     padding_right()       const { return GetLengthPercentage(common.padding_right_type, common.padding_right_value, ComputedCalculationSlot::PaddingRight); }
		LengthPercentage     padding_bottom()      const { return GetLengthPercentage(common.padding_bottom_type, common.padding_bottom_value, ComputedCalculationSlot::PaddingBottom); }
		LengthPercentage     padding_left()        const { return GetLengthPercentage(common.padding_left_type, common.padding_left_value, ComputedCalculationSlot::PaddingLeft); }
		LengthPercentageAuto top()                 const { return GetLengthPercentageAuto(common.top_type, common.top_value, ComputedCalculationSlot::Top); }
		LengthPercentageAuto right()               const { return GetLengthPercentageAuto(common.right_type, common.right_value, ComputedCalculationSlot::Right); }
		LengthPercentageAuto bottom()              const { return GetLengthPercentageAuto(common.bottom_type, common.bottom_value, ComputedCalculationSlot::Bottom); }
		LengthPercentageAuto left()                const { return GetLengthPercentageAuto(common.left_type, common.left_value, ComputedCalculationSlot::Left); }
		NumberAuto           z_index()             const { return NumberAuto(common.z_index_type, common.z_index_value); }
		float                border_top_width()    const { return IsBorderStylePainted(common.border_top_style) ? common.border_top_width : 0.f; }
		float                border_right_width()  const { return IsBorderStylePainted(common.border_right_style) ? common.border_right_width : 0.f; }
		float                border_bottom_width() const { return IsBorderStylePainted(common.border_bottom_style) ? common.border_bottom_width : 0.f; }
		float                border_left_width()   const { return IsBorderStylePainted(common.border_left_style) ? common.border_left_width : 0.f; }
		BorderStyle          border_top_style()    const { return common.border_top_style; }
		BorderStyle          border_right_style()  const { return common.border_right_style; }
		BorderStyle          border_bottom_style() const { return common.border_bottom_style; }
		BorderStyle          border_left_style()   const { return common.border_left_style; }
		BoxSizing            box_sizing()          const { return common.box_sizing; }
		Display              display()             const { return common.display; }
		Position             position()            const { return common.position; }
		Float                float_()              const { return common.float_; }
		Clear                clear()               const { return common.clear; }
		Overflow             overflow_x()          const { return common.overflow_x; }
		Overflow             overflow_y()          const { return common.overflow_y; }
		Visibility           visibility()          const { return inherited.visibility; }
		Colourb              background_color()    const { return common.background_color; }
		Colourb              border_top_color()    const { return common.border_top_color; }
		Colourb              border_right_color()  const { return common.border_right_color; }
		Colourb              border_bottom_color() const { return common.border_bottom_color; }
		Colourb              border_left_color()   const { return common.border_left_color; }
		bool                 has_decorator()       const { return common.has_decorator; }

		// -- Inherited --
		String         font_family()      const;
		String         cursor()           const;
		FontFaceHandle font_face_handle() const { return inherited.font_face_handle; }
		float          font_size()        const { return inherited.font_size; }
		float          letter_spacing()   const { return inherited.letter_spacing; }
		bool           has_font_effect()  const { return inherited.has_font_effect; }
		FontStyle      font_style()       const { return inherited.font_style; }
		FontWeight     font_weight()      const { return inherited.font_weight; }
		FontKerning    font_kerning()     const { return inherited.font_kerning; }
		PointerEvents  pointer_events()   const { return inherited.pointer_events; }
		Focus          focus()            const { return inherited.focus; }
		TextAlign      text_align()       const { return ResolveTextAlign(inherited.text_align, inherited.direction); }
		TextDecoration text_decoration()  const { return inherited.text_decoration; }
		TextTransform  text_transform()   const { return inherited.text_transform; }
		WhiteSpace     white_space()      const { return ToWhiteSpace(inherited.white_space_collapse, inherited.text_wrap_mode); }
		WordBreak      word_break()       const { return inherited.word_break; }
		Colourb        color()            const { return inherited.color; }
		LineHeight     line_height()      const { return LineHeight(inherited.line_height, inherited.line_height_inherit_type, inherited.line_height_inherit); }
		const String&  language()         const { return inherited.language; }
		Direction      direction()        const { return inherited.direction; }

		// -- Rare --
		MinWidth          min_width()                  const { return GetLengthPercentageAuto(rare.min_width_type, rare.min_width, ComputedCalculationSlot::MinWidth); }
		MaxWidth          max_width()                  const { return GetLengthPercentage(rare.max_width_type, rare.max_width, ComputedCalculationSlot::MaxWidth); }
		MinHeight         min_height()                 const { return GetLengthPercentageAuto(rare.min_height_type, rare.min_height, ComputedCalculationSlot::MinHeight); }
		MaxHeight         max_height()                 const { return GetLengthPercentage(rare.max_height_type, rare.max_height, ComputedCalculationSlot::MaxHeight); }
		VerticalAlign     vertical_align()             const { return VerticalAlign(rare.vertical_align_type, rare.vertical_align_length); }
		const             AnimationList* animation()   const;
		const             TransitionList* transition() const;
		float             perspective()                const { return rare.perspective; }
		PerspectiveOrigin perspective_origin_x()       const { return GetLengthPercentage(rare.perspective_origin_x_type, rare.perspective_origin_x, ComputedCalculationSlot::PerspectiveOriginX); }
		PerspectiveOrigin perspective_origin_y()       const { return GetLengthPercentage(rare.perspective_origin_y_type, rare.perspective_origin_y, ComputedCalculationSlot::PerspectiveOriginY); }
		TransformPtr      transform()                  const { return GetLocalProperty(PropertyId::Transform, TransformPtr()); }
		TransformOrigin   transform_origin_x()         const { return GetLengthPercentage(rare.transform_origin_x_type, rare.transform_origin_x, ComputedCalculationSlot::TransformOriginX); }
		TransformOrigin   transform_origin_y()         const { return GetLengthPercentage(rare.transform_origin_y_type, rare.transform_origin_y, ComputedCalculationSlot::TransformOriginY); }
		float             transform_origin_z()         const { return rare.transform_origin_z; }
		bool              has_local_transform()        const { return rare.has_local_transform || rare.has_translate || rare.has_rotate || rare.has_scale; }
		bool              has_local_perspective()      const { return rare.has_local_perspective; }
		AlignContent      align_content()              const { return rare.has_align_content ? GetLocalPropertyAlignment(PropertyId::AlignContent, AlignContent::Normal) : AlignContent::Normal; }
		AlignItems        align_items()                const { return GetLocalPropertyAlignment(PropertyId::AlignItems, AlignItems::Normal); }
		AlignSelf         align_self()                 const { return GetLocalPropertyAlignment(PropertyId::AlignSelf, AlignSelf::Auto); }
		FlexDirection     flex_direction()             const { return GetLocalPropertyKeyword(PropertyId::FlexDirection, FlexDirection::Row); }
		FlexWrap          flex_wrap()                  const { return GetLocalPropertyKeyword(PropertyId::FlexWrap, FlexWrap::Nowrap); }
		JustifyContent    justify_content()            const { return GetLocalPropertyAlignment(PropertyId::JustifyContent, JustifyContent::Normal); }
		float             flex_grow()                  const { return GetLocalNumericProperty(PropertyId::FlexGrow, 0.f); }
		float             flex_shrink()                const { return GetLocalNumericProperty(PropertyId::FlexShrink, 1.f); }
		FlexBasis         flex_basis()                 const { return GetLengthPercentageAuto(rare.flex_basis_type, rare.flex_basis, ComputedCalculationSlot::FlexBasis); }
		TextOverflow      text_overflow()              const { return rare.text_overflow; }
		String            text_overflow_string()       const { return GetLocalProperty(PropertyId::TextOverflow, String());; }
		Clip              clip()                       const { return rare.clip; }
		Drag              drag()                       const { return rare.drag; }
		TabIndex          tab_index()                  const { return rare.tab_index; }
		Colourb           image_color()                const { return rare.image_color; }
		LengthPercentage  row_gap()                    const { return GetLengthPercentage(rare.row_gap_type, rare.row_gap, ComputedCalculationSlot::RowGap); }
		LengthPercentage  column_gap()                 const { return GetLengthPercentage(rare.column_gap_type, rare.column_gap, ComputedCalculationSlot::ColumnGap); }
		OverscrollBehavior overscroll_behavior()       const { return rare.overscroll_behavior; }
		float             scrollbar_margin()           const { return rare.scrollbar_margin; }
		bool              has_mask_image()             const { return rare.has_mask_image; }
		bool              has_filter()                 const { return rare.has_filter; }
		bool              has_backdrop_filter()        const { return rare.has_backdrop_filter; }
		bool              has_box_shadow()             const { return rare.has_box_shadow; }

		// -- Phase 3: one section per package (see RareValues) --
		// Groundwork (p3-base)
		int               order()                      const { return rare.order; }
		uint8_t           will_change()                const { return rare.will_change; }
		JustifyItems      justify_items()              const { return rare.has_justify_items ? GetLocalPropertyAlignment(PropertyId::JustifyItems, JustifyItems::Legacy) : JustifyItems::Legacy; }
		JustifySelf       justify_self()               const { return rare.has_justify_self ? GetLocalPropertyAlignment(PropertyId::JustifySelf, JustifySelf::Auto) : JustifySelf::Auto; }
		OverflowAlignment align_content_overflow()     const { return rare.has_align_content ? GetLocalPropertyOverflowAlignment(PropertyId::AlignContent) : OverflowAlignment::Default; }
		OverflowAlignment align_items_overflow()       const { return GetLocalPropertyOverflowAlignment(PropertyId::AlignItems); }
		OverflowAlignment align_self_overflow()        const { return GetLocalPropertyOverflowAlignment(PropertyId::AlignSelf); }
		OverflowAlignment justify_content_overflow()   const { return GetLocalPropertyOverflowAlignment(PropertyId::JustifyContent); }
		OverflowAlignment justify_items_overflow()     const { return rare.has_justify_items ? GetLocalPropertyOverflowAlignment(PropertyId::JustifyItems) : OverflowAlignment::Default; }
		OverflowAlignment justify_self_overflow()      const { return rare.has_justify_self ? GetLocalPropertyOverflowAlignment(PropertyId::JustifySelf) : OverflowAlignment::Default; }

		// Values (p3-values)

		// Cascade (p3-cascade)

		// Paint (p3-paint)
		// A border radius as declared: corner 0-3 (top-left, top-right, bottom-right, bottom-left), axis 0 (horizontal)
		// or 1 (vertical): a length, a percentage or a calc() of both.
		const LengthPercentage& border_radius(int corner, int axis) const { return rare.border_radius_values[2 * corner + axis]; }
		bool              has_border_radius()          const { for (const LengthPercentage& value : rare.border_radius_values) { if (value.type != LengthPercentage::Length || value.value != 0.f) return true; } return false; }
		float             opacity()                    const { return rare.opacity; }
		Isolation         isolation()                  const { return rare.isolation; }
		bool              has_background_image()       const { return rare.has_background_image; }
		ObjectFit         object_fit()                 const { return rare.object_fit; }
		LengthPercentage  object_position_x()          const { return rare.object_position_x; }
		LengthPercentage  object_position_y()          const { return rare.object_position_y; }
		BorderStyle       outline_style()              const { return rare.outline_style; }
		float             outline_width()              const { return rare.outline_style == BorderStyle::None ? 0.f : rare.outline_width; }
		float             outline_offset()             const { return rare.outline_offset; }
		Colourb           outline_color()              const { return rare.outline_color_current ? inherited.color : rare.outline_color; }
		bool              outline_color_is_current()   const { return rare.outline_color_current; }

		// Text (p3-text)
		TextAlign          specified_text_align()      const { return inherited.text_align; }
		TextAlignLegacy    text_align_legacy()         const { return inherited.text_align_legacy; }
		WhiteSpaceCollapse white_space_collapse()      const { return inherited.white_space_collapse; }
		TextWrapMode       text_wrap_mode()            const { return inherited.text_wrap_mode; }
		OverflowWrap       overflow_wrap()             const { return inherited.overflow_wrap; }
		float              word_spacing()              const { return inherited.word_spacing; }
		bool               has_text_shadow()           const { return inherited.has_text_shadow; }

		// Layout (p3-layout)
		SizingKeyword     width_keyword()              const { return rare.width_keyword; }
		SizingKeyword     height_keyword()             const { return rare.height_keyword; }
		SizingKeyword     min_width_keyword()          const { return rare.min_width_keyword; }
		SizingKeyword     min_height_keyword()         const { return rare.min_height_keyword; }
		SizingKeyword     max_width_keyword()          const { return rare.max_width_keyword; }
		SizingKeyword     max_height_keyword()         const { return rare.max_height_keyword; }
		SizingKeyword     flex_basis_keyword()         const { return rare.flex_basis_keyword; }
		LengthPercentage  fit_content_width()          const { return LengthPercentage(rare.fit_content_width_type, rare.fit_content_width); }
		LengthPercentage  fit_content_height()         const { return LengthPercentage(rare.fit_content_height_type, rare.fit_content_height); }
		// The preferred aspect ratio (width / height), 0 for none ('aspect-ratio: auto' of a non-replaced element).
		float             aspect_ratio()               const { return rare.aspect_ratio; }
		// True for 'auto' and 'auto && <ratio>': a replaced element then uses its natural ratio when it has one.
		bool              aspect_ratio_auto()          const { return rare.aspect_ratio_auto; }
		float             overflow_clip_margin()       const { return rare.overflow_clip_margin; }
		ScrollbarGutter   scrollbar_gutter()           const { return rare.scrollbar_gutter; }
		int               flex_line_count()            const { return (int)GetLocalNumericProperty(PropertyId::FlexLineCount, 1.f); }

		// Animation (p3-animation)
		TransformPtr      translate()                  const { return rare.has_translate ? GetLocalProperty(PropertyId::Translate, TransformPtr()) : TransformPtr(); }
		TransformPtr      rotate()                     const { return rare.has_rotate ? GetLocalProperty(PropertyId::Rotate, TransformPtr()) : TransformPtr(); }
		TransformPtr      scale()                      const { return rare.has_scale ? GetLocalProperty(PropertyId::Scale, TransformPtr()) : TransformPtr(); }

		// Grid (p3-grid): the values of the template and placement properties are StyleValues (GridStyleValues.h), null
		// when the property has its initial value.
		StyleValuePtr     grid_template_columns()      const { return GetLocalProperty(PropertyId::GridTemplateColumns, StyleValuePtr()); }
		StyleValuePtr     grid_template_rows()         const { return GetLocalProperty(PropertyId::GridTemplateRows, StyleValuePtr()); }
		StyleValuePtr     grid_template_areas()        const { return GetLocalProperty(PropertyId::GridTemplateAreas, StyleValuePtr()); }
		StyleValuePtr     grid_auto_columns()          const { return GetLocalProperty(PropertyId::GridAutoColumns, StyleValuePtr()); }
		StyleValuePtr     grid_auto_rows()             const { return GetLocalProperty(PropertyId::GridAutoRows, StyleValuePtr()); }
		GridAutoFlow      grid_auto_flow()             const { return GetLocalPropertyKeyword(PropertyId::GridAutoFlow, GridAutoFlow::Row); }
		StyleValuePtr     grid_row_start()             const { return GetLocalProperty(PropertyId::GridRowStart, StyleValuePtr()); }
		StyleValuePtr     grid_row_end()               const { return GetLocalProperty(PropertyId::GridRowEnd, StyleValuePtr()); }
		StyleValuePtr     grid_column_start()          const { return GetLocalProperty(PropertyId::GridColumnStart, StyleValuePtr()); }
		StyleValuePtr     grid_column_end()            const { return GetLocalProperty(PropertyId::GridColumnEnd, StyleValuePtr()); }

		// -- Assignment --
		// Common
		void width              (LengthPercentageAuto value) { StoreLengthPercentageAuto(ComputedCalculationSlot::Width, value); common.width_type = value.type; common.width_value = value.value; }
		void height             (LengthPercentageAuto value) { StoreLengthPercentageAuto(ComputedCalculationSlot::Height, value); common.height_type = value.type; common.height_value = value.value; }
		void margin_top         (LengthPercentageAuto value) { StoreLengthPercentageAuto(ComputedCalculationSlot::MarginTop, value); common.margin_top_type = value.type; common.margin_top_value = value.value; }
		void margin_right       (LengthPercentageAuto value) { StoreLengthPercentageAuto(ComputedCalculationSlot::MarginRight, value); common.margin_right_type = value.type; common.margin_right_value = value.value; }
		void margin_bottom      (LengthPercentageAuto value) { StoreLengthPercentageAuto(ComputedCalculationSlot::MarginBottom, value); common.margin_bottom_type = value.type; common.margin_bottom_value = value.value; }
		void margin_left        (LengthPercentageAuto value) { StoreLengthPercentageAuto(ComputedCalculationSlot::MarginLeft, value); common.margin_left_type = value.type; common.margin_left_value = value.value; }
		void padding_top        (LengthPercentage value)     { StoreLengthPercentage(ComputedCalculationSlot::PaddingTop, value); common.padding_top_type = value.type; common.padding_top_value = value.value; }
		void padding_right      (LengthPercentage value)     { StoreLengthPercentage(ComputedCalculationSlot::PaddingRight, value); common.padding_right_type = value.type; common.padding_right_value = value.value; }
		void padding_bottom     (LengthPercentage value)     { StoreLengthPercentage(ComputedCalculationSlot::PaddingBottom, value); common.padding_bottom_type = value.type; common.padding_bottom_value = value.value; }
		void padding_left       (LengthPercentage value)     { StoreLengthPercentage(ComputedCalculationSlot::PaddingLeft, value); common.padding_left_type = value.type; common.padding_left_value = value.value; }
		void top                (LengthPercentageAuto value) { StoreLengthPercentageAuto(ComputedCalculationSlot::Top, value); common.top_type = value.type; common.top_value = value.value; }
		void right              (LengthPercentageAuto value) { StoreLengthPercentageAuto(ComputedCalculationSlot::Right, value); common.right_type = value.type; common.right_value = value.value; }
		void bottom             (LengthPercentageAuto value) { StoreLengthPercentageAuto(ComputedCalculationSlot::Bottom, value); common.bottom_type = value.type; common.bottom_value = value.value; }
		void left               (LengthPercentageAuto value) { StoreLengthPercentageAuto(ComputedCalculationSlot::Left, value); common.left_type = value.type; common.left_value = value.value; }
		void z_index            (NumberAuto value)           { common.z_index_type        = value.type; common.z_index_value        = value.value; }
		void border_top_width   (float value)                { common.border_top_width    = value; }
		void border_right_width (float value)                { common.border_right_width  = value; }
		void border_bottom_width(float value)                { common.border_bottom_width = value; }
		void border_left_width  (float value)                { common.border_left_width   = value; }
		void border_top_style   (BorderStyle value)          { common.border_top_style    = value; }
		void border_right_style (BorderStyle value)          { common.border_right_style  = value; }
		void border_bottom_style(BorderStyle value)          { common.border_bottom_style = value; }
		void border_left_style  (BorderStyle value)          { common.border_left_style   = value; }
		void box_sizing         (BoxSizing value)            { common.box_sizing          = value; }
		void display            (Display value)              { common.display             = value; }
		void position           (Position value)             { common.position            = value; }
		void float_             (Float value)                { common.float_              = value; }
		void clear              (Clear value)                { common.clear               = value; }
		void overflow_x         (Overflow value)             { common.overflow_x          = value; }
		void overflow_y         (Overflow value)             { common.overflow_y          = value; }
		void visibility         (Visibility value)           { inherited.visibility       = value; }
		void background_color   (Colourb value)              { common.background_color    = value; }
		void border_top_color   (Colourb value)              { common.border_top_color    = value; }
		void border_right_color (Colourb value)              { common.border_right_color  = value; }
		void border_bottom_color(Colourb value)              { common.border_bottom_color = value; }
		void border_left_color  (Colourb value)              { common.border_left_color   = value; }
		void has_decorator      (bool value)                 { common.has_decorator       = value; }
		// Inherited
		void font_face_handle  (FontFaceHandle value) { inherited.font_face_handle   = value; }
		void font_size         (float value)          { inherited.font_size          = value; }
		void has_letter_spacing(bool value)           { inherited.has_letter_spacing = value; }
		void has_font_effect   (bool value)           { inherited.has_font_effect    = value; }
		void font_style        (FontStyle value)      { inherited.font_style         = value; }
		void font_weight       (FontWeight value)     { inherited.font_weight        = value; }
		void font_kerning      (FontKerning value)    { inherited.font_kerning       = value; }
		void pointer_events    (PointerEvents value)  { inherited.pointer_events     = value; }
		void focus             (Focus value)          { inherited.focus              = value; }
		void text_align        (TextAlign value)      { inherited.text_align         = value; }
		void text_align_legacy (TextAlignLegacy value) { inherited.text_align_legacy = value; }
		void text_decoration   (TextDecoration value) { inherited.text_decoration    = value; }
		void text_transform    (TextTransform value)  { inherited.text_transform     = value; }
		void word_break        (WordBreak value)      { inherited.word_break         = value; }
		void color             (Colourb value)        { inherited.color              = value; }
		void line_height       (LineHeight value)     { inherited.line_height = value.value; inherited.line_height_inherit_type = value.inherit_type; inherited.line_height_inherit = value.inherit_value;  }
		void language          (const String& value)  { inherited.language           = value; }
		void direction         (Direction value)      { inherited.direction          = value; }
		// Rare
		void min_width                 (MinWidth value)          { StoreLengthPercentageAuto(ComputedCalculationSlot::MinWidth, value); rare.min_width_type = value.type; rare.min_width = value.value; }
		void max_width                 (MaxWidth value)          { StoreLengthPercentage(ComputedCalculationSlot::MaxWidth, value); rare.max_width_type = value.type; rare.max_width = value.value; }
		void min_height                (MinHeight value)         { StoreLengthPercentageAuto(ComputedCalculationSlot::MinHeight, value); rare.min_height_type = value.type; rare.min_height = value.value; }
		void max_height                (MaxHeight value)         { StoreLengthPercentage(ComputedCalculationSlot::MaxHeight, value); rare.max_height_type = value.type; rare.max_height = value.value; }
		void vertical_align            (VerticalAlign value)     { rare.vertical_align_type        = value.type; rare.vertical_align_length      = value.value; }
		void perspective_origin_x      (PerspectiveOrigin value) { StoreLengthPercentage(ComputedCalculationSlot::PerspectiveOriginX, value); rare.perspective_origin_x_type = value.type; rare.perspective_origin_x = value.value; }
		void perspective_origin_y      (PerspectiveOrigin value) { StoreLengthPercentage(ComputedCalculationSlot::PerspectiveOriginY, value); rare.perspective_origin_y_type = value.type; rare.perspective_origin_y = value.value; }
		void transform_origin_x        (TransformOrigin value)   { StoreLengthPercentage(ComputedCalculationSlot::TransformOriginX, value); rare.transform_origin_x_type = value.type; rare.transform_origin_x = value.value; }
		void transform_origin_y        (TransformOrigin value)   { StoreLengthPercentage(ComputedCalculationSlot::TransformOriginY, value); rare.transform_origin_y_type = value.type; rare.transform_origin_y = value.value; }
		void row_gap                   (LengthPercentage value)  { StoreLengthPercentage(ComputedCalculationSlot::RowGap, value); rare.row_gap_type = value.type; rare.row_gap = value.value; }
		void column_gap                (LengthPercentage value)  { StoreLengthPercentage(ComputedCalculationSlot::ColumnGap, value); rare.column_gap_type = value.type; rare.column_gap = value.value; }
		void flex_basis                (FlexBasis value)         { StoreLengthPercentageAuto(ComputedCalculationSlot::FlexBasis, value); rare.flex_basis_type = value.type; rare.flex_basis = value.value; }
		void transform_origin_z        (float value)             { rare.transform_origin_z         = value; }
		void perspective               (float value)             { rare.perspective                = value; }
		void has_local_perspective     (bool value)              { rare.has_local_perspective      = value; }
		void has_local_transform       (bool value)              { rare.has_local_transform        = value; }
		void text_overflow             (TextOverflow value)      { rare.text_overflow              = value; }
		void clip                      (Clip value)              { rare.clip                       = value; }
		void drag                      (Drag value)              { rare.drag                       = value; }
		void tab_index                 (TabIndex value)          { rare.tab_index                  = value; }
		void image_color               (Colourb value)           { rare.image_color                = value; }
		void overscroll_behavior       (OverscrollBehavior value){ rare.overscroll_behavior        = value; }
		void scrollbar_margin          (float value)             { rare.scrollbar_margin           = value; }
		void has_mask_image            (bool value)              { rare.has_mask_image             = value; }
		void has_filter                (bool value)              { rare.has_filter                 = value; }
		void has_backdrop_filter       (bool value)              { rare.has_backdrop_filter        = value; }
		void has_box_shadow            (bool value)              { rare.has_box_shadow             = value; }

		// Phase 3: one section per package (see RareValues)
		// Groundwork (p3-base)
		void order                     (int value)               { rare.order                      = value; }
		void will_change               (uint8_t value)           { rare.will_change                = value; }
		void has_justify_items         (bool value)              { rare.has_justify_items          = value; }
		void has_justify_self          (bool value)              { rare.has_justify_self           = value; }
		void has_align_content         (bool value)              { rare.has_align_content          = value; }

		// Values (p3-values)

		// Cascade (p3-cascade)

		// Paint (p3-paint)
		void opacity                   (float value)                { rare.opacity = value; }
		void isolation                 (Isolation value)            { rare.isolation = value; }
		void has_background_image      (bool value)                 { rare.has_background_image = value; }
		void object_fit                (ObjectFit value)            { rare.object_fit = value; }
		void object_position_x         (LengthPercentage value)     { rare.object_position_x = std::move(value); }
		void object_position_y         (LengthPercentage value)     { rare.object_position_y = std::move(value); }
		void outline_style             (BorderStyle value)          { rare.outline_style = value; }
		void outline_width             (float value)                { rare.outline_width = value; }
		void outline_offset            (float value)                { rare.outline_offset = value; }
		void outline_color             (Colourb value)              { rare.outline_color = value; rare.outline_color_current = false; }
		void outline_color_current     ()                           { rare.outline_color_current = true; }
		void border_radius             (int corner, int axis, LengthPercentage value) { rare.border_radius_values[2 * corner + axis] = std::move(value); }

		// Text (p3-text)
		void white_space_collapse      (WhiteSpaceCollapse value) { inherited.white_space_collapse = value; }
		void text_wrap_mode            (TextWrapMode value)       { inherited.text_wrap_mode       = value; }
		void overflow_wrap             (OverflowWrap value)       { inherited.overflow_wrap        = value; }
		void letter_spacing            (float value)              { inherited.letter_spacing       = value; }
		void word_spacing              (float value)              { inherited.word_spacing         = value; }
		void has_text_shadow           (bool value)               { inherited.has_text_shadow      = value; }

		// Layout (p3-layout)
		void width_keyword             (SizingKeyword value)     { rare.width_keyword              = value; }
		void height_keyword            (SizingKeyword value)     { rare.height_keyword             = value; }
		void min_width_keyword         (SizingKeyword value)     { rare.min_width_keyword          = value; }
		void min_height_keyword        (SizingKeyword value)     { rare.min_height_keyword         = value; }
		void max_width_keyword         (SizingKeyword value)     { rare.max_width_keyword          = value; }
		void max_height_keyword        (SizingKeyword value)     { rare.max_height_keyword         = value; }
		void flex_basis_keyword        (SizingKeyword value)     { rare.flex_basis_keyword         = value; }
		void fit_content_width         (LengthPercentage value)  { rare.fit_content_width_type = value.type; rare.fit_content_width = value.value; }
		void fit_content_height        (LengthPercentage value)  { rare.fit_content_height_type = value.type; rare.fit_content_height = value.value; }
		void aspect_ratio              (float value, bool is_auto) { rare.aspect_ratio = value; rare.aspect_ratio_auto = is_auto; }
		void overflow_clip_margin      (float value)             { rare.overflow_clip_margin       = value; }
		void scrollbar_gutter          (ScrollbarGutter value)   { rare.scrollbar_gutter           = value; }

		// Animation (p3-animation)
		void has_translate             (bool value)              { rare.has_translate              = value; }
		void has_rotate                (bool value)              { rare.has_rotate                 = value; }
		void has_scale                 (bool value)              { rare.has_scale                  = value; }

		// Grid (p3-grid)

		// clang-format on

		// -- Management --
		void CopyNonInherited(const ComputedValues& other)
		{
			common = other.common;
			rare = other.rare;
#ifdef RMLUI_MATH_EXPRESSIONS
			if (calculations || other.calculations)
				CopyNonInheritedCalculations(other);
#endif
		}
		void CopyInherited(const ComputedValues& parent) { inherited = parent.inherited; }

	private:
		enum class ComputedCalculationSlot : uint8_t {
			Width,
			Height,
			MarginTop,
			MarginRight,
			MarginBottom,
			MarginLeft,
			PaddingTop,
			PaddingRight,
			PaddingBottom,
			PaddingLeft,
			Top,
			Right,
			Bottom,
			Left,
			MinWidth,
			MaxWidth,
			MinHeight,
			MaxHeight,
			PerspectiveOriginX,
			PerspectiveOriginY,
			TransformOriginX,
			TransformOriginY,
			FlexBasis,
			RowGap,
			ColumnGap
		};
#ifdef RMLUI_MATH_EXPRESSIONS
		LengthPercentage GetLengthPercentage(LengthPercentage::Type type, float value, ComputedCalculationSlot slot) const
		{
			if (type == LengthPercentage::Calculation)
				return LengthPercentage(GetCalculation(slot));
			return LengthPercentage(type, value);
		}
		LengthPercentageAuto GetLengthPercentageAuto(LengthPercentageAuto::Type type, float value, ComputedCalculationSlot slot) const
		{
			if (type == LengthPercentageAuto::Calculation)
				return LengthPercentageAuto(GetCalculation(slot));
			return LengthPercentageAuto(type, value);
		}
		void StoreLengthPercentage(ComputedCalculationSlot slot, LengthPercentage& value)
		{
			if (value.type == LengthPercentage::Calculation)
			{
				RMLUI_ASSERT(value.calculation);
				SetCalculation(slot, value.calculation);
				value.value = 0;
			}
			else if (calculations)
				SetCalculation(slot, nullptr);
		}
		void StoreLengthPercentageAuto(ComputedCalculationSlot slot, LengthPercentageAuto& value)
		{
			if (value.type == LengthPercentageAuto::Calculation)
			{
				RMLUI_ASSERT(value.calculation);
				SetCalculation(slot, value.calculation);
				value.value = 0;
			}
			else if (calculations)
				SetCalculation(slot, nullptr);
		}
		CalculationPtr GetCalculation(ComputedCalculationSlot slot) const;
		void SetCalculation(ComputedCalculationSlot slot, CalculationPtr calculation);
		void CopyNonInheritedCalculations(const ComputedValues& other);
#else
		LengthPercentage GetLengthPercentage(LengthPercentage::Type type, float value, ComputedCalculationSlot) const
		{
			return LengthPercentage(type, value);
		}
		LengthPercentageAuto GetLengthPercentageAuto(LengthPercentageAuto::Type type, float value, ComputedCalculationSlot) const
		{
			return LengthPercentageAuto(type, value);
		}
		void StoreLengthPercentage(ComputedCalculationSlot, LengthPercentage&) {}
		void StoreLengthPercentageAuto(ComputedCalculationSlot, LengthPercentageAuto&) {}
#endif
		template <typename T>
		inline T GetLocalPropertyKeyword(PropertyId id, T default_value) const
		{
			if (auto p = GetLocalPropertyWithResolvedVariables(id))
				return static_cast<T>(p->Get<int>());
			return default_value;
		}
		// Alignment keywords carry their 'safe'/'unsafe' prefix above the low byte (PropertyParserAlignment).
		template <typename T>
		inline T GetLocalPropertyAlignment(PropertyId id, T default_value) const
		{
			if (auto p = GetLocalPropertyWithResolvedVariables(id))
				return static_cast<T>(p->Get<int>() & 0xff);
			return default_value;
		}
		inline OverflowAlignment GetLocalPropertyOverflowAlignment(PropertyId id) const
		{
			if (auto p = GetLocalPropertyWithResolvedVariables(id))
				return static_cast<OverflowAlignment>((p->Get<int>() >> 8) & 0x3);
			return OverflowAlignment::Default;
		}
		template <typename T>
		inline T GetLocalProperty(PropertyId id, T default_value) const
		{
			if (auto p = GetLocalPropertyWithResolvedVariables(id))
				return p->Get<T>();
			return default_value;
		}
		float GetLocalNumericProperty(PropertyId id, float default_value) const;

		const Property* GetLocalPropertyWithResolvedVariables(PropertyId id) const;

		Element* element = nullptr;

		CommonValues common;
		InheritedValues inherited;
		RareValues rare;
#ifdef RMLUI_MATH_EXPRESSIONS
		UniquePtr<ComputedCalculationStore> calculations;
#endif
	};

} // namespace Style

// Resolves a computed LengthPercentage(Auto) value to the base unit 'px'.
// Percentages are scaled by the base value, if definite (>= 0), otherwise return the default value.
// Auto lengths always return the default value.
RMLUICORE_API float ResolveValueOr(Style::LengthPercentageAuto length, float base_value, float default_value);
RMLUICORE_API float ResolveValueOr(Style::LengthPercentage length, float base_value, float default_value);

RMLUICORE_API_INLINE float ResolveValue(Style::LengthPercentageAuto length, float base_value)
{
#ifdef RMLUI_MATH_EXPRESSIONS
	// Keep ordinary used values entirely inline when calculations are enabled. This avoids routing
	// the larger calculation-capable value object through the out-of-line fallback on the dominant
	// Length/Percentage/Auto paths.
	if (length.type == Style::LengthPercentageAuto::Length)
		return length.value;
	if (length.type == Style::LengthPercentageAuto::Percentage)
		return base_value >= 0.f ? length.value * 0.01f * base_value : 0.f;
	if (length.type == Style::LengthPercentageAuto::Auto)
		return 0.f;
#endif
	return ResolveValueOr(length, base_value, 0.f);
}
RMLUICORE_API_INLINE float ResolveValue(Style::LengthPercentage length, float base_value)
{
#ifdef RMLUI_MATH_EXPRESSIONS
	if (length.type == Style::LengthPercentage::Length)
		return length.value;
	if (length.type == Style::LengthPercentage::Percentage)
		return base_value >= 0.f ? length.value * 0.01f * base_value : 0.f;
#endif
	return ResolveValueOr(length, base_value, 0.f);
}

using ComputedValues = Style::ComputedValues;

} // namespace Rml
