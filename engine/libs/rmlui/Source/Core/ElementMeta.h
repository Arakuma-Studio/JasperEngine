#pragma once

#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/ElementScroll.h"
#include "../../Include/RmlUi/Core/Traits.h"
#include "../../Include/RmlUi/Core/Types.h"
#include "ControlledLifetimeResource.h"
#include "ElementBackgroundBorder.h"
#include "ElementEffects.h"
#include "ElementStyle.h"
#include "EventDispatcher.h"
#include "Pool.h"
#include <optional>

namespace Rml {

// Meta objects for element collected in a single struct to reduce memory allocations
struct ElementMeta {
	explicit ElementMeta(Element* el) : event_dispatcher(el), style(el), background_border(), effects(el), scroll(el), computed_values(el) {}
	SmallUnorderedMap<EventId, EventListener*> attribute_event_listeners;
	EventDispatcher event_dispatcher;
	ElementStyle style;
	ElementBackgroundBorder background_border;
	ElementEffects effects;
	ElementScroll scroll;
	Style::ComputedValues computed_values;
	// The containing block of an absolutely positioned element when it is not its offset parent's padding box (a grid
	// area, the viewport), relative to the offset parent's border box; set by the box that formats it
	// (ContainerBox::ClosePositionedElements).
	std::optional<Rectanglef> containing_block_override;
	// The pseudo-elements the element generates, by type: ::before, ::after, ::marker (phase 3, cascade: PseudoElements).
	Element* pseudo_elements[3] = {nullptr, nullptr, nullptr};
};

struct ElementMetaPool {
	Pool<ElementMeta> pool{50, true};

	static ControlledLifetimeResource<ElementMetaPool> element_meta_pool;
	static void Initialize();
	static void Shutdown();
};

} // namespace Rml
