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
firewalld_reply_rule='rule source-port port="47993" protocol="udp" accept'

if command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
  if [ "$action" = "install" ]; then
    firewall-cmd --quiet --permanent --add-port="$rule"
    firewall-cmd --quiet --add-port="$rule"
    firewall-cmd --quiet --permanent --add-rich-rule="$firewalld_reply_rule"
    firewall-cmd --quiet --add-rich-rule="$firewalld_reply_rule"
  else
    firewall-cmd --quiet --permanent --remove-rich-rule="$firewalld_reply_rule" >/dev/null 2>&1 || true
    firewall-cmd --quiet --remove-rich-rule="$firewalld_reply_rule" >/dev/null 2>&1 || true
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
    if ! ufw status 2>/dev/null | grep -Fq 'OPAL LAN discovery replies'; then
      ufw allow in proto udp from any port 47993 to any comment 'OPAL LAN discovery replies'
    fi
    ufw reload >/dev/null
  else
    ufw --force delete allow in proto udp from any port 47993 to any >/dev/null 2>&1 || true
    ufw --force delete allow "$rule" >/dev/null 2>&1 || true
    ufw reload >/dev/null 2>&1 || true
  fi
  exit 0
fi

exit 0
