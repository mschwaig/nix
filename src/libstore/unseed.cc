#include "nix/store/unseed.hh"

#include <nlohmann/json.hpp>

#include "nix/store/derivations.hh"
#include "nix/store/local-store.hh"
#include "nix/store/references.hh"
#include "nix/util/util.hh"

namespace nix {

std::string renderDrvHashModulo(const DrvHashModulo & drvHash)
{
    return std::visit(
               overloaded{
                   [](const DrvHashModulo::DrvHash & h) -> nlohmann::json {
                       return nlohmann::json{{"drvHash", h.to_string(HashFormat::Base16, true)}};
                   },
                   [](const DrvHashModulo::CaOutputHashes & hashes) -> nlohmann::json {
                       auto obj = nlohmann::json::object();
                       for (auto & [outputName, h] : hashes)
                           obj[outputName] = h.to_string(HashFormat::Base16, true);
                       return nlohmann::json{{"caOutputHashes", std::move(obj)}};
                   },
                   [](const DrvHashModulo::DeferredDrv &) -> nlohmann::json {
                       throw Error("deferred derivations do not have an unseeded derivation hash");
                   },
               },
               drvHash.raw)
        .dump();
}

DrvHashModulo parseDrvHashModulo(std::string_view s)
{
    auto json = nlohmann::json::parse(s);
    if (auto it = json.find("drvHash"); it != json.end())
        return DrvHashModulo::DrvHash{Hash::parseAnyPrefixed(it->get<std::string>())};
    if (auto it = json.find("caOutputHashes"); it != json.end()) {
        DrvHashModulo::CaOutputHashes hashes;
        for (auto & [outputName, h] : it->items())
            hashes.insert_or_assign(outputName, Hash::parseAnyPrefixed(h.get<std::string>()));
        return hashes;
    }
    throw Error("invalid serialized derivation hash '%s'", s);
}

StorePath Unseeder::unseedPath(const StorePath & path)
{
    if (auto i = pathMemo.find(path); i != pathMemo.end())
        return i->second;

    auto result = [&]() -> StorePath {
        /* Previously recorded (in particular, all locally built
           outputs). */
        if (auto row = store.queryUnseededPath(path))
            return row->unseededPath;

        if (path.isDerivation())
            return unseedDrv(path).first;

        auto info = store.queryPathInfo(path);

        /* Content-addressed objects (sources, fixed-output derivation
           outputs, floating content-addressed outputs): only the path
           is shifted by the seed, so the unseeded path can be
           recomputed from the content address. */
        if (info->ca) {
            StoreReferences refs;
            StringMap rewrites;
            for (auto & r : info->references) {
                if (r == path)
                    refs.self = true;
                else {
                    auto unseededRef = unseedPath(r);
                    rewrites.insert_or_assign(std::string{r.hashPart()}, std::string{unseededRef.hashPart()});
                    refs.others.insert(std::move(unseededRef));
                }
            }

            auto hash = info->ca->hash;
            if (!rewrites.empty()) {
                /* The contents embed seeded references; the unseeded
                   equivalent has them rewritten, so the content hash
                   must be recomputed over the rewritten contents.
                   References are only allowed for NAR/SHA-256 content
                   addressing, where the hash is over the NAR (modulo
                   self-references, whose zeroed occurrences and
                   positions are unchanged by the rewrite). */
                assert(info->ca->method == ContentAddressMethod::Raw::NixArchive);
                HashSink hashSink(info->ca->hash.algo);
                std::optional<HashModuloSink> hashModuloSink;
                if (refs.self)
                    hashModuloSink.emplace(info->ca->hash.algo, std::string{path.hashPart()});
                Sink & inner = refs.self ? static_cast<Sink &>(*hashModuloSink) : hashSink;
                RewritingSink rsink(rewrites, inner);
                store.narFromPath(path, rsink);
                rsink.flush();
                hash = refs.self ? hashModuloSink->finish().hash : hashSink.finish().hash;
            }

            return store.makeFixedOutputPathFromCA(
                path.name(),
                ContentAddressWithReferences::fromParts(info->ca->method, hash, std::move(refs)),
                SeedPolicy::Unseeded);
        }

        /* Input-addressed output: recompute via its deriver, which
           leaves the unseeded equivalents of all of its
           input-addressed outputs in `pathMemo`. */
        if (info->deriver && store.isValidPath(*info->deriver)) {
            unseedDrv(*info->deriver);
            if (auto i = pathMemo.find(path); i != pathMemo.end())
                return i->second;
        }

        throw Error(
            "cannot compute the unseeded equivalent of %s: it is neither recorded, content-addressed, nor produced by a known deriver",
            store.printStorePath(path));
    }();

    pathMemo.insert_or_assign(path, result);
    return result;
}

std::pair<StorePath, DrvHashModulo> Unseeder::unseedDrv(const StorePath & drvPath)
{
    if (auto i = drvHashMemo.find(drvPath); i != drvHashMemo.end())
        return {pathMemo.at(drvPath), i->second};

    auto memoize = [&](const StorePath & unseededDrvPath, const DrvHashModulo & drvHash) {
        pathMemo.insert_or_assign(drvPath, unseededDrvPath);
        drvHashMemo.insert_or_assign(drvPath, drvHash);
        return std::pair{unseededDrvPath, drvHash};
    };

    if (auto row = store.queryUnseededPath(drvPath); row && row->drvHash)
        return memoize(row->unseededPath, *row->drvHash);

    auto drv = store.readInvalidDerivation(drvPath);

    if (drv.type().isImpure())
        throw Error("cannot compute the unseeded equivalent of impure derivation %s", store.printStorePath(drvPath));

    /* Map every input to its unseeded equivalent, building both the
       structural replacements (inputSrcs, inputDrvs) and the textual
       rewrites for the derivation's builder/args/env. */
    StringMap rewrites;
    StorePathSet unseededInputSrcs;
    decltype(drv.inputDrvs.map) unseededInputDrvs;
    std::map<StorePath, DrvHashModulo> unseededInputDrvHashes;

    for (auto & src : drv.inputSrcs) {
        auto unseededSrc = unseedPath(src);
        rewrites.insert_or_assign(store.printStorePath(src), store.printStorePath(unseededSrc));
        unseededInputSrcs.insert(std::move(unseededSrc));
    }

    for (auto & [inputDrvPath, node] : drv.inputDrvs.map) {
        if (!node.childMap.empty())
            throw Error(
                "cannot compute the unseeded equivalent of derivation %s: dynamic derivations are not supported with store path seeding",
                store.printStorePath(drvPath));

        auto [unseededInputDrvPath, inputDrvHash] = unseedDrv(inputDrvPath);
        rewrites.insert_or_assign(store.printStorePath(inputDrvPath), store.printStorePath(unseededInputDrvPath));

        /* References to the input derivation's outputs occur textually
           in the environment/args. */
        auto inputDrv = store.readInvalidDerivation(inputDrvPath);
        for (auto & outputName : node.value) {
            auto * output = get(inputDrv.outputs, outputName);
            if (!output)
                throw Error("derivation %s has no output '%s'", store.printStorePath(inputDrvPath), outputName);
            if (auto outputPath = output->path(store, inputDrv.name, outputName))
                rewrites.insert_or_assign(
                    store.printStorePath(*outputPath), store.printStorePath(unseedPath(*outputPath)));
            /* Outputs without a statically known path (floating CA,
               deferred) contribute placeholders, not paths, so there
               is nothing to rewrite. */
        }

        unseededInputDrvHashes.insert_or_assign(unseededInputDrvPath, inputDrvHash);
        unseededInputDrvs.insert_or_assign(std::move(unseededInputDrvPath), node);
    }

    Derivation unseededDrv{drv};
    unseededDrv.inputSrcs = std::move(unseededInputSrcs);
    unseededDrv.inputDrvs.map = std::move(unseededInputDrvs);

    /* Rewrite the textual occurrences of the inputs *before* hashing:
       the hash-modulo computation masks this derivation's own output
       paths, but not the input paths in the environment/args. */
    unseededDrv.applyRewrites(rewrites);

    auto lookupInputDrvHash = [&](const StorePath & p) -> DrvHashModulo {
        auto * h = get(unseededInputDrvHashes, p);
        assert(h);
        return *h;
    };

    /* The unseeded derivation hash with *masked* outputs determines
       the unseeded output paths of input-addressed outputs. */
    auto maskedDrvHash = hashDerivationModulo(store, unseededDrv, true, lookupInputDrvHash, SeedPolicy::Unseeded);

    /* Fix up the output paths and their occurrences in the
       environment. */
    StringMap outputRewrites;
    for (auto & [outputName, output] : unseededDrv.outputs) {
        if (auto * ia = std::get_if<DerivationOutput::InputAddressed>(&output.raw)) {
            auto * h = std::get_if<DrvHashModulo::DrvHash>(&maskedDrvHash.raw);
            if (!h)
                throw Error(
                    "cannot compute the unseeded equivalent of derivation %s: it has input-addressed outputs but no input-addressed derivation hash",
                    store.printStorePath(drvPath));
            auto seededOutputPath = ia->path;
            auto unseededOutputPath = store.makeOutputPath(outputName, *h, unseededDrv.name, SeedPolicy::Unseeded);
            outputRewrites.insert_or_assign(
                store.printStorePath(seededOutputPath), store.printStorePath(unseededOutputPath));
            /* Make the outputs of this derivation resolvable by
               `unseedPath` even before they have rows of their own. */
            pathMemo.insert_or_assign(std::move(seededOutputPath), unseededOutputPath);
            ia->path = std::move(unseededOutputPath);
        } else if (auto * dof = std::get_if<DerivationOutput::CAFixed>(&output.raw)) {
            /* Not stored structurally, but the (seeded) output path
               occurs textually in the environment. */
            outputRewrites.insert_or_assign(
                store.printStorePath(dof->path(store, unseededDrv.name, outputName)),
                store.printStorePath(dof->path(store, unseededDrv.name, outputName, SeedPolicy::Unseeded)));
        }
    }

    unseededDrv.applyRewrites(outputRewrites);

    auto unseededDrvPath = computeStorePath(store, unseededDrv, SeedPolicy::Unseeded);

    /* The hash that *referrers* of this derivation must substitute for
       it is computed with unmasked outputs (the unseeded equivalent of
       what `pathDerivationModulo` returns). */
    auto drvHash = hashDerivationModulo(store, unseededDrv, false, lookupInputDrvHash, SeedPolicy::Unseeded);

    /* Persist, so that other processes (and future builds) need not
       recompute this, and so that the unseeded derivation path can be
       queried. Deferred hashes cannot be serialized; the path mapping
       alone is still worth recording. */
    store.upsertUnseededPath(
        drvPath,
        UnseededPathInfo{
            .unseededPath = unseededDrvPath,
            .drvHash = std::get_if<DrvHashModulo::DeferredDrv>(&drvHash.raw) ? std::nullopt : std::optional{drvHash},
        });

    return memoize(unseededDrvPath, drvHash);
}

} // namespace nix
