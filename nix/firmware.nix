# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# One firmware image: make BOARD=<board> <flags>, as docs/dev/building.md has it.
#
#   $out    the one file to flash, under the name the Makefile gives it: with the codec merged in
#           (rfoundry_<id>_codec.bin) when stock is given, else the image with the first stage in front
#           (rfoundry_<id>_install.bin) for a board that has one, else rfoundry_<id>.bin
#   $plain  with stock only: that image without the codec
#   $elf    its .elf, for a debugger and tools/crash_decode.py
#
# The .elf names the compiler's store path, so it has its own output: $out then depends on nothing and its closure
# is the image alone. The .map is not kept: with LTO it lists the compiler's temporary files, whose names are random,
# and two builds of one source would differ.
{
  lib,
  stdenvNoCC,
  gnumake,
  python3,
  unzip,
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
  # the board's stock firmware download (board.toml [codec] download, fetched), and the stock file's path inside it
  # when it is a zip: make merges the codec from it as from a file in codecs/ (codecs/README.md)
  stock ? null,
  stockFile ? null,
}:
let
  codec = stock != null;
in
stdenvNoCC.mkDerivation {
  pname = "rfoundry-${name}";
  inherit version src;
  outputs = [
    "out"
    "elf"
  ]
  ++ lib.optional codec "plain";
  # Any Python 3 runs the build's generators; make is also what the compiler's LTO step runs its jobs with
  nativeBuildInputs = [
    gnumake
    python3
  ]
  ++ lib.optional (stockFile != null) unzip;
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
    ${lib.optionalString codec ''
      # codec_auto.py takes the first supported file in codecs/: the download must be the only one there, whatever
      # a path: flake brought in untracked
      find codecs -mindepth 1 ! -name README.md ! -name .gitignore -exec rm -rf {} +
      ${
        if stockFile != null then
          "unzip -j -d codecs ${stock} ${lib.escapeShellArg stockFile}"
        else
          "cp ${stock} codecs/"
      }
    ''}
    set -o pipefail
    make -j$NIX_BUILD_CORES BOARD=${board} BUILD=build RFOUNDRY_VERSION=${version} RFOUNDRY_GIT_TIME=${gitTime} \
      RELEASE= ${lib.escapeShellArgs flags} 2>&1 | tee build.log
    if grep -i 'warning:' build.log; then
      echo "${name}: the build has warnings (above)" >&2
      exit 1
    fi
    runHook postBuild
  '';
  # With stock, make must have merged the codec: codec_auto.py says "no supported ... stock firmware" and exits 0
  # when the file is not one the codec tool knows. Without it codecs/ holds no stock file in a git flake's source
  # (AGENTS.md rule 2), and a codec image means one came in untracked, through path:.
  # The image must not name a store path either, or a consumer's closure would carry the compiler
  installPhase = ''
    runHook preInstall
    codec=$(find build -maxdepth 1 -name 'rfoundry_*_codec.bin')
    ${
      if codec then
        ''
          if [ -z "$codec" ]; then
            echo "${name}: make merged no codec from ${stock}${
              lib.optionalString (stockFile != null) " (${stockFile})"
            }: not a stock file the codec tool supports (above)" >&2
            exit 1
          fi
          install -Dm644 -t $out $codec
        ''
      else
        ''
          if [ -n "$codec" ]; then
            echo "${name}: the build merged a codec from a file in codecs/ that git does not track" >&2
            exit 1
          fi
        ''
    }
    set -- build/rfoundry_*_install.bin
    [ -e "$1" ] || set -- $(find build -maxdepth 1 -name 'rfoundry_*.bin' ! -name '*_codec.bin')
    if [ $# != 1 ] || [ ! -e "$1" ]; then
      echo "${name}: expected one image to flash in build/, found: $*" >&2
      exit 1
    fi
    install -Dm644 -t ${if codec then "$plain" else "$out"} "$1"
    install -Dm644 -t $elf build/rfoundry_*.elf
    if grep -rl /nix/store $out ${lib.optionalString codec "$plain"}; then
      echo "${name}: the image names a store path (above)" >&2
      exit 1
    fi
    runHook postInstall
  '';

  meta = {
    description =
      "RadioFoundry firmware image ${name}, "
      + (if codec then "with the codec from the stock firmware" else "without the codec");
    platforms = lib.platforms.linux;
  };
}
