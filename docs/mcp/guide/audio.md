# Audio: the mixer, sources, one-shots, fades

Every Audio Source plays through a **mixer group**: a bus with a volume in dB, mute, solo, up to 8
effects and a live meter, inside a tree under **Master**. The project has one **Audio Mixer** asset
(`.mixer`, JSON) chosen in Project Settings > Audio (`audio.mixer`); without one every sound plays
through an implicit Master at 0 dB, exactly as before. A project created by this editor starts with
`audio/Main.mixer`: a Master `limiter` (-1 dB, 1.5 ms, true peak), and the `linear` output stage
below; a project from before keeps what it had. Scripts change the mixer through **exposed
parameters** (`AudioMixer.setFloat("MusicVolume", -80)`, Unity's names) and **snapshots**
(`AudioMixer.transitionTo("Gameplay", 0.65)`). It all works the same in editor Play, in every
exported build and on machines without an audio device (CI): the mixing goes on without output, so
meters, `isPlaying`, events and fades are real. Lua reference: `jasper://docs/lua-api` 2.9 and 4.12.

## 1. The loop for agents

1. Build the mixer in one call (it creates `audio/Main.mixer` in the assets folder and makes it the project's):

```json
{"action": "edit", "ops": [
  {"op": "add_group", "name": "Music"},
  {"op": "add_group", "name": "SFX"},
  {"op": "add_effect", "group": "Music", "type": "lowpass", "params": {"cutoff": 22000}},
  {"op": "add_effect", "group": "Music", "type": "highpass", "params": {"cutoff": 10}},
  {"op": "expose", "name": "MusicVolume", "group": "Music", "volume": true, "remember": true},
  {"op": "expose", "name": "SFXVolume", "group": "SFX", "volume": true, "remember": true},
  {"op": "expose", "name": "lowPassCutoff", "group": "Music", "param": "cutoff", "effect": "lowpass"},
  {"op": "expose", "name": "highPassCutoff", "group": "Music", "param": "cutoff", "effect": "highpass"},
  {"op": "add_snapshot", "name": "Menu"},
  {"op": "add_snapshot", "name": "Gameplay", "values": {"lowPassCutoff": 525, "highPassCutoff": 430}},
  {"op": "set_start_snapshot", "name": "Menu"}
]}
```

2. Route the sources: `set_properties` on the Audio Source `{"outputGroup": "Music"}` (the group's
   name; `""` is Master). Music files of 1 MiB and more stream by themselves (`loadType` Auto).
3. Play, then check what is heard: `eval_lua "return AudioMixer.getMeter('Music')"` (dB; above -40
   is clearly audible, -80 is silence) or `get_play_state` (`audio.mixer_meters`: the groups above
   -60 dB, `audio.voices`). `audio_mixer {action: "get"}` in Play adds `volume_now`, `meter_db`,
   `value_now` and `set_by_script` per group and parameter.
4. Every `audio_mixer edit` is one undo step (`undo`, scope project) and is heard at once, in Play
   too (Unity's Edit in Play Mode is always on).

## 2. The `.mixer` file

```json
{
  "format": "jasperengine.mixer", "version": 1, "updateMode": "scaled",
  "groups": [
    {"id": "00000001", "name": "Master", "parent": null, "volume": 0, "effects": [{"id": "e1a2b3c4", "type": "limiter"}]},
    {"id": "9b51c0de", "name": "Music", "parent": "00000001", "volume": 0, "effects": [
       {"id": "c4f00d01", "type": "lowpass", "params": {"cutoff": 22000, "resonance": 1}}]},
    {"id": "5f000001", "name": "SFX", "parent": "00000001"}
  ],
  "exposed": [{"name": "MusicVolume", "target": "9b51c0de/volume", "remember": true},
              {"name": "lowPassCutoff", "target": "c4f00d01/cutoff"}],
  "snapshots": [{"name": "Normal", "values": {"9b51c0de/volume": 0, "c4f00d01/cutoff": 22000}}],
  "startSnapshot": "Normal"
}
```

- Exactly one group has `"parent": null`: Master. Group names and exposed names are unique.
- Ids (8 hex characters) keep sources and parameters attached when a group is renamed. A source
  stores the group's id and name: when the id is not in the mixer it finds the group by name, else
  it plays through Master (one warning).
- A **target** is `"<groupId>/volume"` or `"<effectId>/<param>"` (any parameter of the table below,
  `wet` included). Snapshot values missing a target take the group's or effect's value.
- Omitted fields take their defaults; the editor writes keys in this order, 2-space indent.
  Unknown effect types are kept (round trip) and do nothing, with a warning.
- Prefer `audio_mixer edit` over writing the file: it validates, names groups instead of ids and
  is one undo step. `write_file` of a `.mixer` works too (the project mixer reloads).

## 3. Groups, volume, mute, solo

- `volume` is in dB, -80..+20; -80 and below is silence. `AudioMixer.dbToLinear(db)` /
  `linearToDb(v)` convert (0 dB = 1, -6.02 dB = 0.5).
- **Mute** silences the group (its sources go on playing). **Solo** follows Unity: while any group
  is soloed, a group is heard only if it is soloed, above a soloed group or below one.
  **Bypass** (`bypassEffects`) turns its effects off.
- A group's level is heard through its parents: Master's fader moves everything.
- Meters: `AudioMixer.getMeter(group)` is the peak of the group's output over the last ~90 ms in
  dB (after its effects and fader, before its parent), -80 when silent.

## 4. Effects

Every effect also has `wet` (0..1, default 1; 0 = dry) and `bypass`. Values are clamped into range.

| `type` | Parameters (default, range) | Notes |
|---|---|---|
| `lowpass` | `cutoff` Hz (5000, 10..22000), `resonance` (1, 0.1..20) | at or above 0.45 x the sample rate (~19.8 kHz) it is bypassed: Unity's 22000 "off" behaves as off, and a sweep down from it does not click |
| `highpass` | `cutoff` Hz (500, 10..22000), `resonance` (1) | at or below 10 Hz it is bypassed |
| `bandpass` | `cutoff` Hz (1000), `resonance` (1) | |
| `echo` | `delay` s (0.3, 0.01..2), `decay` (0.5, 0..1), `filter` (0, 0..0.99) | a longer delay than the effect started with restarts its buffer |
| `reverb` | `roomSize` (0.5), `damp` (0.5), `width` (1), `freeze` (0 or 1) | Freeverb; cost: high (~1-2% of a mobile core) |
| `equalizer` | `band1`..`band8` gain (1, 0..4) | low to high; cost: high |
| `distortion` | `amount` (0.5, -1..1) | wave shaper |
| `flanger` | `delay` s (0.005, 0.001..0.1), `frequency` Hz (10, 0.01..100) | |
| `lofi` | `sampleRate` Hz (8000, 100..22000), `bitDepth` (8, 0.5..16) | |
| `bassboost` | `boost` (4, 0..10) | |
| `duck` | `sidechainGroup` (a group name), `level` (0.5, 0..1), `attack` s (0.1), `release` s (0.5) | lowers this group while the side-chain group plays (dialogue over music) |
| `limiter` | `threshold` dB (-1, -30..0), `release` s (0.1, 0.001..2), `lookahead` ms (0, 0..5), `truePeak` 0/1 (0) | peak limiter: a good last effect on Master. `lookahead` delays the group by that much so that the gain is down when a peak arrives; with `truePeak` 1 and at least 1.5 ms it holds `threshold` + 0.1 dB in true peak up to 24 dB over. Either one makes the file `version` 2. `audio_mixer get` gives `latency_ms` per group |

A limiter's parameters from a script:

```lua
local limiter = AudioMixer.getGroup("Master"):getEffect("limiter")
limiter:setParameter("truePeak", 1)
limiter:setParameter("lookahead", 1.5)
```

## 5. Exposed parameters, snapshots, time

- **Exposed parameters** name a group volume or an effect value for scripts:
  `AudioMixer.setFloat(name, value [, seconds [, opts]])`. A value set by a script **wins over
  snapshots** until `AudioMixer.clearFloat(name)`, which returns it at once to the snapshot's (or
  the mixer's) value. `getFloat(name)` is the value heard now; an unknown name returns false / nil
  and logs once (`hasParameter`, `getParameterNames` list them).
- **remember: true**: what scripts set is saved to UserSettings (`audio.mixer.<name>`) at the end
  of the frame and comes back when the mixer loads, before any sound plays. Settings toggles become
  zero code. `clearFloat` forgets it; `AudioMixer.saveParameters()` writes the ones scripts set now (and
  forgets the others), `loadParameters()` reads them again. `user_settings` lists them.
- **Snapshots** hold a value for every parameter. `transitionTo(snapshot, seconds)` moves every
  parameter there (volumes linearly in dB, frequencies along log2(Hz): the 22000 -> 525 Hz sweep is
  at ~3398 Hz half way, other values linearly); `transitionToSnapshots(names, weights, seconds)`
  goes to a weighted blend. The start snapshot applies when Play (or the exported game) starts;
  in Edit mode the mixer sounds as its faders.
- **Time**: transitions and timed `setFloat` run on **scaled time** (they hold at
  `Engine.timeScale = 0`, Unity's Normal update mode), unless the mixer's `updateMode` is
  `"unscaled"` or the call passes `{unscaled = true}`. Sounds themselves never slow down with the
  time scale (Unity).
- The mixer is engine-global: it, its snapshot and its transitions survive scene loads.

## 6. `audio_mixer` ops

`{"action": "get" | "edit", "mixer"?: "<path>.mixer", "ops": [...]}`. `mixer` defaults to the
project's (created on the first edit). Groups are named by name (or id); effects by `index`
(0-based) or `type` (the first of that type). The ops run in order; one failing op changes nothing.

| op | Fields |
|---|---|
| `add_group` | `name`, `parent` (default Master), `volume` |
| `remove_group` | `name` (its subgroups go too; their sources play through Master) |
| `set_group` | `name`, `volume`, `mute`, `solo`, `bypass`, `parent` (move), `new_name` |
| `add_effect` | `group`, `type`, `params` {name: value}, `wet`, `bypass`, `index` (insert position) |
| `set_effect` | `group`, `index` or `type`, `params`, `wet`, `bypass`, `move_to` (index) |
| `remove_effect` | `group`, `index` or `type` |
| `expose` | `name`, `group`, then `volume: true` or `param` (+ `effect`: index or type, when two effects have it), `remember` |
| `unexpose` / `set_exposed` | `name` / `name`, `remember`, `new_name` |
| `add_snapshot` | `name`, `values` (optional, see below), `start` (default: the first snapshot becomes the start), `live: true` (in Play: the values heard now instead of the faders) |
| `set_snapshot` | `name`, `values`, `capture: true` (every current value), `new_name` |
| `remove_snapshot` / `set_start_snapshot` | `name` (`set_start_snapshot` with `name: ""` clears it) |
| `set_mixer` | `update_mode`: `"scaled"` or `"unscaled"` |

Snapshot `values` keys: an exposed name (`"lowPassCutoff"`), `"<group>/volume"`
(`"Music/volume"`), `"<group>/<effect type or index>/<param>"` (`"Music/lowpass/cutoff"`), or a raw
target. A new snapshot holds every parameter's current value (the faders), with `values` on top.

Project Settings over MCP: `audio.mixer` (the .mixer path), `audio.max_real_voices` (32: voices
mixed at once, the quietest others are virtualised; at most 255 minus the mixer's groups), `audio.max_voices` (256: voices playing at
all), `audio.sample_rate` (`auto`, 22050, 44100, 48000), `audio.default_output_group` (the group
new Audio Sources get).

`audio.output_stage`: `roundoff` (a soft clipper: a small signal leaves 1.66 dB lower, a -1 dB peak
near -3.5 dBFS) or `linear` (samples pass unchanged, hard-clipped at 0 dBFS as a last guard: keep a
limiter on Master). `audio.ios_session_category`: `default` (ignores the
silent switch and stops the player's music), `ambient`, `solo_ambient`, `playback`; iOS only.
`audio.loudness_target_lufs` (-18), `audio.loudness_tolerance_lu` (2), `audio.true_peak_ceiling_dbtp`
(-1) only measure: they change no sample. Reading loudness: `jasper://docs/sound`.

## 7. Audio Sources

| Property | Default | Meaning |
|---|---|---|
| `outputGroup` | `""` (Master) | the mixer group, by name |
| `loadType` | Auto | `AudioLoadType.Auto` (files of 1 MiB and more stream, smaller are decoded), `DecompressOnLoad` (zero-latency effects), `CompressedInMemory` (the file's bytes, decoded while playing), `Streaming` (read from the file while playing: memory = nothing more). Per source (no clip import settings). A packed resource cannot stream: it is held compressed in memory |
| `priority` | 128 | 0 most important .. 256 least |
| `maxVoices` | 8 | voices of this source at once (its clip + one-shots) |
| `ignoreListenerPause` | false | plays on while `AudioListener.pause` is set |
| `pitch` | 1 | = `speed` |

- **Many sources, one clip**: they play at once. `play()` on a playing source starts it over.
- **One-shots**: `sound:playOneShot("sfx/coin.wav", 0.8)` adds a voice (the source's group,
  volume x scale, pitch, pan; 3D voices follow the source). Ten coins from one source overlap up
  to `maxVoices`; past it the oldest one-shot stops. `stop()` stops them too. A one-shot of a clip
  still loading starts when it is loaded, or is dropped after 0.25 s (a warning:
  `AudioSystem.preload(path)` it ahead). Each streamed one-shot reads the file on its own: prefer
  DecompressOnLoad (Auto does it for small files).
- **The voice budget**: past Max Voices (Project Settings > Audio) the playing voice with the
  highest priority number stops (the oldest of them first); a new voice less important than every
  playing one does not start. Protected voices (`protectVoice`) are never stolen by the budget nor dropped when
  SoLoud runs out of voice slots; like any voice they are virtualised (silent until loud enough
  again) when more than Max Real Voices play.
- **Fades**: `fadeIn(seconds)` plays from silence to the current volume (starting it if needed),
  `fadeOut(seconds [, {pause = true}])` goes to silence then stops (or pauses) and puts the volume
  back for the next `play()`, `fadeTo(volume, seconds)` changes `volume`. Scaled time unless
  `{unscaled = true}`. `isFading()`. A fade dies with its source.
- **Pause**: `AudioListener.pause = true` holds every source but the `ignoreListenerPause` ones
  (a pause menu), and only those it paused come back. The app going to the background holds the
  Master bus: the sources keep their own pause, so a source the game paused stays paused after the
  app returns. `AudioSystem.pauseAll()` / `resumeAll()` hold and release the Master bus for the
  game, apart from the app's pause: returning from the background keeps a `pauseAll`, and
  `resumeAll` in the background waits for the app to return.
- **Events** (`getAudioSourceComponent().onStart / onPause / onStop`): onStart when it starts
  playing (play, play on awake, a resume), onPause on pause, onStop on `stop()`, `fadeOut`, the end
  of a non-looping clip, or a voice stolen by the budget. On the main thread, after the audio update.
- `isPlaying()` follows the voice: true right after `play()`, for a looping clip forever, at any
  pitch; one-shots count. `getPlayingTime()` is the position in the clip.
- `AudioSource.playClipAtPoint(scene, path, position [, volume [, group]])` plays a clip at a world
  position (3D against the scene's camera, logarithmic rolloff) without a GameObject; it belongs to
  `scene` and stops with it.
- **3D**: a source with a Transform plays 3D under a camera. One listener hears every scene: the
  main scene's camera (a perspective camera at its position; an orthographic or UI camera at the
  centre of its view, so a sound at the centre of the screen is centred). A layer scene's sources
  are heard where they are in its own camera's view. Doppler follows speeds in units a second,
  under perspective cameras only (2D and UI scenes move in pixels); a teleport or a camera cut has
  none. A clocked source (`enableClocked`) starts at the game time it was played.

## 8. Scene loads, persistence, devices

- A scene's sources stop with the scene, with their one-shots and fades; `playClipAtPoint`
  voices with the scene passed. The mixer and its values survive. Persistent objects
  (DontDestroyOnLoad) keep their sources, voices, one-shots and fades across loads.
- No audio device (CI, a server, xvfb): the engine mixes without output (log: "No audio output
  available ... mixing without output"); `AudioSystem.isOutputAvailable()` is false and everything
  else behaves as on a device. `JASPERENGINE_AUDIO_BACKEND=nosound|null|deterministic` forces it. A
  player run with `--headless` or `--silent-audio` mixes on the deterministic driver, by the frames'
  game time: its sounds play, end and fire their events as in a normal run, the same on every run
  with the same `--fixed-fps`.
- The Web needs a user gesture before sound starts (browsers); mixing and meters run before it.

## 9. Recipes

**Unity's VolumeController** (settings toggles, remembered): expose `MusicVolume`, `SFXVolume`,
`MASTERVOLUME` (the three group volumes) with `remember: true`, then

```lua
local VolumeController = {}

function VolumeController:setMusic(on)
    AudioMixer.setFloat("MusicVolume", on and 0 or -80)
end

function VolumeController:setSfx(on)
    AudioMixer.setFloat("SFXVolume", on and 0 or -80)
end

function VolumeController:isMusicOn()
    return (AudioMixer.getFloat("MusicVolume") or 0) > -80
end

return VolumeController
```

The values come back on the next launch by themselves (no PlayerPrefs code, no startup double-set).

**Unity's MinigameSongs** (menu <-> gameplay filter sweeps): Music has a low-pass and a high-pass
with `lowPassCutoff` / `highPassCutoff` exposed. Either time it with setFloat (scaled time, as the
game's `Mathf.Lerp` on `Time.deltaTime`):

```lua
local MinigameSongs = {}

function MinigameSongs:toGameplay()
    AudioMixer.setFloat("lowPassCutoff", 525, 0.9)
    AudioMixer.setFloat("highPassCutoff", 430, 0.9)
end

function MinigameSongs:toMenu()
    AudioMixer.setFloat("lowPassCutoff", 22000, 0.65)
    AudioMixer.setFloat("highPassCutoff", 10, 0.65)
end

return MinigameSongs
```

or with two snapshots, `AudioMixer.transitionTo("Gameplay", 0.65)` /
`AudioMixer.transitionTo("Menu", 0.65)` (exposed values a script set win over snapshots: use one
way per parameter, or `clearFloat` first).

**A pause menu**: `AudioListener.pause = true` when it opens; its click sounds have
`ignoreListenerPause`.

**Music that streams, effects that do not lag**: leave `loadType` Auto (big files stream, small
ones are decoded); `AudioSystem.preload("sfx/coin.wav")` in a loading screen.

## 10. Pitfalls

- `outputGroup` takes the group's **name**; the Inspector's Output lists them.
- A script value hides snapshot changes of that parameter until `clearFloat`.
- Meters read -80 before the first frame of a sound and for groups nothing plays through.
- Unknown parameter, snapshot or group names never raise: `setFloat` returns false, `getFloat`
  and `getGroup` nil, each logged once.
- `getEffect(1)` in Lua is the first effect (1-based); MCP `index` is 0-based.
