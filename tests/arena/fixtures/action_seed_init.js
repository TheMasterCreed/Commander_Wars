arenaTest.armWatchdog("action seed probe", 120000);
var actionSeedResult = arenaTest.runOperation("actionSeedDeterminism", null, {});
if (actionSeedResult.ok)
{
    arenaTest.finish(0);
}
else
{
    GameConsole.print("ARENA_ACTION_SEED:" + actionSeedResult.failures.join(","), 3);
    arenaTest.finish(4);
}
