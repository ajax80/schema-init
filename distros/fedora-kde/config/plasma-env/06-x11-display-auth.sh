# Stable XAUTHORITY path for the session; schema-autostart-runner.sh merges the
# live Xwayland cookie into it. Deliberately no DISPLAY (kwin nested-X11 trap).
export XAUTHORITY="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/Xauthority"
