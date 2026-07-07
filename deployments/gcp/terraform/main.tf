locals {
  name = "boltstream"
}

resource "google_compute_network" "main" {
  name                    = "${local.name}-vpc"
  auto_create_subnetworks = false
}

resource "google_compute_subnetwork" "main" {
  name          = "${local.name}-subnet"
  ip_cidr_range = "10.30.0.0/24"
  region        = var.region
  network       = google_compute_network.main.id
}

resource "google_service_account" "vm" {
  account_id   = "${local.name}-vm"
  display_name = "BoltStream VM service account"
}

resource "google_compute_firewall" "ssh_operator" {
  name    = "${local.name}-allow-ssh-operator"
  network = google_compute_network.main.name

  allow {
    protocol = "tcp"
    ports    = ["22"]
  }

  source_ranges = [var.operator_cidr]
  target_tags   = ["${local.name}-vm"]
}

resource "google_compute_firewall" "broker_operator" {
  name    = "${local.name}-allow-broker-operator"
  network = google_compute_network.main.name

  allow {
    protocol = "tcp"
    ports    = ["9000"]
  }

  source_ranges = [var.operator_cidr]
  target_tags   = ["${local.name}-vm"]
}

resource "google_compute_disk" "data" {
  name   = "${local.name}-data"
  type   = "pd-standard"
  zone   = var.zone
  size   = 20
  labels = var.labels
}

resource "google_compute_instance" "vm" {
  name         = "${local.name}-vm"
  machine_type = "e2-micro"
  zone         = var.zone
  tags         = ["${local.name}-vm"]
  labels       = var.labels

  boot_disk {
    initialize_params {
      image = "debian-cloud/debian-12"
      size  = 10
      type  = "pd-standard"
    }
  }

  network_interface {
    subnetwork = google_compute_subnetwork.main.id

    access_config {
      network_tier = "STANDARD"
    }
  }

  attached_disk {
    source      = google_compute_disk.data.id
    device_name = "${local.name}-data"
    mode        = "READ_WRITE"
  }

  metadata = {
    enable-oslogin = "FALSE"
    startup-script = file("${path.module}/startup.sh")
  }

  service_account {
    email = google_service_account.vm.email
    scopes = [
      "https://www.googleapis.com/auth/logging.write",
      "https://www.googleapis.com/auth/monitoring.write"
    ]
  }

  depends_on = [google_compute_disk.data]
}

resource "google_secret_manager_secret" "broker_token" {
  secret_id = "${local.name}-broker-token"

  replication {
    auto {}
  }

  labels = var.labels
}

resource "google_secret_manager_secret" "admin_token" {
  secret_id = "${local.name}-admin-token"

  replication {
    auto {}
  }

  labels = var.labels
}

