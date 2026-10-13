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

	// The spans of the tiles along an axis, cropped to the clip: their extent and the part of the tile they show (0 to 1;
	// beyond 1 over tiles drawn as one span). As Chromium's BackgroundImageGeometry and DrawTiledBackground: a tile that is
	// not repeated along the axis is drawn into a destination snapped to whole pixels, the image mapped from its
	// unsnapped place (a tile after the area's start: the image's start snapped and its end one tile further, snapped);
	// repeated tiles keep their fractional size and phase.
	struct Span {
		float begin, end, uv0, uv1;
	};
	auto spans = [](const Vector<float>& starts, float size, LayerRepeat repeat, bool wrap, bool& wrapped, float area_begin, float clip_begin,
					 float clip_end) {
		Vector<Span> result;
		const bool repeating = (repeat == LayerRepeat::Repeat || repeat == LayerRepeat::Round);
		auto add = [&](float origin, float begin, float end) {
			const float b = Math::Max(begin, clip_begin), e = Math::Min(end, clip_end);
			if (e - b > 1e-4f)
				result.push_back({b, e, (b - origin) / size, (e - origin) / size});
		};
		wrapped = false;
		if (wrap && repeating && starts.size() > 1)
		{
			// One span over the tiles, sampled with repeat wrap.
			wrapped = true;
			add(starts.front(), starts.front(), starts.back() + size);
			return result;
		}
		const bool single = (repeat == LayerRepeat::NoRepeat || starts.size() == 1) && !repeating;
		for (float start : starts)
		{
			if (!single)
			{
				add(start, start, start + size);
				continue;
			}
			const float u0 = Math::Max(start, clip_begin), u1 = Math::Min(start + size, clip_end);
			if (u1 - u0 <= 1e-4f)
				continue;
			const float begin = std::round(u0);
			const bool after_start = (start > area_begin + 1e-4f && start >= clip_begin);
			float end = (after_start && u1 >= start + size - 1e-4f ? std::round(begin + size) : std::round(u1));
			end = Math::Min(end, Math::Max(std::round(clip_end), begin));
			if (end - begin < 0.5f)
				continue;
			result.push_back({begin, end, (u0 - start) / size, (u1 - start) / size});
		}
		return result;
	};
	const Vector<Span> columns =
		spans(xs, tile.x, in.repeat_x, in.wrap_x, out.wrapped_x, area.Left(), clip.Left(), clip.Right());
	const Vector<Span> rows = spans(ys, tile.y, in.repeat_y, in.wrap_y, out.wrapped_y, area.Top(), clip.Top(), clip.Bottom());

	out.tiles.reserve(columns.size() * rows.size());
	for (const Span& row : rows)
	{
		for (const Span& column : columns)
		{
			LayerTile result;
			result.rect = Rectanglef::FromCorners(Vector2f(column.begin, row.begin), Vector2f(column.end, row.end));
			result.uv0 = Vector2f(column.uv0, row.uv0);
			result.uv1 = Vector2f(column.uv1, row.uv1);
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

	// A list property of the element. Shared, not a raw pointer: a value resolved from var() lives in the style's one
	// returned-property storage, which the next resolved property replaces (a background with var() in both its image
	// and its size freed the image list while it was drawn).
	template <typename T>
	SharedPtr<const T> GetList(Element* element, PropertyId id, SharedPtr<const PropertySource>* out_source = nullptr)
	{
		if (id == PropertyId::Invalid)
			return nullptr;
		const Property* property = element->GetStyle()->GetLocalPropertyWithResolvedVariables(id);
		if (!property)
			property = element->GetProperty(id);
		if (!property || property->unit != Unit::STYLE_VALUE)
			return nullptr;
		const StyleValuePtr& value = property->value.GetReference<StyleValuePtr>();
		if (!rmlui_dynamic_cast<const T*>(value.get()))
			return nullptr;
		if (out_source)
			*out_source = property->source;
		return std::static_pointer_cast<const T>(value);
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
	const SharedPtr<const ImageLayerList> images = GetList<ImageLayerList>(element, Ids(kind).image);
	return images && images->HasImages();
}

BoxArea ElementBackgroundLayers::GetColourClip(Element* element)
{
	const SharedPtr<const LayerKeywordList> clips = GetList<LayerKeywordList>(element, PropertyId::BackgroundClip);
	if (!clips || clips->GetCount() == 0)
		return BoxArea::Border;
	const SharedPtr<const ImageLayerList> images = GetList<ImageLayerList>(element, PropertyId::BackgroundImage);
	const size_t num_layers = (images && !images->layers.empty()) ? GetLayerCount(element, PropertyId::BackgroundImage, *images) : 1;
	return BoxArea(clips->values[(num_layers - 1) % clips->GetCount()]);
}

size_t ElementBackgroundLayers::GetLayerCount(Element* element, PropertyId image_id, const ImageLayerList& images)
{
	size_t count = images.layers.size();
	if (images.cross_faded)
	{
		const Property* underlying = element->GetStyle()->GetUnderlyingProperty(image_id);
		if (underlying && underlying->unit == Unit::STYLE_VALUE)
		{
			if (const auto* own = rmlui_dynamic_cast<const ImageLayerList*>(underlying->value.GetReference<StyleValuePtr>().get()))
				count = Math::Min(count, Math::Max(own->layers.size(), size_t(1)));
		}
	}
	return Math::Max(count, size_t(1));
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
	SharedPtr<const PropertySource> image_source;
	const SharedPtr<const ImageLayerList> images = GetList<ImageLayerList>(element, ids.image, &image_source);
	if (!render_manager || !images || !images->HasImages())
		return;

	const SharedPtr<const LayerPositionList> positions_x = GetList<LayerPositionList>(element, ids.position_x);
	const SharedPtr<const LayerPositionList> positions_y = GetList<LayerPositionList>(element, ids.position_y);
	const SharedPtr<const LayerSizeList> sizes = GetList<LayerSizeList>(element, ids.size);
	const SharedPtr<const LayerKeywordList> repeats = GetList<LayerKeywordList>(element, ids.repeat);
	const SharedPtr<const LayerKeywordList> origins = GetList<LayerKeywordList>(element, ids.origin);
	const SharedPtr<const LayerKeywordList> clips = GetList<LayerKeywordList>(element, ids.clip);
	const SharedPtr<const LayerKeywordList> attachments = GetList<LayerKeywordList>(element, ids.attachment);

	// The path url()s and decorator textures are relative to.
	String base_path;
	if (image_source)
		base_path = image_source->path;
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
	const int num_layers = (int)Math::Min(GetLayerCount(element, ids.image, *images), images->layers.size());
	for (int i = num_layers - 1; i >= 0; i--)
	{
		const ImageLayer& layer = images->layers[i];
		if (layer.kind == ImageLayer::Kind::None)
			continue;

		auto item = [i](const auto& /*list*/, size_t count) -> size_t { return count ? size_t(i) % count : 0; };
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

		// The image's intrinsic size; a cross-fade's is the weighted mean of its images' (Chromium: CSSCrossfadeValue blends
		// the sizes), each image stretched over the tile.
		Vector2f intrinsic;
		ImageSourceHandler::Image handled;
		bool handled_by_application = false;
		Texture raster;
		struct FadeSource {
			String url;
			float weight = 0.f;
			bool handled = false;
			Texture raster;
		};
		Vector<FadeSource> fade_sources;
		if (layer.kind == ImageLayer::Kind::CrossFade)
		{
			for (const auto& leaf : layer.fade)
			{
				FadeSource source;
				source.url = leaf.first;
				source.weight = leaf.second;
				Vector2f size;
				ImageSourceHandler::Image image;
				if (image_source_handler && image_source_handler->GetImage(element, leaf.first, base_path, Vector2f(0.f), false, false, image))
				{
					source.handled = true;
					size = image.intrinsic_size;
				}
				else
				{
					source.raster = render_manager->LoadTexture(leaf.first, base_path);
					size = Vector2f(source.raster.GetDimensions());
				}
				intrinsic += size * leaf.second;
				fade_sources.push_back(std::move(source));
			}
			if (intrinsic.x <= 0.f && intrinsic.y <= 0.f)
				continue;
		}
		else if (layer.kind == ImageLayer::Kind::Url)
		{
			if (image_source_handler && image_source_handler->GetImage(element, layer.url, base_path, Vector2f(0.f), false, false, handled))
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
			else if (attachment == LayerAttachment::Local && (element->GetScrollWidth() > element->GetClientWidth() + 0.5f ||
						 element->GetScrollHeight() > element->GetClientHeight() + 0.5f))
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
			auto resolve_position = [&](int axis, float free_space) {
				return ResolveLengthPercentage(axis == 0 ? position_x : position_y, *element, free_space);
			};
			// An image repeated along an axis is sampled with repeat wrap there (filtered across the seams of its tiles as
			// Chromium's image shader is), when the texture can be.
			input.wrap_x = input.wrap_y = (layer.kind == ImageLayer::Kind::Url);
			LayerTiles tiles = ComputeLayerTiles(input, resolve_position);
			if (tiles.tiles.empty())
				continue;

			Draw draw;
			Mesh mesh;
			if (layer.kind == ImageLayer::Kind::CrossFade)
			{
				// Each image over every tile, its colour weighted (premultiplied: all four channels); added when rendered.
				for (const FadeSource& source : fade_sources)
				{
					FadeImage image;
					ColourbPremultiplied colour = white;
					if (source.handled)
					{
						ImageSourceHandler::Image handled_image;
						if (!image_source_handler->GetImage(element, source.url, base_path, tiles.tile_size, false, false, handled_image) ||
							!handled_image.texture)
							continue;
						image.texture = handled_image.texture;
						image.image_owner = handled_image.owner;
						colour = handled_image.colour;
					}
					else
					{
						image.texture = source.raster;
					}
					colour = ColourbPremultiplied(byte(colour.red * source.weight + 0.5f), byte(colour.green * source.weight + 0.5f),
						byte(colour.blue * source.weight + 0.5f), byte(colour.alpha * source.weight + 0.5f));
					Mesh fade_mesh;
					for (const LayerTile& tile : tiles.tiles)
						AddQuad(fade_mesh, tile.rect.TopLeft(), tile.rect.BottomRight(), tile.uv0, tile.uv1, colour);
					image.geometry = render_manager->MakeGeometry(std::move(fade_mesh));
					draw.fade.push_back(std::move(image));
				}
				if (draw.fade.empty())
					continue;
			}
			else if (decorator)
			{
				// A gradient: one shader for the tile size, the texture coordinates of each tile its own positions.
				draw.shader = decorator->CompileImageShader(element, tiles.tile_size);
				if (!draw.shader)
					continue;
				const byte alpha = byte(opacity * 255.f);
				// A generated image is not stretched into a snapped destination (Chromium draws it at the tile's size).
				for (const LayerTile& tile : tiles.tiles)
					AddQuad(mesh, tile.rect.TopLeft(), tile.rect.BottomRight(), tile.uv0 * tiles.tile_size,
						tile.uv0 * tiles.tile_size + tile.rect.Size(), ColourbPremultiplied(alpha, alpha));
			}
			else
			{
				ColourbPremultiplied colour = white;
				const bool wrap = (tiles.wrapped_x || tiles.wrapped_y);
				if (handled_by_application)
				{
					ImageSourceHandler::Image image;
					if (!image_source_handler->GetImage(element, layer.url, base_path, tiles.tile_size, tiles.wrapped_x, tiles.wrapped_y, image) ||
						!image.texture)
						continue;
					if (wrap && !image.repeats)
					{
						// The application's texture does not repeat: the tiles one by one.
						input.wrap_x = input.wrap_y = false;
						tiles = ComputeLayerTiles(input, resolve_position);
					}
					draw.texture = image.texture;
					draw.image_owner = image.owner;
					colour = image.colour;
				}
				else if (wrap && render_manager->SupportsTextureWrapHint())
				{
					// The render interface samples a texture whose source asks for it with repeat wrap.
					const char* axes = (tiles.wrapped_x && tiles.wrapped_y ? "repeat" : (tiles.wrapped_x ? "repeat-x" : "repeat-y"));
					draw.texture = render_manager->LoadTexture(layer.url, base_path, String("rmlui-wrap=") + axes);
				}
				else
				{
					if (wrap)
					{
						input.wrap_x = input.wrap_y = false;
						tiles = ComputeLayerTiles(input, resolve_position);
					}
					draw.texture = raster;
				}
				for (const LayerTile& tile : tiles.tiles)
					AddQuad(mesh, tile.rect.TopLeft(), tile.rect.BottomRight(), tile.uv0, tile.uv1, colour);
			}
			if (draw.fade.empty())
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
		const bool fade = !draw.fade.empty();
		if (masked || fade)
			render_manager->PushLayer();

		if (fade)
		{
			// cross-fade(): the weighted images added (Chromium draws the image faded to with plus-lighter over the one
			// faded from), then composited as one layer.
			for (size_t i = 0; i < draw.fade.size(); i++)
			{
				if (i > 0)
					render_manager->PushLayer();
				draw.fade[i].geometry.Render(offset, draw.fade[i].texture);
				if (i > 0)
				{
					render_manager->CompositeLayers(render_manager->GetTopLayer(), render_manager->GetNextLayer(), BlendMode::Plus, {});
					render_manager->PopLayer();
				}
			}
		}
		else if (draw.decorator)
			draw.decorator->RenderElement(element, draw.decorator_data);
		else if (draw.shader)
			draw.geometry.Render(offset, {}, draw.shader);
		else
			draw.geometry.Render(offset, draw.texture);

		if (fade && !masked)
		{
			render_manager->CompositeLayers(render_manager->GetTopLayer(), render_manager->GetNextLayer(), BlendMode::Blend, {});
			render_manager->PopLayer();
		}
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
