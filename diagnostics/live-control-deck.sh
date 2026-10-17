#!/usr/bin/env bash
# SDH OptiScaler Live Control - non-destructive diagnostic for Steam Deck.
set -u
GAME="$HOME/Games/Resonance.A.Plague.Tale.Legacy-InsaneRamZes"
SHARED="$HOME/upscalers/sd-helper-optiscaler"
STATE="$HOME/.config/decky-loader/sdh-framegen-optiscaler-profiles/2621163129-7d05541e/sdh-control.state"
ASI="$GAME/OptiScaler/plugins/sdh-optiscaler-control.asi"
EXPECTED_DXGI="ec3bc2c701e1b13dc6447109cede0d4fafebd71e0f60db3431e841f297e63ece"
EXPECTED_ASI="d55892085c92ed1da186999df90bff4cde01d845c5b1f663af0eb1c5232e1b44"
printf '%s\n' '=== SDH LIVE DIAGNOSTIC (READ ONLY) ==='
for file in "$SHARED/dxgi.dll" "$GAME/dxgi.dll" "$ASI" "$SHARED/OptiScaler/plugins/sdh-optiscaler-control.asi"; do
  printf '\nFile: %s\n' "$file"
  if [ -L "$file" ]; then printf 'symlink: '; readlink "$file"; fi
  if [ -f "$file" ]; then sha256sum "$file"; file "$file" 2>/dev/null || true; else echo MISSING; fi
done
echo
printf 'EXPECTED NEW DXGI: %s\n' "$EXPECTED_DXGI"
printf 'EXPECTED DEV7 ASI: %s\n' "$EXPECTED_ASI"
printf '\n=== RESONANCE PROCESS ===\n'
pgrep -af 'Resonance.exe' | head -12 || true
printf '\n=== STATE (NOW) ===\n'
if [ -f "$STATE" ]; then cat "$STATE" | head -100; stat -c 'State mtime: %y' "$STATE"; else echo MISSING; fi
printf '\n=== STATE AFTER TWO SECONDS ===\n'
sleep 2
if [ -f "$STATE" ]; then grep -E '^(protocol|core_tick|control_ready|apply_mode|path_source|error|helper_loaded|init_called)=' "$STATE" || true; else echo MISSING; fi
printf '\n=== OPTISCALER INI CONFIG ===\n'
if [ -f "$GAME/OptiScaler.ini" ]; then
  awk '/^\[Plugins\]/{section="Plugins"} /^\[Log\]/{section="Log"} /^\[/{if ($0!="[Plugins]"&&$0!="[Log]") section=""} (section!="" && /^(Path|LoadAsiPlugins|LogToFile|LogLevel)[[:space:]]*=/){print section": "$0}' "$GAME/OptiScaler.ini"
else echo MISSING; fi
printf '\n=== OPTISCALER LOG SOURCES ===\n'
if [ -d "$GAME" ]; then
  find "$GAME" -maxdepth 4 -type f \( -iname '*optiscaler*.log' -o -iname 'log.txt' -o -iname '*optiscaler*log*' \) -print | head -12 | while IFS= read -r log; do
    printf '\nLOG: %s\n' "$log"
    stat -c 'Log mtime: %y' "$log" 2>/dev/null || true
    grep -Ei 'initializ.*plugin api|sdh-optiscaler|loaded.*asi|optiplugininit|asi plugin|checking.*asi|crashed.*plugin|plugin api' "$log" | tail -65 || true
  done
fi
printf '\n=== END; diagnostic makes no changes ===\n'
