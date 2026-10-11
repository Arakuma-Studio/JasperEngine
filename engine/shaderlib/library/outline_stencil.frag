#version 450

// @shader mesh
// @name "Outline (stencil)"
// @category "Outline and stencil"
// @description "QuickOutline-style outline that never covers its object: an extra pass After a main material that writes stencil 1 over its whole silhouette (Outline (stencil) preset: stencil always, ref 1, write mask 1, pass and depth-fail replace). Draws the pushed-out mesh where stencil bit 1 is not set. Visibility Always draws it through walls (give the object a sort priority above its occluders, as the preset does, so they draw first); Visible only depth-tests it. Width in pixels or world units, as Outline. Uses stencil bit 0x01. No shadows."
// @require smoothnormals
// @require uv1 feature=TEXTURE
// @require rawcolor feature=VERTEX_COLOR_WIDTH
// @state blend=opaque cull=off zwrite=off ztest=always shadows=off
// @state stencil=notequal ref=1 readmask=1 writemask=0
// @state[VISIBILITY_VISIBLE_ONLY] ztest=lequal zwrite=on
// @keywords WIDTH_MODE "Width in" SCREEN WORLD
// @keywords VISIBILITY "Visibility" ALWAYS VISIBLE_ONLY
// @property outlineColor hdrcolor (1, 0.6, 0.1, 1) "Colour"
// @property screenWidth range(0, 32) 4 "Width (pixels)" feature=WIDTH_MODE_SCREEN
// @property worldWidth float 0.04 "Width (world units)" feature=WIDTH_MODE_WORLD
// @property distanceScale range(0, 1) 1 "Distance scaling (1: constant on-screen size beyond the reference distance)" feature=DISTANCE_SCALE
// @property referenceDistance float 10 "Reference distance (world width applies up to it)" feature=DISTANCE_SCALE
// @property maxWidth float 0.25 "Maximum width (world units)" feature=DISTANCE_SCALE
// @property textureTiling vector4 (1, 1, 0, 0) "Texture tiling (xy) and offset (zw)" feature=TEXTURE
// @texture u_outlineTexture white "Tint texture" feature=TEXTURE
// @feature DISTANCE_SCALE "Grow with distance (world width)"
// @feature TEXTURE "Tint texture"
// @feature VERTEX_COLOR_WIDTH "Width times vertex colour red"

// The main material marks its whole silhouette, visible or hidden, with stencil bit 0x01
// (depth-fail replace marks the hidden parts too, so a through-walls outline stays an
// outline instead of filling the hidden shape); this pass fills the hull outside it, both
// faces (an open mesh outlines from behind too). Always: depth test off and no depth
// write, an overlay over whatever drew before it in the queue (hence the sort priority);
// Visible only: depth tested and written like a hull. The bit stays set until the camera
// pass ends, so outlined objects never outline over one another.

#include "library/outline.glsl"
