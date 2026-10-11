#version 450

// @shader mesh
// @name "Outline"
// @category "Outline and stencil"
// @description "Inverted-hull outline: the mesh pushed out along its smoothed normal (closed at hard edges) and drawn with its front faces culled, in one colour. Use it as an opaque material's extra pass, order After (Outline (hull) preset): the object's depth hides the inner hull. Not for blended objects: the hull's back faces fill the whole shape, which a blended object (no depth write) shows through Before and is covered by After; the Outline (hull) preset gives a blended material Outline (stencil), Visible only, instead. Width in pixels (constant at any distance, perspective or orthographic) or in world units (optionally growing with distance, capped). On hard-edged meshes the band beside a sharp corner is thinner than the width (down to width/sqrt2 in pixels, width/sqrt3 in world units, on a box): the hull follows the corner's smoothed normal so it stays closed. Works with skinning, morph targets and instancing; for Sprites use the Sprite shader's Outline feature instead (a flat quad has no hull). No shadows."
// @require smoothnormals
// @require uv1 feature=TEXTURE
// @require rawcolor feature=VERTEX_COLOR_WIDTH
// @state blend=opaque cull=front zwrite=on shadows=off
// @keywords WIDTH_MODE "Width in" SCREEN WORLD
// @property outlineColor hdrcolor (0.02, 0.02, 0.02, 1) "Colour"
// @property screenWidth range(0, 32) 3 "Width (pixels)" feature=WIDTH_MODE_SCREEN
// @property worldWidth float 0.03 "Width (world units)" feature=WIDTH_MODE_WORLD
// @property distanceScale range(0, 1) 1 "Distance scaling (1: constant on-screen size beyond the reference distance)" feature=DISTANCE_SCALE
// @property referenceDistance float 10 "Reference distance (world width applies up to it)" feature=DISTANCE_SCALE
// @property maxWidth float 0.25 "Maximum width (world units)" feature=DISTANCE_SCALE
// @property textureTiling vector4 (1, 1, 0, 0) "Texture tiling (xy) and offset (zw)" feature=TEXTURE
// @texture u_outlineTexture white "Tint texture" feature=TEXTURE
// @feature DISTANCE_SCALE "Grow with distance (world width)"
// @feature TEXTURE "Tint texture"
// @feature VERTEX_COLOR_WIDTH "Width times vertex colour red"

// The colour may exceed 1 (HDR) and has straight alpha; with an alpha blend set on the pass
// the outline fades. The hull's back faces lie behind the object, so only the band around
// its silhouette shows. Order After an opaque object (its depth rejects the hidden hull
// early). Never on a blended object, in either order: it writes no depth, so the hull's back
// faces fill its whole shape (seen through it Before, covering it After); blended objects
// get library:outline_stencil, Visible only, over a stencil mark of their silhouette (the
// Outline (hull) preset picks it). DISTANCE_SCALE applies to the world width only. See
// library/outline_vs.glsl for the width maths, the instance fade and hard edges.

#include "library/outline.glsl"
