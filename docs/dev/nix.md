<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Building with Nix

`flake.nix` gives a machine with [Nix](https://nixos.org) three things: every board's firmware image as a package, a
development shell with the pinned compilers, and the pull request gate as `nix flake check`. It is optional: `make`
and `tools/ci.sh` need no Nix ([Building](building.md), [CI](ci.md)). Every build is the Makefile's own and the
`ci-*` checks are `tools/ci.sh` runs; the flake only says where the compilers and `third_party/` are. No CI workflow
runs the flake.

| | |
|---|---|
| Systems | `x86_64-linux`: everything on this page was built and run there. `aarch64-linux` evaluates and is untested. No macOS |
| Nix | With the `nix-command` and `flakes` features on. Run with 2.36; older releases are untested |
| Network | For nixpkgs, the submodules' sources and, once, the sources of a compiler built from source. The builds and the checks run in the sandbox, without it |

## Commands

`<board>` is a directory under `boards/` (`tools/boards.py list`), `<toolchain id>` a `[toolchain] id` of
`platform/*/platform.toml` ([The board registry](board-registry.md#platformtoml)), `<variant>` a file in
`config/variants/` ([build-config.md](../build-config.md#variants)).

| Command | Does |
|---|---|
| `nix flake show` | Lists the packages, shells and checks |
| `nix build .#<board>` | One board's image: `result/rfoundry_<board>.bin`, or `result/rfoundry_<board>_install.bin` for a board with a first stage |
| `nix build .#<board>-<value>` | The image of a build option value other than the default ([Building](building.md#hardware-build-options)) |
| `nix build .#<board>.variants.<variant>` | The image as a release variant |
| `nix build '.#<board>^elf'` | Its `.elf`, in `result-elf/` |
| `nix build` | Every image in `result/`, with `SHA256SUMS` |
| `nix build .#firmware-<toolchain id>` | The images one compiler builds |
| `nix build .#toolchain-<toolchain id>` | The compiler alone |
| `nix develop` | A shell with every compiler |
| `nix develop .#<toolchain id>` | A shell with one compiler |
| `nix develop -c make BOARD=<board> CONFIG_X=n` | Any other configuration: the Makefile, in the shell |
| `nix flake check -L` | Every check ([below](#checks)) |
| `nix build .#checks.x86_64-linux.<check> -L` | One check; for a `ci-*` check `result/` holds its logs |
| `nix flake check --no-build` | Evaluates every output and builds nothing: the [assertions](#toolchains) |
| `nix eval --raw .#<board>.version` | The version the image is stamped with |
| `nix fmt` | Formats `flake.nix` and `nix/*.nix` |

`result` and `result-*` are links in the working directory; git ignores them.

| Reference | Note |
|---|---|
| `.` or `git+file://<dir>` | A git checkout: the files git tracks ([New files](#new-files)) |
| A shallow clone | `.#<board>` from inside it works. From elsewhere Nix refuses `git+file://<dir>`; write `git+file://<dir>?shallow=1` |
| `path:<dir>`, a tarball | No git tree: the version is `unknown` ([Differences](#differences-from-a-make-build)). `path:` copies the whole directory to the store, ignored files too. Never use it on a checkout that holds a stock file in `codecs/` or build trees |

## Outputs

| Attribute | Is |
|---|---|
| `packages.<system>.<board>` | The board's default image |
| `packages.<system>.<board>-<value>` | One more image for each other value of a build option (`board.toml` `[[build_options]]`; the first value is the default) |
| `packages.<system>.<image>.variants.<variant>` | The image built with `VARIANT=<variant>`, one for each `config/variants/<variant>.mk`. `nix flake show` doesn't list these |
| `packages.<system>.default` | Every image in one directory, under the Makefile's file names, with a `SHA256SUMS` over them |
| `packages.<system>.firmware-<toolchain id>` | The same, for the images one compiler builds |
| `packages.<system>.toolchain-<toolchain id>` | The compiler, to build it ahead or to push it to a binary cache |
| `devShells.<system>.default` | The shell with every compiler ([Development shells](#development-shells)) |
| `devShells.<system>.<toolchain id>` | The shell with one |
| `checks.<system>.*` | [Checks](#checks) |
| `formatter.<system>` | `nixfmt-tree`, for `nix fmt` |

There are no `apps` and no overlay. Nothing in the flake flashes a radio or talks to one.

The packages are bare images, not release files: no manifest and no release file names
([Releases](release.md#release-artifacts)). Other feature switches are not packages; build them in the shell.

### An image package

| Output | Holds |
|---|---|
| `out` | The one file to flash, under the name the Makefile gives it: `rfoundry_<image>_install.bin` (the first stage, then the application) for a board with a first stage, else `rfoundry_<image>.bin`. It refers to no other store path, so its closure is the image alone |
| `elf` | `rfoundry_<image>.elf`, for a debugger and `tools/crash_decode.py`. It names the compiler's store path, so its closure holds the compiler |

The store name is `rfoundry-<image>[-<variant>]-<version>`. A variant's file has the default image's name.

The build fails when:

| Condition | Why |
|---|---|
| The compiler or the linker prints a warning | `make` exits 0 on one; zero warnings is the rule |
| `build/` holds an image with the codec | The codec must never be in the store, which a cache makes public |
| There isn't exactly one image to flash | The output must be the file a user flashes |
| The image contains a store path | A consumer's closure would carry the compiler |

## Development shells

`nix develop` starts a shell in the checkout. `make BOARD=<board>`, `make -C tests/host` and `tools/ci.sh` run there
as they are.

| In the shell | For |
|---|---|
| The compilers, on `PATH` and through their make variable ([Toolchains](#toolchains)) | `make BOARD=<board>`; `addr2line` for `tools/crash_decode.py`; the debugger of a compiler package that has one |
| The host's `cc` and zlib, with the fortify hardening off | The host tests: they define `__attribute__` away, which glibc's fortified headers don't survive |
| Python 3 with pytest, numpy, pyserial, pyusb, markdown-it-py and resvg-py; `RFOUNDRY_PYTHON` names it | The tools, the Python checks, the site build |
| make, git | |
| Node.js | The web flasher's tests |
| `tshark` | The pcap checks of the host tests |
| gcovr | `tools/coverage.sh` |

| Fact | Detail |
|---|---|
| `third_party/` | The checkout's own: `git submodule update --init` or `tools/checkout-submodules.sh`. The shell doesn't fill it |
| Compiler choice | The make variables are set, so a compiler under `$HOME` is not used |
| `SOURCE_DATE_EPOCH` | Unset: Nix's default (1980) would be the firmware's build date, and a host test refuses a date that old |
| One compiler | `nix develop .#<toolchain id>` has only that compiler and its variable. Use it to avoid building the other from source |
| Not in the shell | `reuse` (ci.sh skips `reuse lint`), `uv`, the SNMP tools the host tests' peer check uses (skipped), pandoc and typst (the manual) |

## Checks

`nix flake check` builds these. Each `ci-*` check is one `tools/ci.sh` run in the build sandbox, on a writable copy of
its source, with `JOBS` set to the build's cores. Times are for 16 cores with the compilers already in the store; the
whole run takes about 13 minutes, the time of its longest check.

| Check | Runs | Is | May skip | Time |
|---|---|---|---|---|
| `firmware` | `packages.<system>.default` | Every image, built with zero warnings | | 2 min |
| `ci-registry` | `tools/ci.sh --steps registry,licenses` | Those steps: part of CI's `checks` job | The radio floor, the flake pins, `reuse lint` | 1.5 to 2 min |
| `ci-python` | `tools/ci.sh --steps python`, then requires the line that says Python files were parsed | That step: the rest of CI's `checks` job | No step | 13 min |
| `ci-host` | `RFOUNDRY_CI_SANITIZE=1 tools/ci.sh --steps host,bench` with CI's `ASAN_OPTIONS`, then requires the pcap checks' lines, then renders the screen gallery (`uiSim --gallery`) | Those steps: CI's `unit tests` job without the coverage counters | No step | 6 min |
| `ci-firmware-<toolchain id>` | `tools/ci.sh --quick --steps firmware --toolchain <toolchain id>` | CI's firmware job of that toolchain: default and maximal builds, option values, the stack check, the flash stubs | A directory under `boards/` without a `board.mk` | 4.5 to 5.5 min each |
| `ci-skip-guard` | The first toolchain's firmware run with its compiler missing; passes only when ci.sh exits 1 with a `ci: FAIL  skipped:` line | Proof that a skipped step fails a check | | seconds |
| `flake-registry` | The images the flake reads from `boards/` against the registry's (`tools/boards.py list --toolchain`, `builds`) | Proof that the flake builds what ci.sh builds | | seconds |
| `nix-fmt` | `nixfmt --check` on `flake.nix` and `nix/*.nix` | | | seconds |

`tools/ci.sh` exits 0 when a step leaves itself out for a missing tool. Each check sets `RFOUNDRY_CI_SKIP_OK`
([CI](ci.md#running-the-pieces-locally)) to the skips in its row, so any other skip fails it. That holds for the
skips ci.sh reports (`ci: skip`). A test that skips inside a step is the test's own, as on CI
([Not run](#not-run)).

| Fact | Detail |
|---|---|
| Logs | A `ci-*` check's output holds `ci.log`, ci.sh's `build/ci/*.log` and, where the run wrote them, `size-report.md` and `gallery.log`. A check that failed has no output: `nix build --keep-failed` leaves its build directory, with the logs ci.sh names. The logs name store paths, so the output of `ci-host` and of a firmware check refers to the Python environment or the compiler: a copy to a cache takes that along |
| Git | The sandbox has no repository. `ci-registry` and `ci-python` make one commit of the tree first, because their steps list files with git |
| Source | `ci-registry` and `ci-python` read the whole tree. `ci-host` reads it without `docs/`, the Markdown pages at the root, the Nix files and `site/` except the radio drawings. The firmware checks also leave out `tests/` and `.github/` |
| Runs again | `ci-registry` and `ci-python` read the whole tree: any changed tracked file builds them again. `ci-host`, the firmware checks, `ci-skip-guard` and `flake-registry` are built again only when their source above, the Nix files they are made from or `flake.lock` changed. `firmware` is built again on every commit: the commit is stamped into each image |
| Build date | The locked nixpkgs' date, not the commit's |

### Not run

| Not run by `nix flake check` | Why, and where it runs |
|---|---|
| The feature matrix (each switch off alone, `fm-minimal`, the `[ci] extra` configurations) | Hundreds of builds. CI's nightly; by hand `nix develop -c tools/ci.sh --matrix --steps firmware` |
| The radio floor (`registry` step) | The sandbox copy has no base branch to compare with |
| `tools/i18n_changes.py` (`python` step, advisory) | The same |
| The staleness test of the bring-up run (`tests/hil/test_bringup.py`, `python` step) | It needs a commit older than `HEAD` whose files differ, and the sandbox repository has one commit: pytest skips it (`9 passed,1 skipped`). It runs in a checkout with history: `nix develop -c tools/ci.sh --steps python` |
| `reuse lint` | The tree doesn't pass it yet ([CI](ci.md)), so the checks don't install it |
| The submodule link checks (`licenses` step) | The sandbox copy has no links: `third_party: submodule links` has nothing to read and the flake pins are skipped. Run `python3 tools/check_flake_pins.py` or `tools/ci.sh --steps licenses` in the checkout |
| Coverage (`tools/coverage.sh`) | CI's `unit tests` job runs the tests through it; `ci-host` runs them plain and under the sanitizers |
| The SNMP tools' part of the host tests' peer check | The tools are not in the checks; the Makefile skips that part, as on CI |
| The `similarity` step | Its reference trees are never in the repository |
| Release packaging (`tools/release_build.sh`), the manual PDF | Not part of the flake |
| HIL | Needs a radio |

## Toolchains

The flake holds no compiler release. It reads each board's platform from `PLATFORM` in its `board.mk` and the
platform's pin from `platform/<p>/platform.toml` `[toolchain]`
([The board registry](board-registry.md#platformtoml)), then takes the compiler by the pin's `id`:

| `[toolchain] id` | Package | Source | Make variable |
|---|---|---|---|
| `arm` | nixpkgs' `gcc-arm-embedded-<major>`, `<major>` from the pinned `version` | Arm's release archive as nixpkgs packages it; fetched from nixpkgs' binary cache (235 MB, 1.0 GiB unpacked) | `ARM_GCC_DIR` |
| `csky` | `nix/csky-toolchain.nix` | binutils, GCC and newlib built from source with the flags of the script `[toolchain] build` names. GCC's release is the pinned `version`; the other releases and the three checksums are read from that script. In no public cache: the first use builds it, 4 to 8 minutes on 16 cores; the script it mirrors takes about 45 on 4 | `CSKY_GCC_DIR` |

Every image build, firmware check and shell sets the make variable to `<package>/bin/`, so `platform.mk` takes that
compiler and not one under `$HOME`. `nix build .#toolchain-<toolchain id>` builds or fetches a compiler ahead of use.

Evaluation stops, with a message that names the file and what to change, when:

| Condition | Read from |
|---|---|
| nixpkgs has no `gcc-arm-embedded-<major>`, or its version is not the pinned one | `[toolchain] version` |
| On `x86_64-linux`, nixpkgs builds that package from another archive than the pinned one | `[toolchain] sha256`, when the pin has one. It is the Linux x86_64 archive's, so no other system compares it |
| The build script doesn't hold exactly one line for the pinned GCC release's checksum, the binutils release, the newlib release, or either of their checksums | The script `[toolchain] build` names |
| A `[toolchain] id` has no entry in `flake.nix` | The table above (`toolchain` in `flake.nix`) |
| A `platform.toml` has no `[toolchain]` table | `platform/<p>/platform.toml` |
| A `board.mk` doesn't hold exactly one `PLATFORM :=` line, or names a platform without a `platform.toml` | `boards/<board>/board.mk` |
| Two images would have one package name, or an image would be named `default`, `firmware-<toolchain id>` or `toolchain-<toolchain id>` | The board directories and option values |
| A submodule in `.gitmodules` has no flake input of its name | `inputs` in `flake.nix` |

An assertion runs when something that needs it is evaluated. `nix flake check --no-build` evaluates everything.

## Differences from a make build

| | `make BOARD=<board>` | The package |
|---|---|---|
| Compiler | The first found: under `$HOME`, then the system's | The pinned one |
| `third_party/` | The checkout's submodules | The flake inputs, at the commits `flake.lock` holds, linked into the build tree |
| Source | The working tree | The files git tracks, without `docs/`, `tests/`, `.github/`, the Nix files and `site/` except the radio drawings. An untracked file is not there ([New files](#new-files)) |
| Version | The commit, with `-dirty` when tracked files differ or untracked sources sit in the built directories | The same form, from the flake's revision: `-dirty` when any tracked file differs. `unknown` for a flake that is no git tree (`path:`, a tarball) |
| Commit time (the debug link's `git_time=`) | The commit's; the clock for a dirty tree | The commit's, for a dirty tree too. None for a flake that is no git tree |
| Build date | The clock | The commit's time; the locked nixpkgs' date for a flake that is no git tree |
| Release name | From a release tag on the commit | None: the build passes `RELEASE=` |
| Build source | `local` or `fork` and the repository's name, in a clone (`tools/build_source.py`) | `unknown` and no repository: the sandbox has no git |
| Codec | Merged into `<image>_codec.bin` when `codecs/` holds the stock file | Never: the source has no stock file, and the build fails if a codec image appears |
| Files kept | `.elf`, `.bin`, `.map` and the rest of `build/<image>/` | `out` and `elf` ([above](#an-image-package)). No `.map`: with LTO it lists the compiler's temporary files, whose names are random |
| Warnings | Printed | Fail the build |

With those inputs made equal, `make` in the shell writes the same bytes as the package. Checked on a board of each
toolchain and on one with a first stage:

```
v=$(nix eval --raw .#<board>.version)
t=$(git log -1 --format=%ct)
nix develop -c env GIT_DIR=/nonexistent SOURCE_DATE_EPOCH=$t \
    make BOARD=<board> BUILD=build/t-nix RFOUNDRY_VERSION=$v RFOUNDRY_GIT_TIME=$t RELEASE=
cmp build/t-nix/rfoundry_<board>.bin "$(nix build --no-link --print-out-paths .#<board>)/rfoundry_<board>.bin"
```

`GIT_DIR=/nonexistent` hides the repository from `tools/build_source.py`, as in the sandbox. For a board with a first
stage the file to compare is `rfoundry_<board>_install.bin`.
`nix build --rebuild '.#<board>^*'` builds an image a second time and fails when either output differs; it passed
for a board of each toolchain.

## Maintaining the flake

Nix files follow the tree's rules: the SPDX header, no radio named (`tools/check_generic.py` reads `.nix` files),
and `nix fmt` before a commit (the `nix-fmt` check). The flake also names no platform and no compiler release.
Nothing checks that: keep it when editing.

| Change | In the flake |
|---|---|
| A new board, build option value or variant | Nothing: read from `boards/` and `config/variants/`. `git add` the files first |
| A new platform on a toolchain already listed | Nothing |
| A new toolchain id | An entry in `toolchain` in `flake.nix`: its package, and the make variable its `platform.mk` reads the compiler's directory from |
| A new submodule | An input ([below](#bumping-a-submodule)) |
| A moved compiler pin (`[toolchain] version`, `sha256`) | `arm`: a locked nixpkgs whose `gcc-arm-embedded-<major>` is that release ([Updating nixpkgs](#updating-nixpkgs)), or the pin set to the release the locked nixpkgs has. `csky`: the build script's checksum line for that GCC release |
| Changed CPU or link flags of a platform whose compiler is built from source | Nothing for the images. `nix/csky-toolchain.nix` holds a copy of them for its install check only: change it when the check should follow, which builds the compiler again |
| A reworded line the flake reads by pattern (`PLATFORM :=`, the build script's release and checksum lines) | The pattern in `flake.nix` |
| A reworded skip in `tools/ci.sh` | The check's allowed skips in `flake.nix` |

### Updating nixpkgs

```
nix flake update nixpkgs        # the newest commit of the branch flake.nix names
nix flake check --no-build      # the compiler assertions
nix flake check -L              # everything
```

| Moves with nixpkgs | Doesn't |
|---|---|
| The host compiler, Python, Node.js, `tshark`: the host tests and the checks can show new warnings or failures | The Arm compiler: asserted against the pin, so a nixpkgs with another release stops the evaluation |
| The compiler built from source is built again when its build inputs changed | Its releases and flags: they are the pin's and the build script's |

To move to another release branch, change `nixpkgs.url` in `flake.nix` and run `nix flake lock`.

### Bumping a submodule

`flake.nix` and `flake.lock` record each submodule's commit a second time: a flake's source holds no submodule
content, so each build takes `third_party/<name>` from the flake input of that name. A flake can't read a link, so
nothing in Nix compares the records. `tools/check_flake_pins.py` does, in ci.sh's `licenses` step
([CI](ci.md#submodules)): the link, the line in `flake.nix` and the lock must name one commit of one repository.

| Step | Command |
|---|---|
| 1. Move the link | `git -C third_party/<name> checkout <commit>`, then `git add third_party/<name>` |
| 2. Put the same commit in `flake.nix` | `<name>.url = "github:<owner>/<repo>/<commit>";` in `inputs` |
| 3. Write the lock | `nix flake lock`: only that input changes in `flake.lock` |
| 4. Commit the link, `flake.nix` and `flake.lock` together | |
| 5. Compare | `python3 tools/check_flake_pins.py` prints `<n> submodules`. It reads the links of `HEAD`, so run it after the commit |

Step 3 needs Nix: the lock holds a hash of the fetched source that nothing else writes. With the commit changed in
`flake.nix` alone, Nix writes the lock by itself at the next build and builds that commit; the check fails on that
state too.

A new submodule also gets an input named as its directory under `third_party/`, with `flake = false`, as the others
have: the check reads the commit from the one line `<name>.url = "<reference>";`. It also fails when the input's
repository is not the one `.gitmodules` names for that path.

### New files

A flake in a git checkout sees only the files git tracks. `git add` a new file before `nix build` or
`nix flake check` (no commit is needed), or the build runs without it. The Makefile finds its sources with wildcards,
so a missing source file is not always an error. This holds for a new file under `nix/` too.
