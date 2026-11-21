#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# Cases for RFOUNDRY_CI_SKIP_OK of tools/ci.sh: the skips it lets pass and the ones it turns into a failure.
# tools/ci.sh runs it, in its python step.
#
# Each case is a real run of the one step that skips on every machine and does nothing else: the similarity scan
# without its reference trees.
set -u
cd "$(dirname "$0")/.." || exit 1
unset RFOUNDRY_SIMILARITY_REFS RFOUNDRY_CI_SKIP_OK
SKIP='ci: skip  similarity scan: '
FAIL='ci: FAIL  skipped: similarity scan: '
fails=0 n=0
# check WHAT STATUS LINE: tools/ci.sh ends with STATUS and prints a line that starts with LINE. RFOUNDRY_CI_SKIP_OK
# is the caller's: exported for the case, or not set
check() {
	n=$((n + 1))
	out=$(sh tools/ci.sh --steps similarity 2>&1)
	got=$?
	if [ "$got" != "$2" ] || ! printf '%s\n' "$out" | grep -qF -- "$3"; then
		echo "ci-skip: $1: expected exit status $2 and a line with '$3', got $got and:"
		printf '%s\n' "$out"
		fails=$((fails + 1))
	fi
}
check "not set" 0 "$SKIP"
export RFOUNDRY_CI_SKIP_OK
RFOUNDRY_CI_SKIP_OK=""
check "set to nothing" 1 "$FAIL"
RFOUNDRY_CI_SKIP_OK='similarity scan: .*'
check "the whole text matches" 0 "$SKIP"
# part of the text is not enough: a pattern for one skip must not let a longer one pass
RFOUNDRY_CI_SKIP_OK='similarity scan'
check "part of the text matches" 1 "$FAIL"
RFOUNDRY_CI_SKIP_OK='reuse lint: .*|similarity scan: .*'
check "one of two patterns matches" 0 "$SKIP"
RFOUNDRY_CI_SKIP_OK='reuse lint: .*'
check "another skip's pattern" 1 "$FAIL"
# a pattern grep refuses allows nothing
RFOUNDRY_CI_SKIP_OK='similarity scan: .*('
check "a broken pattern" 1 "$FAIL"
if [ "$fails" != 0 ]; then
	echo "ci-skip: $fails of $n cases fail"
	exit 1
fi
echo "ci-skip: $n cases pass"
