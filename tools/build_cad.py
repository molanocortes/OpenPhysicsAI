#!/usr/bin/env python3
"""build_cad.py - fetch and build the pinned geometry libraries of printed-part design, without Homebrew
(docs/THIRD_PARTY.md, docs/cad.md):

    python3 tools/build_cad.py [--fetch-only]      (make cad; the first build takes about an hour on the laptop)

- CMake: on macOS Kitware's 3.31.12 binary, pinned; on Linux the distribution's own (3.18 or newer). It only builds
  the two below and is not linked into anything;
- Open CASCADE Technology 7.9.3 (LGPL-2.1 with its exception; the owner's decision of 2026-10-08): exact solids,
  their features (extrude, revolve, loft, fillet, shell, booleans) and STEP, as static libraries;
- Manifold 3.5.4 (Apache-2.0): meshes that are always closed and manifold, made from a signed distance (level sets)
  and combined by booleans that do not fail: the lattices and infills of printed parts.

Everything lands in one place per machine, $CAD_TP or ~/.openphysicsai/third_party/ (sources, builds and installs),
shared by every copy of the repository on that machine, so the hour is paid once per machine and not once per worktree.
Archives are checked against the SHA-256 pinned here before anything is unpacked. Built in double precision, no
fast-math, -ffp-contract=off (AGENTS.md rule 1), two compile jobs at a time (CAD_JOBS to change it), so that the shared
laptop keeps working.
"""
import hashlib, os, pathlib, platform, re, shutil, subprocess, sys, tarfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
BASE = pathlib.Path(os.environ.get("CAD_TP") or (pathlib.Path.home() / ".openphysicsai" / "third_party")).expanduser()
JOBS = os.environ.get("CAD_JOBS", "2")

CMAKE = ("3.31.12", "https://github.com/Kitware/CMake/releases/download/v3.31.12/cmake-3.31.12-macos-universal.tar.gz",
         "799af7fd545db9bf1b9cfe72f8095880e727a2d4e0df0e3dffc3bc7b95c2d3b0")  # Kitware's cmake-3.31.12-SHA-256.txt
OCCT = ("7.9.3", "https://codeload.github.com/Open-Cascade-SAS/OCCT/tar.gz/refs/tags/V7_9_3",
        "5ecf094ec6b12d5413dfb851d8c3590c354058aee556e32e408bdfbf8c357d57")  # pinned at its first download, 2026-10-08
MANIFOLD = ("3.5.4", "https://codeload.github.com/elalish/manifold/tar.gz/refs/tags/v3.5.4",
            "ce2f8b7798409b9f07e092c0ec0524ed0806431e01aea4020b65d0e934ea9438")  # pinned at its first download, 2026-10-08


def fetch(name, url, sha, archive):
    """the archive, downloaded once (curl: Python's own TLS has no certificates on this Mac) and checked"""
    if not archive.exists():
        part = archive.with_suffix(".download")
        subprocess.run(["curl", "-fL", "--retry", "3", "--max-time", "1800", "-o", str(part), url], check=True)
        part.replace(archive)
    got = hashlib.sha256(archive.read_bytes()).hexdigest()
    if sha and got != sha:
        raise SystemExit("%s: archive checksum %s, pinned %s; refusing to build" % (name, got, sha))
    if not sha:
        print("%s: archive SHA-256 %s (not pinned yet: write it into tools/build_cad.py and docs/THIRD_PARTY.md)" % (name, got))
    return got


def unpack(archive, into):
    """the archive's one top folder, unpacked safely; its path"""
    with tarfile.open(archive) as tar:
        members = tar.getmembers()
        top = members[0].name.split("/")[0]
        dest = (into / top).resolve()
        if not dest.exists():
            for m in members:
                target = (into / m.name).resolve()
                if not (target == dest or target.is_relative_to(dest)) or not (m.isdir() or m.isfile() or m.issym()):
                    raise SystemExit("unsafe archive member: %s" % m.name)
                if m.issym() and not (target.parent / m.linkname).resolve().is_relative_to(dest):
                    raise SystemExit("unsafe archive link: %s" % m.name)
            tar.extractall(into)
    return into / top


def cxx_flags():
    flags = ["-ffp-contract=off"]
    if platform.system() == "Darwin":  # some Command Line Tools put libc++'s headers only in the SDK (as for MFEM)
        sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
        if (pathlib.Path(sdk) / "usr/include/c++/v1").is_dir():
            flags += ["-isystem", str(pathlib.Path(sdk) / "usr/include/c++/v1")]
    return " ".join(flags)


def identity(key):
    """what a build is, without the folder it was installed in (moving the libraries does not make them a new build);
    the folder may hold spaces ("Fluid Sim"), so the option is cut up to the next option, not at a space"""
    return " ".join(re.sub(r"-DINSTALL_DIR=.*?(?= -D|$)", "", key).split())


def build(name, cmake, source, build_dir, install_dir, options):
    stamp = install_dir / ".openphysics-built"
    old = install_dir / ".navier-built"  # the stamp of a build made before the rename of 2026-10-21, as good as the new one
    key = " ".join(options + [cxx_flags()])
    for found in (stamp, old):
        if found.exists() and identity(found.read_text()) == identity(key):
            if found is old:
                stamp.write_text(old.read_text())
            stamp.touch()  # so that make sees it newer than this script
            print("%s already built: %s" % (name, install_dir))
            return
    build_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(cmake), "-S", str(source), "-B", str(build_dir), "-G", "Unix Makefiles", "-DCMAKE_BUILD_TYPE=Release",
                    "-DCMAKE_INSTALL_PREFIX=" + str(install_dir), "-DCMAKE_CXX_FLAGS=" + cxx_flags(),
                    "-DCMAKE_C_FLAGS=-ffp-contract=off", "-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0"] + options, check=True)
    subprocess.run([str(cmake), "--build", str(build_dir), "-j", JOBS], check=True)
    subprocess.run([str(cmake), "--install", str(build_dir)], check=True)
    stamp.write_text(key)
    print("%s built and installed: %s" % (name, install_dir))


def main():
    fetch_only = "--fetch-only" in sys.argv
    sums = {}
    darwin = platform.system() == "Darwin"
    if not darwin:  # the distribution's CMake (Ubuntu: sudo apt install cmake), checked before anything is downloaded
        found = shutil.which("cmake")
        if not found and not fetch_only:
            raise SystemExit("build_cad.py: CMake 3.18 or newer is needed (Ubuntu: sudo apt install cmake); nothing was downloaded")
        if found:
            version = subprocess.check_output([found, "--version"], text=True).split()[2]
            if tuple(int(x) for x in version.split(".")[:2]) < (3, 18) and not fetch_only:
                raise SystemExit("build_cad.py: CMake %s is too old; 3.18 or newer is needed; nothing was downloaded" % version)
        cmake = pathlib.Path(found) if found else None
    BASE.mkdir(parents=True, exist_ok=True)
    for name, (ver, url, sha) in ((("cmake", CMAKE),) if darwin else ()) + (("occt", OCCT), ("manifold", MANIFOLD)):
        archive = BASE / ("%s-%s.tar.gz" % (name, ver))
        sums[name] = fetch(name, url, sha, archive)
    if darwin:
        cmake = unpack(BASE / ("cmake-%s.tar.gz" % CMAKE[0]), BASE) / "CMake.app" / "Contents" / "bin" / "cmake"
    occt_src = unpack(BASE / ("occt-%s.tar.gz" % OCCT[0]), BASE)
    manifold_src = unpack(BASE / ("manifold-%s.tar.gz" % MANIFOLD[0]), BASE)
    print("sources: %s, %s; cmake %s" % (occt_src, manifold_src, cmake))
    if fetch_only:
        return 0
    build("Manifold %s" % MANIFOLD[0], cmake, manifold_src, BASE / "manifold-build", BASE / ("manifold-%s" % MANIFOLD[0]), [
        "-DBUILD_SHARED_LIBS=OFF", "-DMANIFOLD_TEST=OFF", "-DMANIFOLD_CROSS_SECTION=OFF", "-DMANIFOLD_PAR=OFF",
        "-DMANIFOLD_PYBIND=OFF", "-DMANIFOLD_CBIND=OFF", "-DMANIFOLD_JSBIND=OFF", "-DMANIFOLD_EXPORT=OFF",
        "-DMANIFOLD_DEBUG=OFF"])
    build("Open CASCADE %s" % OCCT[0], cmake, occt_src, BASE / "occt-build", BASE / ("occt-%s" % OCCT[0]), [
        "-DBUILD_LIBRARY_TYPE=Static", "-DINSTALL_DIR=" + str(BASE / ("occt-%s" % OCCT[0])), "-DBUILD_CPP_STANDARD=C++17",
        "-DBUILD_MODULE_FoundationClasses=ON", "-DBUILD_MODULE_ModelingData=ON", "-DBUILD_MODULE_ModelingAlgorithms=ON",
        "-DBUILD_MODULE_DataExchange=ON", "-DBUILD_MODULE_ApplicationFramework=ON", "-DBUILD_MODULE_Visualization=OFF",
        "-DBUILD_MODULE_Draw=OFF", "-DBUILD_MODULE_DETools=OFF", "-DBUILD_DOC_Overview=OFF", "-DBUILD_SAMPLES_QT=OFF",
        "-DUSE_FREETYPE=OFF", "-DUSE_TK=OFF", "-DUSE_TCL=OFF", "-DUSE_OPENGL=OFF", "-DUSE_GLES2=OFF",
        "-DUSE_FREEIMAGE=OFF", "-DUSE_FFMPEG=OFF", "-DUSE_OPENVR=OFF", "-DUSE_RAPIDJSON=OFF", "-DUSE_DRACO=OFF",
        "-DUSE_TBB=OFF", "-DUSE_VTK=OFF"] + ([] if darwin else ["-DUSE_XLIB=OFF"]))  # Linux: no X11 (a headless node)
    for name in sums:
        print("SHA-256 %s %s" % (name, sums[name]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
