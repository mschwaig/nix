---
synopsis: New experimental feature `store-path-seeding`
---

A new experimental feature, `store-path-seeding`, adds a `store-path-seed`
setting. When set, the seed is mixed into the calculation of every store path,
deterministically shifting the whole dependency graph away from its canonical
(unseeded) locations.

For every locally built output, Nix records the *unseeded equivalent* — the
store path, NAR hash and references the output would have had without a seed —
in the local store database. This is exposed to the `post-build-hook` via the
`UNSEEDED_DRV_PATH` and `UNSEEDED_OUT_PATHS` environment variables and via
`nix path-info --json`, so that external tooling can make (and sign)
statements about the canonical store paths.

Building the same derivation under two different seeds and comparing the
recorded unseeded NAR hashes proves both that the build is bit-reproducible
*and* that its outputs contain no store path references that Nix's reference
scanner cannot detect (for example compressed or otherwise obfuscated paths).
This closes the gap where a copied runtime closure can silently miss a
dependency. Nix itself only warns when recordings disagree; verification
policy is left to external tools.
