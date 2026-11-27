#pragma once

#include "ai/coordinator/stockcapturevalidation.h"
#include "ai/coordinator/townassaultledger.h"

namespace Coordinator
{
    inline QJsonArray fundedPostShots(const QJsonArray &proofs)
    {
        QJsonArray out;
        for(const auto &v:proofs)
        {
            const auto leg=v.toObject();const auto steps=leg["captureInstallments"].toArray();
            if(leg["status"]!="DECLINED_EXPOSED_APPROACH" || steps.isEmpty())continue;
            const int entry=steps.first().toObject()["tick"].toInt(-1);
            for(const auto &z:leg["shots"].toArray())
            {
                const auto s=z.toObject();if(!s.contains("recruit") || s["targetTile"]!=leg["tile"] ||
                    s["tick"].toInt(-1)!=entry+1 || s["hpAfter"].toDouble(-1)!=0)continue;
                out.append(QJsonObject{{"tile",leg["tile"]},{"capper",leg["unit"]},{"entry",entry},
                    {"enemy",!leg["enemy"].toBool()},{"factory",s["birthSite"]},{"build",s["birthTick"]},{"type",s["recruit"]}});
            }
        }
        return out;
    }

    inline QJsonObject pairedFundedPosts(const QJsonObject &first,const QJsonObject &second)
    {
        std::map<QString,QJsonObject> menus[2];int arm=0;
        for(const auto *team:{&first,&second})
        {
            for(const auto &v:team->value("replyAlternatives").toArray())
            {
                const auto row=v.toObject();const auto name=row["replyPolicy"].toString();
                if(name.isEmpty() || !menus[arm].emplace(name,row).second)return {};
            }
            ++arm;
        }
        const auto post=[](const QJsonObject &reply,const QJsonObject &question){
            for(auto v:reply["productionOrders"].toArray())
            {
                const auto o=v.toObject();if(o["factory"]==question["factory"] && o["enemy"]==question["enemy"] &&
                    o["buildHalfTurn"]==question["build"] && o["type"]==question["type"] && o["kind"]=="DEPLOY" &&
                    o["retainedDeployment"].toBool() && o["end"]==o["firstStop"])return o["end"].toArray();
            }
            return QJsonArray{};
        };
        QJsonObject result;
        for(int i=0;i<2;++i)for(const auto &[name,source]:menus[i])
        {
            const auto peer=menus[1-i].find(name);if(peer==menus[1-i].end())continue;
            for(auto v:source["fundedPostShots"].toArray())
            {
                auto q=v.toObject();bool earned=false;
                for(auto x:peer->second["settledStockIntentions"].toArray())
                {const auto t=x.toObject();if(t["tile"]==q["tile"] && t["capper"]==q["capper"] && t["entryTick"]==q["entry"]){earned=true;break;}}
                if(!earned)continue;
                const auto a=post(menus[0].at(name),q),b=post(menus[1].at(name),q);
                if(a.size()!=2 || b.size()!=2 || a==b)continue;
                q["posts"]=QJsonArray{a,b};auto items=result[name].toArray();if(!items.contains(q))items.append(q);result[name]=items;
            }
        }
        return result;
    }

    inline bool mergeFundedPostQuestions(QJsonObject &known,const QJsonObject &found)
    {
        bool changed=false;
        for(auto it=found.begin();it!=found.end();++it)
        {
            auto current=known.value(it.key()).toArray();
            for(const auto &q:it.value().toArray())if(!current.contains(q)){current.append(q);changed=true;}
            if(!current.isEmpty())known[it.key()]=current;
        }
        return changed;
    }

    inline bool couldExposeFundedPost(const QJsonObject &team)
    {
        for(const auto &v:team.value("replyAlternatives").toArray())
            for(const auto &item:v.toObject().value("productionOrders").toArray())
            {
                const auto order=item.toObject();
                if(order["kind"]=="DEPLOY" && order["retainedDeployment"].toBool() &&
                   order["end"].toArray().size()==2 && order["end"]==order["firstStop"])return true;
            }
        return false;
    }

    // A conditional shot concerns a single recorded raid. Even a numerical
    // title effect leaves the gun's other duties and the response menu open.
    inline QJsonArray fundedPostObligations(const QJsonObject &requests,const QJsonObject &a,const QJsonObject &b)
    {
        QJsonArray pending;
        for(auto it=requests.begin();it!=requests.end();++it)for(const auto &v:it.value().toArray())
        {
            auto question=v.toObject();QJsonArray arms;bool bothEnded=true;
            for(const auto *team:{&a,&b})
            {
                QJsonObject reply;for(const auto &r:team->value("replyAlternatives").toArray())
                    if(r.toObject().value("replyPolicy")==it.key()){reply=r.toObject();break;}
                // Only a native physical endpoint before the purchase can
                // make both post choices irrelevant without a continuation.
                bool ended=false;
                if(reply.value("physicalDecisionBoundary")=="LOST_ON_FIRST_REPLY" && reply.value("hqStatus")=="ENEMY_CAN_WIN_NEXT_ACTION" &&
                   reply.value("terminalClass").toInt()==-1 && reply.value("projectedTerminalHalfTurn").toInt(-1)==1 &&
                   reply.value("priceStatus")=="WITNESSED" && question.value("build").toInt(-1)>1)ended=true;
                QJsonArray checks;
                for(const auto &post:question["posts"].toArray())
                {
                    QString status="NO_MATCHING_REPLAY";
                    for(const auto &x:reply.value("fundedPostResponses").toArray())
                    {
                        const auto hearing=x.toObject();
                        if(hearing["post"]!=post)continue;
                        bool same=true;
                        for(auto field:{"capper","entry","tile","build","type","factory","enemy"})same &= hearing[field]==question[field];
                        if(same){status=hearing["status"].toString();break;}
                    }
                    checks.append(QJsonObject{{"post",post},{"observed",status}});
                }
                bothEnded &= ended;
                arms.append(QJsonObject{{"status",ended?"PHYSICAL_END_BEFORE_PURCHASE":reply.isEmpty()?"REPLY_MISSING":"FULL_CONTINUATION_OUTSTANDING"},{"checks",checks}});
            }
            if(!bothEnded){question["replyPolicy"]=it.key();question["arms"]=arms;pending.append(question);}
        }
        return pending;
    }

    // One conditional line on the frozen orders. The post is fixed before
    // the original infantry's route is replayed. A blocked sampled route,
    // missing job, or unheard counterstrategy cannot certify a team rank.
    template<class Timing> QJsonObject hearFundedPost(const QJsonObject &question,const QJsonArray &requestedPost,
        const CombatTerrain &terrain,std::span<const KnownUnit> units,const ProductionProjection &production,
        const StockCaptureSettlement &settled,const QJsonArray &orders,const QJsonObject &leg,const ProductionProperty &property,
        std::span<const TilePoint> positions,std::span<const double> hp,std::span<const std::array<int,2>> ammo,
        std::span<const MilliFunds> cash,int cutoff,int threshold,StockRouteLowerBounds &lower,Timing timing,std::int64_t &work)
    {
        const auto &column=property.column;
        auto out=question;out.remove("posts");out["post"]=requestedPost;out["scope"]="DECLARED_POST_AND_ONE_RECORDED_STOCK_APPROACH";
        const auto end=[&](const char *reason){out["status"]=reason;return out;};
        const auto xy=[](const QJsonValue &v){const auto a=v.toArray();return a.size()==2?TilePoint{a[0].toInt(-1),a[1].toInt(-1)}:INVALID_TILE;};
        const auto point=[](TilePoint p){return QJsonArray{p.x,p.y};};
        const int capId=question["capper"].toInt(-1),entry=question["entry"].toInt(-1),side=int(question["enemy"].toBool());
        const TilePoint post=xy(requestedPost),factory=xy(question["factory"]),town=xy(question["tile"]);
        if(!settled.townTraffic || town!=column.tile || capId<0 || entry<0 || entry%2==side || entry+1>cutoff ||
           int(leg["enemy"].toBool())==side || leg["unit"].toInt(-1)!=capId || xy(leg["tile"])!=town ||
           (leg["status"]!="WITNESSED" && leg["status"]!="DECLINED_EXPOSED_APPROACH"))return end("STOCK_APPROACH_NOT_VERIFIED");
        for(const auto &prior:leg["shots"].toArray())if(prior.toObject()["tick"].toInt(-1)<=entry)return end("EARLIER_COMBAT_REQUIRES_SHARED_PREFIX");
        const auto initial=leg["captureInstallments"].toArray();if(initial.isEmpty())return end("NO_CAPTURE_INSTALLMENT");
        const auto installment=initial.first().toObject();const double before=installment["hpBeforeReply"].toDouble(-1);
        if(installment["tick"].toInt(-1)!=entry || before<=0 || installment["carryBefore"].toInt(-1)<0)return end("DIFFERENT_CAPTURE_TIMELINE");
        int cap=NO_UNIT,gun=NO_UNIT;const int n=static_cast<int>(units.size());
        if(terrain.units.size()!=units.size() || positions.size()!=units.size() || hp.size()!=units.size() || ammo.size()!=units.size())return end("INCOMPLETE_ROSTER");
        for(int i=0;i<n;++i)if(terrain.units[i].engineUnitId==capId)cap=i;
        for(int i=0;i<int(production.fundedGuns.size());++i)
        {const auto &g=production.fundedGuns[i];if(g.side==side && g.factory==factory && g.buildTick==question["build"].toInt(-1) && g.unit && g.unit->type==question["type"].toString())gun=i;}
        if(cap<0 || gun<0)return end("CAPTURE_OR_PURCHASE_ABSENT");
        for(const auto &event:property.ownershipChanges)
            if(event.entryTick!=entry || event.owner!=(side?OwnerSign::Ours:OwnerSign::Enemy))return end("ANOTHER_TITLE_ON_THE_SAME_CALENDAR");
        const auto &original=production.fundedGuns[gun];const auto &type=*original.unit;const int first=original.buildTick+2,fire=entry+1,id=n+gun;
        if(first>=fire || !type.movementGrid || type.movement<=0 || original.observedService || settled.fundedFollowups.contains(gun))return end("UNRECONCILED_SERVICE_OR_DEPLOYMENT");
        const auto &grid=*type.movementGrid;const int width=grid.width();const auto cell=[&](TilePoint p){return p.y*width+p.x;};
        if(!grid.onGrid(factory.x,factory.y) || !grid.onGrid(post.x,post.y) || !grid.onGrid(town.x,town.y) || !grid.onGrid(original.destination.x,original.destination.y))return end("INVALID_MOVEMENT_GRID");
        QJsonObject order;
        for(auto v:orders){const auto o=v.toObject();if(o["factory"]==question["factory"] && o["enemy"]==question["enemy"] && o["buildHalfTurn"]==question["build"] && o["type"]==question["type"])order=o;}
        if(order["kind"]!="DEPLOY" || !order["retainedDeployment"].toBool() || order["firstStepHalfTurn"].toInt(-1)!=first || xy(order["end"])!=original.destination || order["end"]!=order["firstStop"] ||
           order["target"].toInt(0)!=NO_UNIT || !order["otherPricedShots"].toArray().isEmpty())return end("EXISTING_PURCHASE_JOB_NOT_RELEASED");
        const auto area=std::int64_t(width)*grid.height();if(work<area+n)return end("WORK_LIMIT");work-=area+n;
        StockCaptureTraffic calendar=*settled.townTraffic;
        // Other bookings are immutable. Only this particular recruit's original route is replaced.
        calendar.removeFutureUses(id,first);
        for(const auto &v:leg["stops"].toArray())
        {
            const auto step=v.toObject();const int at=step["tick"].toInt(-1);const auto atTile=xy(step["tile"]);
            if(at<0 || !grid.onGrid(atTile.x,atTile.y))return end("INVALID_STOCK_STOP");
            if(at>entry)break;
            for(auto z:step["cells"].toArray())
            {const auto p=xy(z);if(!grid.onGrid(p.x,p.y))return end("INVALID_STOCK_TRANSIT");calendar.reserve(cell(p),at,at,1-side,cap,false);}
            calendar.reserve(cell(atTile),at,at==entry?fire:at+1,1-side,cap,true);
        }
        const auto deployment=stockCaptureRoute(grid,calendar,id,side,factory,post,type.movement,type.initialFuel,first,first,first,1,work,INVALID_TILE,&lower,{},type.ignoresUnitCollision);
        if(!deployment)return end("DEPLOYMENT_WORK_LIMIT");
        if(deployment->entryTick!=first)return end("NO_LEGAL_FIRST_TURN_DEPLOYMENT_ON_RETAINED_CALENDAR");
        QJsonArray path;for(const auto &s:deployment->steps)for(int sq:s.cells)path.append(point({sq%width,sq/width}));
        out["deploymentPath"]=path;out["deploymentTick"]=first;out["deploymentFuelSpent"]=deployment->fuel;out["originalPost"]=point(original.destination);
        for(int t=first;t<fire;++t)if(!calendar.open(id,side,cell(post),t,true))return end("POST_REQUIRED_BY_ANOTHER_ORDER_BEFORE_FIRE");
        auto reserve=original;reserve.destination=post;reserve.readyTick=first;reserve.targetUnit=NO_UNIT;reserve.impactTick=-1;
        reserve.fuelAtPost=type.initialFuel<0?-1:type.initialFuel-deployment->fuel;
        QJsonArray otherClaims;
        if(auto it=settled.townGunClaims.find(id);it!=settled.townGunClaims.end())for(const auto &job:it->second)
        {
            if(work<=0)return end("DUTY_SCAN_WORK_LIMIT");--work;
            if(job.claimant==cap && job.target==town && job.entry==entry)continue;
            if(job.through<first)continue;
            QJsonObject debt{{"target",point(job.target)},{"entry",job.entry},{"firstShot",job.firstShot},
                {"through",job.through},{"incomplete",job.incomplete}};
            if(job.claimant>=0 && job.claimant<n)debt["claimant"]=terrain.units[job.claimant].engineUnitId;
            otherClaims.append(debt);
        }
        if(auto it=settled.townPostDependencies.find(cell(original.destination));it!=settled.townPostDependencies.end())
            for(const auto &use:it->second)
            {
                if(work<=0)return end("DUTY_SCAN_WORK_LIMIT");--work;
                if((use.holder==id || use.holder==NO_UNIT) && use.occupiedTick>=first &&
                   !(use.claimant==cap && use.target==town && use.claim==entry))
                    otherClaims.append(QJsonObject{{"retainedPost",point(original.destination)},{"occupiedTick",use.occupiedTick},
                        {"target",point(use.target)},{"knownHolder",use.holder==id}});
            }
        out["otherRetainedDuties"]=otherClaims;
        if(!otherClaims.isEmpty())out["dutyStatus"]="REPLAY_REQUIRED_WITH_OTHER_CLAIMANTS";
        // Hypothesis is explicit: this recruit arrives alive and undamaged. A
        // changed exposure or a lost independent deployment still needs pricing.
        if(original.exposure!=0 || type.startingHp!=10.)return end("PURCHASE_HEALTH_NOT_VERIFIED");
        std::vector<CaptureDefenderAvailability> rights(n);
        std::array<FundedCaptureGun,1> one{reserve};
        CaptureRouteDefense defense{terrain,units,positions,hp,ammo,rights,one,
            [&](int who,int t,TilePoint p,bool transit){return who==n && calendar.open(id,side,cell(p),t,!transit);},
            [&](int who,int){return who==n;}};
        defense.canLeavePost=[&](int who,int t,TilePoint p){return who==n && t==fire && p==post && fire+2<=cutoff;};
        CaptureSurvivalState state{before,installment["carryBefore"].toInt()};
        if(!advanceCaptureSurvival(defense,state,cap,town,entry,true,threshold,units[cap].captureRate,units[cap].hpRounded,work))return end("FIRING_REPLAY_UNPRICED");
        const int rate=replyCaptureRate(units[cap].captureRate,units[cap].hpRounded-ceilHpSteps(before),false);
        if(rate<=0 || cash.empty() || cutoff>2*std::int64_t(cash.size()))return end("CAPTURE_TIMING_OR_CASH_UNPRICED");
        TownAssaultLine titled;titled.side=1-side;titled.town=town;
        const auto finish=entry+2*(widenedCaptureTurns(installment["carryBefore"].toInt(),rate,threshold)-1);
        if(finish<=cutoff)titled.finishTick=static_cast<int>(finish);
        // Remove this original's already-earned title from the reference cash
        // before comparing a new t7-style completion to withholding the title.
        // An independent transfer of the same property needs a different wallet.
        std::vector<MilliFunds> noTitle(cash.begin(),cash.end());
        if(!property.ownershipChanges.empty())
        {
            if(property.ownershipChanges.size()!=1 || leg["status"]!="WITNESSED" ||
               leg["actualCompletionTick"].toInt(-1)!=property.ownershipChanges.front().completionTick)
                return end("TITLE_OWNER_OR_COMPLETION_UNMATCHED");
            auto originalTitle=titled;originalTitle.finishTick=property.ownershipChanges.front().completionTick;
            for(int t=1;t<=cutoff;++t)
                if(!RelativeStockDetail::checkedSubtract(noTitle[(t-1)/2],assaultPaydayChange(originalTitle,column,t)))return end("TITLE_REBASE_OVERFLOW");
        }
        for(const auto &item:orders){const auto o=item.toObject();
            if(o["kind"]=="CAPTURE" && xy(o["end"])==town)return end("PENDING_PURCHASED_CAPTURE_OF_SAME_TOWN");}
        const auto title=townAssaultLedger(titled,column,noTitle,cutoff,timing);if(!title)return end("DATED_INCOME_UNPRICED");
        out["withheldFireFinish"]=titled.finishTick;out["withheldFireTitleOnly"]=static_cast<qint64>(title->score);

        if(state.shots.empty())return end("NO_SHOT_WITNESSED_BY_POST_POLICY");
        const auto &shot=state.shots.front();if(shot.gun!=n || shot.shotTick!=fire)return end("UNMATCHED_FUNDED_EXCHANGE");
        out["firingTick"]=fire;out["firingTile"]=point(shot.from);out["claimantHpBefore"]=before;out["claimantHpAfter"]=shot.targetAfter;
        out["gunHpAfter"]=shot.shooterAfter;out["roundTripFuel"]=shot.deployment.fuel;out["roundsRequired"]=1;
        out["hypothesis"]="PURCHASE_SURVIVES_AT_DECLARED_POST_WITH_ONE_VERIFIED_SHOT";
        if(shot.targetAfter>0 || shot.shooterAfter<=0)return end("SURVIVING_PARTICIPANT_REQUIRES_SUFFIX");
        const int back=shot.deployment.from==INVALID_TILE?fire:fire+2;
        const int rejoin=std::min(cutoff,order["impactHalfTurn"].toInt(-1));
        const int fuel=reserve.fuelAtPost<0?-1:reserve.fuelAtPost-shot.deployment.fuel;
        if(post!=original.destination)
        {
            if(rejoin<back+2)return end("OTHER_DEPLOYMENT_CANNOT_BE_REJOINED_BY_ITS_DATED_ACTION");
            const auto restore=stockCaptureRoute(grid,calendar,id,side,post,original.destination,type.movement,fuel,back+2,rejoin,rejoin,1,work,INVALID_TILE,&lower,{},type.ignoresUnitCollision);
            if(!restore)return end("RETURN_TO_ORIGINAL_POST_WORK_LIMIT");
            if(restore->entryTick!=rejoin)return end("ORIGINAL_POST_NOT_RECOVERED_BY_ITS_DEADLINE");
            out["rejoinedTick"]=rejoin;out["rejoinFuel"]=restore->fuel;
        }
        out["lethalFireTitleOnly"]=0;
        out["otherEffects"]="POST_EXPOSURE_AND_ALTERNATIVE_JOBS_UNRESOLVED";
        return end("CONDITIONAL_POST_SHOT_AND_RECOVERY_WITNESSED");
    }
}
