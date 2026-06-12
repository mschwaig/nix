#pragma once
///@file

#include "nix/store/derivations.hh"
#include "nix/store/path.hh"
#include "nix/util/hash.hh"

namespace nix {

class LocalStore;

/**
 * The unseeded (canonical) equivalent of a store object that was
 * created while a `store-path-seed` was set (experimental feature
 * `store-path-seeding`).
 *
 * For built outputs, this additionally records the NAR hash/size and
 * references the object would have had if it had been built without a
 * seed, i.e. after rewriting every (seeded) store path reference in
 * its contents to its unseeded equivalent. Comparing these across
 * builds with different seeds proves both reproducibility and the
 * absence of references invisible to the reference scanner.
 *
 * For derivations, the unseeded derivation hash is recorded instead,
 * memoising the recursive hash-modulo computation.
 */
struct UnseededPathInfo
{
    StorePath unseededPath;

    /**
     * Outputs only: NAR hash of the contents with all references
     * rewritten to their unseeded equivalents.
     */
    std::optional<Hash> narHash;

    /**
     * Outputs only: size of the rewritten NAR (equal to the seeded
     * object's NAR size, since rewriting preserves length).
     */
    std::optional<uint64_t> narSize;

    /**
     * Outputs only: the references, as unseeded store paths.
     */
    std::optional<StorePathSet> references;

    /**
     * Derivations only: the unseeded `DrvHashModulo`.
     */
    std::optional<DrvHashModulo> drvHash;
};

/**
 * Computes unseeded equivalents of seeded store objects.
 *
 * Resolution strategy for a single path:
 *
 * 1. the `UnseededPathsV1` table (covers everything previously
 *    recorded, in particular all locally built outputs — since
 *    dependencies are built before dependents, this is the common
 *    case);
 * 2. recomputation from the object's content address for sources and
 *    fixed-output derivation outputs (their contents are unchanged by
 *    seeding, only the path shifts);
 * 3. recursion over the (seeded) derivation graph for input-addressed
 *    outputs and derivations themselves.
 *
 * Results are memoised in memory; derivation rows are persisted to the
 * database as a side effect so other processes need not recompute
 * them.
 */
class Unseeder
{
    LocalStore & store;

    std::map<StorePath, StorePath> pathMemo;

    std::map<StorePath, DrvHashModulo> drvHashMemo;

public:

    Unseeder(LocalStore & store)
        : store(store)
    {
    }

    /**
     * Return the unseeded equivalent of `path`.
     *
     * @throws Error if no unseeded equivalent can be determined (for
     * example for a path that was imported without provenance, or an
     * output of an impure or dynamic derivation).
     */
    StorePath unseedPath(const StorePath & path);

    /**
     * Return the unseeded `DrvHashModulo` of the derivation stored at
     * (seeded) `drvPath`, additionally yielding the unseeded
     * derivation path.
     */
    std::pair<StorePath, DrvHashModulo> unseedDrv(const StorePath & drvPath);
};

/**
 * Serialization of `DrvHashModulo` for the `UnseededPathsV1` table.
 * `DeferredDrv` is not serializable: deferred derivations have no
 * unseeded hash to record.
 */
std::string renderDrvHashModulo(const DrvHashModulo & drvHash);
DrvHashModulo parseDrvHashModulo(std::string_view s);

} // namespace nix
