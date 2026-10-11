#version 450
// @shader mesh
// @name "Fixture lit outline"
// @description "The fixture outline lit by the object's pixel lights (the extra pass gets the object's light list when its shader asks for lighting)."
// @lighting pixel
// @require smoothnormals feature=!RAW_NORMALS
// @require normals feature=RAW_NORMALS
// @state cull=front blend=opaque
// @property outlineColor color (1, 0.85, 0.3, 1) "Colour"
// @property width float 0.05 "Width (object units)"
// @property offset vector3 (0, 0, 0) "Offset (object units)"
// @feature RAW_NORMALS "Extrude along the mesh normals"

#include "outline_fs.glsl"
