Shader "Custom/By Properties" {
  Properties { _SrcBlend ("Src", Float) = 5 _DstBlend ("Dst", Float) = 10 }
  SubShader { Pass { Blend [_SrcBlend] [_DstBlend] } }
}
