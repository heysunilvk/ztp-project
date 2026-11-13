#!/bin/sh
# CI evidence (QA §11 M7 row, item 7): one GitHub Actions run as a Markdown report — the run, every job with
# its result, time and link, every step's result, and the notices and errors the jobs posted (the suite
# summaries, matrix rows, scale and soak results). GitHub deletes job logs after 90 days; this record stays.
# usage: scripts/ci_evidence.sh RUN_ID [OUT.md]      needs curl and jq
# Env: REPO (default heysunilvk/ztp-project), GITHUB_TOKEN (optional: unauthenticated reads allow 60/hour),
#      SKIP_JOB (a job left out: the evidence job, still running when it reports)
set -u
RUN=${1:?usage: scripts/ci_evidence.sh RUN_ID [OUT.md]}
OUT=${2:-/dev/stdout}
REPO=${REPO:-heysunilvk/ztp-project}
API=https://api.github.com/repos/$REPO
for t in curl jq; do command -v $t > /dev/null || { echo "ci_evidence: $t is required" >&2; exit 1; }; done
get() {
    if [ -n "${GITHUB_TOKEN:-}" ]; then curl -fsS -H "Authorization: Bearer $GITHUB_TOKEN" "$1"
    else curl -fsS "$1"; fi
}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
get "$API/actions/runs/$RUN" > "$T/run.json" || { echo "ci_evidence: cannot read run $RUN" >&2; exit 1; }
get "$API/actions/runs/$RUN/jobs?per_page=100" > "$T/all.json" || exit 1
jq '.jobs |= map(select(.name != ($ENV.SKIP_JOB // "")))' "$T/all.json" > "$T/jobs.json" || exit 1
{
    # The evidence job runs inside the run it reports, so the run itself is still in progress: the headline
    # comes from the other jobs' results.
    verdict=$(jq -r '[.jobs[] | .conclusion // .status] | if all(. == "success") then "all \(length) jobs passed"
        else "\(map(select(. != "success")) | length) of \(length) jobs did not pass" end' "$T/jobs.json")
    jq -r --arg v "$verdict" '"# CI run \(.run_number): \($v)\n",
        "| | |", "|---|---|",
        "| Run | [\(.id)](\(.html_url)) (attempt \(.run_attempt)) |",
        "| Commit | `\(.head_sha[0:7])` \(.head_commit.message | split("\n")[0]) |",
        "| Branch, event | \(.head_branch), \(.event) |",
        "| Started, updated | \(.run_started_at), \(.updated_at) |", ""' "$T/run.json"
    echo "## Jobs"
    echo
    echo "| Job | Result | Time | Log |"
    echo "|---|---|---|---|"
    jq -r '.jobs[] | (if .completed_at and .started_at then (((.completed_at | fromdateiso8601)
        - (.started_at | fromdateiso8601)) / 60 | floor | tostring) + " min" else "" end) as $t
        | "| \(.name) | \(.conclusion // .status) | \($t) | [job](\(.html_url)) |"' "$T/jobs.json"
    echo
    jq -r '.jobs[] | "\(.id)\t\(.name)"' "$T/jobs.json" | while IFS="$(printf '\t')" read -r id name; do
        echo "## $name"
        echo
        jq -r --argjson id "$id" '.jobs[] | select(.id == $id) | .steps[]
            | select(.name | test("^(Set up job|Complete job|Post )") | not)
            | "- \(.conclusion // .status): \(.name)"' "$T/jobs.json"
        if get "$API/check-runs/$id/annotations?per_page=100" > "$T/a.json" 2> /dev/null && [ "$(jq length "$T/a.json")" -gt 0 ]; then
            echo
            echo '```'
            jq -r '.[] | "[\(.annotation_level)] \(.message)"' "$T/a.json"
            echo '```'
        fi
        echo
    done
} > "$OUT"
