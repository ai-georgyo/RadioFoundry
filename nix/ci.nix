# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# One run of tools/ci.sh in the build sandbox, on a writable copy of src. $out holds the run's logs.
#
# tools/ci.sh exits 0 when a step left itself out for a missing tool. Here a skip fails the run unless skipOk (its
# RFOUNDRY_CI_SKIP_OK) names it, so no check passes because a step did not run.
{
  lib,
  stdenv,
  gnumake,
  git,
  zlib,
  # the store name, the tree the run is in, tools/ci.sh's arguments
  name,
  src,
  args,
  # python3 for every step, and what else the steps of args call
  python,
  tools ? [ ],
  # a firmware step: the make variable that says where each compiler is, and the shell that fills third_party/
  toolchainEnv ? { },
  linkThirdParty ? "",
  # the steps that list files with git get one commit of the tree, as CI's checkout gives them
  withGit ? false,
  # the skips this run may have: an extended regular expression over the whole text of one. "" allows none
  skipOk ? "",
  # shell before the run, for its environment, and after a run that passed, for what tools/ci.sh doesn't check
  preRun ? "",
  postRun ? "",
  # For the check of the rule above: the run must fail, with a line this extended regular expression matches
  failsWith ? null,
  # the build date of what the run compiles (SOURCE_DATE_EPOCH)
  epoch,
}:
stdenv.mkDerivation {
  name = "rfoundry-ci-${name}";
  inherit src;
  nativeBuildInputs = [
    gnumake
    python
  ]
  ++ tools
  ++ lib.optional withGit git;
  # the host tests' simulator writes PNGs
  buildInputs = [ zlib ];
  # The host C code defines __attribute__ away (tests/host/Makefile), which glibc's fortified headers don't survive
  hardeningDisable = [
    "fortify"
    "fortify3"
  ];
  # stdenv's 1980 would be the build date, and a host test refuses a date that old
  env = toolchainEnv // {
    SOURCE_DATE_EPOCH = epoch;
  };
  dontConfigure = true;
  dontFixup = true;

  # stdenv's HOME is a directory that doesn't exist, and the steps' tools write under it. git reads no
  # configuration from outside the sandbox: there is none, and the commit must not depend on one.
  # The commit is made before third_party/ is filled: tools/license_grep.sh reads the tracked files, and the
  # submodules' are not ours to check.
  # pipefail keeps tools/ci.sh's status through tee
  buildPhase = ''
    runHook preBuild
    export HOME=$TMPDIR JOBS=$NIX_BUILD_CORES
    export RFOUNDRY_CI_SKIP_OK=${lib.escapeShellArg skipOk}
    ${lib.optionalString withGit ''
      export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null
      git init -q -b main .
      git add -A -f .
      git -c user.name=nix -c user.email=nix@localhost commit -q -m snapshot
    ''}
    ${linkThirdParty}
    ${preRun}
    mkdir -p $out
    set -o pipefail
    status=0
    sh tools/ci.sh ${args} 2>&1 | tee $out/ci.log || status=$?
    ${
      if failsWith == null then
        ''
          if [ $status != 0 ]; then
            echo "${name}: tools/ci.sh ${args} failed (above), exit status $status." >&2
            echo "The logs it names stay in the build directory with: nix build --keep-failed" >&2
            exit 1
          fi
          ${postRun}
        ''
      else
        ''
          if [ $status != 1 ] || ! grep -Eq ${lib.escapeShellArg failsWith} $out/ci.log; then
            echo "${name}: tools/ci.sh ${args} had to fail with a line that matches" >&2
            echo "  "${lib.escapeShellArg failsWith} >&2
            echo "Its exit status is $status." >&2
            exit 1
          fi
        ''
    }
    cp build/ci/*.log $out/
    [ ! -f build/ci/size-report.md ] || cp build/ci/size-report.md $out/
    runHook postBuild
  '';
  dontInstall = true;
}
