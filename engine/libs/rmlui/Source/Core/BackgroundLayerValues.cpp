#include "BackgroundLayerValues.h"
#include "ElementAnimation.h"
#include "../../Include/RmlUi/Core/Decorator.h"
#include "../../Include/RmlUi/Core/StyleSheetTypes.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/Math.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/PropertyDictionary.h"
#include "../../Include/RmlUi/Core/PropertyParser.h"
#include "../../Include/RmlUi/Core/PropertySpecification.h"
#include "../../Include/RmlUi/Core/StringUtilities.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "../../Include/RmlUi/Core/TypeConverter.h"
#include <algorithm>
#include "PropertyShorthandDefinition.h"
#include <mutex>
#include <numeric>

namespace Rml {

RMLUI_RTTI_Define(ImageLayerList)
RMLUI_RTTI_Define(LayerPositionList)
RMLUI_RTTI_Define(LayerSizeList)
RMLUI_RTTI_Define(LayerKeywordList)

// ---- Helpers ------------------------------------------------------------------------------------------------------------

StringList SplitLayerList(const String& value, char separator)
{
	StringList result;
	String item;
	int depth = 0;
	char quote = 0;
	const bool whitespace = (separator == ' ');
	for (char c : value)
	{
		if (quote)
		{
			if (c == quote)
				quote = 0;
			item += c;
			continue;
		}
		if (c == '"' || c == '\'')
			quote = c;
		else if (c == '(')
			depth++;
		else if (c == ')')
			depth--;
		const bool is_separator = (depth == 0 && (whitespace ? (c == ' ' || c == '\t' || c == '\n' || c == '\r') : c == separator));
		if (is_separator)
		{
			item = StringUtilities::StripWhitespace(item);
			if (!whitespace || !item.empty())
				result.push_back(item);
			item.clear();
			continue;
		}
		item += c;
	}
	item = StringUtilities::StripWhitespace(item);
	if (!whitespace || !item.empty())
		result.push_back(item);
	return result;
}

namespace {

	String Lower(const String& value)
	{
		return StringUtilities::ToLower(value);
	}

	void SetStyleValue(Property& property, StyleValuePtr value)
	{
		property.value = Variant(std::move(value));
		property.unit = Unit::STYLE_VALUE;
	}

	String FunctionName(const String& token)
	{
		const size_t open = token.find('(');
		if (open == String::npos || token.back() != ')')
			return String();
		return Lower(StringUtilities::StripWhitespace(token.substr(0, open)));
	}

	// The name before the first '(' (a decorator function followed by keywords).
	String FunctionLikeName(const String& token)
	{
		const size_t open = token.find('(');
		return open == String::npos ? String() : Lower(StringUtilities::StripWhitespace(token.substr(0, open)));
	}

	String FunctionArguments(const String& token)
	{
		const size_t open = token.find('(');
		return StringUtilities::StripWhitespace(token.substr(open + 1, token.size() - open - 2));
	}

	String Unquote(String text)
	{
		text = StringUtilities::StripWhitespace(text);
		if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') && text.back() == text.front())
			text = text.substr(1, text.size() - 2);
		return text;
	}

	bool IsMathFunction(const String& name)
	{
		return name == "calc" || name == "min" || name == "max" || name == "clamp";
	}

	// A token that is an image: url(), image(), image-set(), or another function that is not a length or a colour.
	bool IsColorFunction(const String& name)
	{
		static const char* const names[] = {"rgb", "rgba", "hsl", "hsla", "hwb", "lab", "lch", "oklab", "oklch", "color", "color-mix",
			"light-dark"};
		for (const char* n : names)
			if (name == n)
				return true;
		return false;
	}

	bool IsImageToken(const String& token)
	{
		if (Lower(token) == "none")
			return true;
		const String name = FunctionName(token);
		return !name.empty() && !IsMathFunction(name) && !IsColorFunction(name) && name != "var";
	}

	// The first url of image() / image-set(): url("a.png") or a string.
	bool FirstUrl(const String& arguments, String& url)
	{
		for (const String& item : SplitLayerList(arguments, ','))
		{
			for (const String& word : SplitLayerList(item, ' '))
			{
				if (FunctionName(word) == "url")
				{
					url = Unquote(FunctionArguments(word));
					return !url.empty();
				}
				if (!word.empty() && (word.front() == '"' || word.front() == '\''))
				{
					url = Unquote(word);
					return !url.empty();
				}
			}
		}
		return false;
	}

	// A length or percentage of a position, interpolable: its px part and its % part (calc() of both as resolved).
	struct LinearLength {
		float px = 0.f, percent = 0.f;
	};
	LinearLength Linearize(const LengthPercentageValue& value, Element& element)
	{
		const float at_zero = ResolveLengthPercentage(value, element, 0.f);
		const float at_hundred = ResolveLengthPercentage(value, element, 100.f);
		return LinearLength{at_zero, at_hundred - at_zero};
	}
	LengthPercentageValue MixLength(const LengthPercentageValue& a, const LengthPercentageValue& b, float alpha, Element& element)
	{
#ifdef RMLUI_MATH_EXPRESSIONS
		const bool simple = !a.calculation && !b.calculation;
#else
		const bool simple = true;
#endif
		if (simple && a.value.unit == b.value.unit)
			return LengthPercentageValue(Math::Lerp(alpha, a.value.number, b.value.number), a.value.unit);
		const LinearLength la = Linearize(a, element), lb = Linearize(b, element);
		const float px = Math::Lerp(alpha, la.px, lb.px), percent = Math::Lerp(alpha, la.percent, lb.percent);
		if (percent == 0.f)
			return LengthPercentageValue(px, Unit::PX);
		if (px == 0.f)
			return LengthPercentageValue(percent, Unit::PERCENT);
		LengthPercentageValue result;
		if (ParseLengthPercentage(CreateString("calc(%gpx + %g%%)", px, percent), result))
			return result;
		return alpha < 0.5f ? a : b;
	}

	template <typename T>
	size_t RepeatableLength(const Vector<T>& a, const Vector<T>& b)
	{
		if (a.empty() || b.empty())
			return 0;
		return std::lcm(a.size(), b.size());
	}

	const char* RepeatName(LayerRepeat repeat)
	{
		switch (repeat)
		{
		case LayerRepeat::Repeat: return "repeat";
		case LayerRepeat::Space: return "space";
		case LayerRepeat::Round: return "round";
		case LayerRepeat::NoRepeat: return "no-repeat";
		}
		return "repeat";
	}

	bool ParseRepeatKeyword(const String& token, LayerRepeat& repeat)
	{
		const String t = Lower(token);
		if (t == "repeat")
			repeat = LayerRepeat::Repeat;
		else if (t == "space")
			repeat = LayerRepeat::Space;
		else if (t == "round")
			repeat = LayerRepeat::Round;
		else if (t == "no-repeat")
			repeat = LayerRepeat::NoRepeat;
		else
			return false;
		return true;
	}

	bool ParseBoxKeyword(const String& token, BoxArea& box)
	{
		const String t = Lower(token);
		if (t == "border-box")
			box = BoxArea::Border;
		else if (t == "padding-box")
			box = BoxArea::Padding;
		else if (t == "content-box")
			box = BoxArea::Content;
		else
			return false;
		return true;
	}

	const char* BoxName(BoxArea box)
	{
		switch (box)
		{
		case BoxArea::Padding: return "padding-box";
		case BoxArea::Content: return "content-box";
		default: return "border-box";
		}
	}

	bool ParseAttachmentKeyword(const String& token, LayerAttachment& attachment)
	{
		const String t = Lower(token);
		if (t == "scroll")
			attachment = LayerAttachment::Scroll;
		else if (t == "fixed")
			attachment = LayerAttachment::Fixed;
		else if (t == "local")
			attachment = LayerAttachment::Local;
		else
			return false;
		return true;
	}

	bool IsPositionToken(const String& token)
	{
		const String t = Lower(token);
		if (t == "left" || t == "right" || t == "top" || t == "bottom" || t == "center")
			return true;
		LengthPercentageValue value;
		return ParseLengthPercentage(token, value);
	}

	// One axis of a position longhand: center | [left|right]? <length-percentage>? (x), top|bottom (y).
	bool ParseAxisPosition(const String& item, bool horizontal, LengthPercentageValue& result)
	{
		const StringList words = SplitLayerList(item, ' ');
		if (words.empty() || words.size() > 2)
			return false;
		const String near_edge = horizontal ? "left" : "top", far_edge = horizontal ? "right" : "bottom";
		const String first = Lower(words[0]);
		if (words.size() == 1)
		{
			if (first == "center")
				result = LengthPercentageValue(50.f, Unit::PERCENT);
			else if (first == near_edge)
				result = LengthPercentageValue(0.f, Unit::PERCENT);
			else if (first == far_edge)
				result = LengthPercentageValue(100.f, Unit::PERCENT);
			else
				return ParseLengthPercentage(words[0], result);
			return true;
		}
		if (first != near_edge && first != far_edge)
			return false;
		LengthPercentageValue offset;
		if (!ParseLengthPercentage(words[1], offset))
			return false;
		if (first == near_edge)
		{
			result = offset;
			return true;
		}
#ifdef RMLUI_MATH_EXPRESSIONS
		if (!offset.calculation && offset.value.unit == Unit::PERCENT)
		{
			result = LengthPercentageValue(100.f - offset.value.number, Unit::PERCENT);
			return true;
		}
		return ParseLengthPercentage("calc(100% - " + words[1] + ")", result);
#else
		if (offset.value.unit != Unit::PERCENT)
			return false;
		result = LengthPercentageValue(100.f - offset.value.number, Unit::PERCENT);
		return true;
#endif
	}

	bool ParseSizeItem(const String& item, LayerSize& size)
	{
		const StringList words = SplitLayerList(item, ' ');
		if (words.empty() || words.size() > 2)
			return false;
		size = LayerSize();
		const String first = Lower(words[0]);
		if (first == "cover" || first == "contain")
		{
			if (words.size() != 1)
				return false;
			size.mode = (first == "cover" ? LayerSize::Mode::Cover : LayerSize::Mode::Contain);
			return true;
		}
		auto axis = [](const String& word, bool& automatic, LengthPercentageValue& value) {
			if (Lower(word) == "auto")
			{
				automatic = true;
				return true;
			}
			automatic = false;
			if (!ParseLengthPercentage(word, value))
				return false;
#ifdef RMLUI_MATH_EXPRESSIONS
			if (!value.calculation && value.value.number < 0.f)
				return false;
#else
			if (value.value.number < 0.f)
				return false;
#endif
			return true;
		};
		if (!axis(words[0], size.auto_x, size.x))
			return false;
		if (words.size() == 2)
			return axis(words[1], size.auto_y, size.y);
		size.auto_y = true;
		return true;
	}

	String SizeToString(const LayerSize& size)
	{
		if (size.mode == LayerSize::Mode::Cover)
			return "cover";
		if (size.mode == LayerSize::Mode::Contain)
			return "contain";
		const String x = size.auto_x ? String("auto") : size.x.ToString();
		if (size.auto_y)
			return x;
		return x + " " + size.y.ToString();
	}

	// ---- Parsers ----

	// The image lists parsed last, by text (fork, P5-6): a value with var() is parsed again each time it is resolved (style
	// computation, then the background's geometry and colour clip), and the elements whose var() resolve to the same text
	// parse it each; a list is immutable, so they share the first one. Dropped when decorator instancers change.
	struct ImageLayerParseCache {
		struct Entry {
			String text;
			bool mask = false;
			StyleValuePtr list;
		};
		static constexpr size_t capacity = 64;
		std::mutex mutex;
		Vector<Entry> entries; // the most recent last
		uint64_t hits = 0;
	};
	ImageLayerParseCache& GetImageLayerParseCache()
	{
		static ImageLayerParseCache cache;
		return cache;
	}

	class PropertyParserImageLayers final : public PropertyParser {
	public:
		bool ParseValue(Property& property, const String& value, const ParameterMap& parameters) const override
		{
			const String stripped = StringUtilities::StripWhitespace(value);
			if (stripped.empty())
				return false;
			const bool mask = (parameters.find("mask") != parameters.end());
			ImageLayerParseCache& cache = GetImageLayerParseCache();
			{
				std::lock_guard<std::mutex> lock(cache.mutex);
				for (size_t i = cache.entries.size(); i-- > 0;)
				{
					ImageLayerParseCache::Entry& entry = cache.entries[i];
					if (entry.mask == mask && entry.text == stripped)
					{
						SetStyleValue(property, entry.list);
						cache.hits++;
						return true;
					}
				}
			}
			StyleValuePtr parsed;
			if (!Parse(parsed, stripped, mask))
				return false;
			{
				std::lock_guard<std::mutex> lock(cache.mutex);
				if (cache.entries.size() >= ImageLayerParseCache::capacity)
					cache.entries.erase(cache.entries.begin());
				cache.entries.push_back(ImageLayerParseCache::Entry{stripped, mask, parsed});
			}
			SetStyleValue(property, std::move(parsed));
			return true;
		}

	private:
		static bool Parse(StyleValuePtr& out, const String& stripped, bool mask)
		{
			auto list = MakeShared<ImageLayerList>();
			list->mask = mask;
			auto decorators = MakeShared<DecoratorDeclarationList>();
			PropertyParser* decorator_parser = StyleSheetSpecification::GetParser("decorator");
			for (const String& item : SplitLayerList(stripped, ','))
			{
				if (item.empty())
					return false;
				ImageLayer layer;
				layer.text = item;
				const String name = FunctionName(item);
				if (Lower(item) == "none")
				{
					layer.kind = ImageLayer::Kind::None;
				}
				else if (name == "url")
				{
					layer.kind = ImageLayer::Kind::Url;
					layer.url = Unquote(FunctionArguments(item));
					if (layer.url.empty())
						return false;
				}
				else if ((name == "image" || name == "image-set" || name == "-webkit-image-set") && FirstUrl(FunctionArguments(item), layer.url))
				{
					// CSS image() and image-set(): the first url() or string. (RCSS's image() decorator, with an unquoted
					// source and its fit and alignment, is a decorator below.)
					layer.kind = ImageLayer::Kind::Url;
				}
				else if (decorator_parser && (item.find('(') != String::npos || item.find(' ') == String::npos) &&
					FunctionLikeName(item) != "cross-fade" &&
					FunctionLikeName(item) != "element" && !IsMathFunction(FunctionLikeName(item)) && !IsColorFunction(FunctionLikeName(item)))
				{
					// (RCSS: a decorator function may be followed by its paint area, 'border-box'.)
					// A gradient or a registered decorator function.
					Property parsed;
					if (!decorator_parser->ParseValue(parsed, item, ParameterMap()) || parsed.unit != Unit::DECORATOR)
						return false;
					const DecoratorsPtr& declarations = parsed.value.GetReference<DecoratorsPtr>();
					// (Without an instancer: a named @decorator of the style sheet.)
					if (!declarations || declarations->list.size() != 1)
						return false;
					layer.kind = ImageLayer::Kind::Decorator;
					layer.decorator_index = (int)decorators->list.size();
					decorators->list.push_back(declarations->list[0]);
				}
				else
				{
					return false;
				}
				list->layers.push_back(std::move(layer));
			}
			decorators->value = stripped;
			list->decorators = std::move(decorators);
			out = std::move(list);
			return true;
		}
	};

	class PropertyParserLayerPosition final : public PropertyParser {
	public:
		explicit PropertyParserLayerPosition(bool horizontal) : horizontal(horizontal) {}
		bool ParseValue(Property& property, const String& value, const ParameterMap& /*parameters*/) const override
		{
			auto list = MakeShared<LayerPositionList>();
			for (const String& item : SplitLayerList(value, ','))
			{
				LengthPercentageValue position;
				if (!ParseAxisPosition(item, horizontal, position))
					return false;
				list->values.push_back(std::move(position));
			}
			if (list->values.empty())
				return false;
			SetStyleValue(property, std::move(list));
			return true;
		}

	private:
		bool horizontal;
	};

	class PropertyParserLayerSize final : public PropertyParser {
	public:
		bool ParseValue(Property& property, const String& value, const ParameterMap& /*parameters*/) const override
		{
			auto list = MakeShared<LayerSizeList>();
			for (const String& item : SplitLayerList(value, ','))
			{
				LayerSize size;
				if (!ParseSizeItem(item, size))
					return false;
				list->values.push_back(std::move(size));
			}
			if (list->values.empty())
				return false;
			SetStyleValue(property, std::move(list));
			return true;
		}
	};

	bool ParseRepeatItem(const String& item, LayerRepeat& x, LayerRepeat& y)
	{
		const StringList words = SplitLayerList(item, ' ');
		if (words.size() == 1)
		{
			const String t = Lower(words[0]);
			if (t == "repeat-x")
			{
				x = LayerRepeat::Repeat;
				y = LayerRepeat::NoRepeat;
				return true;
			}
			if (t == "repeat-y")
			{
				x = LayerRepeat::NoRepeat;
				y = LayerRepeat::Repeat;
				return true;
			}
			if (!ParseRepeatKeyword(words[0], x))
				return false;
			y = x;
			return true;
		}
		return words.size() == 2 && ParseRepeatKeyword(words[0], x) && ParseRepeatKeyword(words[1], y);
	}

	class PropertyParserLayerKeywords final : public PropertyParser {
	public:
		explicit PropertyParserLayerKeywords(LayerKeywordList::Type type) : type(type) {}
		bool ParseValue(Property& property, const String& value, const ParameterMap& /*parameters*/) const override
		{
			auto list = MakeShared<LayerKeywordList>();
			list->type = type;
			for (const String& item : SplitLayerList(value, ','))
			{
				if (type == LayerKeywordList::Type::Repeat)
				{
					LayerRepeat x, y;
					if (!ParseRepeatItem(item, x, y))
						return false;
					list->values.push_back(uint8_t(x));
					list->values.push_back(uint8_t(y));
				}
				else if (type == LayerKeywordList::Type::Box)
				{
					BoxArea box;
					if (!ParseBoxKeyword(item, box))
						return false;
					list->values.push_back(uint8_t(box));
				}
				else
				{
					LayerAttachment attachment;
					if (!ParseAttachmentKeyword(item, attachment))
						return false;
					list->values.push_back(uint8_t(attachment));
				}
			}
			if (list->values.empty())
				return false;
			SetStyleValue(property, std::move(list));
			return true;
		}

	private:
		LayerKeywordList::Type type;
	};

	// ---- Shorthands ----

	struct LayerParts {
		String image = "none";
		String position_x = "0%", position_y = "0%";
		String size = "auto";
		String repeat = "repeat";
		String attachment = "scroll";
		String origin, clip;
		String color;
	};

	// One layer of 'background' (with_color: the last one) or of 'mask'.
	bool ParseLayer(const String& text, bool is_mask, bool with_color, const PropertySpecification& specification, LayerParts& parts)
	{
		// '/' outside of parentheses is a word of its own.
		String spaced;
		int depth = 0;
		for (char c : text)
		{
			if (c == '(')
				depth++;
			else if (c == ')')
				depth--;
			if (c == '/' && depth == 0)
				spaced += " / ";
			else
				spaced += c;
		}
		const StringList words = SplitLayerList(spaced, ' ');
		if (words.empty())
			return false;

		bool has_image = false, has_position = false, has_repeat = false, has_attachment = false, has_color = false;
		int num_boxes = 0;
		for (size_t i = 0; i < words.size();)
		{
			const String& word = words[i];
			const String lower = Lower(word);
			if (!has_image && IsImageToken(word))
			{
				parts.image = word;
				has_image = true;
				i++;
				continue;
			}
			if (!has_position && IsPositionToken(word))
			{
				StringList tokens;
				while (i < words.size() && tokens.size() < 4 && IsPositionToken(words[i]))
					tokens.push_back(words[i++]);
				LengthPercentageValue x, y;
				if (!ParsePosition(tokens, x, y))
					return false;
				parts.position_x = x.ToString();
				parts.position_y = y.ToString();
				has_position = true;
				if (i < words.size() && words[i] == "/")
				{
					i++;
					StringList size_words;
					while (i < words.size() && size_words.size() < 2)
					{
						const String t = Lower(words[i]);
						LengthPercentageValue length;
						if (t == "auto" || t == "cover" || t == "contain" || ParseLengthPercentage(words[i], length))
						{
							size_words.push_back(words[i++]);
							if (t == "cover" || t == "contain")
								break;
						}
						else
							break;
					}
					if (size_words.empty())
						return false;
					String size_text = size_words[0];
					if (size_words.size() == 2)
						size_text += " " + size_words[1];
					LayerSize size;
					if (!ParseSizeItem(size_text, size))
						return false;
					parts.size = size_text;
				}
				continue;
			}
			if (!has_repeat && (lower == "repeat-x" || lower == "repeat-y"))
			{
				parts.repeat = lower;
				has_repeat = true;
				i++;
				continue;
			}
			LayerRepeat repeat;
			if (!has_repeat && ParseRepeatKeyword(word, repeat))
			{
				parts.repeat = lower;
				i++;
				if (i < words.size() && ParseRepeatKeyword(words[i], repeat))
					parts.repeat += " " + Lower(words[i++]);
				has_repeat = true;
				continue;
			}
			LayerAttachment attachment;
			if (!is_mask && !has_attachment && ParseAttachmentKeyword(word, attachment))
			{
				parts.attachment = lower;
				has_attachment = true;
				i++;
				continue;
			}
			BoxArea box;
			if (num_boxes < 2 && ParseBoxKeyword(word, box))
			{
				if (num_boxes == 0)
					parts.origin = parts.clip = lower;
				else
					parts.clip = lower;
				num_boxes++;
				i++;
				continue;
			}
			if (is_mask)
			{
				// The mask properties the fork has not (mask-mode, mask-composite, mask-clip: no-clip): accepted, without
				// an effect (compat reports them).
				if (lower == "alpha" || lower == "luminance" || lower == "match-source" || lower == "add" || lower == "subtract" ||
					lower == "intersect" || lower == "exclude" || lower == "no-clip")
				{
					i++;
					continue;
				}
			}
			if (with_color && !has_color)
			{
				PropertyDictionary color;
				if (specification.ParsePropertyDeclaration(color, PropertyId::BackgroundColor, word))
				{
					parts.color = word;
					has_color = true;
					i++;
					continue;
				}
			}
			return false;
		}
		return true;
	}

	bool ApplyParts(PropertyDictionary& dictionary, const PropertySpecification& specification, const Vector<std::pair<PropertyId, String>>& values)
	{
		PropertyDictionary parsed;
		for (const auto& id_value : values)
		{
			if (!specification.ParsePropertyDeclaration(parsed, id_value.first, id_value.second))
				return false;
		}
		for (const auto& id_property : parsed.GetProperties())
			dictionary.SetProperty(id_property.first, id_property.second);
		return true;
	}

	String Join(const StringList& list)
	{
		String out;
		for (const String& item : list)
		{
			if (!out.empty())
				out += ", ";
			out += item;
		}
		return out;
	}

} // namespace

// ---- Values -------------------------------------------------------------------------------------------------------------

bool ImageLayerList::HasImages() const
{
	for (const ImageLayer& layer : layers)
		if (layer.kind != ImageLayer::Kind::None)
			return true;
	return false;
}

String ImageLayerList::ToString() const
{
	String out;
	for (const ImageLayer& layer : layers)
	{
		if (!out.empty())
			out += ", ";
		out += layer.text;
	}
	return out;
}

bool ImageLayerList::Equals(const StyleValue& other) const
{
	return ToString() == static_cast<const ImageLayerList&>(other).ToString();
}

namespace {
	// The url()s of an image of a list and their weights: a url() alone, or a cross-fade.
	Vector<std::pair<String, float>> FadeLeaves(const ImageLayer& layer)
	{
		if (layer.kind == ImageLayer::Kind::CrossFade)
			return layer.fade;
		return {{layer.url, 1.f}};
	}

	// cross-fade(A, B, alpha): the url()s of both, weighted by 1 - alpha and alpha (the same url() once); serialized as
	// Chromium's computed value, e.g. 'cross-fade(url("a.png") 75%, url("b.png"))'.
	ImageLayer CrossFade(const ImageLayer& from, const ImageLayer& to, float alpha)
	{
		Vector<std::pair<String, float>> leaves;
		auto add = [&leaves](const Vector<std::pair<String, float>>& source, float factor) {
			for (const auto& leaf : source)
			{
				const float weight = leaf.second * factor;
				if (weight <= 0.f)
					continue;
				auto it = std::find_if(leaves.begin(), leaves.end(), [&](const std::pair<String, float>& entry) { return entry.first == leaf.first; });
				if (it != leaves.end())
					it->second += weight;
				else
					leaves.emplace_back(leaf.first, weight);
			}
		};
		add(FadeLeaves(from), 1.f - alpha);
		add(FadeLeaves(to), alpha);

		ImageLayer layer;
		if (leaves.size() == 1)
		{
			layer.kind = ImageLayer::Kind::Url;
			layer.url = leaves[0].first;
			layer.text = "url(" + layer.url + ")";
			return layer;
		}
		layer.kind = ImageLayer::Kind::CrossFade;
		layer.text = "cross-fade(";
		for (size_t i = 0; i < leaves.size(); i++)
		{
			if (i > 0)
				layer.text += ", ";
			layer.text += "url(" + leaves[i].first + ")";
			if (i + 1 < leaves.size())
				layer.text += CreateString(" %g%%", double(leaves[i].second) * 100.0);
		}
		layer.text += ")";
		layer.fade = std::move(leaves);
		return layer;
	}

	bool OnlyImages(const ImageLayerList& list)
	{
		if (list.layers.empty() || list.mask)
			return false;
		for (const ImageLayer& layer : list.layers)
			if (layer.kind != ImageLayer::Kind::Url && layer.kind != ImageLayer::Kind::CrossFade)
				return false;
		return true;
	}
} // namespace

// Fork (phase 5, P5-3): the same layers, the differing ones decorator functions of one instancer that opts in
// (DecoratorInstancer::InterpolatesAsImageLayer, the engine's effects), interpolated property by property.
static SharedPtr<const StyleValue> InterpolateDecoratorLayers(const ImageLayerList& from, const ImageLayerList& to, float alpha, Element& element)
{
	if (from.layers.size() != to.layers.size() || from.layers.empty() || !from.decorators || !to.decorators)
		return nullptr;
	auto result = MakeShared<ImageLayerList>();
	result->mask = from.mask;
	result->layers = from.layers;
	auto declarations = MakeShared<DecoratorDeclarationList>();
	bool interpolated = false;
	for (size_t i = 0; i < from.layers.size(); i++)
	{
		const ImageLayer& a = from.layers[i];
		const ImageLayer& b = to.layers[i];
		if (a.kind != b.kind)
			return nullptr;
		if (a.kind != ImageLayer::Kind::Decorator)
		{
			if (a.kind == ImageLayer::Kind::CrossFade || a.url != b.url)
				return nullptr;
			continue;
		}
		if (a.decorator_index < 0 || a.decorator_index >= (int)from.decorators->list.size() || b.decorator_index < 0 ||
			b.decorator_index >= (int)to.decorators->list.size())
			return nullptr;
		const DecoratorDeclaration& da = from.decorators->list[a.decorator_index];
		const DecoratorDeclaration& db = to.decorators->list[b.decorator_index];
		if (!da.instancer || da.instancer != db.instancer)
			return nullptr;
		DecoratorDeclaration declaration{da.type, da.instancer, PropertyDictionary(), da.paint_area};
		if (da.instancer->InterpolatesAsImageLayer())
		{
			if (!InterpolateDecoratorDeclaration(declaration.properties, da, db, alpha, element))
				return nullptr;
			interpolated = true;
		}
		else
		{
			// another decorator function (a gradient) interpolates only when it does not change
			if (da.type != db.type || da.paint_area != db.paint_area || da.properties.GetNumProperties() != db.properties.GetNumProperties())
				return nullptr;
			for (const auto& entry : da.properties.GetProperties())
			{
				const Property* other = db.properties.GetProperty(entry.first);
				if (!other || !(*other == entry.second))
					return nullptr;
			}
			declaration.properties = da.properties;
		}
		result->layers[i].decorator_index = (int)declarations->list.size();
		if (da.instancer->InterpolatesAsImageLayer())
		{
			// the layer's text is the interpolated values (ToString and Equals compare the text: each frame's value
			// differs from the last, so the element takes it), in property id order
			Vector<std::pair<PropertyId, String>> values;
			for (const auto& entry : declaration.properties.GetProperties())
				values.emplace_back(entry.first, entry.second.ToString());
			std::sort(values.begin(), values.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
			String text = declaration.type + "(";
			for (size_t k = 0; k < values.size(); k++)
				text += (k ? " " : "") + values[k].second;
			result->layers[i].text = text + ")";
		}
		declarations->list.push_back(std::move(declaration));
	}
	if (!interpolated)
		return nullptr;
	// (an empty value: the style sheet does not cache the interpolated decorators)
	result->decorators = std::move(declarations);
	return result;
}

SharedPtr<const StyleValue> ImageLayerList::Interpolate(const StyleValue& to_value, float alpha, Element& element) const
{
	const ImageLayerList& to = static_cast<const ImageLayerList&>(to_value);
	if (SharedPtr<const StyleValue> layers = InterpolateDecoratorLayers(*this, to, alpha, element))
		return layers;
	if (!OnlyImages(*this) || !OnlyImages(to))
		return nullptr;

	const size_t from_count = layers.size(), to_count = to.layers.size();
	size_t a = from_count, b = to_count;
	while (b != 0)
	{
		const size_t r = a % b;
		a = b;
		b = r;
	}
	const size_t count = from_count / a * to_count;
	// (a list of hundreds of layers is no background: discrete)
	if (count > 256)
		return nullptr;

	auto result = MakeShared<ImageLayerList>();
	result->cross_faded = true;
	result->layers.reserve(count);
	for (size_t i = 0; i < count; i++)
		result->layers.push_back(CrossFade(layers[i % from_count], to.layers[i % to_count], alpha));
	return result;
}

Units ImageLayerList::GetUnitDependencies() const
{
	Units units = Unit::UNKNOWN;
	if (decorators)
	{
		for (const DecoratorDeclaration& declaration : decorators->list)
			for (const auto& id_property : declaration.properties.GetProperties())
			{
				const Property& property = id_property.second;
				if (property.unit == Unit::STYLE_VALUE)
				{
					const StyleValuePtr value = property.Get<StyleValuePtr>();
					if (value)
						units = units | value->GetUnitDependencies();
				}
				else
					units = units | (property.unit & (Unit::EM | Unit::REM | Unit::VW | Unit::VH | Unit::VMIN | Unit::VMAX));
			}
	}
	return units;
}

String LayerPositionList::ToString() const
{
	StringList items;
	for (const LengthPercentageValue& value : values)
		items.push_back(value.ToString());
	return Join(items);
}

bool LayerPositionList::Equals(const StyleValue& other) const
{
	return values == static_cast<const LayerPositionList&>(other).values;
}

SharedPtr<const StyleValue> LayerPositionList::Interpolate(const StyleValue& to_value, float alpha, Element& element) const
{
	const LayerPositionList& to = static_cast<const LayerPositionList&>(to_value);
	const size_t count = RepeatableLength(values, to.values);
	if (count == 0)
		return nullptr;
	auto result = MakeShared<LayerPositionList>();
	for (size_t i = 0; i < count; i++)
		result->values.push_back(MixLength(values[i % values.size()], to.values[i % to.values.size()], alpha, element));
	return result;
}

Units LayerPositionList::GetUnitDependencies() const
{
	Units units = Unit::UNKNOWN;
	for (const LengthPercentageValue& value : values)
		units = units | value.GetUnitDependencies();
	return units;
}

String LayerSizeList::ToString() const
{
	StringList items;
	for (const LayerSize& size : values)
		items.push_back(SizeToString(size));
	return Join(items);
}

bool LayerSizeList::Equals(const StyleValue& other) const
{
	return values == static_cast<const LayerSizeList&>(other).values;
}

SharedPtr<const StyleValue> LayerSizeList::Interpolate(const StyleValue& to_value, float alpha, Element& element) const
{
	const LayerSizeList& to = static_cast<const LayerSizeList&>(to_value);
	const size_t count = RepeatableLength(values, to.values);
	if (count == 0)
		return nullptr;
	auto result = MakeShared<LayerSizeList>();
	for (size_t i = 0; i < count; i++)
	{
		const LayerSize& a = values[i % values.size()];
		const LayerSize& b = to.values[i % to.values.size()];
		// Keywords and auto do not interpolate with lengths (CSS Backgrounds 3: by computed value, per axis).
		if (a.mode != LayerSize::Mode::Explicit || b.mode != LayerSize::Mode::Explicit || a.auto_x != b.auto_x || a.auto_y != b.auto_y)
			return nullptr;
		LayerSize mixed = a;
		if (!a.auto_x)
			mixed.x = MixLength(a.x, b.x, alpha, element);
		if (!a.auto_y)
			mixed.y = MixLength(a.y, b.y, alpha, element);
		result->values.push_back(std::move(mixed));
	}
	return result;
}

Units LayerSizeList::GetUnitDependencies() const
{
	Units units = Unit::UNKNOWN;
	for (const LayerSize& size : values)
	{
		if (!size.auto_x)
			units = units | size.x.GetUnitDependencies();
		if (!size.auto_y)
			units = units | size.y.GetUnitDependencies();
	}
	return units;
}

String LayerKeywordList::ToString() const
{
	StringList items;
	if (type == Type::Repeat)
	{
		for (size_t i = 0; i + 1 < values.size(); i += 2)
		{
			const LayerRepeat x = LayerRepeat(values[i]), y = LayerRepeat(values[i + 1]);
			if (x == y)
				items.push_back(RepeatName(x));
			else if (x == LayerRepeat::Repeat && y == LayerRepeat::NoRepeat)
				items.push_back("repeat-x");
			else if (x == LayerRepeat::NoRepeat && y == LayerRepeat::Repeat)
				items.push_back("repeat-y");
			else
				items.push_back(String(RepeatName(x)) + " " + RepeatName(y));
		}
	}
	else if (type == Type::Box)
	{
		for (uint8_t value : values)
			items.push_back(BoxName(BoxArea(value)));
	}
	else
	{
		static const char* const names[] = {"scroll", "fixed", "local"};
		for (uint8_t value : values)
			items.push_back(names[value < 3 ? value : 0]);
	}
	return Join(items);
}

bool LayerKeywordList::Equals(const StyleValue& other) const
{
	const LayerKeywordList& list = static_cast<const LayerKeywordList&>(other);
	return type == list.type && values == list.values;
}

// ---- Registration -------------------------------------------------------------------------------------------------------

void ClearImageLayerParseCache()
{
	ImageLayerParseCache& cache = GetImageLayerParseCache();
	std::lock_guard<std::mutex> lock(cache.mutex);
	cache.entries.clear();
}

uint64_t GetImageLayerParseCacheHits()
{
	ImageLayerParseCache& cache = GetImageLayerParseCache();
	std::lock_guard<std::mutex> lock(cache.mutex);
	return cache.hits;
}

void RegisterBackgroundLayerParsers()
{
	static PropertyParserImageLayers image_layers;
	static PropertyParserLayerPosition position_x(true), position_y(false);
	static PropertyParserLayerSize size;
	static PropertyParserLayerKeywords repeat(LayerKeywordList::Type::Repeat), box(LayerKeywordList::Type::Box),
		attachment(LayerKeywordList::Type::Attachment);
	StyleSheetSpecification::RegisterParser("image_layers", &image_layers);
	StyleSheetSpecification::RegisterParser("layer_position_x", &position_x);
	StyleSheetSpecification::RegisterParser("layer_position_y", &position_y);
	StyleSheetSpecification::RegisterParser("layer_size", &size);
	StyleSheetSpecification::RegisterParser("layer_repeat", &repeat);
	StyleSheetSpecification::RegisterParser("layer_box", &box);
	StyleSheetSpecification::RegisterParser("layer_attachment", &attachment);
}

bool ParseBackgroundShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& /*definition*/, const String& value,
	const PropertySpecification& specification)
{
	const StringList layers = SplitLayerList(value, ',');
	if (layers.empty())
		return false;
	StringList images, xs, ys, sizes, repeats, attachments, origins, clips;
	String color = "transparent";
	for (size_t i = 0; i < layers.size(); i++)
	{
		LayerParts parts;
		const bool last = (i + 1 == layers.size());
		if (layers[i].empty() || !ParseLayer(layers[i], false, last, specification, parts))
			return false;
		images.push_back(parts.image);
		xs.push_back(parts.position_x);
		ys.push_back(parts.position_y);
		sizes.push_back(parts.size);
		repeats.push_back(parts.repeat);
		attachments.push_back(parts.attachment);
		origins.push_back(parts.origin.empty() ? "padding-box" : parts.origin);
		clips.push_back(parts.clip.empty() ? "border-box" : parts.clip);
		if (last && !parts.color.empty())
			color = parts.color;
	}
	return ApplyParts(dictionary, specification,
		{
			{PropertyId::BackgroundColor, color},
			{PropertyId::BackgroundImage, Join(images)},
			{PropertyId::BackgroundPositionX, Join(xs)},
			{PropertyId::BackgroundPositionY, Join(ys)},
			{PropertyId::BackgroundSize, Join(sizes)},
			{PropertyId::BackgroundRepeat, Join(repeats)},
			{PropertyId::BackgroundAttachment, Join(attachments)},
			{PropertyId::BackgroundOrigin, Join(origins)},
			{PropertyId::BackgroundClip, Join(clips)},
		});
}

bool ParseMaskShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& /*definition*/, const String& value,
	const PropertySpecification& specification)
{
	const StringList layers = SplitLayerList(value, ',');
	if (layers.empty())
		return false;
	StringList images, xs, ys, sizes, repeats;
	for (const String& layer : layers)
	{
		LayerParts parts;
		if (layer.empty() || !ParseLayer(layer, true, false, specification, parts))
			return false;
		images.push_back(parts.image);
		xs.push_back(parts.position_x);
		ys.push_back(parts.position_y);
		sizes.push_back(parts.size);
		repeats.push_back(parts.repeat);
	}
	return ApplyParts(dictionary, specification,
		{
			{PropertyId::MaskImage, Join(images)},
			{PropertyId::MaskPositionX, Join(xs)},
			{PropertyId::MaskPositionY, Join(ys)},
			{PropertyId::MaskSize, Join(sizes)},
			{PropertyId::MaskRepeat, Join(repeats)},
		});
}

bool ParseLayerPositionShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification)
{
	if (definition.items.size() != 2)
		return false;
	StringList xs, ys;
	for (const String& item : SplitLayerList(value, ','))
	{
		LengthPercentageValue x, y;
		if (item.empty() || !ParsePosition(SplitLayerList(item, ' '), x, y))
			return false;
		xs.push_back(x.ToString());
		ys.push_back(y.ToString());
	}
	return ApplyParts(dictionary, specification, {{definition.items[0].property_id, Join(xs)}, {definition.items[1].property_id, Join(ys)}});
}

} // namespace Rml
