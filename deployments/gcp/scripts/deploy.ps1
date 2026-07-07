param(
  [string]$ProjectId = "boltstream-r7m5o9ld",
  [string]$Zone = "us-central1-a",
  [string]$InstanceName = "boltstream-vm",
  [Parameter(Mandatory = $true)][string]$Artifact,
  [string]$GitSha = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ExpectedAccount = "nickaccturk@gmail.com"
$Gcloud = "gcloud.cmd"
$ActiveAccount = (& $Gcloud auth list --filter=status:ACTIVE --format="value(account)").Trim()
if ($ActiveAccount -ne $ExpectedAccount) {
  throw "Refusing to deploy. Active account is '$ActiveAccount', expected '$ExpectedAccount'."
}

if (-not (Test-Path $Artifact)) {
  throw "Artifact not found: $Artifact"
}

if ([string]::IsNullOrWhiteSpace($GitSha)) {
  $GitSha = (git rev-parse --short=12 HEAD).Trim()
}
if ([string]::IsNullOrWhiteSpace($GitSha)) {
  throw "GitSha is required."
}

$RemoteArtifact = "/tmp/boltstream-$GitSha.tar.gz"
$RemoteScript = "/tmp/boltstream-deploy-$GitSha.sh"
$LocalScript = Join-Path $env:TEMP "boltstream-deploy-$GitSha.sh"

@"
#!/usr/bin/env bash
set -euo pipefail

GIT_SHA="$GitSha"
ARTIFACT="$RemoteArtifact"
RELEASE_DIR="/opt/boltstream/releases/$GitSha"

sudo useradd --system --home /var/lib/boltstream --shell /usr/sbin/nologin boltstream 2>/dev/null || true
sudo mkdir -p /opt/boltstream/releases /etc/boltstream /var/lib/boltstream
sudo rm -rf "`$RELEASE_DIR"
sudo mkdir -p "`$RELEASE_DIR"
sudo tar -xzf "`$ARTIFACT" -C "`$RELEASE_DIR"
sudo ln -sfn "`$RELEASE_DIR" /opt/boltstream/current
sudo chown -R root:root /opt/boltstream
sudo chown -R boltstream:boltstream /var/lib/boltstream

sudo tee /etc/boltstream/boltstream.env >/dev/null <<EOF
BOLTSTREAM_GIT_SHA=$GitSha
EOF
sudo chmod 0640 /etc/boltstream/boltstream.env
sudo chown root:boltstream /etc/boltstream/boltstream.env

sudo tee /etc/systemd/system/boltstream.service >/dev/null <<'EOF'
[Unit]
Description=BoltStream broker
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=boltstream
Group=boltstream
WorkingDirectory=/var/lib/boltstream
EnvironmentFile=-/etc/boltstream/boltstream.env
ExecStart=/opt/boltstream/current/bin/boltstream-server --listen 0.0.0.0:9000 --admin-listen 127.0.0.1:9100 --data /var/lib/boltstream
Restart=always
RestartSec=2
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
ReadWritePaths=/var/lib/boltstream

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable boltstream.service
sudo systemctl restart boltstream.service
sleep 2
systemctl --no-pager --full status boltstream.service
curl -fsS http://127.0.0.1:9100/version
"@ | Set-Content -Path $LocalScript -Encoding ASCII

& $Gcloud compute scp --project $ProjectId --zone $Zone $Artifact "${InstanceName}:$RemoteArtifact"
& $Gcloud compute scp --project $ProjectId --zone $Zone $LocalScript "${InstanceName}:$RemoteScript"
& $Gcloud compute ssh $InstanceName --project $ProjectId --zone $Zone --command "bash $RemoteScript"
