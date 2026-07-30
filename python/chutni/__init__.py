"""Thin, stdlib-only binding for libchutni's JSON call surface."""
import ctypes
import json
import os
from pathlib import Path


class ChutniError(RuntimeError):
    def __init__(self, code, message):
        self.code, self.message = code, message
        super().__init__(f"{code}: {message}")


class BusyError(ChutniError):
    pass


def _library():
    names = ("libchutni.dylib", "libchutni.so")
    candidates = [os.environ.get("CHUTNI_LIBRARY")]
    candidates += [str(Path(p) / n) for p in (os.environ.get("CHUTNI_PREFIX", "/usr/local") + "/lib",) for n in names]
    candidates += [str(Path(__file__).resolve().parents[2] / "build" / n) for n in names]
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            return ctypes.CDLL(candidate)
    raise OSError("libchutni not found; set CHUTNI_LIBRARY or run make")


_lib = _library()
_store_p = ctypes.c_void_p
_lib.chutni_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(_store_p)]
_lib.chutni_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(_store_p)]
_lib.chutni_close.argtypes = [_store_p]
_lib.chutni_call.argtypes = [_store_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
_lib.chutni_free.argtypes = [ctypes.c_void_p]


def _text(value): return os.fsencode(os.path.expanduser(os.fspath(value)))
def _args(value): return json.dumps(value or {}, separators=(",", ":")).encode()

def _call(handle, operation, arguments=None):
    out = ctypes.c_void_p()
    status = _lib.chutni_call(handle, operation.encode(), _args(arguments), ctypes.byref(out))
    raw = ctypes.string_at(out).decode() if out.value else "{}"
    if out.value: _lib.chutni_free(out)
    result = json.loads(raw)
    if status:
        error = result.get("error", {})
        cls = BusyError if error.get("code") == "busy" else ChutniError
        raise cls(error.get("code", "chutni error"), error.get("message", raw))
    return result


def discover(): return _call(None, "discover")["stores"]


class Store:
    def __init__(self, handle): self._handle = handle
    @classmethod
    def create(cls, path, label=None):
        handle = _store_p(); status = _lib.chutni_create(_text(path), _text(label) if label else None, ctypes.byref(handle))
        if status: raise ChutniError("create failed", os.fspath(path))
        return cls(handle)
    @classmethod
    def open(cls, path, read_only=False):
        handle = _store_p(); status = _lib.chutni_open(_text(path), int(read_only), ctypes.byref(handle))
        if status: raise ChutniError("open failed", os.fspath(path))
        return cls(handle)
    def close(self):
        if self._handle: _lib.chutni_close(self._handle); self._handle = None
    def __enter__(self): return self
    def __exit__(self, *_): self.close()
    def call(self, name, **arguments): return _call(self._handle, name, arguments)
    def add_root(self, path, label=None, **policy): return self.call("add_root", path=os.fspath(path), label=label, policy=policy)
    def scan(self, **kw): return self.call("scan", **kw)
    def observe(self, source): return self.call("observe_directory", source_path=os.fspath(source))
    def children(self, source): return self.call("children", source_path=os.fspath(source))["children"]
    def coverage(self, id=None): return self.call("coverage", **({"source_id": id} if id else {}))
    def search(self, query, **kw): return self.call("search", query=query, **kw)["results"]
    def search_semantic(self, vector, profile, **kw): return self.call("search_semantic", vector=vector, profile=profile, **kw)["results"]
    def inspect(self, source): return self.call("source_context", source_path=os.fspath(source))
    def verify(self, source=None): return self.call("check_freshness", **({"source_path": os.fspath(source)} if source else {}))
    def put_artifacts(self, producer, operation, artifacts, input_refs=None, **kw): return self.call("put_artifacts", producer=producer, operation=operation, artifacts=artifacts, inputs=input_refs or [], **kw)
    def put_memory(self, memory_kind, text, producer, operation, inputs=None, **kw): return self.call("put_memory", memory_kind=memory_kind, text=text, producer=producer, operation=operation, inputs=inputs or [], **kw)
    def memory(self, memory_id): return self.call("source_context", source_id=memory_id)
    def put_representation(self, artifact_id, profile, vector): return self.call("put_representation", artifact_id=artifact_id, profile=profile, vector=vector)
    def forget(self, source, mode="catalog_only"): return self.call("forget_source", source_path=os.fspath(source), mode=mode)
