arenaTest.armWatchdog("coordinated pause gate", 300000);
var coordinatedPause = arenaTest.runOperation(
    "coordinatedIntegrationProbe",
    null,
    {
        mapPath: "maps/2_player/Plug Mountain.map",
        controller: "Coordinated",
        pause: true
    });
if (!coordinatedPause.ok)
{
    GameConsole.print(
        "ARENA_COORDINATED_PAUSE_GATE:" +
            coordinatedPause.failures.join(","),
        3);
}
arenaTest.finish(coordinatedPause.ok ? 0 : 4);
