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
			left_type(LengthPercentageAuto::Auto), z_index_type(NumberAuto::Auto)
		{}

		Display display : 5;
		Position position : 3;

		Float float_ : 2;
		Clear clear : 2;

		Overflow overflow_x : 2, overflow_y : 2;

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

		uint16_t border_top_width = 0, border_right_width = 0, border_bottom_width = 0, border_left_width = 0;

		// The initial value is 'currentcolor': an element's border colors not declared follow its color (ElementStyle::ComputeValues).
		Colourb border_top_color{0, 0, 0}, border_right_color{0, 0, 0}, border_bottom_color{0, 0, 0}, border_left_color{0, 0, 0};

		Colourb background_color = Colourb(0, 0, 0, 0);
	};

	struct InheritedValues {
		InheritedValues() :
			font_weight(FontWeight::Normal), font_kerning(FontKerning::Auto), has_letter_spacing(0), font_style(FontStyle::Normal),
			has_font_effect(false), pointer_events(PointerEvents::Auto), focus(Focus::Auto), text_align(TextAlign::Start),
			text_decoration(TextDecoration::None), text_transform(TextTransform::None), white_space_collapse(WhiteSpaceCollapse::Collapse),
			text_wrap_mode(TextWrapMode::Wrap), word_break(WordBreak::Normal), direction(Direction::Auto), visibility(Visibility::Visible),
			line_height_inherit_type(LineHeight::Number)
		{}

		// Font face used to render text and resolve ex properties. Does not represent a true property
		// like most computed values, but placed here as it is used and inherited in a similar manner.
		FontFaceHandle font_face_handle = 0;

		float font_size = 16.f;

		// The opacity the element draws with: its own (CSS: not inherited) times its parent's, until opacity groups
		// composite whole subtrees (phase 3, paint).
		float opacity = 1;
		Colourb color = Colourb(0, 0, 0);

		FontWeight font_weight : 10;
		FontKerning font_kerning : 2;
		uint16_t has_letter_spacing : 1;

		FontStyle font_style : 1;
		bool has_font_effect : 1;
		PointerEvents pointer_events : 1;
		Focus focus : 1;

		TextAlign text_align : 3;
		TextDecoration text_decoration : 3;
		TextTransform text_transform : 2;
		WhiteSpaceCollapse white_space_collapse : 3;
		TextWrapMode text_wrap_mode : 1;
		WordBreak word_break : 2;

		Direction direction : 2;

		Visibility visibility : 1;

		LineHeight::InheritType line_height_inherit_type : 1;
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

		int16_t border_top_left_radius = 0, border_top_right_radius = 0, border_bottom_right_radius = 0, border_bottom_left_radius = 0;
		Colourb image_color = Colourb(255, 255, 255);
		float scrollbar_margin = 0.f;

		/*
		  Phase 3 sections (docs/html-ui/phase-3-fork-patches.md 3.0, 3.1.3): each package adds its computed fields in its
		  own section below, and only there (the matching accessors go into the same section of ComputedValues).
		*/

		// -- Groundwork (p3-base) --
		int order = 0;

		// -- Values (p3-values) --

		// -- Cascade (p3-cascade) --

		// -- Paint (p3-paint) --

		// -- Text (p3-text) --

		// -- Layout (p3-layout) --

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
		float                border_top_width()    const { return (float)common.border_top_width; }
		float                border_right_width()  const { return (float)common.border_right_width; }
		float                border_bottom_width() const { return (float)common.border_bottom_width; }
		float                border_left_width()   const { return (float)common.border_left_width; }
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
		float          opacity()          const { return inherited.opacity; }
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
		AlignContent      align_content()              const { return GetLocalPropertyAlignment(PropertyId::AlignContent, AlignContent::Normal); }
		AlignItems        align_items()                const { return GetLocalPropertyAlignment(PropertyId::AlignItems, AlignItems::Normal); }
		AlignSelf         align_self()                 const { return GetLocalPropertyAlignment(PropertyId::AlignSelf, AlignSelf::Auto); }
		FlexDirection     flex_direction()             const { return GetLocalPropertyKeyword(PropertyId::FlexDirection, FlexDirection::Row); }
		FlexWrap          flex_wrap()                  const { return GetLocalPropertyKeyword(PropertyId::FlexWrap, FlexWrap::Nowrap); }
		JustifyContent    justify_content()            const { return GetLocalPropertyAlignment(PropertyId::JustifyContent, JustifyContent::Normal); }
		float             flex_grow()                  const { return GetLocalNumericProperty(PropertyId::FlexGrow, 0.f); }
		float             flex_shrink()                const { return GetLocalNumericProperty(PropertyId::FlexShrink, 1.f); }
		FlexBasis         flex_basis()                 const { return GetLengthPercentageAuto(rare.flex_basis_type, rare.flex_basis, ComputedCalculationSlot::FlexBasis); }
		float             border_top_left_radius()     const { return (float)rare.border_top_left_radius; }
		float             border_top_right_radius()    const { return (float)rare.border_top_right_radius; }
		float             border_bottom_right_radius() const { return (float)rare.border_bottom_right_radius; }
		float             border_bottom_left_radius()  const { return (float)rare.border_bottom_left_radius; }
		CornerSizes       border_radius()              const { return {(float)rare.border_top_left_radius,     (float)rare.border_top_right_radius,
		                                                               (float)rare.border_bottom_right_radius, (float)rare.border_bottom_left_radius}; }
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
		JustifyItems      justify_items()              const { return GetLocalPropertyAlignment(PropertyId::JustifyItems, JustifyItems::Legacy); }
		JustifySelf       justify_self()               const { return GetLocalPropertyAlignment(PropertyId::JustifySelf, JustifySelf::Auto); }
		OverflowAlignment align_content_overflow()     const { return GetLocalPropertyOverflowAlignment(PropertyId::AlignContent); }
		OverflowAlignment align_items_overflow()       const { return GetLocalPropertyOverflowAlignment(PropertyId::AlignItems); }
		OverflowAlignment align_self_overflow()        const { return GetLocalPropertyOverflowAlignment(PropertyId::AlignSelf); }
		OverflowAlignment justify_content_overflow()   const { return GetLocalPropertyOverflowAlignment(PropertyId::JustifyContent); }
		OverflowAlignment justify_items_overflow()     const { return GetLocalPropertyOverflowAlignment(PropertyId::JustifyItems); }
		OverflowAlignment justify_self_overflow()      const { return GetLocalPropertyOverflowAlignment(PropertyId::JustifySelf); }

		// Values (p3-values)

		// Cascade (p3-cascade)

		// Paint (p3-paint)

		// Text (p3-text)
		TextAlign          specified_text_align()      const { return inherited.text_align; }
		WhiteSpaceCollapse white_space_collapse()      const { return inherited.white_space_collapse; }
		TextWrapMode       text_wrap_mode()            const { return inherited.text_wrap_mode; }
		OverflowWrap       overflow_wrap()             const { return inherited.overflow_wrap; }
		float              word_spacing()              const { return inherited.word_spacing; }
		bool               has_text_shadow()           const { return inherited.has_text_shadow; }

		// Layout (p3-layout)

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
		void border_top_width   (int16_t value)              { common.border_top_width    = value; }
		void border_right_width (int16_t value)              { common.border_right_width  = value; }
		void border_bottom_width(int16_t value)              { common.border_bottom_width = value; }
		void border_left_width  (int16_t value)              { common.border_left_width   = value; }
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
		void text_decoration   (TextDecoration value) { inherited.text_decoration    = value; }
		void text_transform    (TextTransform value)  { inherited.text_transform     = value; }
		void word_break        (WordBreak value)      { inherited.word_break         = value; }
		void color             (Colourb value)        { inherited.color              = value; }
		void opacity           (float value)          { inherited.opacity            = value; }
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
		void border_top_left_radius    (float value)             { rare.border_top_left_radius     = (int16_t)value; }
		void border_top_right_radius   (float value)             { rare.border_top_right_radius    = (int16_t)value; }
		void border_bottom_right_radius(float value)             { rare.border_bottom_right_radius = (int16_t)value; }
		void border_bottom_left_radius (float value)             { rare.border_bottom_left_radius  = (int16_t)value; }
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

		// Values (p3-values)

		// Cascade (p3-cascade)

		// Paint (p3-paint)

		// Text (p3-text)
		void white_space_collapse      (WhiteSpaceCollapse value) { inherited.white_space_collapse = value; }
		void text_wrap_mode            (TextWrapMode value)       { inherited.text_wrap_mode       = value; }
		void overflow_wrap             (OverflowWrap value)       { inherited.overflow_wrap        = value; }
		void letter_spacing            (float value)              { inherited.letter_spacing       = value; }
		void word_spacing              (float value)              { inherited.word_spacing         = value; }
		void has_text_shadow           (bool value)               { inherited.has_text_shadow      = value; }

		// Layout (p3-layout)

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
