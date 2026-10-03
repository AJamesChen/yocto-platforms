#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "${script_dir}/.." && pwd)
kas_file="${project_dir}/kas/rpi4b.yml"
build_dir="${project_dir}/build/rpi4b"

if ! command -v kas >/dev/null 2>&1; then
    echo "error: kas is not installed (one option: pipx install kas)" >&2
    exit 1
fi

mkdir -p "${build_dir}"
export KAS_WORK_DIR="${build_dir}"

exec kas shell "${kas_file}"
