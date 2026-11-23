import os
import sys
import tempfile
import time
import unittest

import sandbox


def _setsid():
    os.setsid()


def _alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    # a zombie (killed, not yet reaped by its parent) counts as gone
    try:
        with open(f"/proc/{pid}/stat") as f:
            return f.read().split(") ")[1][0] != "Z"
    except OSError:
        return True


def _wait_gone(pid: int, seconds: float = 3.0) -> bool:
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if not _alive(pid):
            return True
        time.sleep(0.05)
    return False


class SandboxTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        sandbox.JOBS_DIR = os.path.join(self.dir, "jobs")

    def py(self, code: str, timeout: float = 5.0, cap: int = 1024):
        return sandbox.run([sys.executable, "-c", code], b"", self.dir, dict(os.environ), _setsid, timeout, cap)

    def test_output_is_capped_while_it_runs(self):
        r = self.py("import sys\nfor _ in range(2000): sys.stdout.write('x' * 1000)\nsys.stderr.write('e')")
        self.assertEqual(len(r.stdout), 1024)
        self.assertTrue(r.stdout_truncated)
        self.assertEqual(r.stderr, b"e")
        self.assertFalse(r.stderr_truncated)
        self.assertEqual(r.returncode, 0)

    def test_stdin_and_exit_code(self):
        r = sandbox.run([sys.executable, "-c", "import sys; print(sys.stdin.read().upper()); sys.exit(3)"],
                        b"hi", self.dir, dict(os.environ), _setsid, 5.0, 1024)
        self.assertEqual(r.stdout.strip(), b"HI")
        self.assertEqual(r.returncode, 3)

    def test_timeout_kills_the_whole_group(self):
        pidfile = os.path.join(self.dir, "child")
        code = ("import os, time\n"
                "pid = os.fork()\n"
                "if pid == 0:\n"
                "    time.sleep(60)\n"
                f"open({pidfile!r}, 'w').write(str(pid))\n"
                "time.sleep(60)\n")
        r = self.py(code, timeout=1.0)
        self.assertTrue(r.timed_out)
        self.assertIsNone(r.returncode)
        child = int(open(pidfile).read())
        self.assertTrue(_wait_gone(child), "the forked child outlived the timeout")

    def test_children_left_after_a_normal_exit_are_killed(self):
        pidfile = os.path.join(self.dir, "child")
        code = ("import os, time\n"
                "pid = os.fork()\n"
                "if pid == 0:\n"
                "    os.close(1); os.close(2)\n"
                "    time.sleep(60)\n"
                f"open({pidfile!r}, 'w').write(str(pid))\n")
        r = self.py(code)
        self.assertFalse(r.timed_out)
        child = int(open(pidfile).read())
        self.assertTrue(_wait_gone(child), "a background child outlived its job")

    @unittest.skipUnless(os.path.isdir("/proc/self"), "the sweep reads /proc")
    def test_sweep_kills_a_process_that_left_the_group(self):
        pidfile = os.path.join(self.dir, "daemon")
        code = ("import os, time\n"
                "if os.fork() == 0:\n"
                "    os.setsid()\n"
                "    os.close(1); os.close(2)\n"
                f"    open({pidfile!r}, 'w').write(str(os.getpid()))\n"
                "    time.sleep(60)\n"
                "    os._exit(0)\n"
                "time.sleep(0.3)\n")
        self.py(code)
        daemon = int(open(pidfile).read())
        self.assertTrue(_alive(daemon), "too young for the sweep yet")
        time.sleep(2.2)
        sandbox.sweep([daemon])
        self.assertTrue(_wait_gone(daemon), "a daemon in a session of its own survived the sweep")

    def test_network_check(self):
        os.environ.pop("ALLOW_NETWORK", None)
        ifaces = sandbox.network_interfaces()
        self.assertEqual(sandbox.network_isolated(), ifaces is None or not ifaces)
        os.environ["ALLOW_NETWORK"] = "1"
        try:
            self.assertTrue(sandbox.network_isolated())
        finally:
            del os.environ["ALLOW_NETWORK"]


if __name__ == "__main__":
    unittest.main()
