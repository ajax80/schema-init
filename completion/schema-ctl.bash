_schema_ctl_svcs() {
    local d=/etc/schema-init/services f
    for f in "$d"/*.svc "$d"/*.grp; do
        [ -e "$f" ] || continue
        f=${f##*/}
        printf '%s\n' "${f%.*}"
    done
}

_schema_ctl() {
    local cur=${COMP_WORDS[COMP_CWORD]}
    local cmds="status list analyze timing reload reexec start up stop down restart add cat graph pet reset reboot poweroff --help --version"
    if [ "$COMP_CWORD" -eq 1 ]; then
        COMPREPLY=($(compgen -W "$cmds" -- "$cur"))
        return
    fi
    case ${COMP_WORDS[1]} in
        status)  COMPREPLY=($(compgen -W "--json --kv $(_schema_ctl_svcs)" -- "$cur")) ;;
        reload)  COMPREPLY=($(compgen -W "--evict" -- "$cur")) ;;
        add|reexec) COMPREPLY=($(compgen -f -- "$cur")) ;;
        analyze|start|up|stop|down|restart|cat|graph|pet|reset)
                 [ "$COMP_CWORD" -eq 2 ] && COMPREPLY=($(compgen -W "$(_schema_ctl_svcs)" -- "$cur")) ;;
    esac
}

complete -F _schema_ctl schema-ctl
