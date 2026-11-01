Shader "Custom/Premultiplied Glow" {
  SubShader { Pass {
    // a comment: Blend One One is not this shader
    blend one oneminussrcalpha
  } }
}
