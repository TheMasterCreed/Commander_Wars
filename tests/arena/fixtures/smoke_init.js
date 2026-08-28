var ARENA_SMOKE_LOG_LEVEL_ERROR = 3;
if (typeof arenaTest === "undefined")
{
    GameConsole.print("ARENA_SUPPORT_SMOKE:missing-global", ARENA_SMOKE_LOG_LEVEL_ERROR);
}
else
{
    arenaTest.finish(0);
}
