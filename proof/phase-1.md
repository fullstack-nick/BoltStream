# Phase 1 Proof - Repository Foundation

Status: complete

Proof finalized: 2026-07-07

## Runtime Commit

- Repository: `https://github.com/fullstack-nick/BoltStream`
- Runtime commit: `e2dcfdf1921296180d89f447a505297edfe10c4f`
- Runtime short SHA: `e2dcfdf19212`
- CI run: `https://github.com/fullstack-nick/BoltStream/actions/runs/28900071816`
- CI result: success
- Artifact: `boltstream-linux-x86_64-e2dcfdf19212.tar.gz`

The proof document was updated after deploying the runtime commit. The deployed service and `/version` output below identify the exact runtime artifact.

## Local Verification

- Native toolchain check passed for CMake, Ninja, MSVC, MSYS2 UCRT GCC, LLVM Clang, LLDB, Python 3.11, `curl.exe`, `zip`, and `unzip`.
- Native builds/tests passed:
  - `windows-gcc-debug`: `ctest` passed, 6/6 tests.
  - `windows-msvc-debug`: `ctest` passed, 6/6 tests.
  - `windows-clang-debug`: `ctest` passed, 6/6 tests.
- Formatting passed with `scripts/format.ps1`.
- Local server smoke passed for:
  - `GET /health/live`
  - `GET /health/ready`
  - `GET /version`
  - TCP broker banner on port `9000`
  - CLI help and benchmark dry-run placeholders.
- Docker Linux-parity build passed on the final tree:
  - Ubuntu 24.04 builder image.
  - Linux Debug configure/build/test passed.
  - Linux Release configure/build passed.
  - Docker context stayed source-only at about 86 KB.

## GCP Infrastructure

- GCP project: `boltstream-r7m5o9ld`
- Region: `us-central1`
- Zone: `us-central1-a`
- VM: `boltstream-vm`
- Runtime OS: Ubuntu 24.04 LTS, aligned with CI and Docker Linux builds.
- Machine type: `e2-micro`
- Boot disk: 10 GB standard persistent disk.
- Data disk: 20 GB standard persistent disk mounted at `/var/lib/boltstream`.
- Terraform state backend: `gs://boltstream-r7m5o9ld-tfstate/terraform/state`
- Admin endpoint: `127.0.0.1:9100` on the VM only.
- Broker TCP port: `9000`, firewall-restricted to the operator `/32`.
- SSH: firewall-restricted to the operator `/32`.

Operator source IP details are intentionally not committed.

Terraform verification:

```text
terraform plan -detailed-exitcode
No changes. Your infrastructure matches the configuration.
```

## Deployment Verification

Deployment command:

```powershell
.\deployments\gcp\scripts\deploy.ps1 `
  -Artifact "artifacts\ci\boltstream-linux-x86_64-e2dcfdf19212.tar.gz" `
  -GitSha "e2dcfdf19212"
```

Deployment result excerpt:

```text
Active: active (running)
ExecStart=/opt/boltstream/current/bin/boltstream-server --listen 0.0.0.0:9000 --admin-listen 127.0.0.1:9100 --data /var/lib/boltstream
boltstream-server listening on 0.0.0.0:9000 admin=127.0.0.1:9100 data=/var/lib/boltstream
{"service":"boltstream","version":"0.1.0","git_sha":"e2dcfdf19212","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"0","storage_format_version":"0","startup_time_utc":"2026-07-07T21:41:45Z"}
```

Graceful restart proof after deploying the final signal-handling binary:

```text
boltstream-server shutting down on signal 15
Stopping boltstream.service - BoltStream broker...
boltstream.service: Deactivated successfully.
Stopped boltstream.service - BoltStream broker.
Started boltstream.service - BoltStream broker.
```

## Live Smoke

Command:

```powershell
.\deployments\gcp\scripts\smoke-live.ps1 -ExpectedGitSha "e2dcfdf19212"
```

Output excerpt:

```text
Checking TCP broker port 9000 from operator machine.
TCP broker port 9000 reachable.

live:
{"service":"boltstream","status":"live","git_sha":"e2dcfdf19212","detail":"ready"}

ready:
{"service":"boltstream","status":"ready","git_sha":"e2dcfdf19212","detail":"ready"}

version:
{"service":"boltstream","version":"0.1.0","git_sha":"e2dcfdf19212","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"0","storage_format_version":"0","startup_time_utc":"2026-07-07T21:42:22Z"}
```

## SSH Inspection

Command:

```powershell
.\deployments\gcp\scripts\inspect-live.ps1
```

Systemd and log excerpt:

```text
Active: active (running) since Tue 2026-07-07 21:42:22 UTC
boltstream-server listening on 0.0.0.0:9000 admin=127.0.0.1:9100 data=/var/lib/boltstream
```

Data disk excerpt:

```text
Filesystem      Size  Used Avail Use% Mounted on
/dev/sdb         20G   24K   19G   1% /var/lib/boltstream
data dir writable by boltstream
```

Release layout excerpt:

```text
/opt/boltstream/releases/e2dcfdf19212
boltstream-bench
boltstream-consumer
boltstream-producer
boltstream-server
```

## Result

Go.

Phase 1 is complete for the runtime commit `e2dcfdf1921296180d89f447a505297edfe10c4f`: the repository foundation is pushed to GitHub, CI is green, the Linux artifact is deployed to GCP, live health/readiness/version checks pass, the broker TCP port is reachable from the operator machine, Terraform drift is clean, the persistent data disk is mounted and writable by the service user, and the service restarts cleanly under systemd.
