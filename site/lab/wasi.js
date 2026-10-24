// wasi.js - the few WASI (preview 1) calls the lab makes, over files held in memory: nothing is read from or written
// to the visitor's disk, and nothing leaves the page. The lab (build/web/lab.wasm, made by tools/web_build.py) needs
// only these thirteen: files opened by path under one root, read, written, sought and closed; standard output and
// error; the clock; an empty environment; and exit. Anything else it asked for would fail to link, so the list is
// checked when the module is instantiated. Written for the lab rather than taken from a general shim, because a result
// is tens of megabytes written in small pieces, and a file here grows by doubling, not by a copy per write.
// The same file runs in a browser worker and under Node (tools/web_build.py --check).

const ERRNO = { SUCCESS: 0, BADF: 8, EXIST: 20, INVAL: 28, IO: 29, ISDIR: 31, NOENT: 44, NOSYS: 52, NOTDIR: 54 };
const FILETYPE = { CHARACTER_DEVICE: 2, DIRECTORY: 3, REGULAR_FILE: 4 };
const OFLAGS = { CREAT: 1, DIRECTORY: 2, EXCL: 4, TRUNC: 8 };
const FDFLAGS_APPEND = 1;
const RIGHTS_ALL = 0x1fffffffn;

export class ProcExit extends Error {
  constructor(code) {
    super("exit " + code);
    this.code = code;
  }
}

// A file: its bytes and its length; the bytes array is the capacity, doubled when a write needs more.
class MemFile {
  constructor(bytes) {
    this.data = bytes ? new Uint8Array(bytes) : new Uint8Array(0);
    this.size = this.data.length;
  }
  reserve(n) {
    if (n <= this.data.length) return;
    const grown = new Uint8Array(Math.max(n, 2 * this.data.length, 65536));
    grown.set(this.data.subarray(0, this.size));
    this.data = grown;
  }
  bytes() {
    return this.data.subarray(0, this.size);
  }
}

// The file system: a map from a path under the root ("lab/result.lab") to its file. Directories are implied.
export class MemFS {
  constructor() {
    this.files = new Map();
  }
  write(path, bytes) {
    this.files.set(clean(path), new MemFile(bytes));
  }
  read(path) {
    const f = this.files.get(clean(path));
    return f ? f.bytes() : null;
  }
  remove(path) {
    this.files.delete(clean(path));
  }
}

function clean(path) {
  return path.replace(/^\/+/, "").replace(/\/+/g, "/");
}

// The imports for a module (wasi_snapshot_preview1), over fs. args: the command line (labrun.wasm only);
// out(text) and err(text): what the program writes to standard output and error, as it writes it.
export function wasi(fs, { args = [], out = () => {}, err = () => {} } = {}) {
  let memory = null;
  const fds = new Map([
    [0, { kind: "stdin" }],
    [1, { kind: "out", sink: out, text: new TextDecoder() }],
    [2, { kind: "out", sink: err, text: new TextDecoder() }],
    [3, { kind: "root" }],
  ]);
  let nextFd = 4;
  const view = () => new DataView(memory.buffer);
  const bytes = () => new Uint8Array(memory.buffer);
  const decoder = new TextDecoder();
  const encoder = new TextEncoder();
  const argBytes = args.map((a) => encoder.encode(a + "\0"));

  const iovecs = (ptr, n) => {
    const v = view(), list = [];
    for (let i = 0; i < n; i++) list.push([v.getUint32(ptr + 8 * i, true), v.getUint32(ptr + 8 * i + 4, true)]);
    return list;
  };

  const imports = {
    args_sizes_get(countPtr, sizePtr) {
      const v = view();
      v.setUint32(countPtr, argBytes.length, true);
      v.setUint32(sizePtr, argBytes.reduce((s, a) => s + a.length, 0), true);
      return ERRNO.SUCCESS;
    },
    args_get(argvPtr, bufPtr) {
      const v = view(), b = bytes();
      for (let i = 0; i < argBytes.length; i++) {
        v.setUint32(argvPtr + 4 * i, bufPtr, true);
        b.set(argBytes[i], bufPtr);
        bufPtr += argBytes[i].length;
      }
      return ERRNO.SUCCESS;
    },
    environ_sizes_get(countPtr, sizePtr) {
      const v = view();
      v.setUint32(countPtr, 0, true);
      v.setUint32(sizePtr, 0, true);
      return ERRNO.SUCCESS;
    },
    environ_get() {
      return ERRNO.SUCCESS;
    },
    clock_time_get(id, precision, timePtr) {
      // realtime (0) from the date; the monotonic and process clocks (1, 2, 3) from the page's own clock
      const ns = id === 0 ? BigInt(Date.now()) * 1000000n : BigInt(Math.round(performance.now() * 1e6));
      view().setBigUint64(timePtr, ns, true);
      return ERRNO.SUCCESS;
    },
    fd_prestat_get(fd, ptr) {
      if (fd !== 3) return ERRNO.BADF;
      const v = view();
      v.setUint8(ptr, 0); // a directory
      v.setUint32(ptr + 4, 1, true); // its name, "/", is one byte
      return ERRNO.SUCCESS;
    },
    fd_prestat_dir_name(fd, ptr, len) {
      if (fd !== 3) return ERRNO.BADF;
      if (len >= 1) bytes()[ptr] = 47; // "/"
      return ERRNO.SUCCESS;
    },
    fd_fdstat_get(fd, ptr) {
      const f = fds.get(fd);
      if (!f) return ERRNO.BADF;
      const v = view();
      const type = f.kind === "root" ? FILETYPE.DIRECTORY : f.kind === "file" ? FILETYPE.REGULAR_FILE : FILETYPE.CHARACTER_DEVICE;
      v.setUint8(ptr, type);
      v.setUint16(ptr + 2, f.append ? FDFLAGS_APPEND : 0, true);
      v.setBigUint64(ptr + 8, RIGHTS_ALL, true);
      v.setBigUint64(ptr + 16, RIGHTS_ALL, true);
      return ERRNO.SUCCESS;
    },
    fd_fdstat_set_flags(fd, flags) {
      const f = fds.get(fd);
      if (!f) return ERRNO.BADF;
      f.append = (flags & FDFLAGS_APPEND) !== 0;
      return ERRNO.SUCCESS;
    },
    path_open(dirfd, dirflags, pathPtr, pathLen, oflags, rightsBase, rightsInheriting, fdflags, fdPtr) {
      if (dirfd !== 3) return ERRNO.BADF;
      const path = clean(decoder.decode(bytes().slice(pathPtr, pathPtr + pathLen)));
      if (oflags & OFLAGS.DIRECTORY) {
        // only the root, and the directories a file's path implies, exist
        const prefix = path === "" || path === "." ? "" : path + "/";
        if (prefix && ![...fs.files.keys()].some((k) => k.startsWith(prefix))) return ERRNO.NOENT;
        fds.set(nextFd, { kind: "dir" });
      } else {
        let file = fs.files.get(path);
        if (!file) {
          if (!(oflags & OFLAGS.CREAT)) return ERRNO.NOENT;
          file = new MemFile();
          fs.files.set(path, file);
        } else if (oflags & OFLAGS.EXCL) return ERRNO.EXIST;
        if (oflags & OFLAGS.TRUNC) file.size = 0;
        fds.set(nextFd, { kind: "file", file, pos: 0, append: (fdflags & FDFLAGS_APPEND) !== 0 });
      }
      view().setUint32(fdPtr, nextFd, true);
      nextFd++;
      return ERRNO.SUCCESS;
    },
    fd_close(fd) {
      if (!fds.has(fd) || fd < 4) return fds.has(fd) ? ERRNO.SUCCESS : ERRNO.BADF;
      fds.delete(fd);
      return ERRNO.SUCCESS;
    },
    fd_read(fd, iovsPtr, iovsLen, nreadPtr) {
      const f = fds.get(fd);
      if (!f) return ERRNO.BADF;
      let n = 0;
      if (f.kind === "file") {
        const b = bytes();
        for (const [ptr, len] of iovecs(iovsPtr, iovsLen)) {
          const take = Math.max(0, Math.min(len, f.file.size - f.pos));
          b.set(f.file.data.subarray(f.pos, f.pos + take), ptr);
          f.pos += take;
          n += take;
          if (take < len) break;
        }
      } else if (f.kind !== "stdin") return ERRNO.BADF;
      view().setUint32(nreadPtr, n, true);
      return ERRNO.SUCCESS;
    },
    fd_write(fd, iovsPtr, iovsLen, nwrittenPtr) {
      const f = fds.get(fd);
      if (!f) return ERRNO.BADF;
      const list = iovecs(iovsPtr, iovsLen);
      const b = bytes();
      let n = 0;
      if (f.kind === "out") {
        for (const [ptr, len] of list) {
          f.sink(f.text.decode(b.subarray(ptr, ptr + len), { stream: true }));
          n += len;
        }
      } else if (f.kind === "file") {
        if (f.append) f.pos = f.file.size;
        const total = list.reduce((s, [, len]) => s + len, 0);
        f.file.reserve(f.pos + total);
        for (const [ptr, len] of list) {
          f.file.data.set(b.subarray(ptr, ptr + len), f.pos);
          f.pos += len;
        }
        if (f.pos > f.file.size) f.file.size = f.pos;
        n = total;
      } else return ERRNO.BADF;
      view().setUint32(nwrittenPtr, n, true);
      return ERRNO.SUCCESS;
    },
    fd_seek(fd, offset, whence, newPtr) {
      const f = fds.get(fd);
      if (!f || f.kind !== "file") return ERRNO.BADF;
      const base = whence === 0 ? 0 : whence === 1 ? f.pos : whence === 2 ? f.file.size : NaN;
      const to = base + Number(offset);
      if (!(to >= 0)) return ERRNO.INVAL;
      f.pos = to;
      view().setBigUint64(newPtr, BigInt(to), true);
      return ERRNO.SUCCESS;
    },
    proc_exit(code) {
      throw new ProcExit(code);
    },
  };

  return {
    imports,
    // the module's memory, once it is instantiated
    bind(instance) {
      memory = instance.exports.memory;
    },
  };
}

// Every import a module asks for must be one of these; otherwise say which are missing (a lab built with a call
// this file does not provide would fail at the first use, far from the cause).
export function missingImports(module) {
  const have = new Set(Object.keys(wasi(new MemFS()).imports));
  return WebAssembly.Module.imports(module)
    .filter((i) => i.module === "wasi_snapshot_preview1" && !have.has(i.name))
    .map((i) => i.name);
}
