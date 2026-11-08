#!/bin/bash
set -e

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

if [ ! -f "${src_dir}/openembedded-core/oe-init-build-env" ]; then
    echo "error: Yocto sources are missing; run scripts/setup-orin-nano.sh first" >&2
    exit 1
fi

cd "${src_dir}/openembedded-core"
TEMPLATECONF="${src_dir}/yocto-platforms/layers/meta-orin-nano/conf/templates/orin-nano" \
    . ./oe-init-build-env "${build_dir}"

bitbake "${target}"
