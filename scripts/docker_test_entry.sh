#!/usr/bin/env bash
# Runs INSIDE the nRF Connect SDK toolchain container; see scripts/test_docker.ps1
# (Windows) and scripts/test_docker.sh (Linux/macOS), which are thin wrappers
# around `docker run`. The real logic lives here so there is one copy of it
# rather than one per host platform.
#
# It mirrors .github/workflows/ci.yml's unit-tests and integration jobs as
# closely as it can, deliberately: the point of running locally is to find out
# what CI will say, so any divergence between the two is a bug in this file.
#
# Expects:
#   /work   the repo, bind-mounted from the host
#   /ncs    a persistent docker volume for the NCS west workspace
#
# The workspace is a named volume, not a layer, because `west update` pulls
# multiple GB. First run is slow; every run after it reuses the volume.
set -euo pipefail

NCS_REV="${NCS_REV:-v3.3.1}"
REPO=/work
WS=/ncs

echo "==> nrfProxy local test run (NCS ${NCS_REV})"

# The container user does not own the bind-mounted checkout, so git -- including
# west's own calls -- refuses to touch it until it is marked safe. Same reason
# the CI composite action does this.
git config --global --add safe.directory '*'

# native_sim builds its runner (native_simulator) with a Makefile, and the
# toolchain image ships cmake and ninja but no make. CI installs it per job; here
# the container is thrown away each run, so it is installed each run too.
if ! command -v make >/dev/null 2>&1; then
	echo "==> installing make (not in the toolchain image)"
	apt-get update -qq && apt-get install -y -qq make
fi

if [ ! -d "$WS/.west" ]; then
	echo "==> initialising the NCS workspace in the volume (slow, one time)"
	west init -m https://github.com/nrfconnect/sdk-nrf --mr "$NCS_REV" "$WS"
	# Shallow + narrow, matching CI: only the pinned revision's trees are needed.
	(cd "$WS" && west update --narrow -o=--depth=1)
else
	echo "==> reusing the cached NCS workspace"
fi

cd "$WS"
west zephyr-export

# Fail loudly here rather than inside a build: a workspace missing the nrf module
# is the likeliest breakage, and its later symptom ("BT_NUS undefined") points
# somewhere else entirely.
if [ ! -d "$WS/nrf" ]; then
	echo "ERROR: the 'nrf' module is missing from the workspace — NUS would not build" >&2
	exit 1
fi

status=0

run_suite() {
	suite_path="$1"   # e.g. tests/unit
	out_dir="$2"      # e.g. twister-out-unit
	min_tests="$3"    # floor, matching ci.yml

	echo
	echo "==> ${suite_path}"
	# `|| status=1` rather than -e: run both suites and report both, so one
	# failure does not hide the other's result.
	if ! west twister -T "${REPO}/${suite_path}" -p native_sim --inline-logs \
		-O "${REPO}/${out_dir}"; then
		status=1
	fi
	# The same guard CI applies: twister exits 0 when it runs nothing.
	if ! python3 "${REPO}/scripts/assert_tests_ran.py" \
		"${REPO}/${out_dir}/twister.xml" --min "${min_tests}"; then
		status=1
	fi
}

run_suite tests/unit twister-out-unit 20
run_suite tests/integration twister-out-integration 6

# The container runs as root (it must, to apt-get make above), so anything it
# wrote into the bind mount would be root-owned on a Linux host and awkward to
# delete. Hand the reports back to whoever owns the checkout. On Docker Desktop
# for Windows/macOS ownership is already mapped and this is a harmless no-op.
if [ "$(id -u)" = "0" ]; then
	owner="$(stat -c '%u:%g' "$REPO" 2>/dev/null || true)"
	if [ -n "$owner" ] && [ "$owner" != "0:0" ]; then
		chown -R "$owner" "${REPO}/twister-out-unit" \
			"${REPO}/twister-out-integration" 2>/dev/null || true
	fi
fi

echo
if [ "$status" -eq 0 ]; then
	echo "==> all native_sim suites passed"
else
	echo "==> FAILURES above" >&2
fi
exit "$status"
