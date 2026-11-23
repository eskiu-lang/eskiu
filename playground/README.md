# Eskiu playground API

A tiny FastAPI service that powers the **Try it!** section on the website. One
endpoint, `POST /run`, compiles and runs an Eskiu snippet with `eskiuc run`
inside the container and returns its output.

```
POST /run
{ "code": "extern int printf(string fmt, ...); int main(){ printf(\"hi\\n\"); return 0; }",
  "stdin": "" }

200 OK
{ "ok": true, "exit_code": 0, "stdout": "hi\n", "stderr": "",
  "timed_out": false, "truncated": false, "duration_ms": 412 }
```

`GET /healthz` → `{ "ok": true }` once the compiler is present.

## The compiler

The image downloads the Linux x86-64 release of `eskiuc` during the build and checks it
against the release's `SHA256SUMS`. The version is a build argument, `ESKIU_VERSION`, whose
default in the `Dockerfile` tracks the latest release. The release binary statically links
LLVM, so the image only needs a few shared libs and `gcc` as the C linker for `eskiuc run`.

## Build

The build context is **this directory** (`playground/`):

```bash
docker build -t eskiu-playground .
docker build --build-arg ESKIU_VERSION=0.16.2 -t eskiu-playground .   # a specific release
```

## Run on the VPS

The container runs with **no network** (`--network none`): user programs cannot reach the
internet, the host or anything else. The API listens on a Unix socket in a directory the
host mounts, and the reverse proxy forwards to that socket.

```bash
sudo install -d -o 10001 -g 10001 -m 0755 /run/eskiu-playground
docker run -d --name eskiu-playground --restart unless-stopped \
  --network none \
  -v /run/eskiu-playground:/run/eskiu-playground \
  -e ALLOWED_ORIGINS="https://eskiu-lang.org,https://www.eskiu-lang.org" \
  --init \
  --read-only \
  --tmpfs /tmp:size=512m,exec \
  --tmpfs /home/runner:size=64m \
  --security-opt no-new-privileges \
  --cap-drop ALL \
  --pids-limit 512 \
  --memory 2g --cpus 2 \
  eskiu-playground
```

Notes:
- `--network none` is required: the API refuses to run code (`/run` answers an error,
  `/healthz` reports `"network_isolated": false`) while the container has any network
  interface besides loopback. `ALLOW_NETWORK=1` turns that check off, for local
  development only.
- `--init` reaps the processes the API kills; without it they stay as zombies.
- `--tmpfs /tmp:...,exec` is required: `eskiuc run` links and executes a
  temporary binary under `$TMPDIR`, which the app points at `/tmp`.
- `/run/eskiu-playground` does not survive a reboot on most systems: create it again
  at boot (a `tmpfiles.d` entry: `d /run/eskiu-playground 0755 10001 10001 -`).
- Let your reverse proxy (nginx/Caddy/Traefik) terminate TLS and forward to the socket.
  Then set `PLAYGROUND_API` in `site/home.js` to the public URL.

Example nginx location:

```nginx
location /run {
    proxy_pass http://unix:/run/eskiu-playground/api.sock:/run;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
}
location /healthz { proxy_pass http://unix:/run/eskiu-playground/api.sock:/healthz; }
```

Update / rebuild:

```bash
docker build -f playground/Dockerfile -t eskiu-playground playground
docker rm -f eskiu-playground
docker run -d ... eskiu-playground   # same flags as above
```

Behind the socket every request comes from the local proxy, which the default
`TRUSTED_PROXIES` (loopback) trusts, so the rate limit uses the client address nginx
puts in `X-Forwarded-For`.

`TRUSTED_PROXIES` is a comma-separated list of IP addresses or CIDRs for the
reverse proxies that connect directly to the container. The playground ignores
`X-Forwarded-For` from every other peer and walks a trusted chain from right
to left, so a client-supplied leftmost value cannot bypass the rate limit.
Use the actual Docker bridge/proxy address for your deployment; the default
trusts only `127.0.0.1` and `::1`.

## Threat model & limits

User code is untrusted. Isolation is the container plus, per request:

| Control            | Default            | Env var          |
|--------------------|--------------------|------------------|
| Wall-clock timeout | 15 s               | `WALL_TIMEOUT`   |
| CPU time           | 12 s (RLIMIT_CPU)  | `CPU_SECONDS`    |
| Address space      | 1 GiB (RLIMIT_AS)  | `MEM_BYTES`      |
| Max file size      | 64 MiB             | `FSIZE_BYTES`    |
| Max processes      | 96 (RLIMIT_NPROC)  | `NPROC`          |
| Source size        | 64 KiB             | `MAX_CODE_BYTES` |
| Output per stream  | 64 KiB             | `MAX_OUTPUT_BYTES` |
| Rate limit / IP    | 20 per 60 s        | `RATE_REQUESTS`, `RATE_WINDOW` |
| Trusted proxies    | loopback only      | `TRUSTED_PROXIES` |

Runs as an unprivileged user (`uid 10001`) in its own session, with a fresh
ephemeral work directory per request (`sandbox.py`):

- stdout and stderr are read while the program runs and kept up to the cap; the
  rest is dropped, so a program that prints without end does not grow the API's memory;
- when the program ends, times out or fails, its whole process group is killed, and
  a sweep kills any process of the user that left it (a daemon in a new session);
- the container has no network (`--network none`), and the API checks it before
  running anything.

The `docker run` flags above add a read-only root FS, dropped capabilities,
`no-new-privileges`, a PID limit and a memory limit.
