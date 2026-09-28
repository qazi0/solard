#!/bin/sh
# solard installer for macOS and Linux -- the local server for the QuickSolar app.
#
#   curl -fsSL https://raw.githubusercontent.com/qazi0/solard/main/install/install.sh | sh
#
# It downloads one small program into ~/.solard, finds your GoodWe inverter on this
# network, and starts it automatically (macOS: at login, Linux: at boot). Nothing
# else is installed. Afterwards open the QuickSolar app: it finds this server.
#
# Options (environment variables):
#   SOLARD_CAPACITY=15      usable battery size in kWh (for "battery will last")
#   SOLARD_HOST=192.168.1.50  inverter address, if it isn't found automatically
#   SOLARD_HTTP=8768        port of the dashboard / app API
#   SOLARD_HOME=~/.solard   where it lives (program, settings, history)
# Uninstall:
#   curl -fsSL https://raw.githubusercontent.com/qazi0/solard/main/install/install.sh | sh -s -- --uninstall
set -e

BASE="${SOLARD_BASE_URL:-https://github.com/qazi0/solard/releases/latest/download}"
DIR="${SOLARD_HOME:-$HOME/.solard}"
PORT="${SOLARD_HTTP:-8768}"
LABEL="io.github.qazi0.solard"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"

say() { printf '%s\n' "$*"; }
die() { printf 'solard: %s\n' "$*" >&2; exit 1; }
sudo_() { if [ "$(id -u)" = 0 ]; then "$@"; else sudo "$@"; fi; }
have() { command -v "$1" >/dev/null 2>&1; }

OS=$(uname -s); ARCH=$(uname -m)
case "$OS" in
  Darwin) FILE=solard-macos ;;
  Linux) case "$ARCH" in
           x86_64|amd64) FILE=solard-linux-x86_64 ;;
           aarch64|arm64) FILE=solard-linux-arm64 ;;
           *) die "unsupported CPU: $ARCH (x86-64 and ARM64 are supported)" ;;
         esac ;;
  *) die "unsupported system: $OS (use install.ps1 on Windows)" ;;
esac

stop_service() {
  if [ "$OS" = Darwin ]; then
    launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
  elif have systemctl; then
    if [ -f /etc/systemd/system/solard.service ]; then sudo_ systemctl disable --now solard 2>/dev/null || true; fi
    systemctl --user disable --now solard 2>/dev/null || true
  fi
}

if [ "${1:-}" = --uninstall ]; then
  stop_service
  rm -f "$PLIST" "$HOME/.config/systemd/user/solard.service"
  if [ -f /etc/systemd/system/solard.service ]; then sudo_ rm -f /etc/systemd/system/solard.service; sudo_ systemctl daemon-reload; fi
  rm -f "$DIR/solard"
  say "solard removed. Your settings and history are still in $DIR (delete that folder to remove them too)."
  exit 0
fi

say "Installing solard into $DIR"
mkdir -p "$DIR/data"
stop_service
curl -fsSL "$BASE/$FILE" -o "$DIR/solard.new" || die "download failed: $BASE/$FILE"
chmod +x "$DIR/solard.new"
mv -f "$DIR/solard.new" "$DIR/solard"

CONF="$DIR/solard.conf"
if [ ! -f "$CONF" ]; then
  {
    echo "# solard settings (key = value). Restart solard after changes."
    echo "http = $PORT"
    if [ -n "$SOLARD_CAPACITY" ]; then echo "capacity = $SOLARD_CAPACITY"; else echo "# capacity = 15      # usable battery kWh"; fi
    if [ -n "$SOLARD_HOST" ]; then echo "host = $SOLARD_HOST"; else echo "# host = found automatically"; fi
    if [ -n "$SOLARD_PORT" ]; then echo "port = $SOLARD_PORT"; fi
    echo "# lat = 51.50       # for sunrise/sunset on the dashboard"
    echo "# lon = -0.12"
  } > "$CONF"
fi

if ! grep -q '^host' "$CONF"; then
  say "Looking for your GoodWe inverter on this network..."
  if [ "$OS" = Darwin ]; then say "  macOS will ask to let solard \"find devices on your local network\": click Allow."; fi
  IP=""
  for try in 1 2 3 4; do                       # time to answer the macOS prompt
    if OUT=$("$DIR/solard" --config "$CONF" --discover 2>/dev/null); then
      IP=$(printf '%s\n' "$OUT" | sed -n 's/^found: //p'); break
    fi
    [ "$OS" = Darwin ] || [ "$try" -lt 2 ] || break
    sleep 5
  done
  if [ -n "$IP" ]; then
    say "  found it at $IP"
    echo "host = $IP" >> "$CONF"
  else
    say "  not found yet -- solard keeps looking in the background."
    say "  (If it never shows up: re-run with SOLARD_HOST=<inverter address>.)"
  fi
fi

if [ "$OS" = Darwin ]; then
  mkdir -p "$HOME/Library/LaunchAgents"
  cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key><array><string>$DIR/solard</string><string>--config</string><string>$CONF</string></array>
  <key>WorkingDirectory</key><string>$DIR</string>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ProcessType</key><string>Background</string>
  <key>StandardOutPath</key><string>$DIR/solard.log</string>
  <key>StandardErrorPath</key><string>$DIR/solard.log</string>
</dict></plist>
PL
  launchctl bootstrap "gui/$(id -u)" "$PLIST"
  IP=$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || echo localhost)
  WHEN="whenever you're logged in to this Mac"
elif have systemctl && [ -d /run/systemd/system ]; then
  UNIT="[Unit]
Description=solard (QuickSolar server for GoodWe inverters)
Wants=network-online.target
After=network-online.target
[Service]
ExecStart=$DIR/solard --config $CONF
WorkingDirectory=$DIR
Restart=always
RestartSec=5"
  if [ "$(id -u)" = 0 ] || have sudo; then
    say "Setting it up to start at boot (you may be asked for your password)..."
    printf '%s\nUser=%s\n[Install]\nWantedBy=multi-user.target\n' "$UNIT" "$(id -un)" | sudo_ tee /etc/systemd/system/solard.service >/dev/null
    sudo_ systemctl daemon-reload
    sudo_ systemctl enable --now solard >/dev/null 2>&1
  else
    mkdir -p "$HOME/.config/systemd/user"
    printf '%s\n[Install]\nWantedBy=default.target\n' "$UNIT" > "$HOME/.config/systemd/user/solard.service"
    systemctl --user daemon-reload && systemctl --user enable --now solard >/dev/null 2>&1
    loginctl enable-linger "$(id -un)" 2>/dev/null || true
  fi
  IP=$(hostname -I 2>/dev/null | awk '{print $1}')
  WHEN="at every boot"
else
  nohup "$DIR/solard" --config "$CONF" >> "$DIR/solard.log" 2>&1 &
  IP=$(hostname -I 2>/dev/null | awk '{print $1}')
  WHEN="until this machine restarts (no systemd found: add it to your startup yourself)"
fi

sleep 2
if curl -fsS -m 3 "http://127.0.0.1:$PORT/ping" >/dev/null 2>&1; then
  say ""
  say "solard is running, and starts $WHEN."
  say "  Dashboard:      http://${IP:-localhost}:$PORT/"
  say "  Phone / TV app: open QuickSolar -> it finds this server automatically."
  say "  Settings:       $CONF      History: $DIR/data"
  if [ "$OS" = Darwin ]; then say "  Keep this Mac awake (System Settings -> Energy) so it records all day."; fi
else
  say "solard was installed but isn't answering yet -- see $DIR/solard.log"
fi
exit 0
