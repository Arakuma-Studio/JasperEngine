#pragma once

#include "Header.h"
#include "Texture.h"
#include "Types.h"

namespace Rml {

class Element;

/**
    Fork (phase 3, p3-paint): the images of url() background and mask layers. Without a handler, or when it declines,
    the fork loads the url as a raster texture through the render manager. An application sets one to draw SVG images
    at the size they are drawn at, to tint them, or to choose the texture's sampling.
 */
class RMLUICORE_API ImageSourceHandler {
public:
	virtual ~ImageSourceHandler();

	struct Image {
		Texture texture;                  // to draw it at 'draw_size' (none when only the size was asked for)
		Vector2f intrinsic_size;          // in px; 0 in an axis: the image has no intrinsic size in it
		ColourbPremultiplied colour{255}; // multiplied with the texture (the element's draw opacity is applied after)
	};

	/// The image of 'url' (relative to 'base_path', the path of the document or style sheet that names it) for the
	/// element. 'draw_size' is the size one tile of it is drawn at, or zero when only its intrinsic size is needed.
	/// @return False to let the fork load the url as a raster texture.
	virtual bool GetImage(Element* element, const String& url, const String& base_path, Vector2f draw_size, Image& out) = 0;
};

/// Sets the handler of url() layer images (nullptr: none). Not owned; it must outlive its use.
RMLUICORE_API void SetImageSourceHandler(ImageSourceHandler* handler);
RMLUICORE_API ImageSourceHandler* GetImageSourceHandler();

} // namespace Rml
