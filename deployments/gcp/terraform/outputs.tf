output "project_id" {
  value = var.project_id
}

output "region" {
  value = var.region
}

output "zone" {
  value = var.zone
}

output "instance_name" {
  value = google_compute_instance.vm.name
}

output "instance_external_ip" {
  value = google_compute_instance.vm.network_interface[0].access_config[0].nat_ip
}

output "broker_port" {
  value = 9000
}

output "admin_endpoint" {
  value = "http://127.0.0.1:9100"
}

output "data_disk" {
  value = google_compute_disk.data.name
}

output "vm_service_account" {
  value = google_service_account.vm.email
}

