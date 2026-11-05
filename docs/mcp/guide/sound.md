# Sound: measure, place and credit clips

You cannot hear. These tools turn a sound into numbers and pictures, keep what the bytes cannot say in
the clip's `.meta`, and give placeholder and built-in sounds without a network. The mixer is
`jasper://docs/audio`.

## Measure a file

`analyze_audio {paths: [...]}` takes WAV, OGG, MP3 or FLAC paths and a Recorder `.audio.wav`, up to 24
paths; over 48 items page with `offset`. `get_asset_info` gives a smaller summary of one file
(`data.audio`, `audio_error` when it does not decode).

- `loudness`: `integrated_lufs`, `short_term_max`, `momentary_max`, `lra`. Judge a short sound by
  `momentary_max`: under 400 ms has no integrated value.
- `peak`: `sample_dbfs`, `true_peak_dbtp`, `clipped`.
- `bands_db`, `band_share` (octaves from 31.5 Hz), `sub_share`, `centroid_hz`, `flatness`, `rolloff_hz`.
- `onset`: `peak_ms`, `attack_ms`, `first_ms`; `onsets_ms` with `detail: full`. `silence`: `head_ms`, `tail_ms`.
- `stereo`: `correlation`. `loop`: `seam_jump`, `seam_step_db`, `seam_level_db`. `pitch`: `hz`, `confidence`.
- `kind`: the built-in manifest's, else the importer `type`'s, else `measured_kind`. `kind_source` says which.
- `defects`: `edges`, `clicks`, `tail`, `gaps`, `resonance`, `aliasing` with `cuts` of its trims.
- `notes[]`: `{id, text, fix?}`, in plain language. `fix` holds the `set_audio_import_settings` keys that fix it.
  `differences_vs_1` says in words how each later item differs from the first.

A clip with import settings is measured **as the engine plays it**, gain, trims and fades applied and
the seam at its loop region's wrap. `played` gives that region, `source` the raw file's numbers, and the
note `as_it_plays` says so. Results are cached by content in `.jasperengine/cache/audio`.

`image` adds a picture, `max_width` wide: `waveform`, `spectrogram`, `both` or `sheet` (the default for
several items, numbered as `n`). A capture (`capture: "last"`) takes `start`, `end` and `group`.

## Import settings

`set_audio_import_settings {paths, settings, provenance, labels}` writes `import.audioImporter` in the
clips' `.meta`, one undo step, all paths or none, and replies with the settings resolved to frames and a
fresh analysis. A null key resets it; a default key is not written.

| Key | Values | Default |
|---|---|---|
| `type` | a sound type (`search_sounds`' list) | none |
| `gain_db` | -60 to 24 | 0 |
| `trim_head_ms`, `trim_tail_ms` | >= 0, or `"auto"`: to the first or last sample above -60 dBFS | 0 |
| `fade_in_ms`, `fade_out_ms` | 0 to 1000, raised cosine | 0 |
| `loop_start_ms` | >= 0, or -1: the trimmed start | -1 |
| `loop_end_ms` | after the start, or -1: the trimmed end | -1 |

The engine plays these in Play and in every export: a looping voice wraps at the loop region, and fades
never shape a wrap. Settings that do not fit the file are refused, naming the frames. A clip without
settings plays bit for bit as its file. A level change is `gain_db`; do not rewrite the file.

`provenance` keys: `licence` (an SPDX id, or `generated`, `unknown`, `proprietary`), `licence_url`,
`author`, `attribution`, `source`, `url`. `labels` replaces the clip's asset labels.

## Placeholders

`generate_sound {path, preset, seed, variations, params, loudness_lufs}` writes `<path>.wav` (or
`<stem>_1..n.wav`, seeds `seed..seed+n-1`) and its `.meta`, one undo step. The same preset, seed, params and
target give the same bytes in one editor version. Each sound is brought to its type's loudness target (or
`loudness_lufs`, momentary max) without passing -1 dBTP; `limited_by` says when the peak stopped it.

| Presets | Type | `params` |
|---|---|---|
| `coin`, `powerup` | pickup | SfxrParams: `wave_type` 0-3, `p_base_freq`, `p_env_decay` and the others, 0-1 |
| `laser`, `explosion`, `hurt`, `jump`, `blip` | shoot, explosion, hurt, jump, ui | as above |
| `click` | click | `freq_hz`, `decay_ms`, `length_ms` |
| `pluck` | ui | `freq_hz`, `damping`, `brightness`, `length_ms` |
| `bell` | chime | `freq_hz`, `brightness`, `length_ms` |
| `whoosh` | whoosh | `low_hz`, `high_hz`, `q`, `length_ms` |
| `riser` | riser | `start_hz`, `end_hz`, `noise`, `length_ms` |
| `thump` | thump | `freq_hz`, `drop_hz`, `decay_ms`, `click`, `length_ms` |
| `ui_click` | click | layered: `pitch_st`, `length`, `brightness`, `room`, `drive`, `force` |
| `swoosh` | whoosh | as above |
| `impact` | impact | as above |
| `pop` | pop | as above |

`random` picks one by the seed, among the first 13. A name out of range or not in the list is refused, naming the
valid ones. Builds warn about placeholders (`W-AUDIO-PLACEHOLDER`): replace them before release.

**Families.** A layered preset is layers summed, then a chain, and `family` fixes its character (the first is the
default): `ui_click` `tink`, `snap`, `mouse`, `glass`, `soft`; `swoosh` `fast`, `big`, `thin`; `impact` `wood`, `metal`,
`stone`, `soft`, `heavy`; `pop` `bubble`, `cork`, `soft`. A seed varies how hard it is struck (`force`, -1 to 1, or fix
it), its timing and a little pitch, so variations read as takes of one sound. `pitch_st` is -12 to 12, `length` 0.25 to 4,
`brightness`, `room` and `drive` -1 to 1.

**Variety.** Several variations report `variety`: their distances (as `search_sounds similar_to`), the nearest pair,
`near_duplicates` and a verdict: `one_palette`, `wide` (they read as different sounds) or `near_duplicates` (use other
seeds or a different force).

**Like this.** `generate_sound {preset, like}` fits a recipe or a layered preset to a project clip, `essentials:` or
`library:` id as it plays: up to 64 renders against its band balance, envelope, length and attack. `seed` defaults to 1,
`params` are the search's, `family` fixes the family. Loops, speech, silence, more than 8 s, sfxr presets and `random` are
refused. Any licence is accepted: only measurements reach the result, which is `generated`, with the reference
in `provenance.reference`. `fit` has `distance`, `grade` (`close` to 1, `similar` to 2, else `rough`), `start_distance`
and the differences in words. The distance says how alike the measurements are, not whether the result sounds right.

## Slicing takes into a Sound Event

`edit_sound_event {path, create: true, slice: {source, ...}}` cuts a project audio file of several takes into
`<event stem>_01.wav`, `_02.wav`... beside the new event, a shuffle event of them, in one undo step. A clip's `.meta`
holds the source's type, labels and licence, `provenance.sliced_from` and a click-safe fade on an edge that would click.
The source stays as it is, and a target that exists refuses the whole call.

- `gate_db` (-40): an onset counts when the level reaches this re the file's peak within 50 ms.
- `min_gap_ms` (80): an onset closer than this to the previous one is part of it.
- `pre_ms` (4): a slice starts this long before its onset. `max_ms` (2000): its longest.
- `preview: true` returns the slices and an image, and writes nothing; `pick` keeps those numbers.
- `variation: true` adds pitch +/-1 semitone and volume -1.5 to 0 dB per play: for sounds that repeat a lot.

At most 64 slices, a 120 s source and 32 MiB written. Import a built-in clip first to slice it.

## Built-in sounds and licences

`search_sounds {query, type, scope, filters, similar_to, limit, sheet}` finds sounds in the project, the 40
essentials and the 927-clip starter library, without a network. A built-in clip has an id:

- `essentials:<id>`, `essentials:<group>/*`, `essentials:*`, `library:<id>`, `library:<type>/*`: these work in
  `analyze_audio paths`, `search_sounds similar_to` and `import_files sources`.
- `import_files {sources: ["library:footstep/kenney_wood_1"], to_dir: "assets/sounds"}` copies the bytes (their
  SHA-256 checked) to `<to_dir>/<id>.ogg` and writes the `.meta`, at most 48 clips, one undo step. An identical file
  is skipped; a different one is refused unless `overwrite`.
- `filters`: `min_ms`, `max_ms`, `brightness`, `attack`, `tonal`, `loop`, `kind`. `heard_tags` are a model's guess.

`library.available: false` in a result means the install has no library: reinstall the editor or set
`JASPERENGINE_SOUND_LIBRARY_DIR`. The essentials are always there.

Every export writes `licenses/THIRD-PARTY-AUDIO.txt` from the shipped clips' provenance unless the setting
`audio.credits_file` is false. A clip without provenance, your own, is not listed. `licence: unknown` gives
`W-AUDIO-LICENCE`.

## Sound Events and triggers

A **Sound Event** (`.soundevent`, Unity's Audio Random Container) holds clips played by weight
(`random`), `shuffle`, `sequence` or as `layer`s, with pitch, volume and delay ranges, a cooldown, an
instance limit with stealing and an output group. Scenes refer to it by GUID (in its `.meta`), so
`move_asset` it freely; a moved clip's path follows in the event, in the same undo step.

Make one with `edit_sound_event`, then point an AudioSource at it with `set_properties`:

```json
{"path": "sounds/steps.soundevent", "create": true, "values": {
  "mode": "shuffle", "clips": ["sounds/step_1.wav", "sounds/step_2.wav", "sounds/step_3.wav"],
  "pitch_semitones": [-1, 1], "volume_db": [-3, 0], "cooldown_ms": 50, "max_instances": 4}}
```

| Key | Default | Range and meaning |
|---|---|---|
| `mode` | `shuffle` when made here | `random`, `shuffle`, `sequence`, `layer` |
| `clips` | | 1 to 64: a path, a GUID or `{clip, weight (0..1000, random), volume_db (-60..12), pitch_semitones (-24..24), delay_ms (0..2000)}` |
| `avoid_repeat` | true | random: the last clip is left out of the next pick |
| `pitch_semitones`, `volume_db`, `delay_ms` | `[0, 0]` | `[min, max]`, one draw per play |
| `output_group` | the playing source's | a mixer group by name (an `Audio.play` voice: its `group`, else Master) |
| `max_instances` | 0 | live plays at once over every scene, 0..255 (0 = no limit) |
| `steal` | `oldest` | at the limit: `oldest`, `quietest`, `newest`, `none` (reject) |
| `cooldown_ms` | 0 | 0..60000: plays closer than this are rejected |
| `priority` | 128 | 0..256; 128 = the playing source's |
| `loop` | false | every voice loops |
| `spatial` | the source's | `{min_distance, max_distance, attenuation, rolloff, doppler}` |
| `type` | none | a sound type (`footstep`, `impact`, `ui`...) |

- **Limits act in order:** cooldown, instance limit (a steal stops the old voices first), the source's
  `maxVoices`, the global `audio.max_voices`.
- **Same picks every run:** without a device (the null driver, `--headless`) picks use the project's
  `audio.seed`, so `audition` shows what Play will do; with a device each run draws its own seed.
- **AudioSource**: `event` (set, its clip is ignored, with no fallback), `playOn` (`ui_click`, `ui_press`,
  `ui_hover`, `collision_2d`, `collision_3d`, `sensor_2d`, `sensor_3d`) plays a one-shot of its clip or
  event, `minImpactSpeed` gates contacts and `fullVolumeImpactSpeed` scales their volume by speed.
  `pitchRandomSemitones` and `volumeRandomDb` vary each start of a source without an event.
- **Lua**: `Audio.play(eventOrClip [, opts])` returns a `SoundHandle`; `Audio.stopAll([group])`
  (`jasper://docs/lua-api` 2.9).
- **Seeing what played:** run with `JASPERENGINE_AUDIO_TRACE=1` and `read_console` shows every voice's start,
  stop and rejection as `[audio] {json}`.

Recipes: **footsteps** are a `shuffle` with small pitch and volume ranges and a `cooldown_ms`;
**UI clicks** a `random` event on a Button's AudioSource with `playOn: ["ui_click"]`; **impacts** an event on
a Rigidbody's AudioSource with `playOn: ["collision_3d"]`, `minImpactSpeed` and `fullVolumeImpactSpeed`.

## Timeline audio

Sound that must land on an animation frame goes on the clip, not in a script: `edit_animation {clip, audio}`
cues (jasper://docs/animation, Audio cues) start on the sample of the crossing in the Animation Player, the
Animator and montages; an entity Animation takes the same items as audio and Mixer Snapshot actions.

- **Footsteps:** a `shuffle` Sound Event of steps, one cue per contact frame:
  `{"time": 0.32, "sound": "assets/sounds/steps.soundevent", "spatial": true, "path": "Body/LeftFoot"}`. To put the
  step's transient on the foot's contact frame, set `time` = contact − (`onset.peak_ms` of `analyze_audio` on the event's
  clip) / 1000; the Animation window's Align Peak to Playhead does the same. `get_animation {clip}` gives each cue's `length`.
- **Engine loop:** a looping AudioSource on the car, a `source` cue at 0 with `stop: "state_exit"`, and property tracks
  `component: "audiosource"`, `property: "volume"` / `"speed"` on the same path shape it while the state plays.
- **Build-up:** a `snapshot` cue (`{"time": 2.0, "snapshot": "Build", "seconds": 0.5}`) moves the mixer; a later cue
  moves it back. Snapshots are the project mixer's.
- **Check:** capture the Play (below): each fired cue is a `cue` line (`clip`, `cue`, `kind`, `player`, `state`,
  `position`, `delay_samples`, `timeline`) followed by its `voice_start` (owner `cue` or `main`, cause `animation`) on
  the same sample plus `delay_samples`; a snapshot cue writes a `mixer` line with `cause: "animation"`.

## Capture a Play

A Play captures when the editor has no output device (the usual case for an agent), or after
`audio_session {action: "arm"}`, which applies to the next `play start` or `restart` and is used once (`capture: false`
turns the default off; `max_seconds`, `stems`). It runs on the deterministic driver: 48 kHz stereo, mixed once a frame by
game time, and the device is muted. With `play {fixed_fps}` and the same input and `audio.seed`, `master.wav`,
`events.jsonl` and `levels.jsonl` are the same bytes on every run. A player does the same with
`--headless --fixed-fps 60 --audio-capture <dir>` (development players; read it with `session: "<dir>/<id>"`).

`play stop` returns `audio`: the session, its loudness and its counts. `audio_session` then reads it:

- `status`: what the next Play does, and the sessions, newest first.
- `report {session, start, end, events_limit, image, max_width}`: `loudness`, `peak`, `groups`, `voices`, `mixer`, and the
  moments numbered as on the timeline: `events` (`E<n>`, voices) and `markers` (`M<n>`: gameplay).
- `session` is `last` (the default), `current` (the Play still running), an id, or a directory.

`Audio.mark("label")` puts your own moment on the clock; collisions, sensors, UI presses, clicks and hovers, HTML UI clicks,
animation events, spawns, destroys, scene loads and input are marked for you. Project Settings > Audio: `audio.capture_max_seconds`
(audio stops growing, events go on), `audio.capture_keep`, `audio.capture_stems`.

## Checks

`audio_session {action: "check", session, checks, expect, baseline, save_baseline, fail_on, image}`: `verdict`, and for each check
`status` (PASS, FAIL, WARN, SKIP), `measured`, `threshold` and where it comes from, `limited_by`, `at` (times), `fix_hint`.

| Check | What it says |
|---|---|
| `loudness` | integrated LUFS within `audio.loudness_target_lufs` +- `audio.loudness_tolerance_lu`; SKIP under `audio.qa.loudness_min_seconds` of gated audio |
| `true_peak` | at most `audio.true_peak_ceiling_dbtp` + 0.1 dB |
| `clipping` | no output sample at full scale |
| `coverage` | each moment of an `expect` rule has its sound; without rules, a marker kind and entity sounded only some of the times |
| `sync` | the audible onset comes within `audio.qa.head_silence_max_ms` of the stamp |
| `audibility` | a voice's main band stands `audio.qa.audibility_margin_db` over the other groups; `limited_by` names the group and the band |
| `voice_budget` | WARN near `audio.max_voices`; FAIL when a voice of `audio.qa.important_priority` or better is lost |
| `ducking` | a `duck` effect drops its group while the side chain plays |
| `silence` | no gap over `audio.qa.max_silence_seconds` after the first voice of `audio.qa.music_group` |
| `load_delay` | WARN for a voice that waited for its load |
| `repetition` | WARN for one clip `audio.qa.repeat_min_plays` times in a row from one source |
| `regression` | against `baseline`: starts within `audio.qa.regression_timing_ms`, bands within `audio.qa.regression_band_db` |

`expect` items: `{on, entity, sound, within_ms, offset_ms, align}`. `on` is a marker kind (`ui_click`, `collision_3d`...) or an
`Audio.mark` label, `sound` a clip path's part or an event, `align` the point of the sound that must be there: `onset`, `peak`, `end`.

`save_baseline: "<name>"` writes `baselines/audio/<name>.json` (one undo step); `baseline` takes that name or a session.

A script or a build step: `batch` with `play {action: "start", fixed_fps: 60}`, `wait {frames}`, `play {action: "stop"}`,
`audio_session {action: "check", fail_on: "fail"}`. `fail_on` makes the verdict an error, and
`jasperengine-editor call audio_session '{"action":"check","fail_on":"fail"}'` exit 1.

## Rules

- Never claim a sound plays without a capture. Report the numbers.
- Fix the sound or the mix; never move a threshold to pass. `thresholds.changed` shows every threshold that differs from its default.
- After `audio.qa.fix_rounds` failures of one check in a row, `guidance` says to stop: report what it measured.
- A check never changes anything; it suggests.

## Hand over to a human

Numbers cannot say whether a sound is right. `check` gives `listen`: 3 to 5 moments `{t, why}`. Ask the user to open
Window > Audio > Captures, select the session and play them (Enter on a row, Escape stops).
## Loudness

In Play, `audio_mixer get` has `loudness` for Master: `momentary_lufs`, `short_term_lufs`,
`integrated_lufs`, `true_peak_dbtp`, `gain_reduction_db`, `target_lufs`. `get_play_state.audio.loudness`
has the momentary value and the true peak. Measuring starts at the first read and runs until Stop, so
the first read's values are null: read, wait for the sound, read again.

- One-shots: read LUFS-M (`momentary_lufs`, 400 ms) while the sound plays. Short sounds never fill a
  3 s window, and integrated loudness averages them away.
- Music and whole scenes: read integrated (`integrated_lufs`) over at least a minute of play. It is
  null until 400 ms have passed and over a -70 LUFS gate.
- Target: the project's `audio.loudness_target_lufs`, -18 by default, within
  `audio.loudness_tolerance_lu`. It is a measurement: nothing turns the game up or down to meet it. To
  move a level, change the group's volume.
- Guidance by platform, not settings: consoles and PC about -18 to -23 LUFS, mobile about -14 to -18.
- The -1 dBTP rule: keep true peak at or under -1 dBTP, since conversion after the game can overshoot.
  A `limiter` on Master with `truePeak` 1 and `lookahead` 1.5 holds it (a new project has one), and
  `gain_reduction_db` shows how hard it works. More than about 6 dB most of the time means the mix is
  too hot: lower the groups instead of leaning on the limiter.
- Under the `roundoff` output stage what leaves the device is 1.66 dB under what these numbers read.
