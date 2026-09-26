# Fixed ssh-agent socket; schema-autostart-runner.sh starts the agent on it.
export SSH_AUTH_SOCK="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/ssh-agent.socket"
