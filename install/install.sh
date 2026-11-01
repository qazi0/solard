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
#   SOLARD_AUTO_UPDATE=0    don't update automatically
#
# Updates: once a day it checks GitHub for a newer release, verifies the download
# against the release's SHA256SUMS, swaps the program (keeping solard.prev), restarts
# it and rolls back if the new one doesn't answer. Settings and history are kept.
# Turn it off with "auto_update = false" in solard.conf. Update now:
#   sh ~/.solard/install.sh --update
# Uninstall:
#   curl -fsSL https://raw.githubusercontent.com/qazi0/solard/main/install/install.sh | sh -s -- --uninstall
set -e

REPO="${SOLARD_REPO:-qazi0/solard}"
BASE="${SOLARD_BASE_URL:-https://github.com/$REPO/releases/latest/download}"
DIR="${SOLARD_HOME:-$HOME/.solard}"
PORT="${SOLARD_HTTP:-8768}"
CONF="$DIR/solard.conf"
LABEL="io.github.qazi0.solard"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
UPLIST="$HOME/Library/LaunchAgents/$LABEL.update.plist"

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

restart_service() {
  if [ "$OS" = Darwin ]; then launchctl kickstart -k "gui/$(id -u)/$LABEL" 2>/dev/null || true
  elif [ -f /etc/systemd/system/solard.service ]; then sudo_ systemctl restart solard
  elif have systemctl; then systemctl --user restart solard 2>/dev/null || true
  fi
}

answers() {                                       # solard answering on its port within 20 s
  for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    if curl -fsS -m 2 "http://127.0.0.1:$PORT/ping" >/dev/null 2>&1; then return 0; fi
    sleep 1
  done
  return 1
}

sha256() { if have sha256sum; then sha256sum "$1" | awk '{print $1}'; else shasum -a 256 "$1" | awk '{print $1}'; fi; }

newer() {                                         # newer A B: is version A newer than B? ("dev" counts as oldest)
  [ "$1" = "$2" ] && return 1
  case "$2" in dev*|"") return 0 ;; esac
  case "$1" in dev*|"") return 1 ;; esac
  [ "$(printf '%s\n%s\n' "$1" "$2" | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)" = "$1" ]
}

update() {
  [ -t 1 ] || exec >> "$DIR/update.log" 2>&1        # scheduled runs log to update.log
  say "--- $(date '+%Y-%m-%d %H:%M:%S') update check"
  [ -x "$DIR/solard" ] || die "solard is not installed in $DIR"
  if grep -qiE '^[[:space:]]*auto_update[[:space:]]*=[[:space:]]*(false|0|no|off)' "$CONF" 2>/dev/null && [ "${1:-}" != --now ]; then
    say "auto_update is off in $CONF"; exit 0
  fi
  PORT=$(sed -n 's/^[[:space:]]*http[[:space:]]*=[[:space:]]*\([0-9]*\).*/\1/p' "$CONF" | tail -1); PORT=${PORT:-8768}
  TAG="${SOLARD_UPDATE_TAG:-$(curl -fsSLI -o /dev/null -w '%{url_effective}' "https://github.com/$REPO/releases/latest" | sed 's|.*/tag/||')}"
  case "$TAG" in v[0-9]*) ;; *) die "could not read the latest release ($TAG)" ;; esac
  CUR=$("$DIR/solard" --version 2>/dev/null || echo dev)
  if ! newer "${TAG#v}" "$CUR" && [ -z "$SOLARD_UPDATE_TAG" ]; then say "solard $CUR is up to date"; exit 0; fi
  UB="${SOLARD_UPDATE_BASE:-https://github.com/$REPO/releases/download/$TAG}"
  T="$DIR/update.tmp"; rm -rf "$T"; mkdir -p "$T"
  curl -fsSL "$UB/SHA256SUMS" -o "$T/SHA256SUMS" || die "download failed: SHA256SUMS"
  curl -fsSL "$UB/$FILE" -o "$T/$FILE" || die "download failed: $FILE"
  WANT=$(awk -v f="$FILE" '$2 == f || $2 == "*"f {print $1}' "$T/SHA256SUMS")
  GOT=$(sha256 "$T/$FILE")
  if [ -z "$WANT" ] || [ "$WANT" != "$GOT" ]; then rm -rf "$T"; die "checksum mismatch for $FILE: not installing it"; fi
  chmod +x "$T/$FILE"
  if curl -fsSL "$UB/install.sh" -o "$T/install.sh" 2>/dev/null; then
    W2=$(awk '$2 == "install.sh" {print $1}' "$T/SHA256SUMS")
    if [ -n "$W2" ] && [ "$W2" = "$(sha256 "$T/install.sh")" ]; then cp "$T/install.sh" "$DIR/install.sh"; fi
  fi
  cp -f "$DIR/solard" "$DIR/solard.prev"
  mv -f "$T/$FILE" "$DIR/solard"
  chown "$(ls -ld "$DIR" | awk '{print $3}')" "$DIR/solard" "$DIR/solard.prev" "$DIR/install.sh" 2>/dev/null || true
  rm -rf "$T"
  restart_service
  if answers; then
    say "updated solard $CUR -> $("$DIR/solard" --version)"
  else
    mv -f "$DIR/solard.prev" "$DIR/solard"; restart_service
    die "the new solard did not start: rolled back to $CUR"
  fi
}

if [ "${1:-}" = --update ]; then update "${2:-}"; exit 0; fi

if [ "${1:-}" = --uninstall ]; then
  stop_service
  launchctl bootout "gui/$(id -u)/$LABEL.update" 2>/dev/null || true
  rm -f "$PLIST" "$UPLIST" "$HOME/.config/systemd/user/solard.service" "$HOME/.config/systemd/user/solard-update.service" "$HOME/.config/systemd/user/solard-update.timer"
  systemctl --user disable --now solard-update.timer 2>/dev/null || true
  if [ -f /etc/systemd/system/solard-update.timer ]; then sudo_ systemctl disable --now solard-update.timer 2>/dev/null || true; sudo_ rm -f /etc/systemd/system/solard-update.timer /etc/systemd/system/solard-update.service; fi
  if [ -f /etc/systemd/system/solard.service ]; then sudo_ rm -f /etc/systemd/system/solard.service; sudo_ systemctl daemon-reload; fi
  rm -f "$DIR/solard" "$DIR/solard.prev" "$DIR/install.sh"
  say "solard removed. Your settings and history are still in $DIR (delete that folder to remove them too)."
  exit 0
fi

say "Installing solard into $DIR"
mkdir -p "$DIR/data"
stop_service
curl -fsSL "$BASE/$FILE" -o "$DIR/solard.new" || die "download failed: $BASE/$FILE"
chmod +x "$DIR/solard.new"
mv -f "$DIR/solard.new" "$DIR/solard"

if [ ! -f "$CONF" ]; then
  {
    echo "# solard settings (key = value). Restart solard after changes."
    echo "http = $PORT"
    if [ -n "$SOLARD_CAPACITY" ]; then echo "capacity = $SOLARD_CAPACITY"; else echo "# capacity = 15      # usable battery kWh"; fi
    if [ -n "$SOLARD_HOST" ]; then echo "host = $SOLARD_HOST"; else echo "# host = found automatically"; fi
    if [ -n "$SOLARD_PORT" ]; then echo "port = $SOLARD_PORT"; fi
    echo "# lat = 51.50       # for sunrise/sunset on the dashboard"
    echo "# lon = -0.12"
    if [ "${SOLARD_AUTO_UPDATE:-1}" = 0 ]; then echo "auto_update = false"; else echo "# auto_update = false  # stop the daily update check"; fi
  } > "$CONF"
fi
# keep a copy of this installer: the daily update check runs it with --update
curl -fsSL "$BASE/install.sh" -o "$DIR/install.sh" 2>/dev/null || cp "$0" "$DIR/install.sh" 2>/dev/null || true

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
  if [ -s "$DIR/install.sh" ]; then                # daily update check (and one at login)
    cat > "$UPLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>$LABEL.update</string>
  <key>ProgramArguments</key><array><string>/bin/sh</string><string>$DIR/install.sh</string><string>--update</string></array>
  <key>EnvironmentVariables</key><dict><key>SOLARD_HOME</key><string>$DIR</string></dict>
  <key>StartCalendarInterval</key><dict><key>Hour</key><integer>4</integer><key>Minute</key><integer>$(( $(od -An -N1 -tu1 /dev/urandom) % 60 ))</integer></dict>
  <key>ProcessType</key><string>Background</string>
</dict></plist>
PL
    launchctl bootout "gui/$(id -u)/$LABEL.update" 2>/dev/null || true
    launchctl bootstrap "gui/$(id -u)" "$UPLIST"
  fi
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
    if [ -s "$DIR/install.sh" ]; then              # daily update check (runs as root to restart the service)
      printf '[Unit]\nDescription=solard update check\nWants=network-online.target\nAfter=network-online.target\n[Service]\nType=oneshot\nEnvironment=SOLARD_HOME=%s\nExecStart=/bin/sh %s/install.sh --update\n' "$DIR" "$DIR" | sudo_ tee /etc/systemd/system/solard-update.service >/dev/null
      printf '[Unit]\nDescription=Daily solard update check\n[Timer]\nOnCalendar=daily\nRandomizedDelaySec=4h\nPersistent=true\n[Install]\nWantedBy=timers.target\n' | sudo_ tee /etc/systemd/system/solard-update.timer >/dev/null
      sudo_ systemctl daemon-reload && sudo_ systemctl enable --now solard-update.timer >/dev/null 2>&1
    fi
  else
    mkdir -p "$HOME/.config/systemd/user"
    printf '%s\n[Install]\nWantedBy=default.target\n' "$UNIT" > "$HOME/.config/systemd/user/solard.service"
    systemctl --user daemon-reload && systemctl --user enable --now solard >/dev/null 2>&1
    if [ -s "$DIR/install.sh" ]; then
      printf '[Unit]\nDescription=solard update check\n[Service]\nType=oneshot\nEnvironment=SOLARD_HOME=%s\nExecStart=/bin/sh %s/install.sh --update\n' "$DIR" "$DIR" > "$HOME/.config/systemd/user/solard-update.service"
      printf '[Unit]\nDescription=Daily solard update check\n[Timer]\nOnCalendar=daily\nRandomizedDelaySec=4h\nPersistent=true\n[Install]\nWantedBy=timers.target\n' > "$HOME/.config/systemd/user/solard-update.timer"
      systemctl --user daemon-reload && systemctl --user enable --now solard-update.timer >/dev/null 2>&1
    fi
    loginctl enable-linger "$(id -un)" 2>/dev/null || true
  fi
  IP=$(hostname -I 2>/dev/null | awk '{print $1}')
  WHEN="at every boot"
else
  nohup "$DIR/solard" --config "$CONF" >> "$DIR/solard.log" 2>&1 &
  IP=$(hostname -I 2>/dev/null | awk '{print $1}')
  WHEN="until this machine restarts (no systemd found: add it to your startup yourself)"
fi

UP=""
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do     # give the service a moment to start
  if curl -fsS -m 2 "http://127.0.0.1:$PORT/ping" >/dev/null 2>&1; then UP=1; break; fi
  sleep 1
done
if [ -n "$UP" ]; then
  say ""
  say "solard is running, and starts $WHEN."
  say "  Dashboard:      http://${IP:-localhost}:$PORT/"
  say "  Phone / TV app: open QuickSolar -> it finds this server automatically."
  say "  Settings:       $CONF      History: $DIR/data"
  if [ -s "$DIR/install.sh" ] && ! grep -qiE '^[[:space:]]*auto_update[[:space:]]*=[[:space:]]*(false|0|no|off)' "$CONF"; then
    say "  Updates:        checked daily (auto_update = false in the settings to turn off)"
  fi
  if [ "$OS" = Darwin ]; then say "  Keep this Mac awake (System Settings -> Energy) so it records all day."; fi
else
  say "solard was installed but isn't answering yet -- see $DIR/solard.log"
fi
exit 0
