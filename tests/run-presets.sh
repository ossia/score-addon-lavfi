#!/usr/bin/env bash
# Runs tests/presets.js in a real ossia-score and decides pass/fail from what
# it printed.
#
#   tests/run-presets.sh /path/to/ossia-score
#
# The exit code of score itself is not usable here: score currently crashes in
# ScenarioDocumentPresenter's destructor when it exits from a --script (with an
# empty script too, so it is not about what we do), which replaces whatever
# Qt.exit() was given. What the script prints is unambiguous, so that is what
# this checks.
set -u
score=${1:-ossia-score}
here=$(cd "$(dirname "$0")" && pwd)

# 77 is ctest's skip code: no application to run the presets in (an SDK build,
# or a configure that has not built score yet).
if ! command -v "$score" >/dev/null 2>&1 && [[ ! -x "$score" ]]; then
  echo "run-presets: no ossia-score at '$score', skipping"
  exit 77
fi

log=$(mktemp)
trap 'rm -f "$log"' EXIT

# A previous crash leaves a marker that makes score open a modal "reload?"
# question at startup, which never returns without a user.
rm -f "${TMPDIR:-/tmp}/score_open_docs.${USER:-$(id -un)}"

LAVFI_PRESET_DIR="$here/../Presets/FFmpeg filter" \
SCORE_AUDIO_BACKEND=${SCORE_AUDIO_BACKEND:-dummy} \
QT_QPA_PLATFORM=${QT_QPA_PLATFORM:-offscreen} \
  timeout 300 "$score" --script "$here/presets.js" >"$log" 2>&1

grep -E '^(Info|Critical):' "$log" | sed -e 's/^Info: //' -e 's/^Critical: //' \
  | grep -Ev '^\s*$'

status=0
grep -q "presets: play ok" "$log" || { echo "run-presets: the play phase did not finish"; status=1; }
if grep -q "^Critical: FAIL" "$log"; then status=1; fi
# Anything the addon itself complains about is a failure too.
if grep -q "lavfi: graph configuration failed" "$log"; then
  echo "run-presets: the addon refused a graph:"
  grep "lavfi:" "$log" | sort -u
  status=1
fi
exit $status
