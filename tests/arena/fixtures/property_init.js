arenaTest.armWatchdog("property economics", 120000);
var propertyResult = arenaTest.runOperation("propertyEconomicsFacts", null, {mapPath: "maps/2_player/Plug Mountain.map"});
if (!propertyResult.ok) {
    GameConsole.print("ARENA_PROPERTY:" + propertyResult.failures.join(","), 3);
}
arenaTest.finish(propertyResult.ok ? 0 : 4);
