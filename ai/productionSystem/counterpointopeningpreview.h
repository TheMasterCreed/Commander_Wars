#pragma once

// Detached adapter for the existing Counterpoint ordinary purchase policy.
inline constexpr char COUNTERPOINT_OPENING_SCRIPT[]=R"CounterpointJS(
var COORDINATED_COUNTERPOINT_OPENING = {
    // The acting turn may reserve the same seed the real buyer would draw.
    // Leave already prepared or deterministic streams in their original form.
    newTurnSeed: function(system, ai, map) {
        if (typeof COUNTERPOINTAI === "undefined" || typeof AI_BEHAVIOR_DISPATCH === "undefined" ||
            AI_BEHAVIOR_DISPATCH.getStrategy(map) !== COUNTERPOINTAI ||
            map.getCurrentPlayer().getPlayerID() !== ai.getPlayer().getPlayerID() ||
            COUNTERPOINTAI.PLANNER_DETERMINISTIC_SEED === true)
            return;
        var previous = COUNTERPOINTAI._loadPlannerState(system);
        if (previous && previous.playerId === ai.getPlayer().getPlayerID() && previous.day === map.getCurrentDay())
            return;
        return COUNTERPOINTAI._ensurePlannerState(system, ai, map).seed;
    },
    islandIndex: function(ai,unit){return ai.getIslandIndex(unit);},
    preview: function(system, ai, map, input, warm, cards) {
        try {
            if(typeof input==="string")input=JSON.parse(input);
            warm=warm||{};
            function memo(group,key,obtain) {
                var records=warm[group]||(warm[group]=Object.create(null));
                return Object.prototype.hasOwnProperty.call(records,key)?records[key]:(records[key]=obtain());
            }
            if (typeof COUNTERPOINTAI==="undefined" || typeof ACTION_BUILD_UNITS==="undefined" ||
                typeof COUNTERPOINTAI._preparePlanPhase!=="function" || typeof Proxy!=="function")
                throw Error("ordinary Counterpoint strategy unavailable");
            var strategy=COUNTERPOINTAI;
            var player=ai.getPlayer(), who=player.getPlayerID(), day=map.getCurrentDay();
            var chained=input.includeContinuationState===true, future=input.futureDay!==undefined;
            if(future) {
                if(!chained || !Number.isInteger(input.futureDay) || input.futureDay<=day || input.futureDay>day+10 ||
                    !Number.isInteger(input.comparisonSeed) || input.comparisonSeed<0 || input.comparisonSeed>4294967295 ||
                    strategy.PLANNER_DETERMINISTIC_SEED===true) fail("future day or buyer stream unavailable");
                day=input.futureDay;
            }
            var initialSourceFunds=player.getFunds();
            var width=map.getMapWidth(),height=map.getMapHeight();
            var stable={getUnitID:1,getUnitDamageID:1,getUnitDamage:1,getUnitType:1,getUnitCosts:1,getBaseCosts:1,
                getBaseMovementPoints:1,getMovementType:1,getBaseMinRange:1,getBaseMaxRange:1,getWeapon1ID:1,getWeapon2ID:1,
                getTypeOfWeapon1:1,getTypeOfWeapon2:1,isTransporter:1,getLoadingPlace:1,canCapture:1,getTransportUnits:2,getActionList:2};
            function key(x,y){return y*width+x;}
            function array(v){var a=[];for(var i=0;i<strategy._collectionLength(v);++i)a.push(strategy._collectionAt(v,i));return a;}
            function view(real, overrides,identity) {
                return new Proxy({}, {get:function(_,name){
                    if(Object.prototype.hasOwnProperty.call(overrides,name))return overrides[name];
                    var f=real[name];
                    if(identity && stable[name] && typeof f==="function")return function(){
                        var args=Array.prototype.slice.call(arguments);
                        return memo("unitFacts",identity+"/"+name+"/"+JSON.stringify(args),function(){
                            var result=f.apply(real,args);return stable[name]===2 ? array(result):result;
                        });
                    };
                    return typeof f==="function" ? function(){return f.apply(real,arguments);} : f;
                }});
            }
            function fail(reason){throw Error(reason);}
            var rawUnits={},owned=[],opposing=[],occupants={},players=[],buildings=[],realBuildings={},finalOwners={};
            var budget=input.funds,slots=input.unitSlots;
            if (!Array.isArray(input.units) || !Array.isArray(input.properties) || !Number.isInteger(budget) ||
                budget<0 || !Number.isInteger(slots) || slots<0 || map.getPlayerCount()!==2) fail("incomplete opening board");
            for(var side=0;side<2;++side){
                var army=map.getPlayer(side);
                array(army.getUnits()).forEach(function(u){rawUnits[u.getUniqueID()]=u;});
                array(army.getBuildings()).forEach(function(b){realBuildings[key(b.getX(),b.getY())]=b;});
            }
            array(map.getBuildings(null)).forEach(function(b){realBuildings[key(b.getX(),b.getY())]=b;});
            input.properties.forEach(function(b){finalOwners[key(b.x,b.y)]=b.owner;});
            if(chained && Object.keys(realBuildings).length!==Object.keys(finalOwners).length)
                fail("missing future property");
            var seen={};
            input.units.forEach(function(row){
                var u=rawUnits[row.id] || (chained && cards.units[String(row.id)]);
                if (!u) {if(row.hp<=0)return; fail("unknown/embarked projected unit "+row.id);}
                if (seen[row.id]) fail("duplicate projected unit "+row.id);
                seen[row.id]=true;
                if(row.hp<=0)return;
                if(!Number.isFinite(row.hp) || row.hp>10 || !map.onMap(row.x,row.y) || occupants[key(row.x,row.y)]) fail("invalid projected unit position");
                var id=u.getOwner().getPlayerID();
                var reads={getX:function(){return row.x;},getY:function(){return row.y;},getHp:function(){return row.hp;},
                    getHpRounded:function(){return Math.ceil(row.hp);},getOwner:function(){return players[id];},previewOriginal:u};
                if(chained) {
                    if(id!==row.owner || (row.type!==undefined && u.getUnitID()!==row.type)) fail("future unit card mismatch");
                    reads.getUniqueID=function(){return row.id;};
                    reads.getUnitCosts=function(){return players[id].getCosts(u.getUnitID(),Qt.point(row.x,row.y));};
                }
                var unit=view(u,reads,"unit"+row.id);
                occupants[key(row.x,row.y)]=unit;(id===who?owned:opposing).push(unit);
            });
            Object.keys(rawUnits).forEach(function(k){if (!seen[k]) fail("missing projected unit "+k);});
            function sorted(a){return a.sort(function(l,r){return key(l.getX(),l.getY())-key(r.getX(),r.getY());});}
            sorted(owned);sorted(opposing);
            function ownedBuildings(s,id) {
                return buildings.filter(function(b){return b.getOwnerID()===s && (!id || b.getBuildingID()===id);});
            }
            [0,1].forEach(function(s) {
                var real=map.getPlayer(s);
                var reads={getUnits:function(){return s===who?owned:opposing;},getEnemyUnits:function(){return s===who?opposing:owned;},
                    getBuildings:function(id){return ownedBuildings(s,id);},getEnemyBuildings:function(){return ownedBuildings(1-s);},
                    getUnitCount:function(){var limit=map.getGameRules().getUnitLimit();return s===who && limit>0 ? limit-slots : (s===who?owned:opposing).length;}};
                if(s===who)reads.getFunds=function(){return budget;};
                if(chained) {
                    reads.calcIncome=function(modifier){
                        if(modifier!==undefined && modifier!==1)fail("unknown future income modifier");
                        return ownedBuildings(s).reduce(function(sum,b){return sum+b.getIncome();},0);
                    };
                    if(future && s!==who)reads.getFunds=function(){
                        if(!Number.isInteger(input.opponentFunds) || input.opponentFunds<0)fail("future opposing wallet unavailable");
                        return input.opponentFunds;
                    };
                }
                players[s]=view(real,reads);
            });
            Object.keys(realBuildings).forEach(function(sq){
                var raw=realBuildings[sq],old=raw.getOwnerID(),s=Object.prototype.hasOwnProperty.call(finalOwners,sq)?finalOwners[sq]:old;
                if(chained && s!==old) {
                    raw=cards.buildings[sq];
                    if(!raw || raw.getOwnerID()!==s)fail("future building owner card unavailable");
                }
                else if(s!==old && raw.isProductionBuilding()) fail("newly captured production menu requires its new owner's action query");
                var t=view(raw.getTerrain(),{getUnit:function(){return occupants[sq]||null;}});
                buildings.push(view(raw,{getOwnerID:function(){return s;},getOwner:function(){return s<0?null:players[s];},
                    getTerrain:function(){return t;},previewOriginal:raw}));
            });
            sorted(buildings);
            var byTile={}; buildings.forEach(function(b){byTile[key(b.getX(),b.getY())]=b;});
            var proxyMap=view(map,{getPlayer:function(s){return players[s];},getCurrentDay:function(){return day;},
                getBuildings:function(p){return ownedBuildings(p===null?-1:p.getPlayerID());},
                getTerrain:function(x,y){var b=byTile[key(x,y)];return b ? {getBuilding:function(){return b;}} : map.getTerrain(x,y);}});
            var ownBuildings=ownedBuildings(who),enemyBuildings=ownedBuildings(1-who);
            var varId=strategy.PLANNER_STATE_VARIABLE_ID, variable=input.persistedState===undefined ? system.getVariables().getVariable(varId):null;
            var stored=input.persistedState!==undefined ? input.persistedState : (variable ? array(variable.readDataListString()):[]);
            var original=JSON.stringify(stored),scratch=stored.slice();
            if(warm.originalState!==original) {warm.portfolios=Object.create(null);warm.originalState=original;}
            var variableCopy={readDataListString:function(){return scratch;},writeDataListString:function(v){scratch=array(v);}};
            var comparisonSeed,mode="common-derived";
            var previous=stored.length===1?JSON.parse(String(stored[0])):null;
            if(previous && !strategy._validPlannerState(previous)) previous=null;
            if(future && (!previous || previous.day!==day-1 || previous.playerId!==who || !previous.ordinaryPrepared ||
                strategy._hasPendingOrdinaryPlan(previous)))fail("future buyer predecessor does not end the previous turn");
            if(previous && previous.day===day && previous.playerId===who)mode="saved-current-stream";
            else if(strategy.PLANNER_DETERMINISTIC_SEED===true)mode="buyer-derived";
            else if(input.reservedTurnSeed!==undefined)mode="reserved-current-stream";
            else if(input.comparisonSeed!==undefined)mode="common-supplied";
            comparisonSeed=mode==="saved-current-stream" ? previous.seed :
                mode==="reserved-current-stream" ? input.reservedTurnSeed>>>0 :
                mode==="common-supplied" ? input.comparisonSeed>>>0 :
                memo("seed",previous ? previous.generation+1:1,function(){return system.deriveCounterpointSeed(strategy.RNG_ALGORITHM_VERSION,previous ? previous.generation+1 : 1);});
            var orders=[],menus={},born={};
            function nativeMenu(b,action) {
                var sq=key(b.getX(),b.getY()),k=sq+":"+action;
                if(Object.prototype.hasOwnProperty.call(menus,k))return menus[k];
                var native=memo("menu",k,function(){
                    var data;
                    if(chained) {
                        if(action!==strategy.ACTION_BUILD_UNITS || typeof ACTION_BUILD_UNITS.getProductionMenuData==="function")
                            fail("future special or custom production menu");
                        data=cards.collectors[String(sq)];
                        if(!data)fail("missing prospective owner menu");
                        ACTION_BUILD_UNITS.getStepData({getTargetBuilding:function(){return b;},
                            getTargetUnit:function(){return occupants[sq]||null;}},data,proxyMap);
                    }
                    else data=system.getProductionActionData(b.previewOriginal,action);
                    if (!data)return null;
                    if(typeof data.getLegalIgnoringFundsList!=="function")fail("menu lacks independent item legality");
                    return {ids:array(data.getUnitIds()),costs:array(data.getTransactionCosts()),values:array(data.getStrategicValues()),
                        legal:array(data.getLegalIgnoringFundsList())};
                });
                if (!native)return menus[k]=null;
                if (action!==strategy.ACTION_BUILD_UNITS)fail("special production action "+action);
                var ids=native.ids,costs=native.costs,values=native.values,legal=native.legal;
                var available=function(){
                    if(b.getOwnerID()!==who || slots<=0 || born[sq])return false;
                    var actionView={getTargetUnit:function(){return occupants[sq]||null;},getTargetBuilding:function(){return b;}};
                    return ACTION_BUILD_UNITS.canBePerformed(actionView,proxyMap)===true;
                };
                return menus[k]={getX:function(){return b.getX();},getY:function(){return b.getY();},getActionId:function(){return action;},
                    getUnitIds:function(){return ids;},getTransactionCosts:function(){return costs;},getStrategicValues:function(){return values;},
                    getLegalIgnoringFundsList:function(){return legal;},getActionAvailable:available,
                    getEnabledList:function(){return legal.map(function(v,i){return v && costs[i]>=0 && costs[i]<=budget;});}};
            }
            var ghosts=Object.create(null);
            function dummy(id) {
                if(ghosts[id])return ghosts[id];
                var model=new Proxy({},{get:function(_,name){
                    if(name==="previewDummy")return id;
                    var cacheKey=id+"/"+name;
                    if(!stable[name]) {
                        var sample=system.getDummyUnit(id)[name];
                        if(typeof sample!=="function")return sample;
                        return function(){var u=system.getDummyUnit(id);return u[name].apply(u,arguments);};
                    }
                    return function(){
                        var args=Array.prototype.slice.call(arguments);
                        return memo("dummyFacts",cacheKey+"/"+JSON.stringify(args),function(){
                            var u=system.getDummyUnit(id),v=u[name].apply(u,args);
                            return stable[name]===2 ? array(v):v;
                        });
                    };
                }});
                return ghosts[id]=model;
            }
            var previewSystem=view(system,{
                getVariables:function(){return {createVariable:function(id){if(id!==varId)fail("unrecognized strategy state");return variableCopy;}};},
                deriveCounterpointSeed:function(){return comparisonSeed;},randomCounterpointSeed:function(){return comparisonSeed;},
                getDummyUnit:dummy,
                getCounterpointBaseDamage:function(a,b){return memo("power",a+"/"+b,function(){return system.getCounterpointBaseDamage(a,b);});},
                estimateCounterpointDeploymentTurns:function(){
                    var args=Array.prototype.slice.call(arguments);
                    return memo("deployment",JSON.stringify(args),function(){return system.estimateCounterpointDeploymentTurns.apply(system,args);});
                },
                resetForcedProduction:function(){},getProductionActionData:nativeMenu,
                executeCounterpointBuild:function(x,y,id,ordinal,price){
                    var sq=key(x,y),b=byTile[sq];
                    if(!b || !nativeMenu(b,strategy.ACTION_BUILD_UNITS).getActionAvailable() || price<0 || price>budget) fail("invalid preview purchase");
                    born[sq]=true;budget-=price;--slots;orders.push({x:x,y:y,type:id,price:price});return true;
                }});
            function sameIsland(unit,b) {
                var idx=ai.getIslandIndex(unit.previewOriginal);
                return ai.onSameIsland(idx,unit.getX(),unit.getY(),b.getX(),b.getY());
            }
            var previewAI=view(ai,{getPlayer:function(){return players[who];},getSimpleProductionSystem:function(){return previewSystem;},
                getIslandSize:function(u,x,y){return ai.getIslandSize(u.previewDummy ? system.getDummyUnit(u.previewDummy):u,x,y);},
                shareIslandWithEnemy:function(units,ours,theirs){return units.some(function(u){
                    var t=u.getUnitType();
                    return t!==GameEnums.UnitType_Infantry && t!==GameEnums.UnitType_Hovercraft && t!==GameEnums.UnitType_Air &&
                        t!==GameEnums.UnitType_Naval && ours.some(function(b){return b.isProductionBuilding()&&sameIsland(u,b);}) &&
                        theirs.some(function(b){return b.isProductionBuilding()&&sameIsland(u,b);});});},
                getBuildingCountsOnEnemyIslands:function(units,theirs){return theirs.filter(function(b){return !units.some(function(u){return sameIsland(u,b);});}).length;}});
            // In the stock ordinary policy own coordinates are queried for occupied
            // facilities and island connectivity. Keep census order (sampling is
            // bounded), every enemy tile/HP, all owners and every factory blocker.
            function ownComponents() {
                var on=Object.create(null),ground=Object.create(null),order=[];
                for(var i=0;i<owned.length;++i) {
                    var u=owned[i],t=u.getMovementType(),type=u.getUnitType(),card=warm.components && warm.components[t];
                    if(!card)return owned.map(function(v){return [v.getUniqueID(),v.getX(),v.getY()];});
                    var id=card[key(u.getX(),u.getY())];
                    if(id<0)continue;
                    var k=t+"/"+id;
                    on[k]=true;
                    if(type!==GameEnums.UnitType_Infantry && type!==GameEnums.UnitType_Hovercraft &&
                       type!==GameEnums.UnitType_Air && type!==GameEnums.UnitType_Naval)ground[k]=true;
                    if(order.indexOf(t)<0)order.push(t);
                }
                var productive=Object.create(null),share=false,unreachable=0;
                ownBuildings.forEach(function(b){
                    if(b.isProductionBuilding())order.forEach(function(t){productive[t+"/"+warm.components[t][key(b.getX(),b.getY())]]=true;});
                });
                enemyBuildings.forEach(function(b){
                    var reachable=false;
                    order.forEach(function(t){
                        var k=t+"/"+warm.components[t][key(b.getX(),b.getY())];
                        if(on[k])reachable=true;
                        if(ground[k]&&productive[k]&&b.isProductionBuilding())share=true;
                    });
                    if(!reachable)++unreachable;
                });
                return [share,unreachable];
            }
            function census() {
                if(owned.length>strategy._candidateLimit())return owned.map(function(u){return [u.getUniqueID(),u.getHp()];});
                var sample=[],groups=Object.create(null),ids=[];
                owned.forEach(function(u) {
                    var id=u.getUniqueID();ids.push(id);
                    var k=memo("censusClass",id,function(){
                        var kind=u.getUnitID();
                        var weapons=strategy._weaponData(u,u.getBaseMinRange(),u.getBaseMaxRange());
                        return strategy._sampleCompositionKey({id:kind,ownerId:who,weaponProfileKey:strategy._damageCacheKey(kind,weapons)});
                    });
                    var g=groups[k];
                    if(!g){g=[k,id,0,0];sample.push(g);groups[k]=g;}
                    ++g[2];g[3]+=strategy._hpFrac(u);
                });
                ids.sort(function(a,b){return a-b;});
                return [ids,sample];
            }
            function derivedKey(){
                return JSON.stringify([mode,comparisonSeed,budget,slots,census(),ownComponents(),
                opposing.map(function(u){return [u.getUniqueID(),u.getHp(),u.getX(),u.getY()];}),
                buildings.map(function(b){return [b.getOwnerID(),b.isProductionBuilding() ? !!occupants[key(b.getX(),b.getY())]:false];})]);
            }
            var derived=derivedKey();
            if(warm.portfolios && warm.portfolios[derived]) return Object.assign({},warm.portfolios[derived],{reusedEquivalent:true});
            var s=strategy._ensurePlannerState(previewSystem,previewAI,proxyMap);
            if(mode==="common-derived" && s.seed!==comparisonSeed && strategy.privateFactoryStreamInstalled===true &&
                input.privateReplay===true) mode="isolated-shared-stream";
            if(s.ordinaryPrepared)fail("ordinary queue already committed");
            if(!s.specialPrepared) {
                strategy._preparePlanPhase(previewSystem,previewAI,ownBuildings,owned,opposing,enemyBuildings,proxyMap,s,strategy.PHASE_SPECIAL);
                s.specialPrepared=true;
            }
            if(s.plans.some(function(p){return p.phase===strategy.PHASE_SPECIAL && !p.complete;}))fail("pending special order");
            strategy._preparePlanPhase(previewSystem,previewAI,ownBuildings,owned,opposing,enemyBuildings,proxyMap,s,strategy.PHASE_ORDINARY);
            s.ordinaryPrepared=true;
            if(!strategy._savePlannerState(previewSystem,s))fail("ordinary plan size");
            var prepared=s.drawCounter,seed=s.seed,max=s.plans.length;
            for(var i=0;i<=max && strategy._hasPendingOrdinaryPlan(s);++i) {
                if(!strategy.buildUnitSimpleProductionSystem(previewSystem,previewAI,ownBuildings,owned,opposing,enemyBuildings,proxyMap)) break;
                s=JSON.parse(scratch[0]);
            }
            if((input.persistedState===undefined && JSON.stringify(variable ? array(variable.readDataListString()):[])!==original) || player.getFunds()!==initialSourceFunds)
                fail("source buyer state differed at end of preview");
            var answer={supported:true,reason:"",seedMode:mode,seed:seed,orders:orders,remainingFunds:budget,
                preparedDrawCounter:prepared,drawCounter:JSON.parse(scratch[0]).drawCounter};
            if(chained) {
                if(strategy._hasPendingOrdinaryPlan(JSON.parse(scratch[0])))fail("future ordinary queue unfinished");
                answer.continuationState=scratch.slice();
                answer.projectedDay=day;
            }
            if(!warm.portfolios)warm.portfolios=Object.create(null);
            warm.portfolios[derived]=answer;
            return answer;
        } catch(error) {
            return {supported:false,reason:String(error),orders:[]};
        }
    }
};

)CounterpointJS";
