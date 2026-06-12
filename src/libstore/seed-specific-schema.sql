-- Extension of the sql schema for store path seeding.
-- Won't be loaded unless the experimental feature `store-path-seeding`
-- is enabled.

-- Records the unseeded (canonical) equivalent of store objects created
-- while a `store-path-seed` was set. For built outputs, the NAR
-- hash/size and references the object would have had without a seed
-- are recorded as well; for derivations, the unseeded derivation hash
-- is recorded (memoising the recursive computation).
--
-- Rows deliberately do not reference the ValidPaths table: the
-- unseeded paths they mention generally do not exist in this store,
-- and the recorded facts remain meaningful after the seeded paths have
-- been garbage-collected.

create table if not exists UnseededPathsV1 (
    seededPath       text primary key not null,
    unseededPath     text not null,
    unseededNarHash  text,    -- outputs only
    unseededNarSize  integer, -- outputs only
    unseededRefs     text,    -- outputs only; space-separated unseeded store paths
    unseededDrvHash  text     -- derivations only; serialized DrvHashModulo (JSON)
);

create index if not exists IndexUnseededPathsV1 on UnseededPathsV1(unseededPath);
