#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "${script_dir}/.." && pwd)
work_dir="${project_dir}/build/orin-nano"
src_dir="${work_dir}/src"
build_dir="${work_dir}/build"
target=${1:-core-image-orin-nano}

if [ "$#" -gt 1 ]; then
    echo "usage: $0 [bitbake-target]" >&2
    exit 2
fi

if [ ! -f "${src_dir}/poky/oe-init-build-env" ]; then
    echo "error: Yocto sources are missing; run scripts/setup-orin-nano.sh first" >&2
    exit 1
fi

cd "${src_dir}/poky"
TEMPLATECONF="${src_dir}/yocto-platforms/boards/orin-nano/conf-template" \
    . ./oe-init-build-env "${build_dir}"

bitbake "${target}"
