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

namespace Rml {

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
	if (HasAttribute("src"))
	{
		if (fitted_geometry)
			fitted_geometry.Render(GetAbsoluteOffset(BoxArea::Content), handle->texture);
	}
	else
	{
		handle->geometry.Render(GetAbsoluteOffset(BoxArea::Content), handle->texture);
	}
}

void ElementSVG::GenerateFittedGeometry(const Function<SharedPtr<SVG::SVGData>(const Vector2i*)>& get_handle)
{
	fitted_geometry = Geometry();
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

	Mesh mesh;
	MeshUtilities::GenerateQuad(mesh, fit.rect.TopLeft(), fit.rect.Size(), fitted_colour, fit.uv.TopLeft(), fit.uv.BottomRight());
	fitted_geometry = render_manager->MakeGeometry(std::move(mesh));
}

void ElementSVG::OnResize()
{
	svg_dirty = true;
}

void ElementSVG::OnAttributeChange(const ElementAttributes& changed_attributes)
{
	Element::OnAttributeChange(changed_attributes);

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

	content += svg_data;
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

	svg_data = "" + content;
	svg_dirty = true;
	EnsureSourceLoaded();
}

void ElementSVG::EnsureSourceLoaded()
{
	if (!svg_dirty)
		return;

	svg_dirty = false;

	const bool crop_to_content = HasAttribute("crop-to-content");
	const auto source = GetAttribute<String>("src", "");
	fitted_geometry = Geometry();
	if (source.empty())
	{
		const auto source_id = GetAttribute<String>("rmlui-svgdata-id", "svgdata:undefined");

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
