#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "${script_dir}/.." && pwd)
work_dir="${project_dir}/build/orin-nano"
src_dir="${work_dir}/src"
build_dir="${work_dir}/build"
manifest_url="https://github.com/AJamesChen/yocto-platforms.git"

if ! command -v repo >/dev/null 2>&1; then
    echo "error: the Android repo tool is not installed" >&2
    exit 1
fi

mkdir -p "${src_dir}"
cd "${src_dir}"
repo init -u "${manifest_url}" -b update-mfrc522 -m manifests/orin-nano.xml
repo sync

cd "${src_dir}/openembedded-core"
TEMPLATECONF="${src_dir}/yocto-platforms/boards/orin-nano/conf-template" \
    . ./oe-init-build-env "${build_dir}"

echo "Yocto sources and build configuration are ready in ${work_dir}"
