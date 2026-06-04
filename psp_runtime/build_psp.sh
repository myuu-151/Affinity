#!/bin/bash
# Affinity PSP build helper — run via: wsl -d Ubuntu -- bash <this>
export PSPDEV=/opt/pspdev
export PATH="$PSPDEV/bin:$PATH"
cd "$(dirname "$0")" || exit 9
{
  echo "PSPGCC=$(command -v psp-gcc || echo MISSING)"
  psp-gcc --version 2>&1 | head -1
  echo "----- make clean -----"
  make clean
  echo "----- make -----"
  make
  echo "EXIT=$?"
} 2>&1 | tee make_log.txt
