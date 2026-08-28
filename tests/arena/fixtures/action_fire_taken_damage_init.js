var ARENA_ACTION_FIRE_LOG_LEVEL_ERROR = 3;
var ARENA_ACTION_FIRE_EXIT_ASSERT_FAILED = 4;
var ARENA_ACTION_FIRE_ATTACKER_TAKEN_DAMAGE = 3.5;
var ARENA_ACTION_FIRE_DEFENDER_TAKEN_DAMAGE = 4.5;
if (typeof arenaTest === "undefined")
{
    GameConsole.print("ARENA_ACTION_FIRE:missing-global", ARENA_ACTION_FIRE_LOG_LEVEL_ERROR);
}
else if (typeof ACTION_FIRE === "undefined")
{
    GameConsole.print("ARENA_ACTION_FIRE:missing-action", ARENA_ACTION_FIRE_LOG_LEVEL_ERROR);
    arenaTest.finish(ARENA_ACTION_FIRE_EXIT_ASSERT_FAILED);
}
else
{
    var observedAttackerTakenDamage = null;
    var observedDefenderTakenDamage = null;
    var originalCalcBattleDamage4 = ACTION_FIRE.calcBattleDamage4;
    ACTION_FIRE.calcBattleDamage4 = function(map, action, attacker, attackerTakenDamage, atkPosX, atkPosY, defender, x, y, defenderTakenDamage)
    {
        observedAttackerTakenDamage = attackerTakenDamage;
        observedDefenderTakenDamage = defenderTakenDamage;
        return null;
    };
    try
    {
        ACTION_FIRE.calcBattleDamage3(null, null, null, ARENA_ACTION_FIRE_ATTACKER_TAKEN_DAMAGE, 0, 0, null, 0, 0, ARENA_ACTION_FIRE_DEFENDER_TAKEN_DAMAGE, 0, 0);
    }
    finally
    {
        ACTION_FIRE.calcBattleDamage4 = originalCalcBattleDamage4;
    }
    var forwardingOk = observedAttackerTakenDamage === ARENA_ACTION_FIRE_ATTACKER_TAKEN_DAMAGE &&
                       observedDefenderTakenDamage === ARENA_ACTION_FIRE_DEFENDER_TAKEN_DAMAGE;
    arenaTest.finish(forwardingOk ? 0 : ARENA_ACTION_FIRE_EXIT_ASSERT_FAILED);
}
