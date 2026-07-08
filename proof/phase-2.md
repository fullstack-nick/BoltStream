# Phase 2 Proof - Binary Protocol and Async Client Library

Status: complete

Proof finalized: 2026-07-08

## Runtime Commit

- Repository: `https://github.com/fullstack-nick/BoltStream`
- Runtime commit: `7461470e40c9bacc8fb53c1252296bc5ee3f7b17`
- Runtime short SHA: `7461470e40c9`
- CI run: `https://github.com/fullstack-nick/BoltStream/actions/runs/28917050913`
- CI result: success
- Artifact: `boltstream-linux-x86_64-7461470e40c9.tar.gz`

The deployed service and `/version` output below identify the exact runtime artifact.

## Local Verification

- Formatting passed:
  - `.\scripts\format.ps1`
- Native builds/tests passed:
  - `windows-gcc-debug`: `ctest` passed, 19/19 tests.
  - `windows-msvc-debug`: `ctest` passed, 19/19 tests.
  - `windows-clang-debug`: `ctest` passed, 19/19 tests.
- CLI protocol smoke passed:

```text
producer returned expected not_implemented response.
{"status":"not_implemented","error_code":"not_implemented","message":"produce storage is implemented in Phase 4","correlation_id":1}
consumer returned expected not_implemented response.
{"status":"not_implemented","error_code":"not_implemented","message":"fetch storage is implemented in Phase 4","correlation_id":1}
Phase 2 CLI smoke passed on 127.0.0.1:10795.
```

- Docker Linux-parity build passed:
  - Ubuntu 24.04 builder image.
  - Linux Debug configure/build/test passed, 19/19 tests.
  - Linux Release configure/build passed.

Protocol test coverage includes valid frames, truncated headers, invalid magic, invalid
lengths, unsupported versions, CRC mismatch, reserved flags, malformed produce payloads,
configured max-frame enforcement, error-response payload decoding, loopback client/broker
health, produce/fetch `not_implemented`, and concurrent in-flight correlation ids.

## CI Verification

GitHub Actions run `28917050913` passed for pushed commit `7461470e40c9bacc8fb53c1252296bc5ee3f7b17`:

- Linux Debug: success.
- Linux Release: success; uploaded `boltstream-linux-x86_64-7461470e40c9.tar.gz`.
- Windows MSVC smoke: success.

The run emitted Node.js deprecation annotations for GitHub actions, but no BoltStream
build, test, format, or packaging failures.

## Deployment Verification

Deployment command:

```powershell
.\deployments\gcp\scripts\deploy.ps1 `
  -Artifact "artifacts\ci-phase2\boltstream-linux-x86_64-7461470e40c9.tar.gz" `
  -GitSha "7461470e40c9"
```

Deployment result excerpt:

```text
Active: active (running) since Wed 2026-07-08 04:18:27 UTC
ExecStart=/opt/boltstream/current/bin/boltstream-server --listen 0.0.0.0:9000 --admin-listen 127.0.0.1:9100 --data /var/lib/boltstream
boltstream-server listening on 0.0.0.0:9000 admin=127.0.0.1:9100 data=/var/lib/boltstream
{"service":"boltstream","version":"0.1.0","git_sha":"7461470e40c9","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"1","storage_format_version":"0","startup_time_utc":"2026-07-08T04:18:27Z"}
```

## Live Smoke

Command:

```powershell
.\deployments\gcp\scripts\smoke-live.ps1 -ExpectedGitSha "7461470e40c9"
```

Output excerpt:

```text
Checking TCP broker port 9000 from operator machine.
TCP broker port 9000 reachable.

live:
{"service":"boltstream","status":"live","git_sha":"7461470e40c9","detail":"ready"}
ready:
{"service":"boltstream","status":"ready","git_sha":"7461470e40c9","detail":"ready"}
version:
{"service":"boltstream","version":"0.1.0","git_sha":"7461470e40c9","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"1","storage_format_version":"0","startup_time_utc":"2026-07-08T04:18:27Z"}
```

Live protocol calls against VM `boltstream-vm` external address `35.209.179.9`:

```powershell
.\build\windows-gcc-debug\boltstream-producer.exe `
  --host 35.209.179.9 --port 9000 `
  --topic trades --key AAPL --message "AAPL,100,192.41"

.\build\windows-gcc-debug\boltstream-consumer.exe `
  --host 35.209.179.9 --port 9000 `
  --topic trades --from beginning
```

Observed output:

```text
{"status":"not_implemented","error_code":"not_implemented","message":"produce storage is implemented in Phase 4","correlation_id":1}
producer_exit=3
{"status":"not_implemented","error_code":"not_implemented","message":"fetch storage is implemented in Phase 4","correlation_id":1}
consumer_exit=3
```

## SSH Inspection

Command:

```powershell
.\deployments\gcp\scripts\inspect-live.ps1
```

Systemd and log excerpt:

```text
Active: active (running) since Wed 2026-07-08 04:18:27 UTC
boltstream-server listening on 0.0.0.0:9000 admin=127.0.0.1:9100 data=/var/lib/boltstream
protocol request correlation_id=1 type=produce_request payload_bytes=37
protocol request correlation_id=1 type=fetch_request payload_bytes=23
```

Version excerpt:

```text
{"service":"boltstream","version":"0.1.0","git_sha":"7461470e40c9","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"1","storage_format_version":"0","startup_time_utc":"2026-07-08T04:18:27Z"}
```

Data disk excerpt:

```text
Filesystem      Size  Used Avail Use% Mounted on
/dev/sdb         20G   24K   19G   1% /var/lib/boltstream
data dir writable by boltstream
```

Release layout excerpt:

```text
/opt/boltstream/releases/7461470e40c9
boltstream-bench
boltstream-consumer
boltstream-producer
boltstream-server
```

## Terraform Drift

Command:

```powershell
terraform plan -detailed-exitcode
```

Result:

```text
No changes. Your infrastructure matches the configuration.
```

## Result

Go.

Phase 2 is complete for the runtime commit `7461470e40c9bacc8fb53c1252296bc5ee3f7b17`: the
binary protocol is versioned at `1`, broker TCP traffic uses framed request/response
handling, the C++ async client library drives the producer and consumer CLIs, protocol
errors are structured, local and Docker/Linux tests pass, CI is green, the exact CI
artifact is deployed to GCP, live binary protocol calls return the expected
`not_implemented` responses, SSH inspection shows correlation-id protocol logs, and
Terraform reports no drift.
