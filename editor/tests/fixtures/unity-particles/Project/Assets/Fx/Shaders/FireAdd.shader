Shader "Custom/Fire Additive (Soft)" {
  Properties { _MainTex ("Texture", 2D) = "white" {} _TintColor ("Tint", Color) = (0.5,0.5,0.5,0.5) _InvFade ("Soft", Float) = 1 }
  SubShader {
    Tags { "Queue"="Transparent" }
    Pass {
      Blend SrcAlpha One
      ZWrite Off
    }
  }
}
