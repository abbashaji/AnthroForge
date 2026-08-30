#!/usr/bin/env bash
# Builds rust-core in release mode and copies the resulting dylib into the
# Unreal plugin's Binaries/ThirdParty directory, under the platform-specific
# name UAnthroforgeCharacterAssembler expects (see GetPlatformDylibFileName
# in AnthroforgeCharacterAssembler.cpp).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUST_CORE_DIR="${SCRIPT_DIR}/rust-core"
DEST_DIR="${SCRIPT_DIR}/unreal-plugin/Source/AnthroforgeEngine/Binaries/ThirdParty"

echo "[copy_lib] building rust-core (release)..."
( cd "${RUST_CORE_DIR}" && cargo build --release )

mkdir -p "${DEST_DIR}"

UNAME_S="$(uname -s)"
case "${UNAME_S}" in
	Linux*)
		SRC="${RUST_CORE_DIR}/target/release/libanthroforge_core.so"
		DEST="${DEST_DIR}/libanthroforge_core.so"
		;;
	Darwin*)
		SRC="${RUST_CORE_DIR}/target/release/libanthroforge_core.dylib"
		DEST="${DEST_DIR}/libanthroforge_core.dylib"
		;;
	*)
		echo "[copy_lib] unrecognized platform '${UNAME_S}'; if this is Windows, use copy_lib.bat instead." >&2
		exit 1
		;;
esac

if [[ ! -f "${SRC}" ]]; then
	echo "[copy_lib] expected build output not found at '${SRC}'." >&2
	exit 1
fi

cp -f "${SRC}" "${DEST}"
echo "[copy_lib] copied $(basename "${SRC}") -> ${DEST}"
