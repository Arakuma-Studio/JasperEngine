#pragma once

#include "../../Include/RmlUi/Core/StyleSheetTypes.h"
#include "../../Include/RmlUi/Core/StyleValue.h"

namespace Rml {

class PropertyDictionary;
class PropertyParser;
class PropertySpecification;
struct ShorthandDefinition;

// Background and mask layers (phase 3, p3-paint; docs/html-ui/phase-3-fork-patches.md 3.9): the values of the comma-list
// longhands background-image, -position-x, -position-y, -size, -repeat, -origin, -clip, -attachment and of mask-image,
// -position-x, -position-y, -size, -repeat. Each list is a StyleValue; a layer reads item i % count of each list (CSS
// Backgrounds 3, 2.2: the image list sets the number of layers).

enum class LayerRepeat : uint8_t { Repeat, Space, Round, NoRepeat };
enum class LayerAttachment : uint8_t { Scroll, Fixed, Local };

// One image of an image list: none, a url(), a decorator function (the gradients and every registered decorator
// function: compat's html-layer(), html-shape(), phase 5's jfx-*()), or the cross-fade of url() images a transition or an
// animation of background-image draws (CSS Images 4 cross-fade(): ImageLayerList::Interpolate).
struct ImageLayer {
	enum class Kind : uint8_t { None, Url, Decorator, CrossFade };
	Kind kind = Kind::None;
	String url;               // Url: as written, unquoted
	int decorator_index = -1; // Decorator: its declaration in ImageLayerList::decorators (with its RCSS paint area, Auto: none)
	String text;              // the item as written (CrossFade: serialized as Chromium's computed value)
	// CrossFade: the url()s mixed and their weights (each above 0, summing to 1), the images added with them.
	Vector<std::pair<String, float>> fade;
};

class ImageLayerList final : public StyleValue {
public:
	RMLUI_RTTI_DeclareWithParent(ImageLayerList, StyleValue)

	Vector<ImageLayer> layers;
	// The decorator functions of the list, instanced by the style sheet (StyleSheet::InstanceDecorators); its 'value' is
	// the list's text, the instancing cache key.
	SharedPtr<DecoratorDeclarationList> decorators;

	// mask-image's list: discrete (Chromium 153 does not interpolate it).
	bool mask = false;
	// Interpolated (url() images cross-faded): the element draws no more layers than its own value has (Chromium paints the
	// layers of the element's style, the interpolated list being the lowest common multiple of the two lengths).
	bool cross_faded = false;

	bool HasImages() const;
	String ToString() const override;
	bool Equals(const StyleValue& other) const override;
	// As Chromium 153 (CSSImageListInterpolationType): two lists of url() images (or their cross-fades) interpolate image by
	// image as cross-fade(), the shorter list repeated to the lowest common multiple of the lengths; a list with a gradient,
	// a decorator function or 'none', and mask-image, are discrete (no transition starts without 'allow-discrete').
	SharedPtr<const StyleValue> Interpolate(const StyleValue& to, float alpha, Element& element) const override;
	Units GetUnitDependencies() const override;
};

// background-position-x / -y, mask-position-x / -y: a length or percentage per layer (an offset from the right or bottom
// edge is a calc()). Interpolated per layer.
class LayerPositionList final : public StyleValue {
public:
	RMLUI_RTTI_DeclareWithParent(LayerPositionList, StyleValue)

	Vector<LengthPercentageValue> values;

	String ToString() const override;
	bool Equals(const StyleValue& other) const override;
	SharedPtr<const StyleValue> Interpolate(const StyleValue& to, float alpha, Element& element) const override;
	Units GetUnitDependencies() const override;
};

struct LayerSize {
	enum class Mode : uint8_t { Explicit, Cover, Contain };
	Mode mode = Mode::Explicit;
	bool auto_x = true, auto_y = true;
	LengthPercentageValue x, y;
	bool operator==(const LayerSize& other) const
	{
		return mode == other.mode && auto_x == other.auto_x && auto_y == other.auto_y && (auto_x || x == other.x) && (auto_y || y == other.y);
	}
};

// background-size, mask-size: auto | cover | contain | <length-percentage>{1,2} with auto per axis. Interpolated per layer
// where both sizes are lengths in the same axes.
class LayerSizeList final : public StyleValue {
public:
	RMLUI_RTTI_DeclareWithParent(LayerSizeList, StyleValue)

	Vector<LayerSize> values;

	String ToString() const override;
	bool Equals(const StyleValue& other) const override;
	SharedPtr<const StyleValue> Interpolate(const StyleValue& to, float alpha, Element& element) const override;
	Units GetUnitDependencies() const override;
};

// background-repeat, -origin, -clip, -attachment and mask-repeat: keywords per layer (a repeat is two: x and y), discrete.
class LayerKeywordList final : public StyleValue {
public:
	RMLUI_RTTI_DeclareWithParent(LayerKeywordList, StyleValue)

	enum class Type : uint8_t { Repeat, Box, Attachment };
	Type type = Type::Box;
	Vector<uint8_t> values; // Repeat: LayerRepeat x, y per layer; Box: BoxArea; Attachment: LayerAttachment

	size_t GetCount() const { return type == Type::Repeat ? values.size() / 2 : values.size(); }
	String ToString() const override;
	bool Equals(const StyleValue& other) const override;
};

// The parsers ("image_layers", "layer_position_x", "layer_position_y", "layer_size", "layer_repeat", "layer_box",
// "layer_attachment") and the Custom shorthands background, background-position, mask, mask-position.
void RegisterBackgroundLayerParsers();
// The image-layer parser's lists by text (fork, P5-5): cleared when decorator instancers are registered, the style sheet
// cache is cleared, or the factory shuts down; the hits count lookups that parsed nothing (tests).
void ClearImageLayerParseCache();
uint64_t GetImageLayerParseCacheHits();
bool ParseBackgroundShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification);
bool ParseMaskShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification);
bool ParseLayerPositionShorthand(PropertyDictionary& dictionary, const ShorthandDefinition& definition, const String& value,
	const PropertySpecification& specification);

// Splits at the commas (or white space) outside of parentheses and quotes.
StringList SplitLayerList(const String& value, char separator);

} // namespace Rml
