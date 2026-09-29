#!/bin/bash
# Bring G8BPQ's latest LinBPQ into a pull request. Run by
# .github/workflows/upstream-sync.yml from a checkout of main; see
# pdn/README.md for how updates flow.
#
#   pdn/upstream-sync.sh
#
# 1. Fetch John's master from his own git server, or from his GitHub mirror if
#    that is unreachable (the two carry the same commits).
# 2. Fast-forward our `upstream` branch to it. `upstream` only ever mirrors
#    John's master; if his master is not a fast-forward of it, stop.
# 3. Stop, successfully, if main already has that commit, or if a pull request
#    (or an issue standing in for one) was opened for it before, whatever
#    became of it.
# 4. Merge it into sync/upstream-<LinBPQ version>, a branch off main, run the
#    full test suite (pdn/tests/ci.sh), push the branch and open a pull request
#    into main with the result. On a merge conflict the branch is John's commit
#    itself, so the pull request shows the conflicts; no tests are run. Where
#    the organisation doesn't let workflows open pull requests, it opens an
#    issue with the same text and a one-click link to open the pull request.
#
# Exits 0 when there is nothing to do or the tests passed, 1 after opening a
# pull request for a conflict or a test failure, 2 on any other problem.
#
# Environment: GH_TOKEN (push and pull request rights on this repository),
# GITHUB_REPOSITORY (owner/name), and for trying it out:
#   DRY_RUN=1        fetch, merge and test, but push nothing and open nothing
#   TEST_STAGES=...  pdn/tests/ci.sh stages to run (default: all of them)
#   JOHN_GIT, JOHN_GITHUB   the two sources, in order of preference

set -uo pipefail

JOHN_GIT=${JOHN_GIT:-git://vps1.g8bpq.net/linbpq}
JOHN_GITHUB=${JOHN_GITHUB:-https://github.com/g8bpq/linbpq}
DRY_RUN=${DRY_RUN:-0}
REPO=${GITHUB_REPOSITORY:-packet-net/linmail-pdn}
RUN_URL=""
if [ -n "${GITHUB_RUN_ID:-}" ]; then
	RUN_URL="${GITHUB_SERVER_URL:-https://github.com}/$REPO/actions/runs/$GITHUB_RUN_ID"
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

say() {
	echo "sync: $*"
}

die() {
	echo "sync: ERROR: $*"
	summary "Upstream sync failed: $*"
	exit 2
}

summary() {
	if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
		echo "$*" >> "$GITHUB_STEP_SUMMARY"
	fi
}

push() {
	if [ "$DRY_RUN" = 1 ]; then
		say "dry run, would run: git push $*"
	else
		git push "$@" || die "git push $* failed"
	fi
}

cd "$(git rev-parse --show-toplevel)" || die "not in a git checkout"

if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
	die "the checkout has uncommitted changes"
fi

# The upstream files linmail-pdn builds, read now: after a conflict the
# checkout is G8BPQ's tree, which has no pdn/
UPSTREAM_FILES="$(make -s -C pdn upstream-sources) templatedefs.c"

if ! git config user.email > /dev/null; then
	git config user.name "github-actions[bot]"
	git config user.email "41898282+github-actions[bot]@users.noreply.github.com"
fi

# 1. John's master

SOURCE=""
for url in "$JOHN_GIT" "$JOHN_GITHUB"; do
	say "fetching master from $url"
	if timeout 300 git fetch --no-tags "$url" "+refs/heads/master:refs/sync/john-master"; then
		SOURCE=$url
		break
	fi
	say "could not fetch from $url"
done
[ -n "$SOURCE" ] || die "could not fetch G8BPQ's master from either source"

TARGET=$(git rev-parse refs/sync/john-master)
SUBJECT=$(git log -1 --format=%s "$TARGET")
say "G8BPQ's master is $TARGET ($SUBJECT), from $SOURCE"

git fetch --no-tags origin "+refs/heads/main:refs/remotes/origin/main" \
	"+refs/heads/upstream:refs/remotes/origin/upstream" || die "could not fetch main and upstream from origin"

# 2. Fast-forward upstream

OLD_UPSTREAM=$(git rev-parse refs/remotes/origin/upstream)

if [ "$OLD_UPSTREAM" = "$TARGET" ]; then
	say "upstream is already at $TARGET"
elif git merge-base --is-ancestor "$OLD_UPSTREAM" "$TARGET"; then
	say "fast-forwarding upstream from $OLD_UPSTREAM to $TARGET"
	push origin "$TARGET:refs/heads/upstream"
else
	die "G8BPQ's master ($TARGET) is not a fast-forward of our upstream branch ($OLD_UPSTREAM). His history has been rewritten; sort this out by hand."
fi

# 3. Anything new?

if git merge-base --is-ancestor "$TARGET" refs/remotes/origin/main; then
	say "main already has G8BPQ's $TARGET ($SUBJECT). Nothing to do."
	summary "Nothing new: main already has G8BPQ's \`$TARGET\` ($SUBJECT), fetched from $SOURCE."
	exit 0
fi

# A pull request, or the issue opened in place of one (see the end), names
# the upstream commit in its description
FIND=".[] | select(.body | contains(\"$TARGET\")) | .url"
EXISTING=$(gh pr list --repo "$REPO" --state all --limit 1000 --json url,body --jq "$FIND" | head -n 1) \
	|| die "could not list pull requests"
if [ -z "$EXISTING" ]; then
	EXISTING=$(gh issue list --repo "$REPO" --state all --limit 1000 --json url,body --jq "$FIND" | head -n 1) \
		|| die "could not list issues"
fi
if [ -n "$EXISTING" ]; then
	say "G8BPQ's $TARGET was already raised: $EXISTING. Nothing to do."
	summary "Nothing to do: G8BPQ's \`$TARGET\` was already raised, $EXISTING"
	exit 0
fi

# 4. Version and branch. John's commit subject is the LinBPQ version
#    (6.0.25.41); Versions.h carries the same as KVerstring. If the subject is
#    something else, use Versions.h.

VERSIONS_H=$(git show "$TARGET:Versions.h" | sed -n 's/^#define KVerstring "\([0-9.]*\).*/\1/p')
NOTE=""
if echo "$SUBJECT" | grep -Eq '^[0-9]+(\.[0-9]+){3}$'; then
	VERSION=$SUBJECT
	if [ "$VERSION" != "$VERSIONS_H" ]; then
		NOTE="Note: the commit subject says $VERSION but Versions.h says ${VERSIONS_H:-nothing}."
	fi
else
	VERSION=${VERSIONS_H:-unknown}
	NOTE="Note: G8BPQ's commit subject (\"$SUBJECT\") is not a version, so the version here comes from Versions.h."
fi
SHORT=$(git rev-parse --short=10 "$TARGET")

BRANCH="sync/upstream-$VERSION"
if git ls-remote --exit-code --heads origin "$BRANCH" > /dev/null; then
	git fetch --no-tags origin "+refs/heads/$BRANCH:refs/sync/old-branch" || die "could not fetch $BRANCH"
	if ! git merge-base --is-ancestor "$TARGET" refs/sync/old-branch; then
		BRANCH="$BRANCH-$SHORT"		# an earlier commit with the same version
	fi
fi
say "LinBPQ $VERSION, branch $BRANCH"

# 5. Merge and test

git checkout -q -B "$BRANCH" refs/remotes/origin/main || die "could not create $BRANCH"

OUTCOME=pass
CONFLICTS=""
LOG="$WORK/test.log"

if git merge --no-ff --no-edit -m "Merge LinBPQ $VERSION from G8BPQ" "$TARGET"; then
	say "merged cleanly; running the tests"
	echo "pdn/tests/ci.sh ${TEST_STAGES:-}" > "$LOG"
	# shellcheck disable=SC2086
	if pdn/tests/ci.sh ${TEST_STAGES:-} 2>&1 | tee -a "$LOG"; then
		say "tests passed"
	else
		OUTCOME=fail
		say "tests failed"
	fi
else
	OUTCOME=conflict
	CONFLICTS=$(git diff --name-only --diff-filter=U)
	git merge --abort
	git checkout -q -B "$BRANCH" "$TARGET"
	say "the merge conflicts in: $(echo "$CONFLICTS" | tr '\n' ' ')"
fi

MERGED=$(git rev-parse HEAD)
push --force origin "HEAD:refs/heads/$BRANCH"

# 6. The pull request

COMMITS=$(git rev-list --count "$OLD_UPSTREAM..$TARGET" 2>/dev/null || echo "?")
STAT=$(git diff --shortstat "$OLD_UPSTREAM" "$TARGET")
# shellcheck disable=SC2086
MAILFILES=$(git diff --name-only "$OLD_UPSTREAM" "$TARGET" -- $UPSTREAM_FILES '*.h' | tr '\n' ' ')

# shellcheck disable=SC2016
CONFLICT_LIST=$(echo "$CONFLICTS" | sed 's/^/- `/; s/$/`/')

case $OUTCOME in
pass)
	TITLE="Sync LinBPQ $VERSION from G8BPQ"
	RESULT="## Tests: passed"$'\n\n'"The test suite passed on the merged tree: the fake RHP server and web suites, the same under AddressSanitizer, the tests against a real pdn node and LinBPQ, and the .deb installed and run by pdn." ;;
fail)
	TITLE="Sync LinBPQ $VERSION from G8BPQ (TESTS FAILING)"
	RESULT="## Tests: FAILED"$'\n\n'"The merge was clean but the test suite failed on the merged tree. Don't merge this until it is fixed: push fixes to this branch (\`$BRANCH\`), keeping G8BPQ's files unedited." ;;
conflict)
	TITLE="Sync LinBPQ $VERSION from G8BPQ (MERGE CONFLICTS)"
	RESULT="## Tests: not run, the merge conflicts"$'\n\n'"Merging G8BPQ's commit into main conflicts in:"$'\n\n'"$CONFLICT_LIST"$'\n\n'"This branch is G8BPQ's commit itself. To resolve: \`git fetch origin && git checkout $BRANCH && git merge origin/main\`, fix the conflicts (keeping G8BPQ's files as he has them), run \`pdn/tests/ci.sh\`, and push." ;;
esac

BODY="$WORK/body.md"
{
	echo "G8BPQ's LinBPQ master has moved on. This pull request merges it into main."
	echo
	echo "- Upstream commit: \`$TARGET\` (\"$SUBJECT\", $(git log -1 --format=%cs "$TARGET"))"
	echo "- LinBPQ version: $VERSION"
	echo "- Fetched from: $SOURCE"
	echo "- Since the last sync (\`$OLD_UPSTREAM\`): $COMMITS commit(s),$STAT"
	echo "- Files linmail-pdn builds, and headers, that changed: ${MAILFILES:-none}"
	[ -n "$NOTE" ] && echo "- $NOTE"
	echo
	echo "$RESULT"
	echo
	if [ -n "$RUN_URL" ]; then
		echo "Workflow run: $RUN_URL"
		echo
	fi
	if [ "$OUTCOME" != conflict ]; then
		echo "Test summary (\`pdn/tests/ci.sh ${TEST_STAGES:-}\`):"
		echo
		echo '```'
		grep -E '^=== |^=+ .*(passed|failed|error|skipped).* =+$' "$LOG" | cut -c 1-300 | tr -d '\r' \
			|| echo "(no stage got as far as starting)"
		echo '```'
		echo
	fi
	if [ "$OUTCOME" = fail ]; then
		echo "<details open><summary>Last 100 lines of the test log</summary>"
		echo
		echo '```'
		tail -n 100 "$LOG" | cut -c 1-300 | tr -d '\r'
		echo '```'
		echo
		echo "</details>"
		echo
	fi
	echo "## Before merging"
	echo
	echo "- Pull requests opened by a workflow don't start the test workflow, so the result above, from the sync workflow itself, is the evidence. Pushing to this branch by hand does start it."
	echo "- Look over G8BPQ's changes to the files listed above for anything the shim relies on: the host API calls in pdnhost.c, the node structures it fakes, the copied \`struct HtmlFormDir\` in pdnweb.c, and the stubs in pdnstubs.c."
	echo "- Merge with a merge commit, not squash or rebase, so main keeps G8BPQ's history and the next sync merges cleanly."
	echo
	echo "Opened by .github/workflows/upstream-sync.yml (pdn/upstream-sync.sh)."
} > "$BODY"

if [ "$DRY_RUN" = 1 ]; then
	say "dry run, would open a pull request from $BRANCH ($MERGED) into main:"
	echo "Title: $TITLE"
	cat "$BODY"
	PR="(dry run)"
else
	if PR=$(gh pr create --repo "$REPO" --base main --head "$BRANCH" --title "$TITLE" --body-file "$BODY" 2> "$WORK/pr.err"); then
		say "opened $PR"
	elif grep -q "not permitted to create or approve pull requests" "$WORK/pr.err"; then
		# The organisation doesn't let workflows open pull requests. Open an
		# issue with the same text instead, and a link that opens the pull
		# request in one click (and closes the issue when it is merged).
		say "workflows may not open pull requests here; opening an issue instead"
		PR=$(gh issue create --repo "$REPO" --title "$TITLE" --body-file "$BODY") || die "could not open an issue either"
		NUMBER=${PR##*/}
		COMPARE="${GITHUB_SERVER_URL:-https://github.com}/$REPO/compare/main...$BRANCH?expand=1&title=$(jq -rn --arg s "$TITLE" '$s|@uri')&body=$(jq -rn --arg s "Closes #$NUMBER, which has the details and the test result." '$s|@uri')"
		{
			echo "**[Open the pull request]($COMPARE)** for \`$BRANCH\`. (This repository's organisation doesn't let workflows open pull requests, so the sync workflow opened this issue instead.)"
			echo
			cat "$BODY"
		} > "$WORK/issue.md"
		gh issue edit "$PR" --repo "$REPO" --body-file "$WORK/issue.md" > /dev/null || die "could not add the pull request link to $PR"
		say "opened $PR"
	else
		cat "$WORK/pr.err"
		die "could not open the pull request"
	fi
fi

summary "LinBPQ $VERSION (\`$TARGET\`): $TITLE, $PR"

if [ "$OUTCOME" = pass ]; then
	exit 0
fi
say "$TITLE: see $PR"
exit 1
