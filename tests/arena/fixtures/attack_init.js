arenaTest.armWatchdog("attack opportunity differential", 300000);
var attackResult = arenaTest.runOperation("attackOpportunityDifferential", null, {mapPath: "maps/2_player/River of War.map"});
if (!attackResult.ok) {
    GameConsole.print("ARENA_ATTACK:" + attackResult.failures.join(","), 3);
}
arenaTest.finish(attackResult.ok ? 0 : 4);
