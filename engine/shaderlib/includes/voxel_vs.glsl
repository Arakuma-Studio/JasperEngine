// A Voxel Volume's vertex (T6 R49). Slots are unpacked with integer ops on the attribute's value,
// never on a float's bits (D13), and passed flat: all three vertices of a triangle share one cell's set.

in vec2 a_voxelSlots;
in vec4 a_voxelWeights;

out vec3 v_voxelPosition;       // the volume's frame
out vec3 v_voxelNormal;         // the volume's frame
out vec4 v_voxelWeights;
flat out vec4 v_voxelSlots;
// the volume's axes in the space the pass shades in, to bring detail normals there
flat out vec3 v_voxelAxisX;
flat out vec3 v_voxelAxisY;

void setVoxelVaryings(vec3 position, vec3 normal, mat4 normalMatrix){
    v_voxelPosition = position;
    v_voxelNormal = normal;
    v_voxelWeights = a_voxelWeights;
    int code = int(a_voxelSlots.x * 65535.0 + 0.5);
    v_voxelSlots = vec4(float(code & 15), float((code >> 4) & 15), float((code >> 8) & 15), float((code >> 12) & 15));
    v_voxelAxisX = normalize(mat3(normalMatrix) * vec3(1.0, 0.0, 0.0));
    v_voxelAxisY = normalize(mat3(normalMatrix) * vec3(0.0, 1.0, 0.0));
}
