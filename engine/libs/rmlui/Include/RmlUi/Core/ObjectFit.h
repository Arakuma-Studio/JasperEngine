#pragma once

#include "Header.h"
#include "StyleTypes.h"
#include "Types.h"

namespace Rml {

/// Where a replaced element draws its content (CSS Images 3, 'object-fit' and 'object-position'; phase 3, p3-paint).
struct ObjectFitRect {
	Rectanglef rect; // The drawn part, relative to the content box: the content box at most (cropped, no overdraw).
	Rectanglef uv;   // The part of the object it shows, in fractions of the object (0 to 1).
	Vector2f size;   // The size the whole object is drawn at (before cropping), e.g. to rasterize a vector image.
};

/// Places an object of 'intrinsic_size' in a content box of 'box_size': 'fill' stretches it over the box, 'contain' and
/// 'cover' scale it keeping its ratio to fit inside or to cover the box, 'none' keeps its size, 'scale-down' is the
/// smaller of 'none' and 'contain'; the position is a length or percentage of the free space (box less object) per axis.
/// An object without an intrinsic size (0 in an axis) fills the box. Returns false when nothing of it is visible.
RMLUICORE_API bool ComputeObjectFitRect(Vector2f box_size, Vector2f intrinsic_size, Style::ObjectFit fit, Style::LengthPercentage position_x,
	Style::LengthPercentage position_y, ObjectFitRect& out);

} // namespace Rml
