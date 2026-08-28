arenaTest.armWatchdog("AI choice", 120000);
var result = arenaTest.runOperation("aiChoice", null, {});
if (result.ok)
{
    arenaTest.finish(0);
}
else
{
    GameConsole.print("ARENA_AI_CHOICE:" + result.failures.join(","), 3);
    arenaTest.finish(4);
}
