#!/usr/bin/env bash

source common.sh

TODO_NixOS # requires editing nix.conf and restarting the daemon

clearStore

enableFeatures "store-path-seeding"

setSeed() {
    sed -i '/^store-path-seed =/d' "$test_nix_conf"
    if [ -n "$1" ]; then
        echo "store-path-seed = $1" >> "$test_nix_conf"
    fi
    restartDaemon
}

restoreSeed() {
    setSeed ""
}
trap restoreSeed EXIT

# Build the canonical (unseeded) graph first, for later comparison.
setSeed ""
canonicalTop=$(nix-build dependencies.nix --no-out-link)
canonicalTopNarHash=$(nix path-info --json --json-format 2 "$canonicalTop" | jq -r ".info.\"$(basename "$canonicalTop")\".narHash")

# Building with a seed shifts every store path...
setSeed "seed-a"
seededTopA=$(nix-build dependencies.nix --no-out-link)
[[ "$seededTopA" != "$canonicalTop" ]]
[[ "$(basename "$seededTopA" | cut -c34-)" = "$(basename "$canonicalTop" | cut -c34-)" ]]

# ...and records the unseeded equivalent of each output: the canonical
# path and the NAR hash the output would have had without a seed, which
# must match the actual unseeded build.
unseededInfo=$(nix path-info --json --json-format 2 "$seededTopA" | jq ".info.\"$(basename "$seededTopA")\".unseeded")
[[ "$(echo "$unseededInfo" | jq -r .path)" = "$(basename "$canonicalTop")" ]]
[[ "$(echo "$unseededInfo" | jq -r .narHash)" = "$canonicalTopNarHash" ]]

# References are recorded under their unseeded names.
echo "$unseededInfo" | jq -r '.references[]' | grepQuiet "dependencies-input-2"

# A second seed produces a third copy of the graph, whose recorded
# unseeded NAR hashes must agree with the first seed's (proving
# reproducibility and the absence of hidden references).
setSeed "seed-b"
seededTopB=$(nix-build dependencies.nix --no-out-link 2> "$TEST_ROOT/seed-b.log")
[[ "$seededTopB" != "$seededTopA" ]]
[[ "$seededTopB" != "$canonicalTop" ]]
grepQuietInverse "differ" "$TEST_ROOT/seed-b.log"
unseededInfoB=$(nix path-info --json --json-format 2 "$seededTopB" | jq ".info.\"$(basename "$seededTopB")\".unseeded")
[[ "$(echo "$unseededInfoB" | jq -r .narHash)" = "$canonicalTopNarHash" ]]

if [ -n "$(type -p sqlite3)" ]; then
    # Both seeded builds of the top-level derivation map to the same
    # unseeded path.
    count=$(sqlite3 "$NIX_STATE_DIR/db/db.sqlite" "select count(distinct seededPath) from UnseededPathsV1 where unseededPath = '$(basename "$canonicalTop")'")
    [[ "$count" = 2 ]]
fi

# The post-build hook is told about the unseeded equivalents.
hookLog="$TEST_ROOT/unseeded-hook.log"
cat > "$TEST_ROOT/unseeded-hook.sh" <<EOF
#!/bin/sh
echo "drv: \$DRV_PATH" >> "$hookLog"
echo "unseeded drv: \$UNSEEDED_DRV_PATH" >> "$hookLog"
echo "outs: \$OUT_PATHS" >> "$hookLog"
echo "unseeded outs: \$UNSEEDED_OUT_PATHS" >> "$hookLog"
EOF
chmod +x "$TEST_ROOT/unseeded-hook.sh"

setSeed "seed-c"
nix-build dependencies.nix --no-out-link --post-build-hook "$TEST_ROOT/unseeded-hook.sh" > /dev/null
grepQuiet "unseeded outs: $canonicalTop" "$hookLog"
# The unseeded drv path must be the same one an unseeded evaluation produces.
setSeed ""
canonicalDrv=$(nix-instantiate dependencies.nix)
grepQuiet "unseeded drv: $canonicalDrv" "$hookLog"

# Negative test: a derivation that hides a self-reference from the
# reference scanner. The recorded unseeded NAR hashes differ between
# seeds, which must be reported.
cat > "$TEST_ROOT/hidden-ref.nix" <<EOF
with import ${PWD}/config.nix;
mkDerivation {
  name = "hidden-self-ref";
  buildCommand = ''
    mkdir -p \$out
    echo \$out | tr 'a-z' 'A-Z' > \$out/hidden
  '';
}
EOF

setSeed "seed-a"
nix-build "$TEST_ROOT/hidden-ref.nix" --no-out-link
setSeed "seed-b"
nix-build "$TEST_ROOT/hidden-ref.nix" --no-out-link 2> "$TEST_ROOT/hidden-ref.log"
grepQuiet "not reproducible or its output contains a hidden store path reference" "$TEST_ROOT/hidden-ref.log"
