arenaTest.armWatchdog("coordinated action construction", 120000);
var construction = arenaTest.runOperation("coordinatedActionConstruction", null, {});
if (!construction.ok) {
    GameConsole.print("ARENA_ACTION_CONSTRUCTION:" + construction.failures.join(","), 3);
}
arenaTest.finish(construction.ok ? 0 : 4);
