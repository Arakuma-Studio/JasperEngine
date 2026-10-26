# Sources of the HTML UI widgets (engine/core/htmlui/widgets, S17: virtual views, tooltips,
# drag and drop, Markdown), included by engine/CMakeLists.txt. Globbed per directory like the
# compatibility layer (the Xcode project lists them explicitly: tools/htmlui/xcode_sync_widgets.py).

set(HTMLUI_WIDGETS_DIR ${CMAKE_CURRENT_LIST_DIR})

file(GLOB HTMLUI_WIDGETS_SRCS CONFIGURE_DEPENDS
    ${HTMLUI_WIDGETS_DIR}/*.cpp
    ${HTMLUI_WIDGETS_DIR}/Markdown/*.cpp
)
list(SORT HTMLUI_WIDGETS_SRCS)
