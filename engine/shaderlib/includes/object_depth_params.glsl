// The object block of a mesh depth program, depthParams.modelMatrix and mvpMatrix: per draw
// (u_vs_depthParams), or per instance in an automatically instanced batch (HAS_AUTO_INSTANCING,
// spec S55), the draw's matrices bit for bit (includes/object_params.glsl):
//   i_objectModel0..2  the model matrix's first three rows (its last row is 0 0 0 1)
//   i_objectMvp0..3    the model-view-projection matrix's columns
//   i_objectExtra      x the instance's LOD fade
// A batch has no lightVPMatrix: a program reads modelMatrix and mvpMatrix only.
// objectLodFade() is the draw's LOD cross-fade (S54: -1..1, 0 none).

#ifdef HAS_AUTO_INSTANCING
    in vec4 i_objectModel0;
    in vec4 i_objectModel1;
    in vec4 i_objectModel2;
    in vec4 i_objectMvp0;
    in vec4 i_objectMvp1;
    in vec4 i_objectMvp2;
    in vec4 i_objectMvp3;
    in vec4 i_objectExtra;

    struct ObjectDepthParams {
        mat4 modelMatrix;
        mat4 mvpMatrix;
    };

    ObjectDepthParams objectDepthParams(){
        ObjectDepthParams params;
        params.modelMatrix = mat4(
            vec4(i_objectModel0.x, i_objectModel1.x, i_objectModel2.x, 0.0),
            vec4(i_objectModel0.y, i_objectModel1.y, i_objectModel2.y, 0.0),
            vec4(i_objectModel0.z, i_objectModel1.z, i_objectModel2.z, 0.0),
            vec4(i_objectModel0.w, i_objectModel1.w, i_objectModel2.w, 1.0));
        params.mvpMatrix = mat4(i_objectMvp0, i_objectMvp1, i_objectMvp2, i_objectMvp3);
        return params;
    }

    #define depthParams objectDepthParams()

    float objectLodFade(){
        return i_objectExtra.x;
    }
#else
    uniform u_vs_depthParams {
        mat4 modelMatrix;
        mat4 lightVPMatrix;
        mat4 mvpMatrix;
        // as includes/object_params.glsl's (S41; 0 in shadow maps)
        #ifdef PIXEL_SNAP
            vec4 pixelSnapDraw;
        #endif
    } depthParams;

    float objectLodFade(){
        return 0.0;
    }
#endif
