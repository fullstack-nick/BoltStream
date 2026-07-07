# Phase 1 Proof - Repository Foundation

Status: in progress

## Acceptance Gate

Phase 1 is complete only when all of these pass:

- Native Windows builds: GCC, Clang, and MSVC.
- Docker Linux-parity build and test.
- GitHub push to `fullstack-nick/BoltStream`.
- GitHub Actions `ci` succeeds.
- Linux release artifact is produced for the pushed commit.
- Terraform provisions project `boltstream-r7m5o9ld`.
- Post-apply Terraform drift check is clean.
- The CI artifact for the exact pushed Git SHA is deployed to GCP.
- Live TCP broker port `9000` is reachable from the operator machine.
- SSH-local `/health/live`, `/health/ready`, and `/version` calls succeed on the VM.
- `/version` reports the deployed Git SHA.
- SSH inspection shows clean systemd status, clean logs, mounted data disk, and writable data path.

## Evidence

This file is finalized after implementation and live proof. It must record:

- Commit SHA
- GitHub repository URL
- CI run URL
- Artifact name
- GCP project
- VM name and zone
- Terraform apply and drift-check result
- Live smoke commands
- Live output excerpts
- SSH inspection excerpts
- Final go/no-go result

Concrete operator source IP addresses must not be committed here.

