#!/bin/sh
# Turns a release-notes file into the text of a GitHub release (used by the CI
# when a version tag is pushed): wrapped lines are joined (GitHub shows every
# line break of a release text), and links into docs/ point at that version's
# files on GitHub.
#   packaging/release-body.sh packaging/release-notes-X.Y.Z.md OWNER/REPO vX.Y.Z
set -eu
awk '
/^[[:space:]]*$/ { if (buf != "") print buf; buf = ""; print ""; next }
/^(#|- |\* |[0-9]+\. |\||>)/ { if (buf != "") print buf; buf = $0; next }
{ sub(/^[[:space:]]+/, ""); buf = (buf == "" ? $0 : buf " " $0) }
END { if (buf != "") print buf }
' "$1" | sed "s#](docs/#](https://github.com/$2/blob/$3/docs/#g"
