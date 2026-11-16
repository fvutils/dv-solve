#!/bin/sh
# DVS_CAPTURE round trip of dvs_randomize_parts* calls: test_parts is
# captured (its parts calls have active, inactive and null slots, slot arrays
# with null elements, cycles and the sizes kind) and replayed with no
# mismatch; then each root that has calls is extracted, and its extract
# replays its calls with no mismatch.
#
#   test_capture_parts.sh TEST_PARTS DVS_CAPTURE_REPLAY
set -e
test_parts=$1
replay=$2
rm -f parts.dvscap root.dvscap
DVS_CAPTURE=parts.dvscap "$test_parts" > /dev/null
"$replay" --passes 2 parts.dvscap > parts.out
grep -Eq '^capture .* calls \([1-9][0-9]* parts calls\)$' parts.out
roots=$(sed -n 's/^handle \([0-9]*\): [1-9][0-9]* calls.*/\1/p' parts.out)
test -n "$roots"
for id in $roots; do
    "$replay" --extract "$id" -o root.dvscap parts.dvscap > /dev/null
    "$replay" --passes 1 root.dvscap > root.out
    # The same calls on the root, and none on any other handle
    want=$(grep -o "^handle $id: [0-9]* calls" parts.out)
    test "$(grep -o "^handle [0-9]*: [1-9][0-9]* calls" root.out)" = "$want"
done
echo "PASS capture round trip of parts calls, roots extracted: $(echo $roots | wc -w)"
