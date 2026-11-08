# Keep the text console readable on 4K DisplayPort monitors.
UBOOT_EXTLINUX_KERNEL_ARGS:append = " video=DP-1:1920x1080@60"

# Select NVIDIA's dual IMX219 overlay for both Orin Nano devkit CSI ports.
UBOOT_EXTLINUX_FDTOVERLAYS = "tegra234-p3767-camera-p3768-imx219-dual.dtbo"
