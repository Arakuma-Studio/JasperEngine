#include "../../Include/RmlUi/SVG/ElementSVG.h"
#include "../../Include/RmlUi/Core/Core.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/ElementUtilities.h"
#include "../../Include/RmlUi/Core/FileInterface.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Geometry.h"
#include "../../Include/RmlUi/Core/MeshUtilities.h"
#include "../../Include/RmlUi/Core/ObjectFit.h"
#include "../../Include/RmlUi/Core/RenderManager.h"
#include "../../Include/RmlUi/Core/PropertyIdSet.h"
#include "../../Include/RmlUi/Core/SystemInterface.h"
#include "SVGCache.h"
#include "../../Include/RmlUi/Core/BaseXMLParser.h"
#include "../../Include/RmlUi/Core/ElementInstancer.h"
#include "../../Include/RmlUi/Core/ElementText.h"
#include "../../Include/RmlUi/Core/Factory.h"
#include "../../Include/RmlUi/Core/StreamMemory.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "../../Include/RmlUi/Core/Transform.h"
#include "../Core/TransformUtilities.h"

namespace Rml {

namespace {
	void DirtyInlineSVG(Element* element)
	{
		for (; element; element = element->GetParentNode())
			if (auto* svg = rmlui_dynamic_cast<ElementSVG*>(element))
			{
				svg->DirtySource();
				break;
			}
	}

	class SVGContentElement : public Element {
	public:
		explicit SVGContentElement(const String& tag) : Element(StringUtilities::ToLower(tag)), svg_tag(tag) {}
	protected:
		void GetRML(String& output) override
		{
			output += "<" + svg_tag;
			String style = GetAttribute<String>("style", "");
			for (const auto& attribute : GetAttributes())
			{
				if (attribute.first != "style")
					output += " " + attribute.first + "=\"" + StringUtilities::EncodeRml(attribute.second.Get<String>()) + "\"";
			}
			for (const auto& property : GetLocalStyleProperties())
			{
				String value = property.second.ToString();
				if (property.first == PropertyId::Transform && property.second.unit == Unit::TRANSFORM)
				{
					const TransformPtr transform = property.second.Get<TransformPtr>();
					Matrix4f matrix = Matrix4f::Identity();
					if (transform)
						for (int i = 0; i < transform->GetNumPrimitives(); ++i)
						{
#ifdef RMLUI_MATH_EXPRESSIONS
							const TransformPrimitive primitive = transform->ResolvePrimitive(i, *this);
#else
							const TransformPrimitive& primitive = transform->GetPrimitive(i);
#endif
							matrix *= TransformUtilities::ResolveTransform(primitive, *this);
						}
					const Matrix4f& resolved = matrix;
					const auto x = resolved.GetRow(0), y = resolved.GetRow(1);
					value = CreateString("matrix(%.9g %.9g %.9g %.9g %.9g %.9g)", x[0], y[0], x[1], y[1], x[3], y[3]);
				}
				style += ";" + StyleSheetSpecification::GetPropertyName(property.first) + ":" + value;
			}
			if (!style.empty()) output += " style=\"" + StringUtilities::EncodeRml(style) + "\"";
			if (GetNumChildren() > 0)
			{
				output += ">";
				Element::GetInnerRML(output);
				output += "</" + svg_tag + ">";
			}
			else output += "/>";
		}
		void OnAttributeChange(const ElementAttributes& attributes) override
		{
			Element::OnAttributeChange(attributes);
			DirtyInlineSVG(GetParentNode());
		}
		void OnPropertyChange(const PropertyIdSet& properties) override
		{
			Element::OnPropertyChange(properties);
			DirtyInlineSVG(GetParentNode());
		}
		void OnChildAdd(Element* child) override
		{
			Element::OnChildAdd(child);
			DirtyInlineSVG(GetParentNode());
		}
		void OnChildRemove(Element* child) override
		{
			Element::OnChildRemove(child);
			DirtyInlineSVG(GetParentNode());
		}
	private:
		String svg_tag;
	};

	ElementInstancer* SVGContentInstancer()
	{
		static ElementInstancerGeneric<SVGContentElement> instancer;
		return &instancer;
	}

	// SVG remains an inert replaced image, but its markup has editable DOM descendants. This
	// parser deliberately bypasses the HTML node handlers (scripts, links and data bindings).
	class SVGContentParser : public BaseXMLParser {
	public:
		explicit SVGContentParser(Element* root) : root(root)
		{
			RegisterCDATATag("style");
			RegisterCDATATag("script");
		}
		void HandleElementStart(const String& name, const XMLAttributes& attributes) override
		{
			if (depth++ == 0) return;
			Element* parent = stack.empty() ? root : stack.back();
			ElementInstancer* instancer = SVGContentInstancer();
			ElementPtr child = instancer->InstanceElement(parent, name, attributes);
			child->SetInstancer(instancer);
			child->SetAttributes(attributes);
			stack.push_back(parent->AppendChild(std::move(child)));
		}
		void HandleElementEnd(const String&) override
		{
			if (--depth > 0 && !stack.empty()) stack.pop_back();
		}
		void HandleData(const String& data, XMLDataType type) override
		{
			if (stack.empty()) return;
			ElementPtr text = Factory::InstanceElement(stack.back(), "#text", "#text", XMLAttributes());
			if (auto* element = rmlui_dynamic_cast<ElementText*>(text.get()))
				element->SetText(type == XMLDataType::CDATA ? data : StringUtilities::DecodeRml(data));
			stack.back()->AppendChild(std::move(text));
		}
	private:
		Element* root;
		Vector<Element*> stack;
		int depth = 0;
	};
} // namespace

namespace SVG {
	static SourceHook* source_hook = nullptr;

	SourceHook::~SourceHook() {}

	void SetSourceHook(SourceHook* hook)
	{
		source_hook = hook;
	}

	SourceHook* GetSourceHook()
	{
		return source_hook;
	}
} // namespace SVG

RMLUI_RTTI_Define(ElementSVG)

unsigned long ElementSVG::internal_id_counter = 0;

ElementSVG::ElementSVG(const String& tag) : Element(tag) {}
ElementSVG::~ElementSVG()
{
	handle.reset();
}

bool ElementSVG::GetIntrinsicDimensions(Vector2f& dimensions, float& ratio)
{
	EnsureSourceLoaded();

	dimensions = handle ? handle->intrinsic_dimensions : Vector2f(0);

	if (HasAttribute("width"))
	{
		dimensions.x = GetAttribute<float>("width", 0);
	}
	if (HasAttribute("height"))
	{
		dimensions.y = GetAttribute<float>("height", 0);
	}

	if (dimensions.y > 0)
		ratio = dimensions.x / dimensions.y;

	dimensions *= ElementUtilities::GetDensityIndependentPixelRatio(this);

	return true;
}

void ElementSVG::OnRender()
{
	EnsureSourceLoaded();
	if (!handle)
		return;
	// Inline: the rasterised content box (the quad of handle->geometry, at the content box).
	if (!HasAttribute("src"))
		picture.Set(Rectanglef::FromPositionSize(GetBox().GetPosition(BoxArea::Content), Vector2f(handle->GetDimensions())), handle->GetColour());
	picture.Render(this, handle->texture);
}

void ElementSVG::GenerateFittedGeometry(const Function<SharedPtr<SVG::SVGData>(const Vector2i*)>& get_handle)
{
	picture.Clear();
	RenderManager* render_manager = GetRenderManager();
	if (!render_manager)
		return;

	// First at the size of the content box: the image's intrinsic size.
	handle = get_handle(nullptr);
	if (!handle)
		return;

	// The image's size as layout sees it (intrinsic size, width/height attributes, dp ratio).
	Vector2f natural = handle->intrinsic_dimensions;
	if (HasAttribute("width"))
		natural.x = GetAttribute<float>("width", 0);
	if (HasAttribute("height"))
		natural.y = GetAttribute<float>("height", 0);
	natural *= ElementUtilities::GetDensityIndependentPixelRatio(this);

	const ComputedValues& computed = GetComputedValues();
	const Vector2f box = GetBox().GetSize(BoxArea::Content).Round();
	ObjectFitRect fit;
	// Nothing visible (no box yet, during the layout that asks for the intrinsic size): the handle stays for that.
	if (!ComputeObjectFitRect(box, natural, computed.object_fit(), computed.object_position_x(), computed.object_position_y(), fit))
		return;

	// Rasterised at the size the whole image is drawn at; the visible part of it is drawn.
	const Vector2i dimensions(Math::Max(fit.size.Round(), Vector2f(1.f)));
	if (dimensions != Vector2i(box))
	{
		SharedPtr<SVG::SVGData> fitted = get_handle(&dimensions);
		handle = std::move(fitted);
	}
	if (!handle)
		return;

	picture.Set(Rectanglef::FromPositionSize(GetBox().GetPosition(BoxArea::Content) + fit.rect.TopLeft(), fit.rect.Size()), fitted_colour,
		fit.uv.TopLeft(), fit.uv.BottomRight());
}

void ElementSVG::OnResize()
{
	svg_dirty = true;
}

void ElementSVG::OnChildAdd(Element* child)
{
	Element::OnChildAdd(child);
	DirtySource();
}

void ElementSVG::OnChildRemove(Element* child)
{
	Element::OnChildRemove(child);
	DirtySource();
}

void ElementSVG::OnAttributeChange(const ElementAttributes& changed_attributes)
{
	Element::OnAttributeChange(changed_attributes);
	DirtySource();

	if (changed_attributes.count("src") || changed_attributes.count("crop-to-content"))
	{
		svg_dirty = true;
		DirtyLayout();
	}

	if (changed_attributes.find("width") != changed_attributes.end() || changed_attributes.find("height") != changed_attributes.end())
	{
		DirtyLayout();
	}
}

void ElementSVG::OnPropertyChange(const PropertyIdSet& changed_properties)
{
	Element::OnPropertyChange(changed_properties);

	if (changed_properties.Contains(PropertyId::ImageColor) || changed_properties.Contains(PropertyId::Opacity) ||
		changed_properties.Contains(PropertyId::ObjectFit) || changed_properties.Contains(PropertyId::ObjectPositionX) ||
		changed_properties.Contains(PropertyId::ObjectPositionY))
	{
		svg_dirty = true;
	}

	if (SVG::SourceHook* hook = SVG::GetSourceHook())
	{
		if (hook->IsSourceProperty(changed_properties))
			svg_dirty = true;
	}
}

void ElementSVG::GetInnerRML(String& content) const
{
	// If the SVG is from a file source don't add anything to the content string.
	const auto source = GetAttribute<String>("src", "");
	if (!source.empty())
		return;

	Element::GetInnerRML(content);
}

void ElementSVG::SetInnerRML(const String& content)
{
	// If the SVG is from a file source don't set the svg xml data on the element.
	const auto source = GetAttribute<String>("src", "");
	if (!source.empty())
		return;

	// We use CreateString instead of std::to_string to avoid having to create an extra std::string and convert it to Rml::String in case clients use
	// a non-std string.
	if (!HasAttribute("rmlui-svgdata-id"))
		SetAttribute("rmlui-svgdata-id", "svgdata:" + CreateString("%lu", internal_id_counter++));

	while (GetNumChildren() > 0)
		RemoveChild(GetFirstChild());
	const String wrapped = "<svg-content>" + content + "</svg-content>";
	StreamMemory stream(reinterpret_cast<const byte*>(wrapped.data()), wrapped.size());
	SVGContentParser parser(this);
	parser.Parse(&stream);
	DirtySource();
	EnsureSourceLoaded();
}

void ElementSVG::DirtySource()
{
	svg_dirty = true;
	source_revision++;
}

void ElementSVG::EnsureSourceLoaded()
{
	if (!svg_dirty)
		return;

	svg_dirty = false;

	const bool crop_to_content = HasAttribute("crop-to-content");
	const auto source = GetAttribute<String>("src", "");
	picture.Clear();
	if (source.empty())
	{
		const auto source_id = GetAttribute<String>("rmlui-svgdata-id", "svgdata:undefined") + ":" + CreateString("%lu", source_revision);
		svg_data.clear();
		Element::GetInnerRML(svg_data);

		// Build an svg wrapper tag, copying all but src attribute (expected attributes could be width, height, viewBox, etc.)
		String svg_element_source = "<svg ";
		ElementAttributes attrs = GetAttributes();
		for (auto& attr : attrs)
		{
			if (attr.first == "src")
				continue;
			svg_element_source.append(attr.first);
			svg_element_source.append("=\"");
			svg_element_source.append(StringUtilities::EncodeRml(attr.second.Get<String>()));
			svg_element_source.append("\" ");
		}
		svg_element_source.append(">");
		svg_element_source.append(svg_data);
		svg_element_source.append("</svg>");

		SVG::SourceHook::Result hooked;
		SVG::SourceHook* hook = SVG::GetSourceHook();
		if (hook && hook->ProcessSource(this, String(), svg_element_source, hooked))
		{
			// the new handle before the old one goes: a colour change keeps the rasterised texture
			handle = SVG::SVGCache::GetHandle(hooked.source_id, hooked.data, SVG::SVGCache::Data, this, crop_to_content, BoxArea::Content,
				hooked.use_colour ? &hooked.colour : nullptr);
			return;
		}

		if (handle)
			handle.reset(); // The old handle won't be re-used so clear it.
		handle = SVG::SVGCache::GetHandle(source_id, svg_element_source, SVG::SVGCache::Data, this, crop_to_content, BoxArea::Content);
	}
	else
	{
		// An SVG file: an image, placed by object-fit and object-position.
		fitted_colour = SVG::SVGCache::GetColour(this);
		if (SVG::SourceHook* hook = SVG::GetSourceHook())
		{
			// The file, resolved against the document as the cache resolves it
			String path;
			if (ElementDocument* document = GetOwnerDocument())
			{
				const String document_source_url = StringUtilities::Replace(document->GetSourceURL(), '|', ':');
				GetSystemInterface()->JoinPath(path, document_source_url, source);
			}

			String file_data;
			SVG::SourceHook::Result hooked;
			if (!path.empty() && GetFileInterface()->LoadFile(path, file_data) && hook->ProcessSource(this, path, file_data, hooked))
			{
				// the new handle before the old one goes: a colour change keeps the rasterised texture
				if (hooked.use_colour)
					fitted_colour = hooked.colour;
				GenerateFittedGeometry([&](const Vector2i* dimensions) {
					return SVG::SVGCache::GetHandle(hooked.source_id, hooked.data, SVG::SVGCache::Data, this, crop_to_content, BoxArea::Content,
						&fitted_colour, dimensions);
				});
				return;
			}
		}

		GenerateFittedGeometry([&](const Vector2i* dimensions) {
			return SVG::SVGCache::GetHandle(source, source, SVG::SVGCache::File, this, crop_to_content, BoxArea::Content, &fitted_colour,
				dimensions);
		});
	}
}
} // namespace Rml
