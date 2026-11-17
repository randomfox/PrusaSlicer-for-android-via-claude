#!/usr/bin/env bash
# Create local source archives for dependencies hosted as GitHub source archives
# (github.com/<owner>/<repo>/archive/<ref>.zip|tar.gz), using plain `git` instead of
# the archive download endpoint.
#
# Useful for offline builds or sandboxes where GitHub's archive endpoint is not
# reachable but git is. The archives are written to <out_dir>/<Package>.tar.gz and are
# picked up by the deps build when configured with -DDEP_SOURCES_DIR=<out_dir>.
#
# Note: the archives are produced from the exact pinned tag/commit, but they are not
# byte-identical to GitHub's archives, so the URL_HASH check is skipped for them.
#
# Usage: tools/android/mirror-deps.sh <out_dir> [Package ...]
set -euo pipefail

out_dir="$(mkdir -p "$1" && cd "$1" && pwd)"
shift
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
work_dir="${out_dir}/.git-cache"
mkdir -p "${work_dir}"

if [[ $# -gt 0 ]]; then
    packages=("$@")
else
    packages=()
    for f in "${repo_root}"/deps/+*/; do
        name="$(basename "$f")"; packages+=("${name#+}")
    done
fi

for pkg in "${packages[@]}"; do
    cmake_file="${repo_root}/deps/+${pkg}/${pkg}.cmake"
    [[ -f "${cmake_file}" ]] || continue
    url="$(grep -oE 'https://github\.com/[^/]+/[^/]+/archive/[^" )]+' "${cmake_file}" | head -n1 || true)"
    [[ -n "${url}" ]] || continue
    if [[ -f "${out_dir}/${pkg}.tar.gz" ]]; then
        echo "[skip] ${pkg}: archive exists"
        continue
    fi

    owner_repo="$(echo "${url}" | sed -E 's#https://github.com/([^/]+/[^/]+)/archive/.*#\1#')"
    ref="$(echo "${url}" | sed -E 's#.*/archive/##; s#^refs/tags/##; s#\.(zip|tar\.gz)$##')"
    repo="${owner_repo#*/}"
    cache="${work_dir}/${owner_repo//\//_}.git"

    echo "[git ] ${pkg}: ${owner_repo} @ ${ref}"
    if [[ ! -d "${cache}" ]]; then
        git init -q --bare "${cache}"
        git -C "${cache}" remote add origin "https://github.com/${owner_repo}"
    fi
    if [[ "${ref}" =~ ^[0-9a-f]{40}$ ]]; then
        git -C "${cache}" fetch -q --depth 1 origin "${ref}"
        commit="${ref}"
    else
        git -C "${cache}" fetch -q --depth 1 origin "refs/tags/${ref}:refs/tags/${ref}"
        commit="refs/tags/${ref}"
    fi
    git -C "${cache}" archive --format=tar.gz --prefix="${repo}-${ref#v}/" "${commit}" \
        > "${out_dir}/${pkg}.tar.gz.part"
    mv "${out_dir}/${pkg}.tar.gz.part" "${out_dir}/${pkg}.tar.gz"
done
