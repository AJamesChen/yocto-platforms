SUMMARY = "Development image for the NVIDIA Jetson Orin Nano devkit"
LICENSE = "MIT"

inherit core-image

# Keep the starting image deliberately small. Add application and hardware
# support packages here as the product requirements become clear.
IMAGE_FEATURES += "ssh-server-openssh"
IMAGE_INSTALL:append = " systemd-networkd-config"
