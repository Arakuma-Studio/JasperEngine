# Copy the editor's engine SDK without local build products. Gradle/CMake output
# under workspaces/androidstudio can be several gigabytes and is not SDK input.
if(NOT DEFINED ENGINE_SOURCE OR NOT DEFINED ENGINE_DEST)
    message(FATAL_ERROR "ENGINE_SOURCE and ENGINE_DEST are required")
endif()

# In an in-tree editor build this directory is also CMake's engine binary
# directory, so keep its generated build files intact.
file(MAKE_DIRECTORY "${ENGINE_DEST}")
# Older copies may already contain Android build output. Remove only these
# generated subtrees; the engine CMake binary directory must remain intact.
file(REMOVE_RECURSE
     "${ENGINE_DEST}/workspaces/androidstudio/build"
     "${ENGINE_DEST}/workspaces/androidstudio/.gradle"
     "${ENGINE_DEST}/workspaces/androidstudio/app/build"
     "${ENGINE_DEST}/workspaces/androidstudio/app/.cxx")
file(COPY "${ENGINE_SOURCE}/" DESTINATION "${ENGINE_DEST}"
     PATTERN ".cxx" EXCLUDE
     PATTERN ".gradle" EXCLUDE
     PATTERN "build" EXCLUDE
     PATTERN ".git" EXCLUDE)
