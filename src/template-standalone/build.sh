#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

if [[ ${1:-} == clean ]]; then
	rm -rf -- "$project_root/build" "$project_root/bin"
	exit 0
fi

cmake -S "$project_root" -B "$project_root/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$project_root/build" --parallel
