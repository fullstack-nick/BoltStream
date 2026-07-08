# Phase 3 Proof - Durable Single-Partition Log

Status: complete

Proof finalized: 2026-07-08

## Runtime Commit

- Repository: `https://github.com/fullstack-nick/BoltStream`
- Runtime commit: `6da0ed1674115b4728888bc0f4731bd23cca29ca`
- Runtime short SHA: `6da0ed167411`
- CI run: `https://github.com/fullstack-nick/BoltStream/actions/runs/28924522843`
- CI result: success
- Artifact: `boltstream-linux-x86_64-6da0ed167411.tar.gz`

The deployed service and `/version` output below identify the exact runtime artifact.

## Local Verification

- Formatting passed:
  - `.\scripts\format.ps1`
- Native builds/tests passed:
  - `windows-gcc-debug`: `ctest` passed, 26/26 tests.
  - `windows-msvc-debug`: `ctest` passed, 26/26 tests.
  - `windows-clang-debug`: `ctest` passed, 26/26 tests.
- Phase 3 storage smoke passed:

```text
Phase 3 storage smoke passed with data dir C:\Users\napet\AppData\Local\Temp\boltstream-phase3-smoke-b46d7002-adae-40df-8480-42efeb6a6474.
```

- Phase 2 protocol smoke still passed:

```text
producer returned expected not_implemented response.
{"status":"not_implemented","error_code":"not_implemented","message":"produce storage is implemented in Phase 4","correlation_id":1}
consumer returned expected not_implemented response.
{"status":"not_implemented","error_code":"not_implemented","message":"fetch storage is implemented in Phase 4","correlation_id":1}
Phase 2 CLI smoke passed on 127.0.0.1:23657.
```

- Docker Linux-parity build passed:
  - Ubuntu 24.04 builder image.
  - Linux Debug configure/build/test passed, 26/26 tests.
  - Linux Release configure/build passed.

Storage test coverage includes append/read round trip, stable offsets, reopen recovery,
segment rolling, cross-segment reads, index rebuild from logs, incomplete trailing-byte
truncation, CRC-corrupt trailing-record truncation, and invalid topic rejection.

## CI Verification

GitHub Actions run `28924522843` passed for pushed commit
`6da0ed1674115b4728888bc0f4731bd23cca29ca`:

- Linux Debug: success.
- Linux Release: success; uploaded `boltstream-linux-x86_64-6da0ed167411.tar.gz`.
- Windows MSVC smoke: success.

The run emitted Node.js deprecation annotations for GitHub actions, but no BoltStream
build, test, format, or packaging failures.

## Deployment Verification

Deployment command:

```powershell
.\deployments\gcp\scripts\deploy.ps1 `
  -Artifact "artifacts\ci-phase3\boltstream-linux-x86_64-6da0ed1674115b4728888bc0f4731bd23cca29ca\boltstream-linux-x86_64-6da0ed167411.tar.gz" `
  -GitSha "6da0ed167411"
```

Deployment result excerpt:

```text
Active: active (running) since Wed 2026-07-08 07:14:59 UTC
ExecStart=/opt/boltstream/current/bin/boltstream-server --listen 0.0.0.0:9000 --admin-listen 127.0.0.1:9100 --data /var/lib/boltstream
storage recovery topics=0 partitions=0 segments=0 indexes_rebuilt=0 records=0 bytes_truncated=0
boltstream-server listening on 0.0.0.0:9000 admin=127.0.0.1:9100 data=/var/lib/boltstream
{"service":"boltstream","version":"0.1.0","git_sha":"6da0ed167411","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"1","storage_format_version":"1","startup_time_utc":"2026-07-08T07:14:59Z"}
```

## Live Smoke

Command:

```powershell
.\deployments\gcp\scripts\smoke-live.ps1 -ExpectedGitSha "6da0ed167411"
```

Output excerpt:

```text
Checking TCP broker port 9000 from operator machine.
TCP broker port 9000 reachable.

live:
{"service":"boltstream","status":"live","git_sha":"6da0ed167411","detail":"ready"}
ready:
{"service":"boltstream","status":"ready","git_sha":"6da0ed167411","detail":"ready"}
version:
{"service":"boltstream","version":"0.1.0","git_sha":"6da0ed167411","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"1","storage_format_version":"1","startup_time_utc":"2026-07-08T07:14:59Z"}
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

## Live Storage Proof

VM-side proof used `sudo -u boltstream /opt/boltstream/current/bin/boltstream-logtool`
against `/var/lib/boltstream` and topic
`phase3-6da0ed167411-20260708091645`.

Append/read excerpt:

```text
append-1:
{"status":"ok","topic":"phase3-6da0ed167411-20260708091645","partition":0,"offset":0,"next_offset":1,"encoded_bytes":88}
append-2:
{"status":"ok","topic":"phase3-6da0ed167411-20260708091645","partition":0,"offset":1,"next_offset":2,"encoded_bytes":88}
read-before-corrupt:
{"status":"ok","topic":"phase3-6da0ed167411-20260708091645","partition":0,"from":0,"count":2,"next_offset":2,"records":[{"offset":0,"timestamp_unix_ns":1783495015440278147,"key":"AAPL","message":"phase3-live-message-000000000000000000000000","encoded_bytes":88},{"offset":1,"timestamp_unix_ns":1783495015460459416,"key":"MSFT","message":"phase3-live-message-111111111111111111111111","encoded_bytes":88}]}
```

Segment files before corruption:

```text
00000000000000000000.index 20 bytes
00000000000000000000.log 88 bytes
00000000000000000001.index 20 bytes
00000000000000000001.log 88 bytes
```

After appending three garbage bytes to the trailing segment:

```text
recover-after-corrupt:
{"status":"ok","topic":"phase3-6da0ed167411-20260708091645","partition":0,"next_offset":2,"segments_scanned":2,"indexes_rebuilt":2,"records_recovered":2,"bytes_truncated":3}
read-after-recover:
{"status":"ok","topic":"phase3-6da0ed167411-20260708091645","partition":0,"from":0,"count":2,"next_offset":2,"records":[{"offset":0,"timestamp_unix_ns":1783495015440278147,"key":"AAPL","message":"phase3-live-message-000000000000000000000000","encoded_bytes":88},{"offset":1,"timestamp_unix_ns":1783495015460459416,"key":"MSFT","message":"phase3-live-message-111111111111111111111111","encoded_bytes":88}]}
```

After `sudo systemctl restart boltstream.service`:

```text
Active: active (running) since Wed 2026-07-08 07:16:55 UTC
storage recovery topics=1 partitions=1 segments=2 indexes_rebuilt=2 records=2 bytes_truncated=0
{"service":"boltstream","version":"0.1.0","git_sha":"6da0ed167411","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"1","storage_format_version":"1","startup_time_utc":"2026-07-08T07:16:55Z"}
read-after-service-restart:
{"status":"ok","topic":"phase3-6da0ed167411-20260708091645","partition":0,"from":0,"count":2,"next_offset":2,"records":[{"offset":0,"timestamp_unix_ns":1783495015440278147,"key":"AAPL","message":"phase3-live-message-000000000000000000000000","encoded_bytes":88},{"offset":1,"timestamp_unix_ns":1783495015460459416,"key":"MSFT","message":"phase3-live-message-111111111111111111111111","encoded_bytes":88}]}
```

## SSH Inspection

Command:

```powershell
.\deployments\gcp\scripts\inspect-live.ps1
```

Systemd and log excerpt:

```text
Active: active (running) since Wed 2026-07-08 07:16:55 UTC
storage recovery topics=1 partitions=1 segments=2 indexes_rebuilt=2 records=2 bytes_truncated=0
boltstream-server listening on 0.0.0.0:9000 admin=127.0.0.1:9100 data=/var/lib/boltstream
```

Version excerpt:

```text
{"service":"boltstream","version":"0.1.0","git_sha":"6da0ed167411","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"1","storage_format_version":"1","startup_time_utc":"2026-07-08T07:16:55Z"}
```

Data disk and file excerpt:

```text
Filesystem      Size  Used Avail Use% Mounted on
/dev/sdb         20G   52K   19G   1% /var/lib/boltstream
data dir writable by boltstream
/var/lib/boltstream/topics/phase3-6da0ed167411-20260708091645/partition-000000/00000000000000000000.index 20 bytes boltstream:boltstream
/var/lib/boltstream/topics/phase3-6da0ed167411-20260708091645/partition-000000/00000000000000000000.log 88 bytes boltstream:boltstream
/var/lib/boltstream/topics/phase3-6da0ed167411-20260708091645/partition-000000/00000000000000000001.index 20 bytes boltstream:boltstream
/var/lib/boltstream/topics/phase3-6da0ed167411-20260708091645/partition-000000/00000000000000000001.log 88 bytes boltstream:boltstream
```

Release layout excerpt:

```text
/opt/boltstream/releases/6da0ed167411
boltstream-bench
boltstream-consumer
boltstream-logtool
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

Phase 3 is complete for the runtime commit
`6da0ed1674115b4728888bc0f4731bd23cca29ca`: the storage format is versioned at
`1`, append-only segment logs assign stable offsets, segment rolling and index rebuild
are tested, CRC/incomplete trailing corruption is detected and truncated, broker startup
recovers existing storage, `boltstream-logtool` is packaged in the CI Linux artifact and
deployed release, local Windows and Docker/Linux verification pass, CI is green, the
exact CI artifact is live on GCP, live storage records survive service restart, SSH
inspection proves the on-disk segment/index files, live producer/consumer protocol calls
still return Phase 4 `not_implemented`, and Terraform reports no drift.
