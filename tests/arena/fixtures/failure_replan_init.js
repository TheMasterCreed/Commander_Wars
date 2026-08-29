arenaTest.armWatchdog("coordinated first failure replan", 300000);

function runWithCaptureFailure(rejectRawCall)
{
    var original = ACTION_CAPTURE.canBePerformed;
    var rawCalls = 0;
    var rejections = 0;
    var result;
    ACTION_CAPTURE.canBePerformed = function(action, map)
    {
        ++rawCalls;
        if (rawCalls === rejectRawCall)
        {
            ++rejections;
            return false;
        }
        return original(action, map);
    };
    try
    {
        result = arenaTest.runOperation(
            "coordinatedIntegrationProbe",
            null,
            {
                mapPath: "maps/2_player/Plug Mountain.map",
                controller: "Coordinated",
                pause: false
            });
    }
    finally
    {
        ACTION_CAPTURE.canBePerformed = original;
    }
    result.rawCalls = rawCalls;
    result.rejections = rejections;
    return result;
}

var replanControl = runWithCaptureFailure(0);
var replanInjected =
    runWithCaptureFailure(replanControl.rawCalls);
var replanOk =
    replanControl.ok &&
    replanInjected.ok &&
    replanControl.rawCalls > 0 &&
    replanControl.rejections === 0 &&
    replanInjected.rejections === 1 &&
    replanInjected.rawCalls === replanControl.rawCalls * 2 &&
    replanControl.actionCount === 1 &&
    replanInjected.actionCount === 1 &&
    replanControl.actionId === replanInjected.actionId &&
    replanControl.targetX === replanInjected.targetX &&
    replanControl.targetY === replanInjected.targetY &&
    replanControl.preStateHash === replanInjected.preStateHash &&
    replanControl.finalStateHash === replanInjected.finalStateHash;
if (!replanOk)
{
    GameConsole.print(
        "ARENA_COORDINATED_FIRST_FAILURE_REPLAN:" +
            JSON.stringify({
                control: replanControl,
                injected: replanInjected
            }),
        3);
}
arenaTest.finish(replanOk ? 0 : 4);
