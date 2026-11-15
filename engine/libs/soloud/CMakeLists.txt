# SoLoud - https://github.com/jarikomppa/soloud/
# Local patches (keep them when updating SoLoud): Bus has a destructor that stops its voices
# (src/core/soloud_bus.cpp: AudioSource's own destructor ran after the Bus part was destroyed, and
# ~BusInstance read the destroyed Bus); AlignedFloatBuffer::clear() leaves an uninitialised buffer
# alone (src/core/soloud.cpp: the nosound backend cleared it with memset of a null pointer);
# postinit_internal frees the resample arrays of an earlier init (src/core/soloud.cpp: re-inits leaked them).
# SoLoud's own stb_vorbis.c is compiled here, not the stock copy (stb has none): it reads through SoLoud's
# File (soloud_file_hack_on.h), which WavStream hands it; the stock one took that File* for a FILE* and
# every streamed or compressed-in-memory Ogg aborted.
# WavStreamInstance::seek lands on its frame in every format with the decoder's own seek
# (src/audiosource/wav/soloud_wavstream.cpp: it tested a union member set for every format and handed
# WAV, FLAC and MP3 decoders to stb_vorbis_seek at each loop wrap; Ogg dropped the sought frame), and a
# streamed Ogg ends at its last granule (getAudio played the frames stb_vorbis decodes past it).
# Wav's loaders stay within the clip (src/audiosource/wav/soloud_wav.cpp: loadogg wrote the frames
# stb_vorbis decodes past the last granule beyond mData; loadwav and loadflac copied whole blocks after
# a short read, stale or uninitialised floats).
# A voice that ends plays its last block to the end (src/core/soloud.cpp mixBus_internal: it was stopped
# at the end of the mix call that read that block, dropping what the block still held).
# A bus never reads ahead (src/core/soloud.cpp mixBus_internal: a voice at step 1 with autostop off, the
# engine's buses, reads only the frames the call still needs; a voice started into a bus was mixed at the
# bus's next 512-frame block, up to 511 frames late, and seen ended up to a block early).
# A mix flushes denormals only while it runs (src/core/soloud.cpp mix_internal: flush-to-zero and
# denormals-are-zero were turned on once per process in the first thread that mixed and never restored,
# so a main thread that mixed, on the null or deterministic driver, flushed denormals from then on).
# soloud.h declares OutputDebugStringA, which SOLOUD_ASSERT calls on MSVC, instead of including
# <windows.h> (include/soloud.h): the engine's public headers include soloud.h, and windows.h's macros
# (near, far, min, max, ERROR, TRANSPARENT...) broke the engine sources that include them.

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
    # SoLoud's copy, which reads through SoLoud's File with soloud_file_hack_on.h: WavStream hands
    # stb_vorbis a SoLoud File*, which a stock copy takes for a FILE*
    src/audiosource/wav/stb_vorbis.c

    src/backend/miniaudio/soloud_miniaudio.cpp
    src/backend/nosound/soloud_nosound.cpp
    # the null driver: the editor's Recorder mixes offline in step with the game (S27 R29)
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
target_compile_definitions(soloud PRIVATE WITH_NULL)

if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
	target_compile_options(soloud PRIVATE -pthread)
endif()

target_link_libraries(
        soloud

        PUBLIC

        stb
        ${SOLOUD_PLATFORM_LIBS}
)
