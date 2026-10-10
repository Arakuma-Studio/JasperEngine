#version 450

// SHINE lays its band over the element's own space
#ifdef SHINE
    #define UI_LOCAL_POSITION
#endif

#include "includes/ui_vs.glsl"
