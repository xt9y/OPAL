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

discovery_rule="47993/udp"
reply_rule="47994/udp"

if command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
  if [ "$action" = "install" ]; then
    firewall-cmd --quiet --permanent --add-port="$discovery_rule"
    firewall-cmd --quiet --add-port="$discovery_rule"
    firewall-cmd --quiet --permanent --add-port="$reply_rule"
    firewall-cmd --quiet --add-port="$reply_rule"
  else
    firewall-cmd --quiet --permanent --remove-port="$reply_rule" >/dev/null 2>&1 || true
    firewall-cmd --quiet --remove-port="$reply_rule" >/dev/null 2>&1 || true
    firewall-cmd --quiet --permanent --remove-port="$discovery_rule" >/dev/null 2>&1 || true
    firewall-cmd --quiet --remove-port="$discovery_rule" >/dev/null 2>&1 || true
  fi
  exit 0
fi

if command -v ufw >/dev/null 2>&1; then
  ufw_active=0
  if LC_ALL=C ufw status 2>/dev/null | grep -q '^Status: active'; then
    ufw_active=1
  fi
  if [ "$action" = "install" ]; then
    ufw allow "$discovery_rule" comment 'OPAL LAN discovery'
    ufw allow "$reply_rule" comment 'OPAL LAN discovery replies'
    if [ "$ufw_active" -eq 1 ]; then
      ufw reload >/dev/null
    fi
  else
    ufw --force delete allow "$reply_rule" >/dev/null 2>&1 || true
    ufw --force delete allow "$discovery_rule" >/dev/null 2>&1 || true
    if [ "$ufw_active" -eq 1 ]; then
      ufw reload >/dev/null 2>&1 || true
    fi
  fi
  exit 0
fi

exit 0
