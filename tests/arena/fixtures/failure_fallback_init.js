arenaTest.armWatchdog("coordinated second failure fallback", 300000);

function runCoordinatedWithCaptureFailures(
    firstEngineCall, failureCount)
{
    var original = ACTION_CAPTURE.canBePerformed;
    var rawCalls = 0;
    var rejections = 0;
    var result;
    ACTION_CAPTURE.canBePerformed = function(action, map)
    {
        ++rawCalls;
        if ((failureCount >= 1 &&
             rawCalls === firstEngineCall) ||
            (failureCount >= 2 &&
             rawCalls === firstEngineCall * 2))
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

var coordinatedControl =
    runCoordinatedWithCaptureFailures(0, 0);
var fallbackControl = arenaTest.runOperation(
    "coordinatedIntegrationProbe",
    null,
    {
        mapPath: "maps/2_player/Plug Mountain.map",
        controller: "Normal",
        pause: false
    });
var fallbackInjected =
    runCoordinatedWithCaptureFailures(
        coordinatedControl.rawCalls, 2);
var fallbackOk =
    coordinatedControl.ok &&
    fallbackControl.ok &&
    fallbackInjected.ok &&
    coordinatedControl.rawCalls > 0 &&
    fallbackInjected.rejections === 2 &&
    fallbackInjected.rawCalls >
        coordinatedControl.rawCalls * 2 &&
    fallbackInjected.actionCount === 1 &&
    fallbackControl.actionId === fallbackInjected.actionId &&
    fallbackControl.targetX === fallbackInjected.targetX &&
    fallbackControl.targetY === fallbackInjected.targetY &&
    fallbackControl.preStateHash ===
        fallbackInjected.preStateHash &&
    fallbackControl.finalStateHash === fallbackInjected.finalStateHash &&
    fallbackInjected.preStateHash !== fallbackInjected.finalStateHash;
if (!fallbackOk)
{
    GameConsole.print(
        "ARENA_COORDINATED_SECOND_FAILURE_FALLBACK:" +
            JSON.stringify({
                coordinated: coordinatedControl,
                control: fallbackControl,
                injected: fallbackInjected
            }),
        3);
}
arenaTest.finish(fallbackOk ? 0 : 4);
