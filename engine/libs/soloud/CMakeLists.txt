# SoLoud - https://github.com/jarikomppa/soloud/
# Local patches (keep them when updating SoLoud): Bus has a destructor that stops its voices
# (src/core/soloud_bus.cpp: AudioSource's own destructor ran after the Bus part was destroyed, and
# ~BusInstance read the destroyed Bus); AlignedFloatBuffer::clear() leaves an uninitialised buffer
# alone (src/core/soloud.cpp: the nosound backend cleared it with memset of a null pointer);
# postinit_internal frees the resample arrays of an earlier init (src/core/soloud.cpp: re-inits leaked them).

include_directories ("${CMAKE_CURRENT_SOURCE_DIR}/include")

include_directories ("${CMAKE_CURRENT_SOURCE_DIR}/../stb")

set(SOLOUD_PLATFORM_LIBS)

add_library(

    soloud

    STATIC

    src/core/soloud_audiosource.cpp
    src/core/soloud_bus.cpp
    src/core/soloud_core_3d.cpp
    src/core/soloud_core_basicops.cpp
    src/core/soloud_core_faderops.cpp
    src/core/soloud_core_filterops.cpp
    src/core/soloud_core_getters.cpp
    src/core/soloud_core_setters.cpp
    src/core/soloud_core_voicegroup.cpp
    src/core/soloud_core_voiceops.cpp
    src/core/soloud_fader.cpp
    src/core/soloud_fft_lut.cpp
    src/core/soloud_fft.cpp
    src/core/soloud_file.cpp
    src/core/soloud_filter.cpp
    src/core/soloud_misc.cpp
    src/core/soloud_queue.cpp
    src/core/soloud_thread.cpp
    src/core/soloud.cpp

    src/audiosource/wav/dr_impl.cpp
    src/audiosource/wav/soloud_wav.cpp
    src/audiosource/wav/soloud_wavstream.cpp

    src/backend/miniaudio/soloud_miniaudio.cpp
    src/backend/nosound/soloud_nosound.cpp
    src/backend/null/soloud_null.cpp

    src/filter/soloud_bassboostfilter.cpp
    src/filter/soloud_biquadresonantfilter.cpp
    src/filter/soloud_dcremovalfilter.cpp
    src/filter/soloud_duckfilter.cpp
    src/filter/soloud_echofilter.cpp
    src/filter/soloud_eqfilter.cpp
    src/filter/soloud_fftfilter.cpp
    src/filter/soloud_flangerfilter.cpp
    src/filter/soloud_freeverbfilter.cpp
    src/filter/soloud_lofifilter.cpp
    src/filter/soloud_robotizefilter.cpp
    src/filter/soloud_waveshaperfilter.cpp
)

if(APPLE)
    set_source_files_properties(src/backend/miniaudio/soloud_miniaudio.cpp PROPERTIES XCODE_EXPLICIT_FILE_TYPE "sourcecode.cpp.objcpp")
    
    find_library(AV_FOUNDATION_LIBRARY AVFoundation)
    find_library(AUDIOTOOLBOX_LIBRARY AudioToolbox)

    list(APPEND SOLOUD_PLATFORM_LIBS
        ${AV_FOUNDATION_LIBRARY} ${AUDIOTOOLBOX_LIBRARY}
    )
endif()

# no device (CI, servers, headless boxes): NoSound mixes in its own thread with no output,
# Null on the main thread (AudioSystem; audio-mixer spec A14)
target_compile_definitions(soloud PRIVATE WITH_NOSOUND WITH_NULL)

find_package(Threads REQUIRED)
target_link_libraries(soloud PRIVATE Threads::Threads)

if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
	target_compile_options(soloud PRIVATE -pthread)
endif()

target_link_libraries(
        soloud

        PUBLIC

        stb
        ${SOLOUD_PLATFORM_LIBS}
)
