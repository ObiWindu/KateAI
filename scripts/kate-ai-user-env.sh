# Kate AI — prepend the user-local Qt plugin dir so Kate can load a
# private ~/.local install. Sourced by KDE at login from
# ~/.config/plasma-workspace/env/
kateai_plugins="${HOME}/.local/lib/qt6/plugins"
case ":${QT_PLUGIN_PATH:-}:" in
  *":${kateai_plugins}:"*) ;;
  *) export QT_PLUGIN_PATH="${kateai_plugins}${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}" ;;
esac
unset kateai_plugins
