"""
Eskiu playground API.

A single endpoint, POST /run, that compiles and runs a snippet of Eskiu with
`eskiuc run` and returns its stdout/stderr/exit code. Everything runs inside
the container as an unprivileged user, under a wall-clock timeout and POSIX
resource limits (CPU, address space, file size, processes). Output is capped.

This is deliberately small so it is easy to audit. See README.md for the
threat model and deployment notes.
"""

import os
import resource
import shutil
import subprocess
import tempfile
import time
from collections import deque

from fastapi import FastAPI, Request
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field

from client_ip import client_ip, parse_trusted_proxies

# ── Configuration (override via environment) ────────────────────────────────
ESKIUC = os.environ.get("ESKIUC", "/opt/eskiu/bin/eskiuc")
# eskiuc resolves the stdlib from $ESKIU_ROOT (dir containing stdlib/).
ESKIU_ROOT = os.environ.get("ESKIU_ROOT", "/opt/eskiu")

MAX_CODE_BYTES = int(os.environ.get("MAX_CODE_BYTES", 64 * 1024))   # 64 KiB
MAX_OUTPUT_BYTES = int(os.environ.get("MAX_OUTPUT_BYTES", 64 * 1024))  # per stream
WALL_TIMEOUT = float(os.environ.get("WALL_TIMEOUT", 15))           # seconds
CPU_SECONDS = int(os.environ.get("CPU_SECONDS", 12))              # RLIMIT_CPU
MEM_BYTES = int(os.environ.get("MEM_BYTES", 1024 * 1024 * 1024))  # RLIMIT_AS (1 GiB; LLVM is hungry)
FSIZE_BYTES = int(os.environ.get("FSIZE_BYTES", 64 * 1024 * 1024))  # RLIMIT_FSIZE
NPROC = int(os.environ.get("NPROC", 96))                          # RLIMIT_NPROC (clang spawns children)

# Comma-separated list of allowed CORS origins, or "*".
ALLOWED_ORIGINS = os.environ.get(
    "ALLOWED_ORIGINS",
    "https://eskiu-lang.org,https://www.eskiu-lang.org,http://localhost:8000",
).split(",")

# Only these immediate proxy peers may supply X-Forwarded-For. CIDRs are accepted.
TRUSTED_PROXIES = parse_trusted_proxies(
    os.environ.get("TRUSTED_PROXIES", "127.0.0.1/32,::1/128")
)

# Simple per-IP rate limit: REQUESTS requests per WINDOW seconds.
RATE_REQUESTS = int(os.environ.get("RATE_REQUESTS", 20))
RATE_WINDOW = float(os.environ.get("RATE_WINDOW", 60))

app = FastAPI(title="Eskiu playground", docs_url=None, redoc_url=None)
app.add_middleware(
    CORSMiddleware,
    allow_origins=ALLOWED_ORIGINS,
    allow_methods=["POST", "GET"],
    allow_headers=["Content-Type"],
)

_hits: dict[str, deque] = {}


def _rate_limited(ip: str) -> bool:
    now = time.monotonic()
    dq = _hits.setdefault(ip, deque())
    while dq and now - dq[0] > RATE_WINDOW:
        dq.popleft()
    if len(dq) >= RATE_REQUESTS:
        return True
    dq.append(now)
    return False


class RunRequest(BaseModel):
    code: str = Field(..., max_length=MAX_CODE_BYTES * 4)  # generous char cap; bytes checked below
    stdin: str = Field("", max_length=16 * 1024)


class RunResponse(BaseModel):
    ok: bool
    exit_code: int | None = None
    stdout: str = ""
    stderr: str = ""
    timed_out: bool = False
    truncated: bool = False
    duration_ms: int = 0
    error: str | None = None


def _set_limits():
    """Applied in the child before exec. Caps CPU, memory, file size, procs."""
    resource.setrlimit(resource.RLIMIT_CPU, (CPU_SECONDS, CPU_SECONDS))
    resource.setrlimit(resource.RLIMIT_AS, (MEM_BYTES, MEM_BYTES))
    resource.setrlimit(resource.RLIMIT_FSIZE, (FSIZE_BYTES, FSIZE_BYTES))
    try:
        resource.setrlimit(resource.RLIMIT_NPROC, (NPROC, NPROC))
    except (ValueError, OSError):
        pass  # not fatal if the host disallows it
    os.setsid()  # own process group so we can kill the whole tree on timeout


def _truncate(b: bytes) -> tuple[str, bool]:
    if len(b) > MAX_OUTPUT_BYTES:
        return b[:MAX_OUTPUT_BYTES].decode("utf-8", "replace") + "\n…[output truncated]", True
    return b.decode("utf-8", "replace"), False


@app.get("/healthz")
def healthz():
    return {"ok": os.path.exists(ESKIUC)}


@app.post("/run", response_model=RunResponse)
def run(req: RunRequest, request: Request):
    peer = request.client.host if request.client else "?"
    ip = client_ip(peer, request.headers.get("x-forwarded-for"), TRUSTED_PROXIES)
    if _rate_limited(ip):
        return RunResponse(ok=False, error="Rate limit exceeded. Slow down a moment.")

    code_bytes = req.code.encode("utf-8")
    if len(code_bytes) > MAX_CODE_BYTES:
        return RunResponse(ok=False, error=f"Source too large (max {MAX_CODE_BYTES} bytes).")
    if not code_bytes.strip():
        return RunResponse(ok=False, error="Empty source.")

    workdir = tempfile.mkdtemp(prefix="esk-")
    src = os.path.join(workdir, "main.esk")
    try:
        with open(src, "w") as f:
            f.write(req.code)

        env = {
            "ESKIU_ROOT": ESKIU_ROOT,
            "PATH": "/usr/local/bin:/usr/bin:/bin",
            "HOME": workdir,
            "TMPDIR": workdir,
        }
        started = time.monotonic()
        try:
            proc = subprocess.run(
                [ESKIUC, "run", src],
                input=req.stdin.encode("utf-8"),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                cwd=workdir,
                env=env,
                preexec_fn=_set_limits,
                timeout=WALL_TIMEOUT,
            )
        except subprocess.TimeoutExpired as e:
            out, t1 = _truncate(e.stdout or b"")
            err, t2 = _truncate(e.stderr or b"")
            return RunResponse(
                ok=True, timed_out=True, stdout=out, stderr=err,
                truncated=t1 or t2,
                duration_ms=int((time.monotonic() - started) * 1000),
                error=f"Timed out after {WALL_TIMEOUT:g}s.",
            )

        out, t1 = _truncate(proc.stdout)
        err, t2 = _truncate(proc.stderr)
        return RunResponse(
            ok=True,
            exit_code=proc.returncode,
            stdout=out,
            stderr=err,
            truncated=t1 or t2,
            duration_ms=int((time.monotonic() - started) * 1000),
        )
    except Exception as e:  # noqa: BLE001 — surface as a clean API error
        return RunResponse(ok=False, error=f"Internal error: {type(e).__name__}")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
