#!/usr/bin/env bash
# Create/update a Gitee release and upload a firmware asset.
# Requires env: GITEE_TOKEN, GITEE_OWNER, GITEE_REPO, TAG, BODY_FILE, ASSET_PATH
set -euo pipefail

: "${GITEE_TOKEN:?}"
: "${GITEE_OWNER:?}"
: "${GITEE_REPO:?}"
: "${TAG:?}"
: "${BODY_FILE:?}"
: "${ASSET_PATH:?}"

API="https://gitee.com/api/v5"
NAME="${RELEASE_NAME:-$TAG}"
ASSET_NAME="$(basename "$ASSET_PATH")"

if [[ ! -f "$BODY_FILE" ]]; then
  echo "missing body file: $BODY_FILE" >&2
  exit 1
fi
if [[ ! -f "$ASSET_PATH" ]]; then
  echo "missing asset: $ASSET_PATH" >&2
  exit 1
fi

BODY="$(python3 - <<'PY' "$BODY_FILE"
import json, pathlib, sys
print(json.dumps(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")))
PY
)"

echo "[gitee] ensure release ${TAG} on ${GITEE_OWNER}/${GITEE_REPO}"

# Try create; if exists, update
HTTP_CODE="$(
  curl -sS -o /tmp/gitee_release.json -w "%{http_code}" \
    -X POST "${API}/repos/${GITEE_OWNER}/${GITEE_REPO}/releases" \
    -H "Content-Type: application/json" \
    -d "{\"access_token\":\"${GITEE_TOKEN}\",\"tag_name\":\"${TAG}\",\"name\":\"${NAME}\",\"body\":${BODY},\"target_commitish\":\"${TARGET_COMMITISH:-master}\"}"
)"

if [[ "$HTTP_CODE" != "201" && "$HTTP_CODE" != "200" ]]; then
  echo "[gitee] create returned ${HTTP_CODE}, try update by tag"
  curl -sS -o /tmp/gitee_release.json -f \
    -X PATCH "${API}/repos/${GITEE_OWNER}/${GITEE_REPO}/releases/tags/${TAG}" \
    -H "Content-Type: application/json" \
    -d "{\"access_token\":\"${GITEE_TOKEN}\",\"name\":\"${NAME}\",\"body\":${BODY}}"
fi

RELEASE_ID="$(python3 - <<'PY'
import json
print(json.load(open("/tmp/gitee_release.json", encoding="utf-8")).get("id", ""))
PY
)"

if [[ -z "$RELEASE_ID" ]]; then
  echo "[gitee] failed to resolve release id; response:" >&2
  cat /tmp/gitee_release.json >&2 || true
  exit 1
fi

echo "[gitee] upload asset ${ASSET_NAME} to release id=${RELEASE_ID}"
curl -sS -f \
  -X POST "${API}/repos/${GITEE_OWNER}/${GITEE_REPO}/releases/${RELEASE_ID}/attach_files" \
  -F "access_token=${GITEE_TOKEN}" \
  -F "file=@${ASSET_PATH};filename=${ASSET_NAME}" \
  -o /tmp/gitee_asset.json

echo "[gitee] done"
