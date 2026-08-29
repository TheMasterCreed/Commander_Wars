arenaTest.armWatchdog("coordinated integration", 300000);
var coordinatedIntegration = arenaTest.runOperation(
    "coordinatedIntegrationProbe",
    null,
    {
        mapPath: "maps/2_player/Plug Mountain.map",
        controller: "Coordinated",
        pause: false
    });
if (!coordinatedIntegration.ok)
{
    GameConsole.print(
        "ARENA_COORDINATED_INTEGRATION:" +
            coordinatedIntegration.failures.join(","),
        3);
}
arenaTest.finish(coordinatedIntegration.ok ? 0 : 4);
