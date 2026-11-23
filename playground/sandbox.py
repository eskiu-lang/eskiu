"""How the playground runs one untrusted program: output read with a cap while it runs, the
whole process tree killed when it ends, stray processes swept, and no network.

- Output: stdout and stderr are read as they arrive and kept up to a cap per stream; the rest
  is drained and dropped, so a program that prints without end cannot fill the API's memory.
- Processes: the job runs in its own session (setsid in the child). When it ends, times out
  or fails, its whole process group is killed. A process that left the group (a new session,
  as a daemon makes) is caught by the sweep: every process of this user whose session is
  neither the server's nor a running job's is killed after each job. Jobs register their
  session in JOBS_DIR so the server's workers do not kill each other's jobs.
- Network: on Linux the API refuses to run programs when the container has any network
  interface besides loopback (run it with `--network none`; see README.md). ALLOW_NETWORK=1
  turns the check off, for local development only.
"""

import os
import selectors
import signal
import subprocess
import time

JOBS_DIR = os.environ.get("JOBS_DIR", "/tmp/eskiu-jobs")


def network_interfaces() -> list[str] | None:
    """The network interfaces other than loopback, or None where that cannot be told (not Linux)."""
    try:
        names = os.listdir("/sys/class/net")
    except OSError:
        return None
    return sorted(n for n in names if n != "lo")


def network_isolated() -> bool:
    """Whether programs run with no network: no interface but loopback (or the check is off)."""
    if os.environ.get("ALLOW_NETWORK") == "1":
        return True
    ifaces = network_interfaces()
    return ifaces is None or not ifaces


def _register(sid: int) -> str | None:
    try:
        os.makedirs(JOBS_DIR, exist_ok=True)
        path = os.path.join(JOBS_DIR, str(sid))
        with open(path, "w"):
            pass
        return path
    except OSError:
        return None


def _kill_group(pgid: int) -> None:
    try:
        os.killpg(pgid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass


def sweep(candidates=None) -> int:
    """Kills every process of this user outside the server's session and the running jobs'
    (older than two seconds: a job another worker has just started may not be registered yet).
    Returns how many it signalled. It runs only where SANDBOX_SWEEP=1 (the image sets it: the
    container's user runs nothing else), or on the given candidate pids (tests); a no-op where
    /proc is missing."""
    if candidates is None and os.environ.get("SANDBOX_SWEEP") != "1":
        return 0
    try:
        pids = list(candidates) if candidates is not None else [int(p) for p in os.listdir("/proc") if p.isdigit()]
    except OSError:
        return 0
    me = os.getuid()
    own_sid = os.getsid(0)
    try:
        with open("/proc/uptime") as f:
            uptime = float(f.read().split()[0])
        ticks = os.sysconf("SC_CLK_TCK")
    except (OSError, ValueError):
        uptime, ticks = 0.0, 100
    try:
        live = {int(n) for n in os.listdir(JOBS_DIR) if n.isdigit()}
    except OSError:
        live = set()
    killed = 0
    for pid in pids:
        try:
            if os.stat(f"/proc/{pid}").st_uid != me:
                continue
            with open(f"/proc/{pid}/stat") as f:
                stat = f.read()
            # pid (comm) state ppid pgrp session ...: comm may hold spaces and parentheses
            fields = stat[stat.rindex(")") + 2:].split()
            sid = int(fields[3])
            age = uptime - int(fields[19]) / ticks
        except (OSError, ValueError, IndexError):
            continue
        # a job another worker has just started may not be registered yet
        if sid == own_sid or sid in live or pid == os.getpid() or age < 2.0:
            continue
        try:
            os.kill(pid, signal.SIGKILL)
            killed += 1
        except (ProcessLookupError, PermissionError):
            pass
    return killed


class Result:
    def __init__(self):
        self.stdout = b""
        self.stderr = b""
        self.truncated = False
        self.stdout_truncated = False
        self.stderr_truncated = False
        self.timed_out = False
        self.returncode: int | None = None


def run(cmd: list[str], stdin: bytes, cwd: str, env: dict, preexec, timeout: float, cap: int) -> Result:
    """Runs cmd with preexec (which must call os.setsid) and returns what it printed, at most
    cap bytes per stream, whether more was dropped, and whether it timed out."""
    r = Result()
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            cwd=cwd, env=env, preexec_fn=preexec)
    job = _register(proc.pid)
    try:
        try:
            proc.stdin.write(stdin)
        except (BrokenPipeError, OSError):
            pass
        try:
            proc.stdin.close()
        except OSError:
            pass
        bufs = {proc.stdout.fileno(): bytearray(), proc.stderr.fileno(): bytearray()}
        cut = {fd: False for fd in bufs}
        sel = selectors.DefaultSelector()
        for fd in bufs:
            os.set_blocking(fd, False)
            sel.register(fd, selectors.EVENT_READ)
        deadline = time.monotonic() + timeout
        while sel.get_map():
            left = deadline - time.monotonic()
            if left <= 0:
                r.timed_out = True
                break
            for key, _ in sel.select(timeout=left):
                try:
                    chunk = os.read(key.fd, 65536)
                except BlockingIOError:
                    continue
                if not chunk:
                    sel.unregister(key.fd)
                    continue
                buf = bufs[key.fd]
                room = cap - len(buf)
                if room > 0:
                    buf += chunk[:room]
                if len(chunk) > room:
                    cut[key.fd] = True
        sel.close()
        if not r.timed_out:
            try:
                proc.wait(timeout=max(0.0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                r.timed_out = True
        r.stdout = bytes(bufs[proc.stdout.fileno()])
        r.stderr = bytes(bufs[proc.stderr.fileno()])
        r.stdout_truncated = cut[proc.stdout.fileno()]
        r.stderr_truncated = cut[proc.stderr.fileno()]
        r.truncated = r.stdout_truncated or r.stderr_truncated
    finally:
        _kill_group(proc.pid)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        for f in (proc.stdout, proc.stderr):
            try:
                f.close()
            except OSError:
                pass
        if job:
            try:
                os.remove(job)
            except OSError:
                pass
        sweep()
    r.returncode = None if r.timed_out else proc.returncode
    return r
