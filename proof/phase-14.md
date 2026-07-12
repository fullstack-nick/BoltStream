# Phase 14 Recruiter-Grade Polish Proof

Date: 2026-07-12  
Runtime commit: `2623764ef500286f6da959c9b2881c0812399825`  
CI run: [29179609563](https://github.com/fullstack-nick/BoltStream/actions/runs/29179609563)

## Scope Delivered

- Replaced the development-log README with a product-oriented document containing a
  CI badge, architecture diagram, Docker quickstart, native commands, metrics examples,
  measured benchmark results, bounded recovery evidence, GCP deployment shape, and
  focused documentation links.
- Verified case-insensitively that `README.md` contains neither `phase` nor `stage`, and
  verified that every local README link resolves.
- Added the zero-dependency Python reference client, its CLI and unit tests, an
  authenticated real-broker smoke, and Linux Debug CI coverage.
- Updated protocol documentation from the stale version-4-only description to protocol
  5 with version-4 compatibility, codec negotiation, batch frames, compressed fetch,
  and current structured errors.
- Updated storage documentation from format 2 to format 3, including mixed record/batch
  segments, CRC and length validation, recovery, compatibility, and claim boundaries.
- Replaced the benchmark publisher's old development-labelled README markers with
  neutral product markers while preserving exact regeneration checks.

## Local Verification

The following checks passed from the repository root:

```text
python -m unittest discover -s clients/python/tests -v
  7 tests passed

scripts/smoke-polish.ps1 -Preset windows-gcc-debug -SkipBuild
  authenticated create, produce, and fetch passed against a real broker

scripts/test.ps1 -Preset windows-gcc-debug
  93/93 C++ tests passed

docker compose config --quiet
  exit 0

scripts/render-benchmarks.ps1 -InputDir benchmarks/results/raw/gcp -Check
  benchmark publication reproducible

README terminology check
  no case-insensitive phase/stage matches

README local-link check
  all 15 links resolved

git diff --check
  exit 0
```

## CI and Artifact

Run `29179609563` completed successfully for runtime commit `2623764ef500`:

- Linux Debug: configure/build, 93 C++ tests, formatting, seven Python unit tests,
  authenticated Python interoperability, benchmark/compression/replication/recovery
  smokes, benchmark publication, and operations asset validation.
- Linux Release: configure/build, package, and artifact upload.
- Windows MSVC: configure/build and the complete configured smoke/publication set.
- Linux ThreadSanitizer: focused concurrent broker tests.

The deployed input was the run's `boltstream-linux-x86_64-<full-sha>` artifact. Its
archive was named `boltstream-linux-x86_64-2623764ef500.tar.gz`; no local rebuild was
substituted.

## Exact-Artifact GCP Deployment

The guarded deploy ran under active account `nickaccturk@gmail.com` in project
`boltstream-r7m5o9ld`, installing the CI artifact at:

```text
/opt/boltstream/releases/2623764ef500
```

The post-deploy service and version evidence was:

```json
{"service":"boltstream","version":"0.1.0","git_sha":"2623764ef500","build_type":"Release","compiler":"GNU 13.3.0","protocol_version":"5","storage_format_version":"3"}
```

`boltstream.service` was active, configuration validation passed, and startup recovery
reported zero truncated bytes. The full authenticated live regression passed for
produce/fetch, committed resume, coordinated group assignment/takeover, lifecycle,
retention, restart, health, version, logs, and storage inspection.

## Python Live Interoperability and Cleanup

The repository Python client connected directly to the source-restricted live broker
using the Secret Manager broker token and completed:

```json
{"topic":"python-live-20260712042947","create_status":"created","partition":0,"offset":0,"next_offset":1,"fetched_records":1,"message":"recruiter-polish-live"}
```

The topic was then deleted through the authenticated admin client. Deletion reported
one partition and one segment removed, and SSH verification confirmed its data path no
longer existed. Final runtime evidence remained:

```text
systemctl is-active boltstream.service -> active
/health/ready -> ready at git_sha 2623764ef500
/version -> Release, protocol 5, storage format 3, git_sha 2623764ef500
boltstream_build_info -> git_sha 2623764ef500
boltstream_ready -> 1
```

No broker token or concrete operator/VM IP address is recorded in this proof.

## Terraform Drift

The final command:

```powershell
terraform plan -detailed-exitcode -input=false
```

returned exit code `0` and `No changes. Your infrastructure matches the configuration.`

## Verdict

Phase 14 is complete: implementation, local verification, published cross-platform CI,
exact-artifact GCP deployment, live Python interoperability, cleanup, service health,
metrics identity, and Terraform no-drift evidence all passed.
