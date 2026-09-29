// Octahedral normal encoding (Cigolle et al. 2014). Maps a unit vector to a
// vec2 in [0,1], suitable for storing a normal in two 8-bit color channels.
// Used by the G-buffer (gbuffer.frag writes, ssr.frag reads).

vec2 octWrap(vec2 v){
    return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

// encode a normalized vector -> [0,1]^2
vec2 octEncode(vec3 n){
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    n.xy = (n.z >= 0.0) ? n.xy : octWrap(n.xy);
    return n.xy * 0.5 + 0.5;
}

// decode [0,1]^2 -> normalized vector
vec3 octDecode(vec2 f){
    f = f * 2.0 - 1.0;
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
