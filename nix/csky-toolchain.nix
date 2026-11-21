# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# The csky-elf cross toolchain (C-SKY CK803S): binutils, GCC (C only) and newlib in one prefix. This is the build
# script platform.toml [toolchain] build names for the toolchain id csky, as a derivation: the same three archives,
# the same configure flags and the same make targets, in the same order. Keep the two in step.
#
# It holds no release and no checksum. flake.nix passes all six: the GCC release from platform.toml, the rest read
# from the script, so the pins stay in one place.
#
# One derivation, not three: GCC looks for the target's as and ld under its own prefix, and newlib installs the
# headers, libc.a and the specs files where that GCC looks for them ($out/csky-elf). With one prefix nothing needs a
# wrapper, a sysroot or -B/-isystem flags, and the result has the layout the script installs and the CI image holds.
#
# Nothing here reads the archives: they are fetched by checksum and built as they are (docs/CLEANROOM.md).
{
  lib,
  stdenv,
  fetchurl,
  gmp,
  mpfr,
  libmpc,
  isl,
  zlib,
  texinfo,
  bison,
  flex,
  # the releases, and the SHA-256 of each one's archive as the build script writes it (hex)
  gccVersion,
  gccSha256,
  binutilsVersion,
  binutilsSha256,
  newlibVersion,
  newlibSha256,
}:

let
  binutilsSrc = fetchurl {
    url = "mirror://gnu/binutils/binutils-${binutilsVersion}.tar.xz";
    sha256 = binutilsSha256;
  };
  gccSrc = fetchurl {
    url = "mirror://gnu/gcc/gcc-${gccVersion}/gcc-${gccVersion}.tar.xz";
    sha256 = gccSha256;
  };
  newlibSrc = fetchurl {
    url = "https://sourceware.org/pub/newlib/newlib-${newlibVersion}.tar.gz";
    sha256 = newlibSha256;
  };

  target = "csky-elf";
  # A copy of the CPUFLAGS and LDSPECS of the platform.mk this compiler serves, for the check below that the
  # toolchain compiles and links for that CPU. Not read from platform.mk: an edit of either line would then build
  # the compiler again, and it is the firmware builds, with platform.mk's own flags, that prove those
  cpuFlags = "-mcpu=ck803s -mlittle-endian -mno-high-registers -msoft-float";
  ldSpecs = "-nostartfiles --specs=nosys.specs -Wl,--no-warn-rwx-segments";
in
stdenv.mkDerivation {
  pname = "${target}-toolchain";
  version = gccVersion;

  srcs = [
    binutilsSrc
    gccSrc
    newlibSrc
  ];
  sourceRoot = ".";

  strictDeps = true;
  nativeBuildInputs = [
    texinfo
    bison
    flex
  ];
  buildInputs = [
    gmp
    mpfr
    libmpc
    isl
    zlib
  ];

  # GCC builds parts of itself with -Wno-format, which the wrapper's -Werror=format-security rejects. The hardening
  # flags reach the host compiler only: the new csky-elf-gcc is never wrapped
  hardeningDisable = [ "format" ];
  enableParallelBuilding = true;

  # The tarballs are built as they were downloaded: stdenv would put nixpkgs' newer config.sub and config.guess in
  # them, which they don't need (theirs know csky-elf and every host this builds on)
  dontUpdateAutotoolsGnuConfigScripts = true;
  dontConfigure = true;

  # Each stage needs the one before it installed in the prefix, so the three are built and installed here, each in
  # its own directory beside its source, which is then deleted (the script's CLEAN=1: about 4 GB at the peak, not 7)
  buildPhase = ''
    runHook preBuild

    export PATH=$out/bin:$PATH
    jobs=''${enableParallelBuilding:+$NIX_BUILD_CORES}
    stage() {
      echo "csky-toolchain: $1 ($SECONDS s)"
      mkdir "b-$1"
      cd "b-$1"
    }
    stageDone() {
      cd ..
      rm -rf "b-$1" "$2"
    }

    # --with-system-zlib is not in the script's Linux build (it is in its macOS note): the tools link nixpkgs' zlib
    # instead of the copy in each tarball. It changes nothing in the code they generate
    stage binutils
    ../binutils-${binutilsVersion}/configure --target=${target} --prefix=$out --disable-nls \
      --disable-werror --disable-gdb --disable-sim --with-system-zlib
    make -j''${jobs:-1}
    make install-strip
    stageDone binutils binutils-${binutilsVersion}

    # No multilib: GCC 14 runs away (gigabytes of memory, no end) on the ck801 variants of newlib's strstr.c and
    # memmem.c, and the firmware is built for ck803s only
    stage gcc
    ../gcc-${gccVersion}/configure --target=${target} --prefix=$out --enable-languages=c \
      --with-cpu=ck803s --with-float=soft --disable-multilib \
      --without-headers --with-newlib --disable-nls --disable-shared --disable-threads --disable-libssp \
      --disable-libquadmath --disable-libgomp --disable-libatomic \
      --with-system-zlib \
      --with-gmp-include=${lib.getDev gmp}/include --with-gmp-lib=${lib.getLib gmp}/lib \
      --with-mpfr-include=${lib.getDev mpfr}/include --with-mpfr-lib=${lib.getLib mpfr}/lib \
      --with-mpc-include=${lib.getDev libmpc}/include --with-mpc-lib=${lib.getLib libmpc}/lib \
      --with-isl-include=${lib.getDev isl}/include --with-isl-lib=${lib.getLib isl}/lib
    make -j''${jobs:-1} all-gcc all-target-libgcc
    make install-strip-gcc install-target-libgcc
    stageDone gcc gcc-${gccVersion}

    # newlib's system.c needs -Wno-error=implicit-function-declaration with GCC 14
    stage newlib
    ../newlib-${newlibVersion}/configure --target=${target} --prefix=$out --disable-multilib \
      --enable-newlib-nano-malloc --disable-newlib-supplied-syscalls --enable-newlib-reent-small --disable-nls \
      CFLAGS_FOR_TARGET="-mcpu=ck803s -O2 -g -Wno-error=implicit-function-declaration"
    make -j''${jobs:-1}
    make install
    stageDone newlib newlib-${newlibVersion}

    echo "csky-toolchain: built ($SECONDS s)"
    runHook postBuild
  '';

  dontInstall = true;

  # install-strip stripped the host programs already. The host's strip and patchelf must not touch what is left: the
  # target's objects and archives (libgcc.a, libc.a, crt0.o), which the firmware links as they were built
  dontStrip = true;
  dontPatchELF = true;

  # So the RPATHs are shrunk in the directories that hold host programs only (patchELF is stdenv's own hook, there on
  # Linux): without it every program keeps the host compiler's library directory in its RPATH.
  # ar stamps an archive's symbol table with the time of the build, the one thing that differed between two builds of
  # this derivation: ranlib -D writes that index again with no time and leaves the members as they were built.
  # share/info/dir is an index that collides with any other package's in a profile
  postFixup = ''
    if [ "$(type -t patchELF)" = function ]; then
      for d in bin ${target}/bin libexec lib/bfd-plugins; do
        patchELF "$out/$d"
      done
    fi
    find $out/${target}/lib $out/lib/gcc/${target} -name '*.a' -exec $out/bin/${target}-ranlib -D {} \;
    rm -f $out/share/info/dir
  '';

  # Compiles and links as the firmware does: its CPU flags, its specs, newlib's stdio and soft-float through -lm,
  # then once more with LTO (the Makefile's LTO=1), which loads the linker plugin. It runs after the fixup, so it
  # checks the programs as they are in the store
  doInstallCheck = true;
  installCheckPhase = ''
    runHook preInstallCheck

    ${target}-gcc --version | head -1
    [ "$(${target}-gcc -dumpversion)" = "${gccVersion}" ]
    for f in csky-elf/lib/libc.a csky-elf/lib/libm.a csky-elf/lib/libnosys.a csky-elf/lib/nosys.specs \
      csky-elf/include/stdio.h lib/gcc/${target}/${gccVersion}/libgcc.a; do
      [ -f "$out/$f" ] || { echo "csky-toolchain: $out/$f is missing" >&2; exit 1; }
    done
    [ "$(${target}-gcc ${cpuFlags} -print-libgcc-file-name)" = "$out/lib/gcc/${target}/${gccVersion}/libgcc.a" ]

    mkdir check
    cat > check/check.c <<'EOF'
    #include <math.h>
    #include <stdint.h>
    #include <stdio.h>
    #include <string.h>
    volatile float in = 2.0f;
    volatile uint64_t wide = 3;
    char text[32];
    void _start(void)
    {
      snprintf(text, sizeof text, "%d %u", (int)(sqrtf(in) * 1000.0f), (unsigned)(wide / 3u));
      memcpy(text + 16, text, 8);
      for (;;) { }
    }
    EOF
    ${target}-gcc ${cpuFlags} -Os -g3 -ffunction-sections -fdata-sections -fno-common -std=gnu11 -Wall -Wextra \
      -Werror -c check/check.c -o check/check.o
    # nosys.specs warns once for every system call libnosys stubs: expected, the firmware has its own
    link() {
      ${target}-gcc ${cpuFlags} "$@" -Wl,--gc-sections -Wl,--entry=_start ${ldSpecs} -lm > check/link.log 2>&1 ||
        { cat check/link.log >&2; exit 1; }
    }
    link -o check/check.elf check/check.o
    ${target}-size check/check.elf
    ${target}-readelf -h check/check.elf | grep Machine:
    ${target}-nm check/check.elf > check/check.sym
    for s in _start snprintf sqrtf; do
      grep -q " [TW] $s\$" check/check.sym || { echo "csky-toolchain: $s is not in the linked check" >&2; exit 1; }
    done
    # .text only: the default linker script puts the data far from the code, and the gap would be in the file
    ${target}-objcopy -O binary -j .text check/check.elf check/check.bin
    [ -s check/check.bin ]

    ${target}-gcc ${cpuFlags} -Os -flto=auto -c check/check.c -o check/check-lto.o
    link -Os -flto=auto -o check/check-lto.elf check/check-lto.o
    ${target}-size check/check-lto.elf
    rm -rf check

    runHook postInstallCheck
  '';

  passthru = {
    inherit gccVersion binutilsVersion newlibVersion;
    targetPrefix = "${target}-";
  };

  meta = {
    description = "GCC (C only), binutils and newlib for ${target}: C-SKY CK803S, soft float, no multilib";
    homepage = "https://gcc.gnu.org/";
    license = with lib.licenses; [
      gpl3Plus
      lgpl3Plus
      gpl2Plus
      lgpl2Plus
      bsd3
    ];
    platforms = lib.platforms.linux;
    mainProgram = "${target}-gcc";
  };
}
