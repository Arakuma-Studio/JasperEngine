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
several items, numbered as `n`). A capture (`capture: "last"`, S35) takes `start`, `end` and `group`.

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
