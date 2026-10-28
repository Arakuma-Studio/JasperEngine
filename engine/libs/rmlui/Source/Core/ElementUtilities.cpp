#include "../../Include/RmlUi/Core/ElementUtilities.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Context.h"
#include "../../Include/RmlUi/Core/Core.h"
#include "../../Include/RmlUi/Core/DecorationTypes.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/ElementScroll.h"
#include "../../Include/RmlUi/Core/Factory.h"
#include "../../Include/RmlUi/Core/FontEngineInterface.h"
#include "../../Include/RmlUi/Core/Math.h"
#include "../../Include/RmlUi/Core/MeshUtilities.h"
#include "../../Include/RmlUi/Core/RenderManager.h"
#include "../../Include/RmlUi/Core/TextShapingContext.h"
#include "DataController.h"
#include "DataModel.h"
#include "DataView.h"
#include "ElementBackgroundBorder.h"
#include "ElementStyle.h"
#include "Layout/LayoutDetails.h"
#include "Layout/LayoutEngine.h"
#include "TransformState.h"
#include <limits>

namespace Rml {

Element* ElementUtilities::GetElementById(Element* root_element, const String& id)
{
	// Breadth first search on elements for the corresponding id
	typedef Queue<Element*> SearchQueue;
	SearchQueue search_queue;
	search_queue.push(root_element);

	while (!search_queue.empty())
	{
		Element* element = search_queue.front();
		search_queue.pop();

		if (element->GetId() == id)
		{
			return element;
		}

		// Add all children to search
		for (int i = 0; i < element->GetNumChildren(); i++)
			search_queue.push(element->GetChild(i));
	}

	return nullptr;
}

void ElementUtilities::GetElementsByTagName(ElementList& elements, Element* root_element, const String& tag, const String& stop_tag)
{
	// Breadth first search on elements for the corresponding id
	typedef Queue<Element*> SearchQueue;
	SearchQueue search_queue;
	for (int i = 0; i < root_element->GetNumChildren(); ++i)
		search_queue.push(root_element->GetChild(i));

	while (!search_queue.empty())
	{
		Element* element = search_queue.front();
		search_queue.pop();

		if (element->GetTagName() == tag)
			elements.push_back(element);

		if (stop_tag.empty() || element->GetTagName() != stop_tag)
		{
			// Add all children to search.
			for (int i = 0; i < element->GetNumChildren(); i++)
				search_queue.push(element->GetChild(i));
		}
	}
}

void ElementUtilities::GetElementsByClassName(ElementList& elements, Element* root_element, const String& class_name)
{
	// Breadth first search on elements for the corresponding id
	typedef Queue<Element*> SearchQueue;
	SearchQueue search_queue;
	for (int i = 0; i < root_element->GetNumChildren(); ++i)
		search_queue.push(root_element->GetChild(i));

	while (!search_queue.empty())
	{
		Element* element = search_queue.front();
		search_queue.pop();

		if (element->IsClassSet(class_name))
			elements.push_back(element);

		// Add all children to search.
		for (int i = 0; i < element->GetNumChildren(); i++)
			search_queue.push(element->GetChild(i));
	}
}

float ElementUtilities::GetDensityIndependentPixelRatio(Element* element)
{
	Context* context = element->GetContext();
	if (context == nullptr)
		return 1.0f;

	return context->GetDensityIndependentPixelRatio();
}

int ElementUtilities::GetStringWidth(Element* element, StringView string, Character prior_character)
{
	const auto& computed = element->GetComputedValues();
	const TextShapingContext text_shaping_context{computed.language(), computed.direction(), computed.font_kerning(), computed.letter_spacing(),
		computed.word_spacing()};

	FontFaceHandle font_face_handle = element->GetFontFaceHandle();
	if (font_face_handle == 0)
		return 0;

	return GetFontEngineInterface()->GetStringWidth(font_face_handle, string, text_shaping_context, prior_character);
}

// A scrollbar or the scrollbar corner of 'scroll_container' (the elements ElementScroll appends to it).
static bool IsScrollbarPart(const Element* element, Element* scroll_container)
{
	if (!element || element->GetParentNode() != scroll_container)
		return false;
	ElementScroll* scroll = scroll_container->GetElementScroll();
	if (element == scroll->GetScrollbar(ElementScroll::VERTICAL) || element == scroll->GetScrollbar(ElementScroll::HORIZONTAL))
		return true;
	return element->GetTagName() == "scrollbarcorner";
}

// Fork (P7-F1): 'element' is a proper ancestor of 'descendant'.
static bool IsProperAncestor(const Element* element, const Element* descendant)
{
	for (const Element* e = descendant->GetParentNode(); e; e = e->GetParentNode())
	{
		if (e == element)
			return true;
	}
	return false;
}

bool ElementUtilities::GetClippingRegion(Element* element, Rectanglei& out_clip_region, ClipMaskGeometryList* out_clip_mask_list,
	bool force_clip_self, const Element* clip_root)
{
	using Style::Clip;
	Clip target_element_clip = element->GetComputedValues().clip();
	if (target_element_clip == Clip::Type::None && !force_clip_self)
		return false;

	int num_ignored_clips = target_element_clip.GetNumber();

	// Search through the element's ancestors, finding all elements that clip their overflow and have overflow to clip.
	// For each that we find, we combine their clipping region with the existing clipping region, and so build up a
	// complete clipping region for the element.
	Element* clipping_element = (force_clip_self ? element : element->GetOffsetParent());
	// The element on the offset-parent chain below 'clipping_element' (a scroll container's scrollbars are not clipped by it).
	const Element* child_on_chain = (force_clip_self ? nullptr : element);

	Rectanglef clip_region = Rectanglef::MakeInvalid();

	while (clipping_element)
	{
		// Fork (P7-F1): inside the effects layer of 'clip_root', the clips of its ancestors are applied to the layer's
		// composite instead (the element's own clip and its descendants' clip the content drawn into the layer).
		if (clip_root && clipping_element != clip_root && IsProperAncestor(clipping_element, clip_root))
			break;

		const bool force_clip_current_element = (force_clip_self && clipping_element == element);
		const ComputedValues& clip_computed = clipping_element->GetComputedValues();
		const bool clip_enabled = (clip_computed.overflow_x() != Style::Overflow::Visible || clip_computed.overflow_y() != Style::Overflow::Visible);
		const bool clip_always = (clip_computed.clip() == Clip::Type::Always);
		const bool clip_none = (clip_computed.clip() == Clip::Type::None);
		const int clip_number = clip_computed.clip().GetNumber();

		// Merge the existing clip region with the current clip region, unless we are ignoring clip regions.
		if (((clip_always || clip_enabled) && num_ignored_clips == 0) || force_clip_current_element)
		{
			const BoxArea clip_area = (force_clip_current_element ? BoxArea::Border : clipping_element->GetClipArea());
			// Every descendant that has this element in its containing-block chain is clipped (CSS). 6.3 clipped only when
			// the scrollable overflow exceeded the client area, which leaves out absolutely positioned boxes (they are laid
			// out after it is measured) and the ink overflow of the content (shadows, outlines): they drew outside an
			// 'overflow: hidden' box.
			bool disable_scissor_clipping = false;

			if (out_clip_mask_list)
			{
				const TransformState* transform_state = clipping_element->GetTransformState();
				const Matrix4f* transform = (transform_state ? transform_state->GetTransform() : nullptr);
				const bool has_border_radius = clip_computed.has_border_radius();

				// If the element has border-radius we always use a clip mask, since we can't easily predict if content is located on the curved
				// region to be clipped; with a transform too, the scissor cannot follow it.
				if (has_border_radius || transform)
				{
					Geometry* clip_geometry = clipping_element->GetElementBackgroundBorder()->GetClipGeometry(clipping_element, clip_area);
					const ClipMaskOperation clip_operation = (out_clip_mask_list->empty() ? ClipMaskOperation::Set : ClipMaskOperation::Intersect);
					const Vector2f absolute_offset = clipping_element->GetAbsoluteOffset(BoxArea::Border).Round();
					if (*clip_geometry)
						out_clip_mask_list->push_back(ClipMaskGeometry{clip_operation, clip_geometry, absolute_offset, transform});
				}

				// If we only have border-radius then we add this element to the scissor region as well as the clip mask. This may help with e.g.
				// culling text render calls. However, when we have a transform, the element cannot be added to the scissor region since its geometry
				// may be projected entirely elsewhere.
				if (transform)
					disable_scissor_clipping = true;
			}

			if (!disable_scissor_clipping)
			{
				// Shrink the scissor region to the element's client area.
				Vector2f element_offset = clipping_element->GetAbsoluteOffset(clip_area).Round();
				Vector2f element_size = clipping_element->GetRenderBoxFillSize(clip_area);
				// The content of a scroll container is clipped to its scrollport: the padding box without the scrollbars (and
				// a 'scrollbar-gutter'), which sit between its border and its padding (CSS Overflow 3 §3); the scrollbars
				// themselves are not clipped by it. 6.3 clipped to the whole padding box, so a positioned descendant (a sticky
				// header, say) painted over the scrollbars.
				if (clip_area == BoxArea::Padding && !force_clip_current_element)
				{
					ElementScroll* scroll = clipping_element->GetElementScroll();
					const float scrollbar_width = scroll->GetScrollbarSize(ElementScroll::VERTICAL);
					const float scrollbar_height = scroll->GetScrollbarSize(ElementScroll::HORIZONTAL);
					// (Without scrollbars there is nothing to take off, whether the element is a scrollbar part or not.)
					if ((scrollbar_width == 0.f && scrollbar_height == 0.f) || !IsScrollbarPart(child_on_chain, clipping_element))
					{
						element_size.x = Math::Max(element_size.x - scrollbar_width, 0.f);
						element_size.y = Math::Max(element_size.y - scrollbar_height, 0.f);
					}
				}
				Rectanglef element_region = Rectanglef::FromPositionSize(element_offset, element_size);

				// 'overflow: clip' clips its axis only, at the padding box extended by 'overflow-clip-margin' (CSS Overflow 3/4).
				const Style::Overflow overflow_x = clip_computed.overflow_x(), overflow_y = clip_computed.overflow_y();
				if (!force_clip_current_element && !clip_always && (overflow_x == Style::Overflow::Clip || overflow_y == Style::Overflow::Clip))
				{
					constexpr float unbounded = 1e9f;
					const float margin = clip_computed.overflow_clip_margin();
					Vector2f p0 = element_region.TopLeft(), p1 = element_region.BottomRight();
					if (overflow_x == Style::Overflow::Clip)
						p0.x -= margin, p1.x += margin;
					else
						p0.x = -unbounded, p1.x = unbounded;
					if (overflow_y == Style::Overflow::Clip)
						p0.y -= margin, p1.y += margin;
					else
						p0.y = -unbounded, p1.y = unbounded;
					element_region = Rectanglef::FromCorners(p0, p1);
				}

				clip_region = element_region.IntersectIfValid(clip_region);
			}
		}

		// If this region is meant to clip and we're skipping regions, update the counter.
		if (num_ignored_clips > 0 && clip_enabled)
			num_ignored_clips--;

		// Inherit how many clip regions this ancestor ignores.
		num_ignored_clips = Math::Max(num_ignored_clips, clip_number);

		// If this region ignores all clipping regions, then we do too.
		if (clip_none)
			break;

		// Climb the tree to this region's parent.
		child_on_chain = clipping_element;
		clipping_element = clipping_element->GetOffsetParent();
	}

	if (clip_region.Valid())
	{
		Math::SnapToPixelGrid(clip_region);
		out_clip_region = Rectanglei(clip_region);
	}

	return clip_region.Valid();
}

bool ElementUtilities::GetRenderClippingRegion(Element* element, RenderManager& render_manager, Rectanglei& scissor_region,
	ClipMaskGeometryList& clip_mask_list, bool force_clip_self)
{
	const Element* clip_root = render_manager.GetClipRoot();
	bool scissoring_enabled = GetClippingRegion(element, scissor_region, &clip_mask_list, force_clip_self, clip_root);

	// Fork (P7-F1): within the window of the effects layer being drawn.
	if (clip_root)
	{
		const Rectanglei window = render_manager.GetClipRootWindow();
		if (window.Valid())
		{
			scissor_region = (scissoring_enabled ? scissor_region.Intersect(window) : window);
			scissoring_enabled = true;
		}
	}
	return scissoring_enabled;
}

bool ElementUtilities::SetClippingRegion(Element* element, bool force_clip_self)
{
	Context* context = element->GetContext();
	if (!context)
		return false;

	RenderManager& render_manager = context->GetRenderManager();

	Rectanglei clip_region;
	ClipMaskGeometryList clip_mask_list;

	const bool scissoring_enabled = GetRenderClippingRegion(element, render_manager, clip_region, clip_mask_list, force_clip_self);
	if (scissoring_enabled)
		render_manager.SetScissorRegion(clip_region);
	else
		render_manager.DisableScissorRegion();

	render_manager.SetClipMask(std::move(clip_mask_list));

	return true;
}

ScopedReplacedContentClip::ScopedReplacedContentClip(Element* element, bool enable)
{
	if (!enable || !element || !element->GetComputedValues().has_border_radius())
		return;
	RenderManager* manager = element->GetRenderManager();
	if (!manager)
		return;
	Geometry* clip_geometry = element->GetElementBackgroundBorder()->GetClipGeometry(element, BoxArea::Content);
	if (!clip_geometry || !*clip_geometry)
		return;

	render_manager = manager;
	saved_state = manager->GetState();
	const TransformState* transform_state = element->GetTransformState();
	const Matrix4f* transform = (transform_state ? transform_state->GetTransform() : nullptr);
	ClipMaskGeometryList clip_mask_list = saved_state.clip_mask_list;
	// The first mask of a list sets the stencil, the others intersect it (as GetClippingRegion's lists).
	const ClipMaskOperation operation = (clip_mask_list.empty() ? ClipMaskOperation::Set : ClipMaskOperation::Intersect);
	clip_mask_list.push_back(ClipMaskGeometry{operation, clip_geometry, element->GetAbsoluteOffset(BoxArea::Border).Round(), transform});
	manager->SetClipMask(std::move(clip_mask_list));
}

ScopedReplacedContentClip::~ScopedReplacedContentClip()
{
	if (render_manager)
		render_manager->SetState(saved_state);
}

void ReplacedContentGeometry::Set(Rectanglef new_rect, ColourbPremultiplied new_colour, Vector2f top_left_texcoord, Vector2f bottom_right_texcoord)
{
	if (is_set && new_rect.TopLeft() == rect.TopLeft() && new_rect.BottomRight() == rect.BottomRight() && new_colour == colour &&
		top_left_texcoord == texcoords[0] && bottom_right_texcoord == texcoords[1])
		return;
	rect = new_rect;
	colour = new_colour;
	texcoords[0] = top_left_texcoord;
	texcoords[1] = bottom_right_texcoord;
	is_set = true;
	dirty = true;
}

void ReplacedContentGeometry::Clear()
{
	geometry = Geometry();
	is_set = false;
	dirty = true;
}

void ReplacedContentGeometry::Render(Element* element, Texture texture)
{
	if (!is_set || !element)
		return;
	RenderManager* render_manager = element->GetRenderManager();
	if (!render_manager)
		return;

	const bool new_rounded = element->GetComputedValues().has_border_radius();
	if (new_rounded)
	{
		const RenderBox new_content_box = element->GetRenderBox(BoxArea::Content, 0, false);
		if (!rounded || !(new_content_box == content_box) || feather_unit != ElementBackgroundBorder::GetFeatherUnit(element))
		{
			content_box = new_content_box;
			dirty = true;
		}
	}
	else if (rounded)
	{
		dirty = true;
	}

	if (dirty || !geometry)
	{
		rounded = new_rounded;
		feather_unit = ElementBackgroundBorder::GetFeatherUnit(element);
		Mesh mesh = geometry.Release(Geometry::ReleaseMode::ClearMesh);
		if (rounded)
		{
			needs_clip = !MeshUtilities::GenerateReplacedContent(mesh, content_box, rect, colour, texcoords[0], texcoords[1], feather_unit);
		}
		else
		{
			MeshUtilities::GenerateQuad(mesh, rect.TopLeft(), rect.Size(), colour, texcoords[0], texcoords[1]);
			needs_clip = false;
		}
		geometry = render_manager->MakeGeometry(std::move(mesh));
		dirty = false;
	}

	ScopedReplacedContentClip clip(element, needs_clip);
	geometry.Render(element->GetAbsoluteOffset(BoxArea::Border), texture);
}

bool ElementUtilities::GetBoundingBox(Rectanglef& out_rectangle, Element* element, BoxArea box_area)
{
	RMLUI_ASSERT(element);

	Vector2f shadow_extent_top_left, shadow_extent_bottom_right;
	if (box_area == BoxArea::Auto)
	{
		// 'Auto' acts like border box extended to encompass any ink overflow, including the element's box-shadow.
		// Note: Does not currently include ink overflow due to filters, as that is handled manually in ElementEffects.
		box_area = BoxArea::Border;

		if (const Property* p_box_shadow = element->GetStyle()->GetLocalPropertyWithResolvedVariables(PropertyId::BoxShadow))
		{
			RMLUI_ASSERT(p_box_shadow->value.GetType() == Variant::BOXSHADOWLIST);
			const BoxShadowList& shadow_list = p_box_shadow->value.GetReference<BoxShadowList>();

			for (const BoxShadow& shadow : shadow_list)
			{
				if (!shadow.inset)
				{
					const float extent = 1.5f * element->ResolveLength(shadow.blur_radius) + element->ResolveLength(shadow.spread_distance);
					const Vector2f offset = {element->ResolveLength(shadow.offset_x), element->ResolveLength(shadow.offset_y)};

					shadow_extent_top_left = Math::Max(shadow_extent_top_left, -offset + Vector2f(extent));
					shadow_extent_bottom_right = Math::Max(shadow_extent_bottom_right, offset + Vector2f(extent));
				}
			}
		}
	}

	// Element bounds in non-transformed space.
	Rectanglef bounds = Rectanglef::FromPositionSize(element->GetAbsoluteOffset(box_area), element->GetBox().GetSize(box_area));
	bounds = bounds.Extend(shadow_extent_top_left, shadow_extent_bottom_right);

	const TransformState* transform_state = element->GetTransformState();
	const Matrix4f* transform = (transform_state ? transform_state->GetTransform() : nullptr);

	// Early exit in the common case of no transform.
	if (!transform)
	{
		out_rectangle = bounds;
		return true;
	}

	Context* context = element->GetContext();
	if (!context)
		return false;

	constexpr int num_corners = 4;
	Vector2f corners[num_corners] = {
		bounds.TopLeft(),
		bounds.TopRight(),
		bounds.BottomRight(),
		bounds.BottomLeft(),
	};

	// Transform and project corners to window coordinates.
	constexpr float z_clip = 10'000.f;
	const Vector2f window_size = Vector2f(context->GetDimensions());
	const Matrix4f project = Matrix4f::ProjectOrtho(0.f, window_size.x, 0.f, window_size.y, -z_clip, z_clip);
	const Matrix4f project_transform = project * (*transform);
	bool any_vertex_depth_clipped = false;

	for (int i = 0; i < num_corners; i++)
	{
		const Vector4f pos_clip_space = project_transform * Vector4f(corners[i].x, corners[i].y, 0, 1);
		const Vector2f pos_ndc = Vector2f(pos_clip_space.x, pos_clip_space.y) / pos_clip_space.w;
		const Vector2f pos_viewport = 0.5f * window_size * (pos_ndc + Vector2f(1));
		corners[i] = pos_viewport;
		any_vertex_depth_clipped |= !(-pos_clip_space.w <= pos_clip_space.z && pos_clip_space.z <= pos_clip_space.w);
	}

	// If any part of the box area is outside the depth clip planes we give up finding the bounding box. In this situation a renderer would normally
	// clip the underlying triangles against the clip planes. We could in principle do the same, but the added complexity does not seem worthwhile for
	// our use cases.
	if (any_vertex_depth_clipped)
		return false;

	// Find the rectangle covering the projected corners.
	out_rectangle = Rectanglef::FromPosition(corners[0]);
	for (int i = 1; i < num_corners; i++)
		out_rectangle = out_rectangle.Join(corners[i]);

	return true;
}

void ElementUtilities::FormatElement(Element* element, Vector2f containing_block)
{
	LayoutEngine::FormatElement(element, containing_block);
}

void ElementUtilities::BuildBox(Box& box, Vector2f containing_block, Element* element, bool inline_element)
{
	LayoutDetails::BuildBox(box, containing_block, element, inline_element ? BuildBoxMode::Inline : BuildBoxMode::Block);
}

bool ElementUtilities::PositionElement(Element* element, Vector2f offset, PositionAnchor anchor)
{
	Element* parent = element->GetParentNode();
	if (!parent)
		return false;

	const Box& parent_box = parent->GetBox();
	Vector2f containing_block = parent_box.GetSize();
	containing_block.x -= parent->GetElementScroll()->GetScrollbarSize(ElementScroll::VERTICAL);
	containing_block.y -= parent->GetElementScroll()->GetScrollbarSize(ElementScroll::HORIZONTAL);

	Box box;
	LayoutDetails::BuildBox(box, containing_block, element);
	if (box.GetSize().y < 0.f)
		box.SetContent(Vector2f(box.GetSize().x, containing_block.y));
	element->SetBox(box);

	Vector2f element_block = box.GetSize(BoxArea::Margin);
	Vector2f resolved_offset = offset;

	if (anchor & RIGHT)
		resolved_offset.x = containing_block.x - (element_block.x + offset.x);
	if (anchor & BOTTOM)
		resolved_offset.y = containing_block.y - (element_block.y + offset.y);

	// Position element relative to its parent.
	Vector2f relative_offset = parent_box.GetPosition(BoxArea::Content);
	relative_offset += resolved_offset;
	relative_offset.x += box.GetEdge(BoxArea::Margin, BoxEdge::Left);
	relative_offset.y += box.GetEdge(BoxArea::Margin, BoxEdge::Top);

	element->SetOffset(relative_offset, parent);

	return true;
}

bool ElementUtilities::ApplyTransform(Element& element)
{
	Context* context = element.GetContext();
	if (!context)
		return false;

	RenderManager& render_manager = context->GetRenderManager();

	const Matrix4f* new_transform = nullptr;
	if (const TransformState* state = element.GetTransformState())
		new_transform = state->GetTransform();

	render_manager.SetTransform(new_transform);

	return true;
}

bool ElementUtilities::ApplyDataViewsControllers(Element* element)
{
	RMLUI_ASSERT(element);

	// If we have an active data model, check the attributes for any data bindings
	DataModel* data_model = element->GetDataModel();
	if (!data_model)
		return false;

	struct ViewControllerInitializer {
		String type;
		String modifier;
		String expression;
		DataViewPtr view;
		DataControllerPtr controller;
		explicit operator bool() const { return view || controller; }
	};

	// Since data views and controllers may modify the element's attributes during initialization, we
	// need to iterate over all the attributes _before_ initializing any views or controllers. We store
	// the information needed to initialize them in the following container.
	Vector<ViewControllerInitializer> initializer_list;

	for (auto& attribute : element->GetAttributes())
	{
		// Data views and controllers are declared by the following element attribute:
		//     data-[type]-[modifier]="[expression]"

		constexpr size_t data_str_length = sizeof("data-") - 1;

		const String& name = attribute.first;

		if (name.size() > data_str_length && name[0] == 'd' && name[1] == 'a' && name[2] == 't' && name[3] == 'a' && name[4] == '-')
		{
			const size_t type_end = name.find('-', data_str_length);
			const size_t type_size = (type_end == String::npos ? String::npos : type_end - data_str_length);
			const String type_name = name.substr(data_str_length, type_size);

			ViewControllerInitializer initializer;

			const size_t modifier_offset = data_str_length + type_name.size() + 1;
			if (modifier_offset < name.size())
				initializer.modifier = name.substr(modifier_offset);

			if (DataViewPtr view = Factory::InstanceDataView(type_name, element))
				initializer.view = std::move(view);

			if (DataControllerPtr controller = Factory::InstanceDataController(type_name, element))
				initializer.controller = std::move(controller);

			if (initializer)
			{
				initializer.type = type_name;
				initializer.expression = attribute.second.Get<String>();

				if (Factory::IsStructuralDataView(type_name))
				{
					// Structural data views should cancel all other non-structural data views and controllers.
					// Clear all other views, and only initialize this one.
					// E.g. in elements with a 'data-for' attribute, the data views should be constructed on the
					// generated children elements and not on the current element generating the 'for' view.
					initializer_list.clear();
					initializer_list.push_back(std::move(initializer));
					break;
				}
				else
				{
					initializer_list.push_back(std::move(initializer));
				}
			}
		}
	}

	bool result = false;

	// Now, we can safely initialize the data views and controllers, even modifying the element's attributes when desired.
	for (ViewControllerInitializer& initializer : initializer_list)
	{
		DataViewPtr& view = initializer.view;
		DataControllerPtr& controller = initializer.controller;

		if (view)
		{
			if (view->Initialize(*data_model, element, initializer.expression, initializer.modifier))
			{
				data_model->AddView(std::move(view));
				result = true;
			}
			else
				Log::Message(Log::LT_WARNING, "Could not add data-%s view to element: %s", initializer.type.c_str(), element->GetAddress().c_str());
		}

		if (controller)
		{
			if (controller->Initialize(*data_model, element, initializer.expression, initializer.modifier))
			{
				data_model->AddController(std::move(controller));
				result = true;
			}
			else
				Log::Message(Log::LT_WARNING, "Could not add data-%s controller to element: %s", initializer.type.c_str(),
					element->GetAddress().c_str());
		}
	}

	return result;
}

} // namespace Rml
