#!/usr/bin/env python3
"""Headless end-to-end test for optional components (Components.h, P0.3).

The lite release zip lacks the upscaler DLLs (nvngx_dlss.dll, libxess.dll,
amd_fidelityfx_*.dll) and the Noto CJK fonts; the app fetches them on demand from the update
object store, using the manifest of the INSTALLED version (never a newer one). This test:

  * builds a mock install from a build dir (EXE + one optional component), deletes that component;
  * builds a stage tree = the install's program files + the component files, runs
    tools/make_update.py on it into a local object store (the manifest is the installed version);
  * signs a local feed whose `manifest` points at that manifest and whose `manifests` holds its hash;
  * runs the install's exe with `--update-feed <feed> --component upscaler:dlss --download-component`
    (headless: this happens before the window is created), once, under a timeout;
  * asserts the deleted DLL is back and its bytes match the staged file;
  * a second run confirms an already-present component is fetched (idempotent), and an unknown id
    fails cleanly;
  * a server that lies is refused and installs nothing: an evil manifest, a changed object, an unsigned feed, a changed
    payload, a signed feed that does not name this version.

Run against a test build that trusts a throwaway key (the header of tools/update_e2e.py has the commands):
    python tools/components_e2e.py [--keep]

Other build dir / key: the environment variables MMDX12_BUILD and MMDX12_FEED_TEST_PEM. Needs tools/fetch_sdks.ps1 run
before that build. Standard library only (+ the openssl command, through tools/sign_feed.py).
"""
import argparse
import base64
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import sign_feed  # noqa: E402

TMP = os.path.join(os.environ.get("TEMP", os.environ.get("TMP", ".")), "opencode", "components_e2e")
BUILD = os.environ.get("MMDX12_BUILD", "build_feedtest")
BIN = os.path.join(ROOT, BUILD, "bin")
KEY = os.environ.get("MMDX12_FEED_TEST_PEM", os.path.join(ROOT, BUILD, "feed_test.pem"))
EXE_NAME = "MMDX12.exe"
MAKE_UPDATE = os.path.join(ROOT, "tools", "make_update.py")
COMPONENT = "upscaler:dlss"
COMPONENT_FILE = "nvngx_dlss.dll"
COMPONENT_FILES = [COMPONENT_FILE, "nvngx_dlssd.dll", "nvngx_dlssg.dll"]  # src/app/Components.h kDlssFiles
OTHER_FILES = ["dxcompiler.dll", "dxil.dll"]


def project_version():
    """The exe's MMDX12_VERSION == CMake project(... VERSION x.y.z); the install's manifest is
    '<current version>.json', so the mock must use the same version."""
    cmake = os.path.join(ROOT, "CMakeLists.txt")
    with open(cmake, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line.startswith("project(") and "VERSION" in line:
                parts = line.replace("(", " ").replace(")", " ").split()
                i = parts.index("VERSION")
                return parts[i + 1]
    sys.exit("could not read the project version from CMakeLists.txt")


VERSION = project_version()

failures = []


def log(msg):
    print(msg, flush=True)


def check(cond, msg):
    if cond:
        log(f"  OK   {msg}")
    else:
        log(f"  FAIL {msg}")
        failures.append(msg)
    return cond


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def force_dirs_ok(path):
    def onerror(func, p, exc):
        try:
            os.chmod(p, 0o700)
            func(p)
        except Exception:
            pass
    if os.path.exists(path):
        shutil.rmtree(path, onerror=onerror)


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)


def wait_no_app(timeout=300, step=10):
    """The plan requires one app window at a time: wait until no MMDX12.exe runs."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        out = run(["tasklist", "/FI", "IMAGENAME eq MMDX12.exe"], cwd=ROOT).stdout or ""
        if "MMDX12.exe" not in out:
            return True
        log("  another MMDX12.exe is running; waiting 10 s...")
        time.sleep(step)
    return False


def run_app(args, install, timeout=90):
    """One headless app run; the app quits by itself with --download-component."""
    exe = os.path.join(install, EXE_NAME)
    cmd = [exe] + args
    log("  launching: " + " ".join(cmd))
    try:
        proc = subprocess.run(cmd, cwd=install, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        check(False, "the app did not exit within the timeout")
        return None
    return proc


def read_log(install):
    p = os.path.join(install, "mmdx12.log")
    if not os.path.exists(p):
        return ""
    with open(p, encoding="utf-8", errors="replace") as f:
        return f.read()


def write_signed(feed, path, key=KEY):
    """The signed feed file: the plain fields + the signed bytes (src/core/FeedSignature.h)."""
    data = json.dumps(feed, ensure_ascii=False, indent=1).encode("utf-8")
    signed_feed, signature = sign_feed.sign_bytes(data, key)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(dict(feed, signedFeed=signed_feed, signature=signature), f, ensure_ascii=False, indent=1)
    return path


def refused(title, install, feed_path, expect_error):
    """A fetch that must fail with `expect_error` and leave the component missing."""
    log(f"\n== {title} ==")
    dst = os.path.join(install, COMPONENT_FILE)
    for name in COMPONENT_FILES:
        if os.path.exists(os.path.join(install, name)):
            os.remove(os.path.join(install, name))
    proc = run_app(["--update-feed", feed_path, "--component", COMPONENT, "--download-component"], install)
    logtext = read_log(install)
    check(proc is not None and proc.returncode != 0, "the app exited non-zero")
    check("COMPONENT ok=0" in logtext and expect_error in logtext, f"refused: {expect_error}")
    check(not os.path.exists(dst), f"{COMPONENT_FILE} was not installed")
    check(not os.path.exists(dst + ".component.part"), "no partial download left")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="do not delete the temp workspace")
    args = ap.parse_args()

    exe_src = os.path.join(BIN, EXE_NAME)
    if not os.path.exists(exe_src):
        sys.exit(f"{exe_src} missing - run build.cmd {BUILD} first")
    if not os.path.exists(MAKE_UPDATE):
        sys.exit(f"{MAKE_UPDATE} missing")
    if not os.path.exists(KEY):
        sys.exit(f"{KEY} missing: the header of tools/update_e2e.py has the test key commands")

    force_dirs_ok(TMP)
    os.makedirs(TMP, exist_ok=True)

    # 1. The stage tree: the install's files + the component files (the "full" tree). This is what
    #    make_update.py hashes; the object store is the source of truth for the component bytes.
    log("== building the stage tree + object store ==")
    stage = os.path.join(TMP, "stage")
    os.makedirs(stage)
    shutil.copyfile(exe_src, os.path.join(stage, EXE_NAME))
    copied = []
    for name in OTHER_FILES:
        src = os.path.join(BIN, name)
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(stage, name))
            copied.append(name)
    for name in COMPONENT_FILES + ["libxess.dll", "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll"]:
        src = os.path.join(BIN, name)
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(stage, name))
    # A minimal assets/fonts tree with the CJK OTFs so they are part of the manifest too.
    fonts_src = os.path.join(ROOT, "assets", "fonts")
    fonts_dst = os.path.join(stage, "assets", "fonts")
    os.makedirs(fonts_dst)
    for name in os.listdir(fonts_src):
        if name.startswith("NotoSansCJK") and name.endswith(".otf"):
            shutil.copyfile(os.path.join(fonts_src, name), os.path.join(fonts_dst, name))
    if not os.path.exists(os.path.join(stage, COMPONENT_FILE)):
        log(f"  WARNING: {COMPONENT_FILE} not in {BIN}; the DLSS fetch cannot be tested. "
            f"Run tools/fetch_sdks.ps1.")

    store = os.path.join(TMP, "store")
    res = run([sys.executable, MAKE_UPDATE, stage, "--version", VERSION, "--out", store])
    log(res.stdout.strip())
    if res.returncode != 0:
        sys.exit("make_update.py failed")
    manifest_path = os.path.join(store, "manifests", VERSION + ".json")
    manifest_sha = sha256_file(manifest_path)

    # The staged component bytes (must equal what lands in the install).
    stage_component = os.path.join(stage, COMPONENT_FILE)
    expect_sha = sha256_file(stage_component)
    log(f"  staged {COMPONENT_FILE}: sha256 {expect_sha[:12]}...")

    # 2. The feed. `manifest` points at the installed version's manifest; the component fetcher
    #    derives <current>.json as its sibling (== the same file here) and checks it against `manifests`.
    zip_path = os.path.join(TMP, f"MMDX12-{VERSION}-win64-lite.zip")
    with open(zip_path, "wb") as f:
        f.write(b"not a real zip (the component path never downloads the zip)")
    feed = {
        "version": VERSION,
        "url": zip_path.replace("\\", "/"),
        "size": os.path.getsize(zip_path),
        "sha256": sha256_file(zip_path),
        "page": "https://example.invalid/releases/",
        "manifest": manifest_path.replace("\\", "/"),
        "manifestSha256": manifest_sha,
        "manifests": {VERSION: manifest_sha},
        "notes": {"en": "components e2e"},
    }
    feed_path = write_signed(feed, os.path.join(TMP, "latest.json"))
    log(f"  feed: {feed_path}")

    if not wait_no_app():
        sys.exit("another MMDX12.exe kept running; aborting before launching")

    # 3. The install = the stage minus the component (a lite install). The exe is the built one.
    install = os.path.join(TMP, "install")
    force_dirs_ok(install)
    shutil.copytree(stage, install)
    for name in COMPONENT_FILES:
        os.remove(os.path.join(install, name))
    check(not os.path.exists(os.path.join(install, COMPONENT_FILE)),
          f"{COMPONENT_FILE} removed from the install (lite)")

    # 4. Fetch it.
    log("\n== fetch the missing component ==")
    proc = run_app(["--update-feed", feed_path, "--component", COMPONENT, "--download-component"], install)
    logtext = read_log(install)
    check(proc is not None and proc.returncode == 0, "the app exited 0")
    check("COMPONENT ok=1" in logtext, "logged COMPONENT ok=1")
    dst = os.path.join(install, COMPONENT_FILE)
    for name in COMPONENT_FILES:
        check(os.path.exists(os.path.join(install, name)), f"{name} landed in the install")
    if os.path.exists(dst):
        check(sha256_file(dst) == expect_sha, "the fetched bytes match the staged file (SHA-256)")

    # 5. Idempotent: fetching again (already present) still succeeds.
    log("\n== fetch an already-present component ==")
    proc = run_app(["--update-feed", feed_path, "--component", COMPONENT, "--download-component"], install)
    logtext = read_log(install)
    check(proc is not None and proc.returncode == 0, "the app exited 0 again")
    check("COMPONENT ok=1" in logtext, "logged COMPONENT ok=1 again")
    if os.path.exists(dst):
        check(sha256_file(dst) == expect_sha, "the bytes are unchanged")

    # 6. An unknown component id fails cleanly (exit != 0, logged).
    log("\n== unknown component id ==")
    proc = run_app(["--update-feed", feed_path, "--component", "nope:none", "--download-component"], install)
    logtext = read_log(install)
    check(proc is not None and proc.returncode != 0, "the app exited non-zero")
    check("COMPONENT ok=0" in logtext, "logged COMPONENT ok=0")

    # 7. A server that lies. The evil store: a well-formed manifest whose component entry names the server's own DLL.
    evil_dll = b"MZ evil from the server"
    evil_sha = hashlib.sha256(evil_dll).hexdigest()
    evil_store = os.path.join(TMP, "store_evil")
    os.makedirs(os.path.join(evil_store, "manifests"))
    os.makedirs(os.path.join(evil_store, "objects", evil_sha[:2]))
    with open(os.path.join(evil_store, "objects", evil_sha[:2], evil_sha), "wb") as f:
        f.write(evil_dll)
    with open(manifest_path, encoding="utf-8") as f:
        evil_manifest = json.load(f)
    for entry in evil_manifest["files"]:
        if entry["path"] == COMPONENT_FILE:
            entry["sha256"] = evil_sha
            entry["size"] = len(evil_dll)
    evil_manifest_path = os.path.join(evil_store, "manifests", VERSION + ".json")
    with open(evil_manifest_path, "w", encoding="utf-8") as f:
        json.dump(evil_manifest, f, indent=1)
    evil_url = evil_manifest_path.replace("\\", "/")

    refused("the store's manifest replaced; the signed feed names the real one's hash", install,
            write_signed(dict(feed, manifest=evil_url), os.path.join(TMP, "feed_evil_manifest.json")),
            "manifest: checksum mismatch")

    # The real manifest, the DLL's object swapped.
    object_store = os.path.join(TMP, "store_evil_object")
    shutil.copytree(store, object_store, copy_function=os.link)
    target = os.path.join(object_store, "objects", expect_sha[:2], expect_sha)
    os.remove(target)
    with open(target, "wb") as f:
        f.write(evil_dll)
    refused("the component's object swapped", install,
            write_signed(dict(feed, manifest=os.path.join(object_store, "manifests", VERSION + ".json").replace("\\", "/")),
                         os.path.join(TMP, "feed_evil_object.json")),
            f"checksum mismatch for {COMPONENT_FILE}")

    evil_fields = dict(feed, manifest=evil_url, manifestSha256=sha256_file(evil_manifest_path),
                       manifests={VERSION: sha256_file(evil_manifest_path)})
    unsigned = os.path.join(TMP, "feed_unsigned.json")
    with open(unsigned, "w", encoding="utf-8") as f:
        json.dump(evil_fields, f, indent=1)
    refused("an unsigned feed naming the evil manifest", install, unsigned, "feed is not signed")

    changed = os.path.join(TMP, "feed_changed.json")
    write_signed(feed, changed)
    with open(changed, encoding="utf-8") as f:
        body = json.load(f)
    body["signedFeed"] = base64.b64encode(json.dumps(evil_fields).encode()).decode()
    with open(changed, "w", encoding="utf-8") as f:
        json.dump(body, f, indent=1)
    refused("a changed payload", install, changed, "feed signature is not valid")

    refused("a signed feed that does not name this version", install,
            write_signed(dict(feed, manifests={"0.0.1": manifest_sha}), os.path.join(TMP, "feed_other_version.json")),
            "no manifest hash for this version")

    log("")
    if failures:
        log(f"FAILED ({len(failures)}):")
        for f in failures:
            log("  - " + f)
    else:
        log("ALL CHECKS PASSED")
    if args.keep:
        log(f"workdir kept: {TMP}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
