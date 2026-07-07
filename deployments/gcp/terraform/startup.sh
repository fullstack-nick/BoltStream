#!/usr/bin/env bash
set -euo pipefail

DATA_DEVICE="/dev/disk/by-id/google-boltstream-data"
DATA_DIR="/var/lib/boltstream"

mkdir -p "${DATA_DIR}"

if [ -e "${DATA_DEVICE}" ]; then
  if ! blkid "${DATA_DEVICE}" >/dev/null 2>&1; then
    mkfs.ext4 -F "${DATA_DEVICE}"
  fi

  if ! grep -q "${DATA_DIR}" /etc/fstab; then
    echo "${DATA_DEVICE} ${DATA_DIR} ext4 defaults,nofail 0 2" >> /etc/fstab
  fi

  mountpoint -q "${DATA_DIR}" || mount "${DATA_DIR}"
fi

apt-get update
apt-get install -y --no-install-recommends ca-certificates curl tar
rm -rf /var/lib/apt/lists/*

