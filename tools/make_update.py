#!/usr/bin/env python3
"""Build the delta-update artifacts for the in-app smart updater (see src/app/Updater.h).

Given a staged release tree (the same folder tools/package_release.ps1 zips: one folder
MMDX12-<ver>-win64/ holding MMDX12.exe, DLLs, shaders/, assets/, licenses/), this writes:

    <out>/manifests/<ver>.json        the per-file manifest (format 1)
    <out>/objects/<aa>/<sha>          raw file bytes, content-addressed by SHA-256
    <out>/objects/<aa>/<sha>.gz       the same bytes gzip -9 (for nginx gzip_static)

and prints the "manifest" / "manifestSha256" values of the update feed, which holds them next to the zip fields and
is signed afterwards (stable: the website repo's src/data/update_manifests.json + its sign-feed workflow). Objects are shared
across versions: existing objects are left alone.

The install folder is the *content* of the top folder: `path` in the manifest is relative to
that folder (no leading MMDX12-<ver>-win64/). library/ and runtime leftovers are skipped.

Usage:
    python tools/make_update.py <stage dir> --version 1.9.2 --out dist/update [--base-url URL]

`--base-url` is the URL the manifests/ and objects/ folders will be served under (e.g.
https://mmdx.codingbot.kr/update). The manifest's `objects` field points at <base-url>/objects/;
without it the manifest uses the relative "../objects/" (resolved against the manifest URL).

Nightly builds (.github/workflows/nightly.yml) also pass:
    --known-feed URL    objects the manifest of that live feed lists are already on the server: <out> leaves them out
    --feed FILE --zip Z --zip-url U --page P --commit SHA --note TEXT
                        write the update feed (what the app reads) for this version
Standard library only (python 3).
"""
import argparse
import gzip
import hashlib
import json
import os
import shutil
import sys
import time
import urllib.error
import urllib.request

# Directory names anywhere under the install root that never belong to a version: the user's
# library and caches/logs the app writes at runtime.
SKIP_DIRS = {"library", "shader_cache", "cache", "update_tmp", "recovery"}
# Runtime leftovers at the install root (a user's settings); the release tree has neither.
SKIP_FILES = {"mmdx12.ini"}

CHUNK = 1 << 20


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(CHUNK)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def find_install_root(stage):
    """The stage dir is either the install root itself or its parent (holding MMDX12.exe)."""
    if os.path.isfile(os.path.join(stage, "MMDX12.exe")):
        return stage
    for name in sorted(os.listdir(stage)):
        sub = os.path.join(stage, name)
        if os.path.isdir(sub) and os.path.isfile(os.path.join(sub, "MMDX12.exe")):
            return sub
    sys.exit(f"error: no MMDX12.exe found under {stage}")


def skipped(path, root):
    rel = os.path.relpath(path, root).replace(os.sep, "/")
    parts = rel.split("/")
    if any(p in SKIP_DIRS for p in parts[:-1]):
        return True
    name = parts[-1]
    if len(parts) == 1 and name.lower() in SKIP_FILES:
        return True
    if name.lower().endswith(".log"):
        return True
    return False


def fetch_json(url):
    # The site sits behind a CDN that turns away urllib's default User-Agent.
    request = urllib.request.Request(url, headers={"User-Agent": "MMDX12-make-update"})
    with urllib.request.urlopen(request, timeout=60) as r:
        return json.load(r)


def known_objects(feed_url):
    """SHA-256 of every file the live feed's manifest lists; empty when there is no feed yet."""
    try:
        return {f["sha256"] for f in fetch_json(fetch_json(feed_url)["manifest"])["files"]}
    except urllib.error.HTTPError as e:
        if e.code != 404:
            raise
        print(f"no feed at {feed_url}: writing every object")
        return set()


def main():
    ap = argparse.ArgumentParser(description="Build the delta-update manifest + object store.")
    ap.add_argument("stage", help="staged release tree (or its parent); MMDX12.exe inside")
    ap.add_argument("--version", required=True, help="version string, e.g. 1.9.2")
    ap.add_argument("--out", required=True, help="output dir for manifests/ and objects/")
    ap.add_argument("--base-url", default="", help="URL the manifests/ and objects/ live under")
    ap.add_argument("--known-feed", default="", help="live feed whose manifest's objects need no upload")
    ap.add_argument("--feed", default="", help="write the update feed to this file (needs --base-url)")
    ap.add_argument("--zip", default="", help="the version's full zip (feed size + sha256)")
    ap.add_argument("--zip-url", default="", help="where the zip will be served (feed url)")
    ap.add_argument("--page", default="", help="release page (feed page)")
    ap.add_argument("--commit", default="", help="commit this build came from (feed commit)")
    ap.add_argument("--note", default="", help="update note (feed notes.en)")
    args = ap.parse_args()
    if args.feed and not (args.base_url and args.zip and args.zip_url):
        sys.exit("error: --feed needs --base-url, --zip and --zip-url")

    known = known_objects(args.known_feed) if args.known_feed else set()
    root = find_install_root(args.stage)
    manifests = os.path.join(args.out, "manifests")
    objects = os.path.join(args.out, "objects")
    os.makedirs(manifests, exist_ok=True)
    os.makedirs(objects, exist_ok=True)

    base = args.base_url.rstrip("/")
    objects_field = (base + "/objects/") if base else "../objects/"

    files = []
    new_objects = 0
    total = 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in sorted(filenames):
            full = os.path.join(dirpath, name)
            if skipped(full, root):
                continue
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            digest = sha256_of(full)
            size = os.path.getsize(full)
            files.append({"path": rel, "size": size, "sha256": digest})
            total += size

            dest = os.path.join(objects, digest[:2], digest)
            if digest not in known and not os.path.exists(dest):
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                shutil.copyfile(full, dest)
                new_objects += 1
                gz = dest + ".gz"
                with open(full, "rb") as src, gzip.open(gz, "wb", compresslevel=9) as out:
                    shutil.copyfileobj(src, out, CHUNK)
                if os.path.getsize(gz) >= size:  # already compressed (png, jpg): serve it raw
                    os.remove(gz)

    files.sort(key=lambda f: f["path"])
    manifest = {
        "format": 1,
        "version": args.version,
        "objects": objects_field,
        "files": files,
    }
    manifest_path = os.path.join(manifests, args.version + ".json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=1)
        f.write("\n")

    manifest_sha = sha256_of(manifest_path)
    if base:
        manifest_url = base + "/manifests/" + args.version + ".json"
    else:
        manifest_url = "manifests/" + args.version + ".json"

    print(f"manifest: {len(files)} files, {total / 1048576:.1f} MB "
          f"({new_objects} new objects, {len(files) - new_objects} reused)")
    print(f"wrote {manifest_path}")
    print()
    print("update feed fields:")
    print(f'  "manifest": "{manifest_url}",')
    print(f'  "manifestSha256": "{manifest_sha}",')

    if args.feed:
        feed = {
            "version": args.version,
            "url": args.zip_url,
            "size": os.path.getsize(args.zip),
            "sha256": sha256_of(args.zip),
            "page": args.page,
            "published": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "commit": args.commit,
            "manifest": manifest_url,
            "manifestSha256": manifest_sha,
            "notes": {"en": args.note},
        }
        with open(args.feed, "w", encoding="utf-8") as f:
            json.dump(feed, f, ensure_ascii=False, indent=1)
            f.write("\n")
        print(f"wrote {args.feed}")


if __name__ == "__main__":
    main()
