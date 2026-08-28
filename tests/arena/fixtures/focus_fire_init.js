arenaTest.armWatchdog("focus fire exposure", 300000);
var focusResult = arenaTest.runOperation("focusFireExposure", null, {mapPath: "maps/2_player/River of War.map"});
if (!focusResult.ok) {
    GameConsole.print("ARENA_FOCUS_FIRE:" + focusResult.failures.join(","), 3);
}
arenaTest.finish(focusResult.ok ? 0 : 4);
