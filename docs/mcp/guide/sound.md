# Sound: levels that hold

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
