# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# One firmware image: make BOARD=<board> <flags>, as docs/dev/building.md has it.
#
#   $out   the one file to flash, under the name the Makefile gives it: the image with the first stage in front
#          (rfoundry_<id>_install.bin) for a board that has one, else rfoundry_<id>.bin
#   $elf   its .elf, for a debugger and tools/crash_decode.py
#
# The .elf names the compiler's store path, so it has its own output: $out then depends on nothing and its closure
# is the image alone. The .map is not kept: with LTO it lists the compiler's temporary files, whose names are random,
# and two builds of one source would differ.
{
  lib,
  stdenvNoCC,
  gnumake,
  python3,
  # the store name of the image, its board, and the make variables that tell it from the board's default build
  name,
  board,
  flags ? [ ],
  src,
  # shell that fills third_party/, and the make variable that says where the board's compiler is
  linkThirdParty,
  toolchainEnv,
  # the commit as the Makefile writes it; its time in Unix seconds, empty when unknown; the build date
  version,
  gitTime,
  epoch,
}:
stdenvNoCC.mkDerivation {
  pname = "rfoundry-${name}";
  inherit version src;
  outputs = [
    "out"
    "elf"
  ];
  # Any Python 3 runs the build's generators; make is also what the compiler's LTO step runs its jobs with
  nativeBuildInputs = [
    gnumake
    python3
  ];
  # SOURCE_DATE_EPOCH is __DATE__ and __TIME__ in the image, which stdenv would leave at 1980. Without
  # PYTHONDONTWRITEBYTECODE the Makefile's Python tools write __pycache__ into the tree they run from
  env = toolchainEnv // {
    SOURCE_DATE_EPOCH = epoch;
    PYTHONDONTWRITEBYTECODE = "1";
  };
  dontConfigure = true;
  # nothing here is for the host's strip and patchelf
  dontFixup = true;

  # No git in the sandbox, so the Makefile would stamp the image "unknown": the commit and its time come in as the
  # variables it documents for a tree copied elsewhere. RELEASE= alone means no release name, and stops it asking
  # git for a tag. BUILD=build: one fixed directory, whatever the flags would name it.
  # make exits 0 on a warning, so the log is searched as tools/ci.sh does (AGENTS.md rule 6); pipefail keeps make's
  # own status through tee
  buildPhase = ''
    runHook preBuild
    ${linkThirdParty}
    set -o pipefail
    make -j$NIX_BUILD_CORES BOARD=${board} BUILD=build RFOUNDRY_VERSION=${version} RFOUNDRY_GIT_TIME=${gitTime} \
      RELEASE= ${lib.escapeShellArgs flags} 2>&1 | tee build.log
    if grep -i 'warning:' build.log; then
      echo "${name}: the build has warnings (above)" >&2
      exit 1
    fi
    runHook postBuild
  '';
  # codecs/ holds no stock firmware in a flake's source, so make can't have merged a codec (AGENTS.md rule 2). It is
  # checked all the same: a store path is public once it is in a cache.
  # The image must not name a store path either, or a consumer's closure would carry the compiler
  installPhase = ''
    runHook preInstall
    if [ -n "$(find build -maxdepth 1 -name '*_codec.bin')" ]; then
      echo "${name}: the build made an image with the codec, which must never be in the store" >&2
      exit 1
    fi
    set -- build/rfoundry_*_install.bin
    [ -e "$1" ] || set -- build/rfoundry_*.bin
    if [ $# != 1 ] || [ ! -e "$1" ]; then
      echo "${name}: expected one image to flash in build/, found: $*" >&2
      exit 1
    fi
    install -Dm644 -t $out "$1"
    install -Dm644 -t $elf build/rfoundry_*.elf
    if grep -rl /nix/store $out; then
      echo "${name}: the image names a store path (above)" >&2
      exit 1
    fi
    runHook postInstall
  '';

  meta = {
    description = "RadioFoundry firmware image ${name}, without the codec";
    platforms = lib.platforms.linux;
  };
}
