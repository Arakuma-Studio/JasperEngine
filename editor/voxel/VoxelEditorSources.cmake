# The Voxel Volume's human editor (docs/specs/T6-voxel-volumes.md R59-R65): the create dialog, the
# panel and its brushes, the Materials tab, the Slice view, overlays and stats, the Inspector. Its
# own list, so the streams adding editor files do not meet here.

list(APPEND JASPERENGINE_EDITOR_SOURCES
    ${EDITOR_DIR}/voxel/VoxelSliceRender.cpp
    ${EDITOR_DIR}/voxel/VoxelOverlay.cpp
)
