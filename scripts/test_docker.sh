#!/usr/bin/env bash
# Run the native_sim test suites locally, in the same container CI uses.
#
# The Linux/macOS twin of scripts/test_docker.ps1. Both are thin wrappers around
# `docker run`; the logic they invoke lives in scripts/docker_test_entry.sh so
# there is one copy of it.
#
# The NCS workspace lives in a named docker volume because `west update` pulls
# several GB: the first run is slow, later runs reuse it.
#
#   ./scripts/test_docker.sh            # run the suites
#   ./scripts/test_docker.sh --fresh    # discard the cached workspace first
#   NCS_REV=v3.3.1 ./scripts/test_docker.sh
set -euo pipefail

rev="${NCS_REV:-v3.3.1}"
fresh=0

while [ $# -gt 0 ]; do
	case "$1" in
	--fresh) fresh=1 ;;
	--rev) shift; rev="${1:?--rev needs a value}" ;;
	-h | --help)
		sed -n '2,14p' "$0"
		exit 0
		;;
	*)
		echo "unknown argument: $1" >&2
		exit 2
		;;
	esac
	shift
done

image="ghcr.io/nrfconnect/sdk-nrf-toolchain:${rev}"
volume="nrfproxy-ncs-${rev}"
repo="$(cd "$(dirname "$0")/.." && pwd)"

if ! command -v docker >/dev/null 2>&1; then
	cat >&2 <<-EOF
		docker not found on PATH.

		Install Docker, or run the suites another way:
		  - the host-only checks need no container:  pwsh tests/host/run.ps1
		  - or let CI run them by pushing a branch
		See TESTING.md for what each tier covers.
	EOF
	exit 1
fi

if [ "$fresh" -eq 1 ]; then
	echo "==> removing the cached workspace volume ${volume}"
	docker volume rm "$volume" >/dev/null 2>&1 || true
fi

echo "==> repo:   ${repo}"
echo "==> image:  ${image}"
echo "==> volume: ${volume}"

# Deliberately NOT --user: the entry script has to apt-get install make (the
# toolchain image ships cmake and ninja but not make, which native_sim's runner
# needs), and that requires root. The entry script hands ownership of the output
# directories back afterwards, which is what --user would have been for.
#
# --entrypoint bash: the image publishes its toolchain env through BASH_ENV,
# which only bash reads. Non-interactive `bash -c` does read it.
exec docker run --rm \
	-v "${repo}:/work" \
	-v "${volume}:/ncs" \
	-e "NCS_REV=${rev}" \
	--entrypoint bash \
	"$image" \
	/work/scripts/docker_test_entry.sh
