#!/usr/bin/env bash

MIN_CLANG_TIDY_VERSION=9

die()
{
    echo "$@" >&2
    exit 1
}

cd "${0%/*}/.." || die "Couldn't cd into the project root directory"
ROOT=$(pwd)

command -v clang-tidy > /dev/null || die "clang-tidy not found"
clang_tidy_version=$(clang-tidy --version | sed -ne 's/[^0-9]*\([0-9]*\)\..*/\1/p')
[ "$clang_tidy_version" -ge $MIN_CLANG_TIDY_VERSION ] || \
    die "clang-tidy v$MIN_CLANG_TIDY_VERSION+ required; found v$clang_tidy_version"

set -e

# We use separate .clang-tidy for the production code and the test suite.
# The latter is held to less stringent standards and is not an ideomatic C++ because it is designed to test C code.

pushd o1heap
clang-tidy ./*.c
popd

pushd tests
clang-tidy ./*.cpp -- -I"$ROOT/o1heap" -Icatch -std=c++17
popd
