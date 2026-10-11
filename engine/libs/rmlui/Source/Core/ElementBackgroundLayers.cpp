#include "ElementBackgroundLayers.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Context.h"
#include "../../Include/RmlUi/Core/Decorator.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/ElementUtilities.h"
#include "../../Include/RmlUi/Core/ImageSource.h"
#include "../../Include/RmlUi/Core/Math.h"
#include "../../Include/RmlUi/Core/Mesh.h"
#include "../../Include/RmlUi/Core/MeshUtilities.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/RenderBox.h"
#include "../../Include/RmlUi/Core/RenderManager.h"
#include "../../Include/RmlUi/Core/StyleSheet.h"
#include "ElementStyle.h"
#include "GeometryBackgroundBorder.h"
#include <cmath>

namespace Rml {

static ImageSourceHandler* image_source_handler = nullptr;

ImageSourceHandler::~ImageSourceHandler() {}

void SetImageSourceHandler(ImageSourceHandler* handler)
{
	image_source_handler = handler;
}

ImageSourceHandler* GetImageSourceHandler()
{
	return image_source_handler;
}

// ---- Tiles --------------------------------------------------------------------------------------------------------------

namespace {
	// The starts of the tiles along an axis that meet [clip_begin, clip_end).
	Vector<float> TileStarts(float area_begin, float area_length, float tile, float position, LayerRepeat repeat, float clip_begin,
		float clip_end)
	{
		Vector<float> starts;
		if (!(tile > 0.f) || !(clip_end > clip_begin))
			return starts;

		float period = tile;
		float origin = area_begin + position;
		bool repeating = (repeat == LayerRepeat::Repeat || repeat == LayerRepeat::Round);
		if (repeat == LayerRepeat::Space)
		{
			const float count = std::floor(area_length / tile + 1e-4f);
			if (count >= 2.f)
			{
				// The first and the last tile touch the area's edges; the position is not used.
				period = tile + (area_length - count * tile) / (count - 1.f);
				origin = area_begin;
				repeating = true;
			}
		}

		if (!repeating)
		{
			if (origin < clip_end && origin + tile > clip_begin)
				starts.push_back(origin);
			return starts;
		}

		// Bound the number of tiles (a tile of a fraction of a pixel over a large box).
		const float count = (clip_end - clip_begin) / period;
		if (count > 4096.f)
			return starts;
		const float first = origin - std::ceil((origin - clip_begin) / period - 1e-4f) * period;
		for (float start = first; start < clip_end - 1e-4f; start += period)
		{
			if (start + tile > clip_begin + 1e-4f)
				starts.push_back(start);
		}
		return starts;
	}
} // namespace

LayerTiles ComputeLayerTiles(const LayerTileInput& in, const Function<float(int axis, float free_space)>& resolve_position)
{
	LayerTiles out;
	const Rectanglef& area = in.positioning_area;
	const Vector2f area_size = area.Size();
	const Vector2f intrinsic = in.intrinsic_size;
	const bool has_ratio = (intrinsic.x > 0.f && intrinsic.y > 0.f);

	// background-size (3.9)
	Vector2f tile;
	const LayerSize& size = in.size;
	if (size.mode == LayerSize::Mode::Cover || size.mode == LayerSize::Mode::Contain)
	{
		if (has_ratio)
		{
			const Vector2f scale = area_size / intrinsic;
			const float s = (size.mode == LayerSize::Mode::Cover ? Math::Max(scale.x, scale.y) : Math::Min(scale.x, scale.y));
			tile = intrinsic * s;
		}
		else
			tile = area_size;
	}
	else if (!size.auto_x && !size.auto_y)
		tile = in.resolved_size;
	else if (!size.auto_x)
	{
		tile.x = in.resolved_size.x;
		tile.y = has_ratio ? tile.x * intrinsic.y / intrinsic.x : (intrinsic.y > 0.f ? intrinsic.y : area_size.y);
	}
	else if (!size.auto_y)
	{
		tile.y = in.resolved_size.y;
		tile.x = has_ratio ? tile.y * intrinsic.x / intrinsic.y : (intrinsic.x > 0.f ? intrinsic.x : area_size.x);
	}
	else
	{
		tile.x = intrinsic.x > 0.f ? intrinsic.x : area_size.x;
		tile.y = intrinsic.y > 0.f ? intrinsic.y : area_size.y;
	}

	// round (3.4): a whole number of tiles; with an auto other dimension the ratio is kept.
	const bool round_x = (in.repeat_x == LayerRepeat::Round && tile.x > 0.f && area_size.x > 0.f);
	const bool round_y = (in.repeat_y == LayerRepeat::Round && tile.y > 0.f && area_size.y > 0.f);
	const Vector2f unrounded = tile;
	if (round_x)
		tile.x = area_size.x / Math::Max(1.f, std::round(area_size.x / tile.x));
	if (round_y)
		tile.y = area_size.y / Math::Max(1.f, std::round(area_size.y / tile.y));
	const bool explicit_mode = (size.mode == LayerSize::Mode::Explicit);
	if (round_x && !round_y && explicit_mode && size.auto_y && unrounded.x > 0.f)
		tile.y = unrounded.y * tile.x / unrounded.x;
	if (round_y && !round_x && explicit_mode && size.auto_x && unrounded.y > 0.f)
		tile.x = unrounded.x * tile.y / unrounded.y;

	if (!(tile.x > 0.f) || !(tile.y > 0.f))
		return out;
	out.tile_size = tile;

	const Rectanglef& clip = in.clip;
	const float position_x = resolve_position(0, area_size.x - tile.x);
	const float position_y = resolve_position(1, area_size.y - tile.y);
	const Vector<float> xs = TileStarts(area.Left(), area_size.x, tile.x, position_x, in.repeat_x, clip.Left(), clip.Right());
	const Vector<float> ys = TileStarts(area.Top(), area_size.y, tile.y, position_y, in.repeat_y, clip.Top(), clip.Bottom());
	if (xs.size() * ys.size() > 65536)
		return out;
	out.tiles.reserve(xs.size() * ys.size());
	for (float y : ys)
	{
		for (float x : xs)
		{
			const Vector2f top_left = Math::Max(Vector2f(x, y), clip.TopLeft());
			const Vector2f bottom_right = Math::Min(Vector2f(x, y) + tile, clip.BottomRight());
			if (bottom_right.x - top_left.x <= 1e-4f || bottom_right.y - top_left.y <= 1e-4f)
				continue;
			LayerTile result;
			result.rect = Rectanglef::FromCorners(top_left, bottom_right);
			result.uv0 = (top_left - Vector2f(x, y)) / tile;
			result.uv1 = (bottom_right - Vector2f(x, y)) / tile;
			out.tiles.push_back(result);
		}
	}
	return out;
}

// ---- Layers -------------------------------------------------------------------------------------------------------------

namespace {
	struct LayerIds {
		PropertyId image, position_x, position_y, size, repeat, origin, clip, attachment;
	};
	const LayerIds& Ids(ElementBackgroundLayers::Kind kind)
	{
		static const LayerIds background = {PropertyId::BackgroundImage, PropertyId::BackgroundPositionX, PropertyId::BackgroundPositionY,
			PropertyId::BackgroundSize, PropertyId::BackgroundRepeat, PropertyId::BackgroundOrigin, PropertyId::BackgroundClip,
			PropertyId::BackgroundAttachment};
		// Masks: origin and clip are the border box, attached to the element (the fork has no mask-origin, mask-clip).
		static const LayerIds mask = {PropertyId::MaskImage, PropertyId::MaskPositionX, PropertyId::MaskPositionY, PropertyId::MaskSize,
			PropertyId::MaskRepeat, PropertyId::Invalid, PropertyId::Invalid, PropertyId::Invalid};
		return kind == ElementBackgroundLayers::Kind::Background ? background : mask;
	}

	template <typename T>
	const T* GetList(Element* element, PropertyId id, const Property** out_property = nullptr)
	{
		if (id == PropertyId::Invalid)
			return nullptr;
		const Property* property = element->GetStyle()->GetLocalPropertyWithResolvedVariables(id);
		if (!property)
			property = element->GetProperty(id);
		if (!property || property->unit != Unit::STYLE_VALUE)
			return nullptr;
		if (out_property)
			*out_property = property;
		const StyleValuePtr& value = property->value.GetReference<StyleValuePtr>();
		return rmlui_dynamic_cast<const T*>(value.get());
	}

	// The rectangle of a box area of the element's box 'index', relative to its border box's origin.
	Rectanglef AreaRect(Element* element, BoxArea area, int index)
	{
		const RenderBox box = element->GetRenderBox(area, index, false);
		return Rectanglef::FromPositionSize(box.GetBorderOffset() + box.GetFillOffset(), box.GetFillSize());
	}

	// The anti-aliased shape of a clip box with rounded corners: its fill area with the inner curves of the border box's
	// radii, the edges feathered as the background colour's.
	Geometry MakeClipMask(RenderManager& render_manager, const RenderBox& clip_box)
	{
		const BorderMetrics metrics = GeometryBackgroundBorder::ComputeBorderMetrics(clip_box.GetBorderOffset(), clip_box.GetBorderWidths(),
			clip_box.GetFillSize(), clip_box.GetBorderRadius());
		CornerSizes2 radii = metrics.inner_radii;
		for (Vector2f& radius : radii)
			radius = Math::Max(radius, Vector2f(0.f));
		const RenderBox shape(clip_box.GetFillSize(), clip_box.GetBorderOffset() + clip_box.GetFillOffset(), EdgeSizes{}, radii);
		const ColourbPremultiplied none(0, 0);
		const ColourbPremultiplied no_border[4] = {none, none, none, none};
		Mesh mesh;
		MeshUtilities::GenerateBackgroundBorder(mesh, shape, ColourbPremultiplied(255), no_border);
		return render_manager.MakeGeometry(std::move(mesh));
	}

	void AddQuad(Mesh& mesh, Vector2f top_left, Vector2f bottom_right, Vector2f uv0, Vector2f uv1, ColourbPremultiplied colour)
	{
		MeshUtilities::GenerateQuad(mesh, top_left, bottom_right - top_left, colour, uv0, uv1);
	}
} // namespace

ElementBackgroundLayers::ElementBackgroundLayers(Kind kind) : kind(kind) {}

ElementBackgroundLayers::~ElementBackgroundLayers()
{
	ReleaseDraws();
}

bool ElementBackgroundLayers::HasLayers(Element* element, Kind kind)
{
	const ImageLayerList* images = GetList<ImageLayerList>(element, Ids(kind).image);
	return images && images->HasImages();
}

BoxArea ElementBackgroundLayers::GetColourClip(Element* element)
{
	const LayerKeywordList* clips = GetList<LayerKeywordList>(element, PropertyId::BackgroundClip);
	if (!clips || clips->GetCount() == 0)
		return BoxArea::Border;
	const ImageLayerList* images = GetList<ImageLayerList>(element, PropertyId::BackgroundImage);
	const size_t num_layers = (images && !images->layers.empty()) ? images->layers.size() : 1;
	return BoxArea(clips->values[(num_layers - 1) % clips->GetCount()]);
}

void ElementBackgroundLayers::Release()
{
	ReleaseDraws();
	dirty = true;
}

void ElementBackgroundLayers::ReleaseDraws()
{
	for (Draw& draw : draws)
	{
		if (draw.decorator && draw.decorator_data)
			draw.decorator->ReleaseElementData(draw.decorator_data);
	}
	draws.clear();
}

void ElementBackgroundLayers::Generate(Element* element, const Rectanglef* canvas, Element* area_element)
{
	ReleaseDraws();
	depends_on_offset = (canvas != nullptr);

	RenderManager* render_manager = element->GetRenderManager();
	const LayerIds& ids = Ids(kind);
	const Property* image_property = nullptr;
	const ImageLayerList* images = GetList<ImageLayerList>(element, ids.image, &image_property);
	if (!render_manager || !images || !images->HasImages())
		return;

	const LayerPositionList* positions_x = GetList<LayerPositionList>(element, ids.position_x);
	const LayerPositionList* positions_y = GetList<LayerPositionList>(element, ids.position_y);
	const LayerSizeList* sizes = GetList<LayerSizeList>(element, ids.size);
	const LayerKeywordList* repeats = GetList<LayerKeywordList>(element, ids.repeat);
	const LayerKeywordList* origins = GetList<LayerKeywordList>(element, ids.origin);
	const LayerKeywordList* clips = GetList<LayerKeywordList>(element, ids.clip);
	const LayerKeywordList* attachments = GetList<LayerKeywordList>(element, ids.attachment);

	// The path url()s and decorator textures are relative to.
	String base_path;
	if (image_property && image_property->source)
		base_path = image_property->source->path;
	else if (ElementDocument* document = element->GetOwnerDocument())
		base_path = document->GetSourceURL();

	// The decorator functions, instanced by the style sheet (cached there).
	DecoratorPtrList decorators;
	if (images->decorators && !images->decorators->list.empty())
	{
		if (const StyleSheet* style_sheet = element->GetStyleSheet())
		{
			PropertySource source(base_path, 0, "");
			decorators = style_sheet->InstanceDecorators(*render_manager, *images->decorators, &source);
		}
	}

	const float opacity = element->GetDrawOpacity();
	const ColourbPremultiplied white = ColourbPremultiplied(255).ToNonPremultiplied().ToPremultiplied(opacity);
	Element* box_element = (area_element ? area_element : element);
	// Rectangles of the area element in the coordinates of the element.
	const Vector2f area_shift = box_element->GetAbsoluteOffset(BoxArea::Border) - element->GetAbsoluteOffset(BoxArea::Border);
	const Vector2f element_offset = element->GetAbsoluteOffset(BoxArea::Border);

	const int num_boxes = canvas ? 1 : element->GetNumBoxes();
	for (int i = (int)images->layers.size() - 1; i >= 0; i--)
	{
		const ImageLayer& layer = images->layers[i];
		if (layer.kind == ImageLayer::Kind::None)
			continue;

		auto item = [i](const void* /*list*/, size_t count) -> size_t { return count ? size_t(i) % count : 0; };
		const LengthPercentageValue position_x =
			positions_x && !positions_x->values.empty() ? positions_x->values[item(positions_x, positions_x->values.size())] : LengthPercentageValue(0.f, Unit::PERCENT);
		const LengthPercentageValue position_y =
			positions_y && !positions_y->values.empty() ? positions_y->values[item(positions_y, positions_y->values.size())] : LengthPercentageValue(0.f, Unit::PERCENT);
		const LayerSize size = sizes && !sizes->values.empty() ? sizes->values[item(sizes, sizes->values.size())] : LayerSize();
		LayerRepeat repeat_x = LayerRepeat::Repeat, repeat_y = LayerRepeat::Repeat;
		if (repeats && repeats->GetCount() > 0)
		{
			const size_t r = item(repeats, repeats->GetCount());
			repeat_x = LayerRepeat(repeats->values[2 * r]);
			repeat_y = LayerRepeat(repeats->values[2 * r + 1]);
		}
		BoxArea origin_area = origins && origins->GetCount() ? BoxArea(origins->values[item(origins, origins->GetCount())])
																	 : (kind == Kind::Background ? BoxArea::Padding : BoxArea::Border);
		BoxArea clip_area = clips && clips->GetCount() ? BoxArea(clips->values[item(clips, clips->GetCount())]) : BoxArea::Border;
		const LayerAttachment attachment =
			attachments && attachments->GetCount() ? LayerAttachment(attachments->values[item(attachments, attachments->GetCount())]) : LayerAttachment::Scroll;

		const Decorator* decorator = nullptr;
		if (layer.kind == ImageLayer::Kind::Decorator && layer.decorator_index >= 0 && layer.decorator_index < (int)decorators.size())
			decorator = decorators[layer.decorator_index].get();
		if (layer.kind == ImageLayer::Kind::Decorator && !decorator)
			continue;
		// RCSS: a decorator function followed by its paint area paints in that box.
		if (decorator && images->decorators->list[layer.decorator_index].paint_area != BoxArea::Auto)
			origin_area = clip_area = images->decorators->list[layer.decorator_index].paint_area;

		// A decorator function that is not an image of a size (not a gradient) paints over the clip box.
		CompiledShader probe_shader;
		if (decorator)
		{
			probe_shader = decorator->CompileImageShader(element, Vector2f(1.f));
			if (!probe_shader)
			{
				Draw draw;
				draw.decorator = decorators[layer.decorator_index];
				draw.decorator_data = draw.decorator->GenerateElementData(element, clip_area);
				if (!draw.decorator_data)
					continue;
				const RenderBox clip_box = element->GetRenderBox(clip_area, 0, false);
				if (clip_box.HasBorderRadius() && !canvas)
					draw.clip_mask = MakeClipMask(*render_manager, clip_box);
				draws.push_back(std::move(draw));
				continue;
			}
			probe_shader.Release();
		}

		// The image's intrinsic size.
		Vector2f intrinsic;
		ImageSourceHandler::Image handled;
		bool handled_by_application = false;
		Texture raster;
		if (layer.kind == ImageLayer::Kind::Url)
		{
			if (image_source_handler && image_source_handler->GetImage(element, layer.url, base_path, Vector2f(0.f), handled))
			{
				handled_by_application = true;
				intrinsic = handled.intrinsic_size;
			}
			else
			{
				raster = render_manager->LoadTexture(layer.url, base_path);
				intrinsic = Vector2f(raster.GetDimensions());
			}
			if (intrinsic.x <= 0.f && intrinsic.y <= 0.f && !handled_by_application)
				continue;
		}

		for (int box_index = 0; box_index < num_boxes; box_index++)
		{
			Rectanglef positioning_area = AreaRect(box_element, origin_area, box_index);
			positioning_area = Rectanglef::FromPositionSize(positioning_area.TopLeft() + area_shift, positioning_area.Size());
			Rectanglef clip_rect = canvas ? *canvas : AreaRect(element, clip_area, box_index);
			if (attachment == LayerAttachment::Fixed)
			{
				// The viewport: the context's rectangle.
				const Vector2f viewport = element->GetContext() ? Vector2f(element->GetContext()->GetDimensions()) : Vector2f(0.f);
				positioning_area = Rectanglef::FromPositionSize(-element_offset, viewport);
				depends_on_offset = true;
			}
			else if (attachment == LayerAttachment::Local && element->GetScrollWidth() > element->GetClientWidth() + 0.5f)
			{
				const Rectanglef padding = AreaRect(element, BoxArea::Padding, box_index);
				const Vector2f scroll(element->GetScrollLeft(), element->GetScrollTop());
				positioning_area = Rectanglef::FromPositionSize(padding.TopLeft() - scroll,
					Math::Max(padding.Size(), Vector2f(element->GetScrollWidth(), element->GetScrollHeight())));
				depends_on_offset = true;
			}

			LayerTileInput input;
			input.positioning_area = positioning_area;
			input.clip = clip_rect;
			input.intrinsic_size = intrinsic;
			input.size = size;
			if (!size.auto_x && size.mode == LayerSize::Mode::Explicit)
				input.resolved_size.x = Math::Max(ResolveLengthPercentage(size.x, *element, positioning_area.Width()), 0.f);
			if (!size.auto_y && size.mode == LayerSize::Mode::Explicit)
				input.resolved_size.y = Math::Max(ResolveLengthPercentage(size.y, *element, positioning_area.Height()), 0.f);
			input.repeat_x = repeat_x;
			input.repeat_y = repeat_y;
			const LayerTiles tiles = ComputeLayerTiles(input, [&](int axis, float free_space) {
				return ResolveLengthPercentage(axis == 0 ? position_x : position_y, *element, free_space);
			});
			if (tiles.tiles.empty())
				continue;

			Draw draw;
			Mesh mesh;
			if (decorator)
			{
				// A gradient: one shader for the tile size, the texture coordinates of each tile its own positions.
				draw.shader = decorator->CompileImageShader(element, tiles.tile_size);
				if (!draw.shader)
					continue;
				const byte alpha = byte(opacity * 255.f);
				for (const LayerTile& tile : tiles.tiles)
					AddQuad(mesh, tile.rect.TopLeft(), tile.rect.BottomRight(), tile.uv0 * tiles.tile_size, tile.uv1 * tiles.tile_size,
						ColourbPremultiplied(alpha, alpha));
			}
			else
			{
				ColourbPremultiplied colour = white;
				if (handled_by_application)
				{
					ImageSourceHandler::Image image;
					if (!image_source_handler->GetImage(element, layer.url, base_path, tiles.tile_size, image) || !image.texture)
						continue;
					draw.texture = image.texture;
					draw.image_owner = image.owner;
					colour = image.colour;
				}
				else
					draw.texture = raster;
				for (const LayerTile& tile : tiles.tiles)
					AddQuad(mesh, tile.rect.TopLeft(), tile.rect.BottomRight(), tile.uv0, tile.uv1, colour);
			}
			draw.geometry = render_manager->MakeGeometry(std::move(mesh));

			const RenderBox clip_box = element->GetRenderBox(clip_area, box_index, false);
			if (clip_box.HasBorderRadius() && !canvas)
			{
				draw.clip_mask = MakeClipMask(*render_manager, clip_box);
			}
			draws.push_back(std::move(draw));
		}
	}

	generated_offset = element_offset;
	generated_scroll = Vector2f(element->GetScrollLeft(), element->GetScrollTop());
	generated_canvas = canvas ? canvas->Size() : Vector2f(0.f);
}

void ElementBackgroundLayers::Render(Element* element, const Rectanglef* canvas, Element* area_element)
{
	if (!dirty && depends_on_offset)
	{
		const Vector2f offset = element->GetAbsoluteOffset(BoxArea::Border);
		const Vector2f scroll(element->GetScrollLeft(), element->GetScrollTop());
		if (offset != generated_offset || scroll != generated_scroll || (canvas ? canvas->Size() : Vector2f(0.f)) != generated_canvas)
			dirty = true;
	}
	if (dirty)
	{
		Generate(element, canvas, area_element);
		dirty = false;
	}
	if (draws.empty())
		return;

	RenderManager* render_manager = element->GetRenderManager();
	if (!render_manager)
		return;
	const Vector2f offset = element->GetAbsoluteOffset(BoxArea::Border);

	for (Draw& draw : draws)
	{
		const bool masked = bool(draw.clip_mask);
		if (masked)
			render_manager->PushLayer();

		if (draw.decorator)
			draw.decorator->RenderElement(element, draw.decorator_data);
		else if (draw.shader)
			draw.geometry.Render(offset, {}, draw.shader);
		else
			draw.geometry.Render(offset, draw.texture);

		if (masked)
		{
			// The layer, masked by the anti-aliased shape of its rounded clip box.
			render_manager->PushLayer();
			draw.clip_mask.Render(offset);
			CompiledFilter mask = render_manager->SaveLayerAsMaskImage();
			render_manager->PopLayer();
			FilterHandleList filters;
			mask.AddHandleTo(filters);
			render_manager->CompositeLayers(render_manager->GetTopLayer(), render_manager->GetNextLayer(), BlendMode::Blend, filters);
			render_manager->PopLayer();
		}
	}
}

} // namespace Rml
