#!/bin/sh
# Makes the self-signed certificate the login site needs (resources/ctx/server.crt and server.key).
# The Growtopia client asks for www.growtopia1.com / growtopia2.com, so those names are in it; install server.crt
# as a trusted root on every device that should log in (see the README).
set -e
cd "$(dirname "$0")/.."
mkdir -p resources/ctx
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -keyout resources/ctx/server.key -out resources/ctx/server.crt \
  -subj "/CN=growtopia1.com" \
  -addext "subjectAltName=DNS:growtopia1.com,DNS:www.growtopia1.com,DNS:growtopia2.com,DNS:www.growtopia2.com" \
  -addext "basicConstraints=critical,CA:TRUE"
echo "wrote resources/ctx/server.crt and server.key (keep the key private, it is in .gitignore)"
