#!/usr/bin/env python3
"""Update feed signatures for tests and key setup (src/core/FeedSignature.h, src/app/Updater.h). Needs the openssl command.

    python tools/sign_feed.py modulus <key.pem>
        Prints the key's public modulus: what src/app/Updater.h holds. An encrypted key asks for its passphrase.
    python tools/sign_feed.py new-test-key <out.pem>
        Writes a throwaway key and prints the CMake option of a test build that trusts it (tools/update_e2e.py,
        tools/components_e2e.py). Never for a release: tools/package_release.ps1 refuses that option.

Real feeds are signed in CI with this same openssl call: .github/workflows/nightly.yml (nightly), the website repo's
sign-feed workflow (stable). Standard library only.
"""
import argparse
import base64
import os
import shutil
import subprocess
import sys
import tempfile

SIGN_OPTS = ["-sha256", "-sigopt", "rsa_padding_mode:pss", "-sigopt", "rsa_pss_saltlen:32"]


def openssl(*args, **kw):
    exe = shutil.which("openssl")
    if not exe:
        sys.exit("error: no openssl on PATH (Git for Windows has one in mingw64/bin)")
    return subprocess.run([exe, *args], check=True, **kw)


def modulus_of(key_path):
    out = openssl("rsa", "-in", key_path, "-noout", "-modulus", stdout=subprocess.PIPE, text=True).stdout
    return base64.b64encode(bytes.fromhex(out.strip().split("=", 1)[1])).decode()


def sign_bytes(data, key_path):
    """(signedFeed, signature) in base64 over exactly `data`."""
    with tempfile.TemporaryDirectory() as tmp:
        payload = os.path.join(tmp, "payload")
        signature = os.path.join(tmp, "signature")
        with open(payload, "wb") as f:
            f.write(data)
        openssl("dgst", *SIGN_OPTS, "-sign", key_path, "-out", signature, payload)
        with open(signature, "rb") as f:
            sig = f.read()
    return base64.b64encode(data).decode(), base64.b64encode(sig).decode()


def main():
    ap = argparse.ArgumentParser(description="Update feed signatures for tests and key setup.")
    sub = ap.add_subparsers(dest="command", required=True)
    p = sub.add_parser("modulus")
    p.add_argument("key")
    p = sub.add_parser("new-test-key")
    p.add_argument("out")
    args = ap.parse_args()

    if args.command == "modulus":
        print(modulus_of(args.key))
    else:
        if os.path.exists(args.out):
            sys.exit(f"error: {args.out} exists")
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        openssl("genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048", "-out", args.out,
                stderr=subprocess.DEVNULL)
        print(f"-DMMDX12_FEED_TEST_KEY={modulus_of(args.out)}")


if __name__ == "__main__":
    main()
