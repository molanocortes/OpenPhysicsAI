#!/usr/bin/env python3
"""Fetch and build the pinned serial, double-precision MFEM dependency without Homebrew."""
import hashlib
import os
from pathlib import Path
import platform
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
VERSION = '4.8'
SHA256 = '65472f732d273832c64b2c39460649dd862df674222c71bfa82cf2da76705052'
URL = f'https://codeload.github.com/mfem/mfem/tar.gz/refs/tags/v{VERSION}'
base = ROOT / 'build/third_party'
base.mkdir(parents=True, exist_ok=True)
archive = base / f'mfem-{VERSION}.tar.gz'
if not archive.exists():
    temporary = archive.with_suffix('.download')
    with urllib.request.urlopen(URL, timeout=120) as response, temporary.open('wb') as dst:
        while chunk := response.read(1024 * 1024):
            dst.write(chunk)
    temporary.replace(archive)
if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
    raise SystemExit('MFEM archive checksum mismatch; refusing to build')
source = base / f'mfem-{VERSION}'
if not source.exists():
    with tarfile.open(archive) as tar:
        for member in tar.getmembers():
            target = (base / member.name).resolve()
            if not target.is_relative_to(source.resolve()) or not (member.isdir() or member.isfile()):
                raise SystemExit(f'Unsafe MFEM archive member: {member.name}')
        tar.extractall(base)
library = source / 'libmfem.a'
stamp = source / '.navier-built'
flags = ['-O2', '-std=c++11', '-ffp-contract=off']
if platform.system() == 'Darwin':
    sdk = subprocess.check_output(['xcrun', '--show-sdk-path'], text=True).strip()
    # Some CLT installations put libc++ headers only in the SDK.
    headers = Path(sdk) / 'usr/include/c++/v1'
    if headers.is_dir():
        flags += ['-isystem', str(headers)]
if library.exists() and stamp.exists() and stamp.read_text() == ' '.join(flags):
    print(f'Pinned MFEM {VERSION} already built: {library}')
    raise SystemExit(0)
subprocess.run(['make', 'config', 'CXX='+os.environ.get('CXX', 'clang++'),
                'MFEM_PRECISION=double', 'MFEM_USE_MPI=NO', 'MFEM_USE_METIS=NO',
                'CXXFLAGS='+' '.join(flags)], cwd=source, check=True)
subprocess.run(['make', '-j2'], cwd=source, check=True)
stamp.write_text(' '.join(flags))
print(f'Pinned MFEM {VERSION}, double precision: {source / "libmfem.a"}')
