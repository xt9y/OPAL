#!/bin/sh
set -eu

action="${1:-}"
case "$action" in
  install|remove) ;;
  *) echo "usage: $0 install|remove" >&2; exit 2 ;;
esac

if [ "${OPAL_SKIP_FIREWALL:-0}" = "1" ]; then
  exit 0
fi

rule="47993/udp"

if command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
  if [ "$action" = "install" ]; then
    firewall-cmd --quiet --permanent --add-port="$rule"
    firewall-cmd --quiet --add-port="$rule"
  else
    firewall-cmd --quiet --permanent --remove-port="$rule" >/dev/null 2>&1 || true
    firewall-cmd --quiet --remove-port="$rule" >/dev/null 2>&1 || true
  fi
  exit 0
fi

if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q '^Status: active'; then
  if [ "$action" = "install" ]; then
    if ! ufw status 2>/dev/null | grep -Eq '(^|[[:space:]])47993/udp([[:space:]]|$)'; then
      ufw allow "$rule" comment 'OPAL LAN discovery'
    fi
  else
    ufw --force delete allow "$rule" >/dev/null 2>&1 || true
  fi
  exit 0
fi

exit 0
