# yocto-platforms

Reproducible Yocto build configurations and custom layers for embedded Linux
platforms. Raspberry Pi 4 Model B is currently supported; STM32MP support is
planned.

## Raspberry Pi 4B

The Raspberry Pi configuration builds Yocto Project Scarthgap for the
`raspberrypi4-64` machine. The upstream repositories are pinned to the exact
revisions used by the verified base build. The RAUC integration follows the
maintained `scarthgap` branch of `meta-rauc`:

| Repository | Branch | Commit |
| --- | --- | --- |
| Poky | `scarthgap` | `3a3d07f625aee7b87ea1026526b756447c357652` |
| meta-raspberrypi | `scarthgap` | `6ca1f75017cc5d5acdb8bb05634c4bc01fa049fd` |
| meta-rauc | `scarthgap` | `d63878f20eba7a85ecf53566e7a3377e78bb46ac` |

Kas acts as the build manifest and orchestration tool. It checks out the pinned
repositories, generates `local.conf` and `bblayers.conf`, and invokes BitBake.

## Build-host requirements

The Yocto Project recommends at least 90 GB of free disk space and 8 GB of RAM.
A native Linux host is recommended. On Ubuntu or Debian, install the Scarthgap
build dependencies with:

```sh
sudo apt install \
  build-essential chrpath cpio debianutils diffstat file gawk gcc git \
  iputils-ping libacl1 liblz4-tool locales python3 python3-git \
  python3-jinja2 python3-pexpect python3-pip python3-subunit socat texinfo \
  unzip wget xz-utils zstd
sudo locale-gen en_US.UTF-8
```

See the [Yocto Project Scarthgap system requirements][yocto-requirements] for
other supported build-host distributions.

### Install Kas

The Kas project recommends `pipx`, which installs Kas in an isolated Python
environment:

```sh
sudo apt install pipx
pipx ensurepath
pipx install kas
```

If `uv` is already installed, `uv tool install kas` is equivalent.

Confirm that it is on `PATH`:

```sh
kas --version
```

The project has been verified with Kas 5.5. See the [Kas installation
guide][kas-install] for other installation and container options.

## Build

Clone the repository and enter it:

```sh
git clone https://github.com/AJamesChen/yocto-platforms.git
cd yocto-platforms
```

Prepare the source repositories and generated Yocto configuration:

```sh
./scripts/setup-rpi4b.sh
```

Build the default project image:

```sh
./scripts/build-rpi4b.sh
```

The first build downloads and compiles the complete toolchain and target image,
so it can take considerable time. Later builds reuse `downloads` and
`sstate-cache` under `build/rpi4b/`.

The project image extends `core-image-base` with RAUC, U-Boot environment tools,
the kernel in each rootfs, and a boot-success service. To build only the
original upstream image instead:

```sh
./scripts/build-rpi4b.sh core-image-base
```

Any other BitBake target can be passed as the sole argument:

```sh
./scripts/build-rpi4b.sh core-image-minimal
```

## Build output

Deployable artifacts are written to:

```text
build/rpi4b/build/tmp/deploy/images/raspberrypi4-64/
```

The compressed A/B SD-card image is:

```text
core-image-jamesc-raspberrypi4-64.rootfs.wic.bz2
```

The matching `.wic.bmap` file can be used with `bmaptool` when writing the image
to an SD card.

## Development shell

Start a shell with the generated BitBake environment:

```sh
./scripts/shell-rpi4b.sh
```

Commands such as these can then be run directly:

```sh
bitbake core-image-base
bitbake rpi4-update-bundle
bitbake-layers show-layers
bitbake -e core-image-base
```

Kas can also be used directly if the same work directory is selected:

```sh
export KAS_WORK_DIR="$PWD/build/rpi4b"
kas checkout kas/rpi4b.yml
kas build kas/rpi4b.yml
kas build --target core-image-jamesc kas/rpi4b.yml
kas shell kas/rpi4b.yml
```

## Repository layout

```text
boards/rpi4b/kas.yml                 Raspberry Pi machine and image settings
kas/rpi4b.yml                        Repositories, revisions, and enabled layers
layers/meta-jamesc/conf/layer.conf   Custom layer registration
layers/meta-jamesc/recipes-core/     Project image recipes
scripts/setup-rpi4b.sh               Checkout and configure the build
scripts/build-rpi4b.sh               Build an image target
scripts/shell-rpi4b.sh               Open the configured BitBake shell
build/rpi4b/                          Generated sources, caches, and build output
```

Everything below `build/` is generated or downloaded and is intentionally not
tracked by Git.

## Configuration notes

The board configuration enables UART, I2C, SPI, Wi-Fi/Bluetooth firmware,
OpenSSH, Yocto `debug-tweaks`, and rootfs-only RAUC A/B updates. See
[Raspberry Pi 4 A/B OTA with RAUC](docs/ota-rauc.md) for the partition layout,
build and installation procedure, rollback test, and production signing notes.

`debug-tweaks` permits development-oriented access, including an empty root
password. Remove `debug-tweaks` and review the SSH configuration in
`boards/rpi4b/kas.yml` before producing a production image.

[kas-install]: https://kas.readthedocs.io/en/latest/userguide/getting-started.html
[yocto-requirements]: https://docs.yoctoproject.org/scarthgap/ref-manual/system-requirements.html
