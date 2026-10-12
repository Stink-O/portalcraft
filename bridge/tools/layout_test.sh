#!/bin/bash
# Layout test: protocol/skycraft_protocol.h (compiled -m32, as the Portal 2 plugin is) against
# the Fabric mod's Proto.java. Every constant the C++ side lists must exist in Java with the same value.
set -eu
cd "$(dirname "$0")/layout"
OUT=$(mktemp -d)
g++ -std=c++17 -m32 -o "$OUT/dump32" layout_dump.cpp
g++ -std=c++17 -o "$OUT/dump64" layout_dump.cpp
PROTO=../../fabric/src/main/java/dev/skycraft/link/Proto.java
javac -d "$OUT" "$PROTO" ProtoDump.java
"$OUT/dump32" | sort > "$OUT/cpp32.txt"
"$OUT/dump64" | sort > "$OUT/cpp64.txt"
java -cp "$OUT" ProtoDump | sort > "$OUT/java.txt"
fail=0
diff -q "$OUT/cpp32.txt" "$OUT/cpp64.txt" >/dev/null || { echo "FAIL: -m32 and -m64 layouts differ:"; diff "$OUT/cpp32.txt" "$OUT/cpp64.txt"; fail=1; }
while read -r name val; do
  jv=$(awk -v n="$name" '$1==n {print $2}' "$OUT/java.txt")
  if [ -z "$jv" ]; then echo "FAIL: $name missing in Proto.java"; fail=1
  elif [ "$jv" != "$val" ]; then echo "FAIL: $name C++ $val Java $jv"; fail=1; fi
done < "$OUT/cpp32.txt"
n=$(wc -l < "$OUT/cpp32.txt")
rm -rf "$OUT"
[ $fail = 0 ] && echo "layout test PASS ($n constants, -m32 == -m64 == Proto.java)"
exit $fail
