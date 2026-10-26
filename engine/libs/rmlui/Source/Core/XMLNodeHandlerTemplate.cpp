#include "XMLNodeHandlerTemplate.h"
#include "../../Include/RmlUi/Core/Dictionary.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/Factory.h"
#include "../../Include/RmlUi/Core/XMLParser.h"
#include "Template.h"
#include "TemplateCache.h"
#include "XMLParseTools.h"

namespace Rml {

XMLNodeHandlerTemplate::XMLNodeHandlerTemplate() {}

XMLNodeHandlerTemplate::~XMLNodeHandlerTemplate() {}

Element* XMLNodeHandlerTemplate::ElementStart(XMLParser* parser, const String& name, const XMLAttributes& attributes)
{
	RMLUI_ASSERT(name == "template");
	(void)name;

	// Tell the parser to use the default handler for all child nodes
	parser->PushDefaultHandler();

	const String template_name = Get<String>(attributes, "src", "");
	Element* element = parser->GetParseFrame()->element;

	if (template_name.empty())
	{
		// Fork (P7-T): an inert element; the parser hands its content over as InnerXML data (ElementData).
		ElementPtr template_element = Factory::InstanceElement(element, name, name, attributes);
		if (!template_element)
			return element;
		return element->AppendChild(std::move(template_element));
	}

	return XMLParseTools::ParseTemplate(element, template_name);
}

bool XMLNodeHandlerTemplate::ElementEnd(XMLParser* /*parser*/, const String& /*name*/)
{
	return true;
}

bool XMLNodeHandlerTemplate::ElementData(XMLParser* parser, const String& data, XMLDataType type)
{
	// Fork (P7-T): the content of a <template> without 'src', kept as RML for its owner.
	if (type == XMLDataType::InnerXML)
	{
		parser->GetParseFrame()->element->SetAttribute("rmlui-inner-rml", data);
		return true;
	}
	return Factory::InstanceElementText(parser->GetParseFrame()->element, data);
}

} // namespace Rml
