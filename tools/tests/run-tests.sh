#!/bin/sh
# Cada passo reporta o que falhou, em vez de a cadeia && engolir o erro em
# silencio: foi assim que o test_fp desapareceu da lista sem ninguem reparar.
cd <suyu-v0.0.4> || exit 1
S=<scratch>
fail=0
for t in adc bitfields extract fp simd crc32 recip aes sha1 sha256 satsimd chain; do
  f=tools/aot/test_$t.cpp
  if ! g++ -std=c++20 -O1 -I src "$f" -o "$S/s_$t" 2>"$S/se_$t"; then
    echo "$t: FALHA a compilar o gerador"; head -2 "$S/se_$t"; fail=1; continue; fi
  if ! "$S/s_$t" > "$S/s_$t.c" 2>"$S/se_$t"; then
    echo "$t: FALHA a correr o gerador"; head -2 "$S/se_$t"; fail=1; continue; fi
  if ! gcc -std=c11 -O2 -fsanitize=undefined -fsanitize-trap=all "$S/s_$t.c" -o "$S/sr_$t" -lcrypto -lm 2>"$S/se_$t"; then
    echo "$t: FALHA a compilar o teste"; head -2 "$S/se_$t"; fail=1; continue; fi
  if out=$(timeout 300 "$S/sr_$t" 2>&1); then echo "$t: ok ($out)"; else
    echo "$t: FALHA ($out)"; fail=1; fi
done
if g++ -std=c++20 -O1 -I src tools/aot/test_exclusive.cpp -o "$S/s_ex" 2>/dev/null; then
  if out=$("$S/s_ex" 2>&1); then echo "exclusive: ok ($out)"; else echo "exclusive: FALHA"; fail=1; fi
else echo "exclusive: FALHA a compilar"; fail=1; fi
exit $fail
