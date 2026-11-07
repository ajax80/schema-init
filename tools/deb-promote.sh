#!/bin/sh
# deb-promote.sh <tag> <sha>: publish a release's debs to the stable apt channel.
# Promotes the tag run's artifact rather than building again, so stable gets the
# bytes that were tested, and publishing a release twice can't change a version.
set -eu
cd "$(dirname "$0")/.."
tag=$1 sha=$2
V=$(sed -n 's/^Version:[[:space:]]*//p' schema-init.spec | head -1)
if [ "$tag" != "v$V" ]; then
    echo "$tag is not v$V, the version in schema-init.spec: not publishing" >&2
    exit 1
fi
run=$(gh run list --workflow deb.yml --event push --branch "$tag" --commit "$sha" --status success \
    --limit 1 --json databaseId --jq '.[0].databaseId // empty')
if [ -z "$run" ]; then
    echo "no green deb.yml tag run for $tag at $sha: wait for it or fix it, then re-run this job" >&2
    exit 1
fi
echo "promoting run $run to stable"
GH_TOKEN=$APT_DISPATCH_TOKEN gh api repos/ajax80/schema-init-apt/dispatches -f event_type=pi-debs \
    -f "client_payload[run_id]=$run" -f "client_payload[channel]=stable"
