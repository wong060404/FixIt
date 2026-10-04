#!/usr/bin/env bash
# Creates (if needed) and pushes the FixIt repository.
#
#   GITHUB_TOKEN=ghp_xxx tools/push_to_github.sh
#
# The token is used only for the two API/git calls below.  It is never written
# into the repository and ~/.git-credentials is left untouched.
set -euo pipefail

ACCOUNT="${ACCOUNT:-wong060404}"
REPO="${REPO:-FixIt}"
: "${GITHUB_TOKEN:?Set GITHUB_TOKEN first, e.g. GITHUB_TOKEN=ghp_xxx tools/push_to_github.sh}"

REMOTE="https://github.com/${ACCOUNT}/${REPO}.git"

echo "==> Checking GitHub authentication"
curl -sS -o /tmp/gh_user.json -w "    HTTP %{http_code}\n" \
  -H "Authorization: Bearer ${GITHUB_TOKEN}" \
  -H "Accept: application/vnd.github+json" \
  https://api.github.com/user
python3 - <<'PY'
import json
with open('/tmp/gh_user.json') as handle:
    data = json.load(handle)
print("    authenticated as:", data.get('login'), "|", data.get('name'))
PY

echo "==> Checking whether ${ACCOUNT}/${REPO} already exists"
CODE=$(curl -sS -o /tmp/gh_repo.json -w '%{http_code}' \
  -H "Authorization: Bearer ${GITHUB_TOKEN}" \
  -H "Accept: application/vnd.github+json" \
  "https://api.github.com/repos/${ACCOUNT}/${REPO}")

if [ "$CODE" = "200" ]; then
  echo "    repository exists - pushing into it"
elif [ "$CODE" = "404" ]; then
  echo "    not found - creating it (public)"
  curl -sS -X POST \
    -H "Authorization: Bearer ${GITHUB_TOKEN}" \
    -H "Accept: application/vnd.github+json" \
    https://api.github.com/user/repos \
    -d "{\"name\":\"${REPO}\",\"description\":\"FixIt - fuzzy-patch agentic C++ repair loop: compiler diagnostics -> code location -> LLM patch -> tolerant apply -> re-verify.\",\"private\":false,\"has_issues\":true,\"has_wiki\":true}" \
    -o /tmp/gh_create.json -w "    HTTP %{http_code}\n"
else
  echo "    unexpected HTTP $CODE from the API:"; cat /tmp/gh_repo.json; exit 1
fi

echo "==> Configuring remote and pushing"
git remote remove origin 2>/dev/null || true
git remote add origin "$REMOTE"
# A classic token (ghp_...) authenticates with the git username set to the
# account name.  'x-access-token' is only valid for fine-grained tokens
# (github_pat_...) and GitHub App installations, and with a classic token it fails
# with "Invalid username or token" -- verified against the API while pushing this
# repository.  GITHUB_USER overrides the account when needed.
git -c credential.helper='!f() { echo "username=${GITHUB_USER:-${ACCOUNT}}"; echo "password=${GITHUB_TOKEN}"; }; f' \
    push -u origin main

echo
echo "==> Done: https://github.com/${ACCOUNT}/${REPO}"
