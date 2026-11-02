"""Download a Qt kit straight from the Qt repository and verify every archive.

Usage: fetch_qt.py <output dir> [version]
Prints the kit's install prefix on the last line.
"""
import hashlib
import os
import platform
import sys
import urllib.request
import xml.etree.ElementTree as ET

import py7zr

OUT = os.path.abspath(sys.argv[1])
VERSION = sys.argv[2] if len(sys.argv) > 2 else "6.12.0"
COMPACT = VERSION.replace(".", "")
SYSTEM = platform.system()
if SYSTEM == "Windows":
    HOST, FOLDER, KIT = "windows_x86", f"qt6_{COMPACT}_msvc2022_64", "win64_msvc2022_64"
elif SYSTEM == "Darwin":
    HOST, FOLDER, KIT = "mac_x64", f"qt6_{COMPACT}", "clang_64"
else:
    HOST, FOLDER, KIT = "linux_x64", f"qt6_{COMPACT}", "linux_gcc_64"
BASE = f"https://download.qt.io/online/qtsdkrepository/{HOST}/desktop/qt6_{COMPACT}/{FOLDER}"
WANT = [f"qt.qt6.{COMPACT}.{KIT}", f"qt.qt6.{COMPACT}.addons.qt5compat.{KIT}"]


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": "FormaIDE-CI"})
    with urllib.request.urlopen(request, timeout=600) as response:
        return response.read()


os.makedirs(OUT, exist_ok=True)
packages = {item.findtext("Name"): item for item in ET.fromstring(fetch(BASE + "/Updates.xml")).findall("PackageUpdate")}
for name in WANT:
    package = packages.get(name)
    if package is None:
        sys.exit(f"Qt package {name} was not found")
    version = package.findtext("Version")
    for archive in (package.findtext("DownloadableArchives") or "").split(","):
        archive = archive.strip()
        if not archive:
            continue
        url = f"{BASE}/{name}/{version}{archive}"
        print("get", archive, flush=True)
        data = fetch(url)
        if hashlib.sha1(data).hexdigest() != fetch(url + ".sha1").decode().split()[0]:
            sys.exit(f"checksum mismatch for {archive}")
        path = os.path.join(OUT, "download.7z")
        with open(path, "wb") as f:
            f.write(data)
        with py7zr.SevenZipFile(path) as z:
            z.extractall(OUT)
        os.remove(path)

for directory, _, files in os.walk(OUT):
    if "Qt6Config.cmake" in files:
        print(os.path.abspath(os.path.join(directory, "..", "..", "..")))
        break
else:
    sys.exit("Qt6Config.cmake was not found after unpacking")
