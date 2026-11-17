#!/bin/sh
# wadamesh for the M5Stack Cardputer Zero.
#   ./install.sh             WADAMESH owns the LCD from boot (M5 launcher paused while it runs)
#   ./install.sh --launcher  WADAMESH is an app in M5Stack's launcher; nothing starts at boot
# Both: wadamesh-cli, the HDMI desktop mode, the launcher icon and an example bot hook.
set -e
cd "$(dirname "$0")"
MODE=boot; [ "$1" = "--launcher" ] && MODE=launcher
DATA=/var/lib/wadamesh-zero
for f in wadamesh-zero wadamesh-desktop wadamesh-hdmi wadamesh-cli wadamesh-launch; do [ ! -f "$f" ] || sudo install -m 755 "$f" /usr/local/bin/ || true; done
sudo install -m 644 wadamesh-zero.service /etc/systemd/system/wadamesh-zero.service
sudo install -m 644 wadamesh.desktop /usr/share/applications/wadamesh.desktop
mkdir -p "$HOME/.config/autostart"
install -m 644 wadamesh-autostart.desktop "$HOME/.config/autostart/wadamesh.desktop"
# HDMI desktop mode and the launcher icon pause and resume the LCD version with sudo;
# allow exactly those two commands without a password (nothing else).
SUDOERS=$(mktemp)
echo "$USER ALL=(root) NOPASSWD: /usr/bin/systemctl stop wadamesh-zero, /usr/bin/systemctl start wadamesh-zero" > "$SUDOERS"
if sudo visudo -cf "$SUDOERS" >/dev/null; then
  sudo install -m 440 -o root -g root "$SUDOERS" /etc/sudoers.d/wadamesh
fi
rm -f "$SUDOERS"
sudo mkdir -p "$DATA/hooks" "$DATA/logs"
sudo install -m 755 hooks/on-message.example "$DATA/hooks/on-message.example"
sudo chown -R pi:pi "$DATA"
if [ -d /usr/share/APPLaunch ]; then
  sudo mkdir -p /usr/share/APPLaunch/applications /usr/share/APPLaunch/share/images
  sudo install -m 644 wadamesh-applaunch.desktop /usr/share/APPLaunch/applications/wadamesh.desktop
  sudo install -m 644 wadamesh-icon.png /usr/share/APPLaunch/share/images/wadamesh.png
fi
dpkg -s libsdl2-2.0-0 >/dev/null 2>&1 || sudo apt-get install -y libsdl2-2.0-0
sudo systemctl daemon-reload
if [ "$MODE" = boot ]; then sudo systemctl enable wadamesh-zero; else sudo systemctl disable --now wadamesh-zero 2>/dev/null || true; sudo systemctl restart APPLaunch.service 2>/dev/null || true; fi
echo
echo "Installed ($MODE mode)."
echo "  LCD now:       sudo systemctl restart wadamesh-zero"
echo "  From SSH:      wadamesh-cli status | wadamesh-cli tail | wadamesh-cli send '#public' 'hi'"
echo "  Bot hook:      cp $DATA/hooks/on-message.example $DATA/hooks/on-message"
echo "  HDMI desktop:  WADAMESH in the desktop menu (or automatic at login with HDMI plugged in)"
echo "  Launcher mode: ./install.sh --launcher      Boot mode: ./install.sh"
