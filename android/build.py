#!/usr/bin/env python3
"""Build the QuickSolar APK: no Gradle, just the Android SDK build tools.

    python3 android/build.py            -> dist/QuickSolar-v<version>.apk

Needs a JDK (javac, jar, keytool) and the Android SDK (build-tools + a platform),
found via ANDROID_HOME / ANDROID_SDK_ROOT or the usual install location.
Signing: SOLARD_KEYSTORE (+ SOLARD_KEYSTORE_PASS, SOLARD_KEY_ALIAS) if set,
otherwise a key created once at ~/.config/solard/release.p12 (outside the repo).
The dashboard page is ../server/index.html, bundled as an asset.
"""
from pathlib import Path
import os
import shutil
import subprocess
from zipfile import ZipFile, ZIP_DEFLATED

root = Path(__file__).resolve().parent
repo = root.parent
build = root / "build"
import re
_ver = re.search(r'versionName="([^"]+)"', (root / "AndroidManifest.xml").read_text()).group(1)
_ver = _ver if _ver.count(".") >= 2 else _ver + ".0"
out = repo / "dist" / f"QuickSolar-v{_ver}.apk"        # e.g. QuickSolar-v1.0.0.apk (one APK for all devices)


def sdk_dir():
    for c in (os.environ.get("ANDROID_HOME"), os.environ.get("ANDROID_SDK_ROOT"),
              str(Path.home() / "Library/Android/sdk"), str(Path.home() / "Android/Sdk"), "/usr/local/lib/android/sdk"):
        if c and Path(c, "build-tools").is_dir():
            return Path(c)
    raise SystemExit("Android SDK not found: set ANDROID_HOME")


def newest(d, pattern):
    items = sorted(Path(d).glob(pattern), key=lambda p: [int(x) if x.isdigit() else 0 for x in p.name.replace("-", ".").split(".")])
    if not items:
        raise SystemExit(f"nothing matching {pattern} in {d}")
    return items[-1]


sdk = sdk_dir()
tools = newest(sdk / "build-tools", "*")
platform = newest(sdk / "platforms", "android-*") / "android.jar"
ext = ".bat" if os.name == "nt" else ""

# signing key
ks = os.environ.get("SOLARD_KEYSTORE")
ks_pass = os.environ.get("SOLARD_KEYSTORE_PASS", "")
alias = os.environ.get("SOLARD_KEY_ALIAS", "quicksolar")
if not ks:
    cfg = Path.home() / ".config" / "solard"
    ks = str(cfg / "release.p12")
    pw_file = cfg / "release.pass"
    if not Path(ks).exists():
        cfg.mkdir(parents=True, exist_ok=True)
        pw_file.write_text(os.urandom(18).hex())
        os.chmod(pw_file, 0o600)
        subprocess.run(["keytool", "-genkeypair", "-keystore", ks, "-storetype", "PKCS12", "-alias", alias,
                        "-keyalg", "RSA", "-keysize", "3072", "-validity", "10000", "-dname", "CN=QuickSolar",
                        "-storepass", pw_file.read_text(), "-keypass", pw_file.read_text()], check=True)
        print("created signing key", ks)
    ks_pass = pw_file.read_text().strip()

(root / "assets").mkdir(exist_ok=True)
shutil.copyfile(repo / "server" / "index.html", root / "assets" / "index.html")
shutil.rmtree(build / "classes", ignore_errors=True)
(build / "classes").mkdir(parents=True, exist_ok=True)
(build / "dex").mkdir(exist_ok=True)
out.parent.mkdir(exist_ok=True)

sources = sorted(str(p) for p in (root / "src").rglob("*.java"))
subprocess.run(["javac", "--release", "8", "-nowarn", "-classpath", str(platform), "-d", str(build / "classes")] + sources, check=True)
subprocess.run(["jar", "cf", str(build / "classes.jar"), "-C", str(build / "classes"), "."], check=True)
subprocess.run([str(tools / ("d8" + ext)), "--min-api", "21", "--lib", str(platform),
                "--output", str(build / "dex"), str(build / "classes.jar")], check=True)
subprocess.run([str(tools / "aapt"), "package", "-f", "-M", str(root / "AndroidManifest.xml"),
                "-S", str(root / "res"), "-I", str(platform), "-F", str(build / "base.apk")], check=True)

with ZipFile(build / "base.apk") as src, ZipFile(build / "unsigned.apk", "w") as dst:
    for entry in src.infolist():
        dst.writestr(entry, src.read(entry.filename))
    dst.writestr("classes.dex", (build / "dex/classes.dex").read_bytes(), compress_type=ZIP_DEFLATED)
    for asset in sorted((root / "assets").rglob("*")):
        if asset.is_file():
            dst.write(str(asset), "assets/" + str(asset.relative_to(root / "assets")))

subprocess.run([str(tools / "zipalign"), "-f", "4", str(build / "unsigned.apk"), str(build / "aligned.apk")], check=True)
subprocess.run([str(tools / ("apksigner" + ext)), "sign", "--ks", ks, "--ks-key-alias", alias,
                "--ks-pass", "pass:" + ks_pass, "--out", str(out), str(build / "aligned.apk")], check=True)
print("built", out, out.stat().st_size, "bytes")
