# Fire the XDG-autostart runner: per-user copy if present, else the one the
# installer ships system-wide.
_runner="$HOME/.local/bin/schema-autostart-runner.sh"
[ -x "$_runner" ] || _runner=/usr/local/lib/schema/schema-autostart-runner.sh
[ -x "$_runner" ] && setsid "$_runner" >/dev/null 2>&1 &
unset _runner
