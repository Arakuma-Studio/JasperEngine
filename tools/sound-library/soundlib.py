"""Helpers shared by build_essentials.py and build_library.py (standard library only).

Everything downloaded is untrusted data. Downloads go only to an explicit host
allow-list over HTTPS (redirects and the final URL are checked against the same
list) and are held in memory until their pins are checked; pinned zips are
re-hashed before they are opened and every member path is screened.
"""

import hashlib
import re
import json
import re
import ssl
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

LICENCE_MARKERS = ("Creative Commons Zero", "publicdomain/zero/1.0")
MAX_LICENCE_BYTES = 64 * 1024


class BuildError(Exception):
    pass


def sha256(data):
    return hashlib.sha256(data).hexdigest()


CC0_URL = "http://creativecommons.org/publicdomain/zero/1.0/"
LABEL_MAX = 32
IMPORTER_FADES = ("fade_in_ms", "fade_out_ms")
# the manifest versions with a per-clip `meta`, and the set an import records (provenance.builtin)
META_VERSIONS = {"jasperengine-sound-essentials": (2, "essentials"), "jasperengine-sound-library": (4, "library")}


def clip_labels(tags):
    """The clip's tags as S25 labels: lowercase [a-z0-9_], at most LABEL_MAX characters, each once, in order."""
    labels = []
    for tag in tags:
        label = re.sub(r"[^a-z0-9_]+", "_", str(tag).lower()).strip("_")[:LABEL_MAX]
        if label and label not in labels:
            labels.append(label)
    return labels[:LABEL_MAX]


def clip_meta(clip, importer, builtin_set):
    """Exactly the S04 .meta sections an import of the clip writes (S36 R16, R17)."""
    audio_importer = {"version": 1, "type": clip["type"]}
    for key in IMPORTER_FADES:
        if importer and key in importer:
            audio_importer[key] = importer[key]
    author = clip.get("author")
    provenance = {"version": 1, "licence": clip["licence"], "licence_url": clip.get("licence_url") or CC0_URL}
    if author:
        provenance["author"] = author
    provenance["attribution"] = clip.get("attribution") or author
    provenance["source"] = clip["source"]
    provenance["url"] = clip["url"]
    provenance["builtin"] = {"set": builtin_set, "id": clip["id"], "sha256": clip["sha256"]}
    return {"import": {"audioImporter": audio_importer}, "provenance": provenance, "labels": clip_labels(clip["tags"])}


def migrate_manifest(manifest, excluded=()):
    """The manifest with a per-clip `meta` in place of `import` (library v3 -> v4, essentials v1 -> v2),
    the excluded ids and any pack left without a clip dropped. Offline and deterministic: a function of
    its input alone."""
    version, builtin_set = META_VERSIONS[manifest["format"]]
    clips = []
    for clip in manifest["clips"]:
        if clip["id"] in excluded:
            continue
        migrated = {}
        for key, value in clip.items():
            if key == "import":
                continue
            if key == "semantic":
                migrated["meta"] = clip_meta(clip, clip.get("import"), builtin_set)
            migrated[key] = value
        if "meta" not in migrated:
            migrated["meta"] = clip_meta(clip, clip.get("import"), builtin_set)
        clips.append(migrated)
    used = {c.get("pack") for c in clips}
    packs = [p for p in manifest["packs"] if p["id"] in used]
    out = dict(manifest)
    out["version"] = version
    out["packs"] = packs
    out["clips"] = clips
    return out


def manifest_text(manifest):
    """manifest.json bytes: indent 1, keys in insertion order, LF, final newline."""
    return json.dumps(manifest, indent=1, ensure_ascii=False) + "\n"


def tree_files(out):
    """Every file (and symlink) under out, as POSIX paths relative to it."""
    return {p.relative_to(out).as_posix() for p in Path(out).rglob("*") if p.is_file() or p.is_symlink()}


# download

def host_allowed(url, hosts):
    """https to one of hosts; an entry starting with "." allows the hosts that end in it (".hf.co": "cdn.hf.co")."""
    u = urllib.parse.urlsplit(url)
    h = u.hostname
    return u.scheme == "https" and h is not None and any(h == x or (x.startswith(".") and h.endswith(x)) for x in hosts)


def _hosts_text(hosts):
    return ", ".join(f"https://*{h}" if h.startswith(".") else f"https://{h}" for h in hosts)


class AllowListRedirects(urllib.request.HTTPRedirectHandler):
    """Follows a redirect only when it is https to one of the allowed hosts."""

    def __init__(self, hosts):
        super().__init__()
        self.hosts = tuple(hosts)

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not host_allowed(newurl, self.hosts):
            if len(self.hosts) == 1:
                raise BuildError(f"refusing redirect to {newurl}: only https://{self.hosts[0]} is allowed")
            raise BuildError(f"refusing redirect to {newurl}: only {_hosts_text(self.hosts)} are allowed")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def download(url, limit, hosts, user_agent, timeout=60):
    """GET url into memory (at most limit bytes); https and the allowed hosts only."""
    hosts = tuple(hosts)
    if not host_allowed(url, hosts):
        raise BuildError(f"refusing to fetch {url}: only {_hosts_text(hosts)} allowed")
    context = ssl.create_default_context()  # verifies certificates and host names
    opener = urllib.request.build_opener(urllib.request.HTTPSHandler(context=context), AllowListRedirects(hosts))
    req = urllib.request.Request(url, headers={"User-Agent": user_agent})
    with opener.open(req, timeout=timeout) as resp:
        if not host_allowed(resp.geturl(), hosts):
            off = hosts[0] if len(hosts) == 1 else "the allowed hosts"
            raise BuildError(f"download ended at {resp.geturl()}, off {off}")
        chunks, total = [], 0
        while True:
            chunk = resp.read(65536)
            if not chunk:
                break
            total += len(chunk)
            if total > limit:
                raise BuildError(f"{url}: larger than {limit} bytes")
            chunks.append(chunk)
    return b"".join(chunks)


def download_to(url, dest, limit, hosts, user_agent, timeout=60):
    """GET url into the file dest (at most limit bytes), streamed; https and the allowed hosts only. Returns
    (bytes, SHA-256). The caller checks the pins before it renames dest into place."""
    hosts = tuple(hosts)
    if not host_allowed(url, hosts):
        raise BuildError(f"refusing to fetch {url}: only {_hosts_text(hosts)} allowed")
    context = ssl.create_default_context()
    opener = urllib.request.build_opener(urllib.request.HTTPSHandler(context=context), AllowListRedirects(hosts))
    req = urllib.request.Request(url, headers={"User-Agent": user_agent})
    h, total = hashlib.sha256(), 0
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    with opener.open(req, timeout=timeout) as resp, open(dest, "wb") as out:
        if not host_allowed(resp.geturl(), hosts):
            raise BuildError(f"download ended at {resp.geturl()}, off the allowed hosts")
        while True:
            chunk = resp.read(1 << 20)
            if not chunk:
                break
            total += len(chunk)
            if total > limit:
                raise BuildError(f"{url}: larger than {limit} bytes")
            h.update(chunk)
            out.write(chunk)
    return total, h.hexdigest()


class PoliteFetcher:
    """download() with a minimum interval between requests, retries with backoff and an offline switch.

    Retries: HTTP 429 and 5xx, connection errors and timeouts, at most `retries` times, waiting
    Retry-After when the server sends one (capped at max_wait), else backoff * 2^attempt seconds.
    Every other HTTP error (404, 403, ...) is returned to the caller at once.
    """

    def __init__(self, hosts, user_agent, min_interval=1.0, retries=3, backoff=2.0, max_wait=120.0,
                 offline=False, sleep=time.sleep, clock=time.monotonic, opener=None, file_opener=None):
        self.hosts = tuple(hosts)
        self.user_agent = user_agent
        self.min_interval = min_interval
        self.retries = retries
        self.backoff = backoff
        self.max_wait = max_wait
        self.offline = offline
        self.sleep = sleep
        self.clock = clock
        self.download = opener or download
        self.download_to = file_opener or download_to
        self.requests = 0
        self._last = None

    def _wait_turn(self):
        if self._last is not None:
            left = self.min_interval - (self.clock() - self._last)
            if left > 0:
                self.sleep(left)
        self._last = self.clock()

    def get(self, url, limit, timeout=60):
        return self._fetch(url, lambda: self.download(url, limit, self.hosts, self.user_agent, timeout))

    def get_to(self, url, dest, limit, timeout=60):
        """download_to() with the same interval, retries and offline switch: (bytes, SHA-256)."""
        return self._fetch(url, lambda: self.download_to(url, dest, limit, self.hosts, self.user_agent, timeout))

    def _fetch(self, url, call):
        if self.offline:
            raise BuildError(f"--offline: refusing to fetch {url} (it is not in the cache)")
        attempt = 0
        while True:
            self._wait_turn()
            self.requests += 1
            try:
                return call()
            except urllib.error.HTTPError as e:
                if e.code != 429 and e.code < 500 or attempt >= self.retries:
                    raise
                wait = self._retry_after(e.headers.get("Retry-After") if e.headers else None, attempt)
            except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
                if isinstance(e, urllib.error.URLError) and isinstance(e.reason, BuildError):
                    raise e.reason
                if attempt >= self.retries:
                    raise
                wait = self.backoff * 2 ** attempt
            attempt += 1
            self.sleep(min(wait, self.max_wait))

    def _retry_after(self, value, attempt):
        if value and value.strip().isdigit():
            return float(value.strip())
        return self.backoff * 2 ** attempt


# pinned zips

def open_pinned_zip(path, pack, selection_name):
    """Open a cached zip after re-checking its size and SHA-256; refuse unsafe member paths."""
    path = Path(path)
    if not path.is_file():
        raise BuildError(f"{path} is missing: run fetch first")
    data = path.read_bytes()
    if len(data) != pack["bytes"] or sha256(data) != pack["sha256"]:
        raise BuildError(f"{path}: size or SHA-256 differs from {selection_name}; delete it and fetch again")
    zf = zipfile.ZipFile(path)
    for info in zf.infolist():
        name = info.filename
        mode = (info.external_attr >> 16) & 0o170000
        if name.startswith("/") or "\\" in name or ".." in name.split("/") or re.match(r"^[A-Za-z]:", name):
            raise BuildError(f"{path.name}: refusing archive with unsafe member path {name!r}")
        if mode == 0o120000:
            raise BuildError(f"{path.name}: refusing archive with symlink member {name!r}")
    return zf


def read_member(zf, name, limit):
    """One listed member's bytes: exactly one entry, capped by its header and by the bytes read."""
    infos = [i for i in zf.infolist() if i.filename == name]
    if len(infos) != 1:
        raise BuildError(f"member {name!r} found {len(infos)} times, expected once")
    info = infos[0]
    if info.is_dir():
        raise BuildError(f"member {name!r} is a directory")
    if info.file_size > limit:
        raise BuildError(f"member {name!r} is {info.file_size} bytes, over the {limit}-byte cap")
    with zf.open(info) as f:
        data = f.read(limit + 1)
    if len(data) > limit or len(data) != info.file_size:
        raise BuildError(f"member {name!r}: decompressed size disagrees with its header")
    return data


def read_licence(zf, pack_id):
    """The pack's License.txt bytes; it must hold both CC0 markers."""
    data = read_member(zf, "License.txt", MAX_LICENCE_BYTES)
    text = data.decode("utf-8", errors="replace")
    for marker in LICENCE_MARKERS:
        if marker not in text:
            raise BuildError(f"{pack_id}: License.txt does not contain {marker!r}")
    return data
