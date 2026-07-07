variable "project_id" {
  type        = string
  description = "Dedicated BoltStream GCP project id."
  default     = "boltstream-r7m5o9ld"
}

variable "region" {
  type        = string
  description = "Free-tier eligible GCP region."
  default     = "us-central1"
}

variable "zone" {
  type        = string
  description = "Free-tier eligible GCP zone."
  default     = "us-central1-a"
}

variable "operator_cidr" {
  type        = string
  description = "Operator source IPv4 CIDR for direct SSH and broker smoke tests."
  sensitive   = true
}

variable "labels" {
  type        = map(string)
  description = "Labels applied to GCP resources."
  default = {
    app       = "boltstream"
    managedby = "terraform"
    phase     = "1"
  }
}

