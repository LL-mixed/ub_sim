#!/bin/bash
# git-batch-push.sh - push a large branch in commit-sized batches.
#
# The remote branch is advanced through a temporary remote ref first.  Each
# intermediate push uses "<commit>:refs/heads/<temp>", not a commit range.

if [ -z "${BASH_VERSION:-}" ]; then
    exec bash "$0" "$@"
fi

set -euo pipefail

DEFAULT_BRANCH="main"
DEFAULT_BATCH_SIZE="500"
DEFAULT_TEMP_BRANCH="batch-push-temp"

usage() {
    cat <<EOF
Usage:
  $0 <remote> [branch] [batch_size]
  $0 --self-test

Environment:
  TEMP_BRANCH=<name>  Temporary remote branch name. Default: $DEFAULT_TEMP_BRANCH
  DRY_RUN=1           Print git push commands without executing them.
  NO_RESUME=1         Ignore the remote temporary branch and start from batch 1.
EOF
}

die() {
    echo "error: $*" >&2
    exit 1
}

normalize_remote_ref() {
    local branch="$1"

    if [[ "$branch" == refs/heads/* ]]; then
        printf '%s\n' "$branch"
    else
        printf 'refs/heads/%s\n' "$branch"
    fi
}

remote_ref_commit() {
    local remote="$1"
    local ref="$2"

    git ls-remote "$remote" "$ref" | awk 'NR == 1 { print $1 }'
}

run_push() {
    local remote="$1"
    local refspec="$2"

    echo "git push $remote $refspec"
    if [[ "${DRY_RUN:-0}" != "1" ]]; then
        git push "$remote" "$refspec"
    fi
}

batch_push_main() {
    local remote="${1:-}"
    local branch="${2:-$DEFAULT_BRANCH}"
    local batch_size="${3:-$DEFAULT_BATCH_SIZE}"
    local temp_branch="${TEMP_BRANCH:-$DEFAULT_TEMP_BRANCH}"
    local remote_branch_ref
    local temp_branch_ref
    local branch_commit
    local endpoints_file
    local total_commits
    local earliest_commit
    local remote_branch_commit
    local remote_temp_commit
    local resume_batch=1
    local resume_endpoint_index=0
    local batches
    local i

    [[ -n "$remote" ]] || die "remote is required; see --help"
    [[ "$batch_size" =~ ^[1-9][0-9]*$ ]] || die "batch_size must be a positive integer: $batch_size"

    branch_commit="$(git rev-parse --verify "${branch}^{commit}")" ||
        die "branch or commit not found: $branch"
    remote_branch_ref="$(normalize_remote_ref "$branch")"
    temp_branch_ref="$(normalize_remote_ref "$temp_branch")"
    [[ "$temp_branch_ref" != "$remote_branch_ref" ]] ||
        die "TEMP_BRANCH must not be the same as target branch: $remote_branch_ref"
    total_commits="$(git rev-list --count "$branch_commit")"

    echo "=== Git Batch Push ==="
    echo "Remote:   $remote"
    echo "Branch:   $branch"
    echo "Batch:    $batch_size commits"
    echo "Temp ref: $temp_branch_ref"
    if [[ "${DRY_RUN:-0}" == "1" ]]; then
        echo "Mode:     dry-run"
    fi
    echo ""
    echo "Total commits: $total_commits"

    if [[ "$total_commits" -eq 0 ]]; then
        die "no commits reachable from $branch"
    fi

    remote_branch_commit="$(remote_ref_commit "$remote" "$remote_branch_ref")"
    if [[ "$remote_branch_commit" == "$branch_commit" ]]; then
        echo "Remote branch is already at ${branch_commit:0:12}; nothing to push."
        return 0
    fi

    if [[ "$total_commits" -le "$batch_size" ]]; then
        echo "Commit count <= batch size, pushing directly..."
        run_push "$remote" "${branch}:${remote_branch_ref}"
        return 0
    fi

    batches=$(( (total_commits + batch_size - 1) / batch_size ))
    earliest_commit="$(git rev-list --max-parents=0 "$branch_commit" | tail -1)"
    endpoints_file="$(mktemp "${TMPDIR:-/tmp}/git-batch-push-endpoints.XXXXXX")"
    git rev-list --reverse "$branch_commit" |
        awk -v batch_size="$batch_size" -v total="$total_commits" \
            'NR % batch_size == 0 && NR < total { print NR, $0 }' > "$endpoints_file"

    if [[ "${NO_RESUME:-0}" != "1" ]]; then
        remote_temp_commit="$(remote_ref_commit "$remote" "$temp_branch_ref")"
        if [[ -n "$remote_temp_commit" ]]; then
            if [[ "$remote_temp_commit" == "$branch_commit" ]]; then
                echo "Temporary ref is already at the branch tip; pushing final branch ref..."
                run_push "$remote" "${branch}:${remote_branch_ref}"
                rm -f "$endpoints_file"
                return 0
            fi

            resume_endpoint_index="$(
                awk -v commit="$remote_temp_commit" '$2 == commit { print NR; exit }' "$endpoints_file"
            )"
            if [[ -n "$resume_endpoint_index" ]]; then
                resume_batch=$((resume_endpoint_index + 1))
                echo "Resume: $temp_branch_ref is at batch $resume_endpoint_index (${remote_temp_commit:0:12})"
                echo "Resume: starting from batch $resume_batch"
                echo ""
            elif git merge-base --is-ancestor "$remote_temp_commit" "$branch_commit" 2>/dev/null; then
                die "$temp_branch_ref points to ${remote_temp_commit:0:12}, which is on $branch but not a batch endpoint for batch_size=$batch_size. Re-run with the same batch size, set TEMP_BRANCH to another name, or set NO_RESUME=1 to restart."
            else
                die "$temp_branch_ref points to ${remote_temp_commit:0:12}, which is not an ancestor of $branch. Delete or rename the temporary remote branch, set TEMP_BRANCH to another name, or set NO_RESUME=1 to restart."
            fi
        fi
    fi

    echo "Will push in $batches batches"
    echo ""
    echo "Earliest commit: ${earliest_commit:0:12}"
    echo ""

    for ((i = resume_batch; i <= batches; i++)); do
        local start_index=$(( (i - 1) * batch_size + 1 ))
        local end_index=$(( i * batch_size ))
        local end_commit

        if (( end_index > total_commits )); then
            end_index="$total_commits"
        fi

        echo "--- Batch $i/$batches: commits $start_index ~ $end_index ---"

        if (( i == batches )); then
            echo "Pushing final branch tip to $remote_branch_ref..."
            run_push "$remote" "${branch}:${remote_branch_ref}"
        else
            end_commit="$(sed -n "${i}p" "$endpoints_file" | awk '{ print $2 }')"
            echo "Advancing temporary ref to ${end_commit:0:12}..."
            run_push "$remote" "+${end_commit}:${temp_branch_ref}"
        fi

        echo "Batch $i done"
        echo ""
    done

    echo "=== All batches pushed ==="
    echo "Remote branch: $remote_branch_ref"
    echo "Temp branch:   $temp_branch_ref"
    rm -f "$endpoints_file"
}

self_test() {
    local tmpdir
    local repo
    local script_path
    local output
    local commit2
    local commit4

    tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/git-batch-push-test.XXXXXX")"
    script_path="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
    trap "rm -rf '$tmpdir'" EXIT

    git init -q --bare "$tmpdir/remote.git"
    git init -q "$tmpdir/repo"
    repo="$tmpdir/repo"
    git -C "$repo" config user.email "batch-push@example.invalid"
    git -C "$repo" config user.name "batch push test"
    git -C "$repo" remote add origin "$tmpdir/remote.git"
    git -C "$repo" switch -q -c main

    if output="$(cd "$repo" && "$script_path" 2>&1)"; then
        die "self-test expected missing remote to fail"
    fi
    grep -q 'remote is required' <<<"$output" ||
        die "self-test expected an explicit remote requirement"

    for i in 1 2 3 4 5; do
        printf 'commit %s\n' "$i" > "$repo/file.txt"
        git -C "$repo" add file.txt
        git -C "$repo" commit -q -m "commit $i"
    done

    output="$(cd "$repo" && DRY_RUN=1 "$script_path" origin main 2)"

    grep -q ':refs/heads/batch-push-temp' <<<"$output" ||
        die "self-test expected full temporary ref in push command"
    grep -q 'git push origin main:refs/heads/main' <<<"$output" ||
        die "self-test expected full final branch ref in push command"
    if grep '^git push ' <<<"$output" | grep -q '\.\.'; then
        die "self-test found invalid range refspec in push command"
    fi

    commit2="$(git -C "$repo" rev-list --reverse main | sed -n '2p')"
    commit4="$(git -C "$repo" rev-list --reverse main | sed -n '4p')"
    git -C "$repo" push -q origin "$commit2:refs/heads/batch-push-temp"

    output="$(cd "$repo" && DRY_RUN=1 "$script_path" origin main 2)"
    grep -q 'Resume: starting from batch 2' <<<"$output" ||
        die "self-test expected resume from batch 2"
    grep -q "git push origin +${commit4}:refs/heads/batch-push-temp" <<<"$output" ||
        die "self-test expected resumed temporary ref push to batch 2 endpoint"
    if grep -q "git push origin +${commit2}:refs/heads/batch-push-temp" <<<"$output"; then
        die "self-test unexpectedly restarted from batch 1 endpoint"
    fi

    output="$(cd "$repo" && "$script_path" origin main 2)"
    [[ "$(git -C "$tmpdir/remote.git" rev-parse refs/heads/main)" == \
       "$(git -C "$repo" rev-parse main)" ]] ||
        die "self-test expected actual resumed push to reach the branch tip"
    output="$(cd "$repo" && DRY_RUN=1 "$script_path" origin main 2)"
    grep -q 'Remote branch is already at' <<<"$output" ||
        die "self-test expected already-up-to-date remote branch detection"

    echo "self-test passed"
}

main() {
    case "${1:-}" in
        -h|--help)
            usage
            ;;
        --self-test)
            self_test
            ;;
        *)
            batch_push_main "$@"
            ;;
    esac
}

main "$@"
