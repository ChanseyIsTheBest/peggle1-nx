#!/bin/bash
# Peggle 1's NRO in banjo-nx's Ryujinx fork, with the Peggle port's own data folder (D:\New folder\ryu-data\peggle2,
# -r; its SD folder switch/peggle1-nx is a junction to out/emu_sd/peggle1-nx: game pack, NRO, toml, dlc/). Runs
# <seconds>, stops only this instance (its process id), and collects the NRO's log and shots into
# out/renut_tests/emu/<name>. Other ports' instances may run at the same time (timings are then not meaningful).
#
# Usage: tools/win/p1_emu_run.sh <name> <seconds> [key = value ...]
#   The keys go into the SD's peggle1.toml for this run, on top of EMU_BASE (default tools/win/p1_emu_base.toml).
set -u
NAME=$1; SECS=$2; shift 2
if [ "$SECS" -gt 590 ]; then SECS=590; fi
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SD="$ROOT/out/emu_sd/peggle1-nx"
OUT="$ROOT/out/renut_tests/emu"; mkdir -p "$OUT/$NAME"
# EMU_BASE=none: no toml is written (a first start as the player has it: the NRO writes its own).
if [ "${EMU_BASE:-}" != none ]; then
python -P - "${EMU_BASE:-$ROOT/tools/win/p1_emu_base.toml}" "$SD/peggle1.toml" "$@" <<'EOF'
import re, sys
text = open(sys.argv[1], encoding='utf-8').read()
for kv in sys.argv[3:]:
    k, _, v = kv.partition('=')
    k, v = k.strip(), v.strip()
    pat = re.compile(r'^%s\s*=.*$' % re.escape(k), re.M)
    line = '%s = %s' % (k, v)
    text = pat.sub(line, text) if pat.search(text) else text.rstrip('\n') + '\n' + line + '\n'
open(sys.argv[2], 'w', encoding='utf-8', newline='\n').write(text)
EOF
fi
rm -rf "$SD/shots"
rm -f "$SD"/logs/*.log 2>/dev/null
WOUT=$(cygpath -w "$OUT/$NAME")
powershell.exe -NoProfile -Command "\$env:DOTNET_ROOT = 'D:\New folder\dotnet10'; \$env:RYUJINX_HOMEBREW_SHADER_CACHE_ID = '0500000050454747'; \$nro = 'D:\New folder\ryu-data\peggle2\sdcard\switch\peggle1-nx\peggle1.nro'; \$p = Start-Process -FilePath 'D:\New folder\Ryubing-src\ryubing\build\Ryujinx.exe' -ArgumentList @('-r', '\"D:\New folder\ryu-data\peggle2\"', ('\"' + \$nro + '\"')) -WindowStyle Minimized -PassThru -RedirectStandardError '$WOUT\\ryu_stderr.txt' -RedirectStandardOutput '$WOUT\\ryu_stdout.txt'; 'pid ' + \$p.Id; try { Wait-Process -Id \$p.Id -Timeout $SECS -ErrorAction Stop; 'exited by itself' } catch { 'stopped after $SECS s' }; Stop-Process -Id \$p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep 2"
[ -d "$SD/shots" ] && cp -r "$SD/shots" "$OUT/$NAME/"
for d in "$SD/logs" "$SD"; do
  L=$(ls -t "$d"/*.log 2>/dev/null | head -1)
  [ -n "$L" ] && cp "$L" "$OUT/$NAME/" && break
done
for f in rex_stderr.log rex_crash.log; do [ -f "$SD/$f" ] && cp "$SD/$f" "$OUT/$NAME/"; done
RL=$(ls -t "/d/New folder/ryu-data/peggle2/Logs"/*.log 2>/dev/null | head -1)
[ -n "$RL" ] && cp "$RL" "$OUT/$NAME/ryujinx.log"
[ -f "$SD/peggle1.toml" ] && cp "$SD/peggle1.toml" "$OUT/$NAME/peggle1.toml"
echo "emu run $NAME: $(ls "$OUT/$NAME" | tr '\n' ' ')"
