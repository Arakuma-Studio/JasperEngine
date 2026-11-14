#!/usr/bin/env python3
"""Build and verify the starter sound library (CC0 clips from Kenney and Freesound).

The selection (tools/sound-library/library.json) pins every input: the Kenney zips by URL,
SHA-256 and size, and each Freesound sound's public preview (cdn.freesound.org, "-hq.ogg"
where it exists, else "-hq.mp3") by URL, SHA-256 and size. No Freesound account, API key or
OAuth is used: a sound's licence is read from its public landing page and must be CC0.
For each clip the selection names its source, type, kind, tags, description and the event
(or segment) to cut. See phase 5 §2.11 of docs/superpowers/specs/2026-10-14-sound-layer-phase5-design.md
and its §5.2 notes.

Commands (--selection FILE is accepted by every command; default tools/sound-library/library.json):

    fetch    --cache DIR [--offline] [--refresh]   pinned Kenney zips, Freesound landing-page metadata and previews
    prepare  --cache DIR --work DIR [--only GLOB]  decode, resample, downmix, cut, clean, trim, loop, normalise,
                                                   encode and measure every clip into DIR/<id>.ogg and <id>.json
    judge    --work DIR [--editor PATH]            the editor's analyzer (audio-analyze --type --kind [--fade-in-ms])
                                                   on every encoded clip, and the agreement check (skipped, with
                                                   the reason, when PATH has no analyze command)
    dedupe   --work DIR                            the near-duplicate check, into DIR/dedupe.json: pairs of clips that
                                                   are one sound twice, or the same audio from two uploaders
    write    --work DIR [--out DIR] [--audit FILE] accepted clips (the later of each dedupe pair left out, from
                                                   DIR/dedupe.json; a pair whose clips changed since is not applied),
                                                   manifest.json, licences, audit log; then analyse and verify.
                                                   Refuses, writing nothing, on any budget failure, and on a clip the
                                                   semantic file reads as "review" without a review_note
    analyse  --out DIR --editor PATH               rewrite analysis.json only
    verify   [--out DIR] [--audit FILE] [--allow-no-analysis]
                                                   offline, standard library only
    all      --cache DIR --work DIR [--editor PATH] [--out DIR] [--audit FILE] [--offline]
                                                   fetch, prepare, judge, dedupe, write
    review   --work DIR --editor PATH --type T --png FILE [--semantic]
                                                   a numbered sheet of one type's candidates (editor CLI); --semantic
                                                   adds each clip's recorded label and heard tags to the listing
    semantic-fetch --cache DIR [--offline]         the selection's pinned model files (semantic.model) from
                                                   huggingface.co into DIR/clap/<revision>/, each checked by size,
                                                   SHA-256 and git blob SHA-1 before it is kept
    semantic --work DIR --cache DIR [--check] [--embedder fake] [--semantic-file FILE]
                                                   the optional semantic audit (semantic_stage.py, in its own pinned
                                                   environment: semantic-requirements.txt installed with
                                                   --require-hashes into DIR/semantic-env/): writes the recorded file
                                                   tools/sound-library/library-semantic.jsonl; --check re-runs it and
                                                   compares instead. write and verify read only that file
                                                   (--semantic-file; --allow-fake-semantic for tests)
    seed     --opus FILE --cache DIR --seed-out FILE [--type T ...] [--ids N,N] [--limit N]
                                                   pin Freesound ids from opus-sound-layer's library.json (untrusted
                                                   ideas): landing page, CC0 check, preview probe, SHA-256. When the
                                                   selection has a "seed" pin, a FILE whose SHA-256 or size differs
                                                   is refused before any entry is read

Besides the opus-style filters, prepare judges every decoded clip with Phase 2.5's defect notes
(edge_click, clicks, crackle, silence_gap; tools/mcp/audio_defects_ref.py, imported by path, its
SHA-256 in the audit): fatal for one-shot and speech kinds, crackle information for loops; a loop
whose first frame clicks gets the manifest import default {"fade_in_ms": 2} and is judged with it. A one-shot whose
tail the oracle calls truncated is cut later or given a release fade, else rejected (P5 §5.7).

Everything but verify needs the pinned packages:

    uv run --no-project --python 3.12 --with numpy==2.5.4 --with scipy==1.18.1 --with soundfile==0.14.0 \\
        --with pyloudnorm==0.2.0 tools/sound-library/build_library.py all --cache /tmp/snd-cache --work /tmp/snd-work \\
        --editor build/linux/editor-install/bin/jasperengine-editor

Network use is only in fetch, seed and semantic-fetch: HTTPS to kenney.nl, freesound.org and
cdn.freesound.org (semantic-fetch: huggingface.co and its CDN, *.huggingface.co and *.hf.co) only, one request per second per host group, a User-Agent naming this tool, retries with
backoff on 429/5xx (Retry-After honoured), freesound.org's robots.txt obeyed, and every file
cached so a warm cache makes no request (--offline makes any request fatal). Downloaded data is
untrusted: it is pinned by SHA-256 before it is opened, zip members are screened, landing pages
are parsed as data and only a few fields are kept, and nothing from a downloaded file or from
opus-sound-layer is ever executed.
"""

import argparse
import fnmatch
import hashlib
import html.parser
import io
import json
import math
import os
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.robotparser
import warnings
from datetime import datetime, timezone
from pathlib import Path

import soundlib
from soundlib import LICENCE_MARKERS, BuildError, PoliteFetcher, manifest_text, read_licence, read_member, sha256, tree_files

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
DEFAULT_SELECTION = HERE / "library.json"
DEFAULT_OUT = ROOT / "editor/resources/sound-library/library"
DEFAULT_AUDIT = HERE / "library-audit.jsonl"
DEFAULT_SEMANTIC = HERE / "library-semantic.jsonl"
SEMANTIC_STAGE = HERE / "semantic_stage.py"
SEMANTIC_REQUIREMENTS = HERE / "semantic-requirements.txt"
ESSENTIALS_SELECTION = HERE / "essentials.json"

SELECTION_FORMAT = "jasperengine-sound-library-selection"
MANIFEST_FORMAT = "jasperengine-sound-library"
ANALYSIS_FORMAT = "jasperengine-sound-analysis"
SEED_FORMAT = "jasperengine-sound-library-seed"
MANIFEST = "manifest.json"
ANALYSIS = "analysis.json"
# an analysis.json item is the CLI item minus what is resolved per call (§2.2.3, §5.0.5 item 8); format.load_type too
ANALYSIS_DROPPED = ("n", "path", "type", "labels", "licence", "notes", "import")
JUDGE = "judge.json"
DEDUPE = "dedupe.json"
LICENCE = "CC0-1.0"
CC0_DEED = "creativecommons.org/publicdomain/zero/1.0/"
KENNEY_AUTHOR = "Kenney (www.kenney.nl)"
KENNEY_HOST = "kenney.nl"
FREESOUND_HOSTS = ("freesound.org", "cdn.freesound.org")
# the model files (semantic-fetch): the Hub and its CDN only (P1 §5.2's redirect rule, §2.11.9)
HF_HOSTS = ("huggingface.co", ".huggingface.co", ".hf.co")
SEMANTIC_LABELS = ("ok", "weak", "review")
ROBOTS_URL = "https://freesound.org/robots.txt"
ROBOTS_AGENT = "JasperEngine-sound-library"
USER_AGENT = ROBOTS_AGENT + "/1 (tools/sound-library/build_library.py; +https://github.com/Arakuma-Studio/JasperEngine)"
REQUEST_INTERVAL_S = 1.0

HARD_CAP_BYTES = 37748736  # §2.3.1: 36 MiB for the whole library/ tree
MAX_ZIP_BYTES = 20 * 1024 * 1024
MAX_MEMBER_BYTES = 1024 * 1024
MAX_PAGE_BYTES = 2 * 1024 * 1024
MAX_PREVIEW_BYTES = 20 * 1024 * 1024
MAX_ROBOTS_BYTES = 64 * 1024
MAX_JSON_BYTES = 8 * 1024 * 1024
MAX_SEED_BYTES = 16 * 1024 * 1024
# the uploader's description kept from a landing page (untrusted text; evidence for the curator, P5 §5.7)
MAX_DESCRIPTION_CHARS = 2000
MAX_CLIPS_PER_SOURCE = 16
EDITOR_BATCH = 64
EDITOR_TIMEOUT_S = 600

ID_RE = re.compile(r"^[a-z0-9_]+/[a-z0-9_]{1,48}$")
PACK_RE = re.compile(r"^[a-z0-9]+(-[a-z0-9]+)*$")
TAG_RE = re.compile(r"^[a-z0-9_]{1,32}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
# Tags, ids and descriptions name the sound, not its file or its licence (P5 §5.7, the review's L-3): an uploader's
# page tags like cc0, free, wav or 48khz say nothing about what a clip sounds like and make search find it for them
FORMAT_WORDS = frozenset(("wav", "ogg", "mp3", "flac", "aif", "aiff", "aifc", "m4a", "wma", "aac"))
NON_SOUND_TAGS = FORMAT_WORDS | frozenset((
    "cc0", "cczero", "cc_zero", "cc", "creative_commons", "creativecommons", "public_domain", "publicdomain", "free", "freesound",
    "free_to_use", "freeware", "free_sound", "royalty_free", "copyright_free", "no_copyright", "license", "licence", "attribution",
    "stereo", "mono", "hq", "hd", "lossless", "high_quality", "bit", "bits", "khz", "hz", "kbps", "sample", "samples", "sound",
    "sounds", "sfx", "fx", "audio", "effect", "effects", "sound_effect", "sound_effects", "soundeffect", "file", "recording"))
QUALITY_TAG_RE = re.compile(r"^([0-9]+|(24|32)_?bits?|[0-9]+(_[0-9]+)?_?k(hz)?|[0-9]+_?hz|[0-9]+_?kbps)$")
FILENAME_RE = re.compile(r"\.(" + "|".join(sorted(FORMAT_WORDS)) + r")\b", re.IGNORECASE)
SHA1_RE = re.compile(r"^[0-9a-f]{40}$")
SEED_REPO_RE = re.compile(r"^https://github\.com/[A-Za-z0-9_.-]{1,100}/[A-Za-z0-9_.-]{1,100}$")
HF_REPO_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,95}/[A-Za-z0-9][A-Za-z0-9_.-]{0,95}$")
MODEL_FILE_RE = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_.-]{0,99}$")
SELECTION_KEYS = ("format", "version", "library_version", "encoder", "budget", "targets", "packs", "sources", "rejected",
                  "seed", "semantic", "excluded", "denied_terms")
SEED_KEYS = ("repo", "commit", "path", "sha256", "bytes")
MODEL_KEYS = ("repo", "revision", "licence", "weights", "files")
# what semantic_stage.py loads: the config, the feature extractor's and the tokenizer's files
MODEL_FILES_NEEDED = ("config.json", "preprocessor_config.json", "tokenizer.json", "tokenizer_config.json",
                      "special_tokens_map.json", "vocab.json", "merges.txt")
MAX_MODEL_FILE_BYTES = 2 * 1024 ** 3
FS_USER_RE = re.compile(r"^[A-Za-z0-9_.@+-]{1,150}$")
FS_PREVIEW_RE = re.compile(r"^https://cdn\.freesound\.org/previews/([0-9]{1,7})/([0-9]{1,10})_([0-9]{1,10})-hq\.(ogg|mp3)$")
FS_SEED_URL_RE = re.compile(r"^https://cdn\.freesound\.org/previews/([0-9]{1,7})/([0-9]{1,10})_([0-9]{1,10})-hq\.mp3$")
# opus's landing URLs; older Freesound usernames held spaces ("%20"), which freesound.org now
# redirects to the underscored name; the pinned username is always the page's own.
FS_SEED_LANDING_RE = re.compile(r"^https://freesound\.org/people/([A-Za-z0-9_.@+%-]{1,150})/sounds/([0-9]{1,10})/?$")

SR = 48000
LP_MAX_HZ = 23900
CLIP_LEVEL = 1.0 - 2.0 ** -15
SILENCE_DBFS = -60.0
LEVEL_FLOOR = -120.0
CEILING_DBTP = -1.0
TP_MARGIN_DB = 0.05
# §2.11.3 step 8 as amended by P5 §5.7 (the review's L-5): once the true peak stops the gain, the gain is the highest on
# a TP_STEP_DB grid whose decoded peak is under the ceiling. One reduction by the excess overshot (111 of 264 limited
# clips ended 0.2-1.0 dB under): the decoded peak is the encoded signal's own peak plus the codec's error, which jumps by
# up to about +/-0.3 dB from one gain to the next (Vorbis quantises differently at each level; measured on hit and
# footstep clips at 0.04 dB steps), so no search converges on it; the grid starts TP_SCAN_ABOVE_DB over the gain at
# which the first encode's decoded peak would meet the ceiling (the codec takes up to about 1 dB off a transient's peak,
# so the encoded signal's own peak predicts it badly) and steps down at most TP_SCAN_STEPS times. The trim is held at the first limited
# encode's (a moving -60 dBFS cut moves the codec's frames too). A gain one step higher went over, so at most TP_STEP_DB
# is left; the peak itself may read up to the codec's jitter under the ceiling.
TP_STEP_DB = 0.05
TP_SCAN_ABOVE_DB = 0.3
TP_SCAN_STEPS = 30
# The ceiling holds by both the standard's 4x FIR (BS.1770-4 Annex 2, the manifest's and the editor's true peak) and a
# 16x polyphase oversampler (scipy's resample_poly): on the library's low-passed clips the 4x reading was up to 0.047
# dB under the 16x one (the review's L-10). The 16x reading is taken only when the 4x one is within TP_FINE_RANGE_DB
# of the ceiling (a 4x FIR under-reads a band-limited signal by well under that).
TP_FINE_FACTOR = 16
TP_FINE_RANGE_DB = 1.0
GAIN_CORRECTIONS = 2
GAIN_TOLERANCE_LU = 0.1
MAX_UNDER_TARGET_LU = 6.0
# §3.1 D-4: a one-shot ends at most this far over its type's target (momentary max). The gain loop's usual
# corrections stop at GAIN_CORRECTIONS (a miss of 0.1 LU or more); a clip still more than this over gets up to
# OVER_TARGET_CORRECTIONS more, and is rejected when even they leave it over (P5 §5.3.6: ui-audio switch26 ended
# +0.61 LU over click's target, its trim moving with the gain). Clips that end within the bound are not touched.
OVER_TARGET_MAX_LU = 0.5
# The editor's soft_attack (SoundTypes::SOFT_ATTACK_MS) rejects every transient-kind clip whose attack exceeds 25 ms,
# whatever its type (§2.11.4's editor rules, not tunable); opus's rise_max of hit (60), boom and explosion (80), hurt
# (40), impact and shoot (30) only let such clips reach the editor. The Python rule is capped at the editor's bound so
# the two judges apply one limit (P5 §5.3.7: both measure the same rise; 51 Freesound hits and 15 booms reached the
# editor with 25-80 ms). A clip whose attack really swells is given the kind it measures (swell, tonal) in the selection.
SOFT_ATTACK_MAX_MS = 25.0
OVER_TARGET_CORRECTIONS = 3
SOURCE_MOMENTARY_MIN = -36.0
HEAD_SILENCE_MAX_MS = 15.0
HEAD_KEEP_S = 0.005
# Digital silence put before a one-shot or speech clip's first sample when it is encoded: Vorbis leaves codec noise
# in a file's first decoded samples, which the edge rule (decision 15) called a click on 31 of curate's 670 Kenney
# candidates; 64 samples left one, 128 none (P5 §5.6.3). The head trim keeps that much less, so the head silence is
# still at most HEAD_KEEP_S, and the cut leaves room for it inside the type's window.
PREROLL_FRAMES = 128
# The same noise at a clip's last decoded samples (5 of curate's 646 encoded one-shots): when the decoded end clicks,
# the clip is encoded again at the same gain with this much silence after it (1024 samples cleared every one; 64
# and 256 did not), if it still fits its window. Only those clips change.
POSTROLL_FRAMES = 1024
# A one-shot whose decoded tail Phase 2.5's oracle calls truncated (truncated_tail: still decaying faster than 20 dB/s,
# ending at or above -45 dB re its peak, with a release under 15 ms) is repaired in prepare, since the note tells an
# agent to fix a library clip (P5 §5.7, the review's M-1). First its cut ends later, where the kind's envelope falls
# TAIL_EXTEND_DB under the event's peak (inside the type's window, before the source's next event, and for a transient
# before a second hit; the cut rule's -40 dB sat just above the note's -45); a swell, whose cut rule stops both edges
# 20 dB down, also tries the shallower ends of SWELL_EXTEND_DB, since a much longer swell widens the edge windows its
# rise-and-fall rule reads. Then, at each end, the fade-out becomes a release: RELEASE_LINEAR_MS linear (the note's own
# fix length), then RELEASE_COSINE_MS of raised cosine (an import's fade counts at its length, a baked one is measured
# against the decay line, and on a steep tail only a fade that ends far under the line clears it). The first try that
# clears the note and passes every filter is kept; a clip no try clears is rejected. The editor's truncated_tail is fatal.
TAIL_EXTEND_DB = -50.0
SWELL_EXTEND_DB = (-50.0, -40.0, -30.0)
RELEASE_LINEAR_MS = 20.0
RELEASE_COSINE_MS = 50.0
TAIL_EXTEND_KINDS = ("transient", "tonal", "swell")
DOWNMIX_CANCEL_DB = -4.5
MIN_RATE = 22050
INTEGRATED_MIN_S = 0.4
# Phase 1's loop seam as built (§2.11.4, A28; SoundTypes::LOOP_SEAM_STEP_MAX_DB and the editor's
# AudioAnalysis): per channel the wrap's jump |x[0] - x[N-1]| over the RMS of the W-1 first differences
# inside each of the last and the first W = round(0.010 rate) samples (all N-1 when N < 2W), that RMS
# floored at 2^-15; the largest over the channels, floored at -120 dB. About 0 dB for a continuous wrap.
LOOP_SEAM_STEP_MAX_DB = 12.0
# The library's own, tighter bound (P5 §5.7, the review's L-8): with the seam search taking the lowest decoded step, the
# 47 loops of the rebuilt library sit at -4 to +7.6 dB (11 of them were at 6-11.4 dB before, the search keeping the
# unshifted end whenever it was under 11); a crossfaded wrap is two adjacent source samples, so a step over this means
# the codec moved the edges apart, and 4 dB stay between it and the editor's +12 note.
LIBRARY_SEAM_STEP_MAX_DB = 8.0
LOOP_SEAM_LEVEL_MAX_DB = 3.0
SEAM_WINDOW_S = 0.010
SEAM_REFERENCE_FLOOR = 2.0 ** -15
LOOP_LRA_MAX_LU = 6.0
# §3.1 D-7: the RMS of the 20 ms around a loop's wrap (its last 10 ms, then its first 10 ms) is within this of the
# clip's own RMS, on the decoded file, so the wrap neither dips nor bumps against the bed. The seam step and level
# only compare the two sides with each other; a wrap placed in a lull between a bed's birdsong or a beat's hits passes
# them and fails this (P5 §5.3.8: 37 of the first 51 Freesound loops). Prepare rejects such a loop itself.
LOOP_WRAP_RMS_MAX_DB = 1.0
# BS.1770-4's K-weighting at 48 kHz (the standard's two biquads), for the integrated loudness and the loudness range,
# which take only the blocks wholly inside the signal, as BS.1770-4, EBU Tech 3342 and the editor's meter do
# (pyloudnorm 0.2.0 also counts a last block that runs past the end, divided by the full block length; P5 §5.6.3)
K_SHELF = ([1.53512485958697, -2.69169618940638, 1.19839281085285], [1.0, -1.69065929318241, 0.73248077421585])
K_HIGHPASS = ([1.0, -2.0, 1.0], [1.0, -1.99004745483398, 0.99007225036621])
GATE_ABSOLUTE_LUFS = -70.0
GATE_RELATIVE_LU = -10.0
LRA_GATE_RELATIVE_LU = -20.0
# Vorbis compression levels: libsndfile maps level L to Vorbis quality 1 - L (§2.11.5)
QUALITY_DEFAULT = 0.6
QUALITY_AMBIENCE = 0.7
# A Vorbis or MP3 decode of a source mastered to full scale overshoots 0 dBFS by a few tenths of a
# dB without the source being clipped; a clipped master decodes with many samples at full scale. So
# a lossy source counts as clipped when more than CLIP_FRACTION_MAX of its samples are at full scale
# or it overshoots by more than LOSSY_OVER_MAX_DB (§5.2: measured on the 744 Kenney members and the
# Freesound previews; it rejects every Kenney member Phase 1 rejected for its peaks).
CLIP_FRACTION_MAX = 1e-4
LOSSY_OVER_MAX_DB = 1.0

KINDS = ("transient", "swell", "tonal", "riser", "glitch", "loop", "speech")
ONE_SHOT_KINDS = ("transient", "swell", "tonal", "riser", "glitch")


def _t(kind, win, hp, lp, **flags):
    return dict(kind=kind, win=win, hp=hp, lp=lp, **flags)


# §2.11.4 per-type parameters. The 19 opus types keep opus-sound-layer's values; the 12 game types
# are the spec's. Loudness targets are not here: they come from the selection's "targets", which
# repeats editor/audio/SoundTypes.h and is checked against `audio analyze --list-types` by judge.
TYPES = {
    "click": _t("transient", (0.03, 0.20), 200, 12000, hi_max=0.5),
    "key": _t("transient", (0.05, 0.15), 200, 11000),
    "tick": _t("transient", (0.015, 0.15), 300, 12000, hi_max=0.55),  # P5 §5.6.3: opus's 0.03 min; Kenney's ticks are 15-23 ms
    "tap": _t("transient", (0.06, 0.60), 100, 12000, hi_max=0.55),
    "pop": _t("transient", (0.05, 0.40), 150, 12000),
    "thump": _t("transient", (0.12, 1.00), 35, 8000, allow_low=True, allow_sub=True),  # P5 §5.6.3: thumps are sub-heavy
    "hit": _t("transient", (0.20, 4.00), 28, 15000, allow_low=True, allow_sub=True, rise_max=60, layered=True, min_s=0.35),
    "boom": _t("transient", (0.40, 4.00), 22, 9000, allow_low=True, allow_sub=True, rise_max=80, layered=True, min_s=0.4),
    "paper": _t("swell", (0.12, 0.70), 200, 12000, noise_ok=True),
    "swish": _t("swell", (0.15, 0.70), 150, 12000, noise_ok=True),
    "whoosh": _t("swell", (0.25, 1.20), 100, 14000, noise_ok=True),
    "transition": _t("swell", (0.60, 3.00), 50, 15000, noise_ok=True, allow_low=True, spiky_max=4.0),
    "riser": _t("riser", (1.00, 6.00), 40, 15000, noise_ok=True, allow_low=True),
    "reverse": _t("riser", (0.40, 3.00), 60, 15000, noise_ok=True, allow_low=True),
    "glitch": _t("glitch", (0.06, 1.50), 80, 16000, noise_ok=True, hi_max=0.7, designed_end=True),
    "ui": _t("tonal", (0.04, 0.90), 200, 15000, hi_max=0.6, designed_end=True),
    "zap": _t("tonal", (0.06, 1.50), 120, 15000, noise_ok=True, hi_max=0.6, designed_end=True),
    "chime": _t("tonal", (0.25, 2.50), 200, 14000, hi_max=0.6),
    "shimmer": _t("tonal", (0.40, 3.50), 300, 16000, hi_max=0.75, noise_ok=True),
    "footstep": _t("transient", (0.05, 0.60), 60, 14000, hi_max=0.5),
    "impact": _t("transient", (0.05, 2.00), 30, 15000, allow_low=True, rise_max=30),
    "explosion": _t("transient", (0.30, 4.00), 22, 12000, allow_low=True, allow_sub=True, noise_ok=True, rise_max=80, layered=True),
    "pickup": _t("tonal", (0.05, 1.50), 150, 15000, hi_max=0.6, designed_end=True),
    "jump": _t("tonal", (0.05, 1.00), 100, 15000, hi_max=0.6, designed_end=True),
    "hurt": _t("transient", (0.05, 1.20), 80, 12000, rise_max=40),
    "shoot": _t("transient", (0.05, 2.00), 40, 15000, allow_low=True, noise_ok=True, hi_max=0.6, rise_max=30),
    "door": _t("swell", (0.20, 3.00), 50, 12000, allow_low=True, noise_ok=True, spiky_max=4.0),
    "stinger": _t("tonal", (0.30, 6.00), 60, 16000, hi_max=0.6, designed_end=True),
    "voice": _t("speech", (0.20, 4.00), 80, 12000),
    "ambience": _t("loop", (8.0, 24.0), 30, 16000, allow_low=True, noise_ok=True, hi_max=0.75),
    "music": _t("loop", (8.0, 40.0), 25, 18000, allow_low=True, allow_sub=True, noise_ok=True, hi_max=0.6),
}
LOOP_CROSSFADE_MS = {"ambience": 500.0, "music": 10.0}
# A crossfaded loop's wrap is two adjacent source samples, so it is continuous before encoding, and Phase 1's seam step
# (the jump over its neighbouring steps) compares that one step with the steps around it; the codec then moves the
# decoded edges, and the decoded step swings by 10-20 dB between ends one sample apart (-3.8 to +17.8 dB over the 40
# ends nearest a test bed's, whose steps before encoding were 5-9 dB), so the step before encoding predicts it badly.
# The segment's end may move by at most SEAM_SEARCH_S: candidates are ranked by their step's excess over the limit with
# SEAM_MARGIN_DB to spare (the same metric, on the filtered source's wrap), then by the size of the change, and the
# first SEAM_TRIES are encoded at the final gain; the one whose decoded seam step is lowest, its seam level and true peak
# passing, is kept (P5 §5.7, the review's L-8: keeping the first that passed left 11 of 47 loops at 6-11.4 dB). When that
# step is still over the library's bound (LIBRARY_SEAM_STEP_MAX_DB), the search goes on to SEAM_MAX_TRIES ends and stops
# at the first under it.
SEAM_SEARCH_S = 0.002
SEAM_MARGIN_DB = 1.0
SEAM_TRIES = 16
SEAM_MAX_TRIES = 48

# Editor notes that reject a clip (§2.11.4); each applies when its condition on the clip holds.
EDITOR_FATAL = (
    ("silent", lambda c: True),
    ("clipping", lambda c: True),
    ("true_peak", lambda c: True),
    ("dc_offset", lambda c: True),
    ("head_silence", lambda c: c["kind"] != "loop"),
    ("loop_seam", lambda c: c["kind"] == "loop"),
    ("phase", lambda c: c["channels"] == 2),
    ("riser_early", lambda c: c["kind"] == "riser"),
    ("soft_attack", lambda c: c["kind"] == "transient"),
    ("low_end", lambda c: not TYPES[c["type"]].get("allow_low")),
    # Phase 2.5's defect notes (decision 15); the analyzer gates them by kind and type itself
    ("edge_click", lambda c: True),
    ("clicks", lambda c: True),
    ("crackle", lambda c: c["kind"] != "loop"),
    ("silence_gap", lambda c: True),
    # a tail cut while it still decays (P5 §5.7): prepare repairs or rejects it; the analyzer gates it by kind and type
    ("truncated_tail", lambda c: True),
)
# Phase 2.5's notes that judge the library (§2.11.4, decision 15), in the order the agreement check
# reports them; crackle is fatal for one-shot and speech kinds, information for loops
DEFECT_NOTES = ("edge_click", "clicks", "crackle", "silence_gap")
# The fade-in a loop whose first frame clicks gets as a manifest import default (P2.5 decision 8's loop
# length; it plays once, before the first wrap)
LOOP_FADE_IN_MS = 2.0
IMPORT_KEYS = ("fade_in_ms", "fade_out_ms")
# Near-duplicates (P5 §5.7, the review's L-4). The similarity of two decoded clips (mono, 48 kHz) is the smallest of
# three normalised cross-correlations, each the largest over all lags: full band, and above NEAR_DUP_BANDS_HZ
# (4th-order Butterworth high-passes). One recording, uploaded twice or repeated inside a file, correlates in every band;
# two takes of an action (Kenney's thumps and punches, steps on one floor) correlate only in their smooth low-frequency
# shape and fall apart in the upper bands. Over the 18,804 same-type and same-source pairs of the 900-clip library the
# similarity was 0.923 or more for the 13 pairs of repeated audio and 0.796 or less for every other pair, so a pair at
# NEAR_DUP_NCC or more is one sound twice and only the first of it is kept: a Kenney clip before a Freesound one, an
# earlier Freesound upload (a lower sound id) before a later one, then the lower clip id. Clips of different authors
# whose full-band correlation reaches PROVENANCE_NCC are the same audio uploaded by two people (the whip crack of
# SciFiSounds and JayRom01, 0.935, its upper bands lost to the re-upload's lossy encode): a re-upload is only as CC0 as
# its original, so the later one goes, and the curator checks the earlier one's page (P5 §5.7). Compared: the clips of
# one type, and the clips of one source across types; a 12 kHz pass first (pairs under DUP_PRESCAN_NCC there are
# not near: decimation lowered the repeated pairs by at most 0.11).
NEAR_DUP_NCC = 0.90
PROVENANCE_NCC = 0.90
NEAR_DUP_BANDS_HZ = (1000.0, 3000.0)
DUP_PRESCAN_NCC = 0.70
DUP_PRESCAN_DECIMATE = 4
# Phase 2.5's oracle, imported by path so the pipeline keeps no second copy (§2.11.4)
DEFECTS_ORACLE = ROOT / "tools/mcp/audio_defects_ref.py"
# Agreement between the editor's item and the Python oracle (§2.11.4); not tunable.
AGREE_DURATION_S = 0.002
AGREE_SAMPLE_PEAK_DB = 0.2
AGREE_TRUE_PEAK_DB = 0.3
AGREE_LOUDNESS_LU = 0.2
AGREE_HEAD_MS = 2.0

# ITU-R BS.1770-4 Annex 2: the 48-tap 4x over-sampling FIR, as 4 phases of 12 taps.
TP_PHASES = (
    (0.0017089843750, 0.0109863281250, -0.0196533203125, 0.0332031250000, -0.0594482421875, 0.1373291015625,
     0.9721679687500, -0.1022949218750, 0.0476074218750, -0.0266113281250, 0.0148925781250, -0.0083007812500),
    (-0.0291748046875, 0.0292968750000, -0.0517578125000, 0.0891113281250, -0.1665039062500, 0.4650878906250,
     0.7797851562500, -0.2003173828125, 0.1015625000000, -0.0582275390625, 0.0330810546875, -0.0189208984375),
    (-0.0189208984375, 0.0330810546875, -0.0582275390625, 0.1015625000000, -0.2003173828125, 0.7797851562500,
     0.4650878906250, -0.1665039062500, 0.0891113281250, -0.0517578125000, 0.0292968750000, -0.0291748046875),
    (-0.0083007812500, 0.0148925781250, -0.0266113281250, 0.0476074218750, -0.1022949218750, 0.9721679687500,
     0.1373291015625, -0.0594482421875, 0.0332031250000, -0.0196533203125, 0.0109863281250, 0.0017089843750),
)

np = signal = sf = pyloudnorm = None


def _dsp():
    """Import the pinned DSP packages on first use, so verify runs on plain Python."""
    global np, signal, sf, pyloudnorm
    if np is None:
        import numpy
        import pyloudnorm as _pyloudnorm
        import soundfile
        from scipy import signal as _signal
        np, signal, sf, pyloudnorm = numpy, _signal, soundfile, _pyloudnorm


_oracle = None


def defects_oracle():
    """Phase 2.5's audio_defects_ref module, run from the bytes whose SHA-256 the audit records."""
    global _oracle
    if _oracle is None:
        import types
        data = DEFECTS_ORACLE.read_bytes()
        mod = types.ModuleType("audio_defects_ref")
        mod.__file__ = str(DEFECTS_ORACLE)
        exec(compile(data, str(DEFECTS_ORACLE), "exec"), mod.__dict__)
        mod.SOURCE_SHA256 = sha256(data)
        _oracle = mod
    return _oracle


# ---------------------------------------------------------------------------------------------
# JSON helpers

def load_json(path, limit, what):
    p = Path(path)
    if not p.is_file():
        raise BuildError(f"{what} {p} is missing")
    if p.stat().st_size > limit:
        raise BuildError(f"{what} {p} is larger than {limit} bytes")
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except (ValueError, UnicodeDecodeError) as e:
        raise BuildError(f"{what} {p} is not valid JSON ({e})")


def json_text(obj):
    """Work and cache files: keys sorted, indent 1, LF, final newline."""
    return json.dumps(obj, indent=1, sort_keys=True, ensure_ascii=False) + "\n"


def write_if_changed(path, data):
    """Write bytes through a temporary file, or leave an identical file untouched."""
    path = Path(path)
    if path.is_file() and not path.is_symlink() and path.read_bytes() == data:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".part")
    tmp.write_bytes(data)
    tmp.replace(path)
    return True


def rnd(v, digits):
    return None if v is None else round(float(v), digits)


# ---------------------------------------------------------------------------------------------
# The selection

def source_key(src):
    if src["source"] == "kenney":
        return f"kenney:{src['pack']}:{src['member']}"
    return f"freesound:{src['id']}"


def is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)


def _check_text(v, limit):
    return isinstance(v, str) and 0 < len(v) <= limit and all(ch.isprintable() for ch in v)


class Selection:
    """library.json, validated. Raises BuildError listing every problem."""

    def __init__(self, path):
        self.path = Path(path)
        self.name = self.path.name
        raw = load_json(self.path, MAX_JSON_BYTES, "selection")
        problems = []
        p = problems.append
        if not isinstance(raw, dict) or raw.get("format") != SELECTION_FORMAT or raw.get("version") != 1:
            raise BuildError(f"{self.path}: not a {SELECTION_FORMAT} version 1 file")
        self.raw = raw
        extra = set(raw) - set(SELECTION_KEYS)
        if extra:
            p(f"unknown top-level keys {sorted(extra)}")
        self.seed = self._check_seed(raw.get("seed"), p)
        self.semantic = self._check_semantic(raw.get("semantic"), p)
        self.excluded = raw.get("excluded") or []
        self.denied_terms = raw.get("denied_terms") or []
        for item in self.excluded:
            if not isinstance(item, dict) or not ID_RE.match(str(item.get("id"))) or not item.get("reason"):
                p(f"excluded[] holds {{id, reason}}: {item!r}")
        self.library_version = raw.get("library_version")
        if not is_int(self.library_version) or self.library_version < 1:
            p("library_version must be a positive integer")
        enc = raw.get("encoder")
        if not isinstance(enc, dict) or not all(isinstance(enc.get(k), str) and enc.get(k) for k in ("soundfile", "libsndfile", "vendor")):
            p("encoder must give soundfile, libsndfile and vendor strings")
        self.encoder = enc if isinstance(enc, dict) else {}

        budget = raw.get("budget")
        self.budget = budget if isinstance(budget, dict) else {}
        mb = self.budget.get("max_bytes")
        if not is_int(mb) or not 0 < mb <= HARD_CAP_BYTES:
            p(f"budget.max_bytes must be an integer in 1..{HARD_CAP_BYTES}")
        tot = self.budget.get("total")
        if not (isinstance(tot, list) and len(tot) == 2 and all(is_int(x) and x >= 0 for x in tot) and tot[0] <= tot[1]):
            p("budget.total must be [min, max] clip counts")
        mins = self.budget.get("min_per_type")
        if not isinstance(mins, dict) or any(t not in TYPES or not is_int(n) or n < 0 for t, n in mins.items()):
            p("budget.min_per_type must map types to counts")

        targets = raw.get("targets")
        if not isinstance(targets, dict) or set(targets) != set(TYPES) or not all(is_num(v) and -60 <= v <= 0 for v in targets.values()):
            p(f"targets must give a loudness target in -60..0 LUFS for each of the {len(TYPES)} types, and nothing else")
            targets = {}
        self.targets = targets

        self.packs = {}
        for pack in raw.get("packs") or []:
            pid = pack.get("id") if isinstance(pack, dict) else None
            if not isinstance(pid, str) or not PACK_RE.match(pid) or pid in self.packs:
                p(f"bad or duplicate pack id {pid!r}")
                continue
            self.packs[pid] = pack
            if not soundlib.host_allowed(pack.get("url") or "", (KENNEY_HOST,)):
                p(f"pack {pid}: url must be https://{KENNEY_HOST}/...")
            if pack.get("landing") != f"https://kenney.nl/assets/{pid}":
                p(f"pack {pid}: landing must be https://kenney.nl/assets/{pid}")
            if not _check_text(pack.get("title"), 80):
                p(f"pack {pid}: title missing")
            if not isinstance(pack.get("sha256"), str) or not SHA256_RE.match(pack["sha256"]):
                p(f"pack {pid}: sha256 must be 64 lowercase hex digits")
            if not is_int(pack.get("bytes")) or not 0 < pack["bytes"] <= MAX_ZIP_BYTES:
                p(f"pack {pid}: bytes must be 1..{MAX_ZIP_BYTES}")
            cm = pack.get("credits_member")
            if cm is not None and not _check_text(cm, 200):
                p(f"pack {pid}: credits_member must be a member name")
            voices = pack.get("voices")
            if voices is not None:
                if cm is None or not isinstance(voices, list) or not all(
                        isinstance(v, dict) and _check_text(v.get("prefix"), 100) and _check_text(v.get("name"), 100) for v in voices):
                    p(f"pack {pid}: voices must be [{{prefix, name}}] and needs credits_member")

        rejected = raw.get("rejected") or []
        self.rejected = {}
        for r in rejected:
            if not isinstance(r, dict) or not _check_text(r.get("source"), 300) or not _check_text(r.get("reason"), 500):
                p(f"bad rejected entry {r!r}")
                continue
            self.rejected[r["source"]] = r["reason"]

        self.sources = []
        self.clips = {}
        keys = set()
        for src in raw.get("sources") or []:
            if not isinstance(src, dict) or src.get("source") not in ("kenney", "freesound"):
                p(f"bad source {src!r:.120}")
                continue
            if src["source"] == "kenney":
                if src.get("pack") not in self.packs:
                    p(f"kenney source {src.get('member')!r}: unknown pack {src.get('pack')!r}")
                    continue
                m = src.get("member")
                if not _check_text(m, 300) or m.startswith("/") or "\\" in m or ".." in m.split("/"):
                    p(f"kenney source in {src['pack']}: bad member {m!r}")
                    continue
            else:
                self._check_freesound(src, p)
                if not is_int(src.get("id")):
                    continue
            key = source_key(src)
            if key in keys:
                p(f"{key} is listed twice")
                continue
            keys.add(key)
            if key in self.rejected:
                p(f"{key} is in both sources and rejected")
            ev = src.get("events")
            if ev is not None and (not is_int(ev) or not 1 <= ev <= MAX_CLIPS_PER_SOURCE):
                p(f"{key}: events must be 1..{MAX_CLIPS_PER_SOURCE}")
            clips = src.get("clips")
            if not isinstance(clips, list) or not 1 <= len(clips) <= MAX_CLIPS_PER_SOURCE:
                p(f"{key}: clips must list 1..{MAX_CLIPS_PER_SOURCE} clips")
                continue
            for c in clips:
                if self._check_clip(c, src, key, p):
                    self.clips[c["id"]] = (c, src)
            self.sources.append(src)
        if problems:
            raise BuildError(f"{self.path} is invalid:\n  " + "\n  ".join(problems))

    @staticmethod
    def _check_seed(seed, p):
        """The optional pin of opus-sound-layer's seed list (§2.14 H4): {repo, commit, path, sha256, bytes}."""
        if seed is None:
            return None
        if not isinstance(seed, dict):
            p("seed must be an object {repo, commit, path, sha256, bytes}")
            return None
        problems = []
        if set(seed) != set(SEED_KEYS):
            problems.append(f"seed must have exactly the keys {', '.join(SEED_KEYS)}")
        if not isinstance(seed.get("repo"), str) or not SEED_REPO_RE.match(seed["repo"]):
            problems.append("seed.repo must be https://github.com/<owner>/<name>")
        if not isinstance(seed.get("commit"), str) or not SHA1_RE.match(seed["commit"]):
            problems.append("seed.commit must be the full 40-digit commit SHA-1")
        path = seed.get("path")
        if not _check_text(path, 200) or path.startswith("/") or "\\" in path or ".." in path.split("/"):
            problems.append("seed.path must be a relative path in the repository")
        if not isinstance(seed.get("sha256"), str) or not SHA256_RE.match(seed["sha256"]):
            problems.append("seed.sha256 must be 64 lowercase hex digits")
        if not is_int(seed.get("bytes")) or not 0 < seed["bytes"] <= MAX_SEED_BYTES:
            problems.append(f"seed.bytes must be 1..{MAX_SEED_BYTES}")
        for msg in problems:
            p(msg)
        return None if problems else seed

    @staticmethod
    def _check_semantic(sem, p):
        """The optional semantic audit stage (§2.11.9): {model {repo, revision, licence, weights, files}, required}."""
        if sem is None:
            return None
        if not isinstance(sem, dict) or set(sem) - {"model", "required"} or "model" not in sem:
            p("semantic must be {model, required?}")
            return None
        problems = []
        req = sem.get("required", False)
        if not isinstance(req, bool):
            problems.append("semantic.required must be true or false")
        m = sem["model"]
        if not isinstance(m, dict) or set(m) != set(MODEL_KEYS):
            p(f"semantic.model must have exactly the keys {', '.join(MODEL_KEYS)}")
            return None
        if not isinstance(m["repo"], str) or not HF_REPO_RE.match(m["repo"]):
            problems.append("semantic.model.repo must be <owner>/<name>")
        if not isinstance(m["revision"], str) or not SHA1_RE.match(m["revision"]):
            problems.append("semantic.model.revision must be a full 40-digit commit SHA-1")
        if not _check_text(m["licence"], 40):
            problems.append("semantic.model.licence must name the licence")
        files = m["files"]
        names = set()
        if not isinstance(files, list) or not files:
            problems.append("semantic.model.files must list the pinned files")
            files = []
        for f in files:
            name = f.get("name") if isinstance(f, dict) else None
            if not isinstance(name, str) or not MODEL_FILE_RE.match(name) or name in names:
                problems.append(f"semantic.model.files: bad or duplicate name {name!r}")
                continue
            names.add(name)
            if set(f) - {"name", "bytes", "sha256", "blob_sha1"}:
                problems.append(f"semantic.model.files {name}: unknown keys {sorted(set(f) - {'name', 'bytes', 'sha256', 'blob_sha1'})}")
            if not is_int(f.get("bytes")) or not 0 < f["bytes"] <= MAX_MODEL_FILE_BYTES:
                problems.append(f"semantic.model.files {name}: bytes must be 1..{MAX_MODEL_FILE_BYTES}")
            if "sha256" in f and (not isinstance(f["sha256"], str) or not SHA256_RE.match(f["sha256"])):
                problems.append(f"semantic.model.files {name}: sha256 must be 64 lowercase hex digits")
            if "blob_sha1" in f and (not isinstance(f["blob_sha1"], str) or not SHA1_RE.match(f["blob_sha1"])):
                problems.append(f"semantic.model.files {name}: blob_sha1 must be 40 lowercase hex digits")
            if "sha256" not in f and "blob_sha1" not in f:
                problems.append(f"semantic.model.files {name}: needs sha256 or blob_sha1")
        weights = next((f for f in files if isinstance(f, dict) and f.get("name") == m["weights"]), None)
        if weights is None or "sha256" not in weights:
            problems.append("semantic.model.weights must name a file pinned by sha256")
        for want in MODEL_FILES_NEEDED:
            if want not in names:
                problems.append(f"semantic.model.files must include {want}")
        for msg in problems:
            p(msg)
        return None if problems else {"model": m, "required": req}

    def semantic_pins(self):
        """The model pins the semantic file's header must repeat: {repo, revision, sha256 (the weights')}."""
        if not self.semantic:
            return None
        m = self.semantic["model"]
        w = next(f for f in m["files"] if f["name"] == m["weights"])
        return {"repo": m["repo"], "revision": m["revision"], "sha256": w["sha256"]}

    @staticmethod
    def _check_freesound(src, p):
        sid = src.get("id")
        if not is_int(sid) or sid <= 0:
            p(f"freesound source: id must be a positive integer, not {sid!r}")
            return
        user = src.get("username")
        if not isinstance(user, str) or not FS_USER_RE.match(user):
            p(f"freesound:{sid}: bad username {user!r}")
            return
        if src.get("landing") != f"https://freesound.org/people/{user}/sounds/{sid}/":
            p(f"freesound:{sid}: landing must be https://freesound.org/people/{user}/sounds/{sid}/")
        m = FS_PREVIEW_RE.match(src.get("preview") or "")
        if not m or int(m.group(1)) != sid // 1000 or int(m.group(2)) != sid:
            p(f"freesound:{sid}: preview must be https://cdn.freesound.org/previews/{sid // 1000}/{sid}_<user id>-hq.ogg (or .mp3)")
        if not isinstance(src.get("sha256"), str) or not SHA256_RE.match(src["sha256"]):
            p(f"freesound:{sid}: sha256 must be 64 lowercase hex digits")
        if not is_int(src.get("bytes")) or not 0 < src["bytes"] <= MAX_PREVIEW_BYTES:
            p(f"freesound:{sid}: bytes must be 1..{MAX_PREVIEW_BYTES}")

    def _check_clip(self, c, src, key, p):
        cid = c.get("id") if isinstance(c, dict) else None
        if not isinstance(cid, str) or not ID_RE.match(cid):
            p(f"{key}: clip id {cid!r} does not match {ID_RE.pattern}")
            return False
        if cid in self.clips:
            p(f"duplicate clip id {cid}")
            return False
        ok = True

        def bad(msg):
            nonlocal ok
            ok = False
            p(f"{cid}: {msg}")

        group, name = cid.split("/")
        typ, kind = c.get("type"), c.get("kind")
        if typ not in TYPES:
            bad(f"type {typ!r} is not one of the {len(TYPES)} types")
            return False
        if group != typ:
            bad(f"the id's group must be its type ({typ})")
        if kind not in KINDS:
            bad(f"kind {kind!r} is not one of {', '.join(KINDS)}")
            return False
        tkind = TYPES[typ]["kind"]
        if (kind in ("loop", "speech") or tkind in ("loop", "speech")) and kind != tkind:
            bad(f"kind {kind} does not fit type {typ} (loop and speech kinds belong to their types only)")
        if src["source"] == "kenney" and not name.startswith("kenney_"):
            bad("a Kenney clip's name starts with kenney_")
        if src["source"] == "freesound" and not (name == f"fs_{src.get('id')}" or name.startswith(f"fs_{src.get('id')}_")):
            bad(f"a Freesound clip's name starts with fs_{src.get('id')}_")
        tags = c.get("tags")
        if not isinstance(tags, list) or len(tags) > 12 or len(set(tags)) != len(tags) or not all(isinstance(t, str) and TAG_RE.match(t) for t in tags):
            bad("tags must be up to 12 distinct [a-z0-9_] words")
        else:
            noise = [t for t in tags if t in NON_SOUND_TAGS or QUALITY_TAG_RE.match(t)]
            if noise:
                bad(f"tags {noise} name a licence, a file format, a quality or nothing at all, not the sound")
        if any(w in FORMAT_WORDS for w in name.split("_")):
            bad("the id names a file format (an uploader's file extension), not the sound")
        desc = c.get("description")
        if not _check_text(desc, 120):
            bad("description must be 1-120 printable characters")
        elif sum(ch.isalpha() for ch in desc) < 3 or FILENAME_RE.search(desc):
            bad(f"description {desc!r} must name the sound in words, not a number or a file name")
        ev = c.get("event")
        if ev is not None and (kind in ("loop", "speech") or not is_int(ev) or not 1 <= ev <= MAX_CLIPS_PER_SOURCE):
            bad(f"event is for one-shot kinds, 1..{MAX_CLIPS_PER_SOURCE}")
        seg = c.get("segment_s")
        if seg is not None and not (isinstance(seg, list) and len(seg) == 2 and is_num(seg[0]) and is_num(seg[1]) and seg[0] >= 0 and seg[1] > 0):
            bad("segment_s must be [start, length] in seconds")
        if kind == "loop" and seg is None:
            bad("a loop clip needs segment_s")
        xf = c.get("crossfade_ms")
        if xf is not None and (kind != "loop" or not is_num(xf) or not 1 <= xf <= 2000):
            bad("crossfade_ms is for loop clips, 1..2000")
        # the curator's recorded decision on a clip the semantic stage marks "review" (§2.11.9)
        if "review_note" in c and not _check_text(c["review_note"], 300):
            bad("review_note must be 1-300 printable characters (the curator's decision in one sentence)")
        extra = set(c) - {"id", "type", "kind", "tags", "description", "event", "segment_s", "crossfade_ms", "review_note"}
        if extra:
            bad(f"unknown keys {sorted(extra)}")
        return ok

    def used_packs(self, clip_ids=None):
        ids = self.clips if clip_ids is None else clip_ids
        return sorted({self.clips[i][1]["pack"] for i in ids if self.clips[i][1]["source"] == "kenney"})


def essentials_members():
    """(pack, member) -> essentials id, from the Phase 1 selection (no library clip may reuse one)."""
    if not ESSENTIALS_SELECTION.is_file():
        return {}
    sel = load_json(ESSENTIALS_SELECTION, MAX_JSON_BYTES, "essentials selection")
    return {(c["pack"], c["member"]): c["id"] for c in sel.get("clips", [])}


def zip_path(cache, pack_id):
    return Path(cache) / f"kenney_{pack_id}.zip"


def fs_meta_path(cache, sid):
    return Path(cache) / "freesound" / f"{sid}.json"


def fs_preview_path(cache, sid, ext):
    return Path(cache) / "freesound" / f"{sid}-hq.{ext}"


def preview_ext(url):
    return url.rsplit(".", 1)[1]


def clip_file(work, cid, suffix):
    return Path(work) / (cid + suffix)


# ---------------------------------------------------------------------------------------------
# Freesound landing pages

FACT_LABELS = ("Type", "Duration", "File size", "Sample rate", "Bit depth", "Channels")


class _LandingParser(html.parser.HTMLParser):
    """Collects the few fields the pipeline uses from a sound's public page; everything else is ignored."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.og = {}
        self.player = None
        self.licence_links = []
        self.cc_links = []
        self.explicit = False
        self.title = None
        self.tags = []
        self.facts = {}
        self.description = None
        self._h1 = None
        self._tag = None
        self._p = None
        self._label = None
        self._desc = None
        self._desc_depth = 0

    def handle_starttag(self, tag, attrs):
        a = {k: (v or "") for k, v in attrs}
        cls = a.get("class", "").split()
        # the uploader's own text about the sound: the first soundDescriptionSection, kept as untrusted text (P5 §5.7)
        if self._desc is not None:
            if tag == "div":
                self._desc_depth += 1
            elif tag in ("p", "br", "li"):
                self._desc.append(" ")
        elif tag == "div" and a.get("id") == "soundDescriptionSection" and self.description is None:
            self._desc, self._desc_depth = [], 1
        if "explicit-sound-blocker" in cls:
            self.explicit = True
        href = a.get("href", "")
        if href and (urllib.parse.urlsplit(href.strip()).hostname or "").lower().endswith("creativecommons.org"):
            self.cc_links.append(href)
        if tag == "meta" and a.get("property", "").startswith("og:"):
            self.og.setdefault(a["property"], a.get("content", ""))
        elif tag == "div" and "bw-player" in cls and "data-sound-id" in a and self.player is None:
            self.player = a
        elif tag == "a":
            if a.get("title") == "Go to the full license text":
                self.licence_links.append(a.get("href", ""))
            m = re.match(r"^/browse/tags/([^/?#]{1,64})/$", a.get("href", ""))
            if m:
                self._tag = m.group(1)
        elif tag == "h1" and self.title is None:
            self._h1 = []
        elif tag == "p":
            self._p = (cls, [])

    def handle_endtag(self, tag):
        if self._desc is not None and tag == "div":
            self._desc_depth -= 1
            if self._desc_depth == 0:
                self.description = "".join(self._desc)
                self._desc = None
        if tag == "h1" and self._h1 is not None:
            self.title = " ".join("".join(self._h1).split())
            self._h1 = None
        elif tag == "a" and self._tag is not None:
            self.tags.append(urllib.parse.unquote(self._tag))
            self._tag = None
        elif tag == "p" and self._p is not None:
            cls, parts = self._p
            text = " ".join("".join(parts).split())
            if "text-grey" in cls and text in FACT_LABELS:
                self._label = text
            elif self._label and "no-margins" in cls:
                self.facts.setdefault(self._label, text[:64])
                self._label = None
            self._p = None

    def handle_data(self, data):
        if self._desc is not None:
            self._desc.append(data)
        if self._h1 is not None:
            self._h1.append(data)
        if self._p is not None:
            self._p[1].append(data)


def _clean_text(s, limit):
    s = " ".join("".join(ch for ch in (s or "") if ch.isprintable()).split())
    return s[:limit]


def parse_landing(text):
    """The metadata kept from a landing page. Raises BuildError when the page is not a sound page."""
    lp = _LandingParser()
    lp.feed(text)
    lp.close()
    if lp.player is None or not lp.player.get("data-sound-id", "").isdigit():
        raise BuildError("the page has no sound player (not a Freesound sound page)")

    def num(v):
        try:
            f = float(v)
            return f if math.isfinite(f) else None
        except (TypeError, ValueError):
            return None

    return {
        "id": int(lp.player["data-sound-id"]),
        "username": _clean_text(lp.og.get("og:audio:artist"), 150),
        "title": _clean_text(lp.og.get("og:audio:title") or lp.title, 200),
        "landing": _clean_text(lp.og.get("og:url"), 300),
        "licence_links": [_clean_text(h, 300) for h in lp.licence_links],
        "cc_links": sorted({_clean_text(h, 300) for h in lp.cc_links}),
        "explicit": lp.explicit,
        "remix_group": lp.player.get("data-remix-group") == "true",
        "duration_s": num(lp.player.get("data-duration")),
        "samplerate": num(lp.player.get("data-samplerate")),
        "file_type": lp.facts.get("Type"),
        "channels": lp.facts.get("Channels"),
        "tags": sorted({_clean_text(t, 64).lower() for t in lp.tags if _clean_text(t, 64)})[:32],
        "description": _clean_text(lp.description, MAX_DESCRIPTION_CHARS) or None,
    }


def normalise_licence(url):
    u = urllib.parse.urlsplit((url or "").strip())
    if u.scheme not in ("http", "https"):
        return None
    path = u.path if u.path.endswith("/") else u.path + "/"
    return (u.hostname or "").lower() + path


def freesound_problems(meta, sid, username=None, landing=None):
    """Why a sound may not be used (§2.11.2 as revised by decision 5); empty when it may."""
    problems = []
    if meta.get("id") != sid:
        problems.append(f"the page is for sound {meta.get('id')}, not {sid}")
    if username is not None and meta.get("username") != username:
        problems.append(f"the page names user {meta.get('username')!r}, the selection {username!r}")
    if landing is not None and meta.get("landing") != landing:
        problems.append(f"the page's canonical URL is {meta.get('landing')!r}, the selection's {landing!r}")
    links = meta.get("licence_links") or []
    if len(links) != 1:
        problems.append(f"the page has {len(links)} licence links, expected exactly one")
    elif normalise_licence(links[0]) != CC0_DEED:
        problems.append(f"licence is {links[0]}, not CC0 ({CC0_DEED})")
    others = [h for h in meta.get("cc_links") or [] if normalise_licence(h) != CC0_DEED]
    if others:
        problems.append(f"the page also links another licence ({', '.join(others[:3])})")
    if meta.get("explicit"):
        problems.append("marked explicit")
    if meta.get("remix_group"):
        problems.append("in a remix group (a remix may carry its sources' licences)")
    return problems


def preview_magic_ok(data, ext):
    if ext == "ogg":
        return data.startswith(b"OggS")
    return data.startswith(b"ID3") or (len(data) > 1 and data[0] == 0xFF and (data[1] & 0xE0) == 0xE0)


# ---------------------------------------------------------------------------------------------
# fetch

class Net:
    """The two polite fetchers and freesound.org's robots.txt."""

    def __init__(self, offline):
        self.kenney = PoliteFetcher((KENNEY_HOST,), USER_AGENT, min_interval=REQUEST_INTERVAL_S, offline=offline)
        self.freesound = PoliteFetcher(FREESOUND_HOSTS, USER_AGENT, min_interval=REQUEST_INTERVAL_S, offline=offline)
        self._robots = None

    def requests(self):
        return self.kenney.requests + self.freesound.requests

    def page(self, url):
        if self._robots is None:
            text = self.freesound.get(ROBOTS_URL, MAX_ROBOTS_BYTES).decode("utf-8", errors="replace")
            rp = urllib.robotparser.RobotFileParser()
            rp.parse(text.splitlines())
            self._robots = rp
        if not self._robots.can_fetch(ROBOTS_AGENT, url):
            raise BuildError(f"freesound.org's robots.txt disallows {url} for {ROBOTS_AGENT}")
        return self.freesound.get(url, MAX_PAGE_BYTES)


def fetch_pack(pack, cache, net):
    dest = zip_path(cache, pack["id"])
    if dest.is_file() and dest.stat().st_size == pack["bytes"] and sha256(dest.read_bytes()) == pack["sha256"]:
        return f"{dest.name} cached, SHA-256 ok"
    data = net.kenney.get(pack["url"], MAX_ZIP_BYTES)
    digest = sha256(data)
    if digest != pack["sha256"] or len(data) != pack["bytes"]:
        if dest.exists():
            dest.unlink()
        raise BuildError(f"{pack['id']}: got {len(data)} bytes with SHA-256 {digest}, expected {pack['bytes']} bytes "
                         f"with {pack['sha256']}; nothing was kept. The pack changed upstream: review it before updating the selection")
    write_if_changed(dest, data)
    return f"{dest.name} {len(data)} bytes, SHA-256 ok"


def fetch_meta(sid, landing, cache, net, refresh):
    """The landing page's metadata, from the cache unless refresh (or absent)."""
    path = fs_meta_path(cache, sid)
    if path.is_file() and not refresh:
        cached = load_json(path, MAX_JSON_BYTES, "cached Freesound metadata")
        if isinstance(cached, dict) and isinstance(cached.get("meta"), dict):
            return cached["meta"], True
    page = net.page(landing)
    meta = parse_landing(page.decode("utf-8", errors="replace"))
    meta["page_sha256"] = sha256(page)
    record = {"fetched_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"), "page_url": landing, "meta": meta}
    write_if_changed(path, json_text(record).encode("utf-8"))
    return meta, False


def fetch_preview(src, cache, net):
    ext = preview_ext(src["preview"])
    dest = fs_preview_path(cache, src["id"], ext)
    if dest.is_file() and dest.stat().st_size == src["bytes"] and sha256(dest.read_bytes()) == src["sha256"]:
        return "cached, SHA-256 ok"
    data = net.freesound.get(src["preview"], MAX_PREVIEW_BYTES)
    digest = sha256(data)
    if digest != src["sha256"] or len(data) != src["bytes"]:
        if dest.exists():
            dest.unlink()
        raise BuildError(f"freesound:{src['id']}: preview {src['preview']} is {len(data)} bytes with SHA-256 {digest}, "
                         f"expected {src['bytes']} bytes with {src['sha256']}; nothing was kept. Freesound regenerated "
                         f"the preview: review it and pin it again")
    if not preview_magic_ok(data, ext):
        raise BuildError(f"freesound:{src['id']}: {src['preview']} is not an {ext.upper()} file")
    write_if_changed(dest, data)
    return f"{len(data)} bytes, SHA-256 ok"


def cmd_fetch(args, sel=None):
    sel = sel or Selection(args.selection)
    net = Net(args.offline)
    errors = []
    for pid in sel.used_packs():
        try:
            print(f"fetch: {fetch_pack(sel.packs[pid], args.cache, net)}")
        except (BuildError, OSError, urllib.error.URLError) as e:
            errors.append(f"{pid}: {e}")
    for src in sel.sources:
        if src["source"] != "freesound":
            continue
        key = source_key(src)
        try:
            meta, cached = fetch_meta(src["id"], src["landing"], args.cache, net, args.refresh)
            problems = freesound_problems(meta, src["id"], src["username"], src["landing"])
            if problems:
                raise BuildError("; ".join(problems) + ": move it to rejected")
            print(f"fetch: {key} metadata {'cached' if cached else 'fetched'}, CC0; preview {fetch_preview(src, args.cache, net)}")
        except (BuildError, OSError, urllib.error.URLError) as e:
            errors.append(f"{key}: {e}")
    print(f"fetch: {net.requests()} network request(s)")
    if errors:
        raise BuildError("fetch failed:\n  " + "\n  ".join(errors))


# ---------------------------------------------------------------------------------------------
# seed: pin Freesound ids from opus-sound-layer's library.json

def slug(text, limit):
    s = re.sub(r"([a-z])([A-Z])", r"\1_\2", text or "")
    s = re.sub(r"[^a-z0-9]+", "_", s.lower()).strip("_")
    return s[:limit].strip("_")


def read_opus_file(path, pin):
    """The opus seed file's bytes, refused before any of it is parsed when it is not the pinned file
    (the selection's seed, §2.14 H4), then parsed as untrusted JSON."""
    p = Path(path)
    if not p.is_file():
        raise BuildError(f"opus library {p} is missing")
    if p.stat().st_size > MAX_SEED_BYTES:
        raise BuildError(f"opus library {p} is larger than {MAX_SEED_BYTES} bytes")
    raw = p.read_bytes()
    if pin is not None and (len(raw) != pin["bytes"] or sha256(raw) != pin["sha256"]):
        raise BuildError(f"{p} is {len(raw)} bytes with SHA-256 {sha256(raw)}, but the selection's seed pin is {pin['bytes']} "
                         f"bytes with {pin['sha256']} ({pin['repo']} at {pin['commit']}, {pin['path']}): refusing to read it. "
                         f"Extract the pinned file again (git show <commit>:{pin['path']})")
    try:
        return json.loads(raw.decode("utf-8")), raw
    except (ValueError, UnicodeDecodeError) as e:
        raise BuildError(f"opus library {p} is not valid JSON ({e})")


def read_opus_seeds(data, path):
    """opus's Freesound entries, validated field by field; anything unexpected is dropped with a reason."""
    items = data.get("freesound") if isinstance(data, dict) else None
    if not isinstance(items, list):
        raise BuildError(f"{path}: no freesound list")
    seeds, dropped = [], []
    for it in items:
        if not isinstance(it, dict):
            dropped.append(("?", "not an object"))
            continue
        sid = str(it.get("id", "")) if is_int(it.get("id")) or isinstance(it.get("id"), str) else "?"
        mu = FS_SEED_URL_RE.match(it["url"]) if isinstance(it.get("url"), str) else None
        ml = FS_SEED_LANDING_RE.match(it["landing"]) if isinstance(it.get("landing"), str) else None
        if not (sid.isascii() and sid.isdigit()) or not mu or not ml or mu.group(2) != sid or ml.group(2) != sid or int(mu.group(1)) != int(sid) // 1000:
            dropped.append((sid, "id, url and landing do not agree"))
            continue
        typ = it.get("type")
        if not isinstance(typ, str) or typ not in TYPES or TYPES[typ]["kind"] in ("loop", "speech"):
            dropped.append((sid, f"type {typ!r:.40} is not a one-shot type"))
            continue
        k = it.get("k", 1)
        keep = it.get("keep")
        if not is_int(k) or not 1 <= k <= 8:
            dropped.append((sid, "bad k"))
            continue
        if keep is None:
            keep = list(range(k))
        if not isinstance(keep, list) or not keep or not all(is_int(i) and 0 <= i < k for i in keep):
            dropped.append((sid, "bad keep"))
            continue
        tags = []
        for t in it["tags"] if isinstance(it.get("tags"), list) else []:
            t = t.lower() if isinstance(t, str) else ""
            if TAG_RE.match(t) and t not in tags:
                tags.append(t)
        seeds.append({"id": int(sid), "user_id": int(mu.group(3)), "type": typ, "k": k,
                      "keep": sorted(set(keep)), "tags": tags[:8]})
    return seeds, dropped


def preview_url(sid, uid, ext):
    return f"https://cdn.freesound.org/previews/{sid // 1000}/{sid}_{uid}-hq.{ext}"


def seed_pin(args):
    """The selection's seed pin; a missing default selection (before curation writes it) has none."""
    path = Path(args.selection)
    if not path.is_file() and path == DEFAULT_SELECTION:
        print(f"seed: no selection at {path}: the opus file is not checked against a seed pin", file=sys.stderr)
        return None
    pin = Selection(path).seed
    if pin is None:
        print(f"seed: {path.name} has no seed pin: the opus file is not checked against one", file=sys.stderr)
    return pin


def cmd_seed(args):
    pin = seed_pin(args)
    opus, raw = read_opus_file(args.opus, pin)
    if pin is not None:
        print(f"seed: {args.opus} matches the seed pin ({pin['repo']} at {pin['commit'][:12]}, {len(raw)} bytes)")
    seeds, dropped = read_opus_seeds(opus, args.opus)
    landings = opus_landings(opus)
    for sid, why in dropped:
        print(f"seed: dropped opus entry {sid}: {why}", file=sys.stderr)
    if args.type:
        seeds = [s for s in seeds if s["type"] in args.type]
    if args.ids:
        want = [int(x) for x in args.ids.split(",") if x.strip()]
        by_id = {s["id"]: s for s in seeds}
        missing = [i for i in want if i not in by_id]
        if missing:
            raise BuildError(f"ids not in the opus list (or dropped): {missing}")
        seeds = [by_id[i] for i in want]
    if args.limit:
        seeds = seeds[: args.limit]
    net = Net(offline=False)
    sources, rejected = [], []
    for s in seeds:
        sid = s["id"]
        try:
            # The page is reached through the opus landing URL (rebuilt from its validated parts);
            # the username and canonical URL that get pinned are the page's own.
            if sid not in landings:
                raise BuildError(f"no landing page for {sid}")
            meta, _ = fetch_meta(sid, landings[sid], args.cache, net, refresh=False)
            user = meta.get("username") or ""
            landing = f"https://freesound.org/people/{user}/sounds/{sid}/"
            problems = freesound_problems(meta, sid, landing=landing)
            if not FS_USER_RE.match(user):
                problems.append(f"bad username {user!r}")
            if problems:
                rejected.append({"source": f"freesound:{sid}", "reason": "; ".join(problems)})
                print(f"seed: freesound:{sid} rejected: {'; '.join(problems)}")
                continue
            data, url = None, None
            for ext in ("ogg", "mp3"):
                url = preview_url(sid, s["user_id"], ext)
                path = fs_preview_path(args.cache, sid, ext)
                if path.is_file():
                    data = path.read_bytes()
                    break
                try:
                    data = net.freesound.get(url, MAX_PREVIEW_BYTES)
                except urllib.error.HTTPError as e:
                    if e.code == 404 and ext == "ogg":
                        continue
                    raise
                if not preview_magic_ok(data, ext):
                    raise BuildError(f"{url} is not an {ext.upper()} file")
                write_if_changed(path, data)
                break
            title = meta.get("title") or ""
            base = f"fs_{sid}_{slug(title, 40)}".rstrip("_")
            clips = []
            for i in s["keep"]:
                suffix = f"_{i + 1}" if len(s["keep"]) > 1 else ""
                clips.append({"id": f"{s['type']}/{base[:48 - len(suffix)].rstrip('_')}{suffix}", "type": s["type"],
                              "kind": TYPES[s["type"]]["kind"], "tags": s["tags"],
                              "description": _clean_text(title, 120) or f"Freesound sound {sid}", "event": i + 1})
            sources.append({"source": "freesound", "id": sid, "username": user, "landing": landing, "preview": url,
                            "sha256": sha256(data), "bytes": len(data), "events": s["k"], "clips": clips})
            print(f"seed: freesound:{sid} CC0 by {user}, {preview_ext(url)} preview {len(data)} bytes, {len(clips)} clip(s)")
        except (BuildError, OSError, urllib.error.URLError, KeyError, TypeError) as e:
            rejected.append({"source": f"freesound:{sid}", "reason": f"seed failed: {e}"})
            print(f"seed: freesound:{sid} failed: {e}", file=sys.stderr)
    out = {"format": SEED_FORMAT, "version": 1,
           "note": "Freesound ids pinned from opus-sound-layer's library.json (untrusted ideas); review before copying into the selection.",
           "opus": {"sha256": sha256(raw), "bytes": len(raw), "pinned": pin is not None},
           "sources": sorted(sources, key=lambda s: s["id"]), "rejected": sorted(rejected, key=lambda r: r["source"])}
    write_if_changed(Path(args.seed_out), manifest_text(out).encode("utf-8"))
    print(f"seed: {len(sources)} pinned, {len(rejected)} rejected, {net.requests()} network request(s); wrote {args.seed_out}")


def opus_landings(data):
    """Each opus entry's landing URL, rebuilt from its validated parts (never used verbatim)."""
    out = {}
    for it in (data.get("freesound") if isinstance(data, dict) else None) or []:
        m = FS_SEED_LANDING_RE.match(it.get("landing") or "") if isinstance(it, dict) and isinstance(it.get("landing"), str) else None
        if m:
            out[int(m.group(2))] = f"https://freesound.org/people/{m.group(1)}/sounds/{m.group(2)}/"
    return out


# ---------------------------------------------------------------------------------------------
# DSP (float64 numpy; deterministic)

def dbfs(v):
    return LEVEL_FLOOR if v <= 0.0 else max(LEVEL_FLOOR, 20.0 * math.log10(v))


def envelope(y, win_s):
    """RMS over a centred sliding window of win_s (zero outside the signal)."""
    n = max(1, int(win_s * SR))
    c = np.concatenate(([0.0], np.cumsum(y * y)))
    i = np.arange(len(y))
    lo = np.clip(i - n // 2, 0, len(y))
    hi = np.clip(i + (n - 1) // 2 + 1, 0, len(y))
    return np.sqrt(np.maximum(c[hi] - c[lo], 0.0) / n)


def event_span(y, t, kind):
    """[start, end) of the strongest event in mono y, by the kind's rule (§2.11.3 step 4)."""
    n = len(y)
    hi_len = int(t["win"][1] * SR)
    if kind == "riser":
        env = envelope(y, 0.03)
        p = int(np.argmax(env))
        thr = env[p] * 10 ** (-30 / 20)
        limit = hi_len - int(0.4 * SR)
        a = p
        if limit > 0:
            lo = max(0, p - limit)
            stop = np.nonzero(env[lo:p][::-1] <= thr)[0]
            a = p - int(stop[0]) if len(stop) else lo
        hi = min(n - 1, p + int(0.6 * SR))
        stop = np.nonzero(env[p + 1:hi + 1] <= env[p] * 0.1)[0]
        b = p + int(stop[0]) if len(stop) else hi
        return a, b + 1
    if kind == "swell":
        env = envelope(y, 0.02)
        p = int(np.argmax(env))
        thr = float(env[p]) * 0.1
        e = env.tolist()
        a = b = p
        while b - a < hi_len:
            left = e[a - 1] if a > 0 else 0.0
            right = e[b + 1] if b < n - 1 else 0.0
            if left <= thr and right <= thr:
                break
            if left >= right:
                a -= 1
            else:
                b += 1
        return a, b + 1
    # transient, tonal and glitch; only a transient (not layered) also ends at the dip before a second hit (§2.11.3
    # step 4; §5.2 decision 4 gave it to tonal too, which cut jingles and multi-tone blips to their first note)
    fine = envelope(y, 0.002)
    smooth = envelope(y, 0.015)
    p = int(np.argmax(fine))
    ps = float(smooth[p])
    if kind == "tonal":
        # a phrase (a jingle, a multi-tone blip) is kept whole: from 5 ms before the first moment its 15 ms envelope is
        # within 40 dB of the peak's to the last such moment, at most the window long. The first-drop end below would
        # stop at the silence between two notes (P5 §5.6.3).
        above = np.nonzero(smooth > ps * 10 ** (-40 / 20))[0]
        a = max(0, int(above[0]) - int(0.005 * SR))
        return a, min(n, int(above[-1]) + 1, a + hi_len)
    lo = max(0, p - int(0.05 * SR))
    stop = np.nonzero(fine[lo + 1:p + 1][::-1] <= fine[p] * 0.1)[0]
    a = p - int(stop[0]) if len(stop) else lo
    a = max(0, a - int(0.005 * SR))
    tail = ps * 10 ** (-40 / 20)
    idx = np.arange(p, n)
    s = smooth[p:]
    brk = np.nonzero(~((idx < n - 1) & (s > tail) & (idx - a < hi_len)))[0]
    end = p + int(brk[0])
    if kind == "transient" and not t.get("layered") and end > p:
        span = s[: end - p]
        low = np.minimum.accumulate(span)
        rel = np.arange(end - p)
        hit = np.nonzero((rel > int(0.01 * SR)) & (span > np.maximum(4.0 * low, 0.1 * ps)))[0]
        if len(hit):
            b = p + int(hit[0])
            end = p + int(np.argmin(smooth[p:b]))
    return a, end


def extended_end(mono, a, e, t, kind, limit, level_db=TAIL_EXTEND_DB):
    """A later end for the cut [a, e) of mono: where the kind's envelope (15 ms; 20 ms for a swell) first falls level_db
    under the event's peak, at most limit; a transient (not layered) stops at the dip before a second hit (the level
    climbing over 4x its lowest point since e and over 10% of the peak). e when there is no later end."""
    if limit <= e:
        return e
    env = envelope(mono[a:limit], 0.02 if kind == "swell" else 0.015)
    peak = float(env[:e - a].max())
    rest = env[e - a:]
    below = np.nonzero(rest < peak * 10 ** (level_db / 20))[0]
    end = int(below[0]) if len(below) else len(rest)
    if kind == "transient" and not t.get("layered") and end > 0:
        span = rest[:end]
        hit = np.nonzero(span > np.maximum(4.0 * np.minimum.accumulate(span), 0.1 * peak))[0]
        if len(hit):
            end = int(np.argmin(span[:int(hit[0])]))
    return e + end


def cut_events(y, t, kind, k):
    """Up to k events, strongest first, each at least -12 dB of the first's peak; returned in time order."""
    y = y.copy()
    out, first = [], None
    margin = int(0.05 * SR)
    for _ in range(k):
        pk = float(np.max(np.abs(y))) if len(y) else 0.0
        if pk <= 1e-6 or (first is not None and pk < first * 0.25):
            break
        a, b = event_span(y, t, kind)
        if b - a < int(0.01 * SR):
            break
        out.append((a, b))
        if first is None:
            first = pk
        y[max(0, a - margin):b + margin] = 0.0
    return sorted(out)


def band_filter(x, t):
    lp = min(t["lp"], LP_MAX_HZ)
    sos_hp = signal.butter(4, t["hp"], "highpass", fs=SR, output="sos")
    sos_lp = signal.butter(4, lp, "lowpass", fs=SR, output="sos")
    return signal.sosfilt(sos_lp, signal.sosfilt(sos_hp, x, axis=0), axis=0), {"highpass_hz": t["hp"], "lowpass_hz": lp}


def fades(y, t, release=None):
    """Linear fades (5 ms in; 5 ms out, 20 ms for designed_end) and the layered tail fade; a tail repair's release is
    (shape, ms): a linear or a raised-cosine fade-out of that length."""
    n = len(y)
    fi = min(int(0.005 * SR), n // 4)
    shape, ms = release or ("linear", 20.0 if t.get("designed_end") else 5.0)
    fo = min(int(ms / 1000 * SR), n // 4)
    if fi > 0:
        y[:fi] *= np.linspace(0.0, 1.0, fi)
    if fo > 0:
        ramp = np.linspace(1.0, 0.0, fo)
        y[n - fo:] *= ramp if shape == "linear" else np.sin(ramp * np.pi / 2) ** 2
    tail_fade = False
    if t.get("layered") and n:
        e = envelope(y, 0.01)
        edge = max(1, n // 10)
        if e[-edge:].max() > e.max() * 0.1:
            m = int(n * 0.3)
            if m > 0:
                y[n - m:] *= np.cos(np.linspace(0.0, np.pi / 2, m)) ** 2
                tail_fade = True
    return fo, {"fade_in_ms": round(fi / SR * 1000, 1), "fade_out_ms": round(fo / SR * 1000, 1), "tail_fade": tail_fade}


def trim_bounds(y, fade_out):
    """Head to at most 5 ms before the first sample at or above -60 dBFS, less the pre-roll the encode adds; tail to
    the last plus the fade."""
    lev = np.abs(y) if y.ndim == 1 else np.abs(y).max(axis=1)
    loud = np.nonzero(lev >= 10 ** (SILENCE_DBFS / 20))[0]
    if not len(loud):
        return None
    return max(0, int(loud[0]) - (int(HEAD_KEEP_S * SR) - PREROLL_FRAMES)), min(len(y), int(loud[-1]) + 1 + fade_out)


def seam_window(rate):
    """W = round(0.010 rate) (C's lround, as the editor rounds)."""
    return int(math.floor(SEAM_WINDOW_S * rate + 0.5))


def seam_step(y, rate):
    """Phase 1's seam step as built (A28, see LOOP_SEAM_STEP_MAX_DB): per channel the wrap's jump over the RMS
    of the W-1 first differences inside each of the last and the first W samples (all N-1 when N < 2W), that
    RMS floored at 2^-15; the largest over the channels, floored at -120 dB."""
    y = as2d(y)
    n = len(y)
    if n < 2:
        return LEVEL_FLOOR
    w = seam_window(rate)
    if n < 2 * w:
        d = np.diff(y, axis=0)
    else:
        d = np.vstack([np.diff(y[n - w:], axis=0), np.diff(y[:w], axis=0)])
    ref = np.maximum(SEAM_REFERENCE_FLOOR, np.sqrt(np.mean(d * d, axis=0)))
    step = LEVEL_FLOOR
    for jump, r in zip(np.abs(y[0] - y[n - 1]), ref):
        if jump > 0.0:
            step = max(step, 20.0 * math.log10(float(jump) / float(r)))
    return max(LEVEL_FLOOR, step)


def seam_measures(y, rate):
    """Phase 1 §2.7.2's loop seam as built: (step, level) in dB; the level is the RMS of the last W samples
    minus that of the first W, all channels pooled."""
    y = as2d(y)
    edge = max(1, min(len(y), seam_window(rate)))
    level = (dbfs(float(np.sqrt(np.mean(y[-edge:] ** 2)))) - dbfs(float(np.sqrt(np.mean(y[:edge] ** 2)))))
    return seam_step(y, rate), level


def wrap_rms_db(y, rate):
    """§3.1 D-7: the RMS (dB) of the last and the first round(0.010 rate) samples together, all channels pooled,
    less the RMS of the whole clip."""
    y = as2d(y)
    w = max(1, min(len(y) // 2, seam_window(rate)))
    wrap = np.concatenate([y[-w:], y[:w]])
    return dbfs(float(np.sqrt(np.mean(wrap * wrap)))) - dbfs(float(np.sqrt(np.mean(y * y))))


def seam_excess(step, level, margin):
    return (max(0.0, step - (LOOP_SEAM_STEP_MAX_DB - margin))
            + max(0.0, abs(level) - (LOOP_SEAM_LEVEL_MAX_DB - margin)))


def loop_head(x, start, length, xf, count):
    """The first count samples of loop_crossfade(x, start, length, xf), without building the whole loop."""
    i = np.arange(min(count, length))
    h = x[start + i].copy()
    m = i < xf
    if m.any():
        k = i[m]
        h[m] = x[start + k] * np.sin(np.pi * k / (2 * xf))[:, None] + x[start + length + k] * np.cos(np.pi * k / (2 * xf))[:, None]
    return h


def seam_candidates(x, start, length, xf):
    """Length changes within +/-SEAM_SEARCH_S, best first: by how far the candidate loop's seam step (Phase 1's
    metric on its last and first W samples) misses the limit with SEAM_MARGIN_DB to spare, then by the size of the
    change. The seam level is not ranked on: moving the end to raise or lower a 10 ms window would hide a real
    level step at the wrap, so the level is only checked, on the decoded file, as the editor does."""
    r = int(SEAM_SEARCH_S * SR)
    w = seam_window(SR)
    scored = []
    for d in range(-r, r + 1):
        n = length + d
        if n <= xf or start + n + xf > len(x) or n < 2 * w:
            continue
        wrap = np.vstack([loop_head(x, start, n, xf, w), x[start + n - w:start + n]])
        step = seam_step(wrap, SR)
        scored.append((round(max(0.0, step - (LOOP_SEAM_STEP_MAX_DB - SEAM_MARGIN_DB)), 9), abs(d), d))
    return [d for _, _, d in sorted(scored)] or [0]


def seam_retry(out, filtered, start, length, xf, candidates, quality, clip_id):
    """The first SEAM_TRIES candidates encoded at the gain the gain loop chose (the first is out itself): the one whose
    decoded seam step is lowest, its seam level and true peak passing, ties to the earlier candidate (out when none
    passes); while that step is over LIBRARY_SEAM_STEP_MAX_DB, up to SEAM_MAX_TRIES. Returns the encoding, its length
    change and the number of encodes."""
    def score(dec):
        step, level = seam_measures(dec, SR)
        return step if seam_excess(step, level, 0.0) == 0.0 else None

    best, best_d, best_step = out, candidates[0], score(out["decoded"])
    tries = 1
    for d in candidates[1:SEAM_MAX_TRIES]:
        if tries >= SEAM_TRIES and best_step is not None and best_step <= LIBRARY_SEAM_STEP_MAX_DB:
            break
        y = loop_crossfade(filtered, start, length + d, xf) * 10 ** (out["gain_db"] / 20)
        data, dec = encoded_clip(y, quality, clip_id)
        tries += 1
        step = score(dec)
        if step is not None and (best_step is None or step < best_step) and peak_guard(dec) <= CEILING_DBTP:
            best, best_d, best_step = dict(out, y=y, ogg=data, decoded=dec, bounds=(0, len(y))), d, step
    return best, best_d, tries


def loop_crossfade(x, start, length, xf):
    """y[n] = x[s+n] for n >= C, x[s+n] sin(pi n/2C) + x[s+L+n] cos(pi n/2C) below: the wrap is continuous."""
    y = x[start:start + length].copy()
    if xf:
        n = np.arange(xf)
        w_in = np.sin(np.pi * n / (2 * xf))[:, None]
        w_out = np.cos(np.pi * n / (2 * xf))[:, None]
        y[:xf] = x[start:start + xf] * w_in + x[start + length:start + length + xf] * w_out
    return y


def as2d(y):
    return y if y.ndim == 2 else y[:, None]


def _meter_blocks(y, block, overlap, pad):
    yy = as2d(y)
    if pad:
        z = np.zeros((int(round(0.4 * SR)), yy.shape[1]))
        yy = np.vstack([z, yy, z])
    meter = pyloudnorm.Meter(SR, block_size=block, overlap=overlap)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        value = meter.integrated_loudness(yy)
    return value, [b for b in meter.blockwise_loudness if np.isfinite(b)]


def momentary_max(y):
    """Phase 1 §5.2: 400 ms windows every 10 ms over the signal zero-padded by 400 ms, K-weighted continuously."""
    if not len(y):
        return None
    _, blocks = _meter_blocks(y, 0.4, 0.975, True)
    return max(blocks) if blocks else None


def block_energies(y, block, hop):
    """K-weighted mean square (channels summed, weight 1) of the blocks of `block` frames every `hop` wholly inside y."""
    z = signal.lfilter(*K_HIGHPASS, signal.lfilter(*K_SHELF, as2d(y), axis=0), axis=0)
    e = np.concatenate(([0.0], np.cumsum((z * z).sum(axis=1))))
    starts = np.arange(0, len(z) - block + 1, hop)
    return (e[starts + block] - e[starts]) / block


def lufs(energy):
    energy = np.asarray(energy, dtype=np.float64)
    with np.errstate(divide="ignore"):
        return np.where(energy > 0, -0.691 + 10.0 * np.log10(np.maximum(energy, 1e-300)), -np.inf)


def integrated(y):
    """BS.1770-4 integrated loudness: 400 ms blocks every 100 ms wholly inside the signal, gated at -70 LUFS, then 10 LU
    under the mean of the blocks that passed (both strict, as the editor's meter); None under 400 ms or when no block
    passes the gates."""
    if len(y) < INTEGRATED_MIN_S * SR:
        return None
    e = block_energies(y, int(0.4 * SR), int(0.1 * SR))
    lk = lufs(e)
    passed = e[lk > GATE_ABSOLUTE_LUFS]
    if not len(passed):
        return None
    relative = float(lufs(passed.mean())) + GATE_RELATIVE_LU
    gated = e[(lk > GATE_ABSOLUTE_LUFS) & (lk > relative)]
    return float(lufs(gated.mean())) if len(gated) else None


def loudness_range(y):
    """EBU Tech 3342: 3 s windows every 100 ms wholly inside the signal, gates -70 LUFS and -20 LU, the 95th minus the
    10th of the gated values by the editor's nearest-rank indices; None under 3 s."""
    if len(y) < 3 * SR:
        return None
    lk = lufs(block_energies(y, 3 * SR, int(0.1 * SR)))
    passed = lk[lk > GATE_ABSOLUTE_LUFS]
    if len(passed) < 2:
        return 0.0
    relative = float(lufs(np.mean(10 ** ((passed + 0.691) / 10)))) + LRA_GATE_RELATIVE_LU
    g = np.sort(lk[(lk > GATE_ABSOLUTE_LUFS) & (lk > relative)])
    if len(g) < 2:
        return 0.0
    return float(g[int((len(g) - 1) * 0.95 + 0.5)] - g[int((len(g) - 1) * 0.10 + 0.5)])


def true_peak(y):
    """dBTP by the BS.1770-4 Annex 2 FIR (4x), the original samples included, 12 zeros at both ends."""
    yy = as2d(y)
    peak = float(np.abs(yy).max()) if len(yy) else 0.0
    z = np.zeros(12)
    for ch in range(yy.shape[1]):
        xp = np.concatenate([z, yy[:, ch], z])
        for h in TP_PHASES:
            peak = max(peak, float(np.abs(np.convolve(xp, h)).max()))
    return dbfs(peak)


def true_peak_fine(y):
    """dBTP by TP_FINE_FACTOR x polyphase oversampling (resample_poly, its default Kaiser window), the original samples
    included, 64 zeros at both ends; per channel."""
    yy = as2d(y)
    peak = float(np.abs(yy).max()) if len(yy) else 0.0
    z = np.zeros(64)
    for ch in range(yy.shape[1]):
        up = signal.resample_poly(np.concatenate([z, yy[:, ch], z]), TP_FINE_FACTOR, 1)
        peak = max(peak, float(np.abs(up).max()))
    return dbfs(peak)


def peak_guard(y):
    """The true peak the ceiling is held against: the 4x FIR's, or the 16x reading when that is higher (P5 §5.7)."""
    tp = true_peak(y)
    if tp > CEILING_DBTP - TP_FINE_RANGE_DB:
        tp = max(tp, true_peak_fine(y))
    return tp


def source_clipping(y, lossy):
    """Clipped samples of a decoded source, or 0 when it is not clipped (see CLIP_FRACTION_MAX above)."""
    a = np.abs(y)
    count = int(np.count_nonzero(a >= CLIP_LEVEL))
    if not count or not lossy:
        return count
    if count > CLIP_FRACTION_MAX * a.size or dbfs(float(a.max())) > LOSSY_OVER_MAX_DB:
        return count
    return 0


def decode(data):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        y, rate = sf.read(io.BytesIO(data), dtype="float64", always_2d=True)
    return y, int(rate)


def measure_decoded(y, rate, loop):
    """The manifest's measured block on a decoded file, and the unrounded values the pipeline decides on."""
    frames, channels = y.shape
    a = np.abs(y)
    peak = float(a.max()) if frames else 0.0
    loud = np.nonzero(a.max(axis=1) >= 10 ** (SILENCE_DBFS / 20))[0] if frames else []
    if len(loud):
        head_ms = loud[0] / rate * 1000.0
        tail_ms = (frames - 1 - loud[-1]) / rate * 1000.0
    else:
        head_ms = tail_ms = frames / rate * 1000.0
    raw = {"sample_peak_dbfs": dbfs(peak), "true_peak_dbtp": true_peak(y), "momentary_max_lufs": momentary_max(y),
           "integrated_lufs": integrated(y), "head_ms": float(head_ms), "tail_ms": float(tail_ms),
           "clipped": int(np.count_nonzero(a >= CLIP_LEVEL)), "duration_s": frames / rate}
    m = {"channels": int(channels), "sample_rate": int(rate), "frames": int(frames), "duration_s": round(frames / rate, 3),
         "sample_peak_dbfs": round(raw["sample_peak_dbfs"], 2), "true_peak_dbtp": round(raw["true_peak_dbtp"], 2),
         "momentary_max_lufs": rnd(raw["momentary_max_lufs"], 2), "integrated_lufs": rnd(raw["integrated_lufs"], 2),
         "head_ms": round(head_ms, 1), "tail_ms": round(tail_ms, 1)}
    if loop:
        raw["seam_step_db"], raw["seam_level_db"] = seam_measures(y, rate)
        m["seam_step_db"] = round(raw["seam_step_db"], 2)
        m["seam_level_db"] = round(raw["seam_level_db"], 2)
    return m, raw


def python_measures(x, t, loop):
    """opus's measure, restated (§2.11.4), on the prepared signal (mono downmix of a stereo one)."""
    stereo = x if x.ndim == 2 else None
    x = x.mean(axis=1) if x.ndim == 2 else x
    n = len(x)
    env2 = envelope(x, 0.002)
    env10 = envelope(x, 0.01)
    env30 = envelope(x, 0.03)
    p = int(np.argmax(env2))
    above10 = np.nonzero(env2[:p + 1] >= 0.1 * env2[p])[0]
    above90 = np.nonzero(env2[:p + 1] >= 0.9 * env2[p])[0]
    rise = (above90[0] - above10[0]) / SR * 1000 if len(above10) and len(above90) else 0.0
    spec = np.abs(np.fft.rfft(x * np.hanning(n))) ** 2
    fr = np.fft.rfftfreq(n, 1 / SR)
    tot = float(spec.sum()) + 1e-20
    band = spec[(fr >= max(t["hp"], 100)) & (fr <= t["lp"])]
    if len(band):
        band = np.maximum(band, band.max() * 1e-6 + 1e-20)
        flat = float(np.exp(np.mean(np.log(band))) / np.mean(band))
    else:
        flat = 0.0
    edge = max(int(0.005 * SR), n // 10)
    top10 = float(env10.max()) + 1e-12
    q = max(1, n // 4)
    m = {
        "dur_s": round(n / SR, 3),
        "peak_offset_s": round(p / SR, 4),
        "rise_ms": round(float(rise), 1),
        "centroid_hz": int(float((fr * spec).sum()) / tot),
        "below150": round(float(spec[fr < 150].sum()) / tot, 3),
        "above6k": round(float(spec[fr > 6000].sum()) / tot, 3),
        "flatness": round(flat, 3),
        "start_db": round(dbfs(float(env10[:edge].max()) / top10), 1),
        "end_db": round(dbfs(float(env10[-edge:].max()) / top10), 1),
        "spikiness": round(float(env2.max()) / (float(env30.max()) + 1e-12), 2),
        "build_db": round(dbfs((float(env30[-q:].mean()) + 1e-12) / (float(env30[:2 * q].mean()) + 1e-12)), 1),
    }
    if loop:
        lra = loudness_range(stereo if stereo is not None else x)
        m["lra_lu"] = rnd(lra, 2)
    return m


def python_reasons(t, kind, py, meas, src_mm):
    """The opus-style hard filters (§2.11.4) on one prepared clip; every reason, not only the first."""
    why = []
    lo, hi = t["win"]
    d = meas["duration_s"]
    if meas["sample_peak_dbfs"] > CEILING_DBTP:
        why.append(f"sample peak {meas['sample_peak_dbfs']:.2f} dBFS > {CEILING_DBTP}")
    if meas["true_peak_dbtp"] > CEILING_DBTP:
        why.append(f"true peak {meas['true_peak_dbtp']:.2f} dBTP > {CEILING_DBTP}")
    if py.get("true_peak_16x_dbtp") is not None and py["true_peak_16x_dbtp"] > CEILING_DBTP:
        why.append(f"true peak by {TP_FINE_FACTOR}x oversampling {py['true_peak_16x_dbtp']:.2f} dBTP > {CEILING_DBTP}")
    if meas["clipped"]:
        why.append(f"{meas['clipped']} clipped samples after encoding")
    if src_mm is None or src_mm < SOURCE_MOMENTARY_MIN:
        why.append(f"source momentary max {src_mm if src_mm is None else round(src_mm, 2)} LUFS < {SOURCE_MOMENTARY_MIN} (buried in noise)")
    if kind != "loop" and meas["head_ms"] > HEAD_SILENCE_MAX_MS:
        why.append(f"head silence {meas['head_ms']:.1f} ms > {HEAD_SILENCE_MAX_MS}")
    if not lo <= d <= hi:
        why.append(f"duration {d:.3f} s outside the {lo}-{hi} s window")
    if py["below150"] > 0.5 and not t.get("allow_low"):
        why.append(f"boomy (below150 {py['below150']} > 0.5)")
    if py["below150"] > 0.85 and not t.get("allow_sub"):
        why.append(f"no body above 150 Hz (below150 {py['below150']} > 0.85)")
    if py["above6k"] > t.get("hi_max", 0.4):
        why.append(f"hissy (above6k {py['above6k']} > {t.get('hi_max', 0.4)})")
    if py["flatness"] > 0.35 and not t.get("noise_ok"):
        why.append(f"noise-like (flatness {py['flatness']} > 0.35)")
    rise_max = min(t.get("rise_max", SOFT_ATTACK_MAX_MS), SOFT_ATTACK_MAX_MS)
    if kind == "transient" and py["rise_ms"] > rise_max:
        why.append(f"soft attack (rise {py['rise_ms']} ms > {rise_max:g})")
    if kind == "swell":
        if max(py["start_db"], py["end_db"]) > -12:
            why.append(f"no rise and fall (edges {py['start_db']}/{py['end_db']} dB > -12)")
        if py["spikiness"] > t.get("spiky_max", 2.5):
            why.append(f"clicks inside (spikiness {py['spikiness']} > {t.get('spiky_max', 2.5)})")
    elif kind == "riser":
        if py["start_db"] > -12:
            why.append(f"starts loud (start {py['start_db']} dB > -12)")
        if py["peak_offset_s"] < 0.7 * py["dur_s"]:
            why.append(f"peak too early (peak at {py['peak_offset_s']} s of {py['dur_s']} s)")
        if py["spikiness"] > 3:
            why.append(f"clicks inside (spikiness {py['spikiness']} > 3)")
        if py["build_db"] < 6:
            why.append(f"doesn't grow (build {py['build_db']} dB < 6)")
    elif kind in ("transient", "tonal", "glitch", "speech") and not t.get("designed_end"):
        if py["end_db"] > -20:
            why.append(f"cut before it decays (end {py['end_db']} dB > -20)")
    if kind in ONE_SHOT_KINDS:
        min_s = t.get("min_s", lo * 0.5)
        if py["dur_s"] < min_s:
            why.append(f"too short ({py['dur_s']} s < {min_s})")
        if t.get("layered") and py["build_db"] > -6:
            why.append(f"doesn't decay (build {py['build_db']} dB > -6)")
    if kind == "loop":
        if meas["seam_step_db"] > LOOP_SEAM_STEP_MAX_DB:
            why.append(f"loop seam step {meas['seam_step_db']:+.2f} dB over its neighbouring samples > +{LOOP_SEAM_STEP_MAX_DB}")
        elif meas["seam_step_db"] > LIBRARY_SEAM_STEP_MAX_DB:
            why.append(f"loop seam step {meas['seam_step_db']:+.2f} dB over its neighbouring samples > the library's "
                       f"+{LIBRARY_SEAM_STEP_MAX_DB:g} (P5 §5.7; the editor notes +{LOOP_SEAM_STEP_MAX_DB:g})")
        if abs(meas["seam_level_db"]) > LOOP_SEAM_LEVEL_MAX_DB:
            why.append(f"loop seam level {meas['seam_level_db']:.2f} dB, more than {LOOP_SEAM_LEVEL_MAX_DB} dB")
        if py.get("lra_lu") is None or py["lra_lu"] > LOOP_LRA_MAX_LU:
            why.append(f"short-term loudness range {py.get('lra_lu')} LU > {LOOP_LRA_MAX_LU}")
    return why


def defect_reading(decoded, kind, typ, imp):
    """Phase 2.5's defects of a decoded clip as it plays with the import defaults imp (the region a sidecar
    with them plays), and the note ids Phase 2.5's gating gives: the oracle's own defects() and note_ids()."""
    ref = defects_oracle()
    x = np.ascontiguousarray(as2d(decoded).T).astype(np.float32)
    region = {}
    if imp:
        region = {"fade_in_frames": ref.lround(imp.get("fade_in_ms", 0.0) * SR / 1000.0),
                  "fade_out_frames": ref.lround(imp.get("fade_out_ms", 0.0) * SR / 1000.0)}
    d = ref.defects(x, float(SR), region)
    return d, ref.note_ids(d, kind, typ)


def defect_check(decoded, kind, typ):
    """§2.11.4's defect filters (decision 15) on one decoded clip: (import defaults, audit record, fatal reasons).
    A loop whose first frame clicks gets {"fade_in_ms": 2} and is judged with it; any other edge click, clicks
    inside, a silence gap, and crackle on a one-shot or speech clip reject it; crackle on a loop is recorded only."""
    d0, notes0 = defect_reading(decoded, kind, typ, None)
    imp, d, notes = None, d0, notes0
    if kind == "loop" and d0 is not None and not d0.get("empty") and d0["edges"]["start_click"]:
        imp = {"fade_in_ms": LOOP_FADE_IN_MS}
        d, notes = defect_reading(decoded, kind, typ, imp)
    verdicts = {n: n in notes for n in DEFECT_NOTES}
    record = {"oracle_sha256": defects_oracle().SOURCE_SHA256, "import": imp, "notes": notes, "verdicts": verdicts}
    if imp:
        record["notes_without_import"] = notes0
    if d is not None and not d.get("empty"):
        record.update(edges=d["edges"], clicks=d["clicks"], gaps=d["gaps"])
        if d.get("tail"):
            record["tail"] = d["tail"]
    reasons = []
    if verdicts["edge_click"]:
        e = d["edges"]
        sides = [f"{side} {e[side + '_db']} dB" for side in ("start", "end") if e[side + "_click"]]
        reasons.append(f"edge click (Phase 2.5's edge rule: {', '.join(sides)} re the peak, not a zero crossing)")
    if verdicts["clicks"]:
        c = d["clicks"]
        reasons.append(f"clicks inside (Phase 2.5: {c['count']} click(s) at {', '.join(f'{t:.3f}' for t in c['times_s'])} s)")
    if verdicts["crackle"] and kind != "loop":
        reasons.append(f"crackle (Phase 2.5: an irregular field of {d['clicks']['count']} clicks)")
    if verdicts["silence_gap"]:
        g = d["gaps"]
        reasons.append(f"silence gap (Phase 2.5: {g['count']} gap(s), the longest {g['longest_ms']} ms)")
    # §3.1 D-8: every non-loop clip's first and last decoded samples pass the edge rule, ungated. Phase 2.5's gating
    # gives no edge_click to the glitch kind, so the rule is applied here as well (P5 §5.3.6: sci-fi computerNoise_001
    # ended on codec noise at -39.5 dB re its peak, its cut at glitch's window, with no room for the post-roll). The
    # agreement check keeps comparing the gated notes, as the editor gives them.
    if kind != "loop" and not verdicts["edge_click"] and d is not None and not d.get("empty"):
        e = d["edges"]
        if e["start_click"] or e["end_click"]:
            record["edge_ungated"] = True
            sides = [f"{side} {e[side + '_db']} dB" for side in ("start", "end") if e[side + "_click"]]
            reasons.append(f"edge click (§3.1 D-8's edge rule, which Phase 2.5's gating leaves out for the {kind} kind: "
                           f"{', '.join(sides)} re the peak, not a zero crossing)")
    return imp, record, reasons


# ---------------------------------------------------------------------------------------------
# Encoding and the Ogg fix-up

def _crc_table():
    table = []
    for i in range(256):
        r = i << 24
        for _ in range(8):
            r = ((r << 1) ^ 0x04C11DB7) if r & 0x80000000 else (r << 1)
        table.append(r & 0xFFFFFFFF)
    return table


_CRC = _crc_table()


def ogg_crc(data):
    """CRC-32 of an Ogg page: polynomial 0x04C11DB7, initial 0, not reflected, no final xor."""
    crc = 0
    table = _CRC
    for b in data:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ table[((crc >> 24) ^ b) & 0xFF]
    return crc


def ogg_pages(data):
    """(start, header length, end) of every page; refuses anything that is not a clean Ogg stream."""
    pages, pos = [], 0
    while pos < len(data):
        if data[pos:pos + 4] != b"OggS" or pos + 27 > len(data) or data[pos + 4] != 0:
            raise BuildError(f"not an Ogg page at offset {pos}")
        nseg = data[pos + 26]
        hlen = 27 + nseg
        if pos + hlen > len(data):
            raise BuildError("truncated Ogg page header")
        end = pos + hlen + sum(data[pos + 27:pos + hlen])
        if end > len(data):
            raise BuildError("truncated Ogg page")
        pages.append((pos, hlen, end))
        pos = end
    if not pages:
        raise BuildError("empty Ogg stream")
    return pages


def page_crc_ok(data, page):
    start, _, end = page
    buf = bytearray(data[start:end])
    stored = bytes(buf[22:26])
    buf[22:26] = b"\0\0\0\0"
    return ogg_crc(buf).to_bytes(4, "little") == stored


def ogg_serial(clip_id):
    """The clip's stream serial: the first 4 bytes of SHA-256(id), read little-endian (stored as those bytes)."""
    return hashlib.sha256(clip_id.encode("utf-8")).digest()[:4]


def fix_ogg(data, clip_id):
    """One logical stream with the id-derived serial on every page and recomputed page CRCs."""
    pages = ogg_pages(data)
    if len({data[s + 14:s + 18] for s, _, _ in pages}) != 1:
        raise BuildError("the encoder wrote more than one logical stream")
    flags = [data[s + 5] for s, _, _ in pages]
    if not flags[0] & 0x02 or any(f & 0x02 for f in flags[1:]) or not flags[-1] & 0x04:
        raise BuildError("the encoder's stream has misplaced begin/end-of-stream flags")
    if not all(page_crc_ok(data, p) for p in pages):
        raise BuildError("the encoder wrote a page with a bad CRC")
    serial = ogg_serial(clip_id)
    out = bytearray(data)
    for start, _, end in pages:
        out[start + 14:start + 18] = serial
        out[start + 22:start + 26] = b"\0\0\0\0"
        out[start + 22:start + 26] = ogg_crc(out[start:end]).to_bytes(4, "little")
    return bytes(out)


def ogg_packets(data, count):
    packets, cur = [], b""
    for start, hlen, _ in ogg_pages(data):
        body = start + hlen
        for seg in data[start + 27:start + hlen]:
            cur += data[body:body + seg]
            body += seg
            if seg < 255:
                packets.append(cur)
                cur = b""
                if len(packets) >= count:
                    return packets
    return packets


def vorbis_vendor(data):
    pk = ogg_packets(data, 2)
    if len(pk) < 2 or not pk[0].startswith(b"\x01vorbis") or not pk[1].startswith(b"\x03vorbis"):
        raise BuildError("not a Vorbis stream")
    n = int.from_bytes(pk[1][7:11], "little")
    return pk[1][11:11 + n].decode("utf-8", errors="replace")


def encode_ogg(y, quality):
    buf = io.BytesIO()
    sf.write(buf, y, SR, format="OGG", subtype="VORBIS", compression_level=quality)
    return buf.getvalue()


def encoded_clip(y, quality, clip_id):
    """Encode, fix the serial and CRCs, and check the fixed file decodes to the same samples."""
    raw = encode_ogg(y, quality)
    fixed = fix_ogg(raw, clip_id)
    a, _ = decode(raw)
    b, rate = decode(fixed)
    if rate != SR or a.shape != b.shape or not np.array_equal(a, b):
        raise BuildError("the Ogg fix-up changed the decoded samples")
    return fixed, b


def check_encoder(sel):
    want = sel.encoder
    got = {"soundfile": sf.__version__, "libsndfile": sf.__libsndfile_version__,
           "vendor": vorbis_vendor(encode_ogg(np.zeros(4800), QUALITY_DEFAULT))}
    if got != {k: want.get(k) for k in got}:
        raise BuildError(f"encoder differs from the pinned one; the output would not reproduce (pinned {want}, found {got})")
    return got


# ---------------------------------------------------------------------------------------------
# prepare

class SourceAudio:
    """One source decoded once: 48 kHz float64, its mono downmix and the source-level problems."""

    def __init__(self, data, lossy):
        y, rate = decode(data)
        self.rate = rate
        self.channels = y.shape[1]
        self.problem = None
        if not len(y):
            self.problem = "source has 0 frames"
        elif self.channels > 2:
            self.problem = f"source has {self.channels} channels"
        elif rate < MIN_RATE:
            self.problem = f"source rate {rate} Hz < {MIN_RATE}"
        else:
            clipped = source_clipping(y, lossy)
            if clipped:
                self.problem = (f"source is clipped ({clipped} samples at full scale, peak {dbfs(float(np.abs(y).max())):+.2f} dBFS), "
                                f"which no gain can repair")
        if self.problem:
            return
        if rate != SR:
            g = math.gcd(SR, rate)
            y = signal.resample_poly(y, SR // g, rate // g, axis=0)
        self.x = y
        self.mono = y.mean(axis=1)
        self.downmix_db = None
        if self.channels == 2:
            ch_rms = np.sqrt(np.mean(y * y, axis=0))
            mean_rms = float(ch_rms.mean())
            m_rms = float(np.sqrt(np.mean(self.mono ** 2)))
            self.downmix_db = dbfs(m_rms / mean_rms) if mean_rms > 0 else 0.0
        self._cuts = {}
        self._filtered = {}

    def events(self, start, length, t, kind, k):
        key = (start, length, t["win"], t.get("layered", False), kind, k)
        if key not in self._cuts:
            self._cuts[key] = cut_events(self.mono[start:start + length], t, kind, k)
        return self._cuts[key]

    def filtered_stereo(self, typ):
        if typ not in self._filtered:
            x = self.x if self.channels == 2 else np.repeat(self.x, 2, axis=1)
            self._filtered[typ] = band_filter(x, TYPES[typ])
        return self._filtered[typ]


def segment_frames(clip, total):
    seg = clip.get("segment_s")
    if seg is None:
        return 0, total
    return int(round(seg[0] * SR)), int(round(seg[1] * SR))


def decoded_edges(dec):
    """Phase 2.5's edge readings (the oracle's edges()) of a decoded clip, or None when it is silent."""
    ref = defects_oracle()
    x = np.ascontiguousarray(as2d(dec).T).astype(np.float32)
    peak = float(np.abs(x.astype(np.float64)).max()) if x.size else 0.0
    return ref.edges(x, peak, float(SR)) if peak > 0 else None


def gain_loop(y_clean, target, measure_fn, fade_out, trim, quality, clip_id, max_frames=None, over_max=None):
    """§2.11.3 step 8 on the delivered file: gain to the target as measured on the decoded encoding
    (at most GAIN_CORRECTIONS corrections of 0.1 LU or more, then, with over_max, up to OVER_TARGET_CORRECTIONS
    more while the clip is more than over_max LU over). When the decoded true peak (peak_guard) goes over -1 dBTP, the
    gain is the highest of a grid below that gain whose decoded peak is under it (TP_STEP_DB, P5 §5.7). Trimming follows
    the gain, so -60 dBFS is judged on the delivered level (held at one trim during the peak's search)."""
    level = measure_fn(y_clean)
    if level is None:
        return None, "silent: no loudness to normalise"
    gain0 = target - level

    def encode(gain, bounds=None):
        y = y_clean * 10 ** (gain / 20)
        if trim:
            bounds = bounds or trim_bounds(y, fade_out)
            if bounds is None:
                return None
            y = np.concatenate([np.zeros((PREROLL_FRAMES,) + y.shape[1:]), y[bounds[0]:bounds[1]]])
        else:
            bounds = (0, len(y))
        data, dec = encoded_clip(y, quality, clip_id)
        return {"gain": gain, "y": y, "bounds": bounds, "ogg": data, "decoded": dec, "tp": peak_guard(dec)}

    gain, corrections, encodes, over_corrections = gain0, 0, 0, 0
    cap = None  # the gain the loudness corrections had reached when the true peak first stopped them
    while True:
        cur = encode(gain)
        if cur is None:
            return None, "silent after gain (nothing at or above -60 dBFS)"
        encodes += 1
        if cur["tp"] <= CEILING_DBTP:
            delivered = measure_fn(cur["decoded"])
            if delivered is not None and abs(delivered - target) >= GAIN_TOLERANCE_LU and corrections < GAIN_CORRECTIONS:
                gain += target - delivered
                corrections += 1
                continue
            if (over_max is not None and delivered is not None and delivered > target + over_max
                    and over_corrections < OVER_TARGET_CORRECTIONS):
                gain += target - delivered
                over_corrections += 1
                continue
            best = cur
            break
        # limited: the peak, not the loudness, sets the gain (see TP_STEP_DB)
        cap, frozen = gain, cur["bounds"] if trim else None
        top = min(cap - TP_STEP_DB, cap + (CEILING_DBTP - cur["tp"]) + TP_SCAN_ABOVE_DB)
        best = None
        for i in range(TP_SCAN_STEPS):
            cur = encode(top - i * TP_STEP_DB, frozen)
            encodes += 1
            if cur["tp"] <= CEILING_DBTP:
                best = cur
                break
        if best is None:
            return None, f"true peak still over {CEILING_DBTP} dBTP {TP_SCAN_STEPS * TP_STEP_DB:g} dB down the gain search"
        # the grid's first point was already under: step up until a point goes over (or the corrections' gain is reached)
        while best["gain"] == top and top + TP_STEP_DB < cap - 1e-9 and encodes < TP_SCAN_STEPS * 2:
            top += TP_STEP_DB
            cur = encode(top, frozen)
            encodes += 1
            if cur["tp"] > CEILING_DBTP:
                break
            best = cur
        break
    limited = cap is not None
    y, data, dec, postroll = best["y"], best["ogg"], best["decoded"], 0
    if trim:
        edges = decoded_edges(dec)
        if edges and edges["end_click"] and (max_frames is None or len(y) + POSTROLL_FRAMES <= max_frames):
            padded = np.concatenate([y, np.zeros((POSTROLL_FRAMES,) + y.shape[1:])])
            data2, dec2 = encoded_clip(padded, quality, clip_id)
            encodes += 1
            after = decoded_edges(dec2)
            if peak_guard(dec2) <= CEILING_DBTP and after and not after["end_click"] and not after["start_click"]:
                y, data, dec, postroll = padded, data2, dec2, POSTROLL_FRAMES
    return {"gain_db": best["gain"], "gain0_db": gain0, "limited": limited, "cap_db": cap, "y": y, "bounds": best["bounds"], "ogg": data,
            "decoded": dec, "encodes": encodes, "postroll": postroll, "over_corrections": over_corrections}, None


def tail_cut(res):
    """Phase 2.5's truncated_tail on a prepared clip's decoded file (the oracle's gated note), or None."""
    d = res.get("defects") or {}
    if "truncated_tail" not in (d.get("notes") or []):
        return None
    return (d.get("tail") or {}).get("cut_db")


def prepare_clip(clip, src, audio, sel, k):
    typ, kind = clip["type"], clip["kind"]
    t = TYPES[typ]
    target = sel.targets[typ]
    quality = QUALITY_AMBIENCE if typ == "ambience" else QUALITY_DEFAULT
    res = {"id": clip["id"], "source": source_key(src), "type": typ, "kind": kind, "verdict": "rejected", "reasons": [],
           "processing": None, "python": None, "measured": None, "defects": None, "import": None, "ogg": None}
    if audio.problem:
        res["reasons"].append(audio.problem)
        return res, None
    proc = {"resampled_from": audio.rate if audio.rate != SR else None, "channels_from": audio.channels}
    if kind != "loop" and audio.downmix_db is not None and audio.downmix_db < DOWNMIX_CANCEL_DB:
        res["reasons"].append(f"mono downmix cancels ({audio.downmix_db:.1f} dB < {DOWNMIX_CANCEL_DB})")
        return res, None
    start, length = segment_frames(clip, len(audio.mono))
    if clip.get("segment_s") is not None:
        proc["segment"] = [start, length]

    def render(y, fo, trim, measure_fn, proc, loop=None):
        """Gain, encode, measure and judge one prepared signal: (res, ogg bytes or None)."""
        r = dict(res, reasons=[])
        src_mm = momentary_max(y)
        out, err = gain_loop(y, target, measure_fn, fo, trim, quality, clip["id"], max_frames=int(t["win"][1] * SR),
                             over_max=None if kind in ("loop", "speech") else OVER_TARGET_MAX_LU)
        if err:
            r["reasons"].append(err)
            r["processing"] = proc
            return r, None
        if loop:
            filtered, xf, candidates = loop
            out, shift, tries = seam_retry(out, filtered, start, length, xf, candidates, quality, clip["id"])
            proc["segment"] = [start, length + shift]
            proc["seam_shift"] = shift
            proc["seam_tries"] = tries
        if trim:
            proc["trim"] = [out["bounds"][0], len(y) - out["bounds"][1]]
            proc["preroll"] = PREROLL_FRAMES
            proc["postroll"] = out["postroll"]
        if out["over_corrections"]:
            proc["over_target_corrections"] = out["over_corrections"]
        proc["gain_db"] = round(out["gain_db"], 2)
        proc["limited_by"] = "true_peak" if out["limited"] else None
        proc["quality"] = quality
        meas, raw = measure_decoded(out["decoded"], SR, kind == "loop")
        py = python_measures(out["y"], t, kind == "loop")
        py["true_peak_16x_dbtp"] = round(true_peak_fine(out["decoded"]), 2)
        reasons = python_reasons(t, kind, py, dict(meas, clipped=raw["clipped"]), src_mm)
        if kind == "loop":
            py["wrap_rms_db"] = round(wrap_rms_db(out["decoded"], SR), 2)
            if abs(py["wrap_rms_db"]) > LOOP_WRAP_RMS_MAX_DB:
                reasons.append(f"loop wrap level {py['wrap_rms_db']:+.2f} dB against the clip's RMS (the 20 ms around the wrap; "
                               f"§3.1 D-7 allows {LOOP_WRAP_RMS_MAX_DB:g} dB)")
        imp, defects, defect_reasons = defect_check(out["decoded"], kind, typ)
        reasons += defect_reasons
        final = raw["integrated_lufs"] if kind in ("loop", "speech") else raw["momentary_max_lufs"]
        if final is None or final < target - MAX_UNDER_TARGET_LU:
            reasons.append(f"too peaky for its type ({'?' if final is None else round(final - target, 2)} LU from the {target} LUFS "
                           f"target at the true-peak limit)")
        if kind not in ("loop", "speech") and final is not None and final > target + OVER_TARGET_MAX_LU:
            reasons.append(f"over its type's target ({final - target:+.2f} LU from the {target} LUFS target after "
                           f"{out['over_corrections']} extra gain correction(s); §3.1 D-4 allows +{OVER_TARGET_MAX_LU:g})")
        py["source_momentary_max_lufs"] = rnd(src_mm, 2)
        py["final_vs_target_lu"] = rnd(None if final is None else final - target, 2)
        r.update(processing=proc, python=py, measured=meas, defects=defects, reasons=reasons,
                 verdict="rejected" if reasons else "accepted", ogg={"sha256": sha256(out["ogg"]), "bytes": len(out["ogg"])})
        if imp:
            r["import"] = imp
        return r, out["ogg"]

    if kind == "loop":
        xf = int(round((clip.get("crossfade_ms") or LOOP_CROSSFADE_MS.get(typ, 500.0)) / 1000 * SR))
        if xf >= length:
            res["reasons"].append("crossfade longer than the segment")
            return res, None
        if start + length + xf > len(audio.mono):
            res["reasons"].append(f"source too short for the segment plus a {xf / SR * 1000:.0f} ms crossfade")
            return res, None
        filtered, finfo = audio.filtered_stereo(typ)
        candidates = seam_candidates(filtered, start, length, xf)
        y = loop_crossfade(filtered, start, length + candidates[0], xf)
        proc["loop_crossfade_ms"] = round(xf / SR * 1000, 1)
        proc.update(finfo)
        return render(y, 0, False, integrated, proc, loop=(filtered, xf, candidates))

    if start + length > len(audio.mono):
        res["reasons"].append("segment_s ends after the source")
        return res, None
    measure_fn = integrated if kind == "speech" else momentary_max
    limit = start + length
    if kind == "speech":
        a, e, n = start, start + length, None
    else:
        n = clip.get("event", 1)
        # the cut leaves room for the pre-roll inside the type's window
        ev = audio.events(start, length, dict(t, win=(t["win"][0], t["win"][1] - PREROLL_FRAMES / SR)), kind, k)
        if n > len(ev):
            res["reasons"].append(f"the source has {len(ev)} event(s) of this kind, event {n} was asked for")
            return res, None
        a, e = start + ev[n - 1][0], start + ev[n - 1][1]
        # a later end stays inside the window (with the pre-roll's room) and before the source's next event
        limit = min(limit, a + int(t["win"][1] * SR) - PREROLL_FRAMES, *(start + s0 for s0, _ in ev[n:n + 1]))
    if kind == "speech" and e - a < INTEGRATED_MIN_S * SR:
        res["reasons"].append(f"speech shorter than {INTEGRATED_MIN_S} s: integrated loudness is undefined")
        return res, None

    def cut(end, release=None, repair=None):
        p = dict(proc)
        if n is not None:
            p["event"] = [n, a, end]
        y, finfo = band_filter(audio.mono[a:end].copy(), t)
        p.update(finfo)
        fo, fade_info = fades(y, t, release)
        p.update(fade_info)
        if repair:
            p["tail_repair"] = repair
        return render(y, fo, True, measure_fn, p)

    first = cut(e)
    cut_db = tail_cut(first[0])
    if cut_db is None:
        return first
    # P5 §5.7: the tail is cut while it still decays: later ends, then releases at each end (see TAIL_EXTEND_DB); the
    # first try that clears the note and passes every filter is kept
    ends = []
    if kind in TAIL_EXTEND_KINDS:
        for level in (SWELL_EXTEND_DB if kind == "swell" else (TAIL_EXTEND_DB,)):
            later = extended_end(audio.mono, a, e, t, kind, limit, level)
            if later > e and later not in ends:
                ends.append(later)
    tries = [(end, None, "later_end") for end in ends]
    for end in ends + [e]:
        prefix = "later_end, " if end != e else ""
        tries += [(end, ("linear", RELEASE_LINEAR_MS), prefix + "release"), (end, ("cosine", RELEASE_COSINE_MS), prefix + "cosine_release")]
    failed = []
    for end, release, repair in tries:
        r, ogg = cut(end, release, repair)
        still = tail_cut(r)
        if still is None and r["processing"] is not None and r.get("defects") is not None and not r["reasons"]:
            return r, ogg
        failed.append(f"{repair}: {'still cut' if still is not None else r['reasons'][0] if r['reasons'] else 'no clip'}")
    r, ogg = first
    r["reasons"].append(f"tail cut while still decaying (Phase 2.5's truncated_tail, {round(cut_db, 1)} dB re its peak); no repair "
                        f"passes ({'; '.join(failed[:4])})")
    r["verdict"] = "rejected"
    return r, ogg


def kenney_attribution(pack, member):
    for v in pack.get("voices") or []:
        if member.startswith(v["prefix"]):
            return f"{KENNEY_AUTHOR}; voice: {v['name']}"
    return None


# Manifest key order (§2.2.2): the essentials' clip keys, the library's optional ones in place.
CLIP_KEYS = ("id", "path", "sha256", "bytes", "type", "kind", "tags", "description", "licence", "source", "pack", "source_file",
             "source_id", "author", "url", "licence_url", "attribution", "processing", "measured", "import", "meta", "semantic")
PROCESSING_KEYS = ("event", "segment", "seam_shift", "seam_tries", "loop_crossfade_ms", "resampled_from", "channels_from", "highpass_hz", "lowpass_hz",
                   "fade_in_ms", "fade_out_ms", "tail_fade", "tail_repair", "trim", "preroll", "postroll", "over_target_corrections", "gain_db", "limited_by", "quality", "preview")
MEASURED_KEYS = ("channels", "sample_rate", "frames", "duration_s", "sample_peak_dbfs", "true_peak_dbtp", "momentary_max_lufs",
                 "integrated_lufs", "head_ms", "tail_ms", "seam_step_db", "seam_level_db")


def ordered(d, keys):
    extra = set(d) - set(keys)
    if extra:
        raise BuildError(f"unexpected manifest keys {sorted(extra)}")
    return {k: d[k] for k in keys if k in d}


def clip_importer_fades(clip):
    """the fades of a v4 clip's meta, as the v3 `import` object; None without any"""
    importer = ((clip.get("meta") or {}).get("import") or {}).get("audioImporter") or {}
    fades = {k: importer[k] for k in IMPORT_KEYS if k in importer}
    return fades or None


def load_selection_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def canonical_entry(e):
    e = ordered(e, CLIP_KEYS)
    e["processing"] = ordered(e["processing"], PROCESSING_KEYS)
    e["measured"] = ordered(e["measured"], MEASURED_KEYS)
    # the v4 manifest: what an import writes, the sections of its S04 .meta (soundlib.clip_meta)
    importer = e.pop("import", None)
    meta = soundlib.clip_meta(e, ordered(importer, IMPORT_KEYS) if importer else None, "library")
    semantic = e.pop("semantic", None)
    e["meta"] = meta
    if semantic is not None:
        e["semantic"] = ordered(semantic, ("label", "heard"))
    return e


def manifest_entry(clip, src, sel, res, meta):
    e = {"id": clip["id"], "path": clip["id"] + ".ogg", "sha256": res["ogg"]["sha256"], "bytes": res["ogg"]["bytes"],
         "type": clip["type"], "kind": clip["kind"], "tags": clip["tags"], "description": clip["description"],
         "licence": LICENCE, "source": src["source"]}
    if src["source"] == "kenney":
        pack = sel.packs[src["pack"]]
        e.update(pack=src["pack"], source_file=src["member"], author=KENNEY_AUTHOR, url=pack["landing"])
        attribution = kenney_attribution(pack, src["member"])
        if attribution:
            e["attribution"] = attribution
    else:
        e.update(pack=None, source_file=None, source_id=src["id"], author=src["username"], url=src["landing"],
                 licence_url=meta["licence_links"][0], attribution=f"{src['username']} (freesound.org)")
    e["processing"] = res["processing"]
    e["measured"] = res["measured"]
    if res.get("import"):
        e["import"] = res["import"]
    return e


def load_kenney_pack(sel, pid, cache):
    """The pinned zip opened, its licence (and credits) read; each voice name checked against the credits."""
    pack = sel.packs[pid]
    zf = soundlib.open_pinned_zip(zip_path(cache, pid), pack, sel.name)
    lic = read_licence(zf, pid)
    credits = None
    if pack.get("credits_member"):
        credits = read_member(zf, pack["credits_member"], soundlib.MAX_LICENCE_BYTES)
        text = credits.decode("utf-8", errors="replace")
        for v in pack.get("voices") or []:
            if v["name"] not in text:
                raise BuildError(f"{pid}: voice {v['name']!r} is not named in {pack['credits_member']}")
    evidence = {"zip_sha256": pack["sha256"], "zip_bytes": pack["bytes"], "licence_sha256": sha256(lic),
                "licence_markers": [m for m in LICENCE_MARKERS if m in lic.decode("utf-8", errors="replace")]}
    if credits is not None:
        evidence["credits_sha256"] = sha256(credits)
    return zf, lic, credits, evidence


def cmd_prepare(args, sel=None):
    _dsp()
    sel = sel or Selection(args.selection)
    encoder = check_encoder(sel)
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    wanted = {cid for cid in sel.clips if not args.only or fnmatch.fnmatchcase(cid, args.only)}
    if args.only and not wanted:
        raise BuildError(f"no clip id matches {args.only!r}")
    ess = essentials_members()
    packs = {}
    counts = {"accepted": 0, "rejected": 0}
    try:
        for src in sel.sources:
            clips = [c for c in src["clips"] if c["id"] in wanted]
            if not clips:
                continue
            key = source_key(src)
            meta = None
            if src["source"] == "kenney":
                pid = src["pack"]
                if pid not in packs:
                    packs[pid] = load_kenney_pack(sel, pid, args.cache)
                    zf, lic, credits, _ = packs[pid]
                    write_if_changed(work / "licenses" / f"{pid}.txt", lic)
                    if credits is not None:
                        write_if_changed(work / "licenses" / f"{pid}.credits.txt", credits)
                zf, _, _, pack_evidence = packs[pid]
                data = read_member(zf, src["member"], MAX_MEMBER_BYTES)
                if not data.startswith(b"OggS"):
                    raise BuildError(f"{key}: the member is not an Ogg file")
                evidence = dict(pack_evidence, pack=pid, member=src["member"], member_sha256=sha256(data), member_bytes=len(data))
                attribution = kenney_attribution(sel.packs[pid], src["member"])
                if attribution:
                    evidence["attribution"] = attribution
                provenance = ess.get((pid, src["member"]))
            else:
                mpath = fs_meta_path(args.cache, src["id"])
                if not mpath.is_file():
                    raise BuildError(f"{key}: no cached metadata; run fetch first")
                meta = load_json(mpath, MAX_JSON_BYTES, "cached Freesound metadata")["meta"]
                problems = freesound_problems(meta, src["id"], src["username"], src["landing"])
                if problems:
                    raise BuildError(f"{key}: " + "; ".join(problems))
                ppath = fs_preview_path(args.cache, src["id"], preview_ext(src["preview"]))
                if not ppath.is_file():
                    raise BuildError(f"{key}: preview not in the cache; run fetch first")
                data = ppath.read_bytes()
                if len(data) != src["bytes"] or sha256(data) != src["sha256"]:
                    raise BuildError(f"{key}: cached preview's size or SHA-256 differs from {sel.name}; delete it and fetch again")
                if not preview_magic_ok(data, preview_ext(src["preview"])):
                    raise BuildError(f"{key}: the cached preview is not an {preview_ext(src['preview']).upper()} file")
                evidence = {"id": src["id"], "username": src["username"], "landing": src["landing"], "preview": src["preview"],
                            "preview_format": "hq." + preview_ext(src["preview"]), "preview_sha256": src["sha256"],
                            "preview_bytes": src["bytes"], "licence_url": meta["licence_links"][0], "licence": LICENCE,
                            "explicit": meta["explicit"], "remix_group": meta["remix_group"], "page_title": meta.get("title"),
                            "page_sha256": meta.get("page_sha256")}
                provenance = None
            evidence["encoder"] = encoder
            try:
                audio = SourceAudio(data, lossy=True)
            except Exception as e:  # an undecodable source rejects its clips
                audio = None
                decode_error = f"does not decode ({e})"
            k = src.get("events") or max(c.get("event", 1) for c in src["clips"])
            for clip in clips:
                if provenance:
                    res, ogg = {"id": clip["id"], "source": key, "type": clip["type"], "kind": clip["kind"], "verdict": "rejected",
                                "reasons": [f"cut from an essentials member (essentials:{provenance}); the library never repeats one"],
                                "processing": None, "python": None, "measured": None, "defects": None, "import": None,
                                "ogg": None}, None
                elif audio is None:
                    res, ogg = {"id": clip["id"], "source": key, "type": clip["type"], "kind": clip["kind"], "verdict": "rejected",
                                "reasons": [decode_error], "processing": None, "python": None, "measured": None, "defects": None, "import": None,
                                "ogg": None}, None
                else:
                    res, ogg = prepare_clip(clip, src, audio, sel, k)
                    if src["source"] == "freesound" and res["processing"] is not None:
                        res["processing"]["preview"] = "hq." + preview_ext(src["preview"])
                res["evidence"] = evidence
                res["entry"] = manifest_entry(clip, src, sel, res, meta) if ogg is not None else None
                ogg_path = clip_file(work, clip["id"], ".ogg")
                if ogg is not None:
                    write_if_changed(ogg_path, ogg)
                elif ogg_path.is_file():
                    ogg_path.unlink()
                write_if_changed(clip_file(work, clip["id"], ".json"), json_text(res).encode("utf-8"))
                counts[res["verdict"]] += 1
                tail = "" if res["verdict"] == "accepted" else ": " + "; ".join(res["reasons"])
                print(f"prepare: {clip['id']} {res['verdict']}{tail}")
    finally:
        for zf, *_ in packs.values():
            zf.close()
    print(f"prepare: {counts['accepted']} accepted, {counts['rejected']} rejected by the Python filters, into {work}")


# ---------------------------------------------------------------------------------------------
# judge (the editor's analyzer)

# The analyzer CLI is spelled `audio analyze` in §2.6 and `audio-analyze` in the cross-phase audit's
# P5-4 resolution (Phase 4's `audio-check` style); snd/p5-core settles it, so both are probed.
EDITOR_COMMANDS = (("audio-analyze",), ("audio", "analyze"))


def editor_types(editor):
    """`<analyze command> --list-types --json` as a dict with "command" added, or (None, why) when the editor cannot run it."""
    if not editor:
        return None, "no --editor given"
    path = Path(editor)
    if not path.is_file() or not os.access(path, os.X_OK):
        return None, f"{editor} is not an executable file"
    failures = []
    for command in EDITOR_COMMANDS:
        try:
            r = subprocess.run([str(path), *command, "--list-types", "--json"], capture_output=True, text=True,
                               timeout=120, stdin=subprocess.DEVNULL)
        except (OSError, subprocess.TimeoutExpired) as e:
            failures.append(f"'{' '.join(command)}' did not run ({e})")
            continue
        if r.returncode != 0:
            first = (r.stderr or r.stdout).strip().splitlines()[:1]
            failures.append(f"'{' '.join(command)}' exit {r.returncode}: {first[0] if first else 'no output'}")
            continue
        try:
            info = json.loads(r.stdout)
            if not is_int(info.get("analyzer_version")) or not isinstance(info.get("types"), list):
                raise ValueError("missing analyzer_version or types")
        except (ValueError, AttributeError) as e:
            failures.append(f"'{' '.join(command)} --list-types --json' printed no usable JSON ({e})")
            continue
        info["command"] = list(command)
        return info, None
    return None, (f"{path.name} has no working analyze command ({'; '.join(failures)}); it arrives with snd/p5-core M1")


def check_editor_types(info, sel):
    problems = []
    got = {t.get("type"): t for t in info["types"] if isinstance(t, dict)}
    if set(got) != set(TYPES):
        problems.append(f"the editor's types differ: missing {sorted(set(TYPES) - set(got))}, extra {sorted(set(got) - set(TYPES))}")
    for name, t in TYPES.items():
        e = got.get(name)
        if not e:
            continue
        if e.get("kind") != t["kind"]:
            problems.append(f"{name}: kind {t['kind']} here, {e.get('kind')} in the editor")
        if not is_num(e.get("target_lufs")) or abs(e["target_lufs"] - sel.targets[name]) > 1e-9:
            problems.append(f"{name}: target {sel.targets[name]} in the selection, {e.get('target_lufs')} in the editor")
        want = "integrated" if t["kind"] in ("loop", "speech") else "momentary_max"
        if e.get("measure") != want:
            problems.append(f"{name}: measure {want} here, {e.get('measure')} in the editor")
    if problems:
        raise BuildError("the selection disagrees with the editor's SoundTypes.h (fix the selection's targets):\n  " + "\n  ".join(problems))


def run_editor_json(editor, args, env=None):
    try:
        r = subprocess.run([str(editor), *args], capture_output=True, text=True, timeout=EDITOR_TIMEOUT_S,
                           stdin=subprocess.DEVNULL, env=env)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise BuildError(f"{editor} {' '.join(args[:3])} did not run ({e})")
    try:
        out = json.loads(r.stdout)
    except ValueError:
        raise BuildError(f"{editor} {' '.join(args[:3])} printed no JSON (exit {r.returncode}): {(r.stderr or '').strip()[:300]}")
    # 0: every input analysed; an input it could not analyse is an errors[] entry (exit 1, or 2 under the
    # audio commands' one convention, P5 §5.0.14a D4); anything else is a failure of the call
    if not isinstance(out, dict) or r.returncode not in (0, 1, 2) or (r.returncode and not out.get("errors")):
        raise BuildError(f"{editor} {' '.join(args[:3])} failed (exit {r.returncode}): {(r.stderr or '').strip()[:300]}")
    return out


def editor_reasons(clip_res, item, target=None):
    """§2.11.4 editor rules and the agreement check, on one editor item; with the type's target, §3.1 D-4's band
    for one-shots on the editor's own momentary max (the two meters may differ by up to 0.2 LU)."""
    meas = clip_res["measured"]
    info = {"kind": clip_res["kind"], "type": clip_res["type"], "channels": meas["channels"]}
    notes = [n.get("id") for n in item.get("notes") or [] if isinstance(n, dict)]
    why = [f"editor note {n}" for n, applies in EDITOR_FATAL if n in notes and applies(info)]
    fmt, peak, loud, sil = (item.get(k) or {} for k in ("format", "peak", "loudness", "silence"))
    loudness_key, py_key = (("integrated_lufs", "integrated_lufs") if clip_res["kind"] in ("loop", "speech")
                            else ("momentary_max", "momentary_max_lufs"))
    checks = (("duration", fmt.get("duration_s"), meas["duration_s"], AGREE_DURATION_S, "s"),
              ("sample peak", peak.get("sample_dbfs"), meas["sample_peak_dbfs"], AGREE_SAMPLE_PEAK_DB, "dB"),
              ("true peak", peak.get("true_peak_dbtp"), meas["true_peak_dbtp"], AGREE_TRUE_PEAK_DB, "dB"),
              ("loudness", loud.get(loudness_key), meas[py_key], AGREE_LOUDNESS_LU, "LU"),
              ("head silence", sil.get("head_ms") if clip_res["kind"] != "loop" else meas["head_ms"], meas["head_ms"], AGREE_HEAD_MS, "ms"))
    for name, ed, py, tol, unit in checks:
        if not is_num(ed) or not is_num(py) or abs(ed - py) > tol + 1e-9:
            why.append(f"analyzers disagree: {name} {ed} (editor) vs {py} (Python), tolerance {tol} {unit}")
    # Phase 2.5's four notes: present or absent in both (the editor gates them by kind and type as the oracle does)
    editor_defects = {n: n in notes for n in DEFECT_NOTES}
    py_defects = (clip_res.get("defects") or {}).get("verdicts") or {}
    for n in DEFECT_NOTES:
        if editor_defects[n] != py_defects.get(n):
            why.append(f"analyzers disagree: {n} {'present' if editor_defects[n] else 'absent'} (editor) vs "
                       f"{'present' if py_defects.get(n) else 'absent'} (Python)")
    if target is not None and clip_res["kind"] not in ("loop", "speech"):
        mm = loud.get("momentary_max")
        lo, hi = target - MAX_UNDER_TARGET_LU, target + OVER_TARGET_MAX_LU
        if is_num(mm) and not lo <= mm <= hi:
            why.append(f"editor: momentary max {mm} LUFS outside §3.1 D-4's {lo:g}..{hi:g} (target {target})")
    if clip_res["kind"] != item.get("kind"):
        why.append(f"the editor judged it as kind {item.get('kind')!r}, not {clip_res['kind']!r}")
    key = {"duration_s": fmt.get("duration_s"), "sample_dbfs": peak.get("sample_dbfs"), "true_peak_dbtp": peak.get("true_peak_dbtp"),
           "momentary_max": loud.get("momentary_max"), "integrated_lufs": loud.get("integrated_lufs"), "head_ms": sil.get("head_ms"),
           "kind": item.get("kind"), "kind_source": item.get("kind_source"), "import": item.get("import"), "defects": editor_defects}
    return why, key, notes


def work_results(sel, work):
    results = {}
    for cid in sorted(sel.clips):
        p = clip_file(work, cid, ".json")
        if not p.is_file():
            raise BuildError(f"{cid}: {p} is missing; run prepare first")
        results[cid] = load_json(p, MAX_JSON_BYTES, "work file")
    return results


def cmd_judge(args, sel=None):
    sel = sel or Selection(args.selection)
    work = Path(args.work)
    results = work_results(sel, work)
    info, why = editor_types(args.editor)
    if info is None:
        judge = {"editor": {"available": False, "reason": why}, "clips": {}}
        write_if_changed(work / JUDGE, json_text(judge).encode("utf-8"))
        print(f"judge: editor judge skipped: {why}. Clips are judged by the Python filters only and analysis.json is not "
              f"written. Phase 5 decision 10 needs the editor for the committed library: rerun with an editor that has "
              f"the analyze command (snd/p5-core M1).")
        return
    check_editor_types(info, sel)
    clips = {}
    batches = {}
    for cid, res in results.items():
        if res["verdict"] == "accepted":
            fade_in = (res.get("import") or {}).get("fade_in_ms")
            batches.setdefault((res["type"], res["kind"], fade_in or 0.0), []).append(cid)
    for typ, kind, fade_in in sorted(batches):
        ids = batches[(typ, kind, fade_in)]
        # the region the clip will play: its kind (a manifest kind, §5.2 decision 10) and its import fade-in
        options = ["--type", typ, "--kind", kind] + (["--fade-in-ms", f"{fade_in:g}"] if fade_in else [])
        for i in range(0, len(ids), EDITOR_BATCH):
            batch = ids[i:i + EDITOR_BATCH]
            files = [str(clip_file(work, cid, ".ogg")) for cid in batch]
            out = run_editor_json(args.editor, [*info["command"], "--json", "--detail", "summary", *options, *files])
            items = {it.get("path"): it for it in out.get("items") or [] if isinstance(it, dict)}
            errors = {e.get("path"): e.get("error") for e in out.get("errors") or [] if isinstance(e, dict)}
            for cid, f in zip(batch, files):
                if f in items:
                    reasons, key, notes = editor_reasons(results[cid], items[f], sel.targets[results[cid]["type"]])
                else:
                    reasons, key, notes = [f"editor could not analyse it: {errors.get(f, 'no item returned')}"], None, []
                clips[cid] = {"verdict": "rejected" if reasons else "accepted", "reasons": reasons, "item": key, "notes": notes}
    judge = {"editor": {"available": True, "path": str(Path(args.editor).resolve()), "command": info["command"],
                        "analyzer_version": info["analyzer_version"]}, "clips": clips}
    write_if_changed(work / JUDGE, json_text(judge).encode("utf-8"))
    bad = sum(1 for c in clips.values() if c["verdict"] != "accepted")
    print(f"judge: editor analyzer v{info['analyzer_version']}: {len(clips) - bad} accepted, {bad} rejected")


# ---------------------------------------------------------------------------------------------
# dedupe (near-duplicates and the same audio from two uploaders)

def _ncc_spectra(signals):
    """rfft of each signal zero-padded to one length that holds every pair's full correlation, with its energy."""
    from scipy.fft import next_fast_len
    n = next_fast_len(2 * max(len(x) for x in signals), real=True)
    return n, [(np.fft.rfft(x, n), float(np.dot(x, x))) for x in signals]


def _ncc(fa, fb, n):
    """The largest |normalised cross-correlation| over all lags, and its lag (b delayed by the lag matches a)."""
    (sa, ea), (sb, eb) = fa, fb
    if ea <= 0 or eb <= 0:
        return 0.0, 0
    c = np.fft.irfft(sa * np.conj(sb), n)
    k = int(np.argmax(np.abs(c)))
    return float(abs(c[k]) / math.sqrt(ea * eb)), (k if k < n // 2 else k - n)


def clip_similarity(x, y):
    """(similarity, full-band NCC, lag in frames, per-band NCCs) of two mono 48 kHz signals (see NEAR_DUP_NCC)."""
    n, (fx, fy) = _ncc_spectra([x, y])
    full, lag = _ncc(fx, fy, n)
    bands = {}
    for hz in NEAR_DUP_BANDS_HZ:
        sos = signal.butter(4, hz, "highpass", fs=SR, output="sos")
        n2, (bx, by) = _ncc_spectra([signal.sosfilt(sos, x), signal.sosfilt(sos, y)])
        bands[int(hz)] = _ncc(bx, by, n2)[0]
    return min([full, *bands.values()]), full, lag, bands


def dedupe_rank(res, sel):
    """The order in which clips of a near pair are kept: Kenney first, then the earlier Freesound upload, then the id."""
    src = sel.clips[res["id"]][1]
    return (0, 0, res["id"]) if src["source"] == "kenney" else (1, src["id"], res["id"])


def dedupe_pairs(sel, results, ids, work):
    """The near pairs among ids (see NEAR_DUP_NCC): [{a, b, similarity, full, lag_ms, bands, rule}], a kept before b."""
    mono = {}
    for cid in ids:
        y, _ = decode(clip_file(work, cid, ".ogg").read_bytes())
        mono[cid] = y.mean(axis=1)
    groups = {}
    for cid in ids:
        groups.setdefault(("type", results[cid]["type"]), []).append(cid)
        groups.setdefault(("source", results[cid]["source"]), []).append(cid)
    pairs = set()
    for members in groups.values():
        members = sorted(members)
        if len(members) < 2:
            continue
        low = [signal.resample_poly(mono[c], 1, DUP_PRESCAN_DECIMATE) for c in members]
        n, spectra = _ncc_spectra(low)
        for i in range(len(members)):
            for j in range(i + 1, len(members)):
                if _ncc(spectra[i], spectra[j], n)[0] >= DUP_PRESCAN_NCC:
                    pairs.add((members[i], members[j]))
    found = []
    for a, b in sorted(pairs):
        if dedupe_rank(results[b], sel) < dedupe_rank(results[a], sel):
            a, b = b, a
        sim, full, lag, bands = clip_similarity(mono[a], mono[b])
        authors = [sel.clips[c][1].get("username") or KENNEY_AUTHOR for c in (a, b)]
        rule = ("near_duplicate" if sim >= NEAR_DUP_NCC else
                "provenance" if authors[0] != authors[1] and full >= PROVENANCE_NCC else None)
        if rule:
            found.append({"a": a, "b": b, "rule": rule, "similarity": round(sim, 3), "full": round(full, 3),
                          "lag_ms": round(lag / SR * 1000, 1), "bands": {str(k): round(v, 3) for k, v in bands.items()},
                          "authors": authors})
    return found


def r_accepted(res, judge):
    """Accepted by the Python filters and, when the editor judged, by it."""
    return res["verdict"] == "accepted" and (not judge["editor"].get("available")
                                             or (judge["clips"].get(res["id"]) or {}).get("verdict") == "accepted")


def dedupe_record(sel, work, results, judge):
    """The near-duplicate check over the clips both judges accept (the Python filters alone without the editor):
    {compared {id: sha256}, pairs, rejected {id: {reason, of, rule}}}, from work/dedupe.json when it compared exactly
    these clips, else computed and written there (P5 §5.7)."""
    work = Path(work)
    ids = sorted(cid for cid, r in results.items() if r_accepted(r, judge))
    compared = {cid: results[cid]["ogg"]["sha256"] for cid in ids}
    settings = {"near_dup_ncc": NEAR_DUP_NCC, "provenance_ncc": PROVENANCE_NCC, "bands_hz": list(NEAR_DUP_BANDS_HZ)}
    path = work / DEDUPE
    if path.is_file():
        old = load_json(path, MAX_JSON_BYTES, "dedupe file")
        if old.get("compared") == compared and all(old.get(k) == v for k, v in settings.items()):
            return old
    _dsp()
    pairs = dedupe_pairs(sel, results, ids, work)
    gone = {}
    for pr in sorted(pairs, key=lambda q: (dedupe_rank(results[q["a"]], sel), q["b"])):
        if pr["a"] in gone or pr["b"] in gone:
            continue  # a clip already gone keeps nobody out, and is not kept out twice
        if pr["rule"] == "near_duplicate":
            why = (f"near-duplicate of {pr['a']} (similarity {pr['similarity']}: cross-correlation {pr['full']} full band at "
                   f"{pr['lag_ms']} ms, {', '.join(f'{v} above {k} Hz' for k, v in pr['bands'].items())}; {NEAR_DUP_NCC:g} or more "
                   f"is one sound twice)")
        else:
            why = (f"the same audio as {pr['a']} by {pr['authors'][0]} (cross-correlation {pr['full']} full band at {pr['lag_ms']} ms): "
                   f"{pr['authors'][1]}'s upload is only as CC0 as the original; check the original's page")
        gone[pr["b"]] = {"reason": why, "of": pr["a"], "rule": pr["rule"]}
    record = dict(settings, compared=compared, pairs=pairs, rejected=gone)
    write_if_changed(path, json_text(record).encode("utf-8"))
    return record


def cmd_dedupe(args, sel=None):
    """The near-duplicate check (all runs it before write, which applies work/dedupe.json): its pairs and rejections."""
    sel = sel or Selection(args.selection)
    work = Path(args.work)
    results = work_results(sel, work)
    jpath = work / JUDGE
    judge = load_json(jpath, MAX_JSON_BYTES, "judge file") if jpath.is_file() else {"editor": {"available": False}, "clips": {}}
    record = dedupe_record(sel, work, results, judge)
    print(f"dedupe: {len(record['compared'])} clips compared, {len(record['pairs'])} near pair(s), {len(record['rejected'])} rejected")
    for pr in record["pairs"]:
        print(f"dedupe: pair {pr['a']} ~ {pr['b']}: {pr['rule']}, similarity {pr['similarity']}, full {pr['full']}, {pr['bands']}")
    for cid, g in sorted(record["rejected"].items()):
        print(f"dedupe: {cid} rejected: {g['reason']}")


# ---------------------------------------------------------------------------------------------
# The semantic audit stage's recorded file (§2.11.9): read here with the standard library only

def read_semantic_file(path):
    """(header, clip lines, problems) of library-semantic.jsonl: a stage header, then one line per clip id, ids
    sorted, keys sorted (the stage writes it; nothing here needs its environment)."""
    problems, lines, header = [], [], None
    text = Path(path).read_text(encoding="utf-8")
    if not text.endswith("\n"):
        problems.append("the semantic file does not end with a newline")
    for n, raw in enumerate(text.splitlines(), 1):
        try:
            rec = json.loads(raw)
        except ValueError:
            problems.append(f"semantic file line {n} is not JSON")
            continue
        if not isinstance(rec, dict) or raw != json.dumps(rec, sort_keys=True, ensure_ascii=False):
            problems.append(f"semantic file line {n}: keys not sorted (or not the stage's layout)")
            if not isinstance(rec, dict):
                continue
        if n == 1:
            if rec.get("line") != "stage":
                problems.append("the semantic file's first line is not the stage header")
            header = rec
            continue
        label = rec.get("label") if isinstance(rec.get("label"), dict) else {}
        heard = rec.get("heard") if isinstance(rec.get("heard"), list) else None
        if (rec.get("line") != "clip" or not isinstance(rec.get("id"), str) or not isinstance(rec.get("audio_sha256"), str)
                or label.get("verdict") not in SEMANTIC_LABELS or heard is None or len(heard) > 3
                or not all(isinstance(h, dict) and isinstance(h.get("tag"), str) for h in heard)):
            problems.append(f"semantic file line {n} is not a clip line")
            continue
        lines.append(rec)
    ids = [r["id"] for r in lines]
    if ids != sorted(set(ids)):
        problems.append("the semantic file's clip lines are not sorted by id, one line per clip id")
    return header, lines, problems


class SemanticFile:
    """The recorded readings a build uses: current for a clip when the line has its encoded file's SHA-256, its type
    and its description (a retyped or re-described clip needs a new reading)."""

    def __init__(self, sel, path, allow_fake):
        self.path = Path(path)
        self.lines = {}
        self.problems = []
        self.present = self.path.is_file()
        if not sel.semantic or not self.present:
            return
        header, lines, self.problems = read_semantic_file(self.path)
        if header is not None:
            if header.get("model") != sel.semantic_pins():
                self.problems.append(f"the semantic file's model pin {header.get('model')} is not the selection's {sel.semantic_pins()}")
            if header.get("embedder") != "clap" and not (allow_fake and header.get("embedder") == "fake"):
                self.problems.append(f"the semantic file was written by the {header.get('embedder')!r} embedder"
                                     + (" (tests only: --allow-fake-semantic)" if header.get("embedder") == "fake" else ""))
        self.lines = {r["id"]: r for r in lines}

    def current(self, cid, sha, clip):
        r = self.lines.get(cid)
        if r and r["audio_sha256"] == sha and r.get("type") == clip["type"] and r.get("description") == clip["description"]:
            return r
        return None

    @staticmethod
    def reading(line):
        """The manifest's semantic object for a line: {label, heard}."""
        return {"label": line["label"]["verdict"], "heard": [h["tag"] for h in line["heard"]]}


def model_file_problem(path, f):
    """Why a cached model file is not the pinned one (None when it is): size, SHA-256 and git blob SHA-1."""
    size = path.stat().st_size
    if size != f["bytes"]:
        return f"{size} bytes, pinned {f['bytes']}"
    h256, h1 = hashlib.sha256(), hashlib.sha1(b"blob %d\0" % size)
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h256.update(chunk)
            h1.update(chunk)
    if "sha256" in f and h256.hexdigest() != f["sha256"]:
        return f"SHA-256 {h256.hexdigest()}, pinned {f['sha256']}"
    if "blob_sha1" in f and h1.hexdigest() != f["blob_sha1"]:
        return f"git blob SHA-1 {h1.hexdigest()}, pinned {f['blob_sha1']}"
    return None


def model_dir(cache, sel):
    d = Path(cache) / "clap" / sel.semantic["model"]["revision"]
    r = d.resolve()
    for inside in (ROOT.resolve(), DEFAULT_OUT.resolve()):
        if r == inside or inside in r.parents:
            raise BuildError(f"{d} is inside {inside}: the model cache is never inside the repository or the library tree")
    return d


def cmd_semantic_fetch(args, fetcher=None):
    sel = Selection(args.selection)
    if not sel.semantic:
        raise BuildError(f"{sel.name} has no semantic block (the model pins)")
    m = sel.semantic["model"]
    d = model_dir(args.cache, sel)
    d.mkdir(parents=True, exist_ok=True)
    fetcher = fetcher or PoliteFetcher(HF_HOSTS, USER_AGENT, min_interval=REQUEST_INTERVAL_S, offline=args.offline)
    errors = []
    for f in m["files"]:
        dest = d / f["name"]
        if dest.is_file() and not dest.is_symlink():
            why = model_file_problem(dest, f)
            if why is None:
                print(f"semantic-fetch: {f['name']} cached, {f['bytes']} bytes, SHA-256 {file_digest(dest)}: pins ok")
                continue
            print(f"semantic-fetch: {f['name']} in the cache is not the pinned file ({why}); fetching it again")
            dest.unlink()
        url = f"https://huggingface.co/{m['repo']}/resolve/{m['revision']}/{f['name']}"
        part = dest.with_name(dest.name + ".part")
        try:
            fetcher.get_to(url, part, f["bytes"], timeout=120)
            why = model_file_problem(part, f)
            if why:
                raise BuildError(f"{url}: {why}; nothing was kept")
            part.replace(dest)
            print(f"semantic-fetch: {f['name']} fetched, {f['bytes']} bytes, SHA-256 {file_digest(dest)}: pins ok")
        except (BuildError, OSError, urllib.error.URLError) as e:
            errors.append(f"{f['name']}: {e}")
        finally:
            if part.exists():
                part.unlink()
    print(f"semantic-fetch: {fetcher.requests} network request(s), into {d}")
    if errors:
        raise BuildError("semantic-fetch failed:\n  " + "\n  ".join(errors))


def file_digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def uv_command():
    found = shutil.which("uv")
    return found or str(Path.home() / ".local/bin/uv")


# The stage's environment: semantic-requirements.txt (every wheel by --hash, torch's CPU wheel from PyTorch's index)
# installed with `uv pip install --require-hashes` into a venv in the cache, one per requirements file. (`uv run
# --with-requirements` was tried first: it does not check the hashes, so a changed wheel would go unnoticed.)
TORCH_CPU_INDEX = "https://download.pytorch.org/whl/cpu"
SEMANTIC_PYTHON = "3.13"


def semantic_python(cache):
    """The stage's interpreter in <cache>/semantic-env/<requirements hash>/, installed when missing."""
    digest = sha256(SEMANTIC_REQUIREMENTS.read_bytes())
    env = Path(cache) / "semantic-env" / digest[:16]
    py, stamp = env / "bin" / "python", env / "installed.sha256"
    if py.is_file() and stamp.is_file() and stamp.read_text(encoding="utf-8").strip() == digest:
        return py
    if env.exists():
        shutil.rmtree(env)
    uv = uv_command()
    for cmd in ([uv, "venv", "--quiet", "--python", SEMANTIC_PYTHON, str(env)],
                [uv, "pip", "install", "--quiet", "--python", str(py), "--require-hashes", "--extra-index-url", TORCH_CPU_INDEX,
                 "--index-strategy", "unsafe-best-match", "-r", str(SEMANTIC_REQUIREMENTS)]):
        r = subprocess.run(cmd, stdin=subprocess.DEVNULL)
        if r.returncode != 0:
            shutil.rmtree(env, ignore_errors=True)
            raise BuildError(f"the stage's environment did not install from {SEMANTIC_REQUIREMENTS.name} (exit {r.returncode}): {' '.join(cmd[:3])}")
    stamp.write_text(digest + "\n", encoding="utf-8")
    return py


def cmd_semantic(args):
    """The stage in its own pinned environment (the real model), or in this one (--embedder fake, tests only)."""
    sel = Selection(args.selection)
    if not sel.semantic:
        raise BuildError(f"{sel.name} has no semantic block (the model pins)")
    stage = [str(SEMANTIC_STAGE), "--selection", str(args.selection), "--work", str(args.work), "--out", str(args.semantic_file),
             "--embedder", args.embedder] + (["--cache", str(args.cache)] if args.cache else []) + (["--check"] if args.check else [])
    env = dict(os.environ, HF_HUB_OFFLINE="1", TRANSFORMERS_OFFLINE="1")
    if args.embedder == "fake":
        cmd = [sys.executable, *stage]
    else:
        if not args.cache:
            raise BuildError("semantic needs --cache (the model files are in <cache>/clap/<revision>/, from semantic-fetch)")
        model_dir(args.cache, sel)
        # the pinned environment first (installing it may download the hashed wheels), then the stage without the network
        cmd = [str(semantic_python(args.cache)), *stage]
        if subprocess.run(["unshare", "-rn", "true"], capture_output=True).returncode == 0:
            cmd = ["unshare", "-rn", *cmd]
        else:
            print("semantic: no network namespace here (unshare -rn failed); the stage runs offline by HF_HUB_OFFLINE and "
                  "TRANSFORMERS_OFFLINE")
    r = subprocess.run(cmd, stdin=subprocess.DEVNULL, env=env)
    if r.returncode != 0:
        raise BuildError(f"the semantic stage failed (exit {r.returncode})")


# ---------------------------------------------------------------------------------------------
# write

def final_verdicts(sel, results, judge, semantic=None, dedupe=None):
    """(accepted ids, audit clip records) combining the Python and editor verdicts and the near-duplicate check; each
    record carries the clip's current semantic reading (and the curator's review_note) when there is one."""
    available = judge["editor"]["available"]
    gone = (dedupe or {}).get("rejected") or {}
    accepted, records = [], {}
    for cid, res in results.items():
        clip = sel.clips[cid][0]
        line = semantic.current(cid, res["ogg"]["sha256"], clip) if semantic and res.get("ogg") else None
        sem = None
        if line:
            sem = {"label": line["label"], "heard": line["heard"]}
            if clip.get("review_note"):
                sem["review_note"] = clip["review_note"]
        reasons = list(res["reasons"])
        if available and res["verdict"] == "accepted":
            j = judge["clips"].get(cid)
            if j is None:
                raise BuildError(f"{cid}: not judged by the editor; run judge again")
            reasons += j["reasons"]
            editor = {"verdict": j["verdict"], "item": j["item"], "notes": j["notes"]}
        elif available:
            editor = {"verdict": "not run (rejected by the Python filters)"}
        else:
            editor = {"skipped": judge["editor"]["reason"]}
        if cid in gone:
            reasons.append(gone[cid]["reason"])
        verdict = "accepted" if res["verdict"] == "accepted" and not reasons else "rejected"
        if verdict == "accepted":
            accepted.append(cid)
        records[cid] = {"line": "clip", "id": cid, "source": res["source"], "type": res["type"], "kind": res["kind"],
                        "verdict": verdict, "reasons": reasons, "processing": res["processing"], "python": res["python"],
                        "measured": res["measured"], "defects": res.get("defects"), "import": res.get("import"), "ogg": res["ogg"],
                        "editor": editor, "semantic": sem}
    return sorted(accepted), records


def audit_lines(results, records):
    sources = {}
    for res in results.values():
        sources.setdefault(res["source"], dict(res["evidence"], line="source", source=res["source"]))
    rows = [((key, 0, ""), rec) for key, rec in sources.items()] + [((r["source"], 1, cid), r) for cid, r in records.items()]
    rows.sort(key=lambda r: r[0])
    return "".join(json.dumps(rec, sort_keys=True, ensure_ascii=False) + "\n" for _, rec in rows)


def is_library_dir(out):
    """An empty or missing directory, or one holding a library manifest: the only places write may delete files."""
    if not out.exists():
        return True
    if not out.is_dir():
        return False
    if not any(out.iterdir()):
        return True
    m = out / MANIFEST
    try:
        return m.is_file() and json.loads(m.read_text(encoding="utf-8")).get("format") == MANIFEST_FORMAT
    except (ValueError, UnicodeDecodeError, OSError):
        return False


def cmd_write(args, sel=None):
    sel = sel or Selection(args.selection)
    work, out = Path(args.work), Path(args.out)
    results = work_results(sel, work)
    jpath = work / JUDGE
    if not jpath.is_file():
        raise BuildError(f"{jpath} is missing; run judge first")
    judge = load_json(jpath, MAX_JSON_BYTES, "judge file")
    # the near-duplicate check's record (dedupe, which all runs before write); write needs no DSP package, as before
    dedupe = None
    dpath = work / DEDUPE
    if dpath.is_file():
        dedupe = load_json(dpath, MAX_JSON_BYTES, "dedupe file")
        now = {cid: (r.get("ogg") or {}).get("sha256") for cid, r in results.items()}
        compared = dedupe.get("compared") or {}
        changed = sorted(cid for cid in now if r_accepted(results[cid], judge) and compared.get(cid) != now[cid])
        if changed:
            # a pair applies only while both its clips are as dedupe compared them (prepare --only changed some since)
            dedupe = dict(dedupe, rejected={b: g for b, g in (dedupe.get("rejected") or {}).items()
                                            if compared.get(b) == now.get(b) and compared.get(g["of"]) == now.get(g["of"])})
            print(f"write: {len(changed)} accepted clip(s) changed since dedupe compared them ({', '.join(changed[:3])}): their "
                  f"near-duplicate check is not current; run dedupe again (all does)")
    else:
        print(f"write: no near-duplicate check ({dpath} is missing: run dedupe, as all does)")
    semantic = SemanticFile(sel, args.semantic_file, args.allow_fake_semantic)
    accepted, records = final_verdicts(sel, results, judge, semantic, dedupe)
    if not is_library_dir(out):
        raise BuildError(f"{out} is not empty and holds no {MANIFEST_FORMAT} manifest: refusing to write (and delete) there")

    # The semantic readings come from the recorded file alone (§2.11.9): the model never runs here.
    problems = []
    if sel.semantic:
        if not semantic.present and sel.semantic["required"]:
            problems.append(f"{semantic.path} is missing and the selection's semantic.required is true: run semantic")
        problems += semantic.problems
    for cid in accepted:
        line = semantic.current(cid, results[cid]["entry"]["sha256"], sel.clips[cid][0]) if sel.semantic and not semantic.problems else None
        if line:
            results[cid]["entry"]["semantic"] = SemanticFile.reading(line)
            if line["label"]["verdict"] == "review" and not sel.clips[cid][0].get("review_note"):
                problems.append(f"{cid}: the semantic stage reads it as review (label z {line['label']['z']}, margin "
                                f"{line['label']['margin']} against {line['label']['confusion']!r}): look at it (review --semantic), "
                                f"then retype, re-describe or reject it, or record the decision as its review_note")
        elif sel.semantic and sel.semantic["required"] and not semantic.problems:
            problems.append(f"{cid}: no current semantic reading (semantic.required): run semantic")
    entries = [canonical_entry(results[cid]["entry"]) for cid in accepted]
    packs = []
    for pid in sel.used_packs(accepted):
        p = sel.packs[pid]
        mp = {"id": pid, "title": p["title"], "url": p["url"], "landing": p["landing"], "sha256": p["sha256"], "bytes": p["bytes"],
              "licence": LICENCE, "licence_file": f"licenses/{pid}.txt", "author": KENNEY_AUTHOR}
        if p.get("credits_member"):
            mp["credits_file"] = f"licenses/{pid}.credits.txt"
        packs.append(mp)
    manifest = {"format": MANIFEST_FORMAT, "version": sel.library_version, "licence": LICENCE,
                "generated_by": "tools/sound-library/build_library.py", "packs": packs, "clips": entries}
    mtext = manifest_text(manifest).encode("utf-8")
    files = {MANIFEST: mtext, ".gitattributes": b"* -text\n*.ogg binary\n"}
    for mp in packs:
        for key in ("licence_file", "credits_file"):
            if key in mp:
                src = work / mp[key]
                if not src.is_file():
                    raise BuildError(f"{src} is missing; run prepare again")
                files[mp[key]] = src.read_bytes()
    for e in entries:
        data = clip_file(work, e["id"], ".ogg").read_bytes()
        if sha256(data) != e["sha256"] or len(data) != e["bytes"]:
            raise BuildError(f"{e['id']}: the work file changed after prepare; run prepare again")
        files[e["path"]] = data

    # Budgets (§2.11.6), before anything is written.
    total = sum(len(b) for b in files.values())
    if total > sel.budget["max_bytes"]:
        problems.append(f"the tree would be {total} bytes, over budget.max_bytes {sel.budget['max_bytes']}")
    lo, hi = sel.budget["total"]
    if not lo <= len(entries) <= hi:
        problems.append(f"{len(entries)} accepted clips, outside budget.total {lo}-{hi}")
    per_type = {}
    for e in entries:
        per_type[e["type"]] = per_type.get(e["type"], 0) + 1
    for typ, n in sorted(sel.budget["min_per_type"].items()):
        if per_type.get(typ, 0) < n:
            problems.append(f"{typ}: {per_type.get(typ, 0)} clips, under its minimum {n}")
    for e in entries:
        wlo, whi = TYPES[e["type"]]["win"]
        if not wlo <= e["measured"]["duration_s"] <= whi:
            problems.append(f"{e['id']}: {e['measured']['duration_s']} s outside its window {wlo}-{whi}")

    # The library version goes up when an existing clip's bytes change or a clip goes (§2.2.2).
    by_id = {e["id"]: e for e in entries}
    previous = {}
    old_manifest = out / MANIFEST
    if old_manifest.is_file():
        old = json.loads(old_manifest.read_text(encoding="utf-8"))
        previous = {c["id"]: c for c in old.get("clips", [])}
        changed = [i for i, c in previous.items() if i in by_id and by_id[i]["sha256"] != c["sha256"]]
        removed = [i for i in previous if i not in by_id]
        if old.get("version", 0) > sel.library_version:
            problems.append(f"the selection's library_version {sel.library_version} is lower than the written library's {old.get('version')}")
        elif (changed or removed) and old.get("version") == sel.library_version:
            problems.append(f"{len(changed)} clip(s) change and {len(removed)} go, so library_version must go up from {sel.library_version}")
    if problems:
        raise BuildError("write refused, nothing was written:\n  " + "\n  ".join(problems))

    out.mkdir(parents=True, exist_ok=True)
    written = sum(write_if_changed(out / rel, data) for rel, data in files.items())
    editor_ok = judge["editor"]["available"]
    keep = set(files) | ({ANALYSIS} if editor_ok else set())
    stale = sorted(rel for rel in tree_files(out) - keep if (out / rel).is_file() and not (out / rel).is_symlink())
    for rel in stale:
        (out / rel).unlink()
    for d in sorted((p for p in out.rglob("*") if p.is_dir()), key=lambda p: len(p.parts), reverse=True):
        if not any(d.iterdir()):
            d.rmdir()
    audit = Path(args.audit)
    write_if_changed(audit, audit_lines(results, records).encode("utf-8"))

    # Summary (the churn rule of §2.3.1 reads it).
    added = [i for i in by_id if i not in previous]
    changed = [i for i in by_id if i in previous and previous[i]["sha256"] != by_id[i]["sha256"]]
    removed = [i for i in previous if i not in by_id]
    per_kind = {}
    for e in entries:
        per_kind[e["kind"]] = per_kind.get(e["kind"], 0) + e["bytes"]
    print(f"write: {len(entries)} clips ({len(records) - len(entries)} rejected), {total} bytes of {sel.budget['max_bytes']}; "
          f"{written} file(s) written, {len(stale)} removed{': ' + ', '.join(stale) if stale else ''}")
    for typ in sorted(set(per_type) | set(sel.budget["min_per_type"])):
        print(f"write:   {typ:11s} {per_type.get(typ, 0):4d} clips (minimum {sel.budget['min_per_type'].get(typ, 0)})")
    print("write:   bytes per kind: " + ", ".join(f"{k} {v}" for k, v in sorted(per_kind.items())))
    print(f"write:   against the previous manifest: {len(added)} added ({sum(by_id[i]['bytes'] for i in added)} bytes), "
          f"{len(changed)} changed ({sum(by_id[i]['bytes'] for i in changed)} bytes), "
          f"{len(removed)} removed ({sum(previous[i]['bytes'] for i in removed)} bytes)")
    print(f"write: audit log {audit}")
    if editor_ok:
        analyse(out, judge["editor"]["path"])
    else:
        print(f"write: analysis.json not written: {judge['editor']['reason']}")
    verify(out, sel, audit, allow_no_analysis=not editor_ok, semantic_file=args.semantic_file,
           allow_fake_semantic=args.allow_fake_semantic)


# ---------------------------------------------------------------------------------------------
# analyse

def analyse(out, editor):
    out = Path(out)
    manifest = load_json(out / MANIFEST, MAX_JSON_BYTES, "manifest")
    ids = [c["id"] for c in manifest.get("clips", [])]
    info, why = editor_types(editor)
    if info is None:
        raise BuildError(why)
    env = dict(os.environ, JASPERENGINE_SOUND_LIBRARY_DIR=str(out.resolve()))
    res = run_editor_json(editor, [*info["command"], "--json", "--detail", "summary", "library:*"], env=env)
    if res.get("errors"):
        raise BuildError(f"the analyzer failed on {len(res['errors'])} clip(s): {res['errors'][:3]}")
    items = {}
    for it in res.get("items") or []:
        ref = it.get("path", "")
        if not ref.startswith("library:"):
            raise BuildError(f"audio analyze returned an item for {ref!r}, not a library: id")
        item = {k: v for k, v in it.items() if k not in ANALYSIS_DROPPED}
        if isinstance(item.get("format"), dict):
            item["format"] = {k: v for k, v in item["format"].items() if k != "load_type"}
        items[ref[len("library:"):]] = item
    if sorted(items) != sorted(ids):
        raise BuildError("audio analyze did not return exactly one item per manifest clip")
    lines = [f"{json.dumps(i, ensure_ascii=False)}: {json.dumps(items[i], sort_keys=True, separators=(',', ':'), ensure_ascii=False)}"
             for i in sorted(items)]
    head = (f'{{"format": "{ANALYSIS_FORMAT}", "version": 1, "analyzer_version": {info["analyzer_version"]}, '
            f'"generated_by": "jasperengine-editor {" ".join(info["command"])}", "items": {{\n')
    text = head + ",\n".join(lines) + "\n}}\n"
    write_if_changed(out / ANALYSIS, text.encode("utf-8"))
    print(f"analyse: {len(items)} items, analyzer v{info['analyzer_version']}, into {out / ANALYSIS}")


def cmd_analyse(args):
    info, why = editor_types(args.editor)
    if info is None:
        raise BuildError(f"analyse needs the editor's 'audio analyze' command: {why}")
    analyse(args.out, args.editor)


# ---------------------------------------------------------------------------------------------
# verify (standard library only)

def verify(out, sel, audit=None, allow_no_analysis=False, semantic_file=DEFAULT_SEMANTIC, allow_fake_semantic=False):
    out = Path(out)
    errors = []
    e = errors.append
    mpath = out / MANIFEST
    if not mpath.is_file():
        raise BuildError(f"{mpath} is missing")
    manifest = load_json(mpath, MAX_JSON_BYTES, "manifest")
    if manifest.get("format") != MANIFEST_FORMAT or manifest.get("version") != sel.library_version:
        e(f"manifest format/version is not {MANIFEST_FORMAT} v{sel.library_version}")
    # the clips no version may ship, and the marks no clip may name (S36 R21)
    excluded = {item["id"] for item in sel.excluded}
    for c in manifest.get("clips") or []:
        words = " ".join([str(c.get("id")), str(c.get("path")), str(c.get("description"))] + [str(t) for t in c.get("tags") or []]).lower()
        if c.get("id") in excluded:
            e(f"{c.get('id')} is excluded: {next(i['reason'] for i in sel.excluded if i['id'] == c.get('id'))}")
        for term in sel.denied_terms:
            if re.search(r"(?<![a-z0-9])" + re.escape(term.lower()) + r"(?![a-z0-9])", words.replace("_", " ")):
                e(f"{c.get('id')}: the denied term {term!r} appears in its id, path, tags or description")
    for item in sel.excluded:
        if (out / f"{item['id']}.ogg").exists():
            e(f"{item['id']} is excluded but its file is in the tree")
    if manifest.get("licence") != LICENCE or manifest.get("generated_by") != "tools/sound-library/build_library.py":
        e("manifest licence or generated_by is wrong")
    if mpath.read_bytes() != manifest_text(manifest).encode("utf-8"):
        e("manifest.json is not in the canonical layout (indent 1, LF, final newline)")
    clips = manifest.get("clips", [])
    ids = [c.get("id") for c in clips]
    if ids != sorted(set(ids)):
        e("manifest clip ids are not unique and sorted")

    # The manifest against the selection.
    for c in clips:
        cid = c.get("id")
        if not isinstance(cid, str) or not ID_RE.match(cid) or cid not in sel.clips:
            e(f"{cid}: not a clip of the selection")
            continue
        clip, src = sel.clips[cid]
        expect = {"path": cid + ".ogg", "type": clip["type"], "kind": clip["kind"], "tags": clip["tags"],
                  "description": clip["description"], "licence": LICENCE, "source": src["source"]}
        if src["source"] == "kenney":
            pack = sel.packs[src["pack"]]
            expect.update(pack=src["pack"], source_file=src["member"], author=KENNEY_AUTHOR, url=pack["landing"])
            att = kenney_attribution(pack, src["member"])
            if att:
                expect["attribution"] = att
        else:
            expect.update(pack=None, source_file=None, source_id=src["id"], author=src["username"], url=src["landing"],
                          attribution=f"{src['username']} (freesound.org)")
            if normalise_licence(c.get("licence_url")) != CC0_DEED:
                e(f"{cid}: licence_url {c.get('licence_url')!r} is not CC0's")
        for key, value in expect.items():
            if c.get(key) != value:
                e(f"{cid}: manifest {key} {c.get(key)!r}, the selection says {value!r}")
        if src["source"] == "kenney" and "attribution" not in expect and "attribution" in c:
            e(f"{cid}: unexpected attribution")
        if not isinstance(c.get("processing"), dict) or not isinstance(c.get("measured"), dict):
            e(f"{cid}: processing or measured missing")
            continue
        if (list(c) != [k for k in CLIP_KEYS if k in c] or list(c["processing"]) != [k for k in PROCESSING_KEYS if k in c["processing"]]
                or list(c["measured"]) != [k for k in MEASURED_KEYS if k in c["measured"]]):
            e(f"{cid}: manifest keys are not in the canonical order (or unknown)")
        imp = clip_importer_fades(c)
        if c.get("meta") != soundlib.clip_meta(c, imp, "library"):
            e(f"{cid}: meta is not the .meta sections an import of it writes")
        if imp:
            if (list(imp) != [k for k in IMPORT_KEYS if k in imp]
                    or not all(is_num(v) and 0 <= v <= 1000 for v in imp.values())):
                e(f"{cid}: meta's fades must hold only fade_in_ms and fade_out_ms (0..1000 ms), in that order")
            elif clip["kind"] != "loop" or imp != {"fade_in_ms": LOOP_FADE_IN_MS}:
                e(f"{cid}: fades {imp} are not ones prepare derives (only a loop whose first frame clicks gets "
                  f"{{'fade_in_ms': {LOOP_FADE_IN_MS:g}}})")
        lo, hi = TYPES[clip["type"]]["win"]
        if not lo <= c["measured"].get("duration_s", -1) <= hi:
            e(f"{cid}: duration {c['measured'].get('duration_s')} s outside its window {lo}-{hi}")
    verify_semantic(clips, sel, semantic_file, allow_fake_semantic, e)
    mpacks = {p.get("id"): p for p in manifest.get("packs", [])}
    used = sorted({sel.clips[c["id"]][1]["pack"] for c in clips if c.get("id") in sel.clips and sel.clips[c["id"]][1]["source"] == "kenney"})
    if [p.get("id") for p in manifest.get("packs", [])] != used:
        e("manifest packs are not exactly the packs its clips use, sorted")
    for pid, mp in mpacks.items():
        p = sel.packs.get(pid, {})
        for key in ("url", "sha256", "bytes", "landing", "title"):
            if mp.get(key) != p.get(key):
                e(f"pack {pid}: manifest {key} {mp.get(key)!r}, the selection says {p.get(key)!r}")
        if mp.get("licence_file") != f"licenses/{pid}.txt" or mp.get("licence") != LICENCE or mp.get("author") != KENNEY_AUTHOR:
            e(f"pack {pid}: licence, licence_file or author is wrong")
        if bool(p.get("credits_member")) != ("credits_file" in mp) or ("credits_file" in mp and mp["credits_file"] != f"licenses/{pid}.credits.txt"):
            e(f"pack {pid}: credits_file is wrong")

    # Files.
    expected = {MANIFEST, ".gitattributes"}
    for c in clips:
        rel = f"{c.get('id')}.ogg"
        expected.add(rel)
        f = out / rel
        if f.is_symlink() or not f.is_file():
            e(f"{rel} is missing or not a regular file")
            continue
        data = f.read_bytes()
        if len(data) != c.get("bytes"):
            e(f"{rel}: {len(data)} bytes, manifest says {c.get('bytes')}")
        if sha256(data) != c.get("sha256"):
            e(f"{rel}: SHA-256 {sha256(data)}, manifest says {c.get('sha256')}")
    for pid, mp in mpacks.items():
        for key in ("licence_file", "credits_file"):
            rel = mp.get(key)
            if not rel:
                continue
            expected.add(rel)
            f = out / rel
            if f.is_symlink() or not f.is_file():
                e(f"{rel} is missing")
                continue
            if key == "licence_file":
                text = f.read_bytes().decode("utf-8", errors="replace")
                for marker in LICENCE_MARKERS:
                    if marker not in text:
                        e(f"{rel} does not contain {marker!r}")

    # analysis.json
    apath = out / ANALYSIS
    if apath.exists():
        expected.add(ANALYSIS)
        try:
            analysis = load_json(apath, MAX_JSON_BYTES, "analysis")
            if analysis.get("format") != ANALYSIS_FORMAT or analysis.get("version") != 1 or not is_int(analysis.get("analyzer_version")):
                e(f"analysis.json format/version is not {ANALYSIS_FORMAT} v1 with an analyzer_version")
            items = analysis.get("items") or {}
            if sorted(items) != sorted(ids):
                e("analysis.json does not name exactly the manifest's clips")
            for cid, item in items.items():
                f = out / f"{cid}.ogg"
                if f.is_file() and (not isinstance(item, dict) or item.get("sha1") != hashlib.sha1(f.read_bytes()).hexdigest()):
                    e(f"analysis.json: {cid}'s sha1 is not its file's")
        except BuildError as ex:
            e(str(ex))
    elif not allow_no_analysis:
        e("analysis.json is missing (written by build_library.py analyse with the editor)")

    extra = sorted(tree_files(out) - expected)
    if extra:
        e(f"not in the manifest: {', '.join(extra)}")
    total = sum((out / rel).stat().st_size for rel in tree_files(out) if (out / rel).is_file())
    if total > sel.budget["max_bytes"]:
        e(f"the tree is {total} bytes, over budget.max_bytes {sel.budget['max_bytes']}")
    lo, hi = sel.budget["total"]
    if not lo <= len(clips) <= hi:
        e(f"{len(clips)} clips, outside budget.total {lo}-{hi}")
    per_type = {}
    for c in clips:
        per_type[c.get("type")] = per_type.get(c.get("type"), 0) + 1
    for typ, n in sorted(sel.budget["min_per_type"].items()):
        if per_type.get(typ, 0) < n:
            e(f"{typ}: {per_type.get(typ, 0)} clips, under its minimum {n}")

    # The audit log: every manifest clip accepted there, every other selected clip rejected there.
    if audit is not None and Path(audit).is_file():
        accepted, rejected, imports = set(), set(), {}
        for line in Path(audit).read_text(encoding="utf-8").splitlines():
            rec = json.loads(line)
            if rec.get("line") == "clip":
                (accepted if rec.get("verdict") == "accepted" else rejected).add(rec.get("id"))
                imports[rec.get("id")] = rec.get("import")
        in_manifest = set(ids)
        for c in clips:
            if c.get("id") in accepted and clip_importer_fades(c) != imports.get(c.get("id")):
                e(f"{c.get('id')}: manifest fades {clip_importer_fades(c)} are not the ones prepare derived ({imports.get(c.get('id'))}, "
                  f"the audit log)")
        for cid in sorted(in_manifest - accepted):
            e(f"{cid}: in the manifest without an accepted line in the audit log")
        for cid in sorted(accepted - in_manifest):
            e(f"{cid}: accepted in the audit log but not in the manifest")
        for cid in sorted(set(sel.clips) - in_manifest - rejected):
            e(f"{cid}: selected, but neither in the manifest nor rejected in the audit log")
    elif audit is not None and Path(audit) != DEFAULT_AUDIT:
        e(f"audit log {audit} is missing")

    if errors:
        raise BuildError("verify failed:\n  " + "\n  ".join(errors))
    print(f"verify: {len(clips)} clips, {total} bytes, version {manifest.get('version')}, {len(mpacks)} licences: ok")


def verify_semantic(clips, sel, path, allow_fake, e):
    """§2.11.6: the semantic file's header pins are the selection's, it is sorted with one line per clip id, every
    manifest semantic is what the file gives for that clip's encoded SHA-256, every review has a review_note,
    and with semantic.required every clip has a current line."""
    for c in clips:
        sem = c.get("semantic")
        if "semantic" in c and (not isinstance(sem, dict) or list(sem) != ["label", "heard"] or sem["label"] not in SEMANTIC_LABELS
                                or not isinstance(sem["heard"], list) or len(sem["heard"]) > 3
                                or not all(isinstance(h, str) and h for h in sem["heard"])):
            e(f"{c.get('id')}: semantic must be {{label: ok|weak|review, heard: up to 3 tags}}")
    if not sel.semantic:
        for c in clips:
            if "semantic" in c:
                e(f"{c.get('id')}: a semantic reading, but the selection has no semantic block")
        return
    sf_ = SemanticFile(sel, path, allow_fake)
    if not sf_.present:
        if sel.semantic["required"] or any("semantic" in c for c in clips):
            e(f"the semantic file {path} is missing")
        return
    for p in sf_.problems:
        e(p)
    for c in clips:
        cid = c.get("id")
        if cid not in sel.clips:
            continue
        clip = sel.clips[cid][0]
        line = sf_.current(cid, c.get("sha256"), clip)
        if line is None:
            if "semantic" in c:
                e(f"{cid}: manifest semantic {c['semantic']}, but the semantic file has no current reading of this clip")
            if sel.semantic["required"]:
                e(f"{cid}: no current semantic reading (semantic.required)")
            continue
        if c.get("semantic") != SemanticFile.reading(line):
            e(f"{cid}: manifest semantic {c.get('semantic')}, the semantic file gives {SemanticFile.reading(line)}")
        if line["label"]["verdict"] == "review" and not clip.get("review_note"):
            e(f"{cid}: labelled review by the semantic stage without a review_note in the selection")


def cmd_migrate_manifest(args):
    """The offline conversion that produced the committed v4: a v3 manifest in, a v4 manifest out. Uses the
    selection's exclusions; no network, no audio."""
    sel = load_selection_json(args.selection)
    manifest = json.loads(Path(args.input).read_text(encoding="utf-8"))
    if manifest.get("format") != MANIFEST_FORMAT or manifest.get("version") != 3:
        raise BuildError(f"{args.input} is not a {MANIFEST_FORMAT} v3 manifest")
    excluded = {e["id"] for e in sel.get("excluded", [])}
    Path(args.output).write_bytes(soundlib.manifest_text(soundlib.migrate_manifest(manifest, excluded)).encode("utf-8"))


def cmd_verify(args):
    verify(args.out, Selection(args.selection), args.audit, args.allow_no_analysis, args.semantic_file, args.allow_fake_semantic)


# ---------------------------------------------------------------------------------------------
# review

def cmd_review(args):
    sel = Selection(args.selection)
    info, why = editor_types(args.editor)
    if info is None:
        raise BuildError(f"review needs the editor's 'audio analyze' command: {why}")
    work = Path(args.work)
    results = work_results(sel, work)
    ids = [cid for cid, r in results.items() if r["type"] == args.type and clip_file(work, cid, ".ogg").is_file()]
    if not ids:
        raise BuildError(f"no encoded {args.type} candidates in {work}")
    run_editor_json(args.editor, [*info["command"], "--json", "--type", args.type, "--image", "sheet", "--png", args.png,
                                  *[str(clip_file(work, cid, ".ogg")) for cid in ids]])
    semantic = SemanticFile(sel, args.semantic_file, allow_fake=True) if args.semantic else None
    if semantic is not None and (not sel.semantic or not semantic.present):
        raise BuildError(f"review --semantic needs the selection's semantic block and {args.semantic_file}")
    for n, cid in enumerate(ids, 1):
        r = results[cid]
        line = semantic.current(cid, r["ogg"]["sha256"], sel.clips[cid][0]) if semantic and r.get("ogg") else None
        sem = ""
        if semantic is not None:
            sem = ("  [semantic: no current reading]" if line is None else
                   f"  [semantic: {line['label']['verdict']} z {line['label']['z']} margin {line['label']['margin']} "
                   f"vs {line['label']['confusion']!r}; heard: {', '.join(h['tag'] for h in line['heard']) or '-'}]")
        print(f"{n:3d}  {cid:52s} {r['verdict']}{': ' + '; '.join(r['reasons']) if r['reasons'] else ''}{sem}")
    print(f"review: {len(ids)} {args.type} candidates in {args.png}")


# ---------------------------------------------------------------------------------------------

def cmd_all(args):
    sel = Selection(args.selection)
    cmd_fetch(args, sel)
    args.only = None
    cmd_prepare(args, sel)
    cmd_judge(args, sel)
    cmd_dedupe(args, sel)
    cmd_write(args, sel)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    sub = parser.add_subparsers(dest="command", required=True)

    def add(name, *opts):
        p = sub.add_parser(name)
        p.add_argument("--selection", default=str(DEFAULT_SELECTION), help="the selection (default: %(default)s)")
        for o in opts:
            o(p)
        return p

    cache = lambda p: p.add_argument("--cache", required=True, help="download cache (zips, Freesound pages and previews)")
    work = lambda p: p.add_argument("--work", required=True, help="work directory (prepared clips)")
    out = lambda p: p.add_argument("--out", default=str(DEFAULT_OUT), help="library directory (default: %(default)s)")
    audit = lambda p: p.add_argument("--audit", default=str(DEFAULT_AUDIT), help="audit log (default: %(default)s)")
    editor = lambda p: p.add_argument("--editor", help="jasperengine-editor with the 'audio analyze' command")
    editor_req = lambda p: p.add_argument("--editor", required=True, help="jasperengine-editor with the 'audio analyze' command")
    offline = lambda p: p.add_argument("--offline", action="store_true", help="fail on any network request; use the cache only")
    semantic_file = lambda p: p.add_argument("--semantic-file", default=str(DEFAULT_SEMANTIC),
                                             help="the semantic stage's recorded file (default: %(default)s)")
    allow_fake = lambda p: p.add_argument("--allow-fake-semantic", action="store_true",
                                          help="tests only: accept a semantic file written by the fake embedder")
    refresh = lambda p: p.add_argument("--refresh", action="store_true", help="fetch every Freesound landing page again (licence changes)")

    add("fetch", cache, offline, refresh)
    add("prepare", cache, work, lambda p: p.add_argument("--only", help="clip id glob"))
    add("judge", work, editor)
    add("dedupe", work)
    add("write", work, out, audit, semantic_file, allow_fake)
    add("analyse", out, editor_req)
    add("verify", out, audit, semantic_file, allow_fake, lambda p: p.add_argument("--allow-no-analysis", action="store_true",
                                                                                 help="accept a tree written without the editor (no analysis.json)"))
    add("all", cache, work, editor, out, audit, offline, refresh, semantic_file, allow_fake)
    add("migrate-manifest", lambda p: p.add_argument("--input", required=True, help="a v3 manifest.json"),
        lambda p: p.add_argument("--output", required=True, help="where to write the v4 manifest"))
    add("review", work, editor_req, semantic_file, lambda p: p.add_argument("--type", required=True, choices=sorted(TYPES)),
        lambda p: p.add_argument("--png", required=True),
        lambda p: p.add_argument("--semantic", action="store_true", help="add each clip's semantic reading to the listing"))
    add("semantic-fetch", cache, offline)
    add("semantic", work, semantic_file, lambda p: p.add_argument("--cache", help="the cache holding clap/<revision>/ (the real model)"),
        lambda p: p.add_argument("--check", action="store_true", help="re-run and compare with the recorded file instead of writing it"),
        lambda p: p.add_argument("--embedder", choices=("clap", "fake"), default="clap", help="fake: tests only"))
    add("seed", cache, lambda p: p.add_argument("--opus", required=True, help="opus-sound-layer scripts/library.json (read as data)"),
        lambda p: p.add_argument("--seed-out", required=True, help="where to write the pinned sources"),
        lambda p: p.add_argument("--type", action="append", choices=sorted(TYPES)),
        lambda p: p.add_argument("--ids", help="comma-separated Freesound ids"),
        lambda p: p.add_argument("--limit", type=int))
    args = parser.parse_args(argv)
    commands = {"fetch": cmd_fetch, "prepare": cmd_prepare, "judge": cmd_judge, "write": cmd_write, "analyse": cmd_analyse,
                "verify": cmd_verify, "all": cmd_all, "dedupe": cmd_dedupe, "review": cmd_review, "seed": cmd_seed,
                "semantic-fetch": cmd_semantic_fetch, "semantic": cmd_semantic, "migrate-manifest": cmd_migrate_manifest}
    try:
        commands[args.command](args)
    except BuildError as e:
        print(f"build_library.py {args.command}: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
