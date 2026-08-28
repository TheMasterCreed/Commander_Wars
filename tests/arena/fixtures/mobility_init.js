arenaTest.armWatchdog("mobility field differential", 300000);
var ARENA_MOBILITY_MAPS = ["maps/2_player/Plug Mountain.map", "maps/2_player/Bean Island.map"];
var arenaMobilityFailures = [];
for (var i = 0; i < ARENA_MOBILITY_MAPS.length; ++i)
{
    var arenaMobilityResult = arenaTest.runOperation(
        "mobilityFieldDifferential", null, {mapPath: ARENA_MOBILITY_MAPS[i]});
    if (!arenaMobilityResult.ok)
    {
        arenaMobilityFailures = arenaMobilityFailures.concat(arenaMobilityResult.failures);
    }
}
if (arenaMobilityFailures.length === 0)
{
    arenaTest.finish(0);
}
else
{
    GameConsole.print("ARENA_MOBILITY:" + arenaMobilityFailures.join(","), 3);
    arenaTest.finish(4);
}
