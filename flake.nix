# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# The firmware image of every board under boards/, the compilers that build them and a development shell
# (docs/dev/nix.md). Every build is the Makefile's own: this file only says where the compiler and third_party/ are.
#
# No radio, platform or compiler release is named here. Boards are read from boards/, their compilers from
# platform/*/platform.toml, the submodules from .gitmodules, the variants from config/variants/. The evaluation stops
# with a message when one of those no longer reads as this file expects, or when nixpkgs has another compiler than the
# one pinned.
{
  description = "RadioFoundry: MIT-licensed firmware for DMR radios";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    # One input per submodule in .gitmodules, named as its directory under third_party/, at the commit its link
    # records: a flake's source holds no submodule content. A flake can't read a link, so tools/check_flake_pins.py
    # (tools/ci.sh) compares the two records. Bump them together, then run `nix flake lock`
    FreeRTOS-Kernel.url = "github:FreeRTOS/FreeRTOS-Kernel/3a22924e0a9ddbbc8b0758881c33b3422a5cc20d";
    FreeRTOS-Kernel.flake = false;
    cmsis-core.url = "github:STMicroelectronics/cmsis-core/b7487de1303e2eb77c402432e368691e9dcfb5f0";
    cmsis-core.flake = false;
    cmsis-device-f4.url = "github:STMicroelectronics/cmsis-device-f4/0fa0e489e053fa1ca7790bb40b4d76458f64c55d";
    cmsis-device-f4.flake = false;
    stm32f4xx-hal-driver.url = "github:STMicroelectronics/stm32f4xx-hal-driver/b6f0ed3829f3829eb358a2e7417d80bba1a42db7";
    stm32f4xx-hal-driver.flake = false;
    tinyusb.url = "github:hathach/tinyusb/1a0e4020ecbf439b75b6b6fd7e8f4127c873d070";
    tinyusb.flake = false;
  };

  outputs =
    { self, nixpkgs, ... }@inputs:
    let
      inherit (nixpkgs) lib;
      fs = lib.fileset;
      # Everything is built and checked on x86_64-linux. aarch64-linux evaluates and is untested
      forAllSystems =
        f: lib.genAttrs [ "x86_64-linux" "aarch64-linux" ] (s: f nixpkgs.legacyPackages.${s});

      toml = file: builtins.fromTOML (builtins.readFile file);
      # a path of this tree as the repository names it, for a message
      rel = file: lib.removePrefix "${toString ./.}/" (toString file);
      # the first group of regex on each line of file it matches whole
      linesOf =
        regex: file:
        lib.concatMap (
          line:
          let
            m = builtins.match regex line;
          in
          lib.optional (m != null) (lib.head m)
        ) (lib.splitString "\n" (builtins.readFile file));
      # The same, for a value the file must hold on exactly one line. No line or several means the file changed its
      # form: stopping is better than building with half a pin
      grep1 =
        what: regex: file:
        let
          found = linesOf regex file;
        in
        if lib.length found == 1 then
          lib.head found
        else
          throw ''
            flake.nix reads ${what} from ${rel file}, on the one line that matches
              ${regex}
            ${toString (lib.length found)} lines match. Add or restore that line, or change the pattern in flake.nix to
            the file's new form.'';
      # the directories under dir that hold file
      having =
        file: dir:
        lib.attrNames (
          lib.filterAttrs (n: _: builtins.pathExists (dir + "/${n}/${file}")) (builtins.readDir dir)
        );

      # A board is a directory under boards/ with a board.mk, as in the Makefile. board.mk names its platform;
      # platform.toml [toolchain] is that platform's compiler pin
      boards = having "board.mk" ./boards;
      pins = lib.genAttrs (having "platform.toml" ./platform) (
        p:
        (toml (./platform + "/${p}/platform.toml")).toolchain or (throw ''
          platform/${p}/platform.toml has no [toolchain] table. flake.nix reads the platform's compiler pin from
          it (docs/dev/board-registry.md): add the table.'')
        // {
          file = "platform/${p}/platform.toml";
        }
      );
      pinOfBoard =
        b:
        let
          p = grep1 "the board's platform" "PLATFORM[ \t]*:=[ \t]*([A-Za-z0-9_]+)[ \t]*" (
            ./boards + "/${b}/board.mk"
          );
        in
        pins.${p} or (throw ''
          boards/${b}/board.mk has PLATFORM := ${p}, and platform/${p}/platform.toml does not exist.
          flake.nix reads the board's compiler from that file's [toolchain] table: add it, or correct the board.mk.'');

      # The images: a board's default build, and one more for each other value of a build option (board.toml
      # [[build_options]]: hardware versions one image can't serve; the first value is the default). name is the
      # Makefile's build directory name and the package's; flags are the make variables beside BOARD; toolchainId
      # says which compiler builds it
      images =
        let
          all = lib.concatMap (
            b:
            let
              image = name: flags: {
                inherit name flags;
                board = b;
                toolchainId = (pinOfBoard b).id;
              };
            in
            [ (image b [ ]) ]
            ++ lib.concatMap (o: map (v: image "${b}-${v.id}" [ "${o.name}=${v.id}" ]) (lib.tail o.values)) (
              (toml (./boards + "/${b}/board.toml")).build_options or [ ]
            )
          ) boards;
          names = map (i: i.name) all;
          # the images share packages.<system> with these, and with each other
          own = [
            "default"
          ]
          ++ lib.concatMap (id: [
            "firmware-${id}"
            "toolchain-${id}"
          ]) toolchainIds;
          taken = lib.intersectLists own names ++ lib.filter (n: lib.count (m: m == n) names > 1) names;
        in
        assert lib.assertMsg (taken == [ ]) ''
          flake.nix: two packages would be named ${lib.concatStringsSep ", " (lib.unique taken)}.
          An image's package is named <board> or <board>-<build option value>.
          default, firmware-<toolchain id> and toolchain-<toolchain id> are this flake's own.
          Rename the board directory or the option value (board.toml [[build_options]]), or the flake's own package
          in flake.nix.'';
        all;
      variants = map (lib.removeSuffix ".mk") (
        lib.filter (lib.hasSuffix ".mk") (lib.attrNames (builtins.readDir ./config/variants))
      );

      # The compilers, by platform.toml [toolchain] id. package is the compiler. dirVar is the make variable that
      # says where it is: without it platform.mk takes a compiler under $HOME first, and not every one looks on PATH.
      # version, and sha256 where there is one, are the repository's pin. The compiler here must be that release and
      # no newer one: zero warnings (AGENTS.md rule 6) is only proven on it
      toolchainIds = lib.naturalSort (lib.unique (lib.mapAttrsToList (_: pin: pin.id) pins));
      toolchain =
        pkgs: id:
        let
          pin = lib.findFirst (pin: pin.id == id) null (lib.attrValues pins);
        in
        {
          arm =
            let
              attr = "gcc-arm-embedded-${lib.versions.major pin.version}";
              package =
                pkgs.${attr} or (throw ''
                  ${pin.file} pins the Arm GNU Toolchain ${pin.version}, and the nixpkgs in flake.lock has no ${attr}.
                  Lock a nixpkgs that has it (nix flake lock --override-input nixpkgs github:NixOS/nixpkgs/<commit>),
                  or set [toolchain] version and sha256 in ${pin.file} to a release nixpkgs has (docs/dev/nix.md,
                  Maintaining the flake).'');
              # The pinned checksum is the Linux x86_64 archive's, and a pin need not have one: nothing to compare
              # then, or on another system. In the file's hex form, whichever form nixpkgs writes its own in
              pinned = pin.sha256 or null;
              archive =
                if pinned != null && pkgs.stdenv.hostPlatform.system == "x86_64-linux" then
                  builtins.convertHash {
                    hash = package.src.outputHash;
                    hashAlgo = "sha256";
                    toHashFormat = "base16";
                  }
                else
                  pinned;
            in
            assert lib.assertMsg (package.version == pin.version) ''
              ${pin.file} pins the Arm GNU Toolchain ${pin.version}, and the nixpkgs in flake.lock has
              ${package.version} as ${attr}.
              Lock a nixpkgs with the pinned release (nix flake lock --override-input nixpkgs
              github:NixOS/nixpkgs/<commit>), or set [toolchain] version and sha256 in ${pin.file} to the release
              nixpkgs has (docs/dev/nix.md, Maintaining the flake).'';
            assert lib.assertMsg (archive == pinned) ''
              ${pin.file} pins the Arm GNU Toolchain archive with the SHA-256
                ${pinned}
              and nixpkgs builds ${attr} from an archive with
                ${archive}
              They must be one archive: lock a nixpkgs whose ${attr} is the pinned release, or correct the pin.'';
            {
              inherit package;
              dirVar = "ARM_GCC_DIR";
            };
          # GCC's release is the pin; the binutils and newlib releases and the three checksums are in the script
          # that builds the toolchain outside Nix, which platform.toml names. Read from there, not copied
          csky =
            let
              script = ./. + "/${pin.build}";
            in
            {
              package = pkgs.callPackage ./nix/csky-toolchain.nix {
                gccVersion = pin.version;
                gccSha256 =
                  grep1 "the SHA-256 of gcc-${pin.version} (${pin.file} [toolchain] version)"
                    "${lib.escapeRegex pin.version}\\) GSUM=([0-9a-f]{64}) ;;"
                    script;
                binutilsVersion = grep1 "the binutils release" "BV=([0-9.]+);.*" script;
                binutilsSha256 = grep1 "the SHA-256 of binutils" "([0-9a-f]{64})  binutils-\\$BV\\.tar\\.xz" script;
                newlibVersion = grep1 "the newlib release" "BV=.*; NV=([0-9.]+);.*" script;
                newlibSha256 = grep1 "the SHA-256 of newlib" "([0-9a-f]{64})  newlib-\\$NV\\.tar\\.gz" script;
              };
              dirVar = "CSKY_GCC_DIR";
            };
        }
        .${id} or (throw ''
          flake.nix has no compiler for the toolchain id "${id}" (${pin.file} [toolchain] id).
          Add it to the table in flake.nix (toolchain): its package, and the make variable its platform.mk reads the
          compiler's directory from.'');
      # <dirVar>=<package>/bin/ for each id. The slash is part of it: platform.mk puts the compiler's name after it
      toolchainEnv =
        pkgs: ids:
        lib.listToAttrs (map (id: with toolchain pkgs id; lib.nameValuePair dirVar "${package}/bin/") ids);

      # The commit as the Makefile writes it (8 digits, -dirty; shortRev has 7), and its time: the debug link's
      # git_time=. A dirty tree has its commit's time, where the Makefile would take the clock.
      # A flake that is no git tree (path:, a tarball) has neither. A tarball has a lastModified all the same, its
      # newest file's date, which is no commit's time: the commit decides, not the date
      isGit = self ? rev || self ? dirtyRev;
      version =
        if isGit then
          builtins.substring 0 8 (self.rev or self.dirtyRev) + lib.optionalString (self ? dirtyRev) "-dirty"
        else
          "unknown";
      gitTime = lib.optionalString isGit (toString self.lastModified);
      # The build date in the image (SOURCE_DATE_EPOCH): the commit's day, else the day of the locked nixpkgs
      epoch = toString (if isGit then self.lastModified else nixpkgs.lastModified);

      # third_party/: where a submodule is, a flake's source has an empty directory or, once filtered, none. Each
      # build links the inputs there instead. In a check that makes a commit of the tree this runs after it, so
      # nothing that lists tracked files sees them
      linkThirdParty = lib.concatMapStrings (name: ''
        mkdir -p third_party && rm -rf third_party/${name}
        ln -s ${
          inputs.${name} or (throw ''
            .gitmodules has third_party/${name}, and flake.nix has no input named ${name}.
            Add it to inputs in flake.nix as the others are (flake = false, at the commit that
            `git ls-tree HEAD third_party/` shows), then run: nix flake lock'')
        } third_party/${name}
      '') (linesOf "[ \t]*path[ \t]*=[ \t]*third_party/([^ \t]+)[ \t]*" ./.gitmodules);

      # What no firmware build reads stays out of its source, so editing it rebuilds no image. Taken away from the
      # whole tree, not listed: the Makefile finds its sources with wildcards, so a directory missing from a list
      # of what it reads could build without an error. One missing here costs a rebuild.
      # notBuilt is what neither the firmware nor the host tests read: the docs, the Markdown pages at the root, the
      # Nix files, and site/ but for the radio drawings, which the web control page embeds
      notBuilt = [
        ./docs
        ./nix
        ./flake.nix
        ./flake.lock
        (fs.difference ./site ./site/static/img/radios/src)
      ]
      ++ map (n: ./. + "/${n}") (
        lib.attrNames (
          lib.filterAttrs (n: kind: kind == "regular" && lib.hasSuffix ".md" n) (builtins.readDir ./.)
        )
      );
      firmwareSrc = fs.toSource {
        root = ./.;
        fileset = fs.difference ./. (
          fs.unions (
            notBuilt
            ++ [
              ./tests
              ./.github
            ]
          )
        );
      };
      # A board whose board.toml [codec] has a download (the maker's stock firmware, by URL and SHA-256) gets the
      # image with the codec merged from it, as make merges it from a file in codecs/
      firmware =
        pkgs: image: variant:
        let
          download = (toml (./boards + "/${image.board}/board.toml")).codec.download or null;
        in
        pkgs.callPackage ./nix/firmware.nix {
          inherit (image) board;
          inherit
            version
            gitTime
            epoch
            linkThirdParty
            ;
          name = image.name + lib.optionalString (variant != null) "-${variant}";
          flags = image.flags ++ lib.optional (variant != null) "VARIANT=${variant}";
          src = firmwareSrc;
          toolchainEnv = toolchainEnv pkgs [ image.toolchainId ];
          stock =
            if download == null then
              null
            else
              pkgs.fetchurl {
                inherit (download) url sha256;
                name = "${image.board}-stock-firmware" + lib.optionalString (download ? file) ".zip";
              };
          stockFile = download.file or null;
        };
      # .#<image>, and .#<image>.variants.<variant> for the same image as a release variant
      # (config/variants/<variant>.mk). Other switches are not packages: nix develop -c make BOARD=<board> CONFIG_X=n
      imagePackages =
        pkgs: select:
        lib.listToAttrs (
          map (
            i:
            lib.nameValuePair i.name (
              firmware pkgs i null // { variants = lib.genAttrs variants (firmware pkgs i); }
            )
          ) (lib.filter select images)
        );
      # Several images in one directory, under the Makefile's file names, with a SHA256SUMS over them
      collection =
        pkgs: name: packages:
        pkgs.runCommand "rfoundry-${name}" { } ''
          mkdir $out
          for image in ${toString (lib.attrValues packages)}; do
            cp $image/*.bin $out/
          done
          cd $out
          touch SHA256SUMS
          for f in *.bin; do
            [ ! -e "$f" ] || sha256sum "$f" >> SHA256SUMS
          done
        '';

      # What the tools, the host tests and the site build import (tools/ci.sh's python step)
      python =
        pkgs:
        pkgs.python3.withPackages (ps: [
          ps.pytest
          ps.numpy
          ps.pyserial
          ps.pyusb
          ps.markdown-it-py
          ps.resvg-py
        ]);
      # make BOARD=<board>, make -C tests/host and tools/ci.sh as they are, with the compilers of ids.
      # third_party/ is the checkout's own here: tools/checkout-submodules.sh
      shell =
        pkgs: ids:
        pkgs.mkShell {
          packages = [
            (python pkgs)
            pkgs.gnumake
            pkgs.git
            # the web flasher's tests, the pcap checks of the host tests, tools/coverage.sh
            pkgs.nodejs
            pkgs.wireshark-cli
            pkgs.gcovr
          ]
          # make finds the compilers through env below. On PATH they serve tools/crash_decode.py (addr2line) and
          # a debugger
          ++ map (id: (toolchain pkgs id).package) ids;
          # the host tests' simulator writes PNGs
          buildInputs = [ pkgs.zlib ];
          # The host tests define __attribute__ away (tests/host/Makefile), which glibc's fortified headers don't
          # survive
          hardeningDisable = [
            "fortify"
            "fortify3"
          ];
          env = toolchainEnv pkgs ids // {
            RFOUNDRY_PYTHON = "${python pkgs}/bin/python3";
          };
          # mkShell's 1980 would be the firmware's build date, and a host test refuses a date that old
          shellHook = "unset SOURCE_DATE_EPOCH";
        };

      # The checks: tools/ci.sh a step or two at a time (nix/ci.nix), as the CI workflows run it. skipOk is what a
      # run may skip, and anything else skipped fails it. The build date is the locked nixpkgs', not the commit's:
      # a check then runs again only when its source or the lock changed
      ci =
        pkgs: name: args:
        pkgs.callPackage ./nix/ci.nix (
          {
            inherit name;
            python = python pkgs;
            epoch = toString nixpkgs.lastModified;
          }
          // args
        );
      # What the host tests and the bench tools build from. Taken away from the whole tree, as for the firmware
      hostSrc = fs.toSource {
        root = ./.;
        fileset = fs.difference ./. (fs.unions notBuilt);
      };
      # CI's firmware job of one compiler. A directory under boards/ that boards share has no board.mk and is no
      # board: tools/ci.sh says so with a skip. The shared third_party/ test makes a worktree of a commit with the
      # submodule links, which the copy has not: skipped too
      firmwareStep = id: {
        src = firmwareSrc;
        args = "--quick --steps firmware --toolchain ${id}";
        inherit linkThirdParty;
        skipOk = "[^ ]+: no boards/[^ ]+/board\\.mk yet .*|shared third_party: no submodule links .*";
      };
      ciChecks =
        pkgs:
        {
          # A copy of the tree has no base branch and no submodule links to compare, and the tree doesn't pass
          # reuse lint yet (docs/dev/ci.md), so reuse is not installed here
          ci-registry = ci pkgs "registry" {
            src = self;
            args = "--steps registry,licenses";
            withGit = true;
            skipOk = "radio floor: no .*|flake pins: no submodule links .*|reuse lint: reuse not installed .*";
          };
          # Without a commit the step would find no Python file to parse and still pass
          ci-python = ci pkgs "python" {
            src = self;
            args = "--steps python";
            withGit = true;
            tools = [ pkgs.nodejs ];
            postRun = ''
              if ! grep -Eq '^ci: ok    python: [1-9][0-9]* files parse' $out/ci.log; then
                echo "python: tools/ci.sh parsed no Python file. It lists them with git ls-files." >&2
                exit 1
              fi
            '';
          };
          # The host tests twice, the second time under ASan and UBSan with the options of CI's unit tests job, then
          # that job's gallery step. The Makefile leaves the pcap checks out without a word when tshark is missing,
          # so their lines are looked for
          ci-host = ci pkgs "host" {
            src = hostSrc;
            args = "--steps host,bench";
            tools = [ pkgs.wireshark-cli ];
            preRun = ''
              export MAKEFLAGS=-j$NIX_BUILD_CORES RFOUNDRY_CI_SANITIZE=1
              export ASAN_OPTIONS=detect_leaks=0:detect_stack_use_after_return=1
            '';
            postRun = ''
              if ! grep -q '^# tshark' build/ci/host-tests.log; then
                echo "host: the pcap checks did not run: build/ci/host-tests.log has no '# tshark' line." >&2
                exit 1
              fi
              make -s -C tests/host uiSim
              tests/host/uiSim --gallery build/ci/gallery > $out/gallery.log || {
                grep FAIL $out/gallery.log
                exit 1
              }
            '';
          };
          # The rule that a skip fails, tried: the first firmware job with its compiler nowhere. tools/ci.sh skips
          # those boards and would exit 0
          ci-skip-guard = ci pkgs "skip-guard" (
            firmwareStep (lib.head toolchainIds)
            // {
              toolchainEnv.${(toolchain pkgs (lib.head toolchainIds)).dirVar} = "/nonexistent/";
              failsWith = "^ci: FAIL  skipped: [^ ]+: compiler /nonexistent/";
            }
          );
        }
        // lib.listToAttrs (
          map (
            id:
            lib.nameValuePair "ci-firmware-${id}" (
              ci pkgs "firmware-${id}" (firmwareStep id // { toolchainEnv = toolchainEnv pkgs [ id ]; })
            )
          ) toolchainIds
        );
      # The images flake.nix reads from boards/ against the board registry's, which is what tools/ci.sh builds:
      # "<image> <toolchain id> <make variables>" a line, both ways
      registryCheck =
        pkgs:
        pkgs.runCommand "rfoundry-flake-registry"
          {
            nativeBuildInputs = [ pkgs.python3 ];
            env.PYTHONDONTWRITEBYTECODE = "1";
            fromFlake = lib.concatMapStrings (
              i: "${i.name} ${i.toolchainId} ${toString ([ "BOARD=${i.board}" ] ++ i.flags)}\n"
            ) images;
            passAsFile = [ "fromFlake" ];
          }
          ''
            cd ${firmwareSrc}
            for id in ${toString toolchainIds}; do
              boards=$(python3 tools/boards.py list --toolchain $id --comma)
              for b in ''${boards//,/ }; do
                echo "$b $id BOARD=$b"
                python3 tools/boards.py builds $b > $TMPDIR/builds
                while read -r value flags; do
                  echo "$b-$value $id BOARD=$b $flags"
                done < $TMPDIR/builds
              done
            done > $TMPDIR/registry
            mkdir $out
            LC_ALL=C sort $TMPDIR/registry > $out/registry
            LC_ALL=C sort $fromFlakePath > $out/flake
            if ! diff -u $out/registry $out/flake; then
              echo "flake.nix and tools/boards.py read different images from boards/ (above: - the registry's," >&2
              echo "+ the flake's). Change how flake.nix reads them (images) to the registry's rules." >&2
              exit 1
            fi
          '';
    in
    {
      packages = forAllSystems (
        pkgs:
        imagePackages pkgs (_: true)
        # every image, and the images of one compiler (the other may have to be built first, from source)
        // {
          default = collection pkgs "firmware-${version}" (imagePackages pkgs (_: true));
        }
        // lib.listToAttrs (
          lib.concatMap (id: [
            (lib.nameValuePair "firmware-${id}" (
              collection pkgs "firmware-${id}-${version}" (imagePackages pkgs (i: i.toolchainId == id))
            ))
            # the compiler alone, to build it ahead or push it to a cache
            (lib.nameValuePair "toolchain-${id}" (toolchain pkgs id).package)
          ]) toolchainIds
        )
      );
      # .#default has every compiler, .#<toolchain id> one
      devShells = forAllSystems (
        pkgs:
        lib.genAttrs toolchainIds (id: shell pkgs [ id ])
        // {
          default = shell pkgs toolchainIds;
        }
      );
      # What `nix flake check` builds: every image, the pull request gate, and that this file agrees with the
      # registry and is formatted as `nix fmt` leaves it
      checks = forAllSystems (
        pkgs:
        ciChecks pkgs
        // {
          firmware = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
          flake-registry = registryCheck pkgs;
          nix-fmt = pkgs.runCommand "rfoundry-nix-fmt" { nativeBuildInputs = [ pkgs.nixfmt ]; } ''
            nixfmt --check ${./flake.nix} ${./nix}/*.nix || {
              echo "flake.nix or a file under nix/ is not formatted (above). Run: nix fmt" >&2
              exit 1
            }
            touch $out
          '';
        }
      );
      formatter = forAllSystems (pkgs: pkgs.nixfmt-tree);
    };
}
