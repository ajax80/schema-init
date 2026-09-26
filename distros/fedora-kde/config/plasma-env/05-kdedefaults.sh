# startplasma prepends the global-theme defaults dir; do the same.
case ":$XDG_CONFIG_DIRS:" in
    *":$HOME/.config/kdedefaults:"*) ;;
    *) export XDG_CONFIG_DIRS="$HOME/.config/kdedefaults${XDG_CONFIG_DIRS:+:$XDG_CONFIG_DIRS}" ;;
esac
