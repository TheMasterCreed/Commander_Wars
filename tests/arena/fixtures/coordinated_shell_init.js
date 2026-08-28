arenaTest.armWatchdog("coordinated shell", 120000);
var coordinatedShellResult = arenaTest.runOperation(
    "@ARENA_COORDINATED_SHELL_OPERATION@", null, {});
if (coordinatedShellResult.ok)
{
    arenaTest.finish(0);
}
else
{
    GameConsole.print(
        "ARENA_COORDINATED_SHELL:" + coordinatedShellResult.failures.join(","), 3);
    arenaTest.finish(4);
}
