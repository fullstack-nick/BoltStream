# Operations

## Local Native Commands

```powershell
.\scripts\toolchain-check.ps1
.\scripts\build.ps1 -Preset windows-gcc-debug
.\scripts\test.ps1 -Preset windows-gcc-debug
.\scripts\smoke-phase4.ps1 -Preset windows-gcc-debug
.\scripts\format.ps1
```

Use `windows-msvc-debug` for the MSVC path and `windows-clang-debug` for the LLVM Clang path.

## Server Runtime

```powershell
.\build\windows-gcc-debug\boltstream-server.exe `
  --listen 127.0.0.1:9000 `
  --admin-listen 127.0.0.1:9100 `
  --data .\data `
  --max-frame-bytes 1048576 `
  --max-fetch-records 100 `
  --max-fetch-bytes 1048576
```

Admin endpoints:

- `GET /health/live`
- `GET /health/ready`
- `GET /version`

Broker protocol smoke:

```powershell
.\build\windows-gcc-debug\boltstream-producer.exe `
  --host 127.0.0.1 --port 9000 `
  --topic trades --key AAPL --message "AAPL,100,192.41"

.\build\windows-gcc-debug\boltstream-consumer.exe `
  --host 127.0.0.1 --port 9000 `
  --topic trades --from beginning

.\build\windows-gcc-debug\boltstream-logtool.exe append `
  --data .\data `
  --topic trades `
  --key AAPL `
  --message "AAPL,100,192.41"

.\build\windows-gcc-debug\boltstream-logtool.exe read `
  --data .\data `
  --topic trades `
  --from 0 `
  --max-records 10
```

Producer and consumer should return exit code `0` with structured JSON. If
`BOLTSTREAM_BROKER_TOKEN` is set on the broker, pass `--token` or set the same
environment variable for the CLI before calling produce/fetch.

Repeatable storage smoke:

```powershell
.\scripts\smoke-phase4.ps1 -Preset windows-gcc-debug
.\scripts\smoke-phase3.ps1 -Preset windows-gcc-debug
```

## Linux Service Layout

On GCP, deployment uses:

- Service user: `boltstream`
- Unit: `boltstream.service`
- Binary symlink: `/opt/boltstream/current`
- Release path: `/opt/boltstream/releases/<git-sha>`
- Data path: `/var/lib/boltstream`
- Config/env path: `/etc/boltstream`

Useful inspection commands:

```bash
systemctl --no-pager --full status boltstream.service
journalctl -u boltstream.service -n 80 --no-pager
curl -fsS http://127.0.0.1:9100/version
df -h /var/lib/boltstream
find /var/lib/boltstream/topics -maxdepth 4 -type f -print 2>/dev/null
readlink -f /opt/boltstream/current
```

## Phase Gate Rule

Local success is necessary but not sufficient. A phase is complete only when the pushed commit is built by CI, deployed to GCP, live-called, SSH-inspected, and recorded in the phase proof file.
