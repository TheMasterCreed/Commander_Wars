arenaTest.armWatchdog("master stock admission", 300000);
var admission = arenaTest.runOperation("masterStockAdmission", null, {
    mapPath: "maps/2_player/River of War.map"
});
var saved = arenaTest.writeJsonResult("master-stock-admission.json", admission);
if (!admission.ok || !saved) {
    GameConsole.print("ARENA_MASTER_STOCK_ADMISSION:" + JSON.stringify(admission), 3);
}
arenaTest.finish(admission.ok && saved ? 0 : 4);
