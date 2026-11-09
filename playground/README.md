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
docker build --build-arg ESKIU_VERSION=0.14.1 -t eskiu-playground .   # a specific release
```

## Run on the VPS

```bash
docker run -d --name eskiu-playground --restart unless-stopped \
  -p 127.0.0.1:8080:8080 \
  -e ALLOWED_ORIGINS="https://eskiu-lang.org,https://www.eskiu-lang.org" \
  -e TRUSTED_PROXIES="172.17.0.1/32" \
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
- `--tmpfs /tmp:...,exec` is required: `eskiuc run` links and executes a
  temporary binary under `$TMPDIR`, which the app points at `/tmp`.
- Bind to `127.0.0.1` and let your existing reverse proxy (nginx/Caddy/Traefik)
  terminate TLS and forward to it. Then set `PLAYGROUND_API` in
  `site/index.html` to the public URL.

Example nginx location:

```nginx
location /run {
    proxy_pass http://127.0.0.1:8080/run;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
}
location /healthz { proxy_pass http://127.0.0.1:8080/healthz; }
```

Update / rebuild:

```bash
docker build -f playground/Dockerfile -t eskiu-playground .
docker rm -f eskiu-playground
docker run -d ... eskiu-playground   # same flags as above
```

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

Runs as an unprivileged user (`uid 10001`), its own session/process group
(killed as a tree on timeout), with a fresh ephemeral work directory per
request. The `docker run` flags above add a read-only root FS, dropped
capabilities, and `no-new-privileges`.

The container does **not** block outbound network from user code on its own.
If that matters to you, run it on an isolated Docker network with no egress, or
front it with an nsjail/gVisor layer.
