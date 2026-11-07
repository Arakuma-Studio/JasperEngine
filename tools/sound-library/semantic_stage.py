#!/usr/bin/env python3
"""The starter library's optional semantic audit stage (Phase 5 spec §2.11.9, decision 14).

A pinned text-to-audio model (CLAP, the selection's semantic.model, loaded from the files
build_library.py semantic-fetch verified) embeds every encoded work clip and its description, and
records per clip a label check (does the clip match its own description and its type's query
better than the type's realistic confusions?) and up to three "heard tags" from
tools/sound-library/semantic-vocab.json, in tools/sound-library/library-semantic.jsonl. The stage
never rejects a clip: build_library.py write reads only the recorded file, so a rebuild needs no
torch, and a "review" label makes write require the curator's review_note.

Run through build_library.py semantic, which installs tools/sound-library/semantic-requirements.txt
(every wheel pinned by --hash) with `uv pip install --require-hashes` into <cache>/semantic-env/ and
starts it as

    <cache>/semantic-env/<hash>/bin/python tools/sound-library/semantic_stage.py \\
        --selection S --work W --cache C --out FILE [--check]

with HF_HUB_OFFLINE=1 and TRANSFORMERS_OFFLINE=1 (and under unshare -n where the host allows it).
--embedder fake (tests only) replaces the model by unit vectors derived from SHA-256 and needs only
numpy, soundfile and scipy.

Calibration (re-implemented from the idea, not from any other project's code or data): the
reference set is the selection's encoded Kenney clips that the Python filters accept plus the 40
essentials (CC0; no Freesound clip, so the clips under check never calibrate themselves). For each
text item q, b_q is the mean cosine of the reference clips with t_q; for a clip a, s_q = cos(a, t_q)
- b_q and z_q = (s_q - mean s) / std s over the vocabulary phrases (population std).

Determinism: fp32, one thread, deterministic algorithms, eval mode, no gradients; audio windows of
at most 10 s cut here (480,000 samples at 48 kHz: one for a shorter clip, else one every 5 s with
the last ending at the clip's end), each passed alone so the feature extractor never truncates.
"""

import argparse
import hashlib
import json
import math
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import build_library as bl  # noqa: E402  (standard library at import; it never imports torch)
from soundlib import BuildError, sha256  # noqa: E402

DEFAULT_VOCAB = HERE / "semantic-vocab.json"
ESSENTIALS = bl.ROOT / "editor/resources/sound-library/essentials"
VOCAB_FORMAT = "jasperengine-semantic-vocab"

RATE = 48000
WINDOW = 480000  # 10 s: the feature extractor's max_length_s at 48 kHz
HOP = 240000  # a window every 5 s for longer clips
FAKE_DIM = 512
# §2.11.9's readings (recorded in the file's header)
LABEL_OK_Z = 2.5
LABEL_WEAK_Z = 2.0
HEARD_Z = 3.0
HEARD_MAX = 3
# semantic --check: every z within CHECK_Z of the file's; a verdict or tag may differ only where a value lies
# within CHECK_Z of its limit (fp32 CPU results move in the last digits between machines)
CHECK_Z = 0.05
DIGITS = 3

np = sf = signal = None


def _numpy():
    global np, sf, signal
    if np is None:
        import numpy
        import soundfile
        from scipy import signal as _signal
        np, sf, signal = numpy, soundfile, _signal


def rnd(v):
    return round(float(v), DIGITS) + 0.0


def dump(obj):
    return json.dumps(obj, sort_keys=True, ensure_ascii=False)


# ---------------------------------------------------------------------------------------------
# The vocabulary

class Vocab:
    """semantic-vocab.json, validated: about 200 phrases in four categories, four templates, and per type of
    the 31 a query and 3-5 confusions."""

    def __init__(self, path):
        self.path = Path(path)
        data = self.path.read_bytes()
        self.sha256 = sha256(data)
        raw = json.loads(data.decode("utf-8"))
        problems = []
        if raw.get("format") != VOCAB_FORMAT or raw.get("version") != 1:
            raise BuildError(f"{self.path}: not a {VOCAB_FORMAT} version 1 file")
        self.templates = raw.get("templates")
        if (not isinstance(self.templates, list) or len(self.templates) != 4
                or not all(isinstance(t, str) and t.count("{}") == 1 for t in self.templates)):
            problems.append("templates must be four strings, each with one {}")
        cats = raw.get("phrases")
        if not isinstance(cats, dict) or sorted(cats) != ["action", "character", "material", "source"]:
            problems.append("phrases must have the categories material, action, source and character")
            cats = {}
        self.phrases = []
        for cat in ("material", "action", "source", "character"):
            for ph in cats.get(cat) or []:
                if not isinstance(ph, str) or not 0 < len(ph) <= 60 or ph in self.phrases:
                    problems.append(f"bad or duplicate phrase {ph!r}")
                    continue
                self.phrases.append(ph)
        if not 150 <= len(self.phrases) <= 300:
            problems.append(f"{len(self.phrases)} phrases; the vocabulary holds about 200")
        types = raw.get("types")
        if not isinstance(types, dict) or set(types) != set(bl.TYPES):
            problems.append(f"types must give each of the {len(bl.TYPES)} sound types")
            types = {}
        self.types = {}
        for name, t in types.items():
            q, conf = (t.get("query"), t.get("confusions")) if isinstance(t, dict) else (None, None)
            if (not isinstance(q, str) or not q or not isinstance(conf, list) or not 3 <= len(conf) <= 5
                    or not all(isinstance(c, str) and c for c in conf) or len(set(conf)) != len(conf) or set(t) != {"query", "confusions"}):
                problems.append(f"type {name}: needs a query and 3-5 distinct confusions")
                continue
            self.types[name] = {"query": q, "confusions": conf}
        if problems:
            raise BuildError(f"{self.path} is invalid:\n  " + "\n  ".join(problems))


# ---------------------------------------------------------------------------------------------
# Audio

def decode_mono48(data):
    """soundfile decode, channel mean, at 48 kHz (resample_poly as the pipeline resamples), float32."""
    import io
    _numpy()
    y, rate = sf.read(io.BytesIO(data), dtype="float64", always_2d=True)
    m = y.mean(axis=1)
    if rate != RATE:
        g = math.gcd(RATE, int(rate))
        m = signal.resample_poly(m, RATE // g, int(rate) // g)
    return np.ascontiguousarray(m, dtype=np.float32)


def windows(m):
    """At most 10 s each: one for a shorter clip; else one every 5 s, the last ending at the clip's end."""
    n = len(m)
    if n <= WINDOW:
        return [m]
    starts = list(range(0, n - WINDOW, HOP)) + [n - WINDOW]
    return [m[s:s + WINDOW] for s in starts]


def unit(v):
    v = np.asarray(v, dtype=np.float64)
    norm = float(np.linalg.norm(v))
    if not norm > 0:
        raise BuildError("an embedding has no length")
    return v / norm


# ---------------------------------------------------------------------------------------------
# Embedders

class FakeEmbedder:
    """Tests only (§2.13): every audio window and text maps to a unit vector derived from its SHA-256."""

    name = "fake"

    def __init__(self, model_dir=None):
        _numpy()

    @staticmethod
    def _vector(digest):
        rng = np.random.default_rng(int.from_bytes(digest, "little"))
        return unit(rng.standard_normal(FAKE_DIM))

    def audio(self, window):
        return self._vector(hashlib.sha256(np.ascontiguousarray(window, dtype="<f4").tobytes()).digest())

    def text(self, text):
        return self._vector(hashlib.sha256(text.encode("utf-8")).digest())

    def packages(self):
        import scipy
        return {"numpy": np.__version__, "scipy": scipy.__version__, "soundfile": sf.__version__}


class ClapEmbedder:
    """The pinned CLAP model from its verified local files: the weights through torch.load(weights_only=True)
    after their SHA-256 matched, into ClapModel(ClapConfig.from_json_file(...)) with strict=True; never
    from_pretrained on the model, never remote code."""

    name = "clap"

    def __init__(self, model_dir, weights, weights_sha256):
        _numpy()
        import torch
        from transformers import ClapConfig, ClapFeatureExtractor, ClapModel, RobertaTokenizerFast
        self.torch = torch
        torch.set_num_threads(1)
        torch.use_deterministic_algorithms(True)
        d = Path(model_dir)
        wpath = d / weights
        if file_sha256(wpath) != weights_sha256:
            raise BuildError(f"{wpath}: SHA-256 differs from the selection's pin; run semantic-fetch again")
        state = torch.load(str(wpath), map_location="cpu", weights_only=True)
        model = ClapModel(ClapConfig.from_json_file(str(d / "config.json")))
        model.load_state_dict(state, strict=True)
        model.eval()
        self.model = model
        self.features = ClapFeatureExtractor.from_json_file(str(d / "preprocessor_config.json"))
        if int(self.features.sampling_rate) != RATE or int(self.features.nb_max_samples) != WINDOW:
            raise BuildError(f"the feature extractor expects {self.features.sampling_rate} Hz and "
                             f"{self.features.nb_max_samples} samples, not {RATE} and {WINDOW}")
        self.tokenizer = RobertaTokenizerFast(vocab_file=str(d / "vocab.json"), merges_file=str(d / "merges.txt"),
                                              tokenizer_file=str(d / "tokenizer.json"),
                                              **_tokenizer_options(d))

    def audio(self, window):
        if len(window) > WINDOW:  # the extractor's random truncation is never reached
            raise BuildError(f"an audio window of {len(window)} samples is over {WINDOW}")
        np.random.seed(0)  # the extractor draws for fused models even when nothing is truncated
        inputs = self.features(window, sampling_rate=RATE, return_tensors="pt")
        with self.torch.no_grad():
            out = self.model.get_audio_features(**inputs)
        return unit(_tensor(out).double().numpy()[0])

    def text(self, text):
        inputs = self.tokenizer([text], return_tensors="pt")
        with self.torch.no_grad():
            out = self.model.get_text_features(input_ids=inputs["input_ids"], attention_mask=inputs["attention_mask"])
        return unit(_tensor(out).double().numpy()[0])

    def packages(self):
        import scipy
        import transformers
        return {"numpy": np.__version__, "scipy": scipy.__version__, "soundfile": sf.__version__,
                "torch": self.torch.__version__, "transformers": transformers.__version__}


def _tensor(out):
    """get_*_features returns a tensor (or, in some versions, an output object holding the pooled embeds)."""
    if hasattr(out, "detach"):
        return out.detach()
    for key in ("audio_embeds", "text_embeds", "pooler_output"):
        v = getattr(out, key, None)
        if v is not None:
            return v.detach()
    raise BuildError(f"unexpected model output {type(out).__name__}")


def _tokenizer_options(d):
    """The special tokens and model_max_length of the pinned tokenizer_config.json / special_tokens_map.json."""
    opts = {}
    for name in ("special_tokens_map.json", "tokenizer_config.json"):
        cfg = json.loads((d / name).read_text(encoding="utf-8"))
        for key in ("bos_token", "eos_token", "unk_token", "sep_token", "cls_token", "pad_token", "mask_token"):
            v = cfg.get(key)
            if isinstance(v, dict):
                v = v.get("content")
            if isinstance(v, str):
                opts[key] = v
        if isinstance(cfg.get("model_max_length"), int):
            opts["model_max_length"] = cfg["model_max_length"]
    return opts


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------------------------------------
# The stage

def clip_set(sel, work):
    """Every selection clip with an encoded work file: (id, type, description, audio bytes)."""
    out = []
    for cid in sorted(sel.clips):
        p = bl.clip_file(work, cid, ".ogg")
        if p.is_file() and not p.is_symlink():
            clip, _ = sel.clips[cid]
            out.append((cid, clip["type"], clip["description"], p.read_bytes()))
    if not out:
        raise BuildError(f"no encoded clips in {work}: run prepare first")
    return out


def reference_set(sel, work):
    """The calibration's reference clips: the selection's encoded Kenney clips the Python filters accept, plus
    the 40 essentials (their committed bytes, each checked against the essentials manifest)."""
    refs = []
    for cid in sorted(sel.clips):
        clip, src = sel.clips[cid]
        p, j = bl.clip_file(work, cid, ".ogg"), bl.clip_file(work, cid, ".json")
        if src["source"] == "kenney" and p.is_file() and j.is_file():
            if json.loads(j.read_text(encoding="utf-8")).get("verdict") == "accepted":
                refs.append((cid, p.read_bytes()))
    manifest = json.loads((ESSENTIALS / "manifest.json").read_text(encoding="utf-8"))
    for c in manifest["clips"]:
        data = (ESSENTIALS / c["path"]).read_bytes()
        if sha256(data) != c["sha256"]:
            raise BuildError(f"essentials:{c['id']} does not match the essentials manifest")
        refs.append(("essentials:" + c["id"], data))
    return refs


def embed_audio(embedder, data):
    return unit(np.mean([embedder.audio(w) for w in windows(decode_mono48(data))], axis=0))


def run_stage(sel, work, embedder, vocab, log=print):
    """The header and the clip lines, plus every clip's full z per text (for --check)."""
    clips = clip_set(sel, work)
    refs = reference_set(sel, work)
    log(f"semantic: {len(clips)} clips, {len(refs)} reference clips, {len(vocab.phrases)} phrases, embedder {embedder.name}")
    texts = list(vocab.phrases)
    for _, typ, desc, _ in clips:
        t = vocab.types[typ]
        for q in [desc, t["query"], *t["confusions"]]:
            if q not in texts:
                texts.append(q)
    index = {q: i for i, q in enumerate(texts)}
    T = np.array([unit(np.mean([embedder.text(tpl.format(q)) for tpl in vocab.templates], axis=0)) for q in texts])
    R = np.array([embed_audio(embedder, data) for _, data in refs])
    bias = (R @ T.T).mean(axis=0)
    nv = len(vocab.phrases)
    lines, full = [], {}
    for n, (cid, typ, desc, data) in enumerate(clips, 1):
        a = embed_audio(embedder, data)
        s = T @ a - bias
        mu, sd = float(s[:nv].mean()), float(s[:nv].std())
        if not sd > 0:
            raise BuildError(f"{cid}: the vocabulary's scores do not vary; no z can be computed")
        z = (s - mu) / sd
        t = vocab.types[typ]
        z_label = max(z[index[desc]], z[index[t["query"]]])
        conf = max(t["confusions"], key=lambda c: (z[index[c]], -t["confusions"].index(c)))
        margin = z_label - z[index[conf]]
        verdict = "ok" if z_label >= LABEL_OK_Z and margin > 0 else "weak" if z_label >= LABEL_WEAK_Z and margin > 0 else "review"
        order = sorted(range(nv), key=lambda i: (-z[i], i))
        heard = [{"tag": vocab.phrases[i], "z": rnd(z[i])} for i in order[:HEARD_MAX] if z[i] >= HEARD_Z]
        lines.append({"line": "clip", "id": cid, "type": typ, "description": desc, "audio_sha256": sha256(data),
                      "label": {"verdict": verdict, "z": rnd(z_label), "margin": rnd(margin), "confusion": conf}, "heard": heard})
        full[cid] = {q: float(z[i]) for q, i in index.items()}
        if n % 50 == 0:
            log(f"semantic: {n} of {len(clips)} clips")
    ids = [r[0] for r in refs]
    header = {"line": "stage", "embedder": embedder.name, "model": sel.semantic_pins(), "packages": embedder.packages(),
              "vocab_sha256": vocab.sha256, "reference": {"count": len(ids), "ids_sha256": sha256(("\n".join(ids) + "\n").encode())},
              "limits": {"label_ok_z": LABEL_OK_Z, "label_weak_z": LABEL_WEAK_Z, "heard_z": HEARD_Z, "heard_max": HEARD_MAX},
              "threads": 1, "windows": {"samples": WINDOW, "hop": HOP, "rate": RATE}}
    return header, lines, full


def file_text(header, lines):
    return "".join(dump(x) + "\n" for x in [header] + sorted(lines, key=lambda r: r["id"]))


def compare(old_text, header, lines, full):
    """semantic --check: the differences that matter between the committed file and a re-run (empty: agrees)."""
    problems, notes = [], []
    rows = [json.loads(x) for x in old_text.splitlines()]
    if not rows or rows[0].get("line") != "stage":
        return ["the file has no stage header"], notes
    old_h, old = rows[0], {r["id"]: r for r in rows[1:]}
    for key in ("embedder", "model", "vocab_sha256", "reference", "limits", "threads", "windows"):
        if old_h.get(key) != header[key]:
            problems.append(f"header {key}: {old_h.get(key)} in the file, {header[key]} now")
    if old_h.get("packages") != header["packages"]:
        notes.append(f"packages: {old_h.get('packages')} in the file, {header['packages']} now")
    new = {r["id"]: r for r in lines}
    for cid in sorted(set(old) ^ set(new)):
        problems.append(f"{cid}: {'only in the file' if cid in old else 'not in the file'}")
    for cid in sorted(set(old) & set(new)):
        o, n = old[cid], new[cid]
        for key in ("type", "description", "audio_sha256"):
            if o.get(key) != n[key]:
                problems.append(f"{cid}: {key} {o.get(key)!r} in the file, {n[key]!r} now")
        ol, nl = o.get("label") or {}, n["label"]
        for key in ("z", "margin"):
            if not isinstance(ol.get(key), (int, float)) or abs(ol[key] - nl[key]) > CHECK_Z:
                problems.append(f"{cid}: label {key} {ol.get(key)} in the file, {nl[key]} now")
        if ol.get("verdict") != nl["verdict"]:
            near = any(abs(v - lim) <= CHECK_Z for v in (ol.get("z", 0), nl["z"]) for lim in (LABEL_OK_Z, LABEL_WEAK_Z)) \
                or any(abs(v) <= CHECK_Z for v in (ol.get("margin", 1), nl["margin"]))
            (notes if near else problems).append(f"{cid}: verdict {ol.get('verdict')} in the file, {nl['verdict']} now"
                                                 + (" (within the tolerance of a limit)" if near else ""))
        oh = {t["tag"]: t["z"] for t in o.get("heard") or []}
        nh = {t["tag"]: t["z"] for t in n["heard"]}
        cut = min(nh.values()) if len(nh) == HEARD_MAX else HEARD_Z
        for tag in sorted(set(oh) | set(nh)):
            z_now = full[cid].get(tag)
            if tag in oh and tag in nh:
                if abs(oh[tag] - nh[tag]) > CHECK_Z:
                    problems.append(f"{cid}: heard {tag!r} z {oh[tag]} in the file, {nh[tag]} now")
            elif z_now is not None and (abs(z_now - HEARD_Z) <= CHECK_Z or abs(z_now - cut) <= CHECK_Z
                                        or tag in oh and abs(oh[tag] - z_now) <= CHECK_Z and abs(oh[tag] - cut) <= CHECK_Z):
                notes.append(f"{cid}: heard {tag!r} {'dropped' if tag in oh else 'added'} at z {rnd(z_now)}, within the tolerance of a limit")
            else:
                problems.append(f"{cid}: heard {tag!r} {'in the file only' if tag in oh else 'only now'} (z {None if z_now is None else rnd(z_now)})")
    return problems, notes


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--selection", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--cache", help="the pipeline's cache; the model files are in <cache>/clap/<revision>/")
    ap.add_argument("--out", required=True, help="the semantic file (library-semantic.jsonl)")
    ap.add_argument("--vocab", default=str(DEFAULT_VOCAB))
    ap.add_argument("--embedder", choices=("clap", "fake"), default="clap")
    ap.add_argument("--check", action="store_true", help="re-run and compare with --out instead of writing it")
    args = ap.parse_args(argv)
    try:
        sel = bl.Selection(args.selection)
        if not sel.semantic:
            raise BuildError(f"{sel.name} has no semantic block (the model pins)")
        vocab = Vocab(args.vocab)
        if args.embedder == "fake":
            embedder = FakeEmbedder()
        else:
            if not args.cache:
                raise BuildError("--cache is needed for the model files")
            m = sel.semantic["model"]
            pins = sel.semantic_pins()
            model_dir = Path(args.cache) / "clap" / m["revision"]
            for f in m["files"]:
                if not (model_dir / f["name"]).is_file():
                    raise BuildError(f"{model_dir / f['name']} is missing: run build_library.py semantic-fetch first")
            embedder = ClapEmbedder(model_dir, m["weights"], pins["sha256"])
        header, lines, full = run_stage(sel, Path(args.work), embedder, vocab)
        text = file_text(header, lines)
        out = Path(args.out)
        if args.check:
            if not out.is_file():
                raise BuildError(f"{out} is missing: nothing to check against")
            problems, notes = compare(out.read_text(encoding="utf-8"), header, lines, full)
            for n in notes:
                print(f"semantic --check: note: {n}")
            if problems:
                raise BuildError(f"{out} disagrees with a re-run:\n  " + "\n  ".join(problems))
            print(f"semantic --check: {len(lines)} clips agree with {out} (z within {CHECK_Z})")
            return 0
        bl.write_if_changed(out, text.encode("utf-8"))
        counts = {}
        for r in lines:
            counts[r["label"]["verdict"]] = counts.get(r["label"]["verdict"], 0) + 1
        print(f"semantic: wrote {out}: {len(lines)} clips, " + ", ".join(f"{k} {counts.get(k, 0)}" for k in ("ok", "weak", "review")))
        return 0
    except BuildError as e:
        print(f"semantic_stage.py: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    os.environ.setdefault("HF_HUB_OFFLINE", "1")
    os.environ.setdefault("TRANSFORMERS_OFFLINE", "1")
    sys.exit(main())
