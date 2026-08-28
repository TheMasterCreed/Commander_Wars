arenaTest.armWatchdog("capture bonus probe", 120000);
var captureBonusResult = arenaTest.runOperation("captureBonusPurity", null, {});
if (captureBonusResult.ok)
{
    arenaTest.finish(0);
}
else
{
    GameConsole.print("ARENA_CAPTURE_BONUS:" + captureBonusResult.failures.join(","), 3);
    arenaTest.finish(4);
}
