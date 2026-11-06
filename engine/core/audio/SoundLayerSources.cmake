# The sound layer's runtime sources (S34 module soundlayer, JASPERENGINE_SOUND_LAYER): one list the
# specs that add to the module can each merge into (S36's clip import settings, S37's events and
# triggers, S39's music, S40's cues).
set(JASPERENGINE_SOUND_LAYER_SRCS
    core/audio/AudioImport.cpp
    core/audio/ClipSource.cpp
    core/audio/MusicPlayer.cpp
    core/audio/MusicSequenceSource.cpp
    core/audio/SoundEvent.cpp
    core/audio/SoundEventIO.cpp
    core/audio/SoundEventLibrary.cpp
    core/audio/TimelineAudio.cpp
    core/object/sound/Audio.cpp
    core/object/sound/SoundHandle.cpp
)
