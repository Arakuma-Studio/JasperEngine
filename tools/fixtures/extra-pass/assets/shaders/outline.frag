#version 450
// @shader mesh
// @name "Fixture outline"
// @description "Inverted hull for the extra-pass fixture: the mesh pushed out along its smoothed normal and drawn with its front faces culled, in one colour. RAW_NORMALS extrudes along the mesh normals instead (gaps at hard edges)."
// @require smoothnormals feature=!RAW_NORMALS
// @require normals feature=RAW_NORMALS
// @state cull=front blend=opaque
// @property outlineColor color (0.02, 0.02, 0.02, 1) "Colour"
// @property width float 0.05 "Width (object units)"
// @property offset vector3 (0, 0, 0) "Offset (object units)"
// @feature RAW_NORMALS "Extrude along the mesh normals"

#include "outline_fs.glsl"
