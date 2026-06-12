#include <gtest/gtest.h>

#include "nix/store/globals.hh"
#include "nix/store/local-store.hh"
#include "nix/store/unseed.hh"
#include "nix/util/finally.hh"

namespace nix {

namespace {

LocalStoreConfig testConfig()
{
    std::filesystem::path storeDir =
#ifdef _WIN32
        "C:\\";
#else
        "/";
#endif
    storeDir /= "nix";
    storeDir /= "store";
    return LocalStoreConfig{"", {{"store", storeDir.string()}}};
}

const Hash someHash = hashString(HashAlgorithm::SHA256, "some contents");

} // namespace

TEST(StorePathSeeding, emptySeedIsUnseeded)
{
    auto config = testConfig();
    EXPECT_EQ(settings.storePathSeed.get(), "");
    EXPECT_EQ(
        config.makeStorePath("source", someHash, "foo"),
        config.makeStorePath("source", someHash, "foo", SeedPolicy::Unseeded));
}

TEST(StorePathSeeding, seedShiftsPath)
{
    auto config = testConfig();
    auto unseeded = config.makeStorePath("source", someHash, "foo");

    settings.storePathSeed = "seed1";
    Finally restoreSeed([&]() { settings.storePathSeed = ""; });

    auto seeded1 = config.makeStorePath("source", someHash, "foo");
    EXPECT_NE(unseeded, seeded1);
    EXPECT_EQ(unseeded.name(), seeded1.name());

    settings.storePathSeed = "seed2";
    auto seeded2 = config.makeStorePath("source", someHash, "foo");
    EXPECT_NE(seeded1, seeded2);

    /* Deterministic per seed. */
    settings.storePathSeed = "seed1";
    EXPECT_EQ(seeded1, config.makeStorePath("source", someHash, "foo"));

    /* The unseeded policy ignores the configured seed. */
    EXPECT_EQ(unseeded, config.makeStorePath("source", someHash, "foo", SeedPolicy::Unseeded));
}

TEST(StorePathSeeding, goldenUnseededPath)
{
    /* Regression guard: without a seed, path calculation must be
       byte-for-byte identical to previous versions of Nix. */
    auto config = testConfig();
    auto path = config.makeStorePath(
        "source", Hash::parseAny("1b8m03r63zqhnjf7l5wnldhh7c134ap5vpj0850ymkq1iyzicy5s", HashAlgorithm::SHA256), "foo");
#ifndef _WIN32
    EXPECT_EQ(config.printStorePath(path), "/nix/store/8nz4xrxpk6mgia8bb9jxncpcikkm2xxv-foo");
#endif
}

TEST(StorePathSeeding, drvHashModuloRoundtrip)
{
    DrvHashModulo plain{DrvHashModulo::DrvHash{someHash}};
    EXPECT_EQ(parseDrvHashModulo(renderDrvHashModulo(plain)), plain);

    DrvHashModulo ca{DrvHashModulo::CaOutputHashes{
        {"out", someHash},
        {"dev", hashString(HashAlgorithm::SHA256, "other contents")},
    }};
    EXPECT_EQ(parseDrvHashModulo(renderDrvHashModulo(ca)), ca);

    DrvHashModulo deferred{DrvHashModulo::DeferredDrv{}};
    EXPECT_THROW(renderDrvHashModulo(deferred), Error);
}

} // namespace nix
