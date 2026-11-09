# The Voxel Volume's human editor (docs/specs/T6-voxel-volumes.md R59-R65): the create dialog, the
# panel and its brushes, the Materials tab, the Slice view, overlays and stats, the Inspector. Its
# own list, so the streams adding editor files do not meet here.

list(APPEND JASPERENGINE_EDITOR_SOURCES
    ${EDITOR_DIR}/voxel/VoxelSliceRender.cpp
    ${EDITOR_DIR}/voxel/VoxelOverlay.cpp
    ${EDITOR_DIR}/voxel/VoxelBrush.cpp
    ${EDITOR_DIR}/voxel/VoxelBrushTool.cpp
    ${EDITOR_DIR}/voxel/VoxelCreate.cpp
    ${EDITOR_DIR}/voxel/VoxelGenerationTask.cpp
    ${EDITOR_DIR}/voxel/VoxelMaterials.cpp
    ${EDITOR_DIR}/voxel/VoxelProblems.cpp
    ${EDITOR_DIR}/voxel/VoxelSceneOverlay.cpp
    ${EDITOR_DIR}/voxel/VoxelVolumeOps.cpp
    ${EDITOR_DIR}/window/VoxelEditWindow.cpp
    ${EDITOR_DIR}/window/VoxelSliceView.cpp
    ${EDITOR_DIR}/window/VoxelCreateDialog.cpp
    ${EDITOR_DIR}/window/InspectorVoxels.cpp
)
