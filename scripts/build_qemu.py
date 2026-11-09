#!/usr/bin/env python3
"""Build pinned QEMU 10.2.1 (x86_64-softmmu only) with the kdc-dma device model.

Everything lands in the gitignored .cache/ directory:
  .cache/downloads/  verified upstream tarballs
  .cache/src/        extracted sources (QEMU gets the device overlay copied in)
  .cache/deps/       glib built from source (the build host has no -dev packages)
  .cache/build/qemu        release build      -> qemu-system-x86_64
  .cache/build/qemu-asan   ASan+UBSan build   -> qemu-system-x86_64

The device model itself lives in qemu/ in this repository; this script copies it
into the extracted tree and adds one Kconfig entry and one meson line.
"""
import argparse
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]
CACHE = ROOT / ".cache"
QEMU_VERSION = "10.2.1"
QEMU_URL = f"https://download.qemu.org/qemu-{QEMU_VERSION}.tar.xz"
QEMU_SHA256 = "a3717477d8e2c84d630bfffbc20f6cd3293eb45aa1e6dac6d0cc27689991c9e1"
GLIB_VERSION = "2.86.5"
GLIB_URL = f"https://download.gnome.org/sources/glib/2.86/glib-{GLIB_VERSION}.tar.xz"
GLIB_SHA256 = "ce85a947bb8b3c0204dbeff79aec39bcb46371c6fafb64ba5b8726c71e038d5f"

KCONFIG = """
config KDC_DMA
    bool
    default y if PCI_DEVICES
    depends on PCI && MSI_NONBROKEN
"""
MESON = "system_ss.add(when: 'CONFIG_KDC_DMA', if_true: files('kdc_dma.c'))\n"


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def fetch(url, digest):
    downloads = CACHE / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    path = downloads / url.rsplit("/", 1)[1]
    if not path.exists():
        partial = path.with_suffix(path.suffix + ".partial")
        with urllib.request.urlopen(url, timeout=120) as response, open(partial, "wb") as out:
            shutil.copyfileobj(response, out)
        partial.rename(path)
    actual = sha256(path)
    if actual != digest:
        raise RuntimeError(f"{path}: sha256 {actual} != pinned {digest}")
    return path


def extract(archive, name):
    """Extract with tarfile's "data" filter (no absolute paths, no traversal, no
    links pointing outside the tree). Symlink members the filter refuses are
    skipped and listed in <name>.skipped.txt: QEMU 10.2.1 ships one absolute
    symlink (roms/edk2/EmulatorPkg/Unix/Host/X11IncludeHack -> /opt/X11/include)
    that the build never uses. Anything else the filter refuses still fails.
    The tree is extracted next to its final place and renamed when complete."""
    target = CACHE / "src" / name
    if target.is_dir():
        return target
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_name(name + ".partial")
    shutil.rmtree(partial, ignore_errors=True)
    partial.mkdir()
    skipped = []

    def data_filter_skip_links(member, path):
        try:
            return tarfile.data_filter(member, path)
        except (tarfile.AbsoluteLinkError, tarfile.LinkOutsideDestinationError) as e:
            if member.issym():
                skipped.append(f"{member.name} -> {member.linkname} ({type(e).__name__})")
                return None
            raise

    with tarfile.open(archive) as tar:
        tar.extractall(partial, filter=data_filter_skip_links)
    (top,) = list(partial.iterdir())
    if top.name != name:
        raise RuntimeError(f"{archive}: unexpected top-level directory {top.name}")
    top.rename(target)
    partial.rmdir()
    (target.parent / f"{name}.skipped.txt").write_text("".join(f"{s}\n" for s in skipped))
    for s in skipped:
        print(f"skipped unsafe link {s}", flush=True)
    return target


def venv(qemu_src):
    """Meson for glib comes from the wheel bundled in the QEMU tarball (offline)."""
    env_dir = CACHE / "venv"
    meson = env_dir / "bin/meson"
    if not meson.exists():
        run([sys.executable, "-m", "venv", env_dir])
        wheel = next((qemu_src / "python/wheels").glob("meson-*.whl"))
        run([env_dir / "bin/pip", "install", "--no-index", wheel])
    return meson


def build_glib(meson, glib_src, jobs):
    prefix = CACHE / "deps"
    if (prefix / "lib/pkgconfig/glib-2.0.pc").exists():
        return prefix
    build = CACHE / "build/glib"
    if not (build / "build.ninja").exists():
        # pcre2 and libffi are not installed as -dev packages; meson fetches them
        # through glib's own pinned wrap files (URL + sha256 inside subprojects/).
        run([meson, "setup", build, glib_src, f"--prefix={prefix}", "--libdir=lib",
             "--buildtype=release", "--wrap-mode=default", "-Dtests=false",
             "-Dintrospection=disabled", "-Dnls=disabled", "-Dman-pages=disabled",
             "-Ddocumentation=false", "-Dselinux=disabled", "-Dlibmount=disabled",
             "-Dsysprof=disabled", "-Dlibelf=disabled", "-Dxattr=false", "-Dglib_debug=disabled"])
    run(["nice", "ninja", "-C", build, f"-j{jobs}", "install"])
    return prefix


def overlay(qemu_src):
    misc = qemu_src / "hw/misc"
    shutil.copy(ROOT / "qemu/kdc_dma.c", misc / "kdc_dma.c")
    shutil.copy(ROOT / "include/kdc_hw.h", misc / "kdc_hw.h")
    for path, text, marker in ((misc / "Kconfig", KCONFIG, "config KDC_DMA"),
                               (misc / "meson.build", MESON, "kdc_dma.c")):
        content = path.read_text()
        if marker not in content:
            path.write_text(content.rstrip("\n") + "\n" + text)


def build_qemu(qemu_src, deps, name, extra, jobs):
    build = CACHE / "build" / name
    env = dict(os.environ, PKG_CONFIG_PATH=str(deps / "lib/pkgconfig"))
    if not (build / "build.ninja").exists():
        build.mkdir(parents=True, exist_ok=True)
        run([qemu_src / "configure", "--target-list=x86_64-softmmu", "--without-default-features",
             "--enable-kvm", "--enable-tcg", "--disable-pixman", "--disable-docs", "--disable-tools",
             "--disable-user", "--disable-rust", "--disable-werror",
             f"--extra-ldflags=-Wl,-rpath,{deps / 'lib'}", *extra], cwd=build, env=env)
    run(["nice", "ninja", "-C", build, f"-j{jobs}", "qemu-system-x86_64"], env=env)
    binary = build / "qemu-system-x86_64"
    print(subprocess.check_output([binary, "--version"], text=True).splitlines()[0], f"({name})")
    return binary


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--jobs", type=int, default=16)
    p.add_argument("--flavor", choices=("release", "asan", "both"), default="both")
    args = p.parse_args()
    jobs = min(args.jobs, 16)
    qemu_src = extract(fetch(QEMU_URL, QEMU_SHA256), f"qemu-{QEMU_VERSION}")
    glib_src = extract(fetch(GLIB_URL, GLIB_SHA256), f"glib-{GLIB_VERSION}")
    deps = build_glib(venv(qemu_src), glib_src, jobs)
    overlay(qemu_src)
    if args.flavor in ("release", "both"):
        build_qemu(qemu_src, deps, "qemu", [], jobs)
    if args.flavor in ("asan", "both"):
        build_qemu(qemu_src, deps, "qemu-asan", ["--enable-asan", "--enable-ubsan", "--enable-debug"], jobs)


if __name__ == "__main__":
    main()
