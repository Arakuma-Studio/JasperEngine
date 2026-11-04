# Stage the starter library next to the editor: the
# destination is removed first, so a clip dropped from the library never lingers in a build
# tree (where `sound-library verify` would report it), then editor/resources/sound-library/library
# is copied as it is and the stamp written. A tree without a library yet (before it is curated)
# stages nothing: the editor then reports the library as not found.
if(NOT DEFINED SOUND_LIBRARY_SOURCE OR NOT DEFINED SOUND_LIBRARY_DEST OR NOT DEFINED SOUND_LIBRARY_STAMP)
    message(FATAL_ERROR "SOUND_LIBRARY_SOURCE, SOUND_LIBRARY_DEST and SOUND_LIBRARY_STAMP are required")
endif()
if(SOUND_LIBRARY_DEST STREQUAL "" OR NOT IS_ABSOLUTE "${SOUND_LIBRARY_DEST}")
    message(FATAL_ERROR "SOUND_LIBRARY_DEST must be an absolute path, not '${SOUND_LIBRARY_DEST}'")
endif()

file(REMOVE_RECURSE "${SOUND_LIBRARY_DEST}")
if(EXISTS "${SOUND_LIBRARY_SOURCE}/manifest.json")
    file(MAKE_DIRECTORY "${SOUND_LIBRARY_DEST}")
    file(COPY "${SOUND_LIBRARY_SOURCE}/" DESTINATION "${SOUND_LIBRARY_DEST}" PATTERN ".gitattributes" EXCLUDE)
endif()
file(TOUCH "${SOUND_LIBRARY_STAMP}")
