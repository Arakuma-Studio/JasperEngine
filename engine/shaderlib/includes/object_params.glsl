// The object block of a mesh colour program, pbrParams.modelMatrix, normalMatrix and mvpMatrix:
// per draw (u_vs_pbrParams), or per instance in an automatically instanced batch
// (HAS_AUTO_INSTANCING, spec S55: draws of one mesh, material and pipeline drawn as one). A
// shader reads pbrParams the same way in both; the instance's matrices are the draw's, bit for
// bit, so a batch draws the image its draws would have:
//   i_objectModel0..2   the model matrix's first three rows (its last row is 0 0 0 1)
//   i_objectNormal0..2  the normal matrix's first three columns (xyz: what a direction reads);
//                       i_objectNormal0.w the instance's LOD fade
//   i_objectMvp0..3     the model-view-projection matrix's columns
// objectLodFade() is the draw's LOD cross-fade (S54: -1..1, 0 none). It is a macro, not a
// function: glslang numbers uniform blocks in the order the reachable code first reads them, and a
// function defined here, called only by the fade variant, would read pbrParams before the
// includes below read theirs (a skinned variant's u_vs_skinning), so the fade variant's block
// slots would differ from its base's and the runtime would refuse it (sameLodFadeBindings).

#ifdef HAS_AUTO_INSTANCING
    in vec4 i_objectModel0;
    in vec4 i_objectModel1;
    in vec4 i_objectModel2;
    in vec4 i_objectNormal0;
    in vec4 i_objectNormal1;
    in vec4 i_objectNormal2;
    in vec4 i_objectMvp0;
    in vec4 i_objectMvp1;
    in vec4 i_objectMvp2;
    in vec4 i_objectMvp3;

    struct ObjectParams {
        mat4 modelMatrix;
        mat4 normalMatrix;
        mat4 mvpMatrix;
    };

    ObjectParams objectParams(){
        ObjectParams params;
        params.modelMatrix = mat4(
            vec4(i_objectModel0.x, i_objectModel1.x, i_objectModel2.x, 0.0),
            vec4(i_objectModel0.y, i_objectModel1.y, i_objectModel2.y, 0.0),
            vec4(i_objectModel0.z, i_objectModel1.z, i_objectModel2.z, 0.0),
            vec4(i_objectModel0.w, i_objectModel1.w, i_objectModel2.w, 1.0));
        params.normalMatrix = mat4(
            vec4(i_objectNormal0.xyz, 0.0),
            vec4(i_objectNormal1.xyz, 0.0),
            vec4(i_objectNormal2.xyz, 0.0),
            vec4(0.0, 0.0, 0.0, 1.0));
        params.mvpMatrix = mat4(i_objectMvp0, i_objectMvp1, i_objectMvp2, i_objectMvp3);
        return params;
    }

    #define pbrParams objectParams()

    #define objectLodFade() (i_objectNormal0.w)
#else
    uniform u_vs_pbrParams {
        mat4 modelMatrix;
        mat4 normalMatrix;
        mat4 mvpMatrix;
    } pbrParams;

    #define objectLodFade() (pbrParams.normalMatrix[0][3])
#endif
