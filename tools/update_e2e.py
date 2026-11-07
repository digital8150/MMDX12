#!/usr/bin/env python3
"""Headless end-to-end test for the updater: the delta path, the full zip fallback, and a server that lies.

Runs against a test build that trusts a throwaway key, because a feed must be signed for the exe to read it:

    python tools/sign_feed.py new-test-key build_feedtest/feed_test.pem     (prints the CMake option)
    cmake -S . -B build_feedtest -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMMDX12_FEED_TEST_KEY=<modulus>
    build.cmd build_feedtest --target MMDX12
    python tools/update_e2e.py [--keep]

Other build dir / key: the environment variables MMDX12_BUILD and MMDX12_FEED_TEST_PEM.

  * builds an "old" install from the build's program files, plus a dummy library file, an ini and a recovery file;
  * builds a mock "new" version 9.9.9 from that tree (one changed shader, one new file, one removed file in a subfolder, a
    VERSION.txt change), runs tools/make_update.py on it into a local object store, zips it, and signs a local feed;
  * every scenario runs the old exe --update-feed <feed> --ui-script <updateinstall> from a fresh or unchanged install:

      1 delta path                     the signed feed, everything honest
      2 corrupt object                 falls back to the zip, installs the real 9.9.9
      3 evil fields outside the feed   the signed bytes are all that count: installs the real 9.9.9
      4 evil manifest in the store     falls back to the zip, installs the real 9.9.9
      5 refused feeds                  unsigned, changed payload, another key, no zip hash: nothing starts
      6 every server file swapped      manifest, objects and zip: nothing is installed

Each run opens a window, one at a time. Standard library only (+ the openssl command, through tools/sign_feed.py).
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
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import sign_feed  # noqa: E402

TMP = os.path.join(os.environ.get("TEMP", os.environ.get("TMP", ".")), "opencode", "delta_e2e")
BUILD = os.path.join(ROOT, os.environ.get("MMDX12_BUILD", "build_feedtest"))
BIN = os.path.join(BUILD, "bin")
EXE = os.path.join(BIN, "MMDX12.exe")
KEY = os.environ.get("MMDX12_FEED_TEST_PEM", os.path.join(BUILD, "feed_test.pem"))
MAKE_UPDATE = os.path.join(ROOT, "tools", "make_update.py")
OLD_VER = "old"
NEW_VER = "9.9.9"
EVIL_FILE = "EVIL_FROM_THE_SERVER.txt"
EVIL_MARKER = "// evil from the server"

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


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)


def force_dirs_ok(path):
    """rmtree that tolerates read-only files (a git checkout can leave them)."""
    def onerror(func, p, exc):
        try:
            os.chmod(p, 0o700)
            func(p)
        except Exception:
            pass
    if os.path.exists(path):
        shutil.rmtree(path, onerror=onerror)


def read(path):
    if not os.path.exists(path):
        return ""
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def make_old_install(dest):
    """The build's program files, plus user data that must survive."""
    force_dirs_ok(dest)
    os.makedirs(dest)
    for name in os.listdir(BIN):
        if name == "MMDX12.exe" or name.lower().endswith(".dll"):
            shutil.copyfile(os.path.join(BIN, name), os.path.join(dest, name))
    shutil.copytree(os.path.join(BIN, "shaders"), os.path.join(dest, "shaders"))
    shutil.copytree(os.path.join(BIN, "assets"), os.path.join(dest, "assets"))
    # User data that the swap must leave alone.
    with open(os.path.join(dest, "mmdx12.ini"), "w", encoding="utf-8") as f:
        f.write("nickname=TestUser\nupdateFeedUrl=\n")
    with open(os.path.join(dest, "dummy_library_file.txt"), "w", encoding="utf-8") as f:
        f.write("user library content\n")
    os.makedirs(os.path.join(dest, "recovery"), exist_ok=True)
    with open(os.path.join(dest, "recovery", "recover.bin"), "wb") as f:
        f.write(b"recovery data")
    # A file the manifest will not list (removed version 9.9.9): must vanish after the swap.
    os.makedirs(os.path.join(dest, "shaders", "old"), exist_ok=True)
    with open(os.path.join(dest, "shaders", "old", "gone.hlsl"), "w", encoding="utf-8") as f:
        f.write("// removed in 9.9.9\n")
    os.makedirs(os.path.join(dest, "library"), exist_ok=True)
    with open(os.path.join(dest, "library", "user_model.txt"), "w", encoding="utf-8") as f:
        f.write("keep me\n")
    with open(os.path.join(dest, "VERSION.txt"), "w", encoding="utf-8") as f:
        f.write(OLD_VER + "\n")
    return dest


def make_new_tree(old, dest):
    """The mock 9.9.9 tree: one changed shader, one new file, one removed file, VERSION.txt."""
    force_dirs_ok(dest)
    shutil.copytree(old, dest)
    for d in ("shader_cache", "cache", "update_tmp", "recovery"):
        force_dirs_ok(os.path.join(dest, d))
    for f in os.listdir(dest):
        if f.endswith(".log") or f.lower() in ("mmdx12.ini", "dummy_library_file.txt"):
            os.remove(os.path.join(dest, f))
    with open(os.path.join(dest, "shaders", "mmd.hlsl"), "a", encoding="utf-8") as f:
        f.write("\n// 9.9.9 marker\n")
    with open(os.path.join(dest, "NEW_IN_999.txt"), "w", encoding="utf-8") as f:
        f.write("brand new in 9.9.9\n")
    os.remove(os.path.join(dest, "shaders", "old", "gone.hlsl"))
    os.rmdir(os.path.join(dest, "shaders", "old"))
    with open(os.path.join(dest, "VERSION.txt"), "w", encoding="utf-8") as f:
        f.write(NEW_VER + "\n")
    return dest


def zip_tree(tree, zip_path):
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for dp, dns, fns in os.walk(tree):
            for fn in fns:
                full = os.path.join(dp, fn)
                z.write(full, os.path.join(os.path.basename(tree), os.path.relpath(full, tree)))


def clone_store(store, name):
    """A copy of the object store that shares its bytes; replace a file with replace_file, never in place."""
    dest = os.path.join(TMP, name)
    force_dirs_ok(dest)
    shutil.copytree(store, dest, copy_function=os.link)
    return dest


def replace_file(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(path):
        os.remove(path)
    with open(path, "wb") as f:
        f.write(data)
    return hashlib.sha256(data).hexdigest()


def make_evil_manifest(store2):
    """What a taken-over server would publish: a well-formed manifest with its own, matching objects."""
    path = os.path.join(store2, "manifests", NEW_VER + ".json")
    manifest = json.loads(read(path))
    shader = (read(os.path.join(TMP, "new_9.9.9", "shaders", "mmd.hlsl")) + EVIL_MARKER + "\n").encode()
    evil = b"evil\n"
    for entry in manifest["files"]:
        if entry["path"] == "shaders/mmd.hlsl":
            entry["sha256"] = hashlib.sha256(shader).hexdigest()
            entry["size"] = len(shader)
            replace_file(os.path.join(store2, "objects", entry["sha256"][:2], entry["sha256"]), shader)
    sha = hashlib.sha256(evil).hexdigest()
    replace_file(os.path.join(store2, "objects", sha[:2], sha), evil)
    manifest["files"].append({"path": EVIL_FILE, "size": len(evil), "sha256": sha})
    replace_file(path, json.dumps(manifest, indent=1).encode())
    return path


def write_signed(feed, path, key=KEY, outer=None):
    """The signed feed file: the plain fields (or `outer`, a server's own idea of them) + the signed bytes."""
    data = json.dumps(feed, ensure_ascii=False, indent=1).encode("utf-8")
    signed_feed, signature = sign_feed.sign_bytes(data, key)
    body = dict(outer if outer is not None else feed)
    body["signedFeed"] = signed_feed
    body["signature"] = signature
    with open(path, "w", encoding="utf-8") as f:
        json.dump(body, f, ensure_ascii=False, indent=1)
    return path


def start_app(work, feed_path, script_name, extra=()):
    script = os.path.join(TMP, script_name)
    with open(script, "w", encoding="utf-8") as f:
        f.write("10 updateinstall\n")
    log_path = os.path.join(work, "mmdx12.log")
    if os.path.exists(log_path):
        os.remove(log_path)
    cmd = [os.path.join(work, "MMDX12.exe"), "--update-feed", feed_path, "--ui-script", script, "--screen", "select", *extra]
    log("  launching: " + " ".join(cmd))
    return subprocess.Popen(cmd, cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def installed_real_version(top):
    """The install holds the real 9.9.9 and the user's files, and nothing from a lying server."""
    check(read(os.path.join(top, "VERSION.txt")).strip() == NEW_VER, f"VERSION.txt is {NEW_VER}")
    check(os.path.exists(os.path.join(top, "NEW_IN_999.txt")), "NEW_IN_999.txt landed")
    mmd = read(os.path.join(top, "shaders", "mmd.hlsl"))
    check("9.9.9 marker" in mmd, "changed shader installed")
    check(EVIL_MARKER not in mmd, "the server's shader is not installed")
    check(not os.path.exists(os.path.join(top, EVIL_FILE)), "the server's extra file is not installed")
    check(not os.path.exists(os.path.join(top, "shaders", "old", "gone.hlsl")), "removed file is gone")
    check(os.path.exists(os.path.join(top, "shaders", "common.hlsli")), "unchanged file intact (shaders/common.hlsli)")
    check("nickname=TestUser" in read(os.path.join(top, "mmdx12.ini")), "mmdx12.ini intact")
    check(os.path.exists(os.path.join(top, "dummy_library_file.txt")), "dummy library file intact")
    check(os.path.exists(os.path.join(top, "recovery", "recover.bin")), "recovery file intact")
    check(os.path.exists(os.path.join(top, "library", "user_model.txt")), "library/ intact")
    check(not os.path.exists(os.path.join(top, "update_tmp")), "update_tmp cleaned")


def install_untouched(top):
    check(read(os.path.join(top, "VERSION.txt")).strip() == OLD_VER, "VERSION.txt is still the old one")
    check(not os.path.exists(os.path.join(top, "NEW_IN_999.txt")), "no new file landed")
    check(EVIL_MARKER not in read(os.path.join(top, "shaders", "mmd.hlsl")), "the server's shader is not installed")
    check(not os.path.exists(os.path.join(top, EVIL_FILE)), "the server's extra file is not installed")
    check(not os.path.exists(os.path.join(top, "update_tmp", "state.json")), "no update is staged")


def scenario(n, title, old, feed_path, expect_delta, expect_log=()):
    """One update that must end with the real 9.9.9 installed."""
    log(f"\n== scenario {n}: {title} ==")
    work = old  # the app runs from the old install dir
    proc = start_app(work, feed_path, f"update_{n}.script", ["--update-relaunch-frames", "60"])
    # Sample the updater's log while it runs: the pre-update log is archived on success, and the
    # relaunched app overwrites mmdx12.log, so read the live file before both.
    sampled = []
    deadline = time.time() + 180
    while proc.poll() is None and time.time() < deadline:
        for line in read(os.path.join(work, "mmdx12.log")).splitlines():
            if line not in sampled:
                sampled.append(line)
        time.sleep(0.4)
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
        check(False, "the updating app did not exit")
        proc.wait()
        return False
    # One immediate final read: the last lines (the delta result) may have been written between the
    # loop's last poll and the exit. The applier archives mmdx12.log to mmdx12.previous.log right
    # after, so poll both for a few seconds until the delta lines are in.
    for _ in range(40):
        for name in ("mmdx12.log", "mmdx12.previous.log"):
            for line in read(os.path.join(work, name)).splitlines():
                if line not in sampled:
                    sampled.append(line)
        if any("update: delta" in s or "falling back to the full zip" in s for s in sampled):
            break
        time.sleep(0.25)
    check(proc.returncode is not None, "updating app exited")
    log("  updater exited, waiting for the applier + relaunch...")
    # The applier writes applied.txt then relaunches; poll for it.
    applied = os.path.join(work, "update_tmp", "applied.txt")
    deadline = time.time() + 120
    while time.time() < deadline and not os.path.exists(applied):
        time.sleep(1)
    # The relaunched app starts fresh (shaders compile ~20 s), logs the breadcrumb and quits with
    # --frames. Poll the fresh log for its "quit after" line, then keep sampling the delta lines.
    log_path = os.path.join(work, "mmdx12.log")
    deadline = time.time() + 150
    while time.time() < deadline:
        if "quit after" in read(log_path):
            break
        time.sleep(1)
    time.sleep(1)
    for line in read(log_path).splitlines():
        if line not in sampled:
            sampled.append(line)
    # CleanupUpdateLeftovers removes update_tmp on the relaunched start; wait for it.
    deadline = time.time() + 30
    while time.time() < deadline and os.path.exists(os.path.join(work, "update_tmp")):
        time.sleep(1)

    # The final log: after the swap the applier archived the pre-update log to mmdx12.previous.log,
    # so the relaunched app wrote a fresh mmdx12.log. Combine the sampled pre-update log (which has
    # the delta lines), any .previous log and the final log.
    logtext = "\n".join(sampled)
    for name in ("mmdx12.previous.log", "mmdx12.log"):
        logtext += f"\n--- {name} ---\n" + read(os.path.join(work, name))
    with open(os.path.join(TMP, f"scenario{n}.log"), "w", encoding="utf-8") as f:
        f.write(logtext)

    if expect_delta:
        check("update: delta 3 files changed" in logtext, "3 files changed, delta path used")
        check("update: delta staged" in logtext, "'update: delta staged' logged")
        check("update: delta failed" not in logtext, "no fallback to zip in the delta run")
    else:
        check("update: delta failed" in logtext and "falling back to the full zip" in logtext,
              "delta failure fell back to the full zip")
    for text in expect_log:
        check(text in logtext, f"logged: {text}")

    installed_real_version(work)
    prev = read(os.path.join(work, "mmdx12.previous.log"))
    check("staged, spawning the applier" in prev and "installed - restarting" in prev,
          "mmdx12.previous.log keeps the updating app's + applier's log")
    check("installed (this run started after the update)" in logtext, "'update: 9.9.9 installed' breadcrumb logged")
    return not failures


def refused_scenario(n, title, work, feed_path, expect_log, extra=()):
    """One run that must leave the install exactly as it was."""
    log(f"\n== scenario {n}: {title} ==")
    proc = start_app(work, feed_path, f"update_{n}.script", extra)
    try:
        proc.wait(timeout=240)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        check(False, "the app did not exit")
        return
    text = read(os.path.join(work, "mmdx12.log"))
    check(expect_log in text, f"logged: {expect_log}")
    check("staged, spawning the applier" not in text, "no update was staged")
    install_untouched(work)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="do not delete the temp workspace")
    args = ap.parse_args()

    if not os.path.exists(EXE):
        sys.exit(f"{EXE} missing: see this script's header for the test build")
    if not os.path.exists(KEY):
        sys.exit(f"{KEY} missing: see this script's header for the test key")

    force_dirs_ok(TMP)
    os.makedirs(TMP, exist_ok=True)
    store = os.path.join(TMP, "store")
    os.makedirs(store, exist_ok=True)

    log("== building the old install + new 9.9.9 tree ==")
    old = make_old_install(os.path.join(TMP, "old_a"))
    newtree = make_new_tree(old, os.path.join(TMP, "new_9.9.9"))

    # make_update.py on the new tree -> local manifest + object store. No --base-url: the
    # manifest's objects field stays "../objects/", resolved relative to the manifest file.
    res = run([sys.executable, MAKE_UPDATE, newtree, "--version", NEW_VER, "--out", store])
    log(res.stdout.strip())
    if res.returncode != 0:
        sys.exit("make_update.py failed")
    manifest_path = os.path.join(store, "manifests", NEW_VER + ".json")
    manifest_sha = sha256_file(manifest_path)

    log("== zipping the full fallback ==")
    zip_path = os.path.join(TMP, f"MMDX12-{NEW_VER}-win64.zip")
    zip_tree(newtree, zip_path)

    feed = {
        "version": NEW_VER,
        "url": zip_path.replace("\\", "/"),
        "size": os.path.getsize(zip_path),
        "sha256": sha256_file(zip_path),
        "page": "https://example.invalid/releases/",
        "manifest": manifest_path.replace("\\", "/"),
        "manifestSha256": manifest_sha,
        "manifests": {NEW_VER: manifest_sha},
        "notes": {"ko": "9.9.9 테스트", "en": "9.9.9 test"},
    }

    def feed_at(store2):
        """The genuine feed, signed for a store at another path: the hashes stay the real ones."""
        return dict(feed, manifest=os.path.join(store2, "manifests", NEW_VER + ".json").replace("\\", "/"))

    # 1: everything honest.
    scenario(1, "delta path", old, write_signed(feed, os.path.join(TMP, "feed_1.json")), True)

    # 2: one object corrupted in the store.
    store2 = clone_store(store, "store_corrupt")
    target = next(f["sha256"] for f in json.loads(read(manifest_path))["files"] if f["path"] == "shaders/mmd.hlsl")
    replace_file(os.path.join(store2, "objects", target[:2], target), b"CORRUPT!")
    scenario(2, "corrupt object", make_old_install(os.path.join(TMP, "old_b")),
             write_signed(feed_at(store2), os.path.join(TMP, "feed_2.json")), False, ["object hash mismatch"])

    # 3: the server rewrites every field outside the signed bytes; they point at its own zip and manifest.
    store3 = clone_store(store, "store_evil_outer")
    evil_manifest = make_evil_manifest(store3)
    evil_zip = os.path.join(TMP, "evil.zip")
    with zipfile.ZipFile(evil_zip, "w") as z:
        z.writestr(f"MMDX12-99.0.0-win64/{EVIL_FILE}", "evil\n")
    outer = dict(feed, version="99.0.0", url=evil_zip.replace("\\", "/"), sha256=sha256_file(evil_zip),
                 size=os.path.getsize(evil_zip), manifest=evil_manifest.replace("\\", "/"),
                 manifestSha256=sha256_file(evil_manifest))
    scenario(3, "evil fields outside the signed bytes", make_old_install(os.path.join(TMP, "old_c")),
             write_signed(feed, os.path.join(TMP, "feed_3.json"), outer=outer), True)

    # 4: the server replaces the manifest; the signed feed names the real one's hash.
    store4 = clone_store(store, "store_evil_manifest")
    make_evil_manifest(store4)
    scenario(4, "evil manifest", make_old_install(os.path.join(TMP, "old_d")),
             write_signed(feed_at(store4), os.path.join(TMP, "feed_4.json")), False, ["manifest checksum mismatch"])

    # 5 + 6: nothing may start. One install serves them all: its first run warms the shader cache.
    untouched = make_old_install(os.path.join(TMP, "old_e"))

    unsigned = os.path.join(TMP, "feed_unsigned.json")
    with open(unsigned, "w", encoding="utf-8") as f:
        json.dump(outer, f, indent=1)
    refused_scenario("5a", "an unsigned feed", untouched, unsigned, "UPDATEINSTALL ok=0 error='feed is not signed'")

    changed = os.path.join(TMP, "feed_changed.json")
    body = json.loads(read(write_signed(feed, changed)))
    body["signedFeed"] = base64.b64encode(json.dumps(outer).encode()).decode()
    with open(changed, "w", encoding="utf-8") as f:
        json.dump(body, f, indent=1)
    refused_scenario("5b", "a changed payload", untouched, changed, "UPDATEINSTALL ok=0 error='feed signature is not valid'")

    other_key = os.path.join(TMP, "other.pem")
    sign_feed.openssl("genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048", "-out", other_key,
                      stderr=subprocess.DEVNULL)
    refused_scenario("5c", "another key's signature", untouched,
                     write_signed(outer, os.path.join(TMP, "feed_other_key.json"), key=other_key),
                     "UPDATEINSTALL ok=0 error='feed signature is not valid'")

    no_hash = {k: v for k, v in feed.items() if k != "sha256"}
    refused_scenario("5d", "a signed feed without a zip hash", untouched,
                     write_signed(no_hash, os.path.join(TMP, "feed_no_hash.json")),
                     "UPDATEINSTALL ok=0 error='feed has no zip hash'")

    # 6: manifest, objects and zip all swapped. The feed's hashes hold: both paths fail, nothing is staged.
    store6 = clone_store(store, "store_evil_all")
    make_evil_manifest(store6)
    evil6 = dict(feed_at(store6), url=evil_zip.replace("\\", "/"), size=os.path.getsize(evil_zip))
    refused_scenario(6, "every server file swapped", untouched,
                     write_signed(evil6, os.path.join(TMP, "feed_6.json")), "update: checksum mismatch",
                     ["--frames", "600"])

    log("")
    if failures:
        log(f"FAILED ({len(failures)}):")
        for f in failures:
            log("  - " + f)
    else:
        log("ALL CHECKS PASSED")
    if args.keep:
        log(f"workdir kept: {TMP}")
    else:
        force_dirs_ok(TMP)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
