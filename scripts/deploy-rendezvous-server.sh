#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID:-$(id -u)} -eq 0 ]]; then
  echo "Run this script as a normal user with sudo access, not as root." >&2
  exit 2
fi

PUBLIC_HOST=${1:-rendezvous.opal.xt9y.de}
PORT=${OPAL_RENDEZVOUS_PORT:-47992}

command -v make >/dev/null 2>&1 || { echo "make is required" >&2; exit 1; }
command -v sudo >/dev/null 2>&1 || { echo "sudo is required" >&2; exit 1; }

make rendezvous-server
sudo install -m 0755 build/opal-rendezvous /usr/local/bin/opal-rendezvous
sudo install -m 0644 systemd/opal-rendezvous.service /etc/systemd/system/opal-rendezvous.service
sudo mkdir -p /etc/systemd/system/opal-rendezvous.service.d
sudo tee /etc/systemd/system/opal-rendezvous.service.d/endpoint.conf >/dev/null <<EOF
[Service]
Environment=OPAL_RENDEZVOUS_BIND=::
Environment=OPAL_RENDEZVOUS_PUBLIC_HOST=${PUBLIC_HOST}
Environment=OPAL_RENDEZVOUS_PORT=${PORT}
EOF

if command -v ufw >/dev/null 2>&1 && sudo ufw status 2>/dev/null | grep -q '^Status: active'; then
  sudo ufw allow "${PORT}/udp"
fi
if command -v firewall-cmd >/dev/null 2>&1 && sudo firewall-cmd --state >/dev/null 2>&1; then
  sudo firewall-cmd --permanent --add-port="${PORT}/udp"
  sudo firewall-cmd --reload
fi

sudo systemctl daemon-reload
sudo systemctl enable --now opal-rendezvous.service
sudo systemctl --no-pager --full status opal-rendezvous.service

echo
echo "OPAL rendezvous/relay service is running on UDP ${PORT}."
echo "Point ${PUBLIC_HOST} to this VPS public IP if DNS is not already configured."
