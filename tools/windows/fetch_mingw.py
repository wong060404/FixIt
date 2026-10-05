"""Fetch a self-contained MinGW-w64 GCC (winlibs) for building FixIt on Windows.

    python tools/windows/fetch_mingw.py [destination]

Nothing is installed system-wide: the archive is unpacked into a folder and used
from there.  The default destination is `<repository parent>/toolchain`, so a
checkout in `C:\\FixIt\\FixIt` gets its compiler in `C:\\FixIt\\toolchain`.
The downloaded archive is kept under `tools/windows/.cache/` (git-ignored) so
re-running the script is cheap.

The destination must be a pure ASCII path.  gcc derives its own library
directory from where its executable sits and hands those absolute paths to the
linker, which decodes them in the local code page; a non-ASCII path (Chinese,
for example) makes every `-l` and `crt*.o` lookup fail with a confusing "No such
file or directory".
"""

import json
import os
import sys
import urllib.request
import zipfile

UA = {"User-Agent": "fixit-toolchain-fetch/1.0"}
API = "https://api.github.com/repos/brechtsanders/winlibs_mingw/releases/latest"
HERE = os.path.dirname(os.path.abspath(__file__))


def find_repo_root(start):
    """Walk up until the FixIt checkout is found."""
    directory = os.path.abspath(start)
    while True:
        if (os.path.exists(os.path.join(directory, "CMakeLists.txt"))
                and os.path.exists(os.path.join(directory, "src", "compiler.cpp"))):
            return directory
        parent = os.path.dirname(directory)
        if parent == directory:
            raise SystemExit("FixIt repository not found above %s" % start)
        directory = parent


REPO = find_repo_root(HERE)
CACHE = os.path.join(HERE, ".cache")
DEST = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 \
    else os.path.join(os.path.dirname(REPO), "toolchain")


def pick_asset(assets):
    best = None
    for asset in assets:
        name = asset["name"].lower()
        if not name.endswith(".zip"):
            continue
        if "x86_64" not in name or "-gcc-" not in name or "llvm" in name or "clang" in name:
            continue
        if "posix" not in name or "seh" not in name or "ucrt" not in name:
            continue
        best = asset
    return best


def main():
    if not DEST.isascii():
        print("WARNING: the destination path contains non-ASCII characters:")
        print("         %s" % DEST)
        print("         gcc cannot link from such a location. Clone FixIt into a")
        print("         plain ASCII folder such as C:\\FixIt and try again.")
        return 2

    os.makedirs(DEST, exist_ok=True)
    os.makedirs(CACHE, exist_ok=True)
    print("repository : %s" % REPO)
    print("destination: %s" % DEST)

    with urllib.request.urlopen(urllib.request.Request(API, headers=UA), timeout=60) as response:
        release = json.load(response)
    asset = pick_asset(release["assets"])
    if asset is None:
        print("no suitable winlibs release asset found; available:")
        for candidate in release["assets"]:
            print("  ", candidate["name"])
        return 1

    print("release    : %s" % release["tag_name"])
    print("asset      : %s (%.1f MB)" % (asset["name"], asset["size"] / 1e6))
    path = os.path.join(CACHE, asset["name"])
    if not os.path.exists(path) or os.path.getsize(path) != asset["size"]:
        with urllib.request.urlopen(
            urllib.request.Request(asset["browser_download_url"], headers=UA), timeout=1800
        ) as response, open(path, "wb") as out:
            total = 0
            while True:
                chunk = response.read(1 << 20)
                if not chunk:
                    break
                out.write(chunk)
                total += len(chunk)
                if total % (50 << 20) < (1 << 20):
                    print("   ... %.0f MB" % (total / 1e6))
        print("downloaded -> %s" % path)

    print("extracting (about 1.5 GB, this takes a minute) ...")
    with zipfile.ZipFile(path) as archive:
        archive.extractall(DEST)
    print("extracted  -> %s" % DEST)

    found = []
    for root, _dirs, files in os.walk(DEST):
        for entry in files:
            if entry.lower() in ("g++.exe", "gcc.exe"):
                found.append(os.path.join(root, entry))
    for compiler in found:
        print("compiler   : %s" % compiler)
    if not found:
        print("WARNING: no gcc.exe/g++.exe found after extraction")
        return 1

    print("")
    print("Next step:")
    print("  powershell -ExecutionPolicy Bypass -File \"%s\""
          % os.path.join(REPO, "tools", "windows", "build_fixit.ps1"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
