"""Firmware keys from The Apple Wiki, cached as JSON beside the IPSW.

The project ships no keys. This fetches the build's key page (Keys:<Codename>_<Build>_(<Device>))
and parses its "Filename / IV / Key" entries, or loads a JSON you supply:
  {"kernelcache": {"file": "kernelcache.release.n94", "iv": "...", "key": "..."},
   "rootfs": {"file": "048-2613-005.dmg", "key": "<72 hex>"}, ...}
Component names are the wiki's with spaces removed; the root filesystem is "rootfs".
"""
import html
import json
import os
import re
import urllib.parse
import urllib.request

WIKI = "https://theapplewiki.com"
UA = "Mozilla/5.0 iPhonEmu-ios-prep (+https://github.com/kazin1898/iPhonEmu)"
# Builds whose codename is known without a search.
CODENAMES = {("10B329", "iPhone4,1"): "BrightonMaps"}
ALIASES = {"RootFilesystem": "rootfs", "RootFS": "rootfs", "Kernelcache": "kernelcache",
           "UpdateRamdisk": "UpdateRamDisk", "RestoreRamdisk": "RestoreRamDisk"}


def _get(url):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read().decode("utf-8", "replace")


def page_title(build, device):
    code = CODENAMES.get((build, device))
    if code:
        return f"Keys:{code}_{build}_({device})"
    q = urllib.parse.urlencode({"action": "query", "list": "search", "format": "json",
                                "srsearch": f"intitle:Keys: {build} {device}", "srlimit": 5})
    res = json.loads(_get(f"{WIKI}/api.php?{q}"))
    for hit in res.get("query", {}).get("search", []):
        t = hit["title"]
        if build in t and device in t:
            return t.replace(" ", "_")
    raise LookupError(f"no key page found for {build} {device}")


def parse_page(text):
    body = re.sub(r"<(script|style)[^>]*>.*?</\1>", "", text, flags=re.S)
    body = re.sub(r"<br\s*/?>", "\n", body)
    body = re.sub(r"</(p|div|tr|li|h\d|table|span)>", "\n", body)
    body = html.unescape(re.sub(r"<[^>]+>", " ", body))
    body = re.sub(r"[ \t]+", " ", body)
    keys = {}
    pat = re.compile(r"\n\s*([A-Za-z0-9 ]+?)\s*\n\s*Filename:\s*(\S+)\s*"
                     r"(?:IV\s*:\s*([0-9a-fA-F]{32}))?\s*Key\s*:\s*([0-9a-fA-F]+)", re.S)
    for comp, fname, iv, key in pat.findall(body):
        name = comp.strip().replace(" ", "")
        name = ALIASES.get(name, name)
        entry = {"file": fname, "key": key.lower()}
        if iv:
            entry["iv"] = iv.lower()
        keys[name] = entry
    if "kernelcache" not in keys:
        raise ValueError("key page parsed, but no Kernelcache entry found")
    return keys


def fetch(build, device):
    title = page_title(build, device)
    text = _get(f"{WIKI}/wiki/{urllib.parse.quote(title)}")
    keys = parse_page(text)
    keys["_source"] = f"{WIKI}/wiki/{title}"
    return keys


def load(build, device, cache_dir, path=None):
    """Keys for the build: from `path`, else the cache in cache_dir, else the wiki (then cached)."""
    if path:
        with open(path) as f:
            return json.load(f)
    cache = os.path.join(cache_dir, f"keys_{build}_{device}.json")
    if os.path.exists(cache):
        with open(cache) as f:
            return json.load(f)
    keys = fetch(build, device)
    os.makedirs(cache_dir, exist_ok=True)
    with open(cache, "w") as f:
        json.dump(keys, f, indent=2)
    return keys
