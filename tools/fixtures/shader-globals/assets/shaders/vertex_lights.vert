#version 450

// Shader globals fixture: the game's custom vertex lighting (Unity VertexLightsUpdater.cs and
// the Amplify shaders reading _VertexLightColor0..3, _LightPositionX/Y/Z and _LightRadius): four
// point lights, their x, y, z and radius packed one light per component, set every frame by
// scripts/VertexLightsUpdater.lua through Shader.setGlobal*, read in the vertex stage.
#define SURFACE_VERTEX
#define SURFACE_NO_VERTEX_COLOR
#include "includes/surface_vs.glsl"

uniform u_vs_globals {
    vec4 _LightPositionX;
    vec4 _LightPositionY;
    vec4 _LightPositionZ;
    vec4 _LightRadius;
    vec4 _VertexLightColor0;
    vec4 _VertexLightColor1;
    vec4 _VertexLightColor2;
    vec4 _VertexLightColor3;
} globals;

out vec3 v_light;

void surfaceVertex(vec3 worldPosition, vec3 worldNormal, vec4 color, vec2 uv) {
    vec4 colors[4] = vec4[4](globals._VertexLightColor0, globals._VertexLightColor1,
                             globals._VertexLightColor2, globals._VertexLightColor3);
    v_light = vec3(0.0);
    for (int i = 0; i < 4; i++) {
        vec3 toLight = vec3(globals._LightPositionX[i], globals._LightPositionY[i], globals._LightPositionZ[i]) - worldPosition;
        float fade = clamp(1.0 - length(toLight) / max(globals._LightRadius[i], 0.0001), 0.0, 1.0);
        v_light += colors[i].rgb * fade * max(dot(worldNormal, normalize(toLight)), 0.0);
    }
}
