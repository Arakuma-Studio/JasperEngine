#include "../../Include/RmlUi/Core/ElementText.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include "../../Include/RmlUi/Core/Context.h"
#include "../../Include/RmlUi/Core/Core.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/ElementUtilities.h"
#include "../../Include/RmlUi/Core/Elements/ElementFormControl.h"
#include "../../Include/RmlUi/Core/Event.h"
#include "../../Include/RmlUi/Core/FontEngineInterface.h"
#include "../../Include/RmlUi/Core/MeshUtilities.h"
#include "../../Include/RmlUi/Core/Profiling.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/RenderManager.h"
#include "../../Include/RmlUi/Core/StyleValue.h"
#include "../../Include/RmlUi/Core/TextShapingContext.h"
#include "ComputeProperty.h"
#include "ElementDefinition.h"
#include "ElementStyle.h"
#include "LineClampEllipsis.h"
#include "Layout/LayoutDetails.h"
#include "TextShadow.h"
#include "TextUnicode.h"
#include "TransformState.h"
#include <limits>

namespace Rml {

// How the text of an element is cut into tokens: words, white space, and the pieces of words between line-break
// opportunities (CSS Text 3 §5: after a zero-width space, a soft hyphen or a hyphen, around ideographs).
struct TokenRules {
	bool collapse_white_space;
	bool break_at_endline;
	bool break_spaces; // 'white-space-collapse: break-spaces': a line-break opportunity after every preserved space
	Style::TextTransform text_transform;
	Style::WordBreak word_break;
	bool decode_escape_characters;
	const char* text_begin; // the element's text, read back for 'capitalize'
	const uint8_t* line_breaks; // the line-break opportunities of the text, by byte offset from 'text_begin'; null: ASCII
	Character context_before;   // the inline formatting context's code point before the text (Null: none)
};

static bool BuildToken(String& token, const char*& token_begin, const char* string_end, bool first_token, const TokenRules& rules);
using LineBreakContext = ElementText::LineBreakContext;
static void ComputeLineBreaks(const String& text, bool decode_escape_characters, Style::WordBreak word_break, bool ascii, const LineBreakContext& prefix,
	const LineBreakContext& suffix, Vector<uint8_t>& out_breaks, bool& out_break_before, bool& out_break_after);
static void GatherLineBreakContext(ElementText* text, bool forward, bool decode_escape_characters, int limit, LineBreakContext& out_context);
static bool IsAsciiText(const String& text, bool decode_escape_characters);
static bool IsTransparentInlineBox(Element* element);
static bool IsSkippedInContext(Element* element);
static float InlineBoxEdge(Element* element, bool left);
static bool IsWhiteSpaceToken(const String& token);
static bool LastToken(const char* token_begin, const char* string_end, bool collapse_white_space, bool break_at_endline);

static int RoundDownToIntegerClamped(float value)
{
	constexpr int clamp = (1 << std::numeric_limits<float>::digits);
	constexpr float clamp_f = float{clamp};
	if (value >= clamp_f)
		return clamp;
	if (value <= -clamp_f)
		return -clamp;
	return static_cast<int>(value);
}

#if defined(_MSC_VER)
	#define RMLUI_TEXT_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
	#define RMLUI_TEXT_NOINLINE __attribute__((noinline))
#else
	#define RMLUI_TEXT_NOINLINE
#endif

// The token, which ends the element's text with a collapsible space, fits in 'max_width' without that space (out of the
// line breaker's loop: rare).
RMLUI_TEXT_NOINLINE static bool FitsWithoutTrailingSpace(const String& token, int max_width, FontFaceHandle font_face_handle, const TextShapingContext& context,
	Character previous_codepoint)
{
	if (token.size() < 2 || token.back() != ' ')
		return false;
	const StringView word(token.data(), token.data() + token.size() - 1);
	return GetFontEngineInterface()->GetStringWidth(font_face_handle, word, context, previous_codepoint) <= max_width;
}

// A line that ends at a soft hyphen shows a hyphen (CSS Text 3 §5.4, 'hyphens: manual').
static void AppendHyphenAfterSoftHyphen(String& line, float& line_width, FontFaceHandle font_face_handle, const TextShapingContext& context)
{
	// U+00AD in UTF-8
	if (line.size() < 2 || (unsigned char)line[line.size() - 2] != 0xC2 || (unsigned char)line.back() != 0xAD)
		return;
	line += '-';
	line_width += (float)GetFontEngineInterface()->GetStringWidth(font_face_handle, "-", context);
}

struct TextOverflowResolved {
	bool enabled = false;
	float overflow_edge = 0.f; // where the text is clipped, in the text element's space
	String overflow_text;
	Colourb colour; // the overflow text takes the block container's colour
};

// 'text-overflow' of the block container the text is laid out in (CSS Overflow 4 §3: it applies to block containers and
// affects the text of their inline descendants too), when it clips its overflow.
static TextOverflowResolved ResolveTextOverflow(ElementText* text_element, FontFaceHandle font_face_handle)
{
	Element* container = text_element->GetParentNode();
	while (container && container->GetDisplay() == Style::Display::Inline)
		container = container->GetParentNode();
	if (!container)
		return {};

	const auto& container_computed = container->GetComputedValues();
	if (container_computed.overflow_x() == Style::Overflow::Visible && container_computed.overflow_y() == Style::Overflow::Visible)
		return {};

	const Style::TextOverflow text_overflow = container_computed.text_overflow();
	if (text_overflow == Style::TextOverflow::Clip)
		return {};

	const Box& box = container->GetBox();
	const BoxArea clip_area = container->GetClipArea();

	auto AccumulateRightSideEdgesUpTo = [](const Box& box, BoxArea up_to_area) -> float {
		float result = 0;
		for (int i = (int)up_to_area; i < int(BoxArea::Content); i++)
			result += box.GetEdge((BoxArea)i, BoxEdge::Right);
		return result;
	};

	// The right edge of the clipping area, from the text element's position.
	const float clip_edge = container->GetAbsoluteOffset(BoxArea::Content).x + box.GetSize().x + AccumulateRightSideEdgesUpTo(box, clip_area);
	const float overflow_edge = clip_edge - text_element->GetAbsoluteOffset(BoxArea::Border).x;

	constexpr char ellipsis_chars[] = "\xE2\x80\xA6"; // U+2026
	constexpr char dots_chars[] = "...";

	String overflow_text = (text_overflow == Style::TextOverflow::String
			? container_computed.text_overflow_string()
			: (GetFontEngineInterface()->GetFontMetrics(font_face_handle).has_ellipsis ? ellipsis_chars : dots_chars));

	return TextOverflowResolved{
		true,
		overflow_edge,
		std::move(overflow_text),
		container_computed.color(),
	};
}

static LengthPercentageValue LengthPercentageValueOf(const Property& property)
{
#ifdef RMLUI_MATH_EXPRESSIONS
	if (property.unit == Unit::CALCULATION)
	{
		LengthPercentageValue value;
		value.calculation = property.value.Get<CalculationPtr>();
		return value;
	}
#endif
	return LengthPercentageValue(property.GetNumericValue());
}

static void AppendMesh(Mesh& to, const Mesh& from)
{
	const int first = (int)to.vertices.size();
	to.vertices.insert(to.vertices.end(), from.vertices.begin(), from.vertices.end());
	to.indices.reserve(to.indices.size() + from.indices.size());
	for (int index : from.indices)
		to.indices.push_back(first + index);
}

// The element whose 'text-decoration-line' the text draws (its decorating box): the nearest one that declares it.
static Element* GetDecoratingElement(ElementText* text_element)
{
	for (Element* element = text_element->GetParentNode(); element; element = element->GetParentNode())
	{
		if (element->GetLocalProperty(PropertyId::TextDecoration))
			return element;
	}
	return text_element->GetParentNode();
}

// The colour of the decoration lines: 'text-decoration-color' of the decorating box, 'currentcolor' its colour.
static Colourb GetDecorationColour(ElementText* text_element)
{
	Element* decorating = GetDecoratingElement(text_element);
	if (!decorating)
		return text_element->GetComputedValues().color();
	const Property* property = decorating->GetLocalProperty(PropertyId::TextDecorationColor);
	if (!property)
		property = decorating->GetProperty(PropertyId::TextDecorationColor);
	if (property && property->unit == Unit::COLOUR)
		return property->Get<Colourb>();
	return decorating->GetComputedValues().color();
}

void LogMissingFontFace(Element* element)
{
	const String font_family_property = element->GetProperty<String>("font-family");
	if (font_family_property.empty())
	{
		Log::Message(Log::LT_WARNING, "No font face defined. Missing 'font-family' property. On element %s", element->GetAddress().c_str());
	}
	else
	{
		const ComputedValues& computed = element->GetComputedValues();
		const String font_face_description = GetFontFaceDescription(font_family_property, computed.font_style(), computed.font_weight());
		Log::Message(Log::LT_WARNING,
			"No font face defined. Ensure (1) that Context::Update is run after new elements are constructed, before Context::Render, "
			"and (2) that the specified font face %s has been successfully loaded. "
			"Please see previous log messages for all successfully loaded fonts. On element %s",
			font_face_description.c_str(), element->GetAddress().c_str());
	}
}

RMLUI_RTTI_Define(ElementText)

ElementText::ElementText(const String& tag) :
	Element(tag), colour(255, 255, 255), opacity(1), font_handle_version(0), geometry_dirty(true), dirty_layout_on_change(true), font_effects_dirty(true),
	font_effects_handle(0)
{}

ElementText::~ElementText()
{
	if (context_previous && context_previous->context_next == this)
		context_previous->context_next = nullptr;
	if (context_next && context_next->context_previous == this)
		context_next->context_previous = nullptr;
}

void ElementText::SetText(const String& _text)
{
	if (text != _text)
	{
		text = _text;
		line_breaks_dirty = true;

		if (dirty_layout_on_change)
		{
			DirtyLayout();
			// White space that collapses away is given no box by the layout (P2-8), which then places no line: the lines
			// of the previous text would stay drawn.
			if (!lines.empty() && std::all_of(text.begin(), text.end(), [](char c) { return StringUtilities::IsWhitespace(c); }))
				ClearLines();
		}
	}
}

const String& ElementText::GetText() const
{
	return text;
}

void ElementText::OnRender()
{
	RMLUI_ZoneScoped;

	FontFaceHandle font_face_handle = GetFontFaceHandle();
	if (font_face_handle == 0)
		return;

	RenderManager& render_manager = GetContext()->GetRenderManager();

	// The opacity the parent draws with changes without a property change of ours (it is not inherited).
	const float draw_opacity = (GetParentNode() ? GetParentNode()->GetDrawOpacity() : 1.f);
	if (draw_opacity != opacity)
	{
		opacity = draw_opacity;
		colour = GetComputedValues().color().ToPremultiplied(opacity);
		font_effects_dirty = true;
		geometry_dirty = true;
	}

	// If our font effects have potentially changed, update it and force a geometry generation if necessary.
	if (font_effects_dirty && UpdateFontEffects())
		geometry_dirty = true;

	// Dirty geometry if font version has changed.
	int new_version = GetFontEngineInterface()->GetVersion(font_face_handle);
	if (new_version != font_handle_version)
	{
		font_handle_version = new_version;
		geometry_dirty = true;
	}

	// Regenerate the geometry if the colour or font configuration has altered.
	if (geometry_dirty)
		GenerateGeometry(render_manager, font_face_handle);

	const Vector2f translation = GetAbsoluteOffset();

	bool render = true;

	// Do a visibility test against the scissor region to avoid unnecessary render calls. Instead of handling
	// culling in complicated transform cases, for simplicity we always proceed to render if one is detected.
	Rectanglei scissor_region = render_manager.GetScissorRegion();
	if (!scissor_region.Valid())
		scissor_region = Rectanglei::FromSize(render_manager.GetViewport());

	if (!GetTransformState() || !GetTransformState()->GetTransform())
	{
		const FontMetrics& font_metrics = GetFontEngineInterface()->GetFontMetrics(GetFontFaceHandle());
		const int ascent = Math::RoundUpToInteger(font_metrics.ascent);
		const int descent = Math::RoundUpToInteger(font_metrics.descent);

		render = false;
		for (const Line& line : lines)
		{
			const Vector2i baseline = Vector2i(translation + line.position);
			const Rectanglei line_region = Rectanglei::FromCorners(baseline - Vector2i(0, ascent), baseline + Vector2i(line.width, descent));

			if (line_region.Valid() && scissor_region.Intersects(line_region))
			{
				render = true;
				break;
			}
		}
		// Shadows draw outside the lines, the line-clamp ellipsis after one.
		if (!shadows.empty() || line_clamp_ellipsis)
			render = true;
	}

	if (!render)
		return;

	const bool transformed = (GetTransformState() && GetTransformState()->GetTransform());
	if (!shadows.empty())
		RenderShadowList(render_manager, shadows, translation, transformed);

	// Underline and overline under the text, line-through over it.
	if (decoration_under)
		decoration_under.Render(translation);

	for (size_t i = 0; i < geometry.size(); ++i)
		geometry[i].geometry.Render(translation, geometry[i].texture);

	if (decoration_over)
		decoration_over.Render(translation);

	// The line-clamp ellipsis (fork, P6-L1) after the text, its shadows first: Chromium paints it after the item it follows,
	// with the block container's text shadows and no decoration.
	if (!line_clamp_ellipsis_geometry.empty())
	{
		if (!line_clamp_ellipsis_shadows.empty())
			RenderShadowList(render_manager, line_clamp_ellipsis_shadows, translation, transformed);
		for (const TexturedGeometry& part : line_clamp_ellipsis_geometry)
			part.geometry.Render(translation, part.texture);
	}
}

void ElementText::RenderShadowList(RenderManager& render_manager, Vector<Shadow>& shadows, Vector2f translation, bool transformed)
{
	const Rectanglei initial_scissor_region = render_manager.GetScissorRegion();

	// The first shadow is painted on top (CSS Text Decoration 3 §4): draw them from the last.
	for (auto it = shadows.rbegin(); it != shadows.rend(); ++it)
	{
		Shadow& shadow = *it;
		if (shadow.blur <= 0.f)
		{
			for (const TexturedGeometry& part : shadow.geometry)
				part.geometry.Render(translation, part.texture);
			if (shadow.decoration)
				shadow.decoration.Render(translation);
			continue;
		}

		// A blurred shadow: drawn into a layer, composited through a Gaussian blur of half the blur radius (CSS). The
		// scissor covers the shadow's ink and the blur's reach, three standard deviations.
		const float sigma = 0.5f * shadow.blur;
		if (!shadow.filter)
			shadow.filter = render_manager.CompileFilter("blur", Dictionary{{"sigma", Variant(sigma)}});
		if (!shadow.filter)
			continue;

		if (!transformed)
		{
			Rectanglef region = shadow.ink.Translate(translation).Extend(3.f * sigma + 1.f);
			Math::ExpandToPixelGrid(region);
			const Rectanglei scissor = Rectanglei(region).IntersectIfValid(initial_scissor_region);
			if (!scissor.Valid())
				continue;
			render_manager.SetScissorRegion(scissor);
		}

		render_manager.PushLayer();
		for (const TexturedGeometry& part : shadow.geometry)
			part.geometry.Render(translation, part.texture);
		if (shadow.decoration)
			shadow.decoration.Render(translation);

		FilterHandleList filter_handles;
		shadow.filter.AddHandleTo(filter_handles);
		render_manager.CompositeLayers(render_manager.GetTopLayer(), render_manager.GetNextLayer(), BlendMode::Blend, filter_handles);
		render_manager.PopLayer();
		render_manager.SetScissorRegion(initial_scissor_region);
	}
}

bool ElementText::GenerateLine(String& line, int& line_length, float& line_width, int line_begin, float maximum_line_width, float right_spacing_width,
	bool trim_whitespace_prefix, bool decode_escape_characters, bool allow_empty)
{
	RMLUI_ZoneScoped;
	RMLUI_ASSERT(maximum_line_width >= 0.f);

	FontFaceHandle font_face_handle = GetFontFaceHandle();

	// Initialise the output variables.
	line.clear();
	line_length = 0;
	line_width = 0;

	// Bail if we don't have a valid font face. White-space-only text (kept between inline-level boxes) places no glyphs,
	// so the indentation of a document without fonts is not reported.
	if (font_face_handle == 0)
	{
		if (!std::all_of(text.begin(), text.end(), [](char c) { return StringUtilities::IsWhitespace(c); }))
			LogMissingFontFace(GetParentNode() ? GetParentNode() : this);
		return true;
	}

	// Determine how we are processing white-space while formatting the text (CSS Text 4 §3, §4: 'white-space-collapse' and
	// 'text-wrap-mode', of which 'white-space' is the shorthand).
	using namespace Style;
	const auto& computed = GetComputedValues();
	const WhiteSpaceCollapse white_space_collapse = computed.white_space_collapse();
	const bool collapse_white_space = (white_space_collapse == WhiteSpaceCollapse::Collapse || white_space_collapse == WhiteSpaceCollapse::PreserveBreaks);
	const bool break_at_line = (computed.text_wrap_mode() == TextWrapMode::Wrap);
	const bool break_at_endline = (white_space_collapse == WhiteSpaceCollapse::Preserve || white_space_collapse == WhiteSpaceCollapse::PreserveBreaks ||
		white_space_collapse == WhiteSpaceCollapse::BreakSpaces);
	const bool break_spaces = (white_space_collapse == WhiteSpaceCollapse::BreakSpaces);
	// Preserved spaces at the end of a line hang ('pre-wrap'): they neither wrap nor take room (CSS Text 3 §4.1.3).
	const bool hang_spaces = !collapse_white_space && !break_spaces;

	const TextShapingContext text_shaping_context{computed.language(), computed.direction(), computed.font_kerning(), computed.letter_spacing(),
		computed.word_spacing()};
	WordBreak word_break = computed.word_break();
	EnsureLineBreaks(decode_escape_characters);
	const TokenRules token_rules{collapse_white_space, break_at_endline, break_spaces, computed.text_transform(), word_break, decode_escape_characters,
		text.c_str(), line_breaks.empty() ? nullptr : line_breaks.data(),
		line_breaks_prefix.size ? line_breaks_prefix.characters[line_breaks_prefix.size - 1] : Character::Null};
	// The content after the text up to the next opportunity when there is none at its end (a word an element boundary
	// splits): measured once, when the text's last token is (Chromium breaks before the whole word, not inside it).
	float glued_after_width = -1.f;

	// A word that does not fit on a line of its own is broken anywhere: 'word-break: break-all' at any width, 'overflow-wrap'
	// ('anywhere', 'break-word', and the legacy 'word-break: break-word') when the word starts the line. The min-content
	// width counts these breaks except for 'overflow-wrap: break-word' (CSS Text 3 §5.2, §5.5).
	const OverflowWrap overflow_wrap = computed.overflow_wrap();
	const bool overflow_wrap_breaks = (word_break == WordBreak::BreakWord || overflow_wrap == OverflowWrap::Anywhere ||
		(overflow_wrap == OverflowWrap::BreakWord && !LayoutDetails::IsMeasuringMinContent()));

	FontEngineInterface* font_engine_interface = GetFontEngineInterface();

	// Starting at the line_begin character, we generate sections of the text (we'll call them tokens) depending on the
	// white-space parsing parameters. Each section is then appended to the line if it can fit. If not, or if an
	// endline is found (and we're processing them), then the line is ended. kthxbai!
	const char* token_begin = text.c_str() + line_begin;
	const char* string_end = text.c_str() + text.size();

	// A text of white space only (between two inline-level boxes) collapses to one space, except at the start of a line
	// (CSS Text 3 §4.1.1).
	const bool white_space_only = collapse_white_space && line_begin == 0 && !text.empty() &&
		std::all_of(text.begin(), text.end(), [](char c) { return StringUtilities::IsWhitespace(c); });

	while (token_begin != string_end)
	{
		String token;
		const char* next_token_begin = token_begin;
		Character previous_codepoint = Character::Null;
		if (!line.empty())
			previous_codepoint =
				StringUtilities::ToCharacter(StringUtilities::SeekBackwardUTF8(&line.back(), line.data()), line.data() + line.size());

		// Generate the next token and determine its pixel-length.
		bool break_line = BuildToken(token, next_token_begin, string_end, line.empty() && trim_whitespace_prefix, token_rules);
		if (white_space_only && token.empty() && !(line.empty() && trim_whitespace_prefix))
			token = " ";
		int token_width = font_engine_interface->GetStringWidth(font_face_handle, token, text_shaping_context, previous_codepoint);

		// If we're breaking to fit a line box, check if the token can fit on the line before we add it.
		if (break_at_line)
		{
			const bool is_last_token = LastToken(next_token_begin, string_end, collapse_white_space, break_at_endline);
			int max_token_width = RoundDownToIntegerClamped(maximum_line_width - (is_last_token ? line_width + right_spacing_width : line_width));

			// The collapsible space that ends the element's text is removed when the line ends after it (CSS Text 3 §4.1.3,
			// LineBox::TrimTrailingWhiteSpace): the token fits when its word does, and the line breaks after the space.
			if (token_width > max_token_width && collapse_white_space && is_last_token &&
				FitsWithoutTrailingSpace(token, max_token_width, font_face_handle, text_shaping_context, previous_codepoint))
				max_token_width = token_width;

			// The last token of a word that goes on in the inline content after the text: it fits with that content, or
			// goes to the next line with it (P6-L2).
			if (!line_break_after && next_token_begin == string_end && word_break != WordBreak::BreakAll && token_width <= max_token_width &&
				(allow_empty || !line.empty()) && !IsWhiteSpaceToken(token) && !(collapse_white_space && !token.empty() && token.back() == ' '))
			{
				if (glued_after_width < 0.f)
					glued_after_width = MeasureGluedContentAfter();
				if (token_width + glued_after_width > max_token_width)
				{
					AppendHyphenAfterSoftHyphen(line, line_width, font_face_handle, text_shaping_context);
					return false;
				}
			}

			if (token_width > max_token_width && hang_spaces && IsWhiteSpaceToken(token))
			{
				// Hanging spaces: consumed on this line, without width; the line ends after them.
				line_length += (int)(next_token_begin - token_begin);
				token_begin = next_token_begin;
				if (token_begin == string_end)
					return true;
				return false;
			}

			if (token_width > max_token_width)
			{
				if (word_break == WordBreak::BreakAll || (overflow_wrap_breaks && line.empty() && !allow_empty))
				{
					// Try to break up the word
					max_token_width = RoundDownToIntegerClamped(maximum_line_width - line_width);
					const int token_max_size = int(next_token_begin - token_begin);
					const char* partial_string_end = token_begin + token_max_size;

					// @performance: Can be made much faster. Use string width heuristics and logarithmic search.
					while (true)
					{
						partial_string_end = StringUtilities::SeekBackwardUTF8(partial_string_end - 1, token_begin);

						bool force_loop_break_at_end = false;
						if (partial_string_end == token_begin)
						{
							// Not even the first character of the token fits. Let it overflow onto the next line if we can.
							if (allow_empty || !line.empty())
								return false;

							// Continue by forcing the first character to be consumed, even though it will overflow.
							partial_string_end = StringUtilities::SeekForwardUTF8(token_begin + 1, token_begin + token_max_size);
							force_loop_break_at_end = true;
						}

						token.clear();
						next_token_begin = token_begin;
						BuildToken(token, next_token_begin, partial_string_end, line.empty() && trim_whitespace_prefix, token_rules);
						token_width = font_engine_interface->GetStringWidth(font_face_handle, token, text_shaping_context, previous_codepoint);

						if (force_loop_break_at_end || token_width <= max_token_width)
							break;
					}

					break_line = true;
				}
				else if (allow_empty || !line.empty())
				{
					// Let the token overflow into the next line; a line that ends at a soft hyphen shows a hyphen.
					AppendHyphenAfterSoftHyphen(line, line_width, font_face_handle, text_shaping_context);
					return false;
				}
			}
		}

		// The token can fit on the end of the line, so add it onto the end and increment our width and length counters.
		line += token;
		line_length += (int)(next_token_begin - token_begin);
		line_width += token_width;

		// Break out of the loop if an endline was forced.
		if (break_line && (allow_empty || !line.empty()))
			return false;

		// Set the beginning of the next token.
		token_begin = next_token_begin;
	}

	return true;
}

void ElementText::EnsureLineBreaks(bool decode_escape_characters)
{
	// (once per layout of the text, as the inline content around it may have changed; again when the text did)
	if (line_breaks_stamp != context_stamp || line_breaks_dirty || line_breaks_decode_escape_characters != decode_escape_characters ||
		line_breaks_word_break != GetComputedValues().word_break())
	{
		line_breaks_stamp = context_stamp;
		UpdateLineBreaks(decode_escape_characters);
	}
}

void ElementText::UpdateLineBreaks(bool decode_escape_characters)
{
	// The inline formatting context's text around this one (fork, P6-L2): the opportunities again when it changed. A text
	// of ASCII with ASCII around it needs two code points before it and one after (Chromium's table); any other, a few.
	const Style::WordBreak word_break = GetComputedValues().word_break();
	const bool text_changed =
		(line_breaks_dirty || line_breaks_word_break != word_break || line_breaks_decode_escape_characters != decode_escape_characters);
	if (text_changed)
		line_breaks_ascii = (word_break != Style::WordBreak::KeepAll && IsAsciiText(text, decode_escape_characters));
	const auto plain = [](const LineBreakContext& context) {
		for (int i = 0; i < context.size; i++)
		{
			const char32_t c = (char32_t)context.characters[i];
			if (c > 0x7E || (c < 0x21 && c != ' ' && c != '\t' && c != '\n' && c != '\r'))
				return false;
		}
		return true;
	};
	LineBreakContext prefix, suffix;
	if (context_previous)
	{
		GatherLineBreakContext(this, false, decode_escape_characters, line_breaks_ascii ? 2 : LineBreakContext::capacity, prefix);
		if (line_breaks_ascii && !plain(prefix))
			GatherLineBreakContext(this, false, decode_escape_characters, LineBreakContext::capacity, prefix);
	}
	if (context_next)
	{
		GatherLineBreakContext(this, true, decode_escape_characters, line_breaks_ascii ? 1 : LineBreakContext::capacity, suffix);
		if (line_breaks_ascii && !plain(suffix))
			GatherLineBreakContext(this, true, decode_escape_characters, LineBreakContext::capacity, suffix);
	}
	if (text_changed || prefix != line_breaks_prefix || suffix != line_breaks_suffix)
	{
		ComputeLineBreaks(text, decode_escape_characters, word_break, line_breaks_ascii, prefix, suffix, line_breaks, line_break_before, line_break_after);
		line_breaks_dirty = false;
		line_breaks_word_break = word_break;
		line_breaks_decode_escape_characters = decode_escape_characters;
		line_breaks_prefix = prefix;
		line_breaks_suffix = suffix;
	}
}

bool ElementText::IsLineBreakOpportunityBefore()
{
	// (nothing before it in the context: no need to look further)
	if (!context_previous)
		return true;
	EnsureLineBreaks(true);
	// Collapsible white space that starts the text is its first word's: the line may break before it, as it disappears
	// at the next line's start (Chromium breaks after it, where it hangs).
	if (!text.empty() && StringUtilities::IsWhitespace(text[0]))
	{
		const Style::WhiteSpaceCollapse collapse = GetComputedValues().white_space_collapse();
		if (collapse == Style::WhiteSpaceCollapse::Collapse || collapse == Style::WhiteSpaceCollapse::PreserveBreaks)
			return true;
	}
	return line_break_before;
}

float ElementText::MeasureLeadingWord(bool& out_whole)
{
	out_whole = false;
	FontFaceHandle font_face_handle = GetFontFaceHandle();
	if (font_face_handle == 0)
		return 0.f;
	EnsureLineBreaks(true);
	if (text.empty())
	{
		out_whole = true;
		return 0.f;
	}
	if (StringUtilities::IsWhitespace(text[0]))
		return 0.f;

	using namespace Style;
	const auto& computed = GetComputedValues();
	const WhiteSpaceCollapse white_space_collapse = computed.white_space_collapse();
	const bool collapse_white_space = (white_space_collapse == WhiteSpaceCollapse::Collapse || white_space_collapse == WhiteSpaceCollapse::PreserveBreaks);
	const bool break_at_endline = (white_space_collapse == WhiteSpaceCollapse::Preserve || white_space_collapse == WhiteSpaceCollapse::PreserveBreaks ||
		white_space_collapse == WhiteSpaceCollapse::BreakSpaces);
	const TokenRules token_rules{collapse_white_space, break_at_endline, white_space_collapse == WhiteSpaceCollapse::BreakSpaces, computed.text_transform(),
		computed.word_break(), true, text.c_str(), line_breaks.empty() ? nullptr : line_breaks.data(),
		line_breaks_prefix.size ? line_breaks_prefix.characters[line_breaks_prefix.size - 1] : Character::Null};
	String token;
	const char* token_begin = text.c_str();
	const char* string_end = token_begin + text.size();
	BuildToken(token, token_begin, string_end, false, token_rules);
	while (!token.empty() && (token.back() == ' ' || token.back() == '\n'))
		token.pop_back();
	out_whole = (token_begin == string_end && !line_break_after);
	const TextShapingContext text_shaping_context{computed.language(), computed.direction(), computed.font_kerning(), computed.letter_spacing(),
		computed.word_spacing()};
	return (float)GetFontEngineInterface()->GetStringWidth(font_face_handle, token, text_shaping_context, Character::Null);
}

float ElementText::MeasureGluedContentAfter()
{
	// The content after this text up to the inline formatting context's next line-break opportunity, through inline boxes
	// (their edges) and texts with none (fork, P6-L2).
	float width = 0.f;
	ElementText* from = this;
	while (ElementText* next = from->GetContextNext())
	{
		width += from->context_edges_after;
		bool whole = false;
		width += next->MeasureLeadingWord(whole);
		if (!whole)
			break;
		from = next;
	}
	return width;
}

namespace {
	struct InlineContextLinker {
		ElementText* last_text = nullptr;
		float edges = 0.f;

		void Visit(Element* parent)
		{
			ScopedLayoutChildList layout_children;
			LayoutDetails::GetLayoutChildren(parent, *layout_children, false);
			for (Element* child : *layout_children)
			{
				if (auto text = rmlui_dynamic_cast<ElementText*>(child))
				{
					text->LinkAfter(last_text, edges);
					last_text = text;
					edges = 0.f;
				}
				else if (IsSkippedInContext(child))
					continue;
				else if (child->GetTagName() != "br" && IsTransparentInlineBox(child))
				{
					edges += InlineBoxEdge(child, true);
					Visit(child);
					edges += InlineBoxEdge(child, false);
				}
				else
				{
					// an atomic inline, a block, a <br>: the context stops
					last_text = nullptr;
					edges = 0.f;
				}
			}
		}
	};
} // namespace

static uint32_t inline_context_stamp = 0;

void ElementText::LinkAfter(ElementText* previous, float edges_between)
{
	// (zero is never a stamp: a text not linked yet computes its breaks the first time)
	if (++inline_context_stamp == 0)
		++inline_context_stamp;
	context_stamp = inline_context_stamp;
	context_previous = previous;
	context_next = nullptr;
	context_edges_after = 0.f;
	if (previous)
	{
		previous->context_next = this;
		previous->context_edges_after = edges_between;
	}
}

void ElementText::LinkInlineFormattingContext(Element* root)
{
	// (most often a text alone: nothing around it)
	if (root->GetNumChildren(true) == 1)
	{
		if (auto text = rmlui_dynamic_cast<ElementText*>(root->GetChild(0)))
		{
			text->LinkAfter(nullptr, 0.f);
			return;
		}
	}
	InlineContextLinker linker;
	linker.Visit(root);
}

void ElementText::ClearLines()
{
	RMLUI_ZoneScoped;
	lines.clear();
	justifications.clear();
	line_clamp_ellipsis.reset();
	line_clamp_ellipsis_line = -1;
	geometry_dirty = true;
}

void ElementText::AddLine(Vector2f line_position, String line)
{
	if (font_effects_dirty)
		UpdateFontEffects();

	lines.emplace_back(std::move(line), line_position);

	geometry_dirty = true;
}

void ElementText::AddLine(Vector2f line_position, String line, const Vector<JustificationSpacing>& justification)
{
	AddLine(line_position, std::move(line));
	lines.back().justification_index = (int)justifications.size();
	justifications.push_back(justification);
}

void ElementText::SetLineClampEllipsis(UniquePtr<LineClampEllipsis> ellipsis)
{
	line_clamp_ellipsis = std::move(ellipsis);
	line_clamp_ellipsis_line = (line_clamp_ellipsis && !lines.empty() ? (int)lines.size() - 1 : -1);
	geometry_dirty = true;
}

void ElementText::SuppressAutoLayout()
{
	dirty_layout_on_change = false;
}

void ElementText::OnPropertyChange(const PropertyIdSet& changed_properties)
{
	RMLUI_ZoneScoped;

	Element::OnPropertyChange(changed_properties);

	bool colour_changed = false;
	bool font_face_changed = false;
	auto& computed = GetComputedValues();

	if (changed_properties.Contains(PropertyId::Color) || changed_properties.Contains(PropertyId::Opacity))
	{
		// Text draws with the opacity its parent draws with (Element::GetDrawOpacity: group opacity, phase 3).
		const float new_opacity = (GetParentNode() ? GetParentNode()->GetDrawOpacity() : 1.f);
		const bool opacity_changed = opacity != new_opacity;

		ColourbPremultiplied new_colour = computed.color().ToPremultiplied(new_opacity);
		colour_changed = colour != new_colour;

		if (colour_changed)
		{
			colour = new_colour;
		}
		if (opacity_changed)
		{
			opacity = new_opacity;
			font_effects_dirty = true;
			geometry_dirty = true;
		}
	}

	if (changed_properties.Contains(PropertyId::FontFamily) ||         //
		changed_properties.Contains(PropertyId::FontWeight) ||         //
		changed_properties.Contains(PropertyId::FontStyle) ||          //
		changed_properties.Contains(PropertyId::FontSize) ||           //
		changed_properties.Contains(PropertyId::FontKerning) ||        //
		changed_properties.Contains(PropertyId::LetterSpacing) ||      //
		changed_properties.Contains(PropertyId::WordSpacing) ||        //
		changed_properties.Contains(PropertyId::RmlUi_Language) ||     //
		changed_properties.Contains(PropertyId::RmlUi_Direction) ||    //
		changed_properties.Contains(PropertyId::TextTransform) ||      //
		changed_properties.Contains(PropertyId::WhiteSpaceCollapse) || //
		changed_properties.Contains(PropertyId::TextWrapMode) ||       //
		changed_properties.Contains(PropertyId::WordBreak) ||          //
		changed_properties.Contains(PropertyId::OverflowWrap) ||       //
		changed_properties.Contains(PropertyId::TextOverflow))
	{
		font_face_changed = true;
		geometry_dirty = true;

		font_effects_handle = 0;
		font_effects_dirty = true;
		font_handle_version = 0;
	}

	if (changed_properties.Contains(PropertyId::FontEffect))
	{
		font_effects_dirty = true;
	}

	// The decoration lines and the shadows are generated with the text.
	if (changed_properties.Contains(PropertyId::TextDecoration) ||          //
		changed_properties.Contains(PropertyId::TextDecorationColor) ||     //
		changed_properties.Contains(PropertyId::TextDecorationStyle) ||     //
		changed_properties.Contains(PropertyId::TextDecorationThickness) || //
		changed_properties.Contains(PropertyId::TextUnderlineOffset) ||     //
		changed_properties.Contains(PropertyId::TextShadow))
	{
		geometry_dirty = true;
	}

	if (font_face_changed)
	{
		// We have to let our document know we need to be regenerated.
		if (dirty_layout_on_change)
			DirtyLayout();
	}
	else if (colour_changed)
	{
		// Force the geometry to be regenerated (decoration lines and shadows in 'currentcolor' follow).
		geometry_dirty = true;
	}
}

void ElementText::GetRML(String& content)
{
	content += StringUtilities::EncodeRml(text);
}

bool ElementText::UpdateFontEffects()
{
	RMLUI_ZoneScoped;

	if (GetFontFaceHandle() == 0)
		return false;

	font_effects_dirty = false;

	static const FontEffectList empty_font_effects;

	// Fetch the font-effect for this text element
	const FontEffectList* font_effects = &empty_font_effects;
	if (GetComputedValues().has_font_effect())
	{
		if (const Property* p = GetProperty(PropertyId::FontEffect))
			if (FontEffectsPtr effects = p->Get<FontEffectsPtr>())
				font_effects = &effects->list;
	}

	// Request a font layer configuration to match this set of effects. If this is different from
	// our old configuration, then return true to indicate we'll need to regenerate geometry.
	FontEffectsHandle new_font_effects_handle = GetFontEngineInterface()->PrepareFontEffects(GetFontFaceHandle(), *font_effects);
	if (new_font_effects_handle != font_effects_handle)
	{
		font_effects_handle = new_font_effects_handle;
		return true;
	}

	return false;
}

void ElementText::GenerateGeometry(RenderManager& render_manager, const FontFaceHandle font_face_handle)
{
	RMLUI_ZoneScopedC(0xD2691E);

	const TextOverflowResolved text_overflow = ResolveTextOverflow(this, font_face_handle);

	const auto& computed = GetComputedValues();
	const TextShapingContext text_shaping_context{computed.language(), computed.direction(), computed.font_kerning(), computed.letter_spacing(),
		computed.word_spacing()};
	FontEngineInterface* font_engine_interface = GetFontEngineInterface();

	// The strings to draw: the lines, cut at the clipping edge with the overflow text when the text overflows.
	StringList line_strings(lines.size());
	for (size_t i = 0; i < lines.size(); i++)
		line_strings[i] = lines[i].text;

	TexturedMeshList mesh_list;
	mesh_list.reserve(geometry.size());

	// A justified line draws its characters with the space added around them (p6-justify).
	const auto LineContext = [this, &text_shaping_context](const Line& line) {
		TextShapingContext context = text_shaping_context;
		context.justification = GetJustification(line);
		return context;
	};

	for (Line& line : lines)
	{
		line.width = font_engine_interface->GenerateString(render_manager, font_face_handle, font_effects_handle, line.text, line.position, colour,
			opacity, line.justification_index < 0 ? text_shaping_context : LineContext(line), mesh_list);
	}

	const auto text_overflows_on_line = [&](const Line& line) { return line.position.x + line.width > text_overflow.overflow_edge; };
	const auto overflows = [&](const Line& line) { return &line - lines.data() != line_clamp_ellipsis_line && text_overflows_on_line(line); };
	if (text_overflow.enabled && std::any_of(lines.begin(), lines.end(), overflows))
	{
		mesh_list.clear();

		for (size_t i = 0; i < lines.size(); i++)
		{
			Line& line = lines[i];
			if (line.text.empty())
				continue;
			// The line a line-clamp ellipsis ends is not ellipsized again (Chromium: its line-clamp state wins).
			if ((int)i == line_clamp_ellipsis_line)
			{
				line.width = font_engine_interface->GenerateString(render_manager, font_face_handle, font_effects_handle, line.text, line.position,
					colour, opacity, line.justification_index < 0 ? text_shaping_context : LineContext(line), mesh_list);
				continue;
			}

			String abbreviated_text;
			String kept_text; // the line's text before the overflow text
			StringIteratorU8 view(line.text, line.text.size());
			--view;

			// If we have text overflow, reduce the string one character at a time, append the ellipsis or custom
			// string, and try again until it fits. @performance Can be improved by e.g. logarithmic search. Consider
			// combining it with the word-breaking algorithm of 'GenerateLine'.
			for (; text_overflows_on_line(line) && view && view.get() != line.text.c_str(); --view)
			{
				abbreviated_text.reserve(line.text.size() + text_overflow.overflow_text.size());
				kept_text.assign(line.text.c_str(), view.get());
				abbreviated_text = kept_text;
				abbreviated_text.append(text_overflow.overflow_text);
				line.width = font_engine_interface->GetStringWidth(font_face_handle, abbreviated_text, text_shaping_context);
				line_strings[i] = abbreviated_text;
			}

			if (abbreviated_text.empty())
			{
				line.width = font_engine_interface->GenerateString(render_manager, font_face_handle, font_effects_handle, line_strings[i],
					line.position, colour, opacity, line.justification_index < 0 ? text_shaping_context : LineContext(line), mesh_list);
				continue;
			}
			// (an abbreviated line is drawn without its justification: it overflows)
			line.justification_index = -1;
			// the kept text in the text's colour, the overflow text after it in the block container's (CSS Overflow 4 §3.2)
			const float kept_width = (float)font_engine_interface->GenerateString(render_manager, font_face_handle, font_effects_handle, kept_text,
				line.position, colour, opacity, text_shaping_context, mesh_list);
			line.width = (int)kept_width +
				font_engine_interface->GenerateString(render_manager, font_face_handle, font_effects_handle, text_overflow.overflow_text,
					line.position + Vector2f(kept_width, 0.f), text_overflow.colour.ToPremultiplied(opacity), opacity, text_shaping_context, mesh_list);
		}
	}

	// The line-clamp ellipsis (fork, P6-L1), in its block container's font, colour and text shadows, after its line's text.
	line_clamp_ellipsis_geometry.clear();
	line_clamp_ellipsis_shadows.clear();
	if (line_clamp_ellipsis && line_clamp_ellipsis_line >= 0 && line_clamp_ellipsis_line < (int)lines.size())
		LineClamp::GenerateEllipsis(render_manager, *line_clamp_ellipsis, lines[line_clamp_ellipsis_line].position + line_clamp_ellipsis->position,
			opacity, line_clamp_ellipsis_geometry, line_clamp_ellipsis_shadows);

	// Apply the new geometry and textures. Reuse the old geometry if the mesh matches, which can be relatively common
	// where the layout is changed in a way that does not visually affect this element.
	geometry.resize(mesh_list.size());
	for (size_t i = 0; i < mesh_list.size(); i++)
	{
		if (!geometry[i].geometry || geometry[i].geometry.GetMesh() != mesh_list[i].mesh)
			geometry[i].geometry = render_manager.MakeGeometry(std::move(mesh_list[i].mesh));

		geometry[i].texture = mesh_list[i].texture;
	}

	// The decoration lines.
	const Style::TextDecoration decoration_lines = computed.text_decoration();
	const ColourbPremultiplied decoration_colour =
		(decoration_lines != Style::TextDecoration::None ? GetDecorationColour(this).ToPremultiplied(opacity) : ColourbPremultiplied());
	{
		Mesh under, over;
		if (decoration_lines != Style::TextDecoration::None)
			GenerateDecoration(under, over, font_face_handle, decoration_colour, Vector2f(0.f));
		decoration_under = (under.indices.empty() ? Geometry() : render_manager.MakeGeometry(std::move(under)));
		decoration_over = (over.indices.empty() ? Geometry() : render_manager.MakeGeometry(std::move(over)));
	}

	// The shadows: the same strings in each shadow's colour, offset, with the decoration lines.
	shadows.clear();
	if (computed.has_text_shadow())
	{
		const FontMetrics& metrics = font_engine_interface->GetFontMetrics(font_face_handle);
		const Colourb text_colour = computed.color();
		for (const ResolvedTextShadow& resolved : ResolveTextShadows(*this))
		{
			Shadow shadow;
			shadow.blur = resolved.blur;
			// 'currentcolor' shadows follow the text's colour; the decoration's shadow takes the shadow's colour too
			const ColourbPremultiplied shadow_colour = resolved.colour.ToPremultiplied(opacity);
			if (shadow_colour.alpha == 0)
				continue;
			(void)text_colour;

			TexturedMeshList shadow_meshes;
			Rectanglef ink = Rectanglef::MakeInvalid();
			for (size_t i = 0; i < lines.size(); i++)
			{
				const Line& line = lines[i];
				const Vector2f position = line.position + resolved.offset;
				font_engine_interface->GenerateString(render_manager, font_face_handle, 0, line_strings[i], position, shadow_colour, opacity,
					line.justification_index < 0 ? text_shaping_context : LineContext(line), shadow_meshes);
				const Rectanglef line_ink =
					Rectanglef::FromCorners(position - Vector2f(0.f, metrics.ascent), position + Vector2f((float)line.width, metrics.descent));
				ink = (ink.Valid() ? ink.Join(line_ink) : line_ink);
			}
			for (TexturedMesh& mesh : shadow_meshes)
				shadow.geometry.push_back(TexturedGeometry{render_manager.MakeGeometry(std::move(mesh.mesh)), mesh.texture});

			if (decoration_lines != Style::TextDecoration::None)
			{
				Mesh under, over;
				GenerateDecoration(under, over, font_face_handle, shadow_colour, resolved.offset);
				AppendMesh(under, over);
				if (!under.indices.empty())
					shadow.decoration = render_manager.MakeGeometry(std::move(under));
			}
			shadow.ink = (ink.Valid() ? ink : Rectanglef::FromPosition(resolved.offset));
			shadows.push_back(std::move(shadow));
		}
	}

	geometry_dirty = false;
}

void ElementText::GenerateDecoration(Mesh& under, Mesh& over, const FontFaceHandle font_face_handle, ColourbPremultiplied decoration_colour,
	Vector2f offset)
{
	RMLUI_ZoneScopedC(0xA52A2A);

	// The geometry of the decoration lines follows Chromium's (CSS Text Decoration 4 leaves it to the UA): measured on
	// fonts with and without underline metrics, sizes 13-40px.
	const FontMetrics& metrics = GetFontEngineInterface()->GetFontMetrics(font_face_handle);
	// (the no-text test variant of the font engine reports no thickness: nothing is drawn then)
	if (metrics.underline_thickness <= 0.f)
		return;

	const Style::TextDecoration decoration_lines = GetComputedValues().text_decoration();
	const float font_size = GetComputedValues().font_size();

	// 'text-decoration-thickness': 'auto' is a tenth of the font size, 'from-font' the font's underline thickness, a
	// length rounded; at least one pixel, whole pixels.
	float thickness_value = font_size / 10.f;
	if (const Property* property = GetProperty(PropertyId::TextDecorationThickness))
	{
		if (property->unit == Unit::KEYWORD)
		{
			if (property->Get<int>() == 1) // from-font
				thickness_value = Math::Round(metrics.underline_thickness);
		}
		else
			thickness_value = Math::Round(ResolveLengthPercentage(LengthPercentageValueOf(*property), *this, font_size));
	}
	const float thickness = Math::Max(1.f, Math::RoundDown(thickness_value));

	// 'text-underline-offset': 'auto' leaves a gap of half the thickness (at least a pixel) under the baseline; a length
	// places the line's top that far under the baseline.
	float underline_top = Math::Max(1.f, Math::RoundUp(0.5f * thickness_value));
	if (const Property* property = GetProperty(PropertyId::TextUnderlineOffset))
	{
		if (property->unit != Unit::KEYWORD)
			underline_top = Math::Round(ResolveLengthPercentage(LengthPercentageValueOf(*property), *this, font_size));
	}
	float ascent, descent;
	LayoutDetails::GetIntegerAscentDescent(metrics, ascent, descent);
	// overline: on top of the ascent; line-through: centred a third of the ascent above the baseline
	const float overline_top = -ascent - thickness;
	const float line_through_top = -ascent / 3.f - 0.5f * thickness;

	Style::TextDecorationStyle style = Style::TextDecorationStyle::Solid;
	if (const Property* property = GetProperty(PropertyId::TextDecorationStyle))
		style = (Style::TextDecorationStyle)property->Get<int>();

	auto GenerateDecorationLine = [&](Mesh& mesh, float top) {
		for (const Line& line : lines)
		{
			// whole pixels from the baseline, as the glyphs are placed
			const Vector2f position = Vector2f(line.position.x, Math::Round(line.position.y + top)) + offset;
			const float width = (float)line.width;
			switch (style)
			{
			case Style::TextDecorationStyle::Double:
				// two lines of the thickness, a third of it apart (at least a pixel)
				MeshUtilities::GenerateLine(mesh, position, Vector2f(width, thickness), decoration_colour);
				MeshUtilities::GenerateLine(mesh, position + Vector2f(0.f, thickness + Math::Max(1.f, Math::RoundDown(thickness / 3.f))),
					Vector2f(width, thickness), decoration_colour);
				break;
			case Style::TextDecorationStyle::Dotted:
			case Style::TextDecorationStyle::Dashed:
			{
				// Chromium's pattern: dots and their gaps as long as the line is thick; dashes three times that with gaps of
				// two (twice and once from three pixels thick); the gaps then stretched so that the line starts and ends
				// with a dash
				const bool dotted = (style == Style::TextDecorationStyle::Dotted);
				const float segment = thickness * (dotted ? 1.f : (thickness >= 3.f ? 2.f : 3.f));
				float gap = thickness * (dotted ? 1.f : (thickness >= 3.f ? 1.f : 2.f));
				if (width > segment)
				{
					const float min_dashes = Math::RoundDown((width + gap) / (segment + gap));
					const float min_gap = (min_dashes > 1.f ? (width - min_dashes * segment) / (min_dashes - 1.f) : width);
					const float max_gap = (width - (min_dashes + 1.f) * segment) / min_dashes;
					gap = (max_gap <= 0.f || Math::Absolute(min_gap - gap) < Math::Absolute(max_gap - gap)) ? min_gap : max_gap;
				}
				for (float x = 0.f; x < width; x += segment + gap)
					MeshUtilities::GenerateLine(mesh, position + Vector2f(x, 0.f), Vector2f(Math::Min(segment, width - x), thickness), decoration_colour);
				break;
			}
			case Style::TextDecorationStyle::Solid:
			case Style::TextDecorationStyle::Wavy: // drawn as solid (phase-3 spec 2.2)
				MeshUtilities::GenerateLine(mesh, position, Vector2f(width, thickness), decoration_colour);
				break;
			}
		}
	};

	// Underline and overline are painted under the text, line-through over it.
	if (Style::HasTextDecoration(decoration_lines, Style::TextDecoration::Underline))
		GenerateDecorationLine(under, underline_top);
	if (Style::HasTextDecoration(decoration_lines, Style::TextDecoration::Overline))
		GenerateDecorationLine(under, overline_top);
	if (Style::HasTextDecoration(decoration_lines, Style::TextDecoration::LineThrough))
		GenerateDecorationLine(over, line_through_top);
}

// The escape codes and UTF-8 sequences of ReadCharacter.
static Character ReadEscapedOrEncodedCharacter(const char* p, const char* string_end, bool decode_escape_characters, const char*& next,
	bool& non_breaking)
{
	non_breaking = false;
	if (decode_escape_characters && *p == '&')
	{
		const char* semicolon = p;
		while (semicolon != string_end && *semicolon != ';')
			++semicolon;
		if (semicolon != string_end)
		{
			const StringView escape_code(p + 1, semicolon);
			Character character = Character::Null;
			if (escape_code == "lt")
				character = Character('<');
			else if (escape_code == "gt")
				character = Character('>');
			else if (escape_code == "amp")
				character = Character('&');
			else if (escape_code == "quot")
				character = Character('"');
			else if (escape_code == "nbsp")
			{
				character = Character(' ');
				non_breaking = true;
			}
			if (character != Character::Null)
			{
				next = semicolon + 1;
				return character;
			}
		}
		// Not a recognised escape code: print it like normal text.
	}
	next = p;
	return TextUnicode::DecodeUTF8(next, string_end);
}

static bool IsContinuationByte(char c)
{
	return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// The character at 'p' (an escape code decoded if asked) and the position after it; 'non_breaking' for '&nbsp;'. ASCII
// is read inline (as DecodeUTF8 reads it: a byte below 0x80 not followed by stray continuation bytes).
static inline Character ReadCharacter(const char* p, const char* string_end, bool decode_escape_characters, const char*& next, bool& non_breaking)
{
	const unsigned char byte = static_cast<unsigned char>(*p);
	if (byte < 0x80 && !(decode_escape_characters && byte == '&') && (p + 1 == string_end || !IsContinuationByte(p[1])))
	{
		non_breaking = false;
		next = p + 1;
		return static_cast<Character>(byte);
	}
	return ReadEscapedOrEncodedCharacter(p, string_end, decode_escape_characters, next, non_breaking);
}

// The text is printable ASCII and white space (an escape code's '&' not, when they are decoded): eight bytes at a time,
// those that may not be looked at one by one.
static bool IsAsciiText(const String& text, bool decode_escape_characters)
{
	const auto is_ascii = [decode_escape_characters](char c) {
		return (c > ' ' && c < 0x7F && !(decode_escape_characters && c == '&')) || StringUtilities::IsWhitespace(c);
	};
	constexpr uint64_t ones = 0x0101010101010101ull, highs = 0x8080808080808080ull;
	const auto has_zero_byte = [](uint64_t w) { return ((w - ones) & ~w & highs) != 0; };
	const char* p = text.data();
	const char* end = p + text.size();
	for (; end - p >= 8; p += 8)
	{
		uint64_t w;
		memcpy(&w, p, 8);
		if (w & highs)
			return false;
		// (exact for bytes below 0x80: a byte below 0x20, 0x7F, '&')
		const bool control = (((w - ones * 0x20) & ~w & highs) != 0);
		if (control || has_zero_byte(w ^ (ones * 0x7F)) || (decode_escape_characters && has_zero_byte(w ^ (ones * '&'))))
		{
			for (int i = 0; i < 8; i++)
			{
				if (!is_ascii(p[i]))
					return false;
			}
		}
	}
	for (; p != end; ++p)
	{
		if (!is_ascii(*p))
			return false;
	}
	return true;
}

static bool IsContextWhiteSpace(Character c)
{
	return c == Character(' ') || c == Character('\t') || c == Character('\n') || c == Character('\r') || c == Character('\f');
}

// The opportunity between two pieces of an inline formatting context's text, 'before' ending where 'after' starts (fork,
// P6-L2): after white space, never before it (UAX #14 LB18, LB7), Chromium's table between two printable ASCII characters,
// else UAX #14 over the two; true when either is empty (the context's start or end, or an atomic inline).
static bool IsBoundaryOpportunity(const Character* before, size_t num_before, const Character* after, size_t num_after, Style::WordBreak word_break)
{
	if (num_before == 0 || num_after == 0)
		return true;
	const Character b = before[num_before - 1], a = after[0];
	if (IsContextWhiteSpace(b))
		return true;
	if (IsContextWhiteSpace(a))
		return false;
	if (word_break == Style::WordBreak::BreakAll)
		return true;
	const auto printable = [](Character c) { return (char32_t)c >= 0x21 && (char32_t)c <= 0x7E; };
	if (word_break != Style::WordBreak::KeepAll && printable(b) && printable(a))
		return TextUnicode::IsAsciiLineBreakOpportunity(num_before >= 2 ? (char32_t)before[num_before - 2] : 0, (char32_t)b, (char32_t)a);
	Vector<Character> window(before, before + num_before);
	window.insert(window.end(), after, after + num_after);
	Vector<bool> breaks;
	TextUnicode::GetLineBreakOpportunities(window, word_break, TextUnicode::LineBreakTailoring::Chromium, breaks);
	return breaks[num_before];
}

// The line-break opportunities of an element's text, read as BuildToken reads it (escape codes decoded if asked): UAX #14
// tailored as Chromium (TextUnicode::GetLineBreakOpportunities) over the text with the inline formatting context's text
// around it ('prefix', 'suffix': a few code points); 'out_breaks[offset]' is 1 when a line may break before the character
// that starts at that byte; 'out_break_before', 'out_break_after' at the text's start and end. 'ascii': the text is printable
// ASCII and white space (IsAsciiText) and 'word-break' is not 'keep-all'.
static void ComputeLineBreaks(const String& text, bool decode_escape_characters, Style::WordBreak word_break, bool ascii, const LineBreakContext& prefix,
	const LineBreakContext& suffix, Vector<uint8_t>& out_breaks, bool& out_break_before, bool& out_break_after)
{
	out_break_before = out_break_after = true;
	if (text.empty())
	{
		out_breaks.clear();
		return;
	}

	// A text of printable ASCII characters and white space (no escape code read, no 'keep-all'), most texts: BuildToken
	// asks Chromium's table for the pairs of printable characters it ends a token between, the only boundaries it asks
	// about (not around white space), and no table is kept.
	if (ascii)
	{
		out_breaks.clear();
		out_breaks.shrink_to_fit();
		if (prefix.size || suffix.size)
		{
			constexpr size_t context_size = LineBreakContext::capacity;
			Character head[context_size], tail[context_size];
			const size_t num = Math::Min(text.size(), context_size);
			for (size_t i = 0; i < num; i++)
			{
				head[i] = Character((unsigned char)text[i]);
				tail[i] = Character((unsigned char)text[text.size() - num + i]);
			}
			out_break_before = IsBoundaryOpportunity(prefix.characters, prefix.size, head, num, word_break);
			out_break_after = IsBoundaryOpportunity(tail, num, suffix.characters, suffix.size, word_break);
		}
		return;
	}

	out_breaks.assign(text.size() + 1, 0);
	Vector<Character> characters(prefix.characters, prefix.characters + prefix.size);
	Vector<int> offsets;
	characters.reserve(prefix.size + text.size() + suffix.size);
	offsets.reserve(text.size());
	const char* p = text.c_str();
	const char* end = p + text.size();
	while (p != end)
	{
		const char* next = nullptr;
		bool non_breaking = false;
		characters.push_back(ReadCharacter(p, end, decode_escape_characters, next, non_breaking));
		offsets.push_back(int(p - text.c_str()));
		p = next;
	}
	const size_t num_characters = offsets.size();
	characters.insert(characters.end(), suffix.characters, suffix.characters + suffix.size);
	Vector<bool> breaks;
	TextUnicode::GetLineBreakOpportunities(characters, word_break, TextUnicode::LineBreakTailoring::Chromium, breaks);
	for (size_t i = 1; i < num_characters; i++)
		out_breaks[offsets[i]] = breaks[prefix.size + i] ? 1 : 0;
	// (white space before or after: as IsBoundaryOpportunity, which the texts on the other side compute)
	const size_t num_prefix = (size_t)prefix.size;
	if (prefix.size)
		out_break_before = IsContextWhiteSpace(prefix.characters[num_prefix - 1]) || (!IsContextWhiteSpace(characters[num_prefix]) && breaks[num_prefix]);
	if (suffix.size)
		out_break_after = IsContextWhiteSpace(characters[num_prefix + num_characters - 1]) ||
			(!IsContextWhiteSpace(suffix.characters[0]) && breaks[num_prefix + num_characters]);
}

// An element the inline formatting context's text goes on through (fork, P6-L2): an inline box (not replaced, in flow).
static bool IsTransparentInlineBox(Element* element)
{
	const ComputedValues& computed = element->GetComputedValues();
	// (a form control is atomic whatever its display: asked first, its intrinsic size can cost)
	return computed.display() == Style::Display::Inline && computed.position() != Style::Position::Absolute &&
		computed.position() != Style::Position::Fixed && computed.float_() == Style::Float::None &&
		!rmlui_dynamic_cast<ElementFormControl*>(element) && !element->IsReplaced();
}

// An element with no part in the inline formatting context's content: not displayed, out of flow.
static bool IsSkippedInContext(Element* element)
{
	const ComputedValues& computed = element->GetComputedValues();
	return computed.display() == Style::Display::None || computed.position() == Style::Position::Absolute ||
		computed.position() == Style::Position::Fixed || computed.float_() != Style::Float::None;
}

// The margin, border and padding an inline box adds to the line on one side.
static float InlineBoxEdge(Element* element, bool left)
{
	const ComputedValues& computed = element->GetComputedValues();
	if (left)
		return ResolveValueOr(computed.margin_left(), 0.f, 0.f) + computed.border_left_width() + ResolveValueOr(computed.padding_left(), 0.f, 0.f);
	return ResolveValueOr(computed.margin_right(), 0.f, 0.f) + computed.border_right_width() + ResolveValueOr(computed.padding_right(), 0.f, 0.f);
}

// A few code points of the inline formatting context's text after (or before) a text, in order (fork, P6-L2).
static void GatherLineBreakContext(ElementText* text, bool forward, bool decode_escape_characters, int limit, LineBreakContext& out_context)
{
	const int capacity = Math::Min(limit, (int)LineBreakContext::capacity);
	out_context.size = 0;
	// (before: gathered from the text nearest backwards, kept in reverse order, turned round at the end)
	Character reversed[LineBreakContext::capacity];
	int num_reversed = 0;
	ElementText* other = text;
	while ((forward ? out_context.size : num_reversed) < capacity)
	{
		other = (forward ? other->GetContextNext() : other->GetContextPrevious());
		if (!other)
			break;
		const String& other_text = other->GetText();
		if (other_text.empty())
			continue;
		const char* p = other_text.c_str();
		const char* end = p + other_text.size();
		if (forward)
		{
			while (p != end && out_context.size < capacity)
			{
				const char* next = nullptr;
				bool non_breaking = false;
				out_context.characters[out_context.size++] = ReadCharacter(p, end, decode_escape_characters, next, non_breaking);
				p = next;
			}
		}
		else
		{
			// the last code points, read from a little before them into a ring
			if (other_text.size() > 4 * capacity)
			{
				p = end - 4 * capacity;
				while (p != end && IsContinuationByte(*p))
					++p;
			}
			Character ring[LineBreakContext::capacity];
			int count = 0;
			while (p != end)
			{
				const char* next = nullptr;
				bool non_breaking = false;
				ring[count % capacity] = ReadCharacter(p, end, decode_escape_characters, next, non_breaking);
				count++;
				p = next;
			}
			const int available = Math::Min(count, capacity);
			for (int i = 0; i < available && num_reversed < capacity; i++)
				reversed[num_reversed++] = ring[(count - 1 - i) % capacity];
		}
	}
	if (!forward)
	{
		for (int i = 0; i < num_reversed; i++)
			out_context.characters[i] = reversed[num_reversed - 1 - i];
		out_context.size = num_reversed;
	}
}

// A character of a run that BuildToken appends at once: printable ASCII other than a space and those a line may break
// next to between two ASCII characters (after '-' and '?', before '(' '<' '[' '{': Chromium's table), and an escape code's
// '&'.
static bool IsPlainAsciiByte(char c, bool decode_escape_characters)
{
	return c > ' ' && c < 0x7F && c != '-' && c != '?' && c != '(' && c != '<' && c != '[' && c != '{' &&
		!(decode_escape_characters && c == '&');
}

static bool IsWhiteSpaceCharacter(Character c)
{
	return (char32_t)c < 128 && StringUtilities::IsWhitespace(static_cast<char>(c));
}

// A token of preserved spaces.
static bool IsWhiteSpaceToken(const String& token)
{
	return !token.empty() && std::all_of(token.begin(), token.end(), [](char c) { return c == ' '; });
}

// The character before 'p' in the element's text (Null at its start).
static Character PreviousCharacter(const char* p, const char* text_begin)
{
	if (p <= text_begin)
		return Character::Null;
	const char* previous = StringUtilities::SeekBackwardUTF8(p - 1, text_begin);
	return StringUtilities::ToCharacter(previous, p);
}

// Appends 'character' to the token with the element's text-transform (CSS Text 3 §2.1: full case mappings,
// 'capitalize' titlecases the first letter of each word).
static void AppendTransformed(String& token, Character character, Character before_previous, Character previous, Character next,
	Style::TextTransform text_transform)
{
	switch (text_transform)
	{
	case Style::TextTransform::Uppercase: TextUnicode::AppendUppercase(token, character); return;
	case Style::TextTransform::Lowercase:
		// Greek capital sigma at the end of a word: final sigma (Unicode Final_Sigma context)
		if ((char32_t)character == 0x03A3 && TextUnicode::IsWordCharacter(previous) && !TextUnicode::IsWordCharacter(next))
			token += "\xCF\x82";
		else
			TextUnicode::AppendLowercase(token, character);
		return;
	case Style::TextTransform::Capitalize:
		if (TextUnicode::IsWordStart(before_previous, previous, character))
		{
			TextUnicode::AppendTitlecase(token, character);
			return;
		}
		break;
	case Style::TextTransform::None: break;
	}
	if ((char32_t)character < 0x80)
		token += static_cast<char>(character);
	else
		token += StringUtilities::ToUTF8(character);
}

static bool BuildToken(String& token, const char*& token_begin, const char* string_end, bool first_token, const TokenRules& rules)
{
	RMLUI_ASSERT(token_begin != string_end);

	token.reserve(string_end - token_begin + token.size());

	const bool collapse_white_space = rules.collapse_white_space;

	// The characters before the token, for capitalize.
	Character previous = Character::Null, before_previous = Character::Null;
	if (rules.text_transform != Style::TextTransform::None)
	{
		previous = PreviousCharacter(token_begin, rules.text_begin);
		if (previous != Character::Null)
			before_previous = PreviousCharacter(StringUtilities::SeekBackwardUTF8(token_begin - 1, rules.text_begin), rules.text_begin);
	}

	// Check what the first character of the token is; all we need to know is if it is white-space or not.
	bool parsing_white_space = StringUtilities::IsWhitespace(*token_begin);

	// Loop through the string from the token's beginning until we find an end to the token. This can occur in various
	// places, depending on the white-space processing;
	//  - at the end of a section of non-white-space characters,
	//  - at the end of a section of white-space characters, if we're not collapsing white-space,
	//  - at an endline token, if we're breaking on endlines,
	//  - at a line-break opportunity inside a section of non-white-space characters.
	const char* peeked_at = nullptr;
	const char* peeked_next = nullptr;
	Character peeked = Character::Null;
	bool peeked_non_breaking = false;
	while (token_begin != string_end)
	{
		// (the character read ahead by the previous iteration, if any)
		const char* next = peeked_next;
		bool non_breaking = peeked_non_breaking;
		const Character character =
			(peeked_at == token_begin ? peeked : ReadCharacter(token_begin, string_end, rules.decode_escape_characters, next, non_breaking));

		// Check for an endline token; if we're breaking on endlines and we find one, then return true to indicate a
		// forced break.
		if (rules.break_at_endline && character == Character('\n'))
		{
			token += '\n';
			token_begin = next;
			return true;
		}

		// If we've transitioned from white-space characters to non-white-space characters, or vice-versa, then check
		// if should terminate the token; if we're not collapsing white-space, then yes (as sections of white-space are
		// non-breaking), otherwise only if we've transitioned from characters to white-space.
		const bool white_space = !non_breaking && IsWhiteSpaceCharacter(character);
		if (white_space != parsing_white_space)
		{
			if (!collapse_white_space)
				return false;

			// We're collapsing white-space; we only tokenise words, not white-space, so we're only done tokenising
			// once we've begun parsing non-white-space and then found white-space.
			if (!parsing_white_space)
			{
				// However, if we are the last non-whitespace character in the string, and there are trailing
				// whitespace characters after this token, then we need to append a single space to the end of this
				// token.
				if (LastToken(token_begin, string_end, collapse_white_space, rules.break_at_endline))
					token += ' ';

				return false;
			}

			// We've transitioned from white-space to non-white-space, so we append a single white-space character.
			if (!first_token)
				token += ' ';

			parsing_white_space = false;
		}

		// If the current character is white-space, we'll append a space character to the token if we're not collapsing
		// sections of white-space.
		if (white_space)
		{
			token_begin = next;
			if (!collapse_white_space)
			{
				token += ' ';
				// 'break-spaces': each preserved space is a token of its own, a line may break after any of them.
				if (rules.break_spaces)
					return false;
			}
		}
		else
		{
			// A run of plain ASCII characters, without a text transform: no line-break opportunity between two of them (only
			// after a hyphen), so all but the last are appended at once; the last one is read again and paired with the
			// character after the run as usual. (Each one is not followed by stray continuation bytes: ReadCharacter reads
			// it alone.)
			if (rules.text_transform == Style::TextTransform::None && !non_breaking && next == token_begin + 1 &&
				IsPlainAsciiByte(*token_begin, rules.decode_escape_characters))
			{
				const char* run_end = next;
				while (run_end != string_end && IsPlainAsciiByte(*run_end, rules.decode_escape_characters) &&
					(run_end + 1 == string_end || !IsContinuationByte(run_end[1])))
					++run_end;
				if (run_end - token_begin >= 2)
				{
					token.append(token_begin, run_end - 1);
					token_begin = run_end - 1;
					peeked_at = nullptr;
					// ('previous' and 'before_previous' are read by the text transforms only.)
					continue;
				}
			}

			// The character after this one, for line breaking and the final sigma.
			Character following = Character::Null;
			bool following_non_breaking = false;
			const char* after_following = nullptr;
			if (next != string_end)
			{
				following = ReadCharacter(next, string_end, rules.decode_escape_characters, after_following, following_non_breaking);
				peeked_at = next;
				peeked = following;
				peeked_next = after_following;
				peeked_non_breaking = following_non_breaking;
			}

			AppendTransformed(token, character, before_previous, previous, following, rules.text_transform);
			token_begin = next;

			// A line-break opportunity between this character and the next ends the token (the text's UAX #14
			// opportunities, ComputeLineBreaks).
			if (following != Character::Null && !IsWhiteSpaceCharacter(following) && !non_breaking && !following_non_breaking)
			{
				const size_t offset = size_t(next - rules.text_begin);
				if (rules.line_breaks)
				{
					if (rules.line_breaks[offset])
						return false;
				}
				else
				{
					// (Chromium's table breaks after '-' and '?', and before '(' '<' '[' '{', nowhere else)
					const char previous_byte = rules.text_begin[offset - 1], byte = rules.text_begin[offset];
					if ((previous_byte == '-' || previous_byte == '?' || byte == '(' || byte == '<' || byte == '[' || byte == '{') &&
						TextUnicode::IsAsciiLineBreakOpportunity(
							offset >= 2 ? (char32_t)rules.text_begin[offset - 2] : (char32_t)rules.context_before, (char32_t)previous_byte, (char32_t)byte))
						return false;
				}
			}
		}

		before_previous = previous;
		previous = character;
	}

	return false;
}

static bool LastToken(const char* token_begin, const char* string_end, bool collapse_white_space, bool break_at_endline)
{
	bool last_token = (token_begin == string_end);
	if (collapse_white_space && !last_token)
	{
		last_token = true;
		const char* character = token_begin;

		while (character != string_end)
		{
			if (!StringUtilities::IsWhitespace(*character) || (break_at_endline && *character == '\n'))
			{
				last_token = false;
				break;
			}

			character++;
		}
	}

	return last_token;
}

} // namespace Rml
