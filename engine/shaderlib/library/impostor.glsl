// library:impostor's billboard, shared by its colour (impostor.vert) and depth (impostor_depth.vert)
// programs (T5 F7, R16). One unit quad per instance, turning about Y to face the camera. The view of
// the atlas comes from the camera's direction and the instance's yaw, through a polynomial arctangent
// so no target's libm decides which view shows.

out vec2 v_impostorUv0;
out vec2 v_impostorUv1;
out float v_impostorBlend;
// the view's frame in terrain-local space: what the normal atlas's X and Z mean
out vec3 v_impostorRight;
out vec3 v_impostorForward;

// atan2 by a polynomial on [0, 1], within 3e-4 rad: y / x is folded into the first octant
float impostorAtan2(float y, float x){
    float ax = abs(x);
    float ay = abs(y);
    float a = min(ax, ay) / max(max(ax, ay), 1e-6);
    float s = a * a;
    float r = ((-0.0464964749 * s + 0.15931422) * s - 0.327622764) * s * a + a;
    if (ay > ax) r = 1.57079637 - r;
    if (x < 0.0) r = 3.14159274 - r;
    if (y < 0.0) r = -r;
    return r;
}

vec2 impostorCell(float view, float views){
    float columns = views <= 4.0 ? 2.0 : 4.0;
    float rows = ceil(views / columns);
    return vec2(mod(view, columns) / columns, floor(view / columns) / rows);
}

// position: the quad's corner in -0.5..0.5, x across and y up. Returns it in terrain-local space.
vec3 impostorPlace(vec2 corner, vec2 uv){
    vec3 origin = i_matrix_col4.xyz;
    float width = foliageParams.impostor.w * i_packed0.w;
    float height = foliageParams.impostorSize.x * i_packed0.w * i_packed3.x * (255.0 / 128.0);

    vec2 toEye = foliageParams.eye.xz - origin.xz;
    float toEyeLength = length(toEye);
    vec2 facing = toEyeLength > 1e-4 ? toEye / toEyeLength : vec2(0.0, 1.0);
    vec3 right = vec3(facing.y, 0.0, -facing.x);

    // the instance's yaw out of its quaternion: twist about Y
    float yaw = 2.0 * impostorAtan2(i_packed1.y, i_packed1.w);
    float views = max(foliageParams.heightRange.w, 1.0);
    float angle = (impostorAtan2(facing.x, facing.y) - yaw) / 6.28318530718;
    float step = fract(angle) * views;
    float first = floor(step);
    float blend = foliageParams.heightRange.z > 0.5 ? step - first : 0.0;
    if (foliageParams.heightRange.z <= 0.5) first = floor(step + 0.5);

    float columns = views <= 4.0 ? 2.0 : 4.0;
    float rows = ceil(views / columns);
    vec2 cellSize = vec2(1.0 / columns, 1.0 / rows);
    v_impostorUv0 = impostorCell(mod(first, views), views) + uv * cellSize;
    v_impostorUv1 = impostorCell(mod(first + 1.0, views), views) + uv * cellSize;
    v_impostorBlend = blend;
    v_impostorRight = right;
    v_impostorForward = vec3(facing.x, 0.0, facing.y);

    vec3 center = origin + foliageParams.impostor.xyz * vec3(i_packed0.w, i_packed0.w * i_packed3.x * (255.0 / 128.0), i_packed0.w);
    vec3 placed = center + right * corner.x * width + vec3(0.0, 1.0, 0.0) * corner.y * height;
    #ifdef USE_FOLIAGE_WIND
        // only the whole-tree term: the block's flutter is 0 for an impostor (T5 R19)
        placed = foliageWindSway(placed, origin, corner.y + 0.5, 0.0);
    #endif
    return placed;
}
