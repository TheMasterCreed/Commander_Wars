arenaTest.armWatchdog("battlefield knowledge facts", 120000);
var knowledgeResult = arenaTest.runOperation("battlefieldKnowledgeFacts", null, {mapPath: "maps/2_player/Plug Mountain.map"});
if (!knowledgeResult.ok) {
    GameConsole.print("ARENA_KNOWLEDGE:" + knowledgeResult.failures.join(","), 3);
}
arenaTest.finish(knowledgeResult.ok ? 0 : 4);
