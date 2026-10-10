#include "PseudoElements.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/ElementInstancer.h"
#include "../../Include/RmlUi/Core/ElementText.h"
#include "../../Include/RmlUi/Core/Factory.h"
#include "../../Include/RmlUi/Core/PropertyIdSet.h"
#include "../../Include/RmlUi/Core/StyleSheet.h"
#include "../../Include/RmlUi/Core/SystemInterface.h"
#include "ContentValue.h"
#include "ElementCascade.h"
#include "ElementDefinition.h"
#include "ListMarker.h"
#include "PluginRegistry.h"

namespace Rml {

RMLUI_RTTI_Define(ElementPseudo)

static const char* PseudoElementTag(PseudoElementType type)
{
	switch (type)
	{
	case PseudoElementType::Before: return "::before";
	case PseudoElementType::After: return "::after";
	case PseudoElementType::Marker: return "::marker";
	case PseudoElementType::None: break;
	}
	return "";
}

ElementPseudo::ElementPseudo(const String& tag) : Element(tag) {}

ElementPseudo::~ElementPseudo() {}

bool ElementPseudo::UsesCounters() const
{
	return content && (content->UsesCounters() || content->UsesQuotes());
}

void ElementPseudo::OnCountersResolved(const CssCounterSet& counters, int& quote_depth)
{
	last_counters = counters;
	last_quote_depth = quote_depth;
	UpdateTexts(&counters, &quote_depth);
}

void ElementPseudo::OnOriginatingAttributeChange(const ElementAttributes& changed_attributes)
{
	if (!content || !content->UsesAttributes())
		return;
	for (const auto& attribute : changed_attributes)
	{
		if (content->UsesAttribute(attribute.first))
		{
			UpdateTexts(nullptr, nullptr);
			return;
		}
	}
}

void ElementPseudo::OnUpdate()
{
	Element::OnUpdate();
	if (content_dirty)
		BuildContent();
}

void ElementPseudo::OnPropertyChange(const PropertyIdSet& changed_properties)
{
	Element::OnPropertyChange(changed_properties);
	if (changed_properties.Contains(PropertyId::Content))
		content_dirty = true;
	if (changed_properties.Contains(PropertyId::Quotes) && content && content->UsesQuotes())
		UpdateTexts(nullptr, nullptr);
	// The computed values are up to date here: the children made now are updated in this frame.
	if (content_dirty)
		BuildContent();
}

void ElementPseudo::BuildContent()
{
	content_dirty = false;
	const Property* property = GetProperty(PropertyId::Content);
	SharedPtr<const ContentValue> new_content;
	if (property && property->unit == Unit::STYLE_VALUE)
		new_content = std::static_pointer_cast<const ContentValue>(property->Get<StyleValuePtr>());

	if (new_content == content || (new_content && content && new_content->Equals(*content)))
		return;

	const bool counted = UsesCounters();
	content = std::move(new_content);
	BuildChildren(content.get());
	UpdateTexts(nullptr, nullptr);
	if (counted || UsesCounters())
		PseudoElements::DirtyCounters(*this);
}

void ElementPseudo::BuildChildren(const ContentValue* new_content)
{
	while (GetNumChildren() > 0)
		RemoveChild(GetChild(GetNumChildren() - 1));
	text_runs.clear();
	if (!new_content)
		return;

	bool in_text_run = false;
	for (const ContentSegment& segment : new_content->segments)
	{
		if (segment.type != ContentSegment::Type::Url)
		{
			if (!in_text_run)
			{
				ElementPtr text = Factory::InstanceElement(this, "#text", "#text", XMLAttributes());
				text_runs.push_back(text ? AppendChild(std::move(text)) : nullptr);
				in_text_run = true;
			}
			continue;
		}

		// An image: the URL as written, relative to the style sheet that declares it.
		in_text_run = false;
		String source = segment.text;
		const Property* property = GetProperty(PropertyId::Content);
		const bool relative = !source.empty() && source[0] != '/' && source.find(':') == String::npos;
		if (relative && property && property->source && !property->source->path.empty())
		{
			if (SystemInterface* system_interface = GetSystemInterface())
			{
				String joined;
				system_interface->JoinPath(joined, StringUtilities::Replace(property->source->path, '|', ':'), source);
				source = StringUtilities::Replace(joined, ':', '|');
			}
		}
		XMLAttributes attributes;
		attributes["src"] = source;
		ElementPtr image = Factory::InstanceElement(this, "img", "img", attributes);
		if (image)
			AppendChild(std::move(image));
		text_runs.push_back(nullptr);
	}
}

String ElementPseudo::GetQuote(int depth, bool open)
{
	const Property* property = GetProperty(PropertyId::Quotes);
	if (property && property->unit == Unit::STYLE_VALUE)
	{
		const auto& quotes = static_cast<const QuotesValue&>(*property->Get<StyleValuePtr>());
		if (quotes.pairs.empty())
			return String();
		const auto& pair = quotes.pairs[Math::Clamp(depth, 0, (int)quotes.pairs.size() - 1)];
		return open ? pair.first : pair.second;
	}
	if (property && property->unit == Unit::KEYWORD && property->Get<int>() == 1)
		return String(); // none

	// auto: the English quotes, “ ” then ‘ ’.
	if (depth <= 0)
		return open ? "\xE2\x80\x9C" : "\xE2\x80\x9D";
	return open ? "\xE2\x80\x98" : "\xE2\x80\x99";
}

void ElementPseudo::UpdateTexts(const CssCounterSet* counters, int* quote_depth)
{
	if (!content)
		return;
	const CssCounterSet& counter_set = (counters ? *counters : last_counters);
	int local_depth = last_quote_depth;
	int& depth = (quote_depth ? *quote_depth : local_depth);
	Element* originating = GetOriginatingElement();

	size_t run = 0;
	String text;
	bool in_text_run = false;
	auto Flush = [&]() {
		if (run < text_runs.size() && text_runs[run])
		{
			if (auto* text_element = rmlui_dynamic_cast<ElementText*>(text_runs[run]))
			{
				if (text_element->GetText() != text)
					text_element->SetText(text);
			}
		}
		run++;
		text.clear();
	};

	for (const ContentSegment& segment : content->segments)
	{
		if (segment.type == ContentSegment::Type::Url)
		{
			if (in_text_run)
				Flush();
			in_text_run = false;
			run++; // the image
			continue;
		}
		in_text_run = true;
		switch (segment.type)
		{
		case ContentSegment::Type::String: text += segment.text; break;
		case ContentSegment::Type::Attr:
			if (originating)
				text += originating->GetAttribute<String>(segment.text, String());
			break;
		case ContentSegment::Type::Counter:
			text += CounterResolver::FormatCounter(CounterResolver::GetCounterValue(counter_set, segment.text), segment.style);
			break;
		case ContentSegment::Type::Counters:
			text += CounterResolver::FormatCounters(counter_set, segment.text, segment.separator, segment.style);
			break;
		case ContentSegment::Type::OpenQuote:
			text += GetQuote(depth, true);
			depth++;
			break;
		case ContentSegment::Type::CloseQuote:
			depth = Math::Max(depth - 1, 0);
			text += GetQuote(depth, false);
			break;
		case ContentSegment::Type::NoOpenQuote: depth++; break;
		case ContentSegment::Type::NoCloseQuote: depth = Math::Max(depth - 1, 0); break;
		case ContentSegment::Type::Url: break;
		}
	}
	if (in_text_run)
		Flush();
}

// Whether a ::before or ::after rule set generates the pseudo-element: 'content' other than 'normal' and 'none'.
static bool GeneratesContent(const ElementDefinition& definition, const Element& originating)
{
	const Property* property = definition.GetProperty(PropertyId::Content);
	if (!property)
		return false;
	switch (property->unit)
	{
	case Unit::STYLE_VALUE:
	case Unit::VAR_EXPRESSION: return true;
	case Unit::CSS_WIDE_KEYWORD:
	{
		// 'inherit' takes the originating element's content; the others give 'normal'.
		if (CssWideKeyword(property->Get<int>()) != CssWideKeyword::Inherit)
			return false;
		const Property* inherited = const_cast<Element&>(originating).GetProperty(PropertyId::Content);
		return inherited && inherited->unit == Unit::STYLE_VALUE;
	}
	default: break;
	}
	return false;
}

template <typename T>
static ElementPtr InstancePseudoElement(Element& originating, PseudoElementType type)
{
	// (the instancer outlives every element it makes, as the factory's do)
	static ElementInstancerGeneric<T> instancer;
	ElementPtr element = instancer.InstanceElement(&originating, PseudoElementTag(type), XMLAttributes());
	if (element)
	{
		element->SetInstancer(&instancer);
		PluginRegistry::NotifyElementCreate(element.get());
	}
	return element;
}

void PseudoElements::Update(Element& element)
{
	if (!ElementCascade::TakePseudoDirty(element))
		return;
	// Text and pseudo-elements generate none.
	if (element.GetPseudoElementType() != PseudoElementType::None || element.GetTagName() == "#text")
		return;

	const StyleSheet* style_sheet = element.GetStyleSheet();
	bool changed = false;

	for (PseudoElementType type : {PseudoElementType::Before, PseudoElementType::After, PseudoElementType::Marker})
	{
		Element*& slot = ElementCascade::PseudoElementSlot(element, type);

		SharedPtr<const ElementDefinition> definition;
		if (style_sheet && style_sheet->HasPseudoElementRules(type))
			definition = style_sheet->GetElementDefinition(&element, type);

		bool generate = false;
		if (type == PseudoElementType::Marker)
			generate = ListMarker::IsGenerated(element, definition.get());
		else
			generate = (definition && GeneratesContent(*definition, element));

		if (!generate)
		{
			if (slot)
			{
				element.RemoveChild(slot);
				slot = nullptr;
				changed = true;
			}
			continue;
		}

		if (!slot)
		{
			ElementPtr pseudo_element = (type == PseudoElementType::Marker ? InstancePseudoElement<ElementListMarker>(element, type)
																		  : InstancePseudoElement<ElementPseudo>(element, type));
			if (!pseudo_element)
				continue;
			slot = element.AppendChild(std::move(pseudo_element), false);
			changed = true;
		}
		// Its rules match through the originating element: match them again.
		ElementCascade::DirtyDefinition(slot, false);
	}

	if (changed)
	{
		ElementCascade::DirtyLayout(element);
		DirtyCounters(element);
	}
}

void PseudoElements::OnAttributeChange(Element& element, const ElementAttributes& changed_attributes)
{
	for (PseudoElementType type : {PseudoElementType::Before, PseudoElementType::After, PseudoElementType::Marker})
	{
		if (Element* pseudo_element = element.GetPseudoElement(type))
			static_cast<ElementPseudo*>(pseudo_element)->OnOriginatingAttributeChange(changed_attributes);
	}
	// <ol start>, <ol reversed>, <li value> number the list items.
	if (changed_attributes.count("start") || changed_attributes.count("reversed") || changed_attributes.count("value"))
		DirtyCounters(element);
}

Element* PseudoElements::GetOriginatingElement(Element* element)
{
	for (Element* ancestor = element; ancestor; ancestor = ancestor->GetParentNode())
	{
		if (ancestor->GetPseudoElementType() != PseudoElementType::None)
			return ancestor->GetParentNode();
	}
	return element;
}

void PseudoElements::DirtyCounters(Element& element)
{
	if (ElementDocument* document = element.GetOwnerDocument())
		ElementCascade::DirtyCounters(*document);
}

} // namespace Rml
