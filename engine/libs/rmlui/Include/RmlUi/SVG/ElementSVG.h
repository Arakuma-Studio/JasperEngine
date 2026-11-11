#pragma once

#include "../Core/Element.h"
#include "../Core/ElementUtilities.h"
#include "../Core/Geometry.h"
#include "../Core/Header.h"

namespace Rml {
namespace SVG {
	struct SVGData;

	/**
	    Fork (JasperEngine, phase-2 hook H8): lets the host rewrite an <svg> element's source before it is rasterised
	    (to resolve currentColor against the element's CSS color, apply CSS paint properties) and choose the colour the
	    rasterised image is multiplied with. Set with SetSourceHook(); the plugin's own behaviour is unchanged without one.
	 */
	class RMLUICORE_API SourceHook {
	public:
		struct Result {
			/// Cache id of the processed source: must differ whenever `data` differs.
			String source_id;
			/// The processed SVG document (text).
			String data;
			/// Multiply the image by `colour` instead of the element's image-color x opacity.
			bool use_colour = false;
			ColourbPremultiplied colour;
		};

		virtual ~SourceHook();

		/// Called when an element loads its source. `path` is the file the source was resolved to (empty for inline
		/// SVG), `source` the SVG text (file contents, or the inline SVG wrapped in its <svg> tag).
		/// @return True to use `result`, false to keep the plugin's own behaviour for this element.
		virtual bool ProcessSource(Element* element, const String& path, const String& source, Result& result) = 0;
		/// @return True when a change of these properties needs the element's source to be processed again.
		virtual bool IsSourceProperty(const PropertyIdSet& changed_properties) = 0;
	};

	/// Installs the hook (nullptr removes it); it must outlive every <svg> element using it.
	RMLUICORE_API void SetSourceHook(SourceHook* hook);
	RMLUICORE_API SourceHook* GetSourceHook();
} // namespace SVG

class RMLUICORE_API ElementSVG : public Element {
public:
	RMLUI_RTTI_DeclareWithParent(ElementSVG, Element)

	explicit ElementSVG(const String& tag);
	~ElementSVG() override;

	/// Returns the element's inherent size.
	bool GetIntrinsicDimensions(Vector2f& dimensions, float& ratio) override;

	/// Loads the current source file if needed. This normally happens automatically during layouting.
	void EnsureSourceLoaded();

	/// Inline descendants changed: regenerate their SVG source on the next render.
	void DirtySource();

	/// Gets the SVG XML data (as text) if using inline SVG, if using a file source this will return a blank string
	/// @param[out] content The SVG XML data (as text) or blank string
	void GetInnerRML(String& content) const override;

	/// Gets the SVG XML data (as text) if using inline SVG, if using a file source this will return a blank string
	/// @param[in] rml The SVG XML data (as text) or blank string
	void SetInnerRML(const String& rml) override;

protected:
	/// Renders the image.
	void OnRender() override;

	/// Regenerates the element's geometry.
	void OnResize() override;
	void OnChildAdd(Element* child) override;
	void OnChildRemove(Element* child) override;

	/// Checks for changes to the image's source or dimensions.
	/// @param[in] changed_attributes A list of attributes changed on the element.
	void OnAttributeChange(const ElementAttributes& changed_attributes) override;

	/// Called when properties on the element are changed.
	/// @param[in] changed_properties The properties changed on the element.
	void OnPropertyChange(const PropertyIdSet& changed_properties) override;

private:
	/// Generate unique internal ids for SVG elements using inline SVG.
	static unsigned long internal_id_counter;

	// An SVG file (src) is a replaced image: object-fit and object-position place it in the content box (phase 3,
	// p3-paint), rasterised at the size it is drawn at. Inline SVG content is placed by its own preserveAspectRatio, as
	// in browsers.
	void GenerateFittedGeometry(const Function<SharedPtr<SVG::SVGData>(const Vector2i*)>& get_handle);

	String svg_data;
	unsigned long source_revision = 0;
	bool svg_dirty = false;

	SharedPtr<SVG::SVGData> handle;
	// The picture drawn: src, the part of the image object-fit shows; inline, the rasterised content box. Trimmed to the
	// content edge curve of a rounded box (fork, P4-G1).
	ReplacedContentGeometry picture;
	ColourbPremultiplied fitted_colour;
};
} // namespace Rml
