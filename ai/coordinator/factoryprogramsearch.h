#pragma once

#include "ai/coordinator/factoryprogrameconomy.h"
#include "ai/coordinator/factorybuyerevidence.h"
#include "ai/coordinator/bundlebuilder.h"
#include "ai/coordinator/mobilityfield.h"
#include "ai/coordinator/propertyeconomics.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <queue>
#include <set>

namespace Coordinator::FactoryPrograms
{
    struct Profile
    {
        QString type;
        int owner{},movement{},initialFuel{};
        const MobilityCostGrid *grid{};
        bool canCapture{},ignoresCollision{};
        std::vector<int> rate;
        std::vector<bool> refills;
        int minRange{},maxRange{};
        bool armed{},mobileFire{};
    };
    struct Body
    {
        int id{},profile{},fuel{},carry{},ready{},job{-1},birthSite{-1},birthTick{-1};
        TilePoint at{INVALID_TILE};
    };
    struct Site
    {
        TilePoint at{INVALID_TILE};
        bool capturable{},headquarters{};
        std::array<std::map<QString,std::pair<int,MilliFunds>>,2> menu;
    };
    struct World
    {
        std::vector<Body> bodies;
        std::vector<int> owners;
        std::array<MilliFunds,2> funds{};
        std::array<int,2> remainingSlots{};
        int nextId{};
        QJsonArray buyerState;
    };
    struct Board
    {
        FactoryEconomy::Board economics;
        std::vector<Profile> types;
        std::vector<Site> sites;
        std::vector<int> cellSite;
        std::map<std::pair<int,int>,int> siteIds;
        int width{},height{},threshold{},day{},player{};
        // The shared opposing programme purchases the selected ordinary
        // forecast portfolio only if it remains physically and financially
        // legal. An unrepresented purchase opportunity is an economic gap.
        std::map<int,QJsonArray> opposingPurchases;
        bool opposingSearchLimited{};
    };
    struct Opening
    {
        QString identity;
        QJsonObject physicalTeam,firstProjection,firstBuyer;
        World afterOrders;
        std::vector<FactoryEconomy::Capture> completed;
    };
    struct Strategy
    {
        int actor{-1},birthSite{-1},factory{-1};
        bool operator==(const Strategy &)const=default;
    };
    struct Simulation
    {
        FactoryEconomy::Program program;
        QJsonArray evidence;
        int firstClaimant{-1},firstFactoryStep{-1};
    };
    inline bool debit(std::int64_t &work,std::int64_t cost)
    {
        if(cost<0 || work<cost)return false;
        work-=cost;return true;
    }
    inline QJsonArray point(TilePoint t){return {t.x,t.y};}
    inline int cell(const Board &b,TilePoint t){return t.y*b.width+t.x;}
    inline TilePoint tile(const Board &b,int n){return {n%b.width,n/b.width};}
    inline bool within(const Board &b,TilePoint t){return t.x>=0 && t.y>=0 && t.x<b.width && t.y<b.height;}
    inline QString digest(const QJsonObject &v)
    {
        return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(v).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
    }
    inline QJsonObject snapshot(const Board &b,const World &w)
    {
        QJsonArray units,properties,wallet;
        for(const auto &u:w.bodies){
            const auto &t=b.types[u.profile];
            units.append(QJsonObject{{"id",u.id},{"owner",t.owner?1-b.player:b.player},{"type",t.type},
                {"x",u.at.x},{"y",u.at.y},{"hp",10},{"capPoints",u.carry},{"fuel",u.fuel}});
        }
        for(int i=0;i<int(b.sites.size());++i){
            const auto at=b.sites[i].at;const int owner=w.owners[i];
            properties.append(QJsonObject{{"x",at.x},{"y",at.y},{"owner",owner<0?-1:owner?1-b.player:b.player}});
        }
        for(auto funds:w.funds)wallet.append(qint64(funds));
        return {{"units",units},{"properties",properties},{"walletsMilli",wallet},{"remainingSlots",QJsonArray{w.remainingSlots[0],w.remainingSlots[1]}}};
    }
    struct Walk
    {
        int target{-1},turns{},fuel{};
        std::vector<TilePoint> current;
    };
    struct Travel
    {
        std::vector<int> cost,parent,occupant;
        bool complete{};
    };
    inline Travel travel(const Board &b,const World &w,const Body &u,std::int64_t &work)
    {
        Travel t;const auto &kind=b.types[u.profile];
        const int cells=b.width*b.height,origin=cell(b,u.at);
        if(!kind.grid || kind.movement<=0 || !debit(work,3LL*cells+w.bodies.size()))return t;
        t.cost.assign(cells,std::numeric_limits<int>::max());t.parent.assign(cells,-1);t.occupant.assign(cells,-1);
        for(const auto &other:w.bodies)t.occupant[cell(b,other.at)]=other.profile;
        using Entry=std::pair<int,int>;
        std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> open;
        t.cost[origin]=0;open.emplace(0,origin);
        while(!open.empty()){
            const auto [spent,n]=open.top();open.pop();
            if(!debit(work,5))return t;
            if(t.cost[n]!=spent)continue;
            const auto from=tile(b,n);
            for(const auto dir:ENTRY_DIRECTIONS){
                const auto delta=entryStep(dir);const TilePoint next{from.x+delta.dx,from.y+delta.dy};
                if(!within(b,next))continue;
                const int k=cell(b,next),occupier=t.occupant[k];
                if(!kind.ignoresCollision && occupier>=0 && b.types[occupier].owner!=kind.owner)continue;
                const int fee=kind.grid->cost(next.x,next.y,dir);
                if(fee<=0 || fee>kind.movement || fee>u.fuel-spent)continue;
                if(spent+fee<t.cost[k]){t.cost[k]=spent+fee;t.parent[k]=n;open.emplace(t.cost[k],k);}
            }
        }
        t.complete=true;return t;
    }
    inline std::optional<Walk> entrance(const Board &b,const Travel &t,const Body &u,int site,std::int64_t &work)
    {
        const auto &kind=b.types[u.profile];const auto goal=b.sites[site].at;
        if(u.at==goal)return Walk{site,1,0,{u.at}};
        int at=cell(b,goal);const int current=cell(b,u.at);
        if(t.parent[at]<0 || t.occupant[at]>=0)return {};
        std::vector<int> path;
        while(at!=current){if(at<0 || !debit(work,1))return {};path.push_back(at);at=t.parent[at];}
        std::reverse(path.begin(),path.end());
        Walk walk{site,1,0,{u.at}};int daily=0,stop=0;
        for(int n:path){
            const auto from=tile(b,t.parent[n]),next=tile(b,n);
            const int fee=kind.grid->cost(next.x,next.y,entryDirection(from.x,from.y,next.x,next.y));
            if(fee>kind.movement-daily){++walk.turns;daily=0;}
            daily+=fee;
            if(walk.turns==1){
                walk.current.push_back(next);
                if(t.occupant[n]<0){stop=int(walk.current.size())-1;walk.fuel=daily;}
            }
        }
        if(stop==0)return {};
        walk.current.resize(stop+1);return walk;
    }
    // A programme keeps jobs already in progress, then allocates remaining
    // capturers to dated income. Every order uses the current board, including
    // purchases and other movers. No route from one team is copied into another.
    inline QString moveSide(const Board &b,World &w,int tick,const Strategy &strategy,
        FactoryEconomy::Phase &phase,QJsonArray &orders,Simulation &output,std::int64_t &work)
    {
        const int side=tick%2;std::set<int> booked;
        for(auto &u:w.bodies){
            const auto &p=b.types[u.profile];
            if(p.owner!=side || u.ready>tick)continue;
            const int here=b.cellSite[cell(b,u.at)];
            if(here>=0 && w.owners[here]==side && p.refills[here])u.fuel=p.initialFuel;
            if(u.carry>0 && here>=0 && w.owners[here]!=side)u.job=here;
            if(u.job>=0 && w.owners[u.job]==side)u.job=-1;
            if(u.job>=0 && !booked.insert(u.job).second)return "DUPLICATE_ACTIVE_CAPTURE_JOB";
        }
        std::vector<int> order;
        for(int i=0;i<int(w.bodies.size());++i)if(b.types[w.bodies[i].profile].owner==side && w.bodies[i].ready<=tick)order.push_back(i);
        const auto founder=[&](const Body &u){return !side && (u.id==strategy.actor ||
            (strategy.birthSite>=0 && u.birthSite==strategy.birthSite && u.birthTick==0));};
        std::stable_sort(order.begin(),order.end(),[&](int x,int y){
            return std::tuple{!founder(w.bodies[x]),w.bodies[x].carry<=0,w.bodies[x].id}<
                   std::tuple{!founder(w.bodies[y]),w.bodies[y].carry<=0,w.bodies[y].id};});
        for(int i:order){
            auto &u=w.bodies[i];const auto &p=b.types[u.profile];const auto origin=u.at;
            QJsonArray path{point(origin)};QString action="ACTION_WAIT";
            if(p.canCapture){
                const auto routes=travel(b,w,u,work);
                if(!routes.complete)return "CAPTURE_ROUTE_WORK_OR_GEOMETRY_OPEN";
                const bool force=founder(u) && strategy.factory>=0 && w.owners[strategy.factory]!=side && u.carry==0;
                if(force && u.job>=0 && u.job!=strategy.factory){booked.erase(u.job);u.job=-1;}
                std::optional<Walk> best;std::tuple<int,MilliFunds,int> rank;
                const auto choose=[&](){
                for(int j=0;j<int(b.sites.size());++j){
                    if(!debit(work,1))return false;
                    const auto &s=b.sites[j];const int rate=p.rate[j];
                    if(!s.capturable || s.headquarters || w.owners[j]==side || rate<=0 ||
                       (force && j!=strategy.factory) || (u.job>=0 && u.job!=j) ||
                       (u.job!=j && booked.contains(j)))continue;
                    const auto wlk=entrance(b,routes,u,j,work);if(!wlk)continue;
                    const int carry=u.at==s.at?u.carry:0;
                    const int finish=wlk->turns+int(widenedCaptureTurns(carry,rate,b.threshold))-1;
                    if(tick+2LL*(finish-1)>2LL*b.economics.horizon-2)continue;
                    const auto k=std::tuple{finish,-b.economics.sites[j].income[side],j};
                    if(!best || k<rank){best=wlk;rank=k;}
                }
                return true;
                };
                if(!choose())return "PROGRAM_SEARCH_WORK_LIMIT";
                if(!best && u.job>=0 && !force && u.carry==0){
                    // An unrelated route can become blocked by either team's
                    // real movement or purchase. Reassign the unspent actor
                    // using the same current-state job rule on both arms.
                    booked.erase(u.job);u.job=-1;
                    if(!choose())return "PROGRAM_SEARCH_WORK_LIMIT";
                }
                if(best){
                    const int j=best->target;
                    if(u.job<0){u.job=j;booked.insert(j);}
                    u.at=best->current.back();u.fuel-=best->fuel;
                    if(u.at!=origin)u.carry=0;
                    path={};for(auto at:best->current)path.append(point(at));
                    if(u.at==b.sites[j].at){
                        action="ACTION_CAPTURE";u.carry+=p.rate[j];
                        if(founder(u) && j==strategy.factory && output.firstFactoryStep<0){
                            output.firstClaimant=u.id;output.firstFactoryStep=tick;
                        }
                        if(u.carry>=b.threshold){
                            phase.completed.push_back({u.id,j,w.owners[j]});
                            w.owners[j]=side;u.carry=0;u.job=-1;booked.erase(j);
                        }
                    }
                }else if(force){
                    // This concrete founder programme retains its commitment.
                    // A wait cannot produce title or fund a recruit; the ledger
                    // requires the subsequent physical capture and useful buy.
                    if(u.job<0 && !booked.contains(strategy.factory)){u.job=strategy.factory;booked.insert(u.job);}
                }
            }
            orders.append(QJsonObject{{"unit",u.id},{"action",action},{"from",point(origin)},{"to",point(u.at)},
                {"path",path},{"fuel",u.fuel},{"carry",u.carry}});
        }
        return {};
    }

    inline bool projectedIncome(const Board &b,const World &w,int side,MilliFunds &income)
    {
        income=b.economics.fixedIncome[side];
        for(int i=0;i<int(b.sites.size());++i)if(w.owners[i]==side &&
            !RelativeStockDetail::checkedAdd(income,b.economics.sites[i].income[side]))return false;
        return true;
    }
    inline quint32 seed(const Board &b,int tick)
    {
        QByteArray policy=QByteArray::fromStdString(b.economics.root)+"/ordinary-counterpoint/"+QByteArray::number(tick);
        const auto bytes=QCryptographicHash::hash(policy,QCryptographicHash::Sha256);
        quint32 n=0;for(int i=0;i<4;++i)n=(n<<8)|static_cast<unsigned char>(bytes[i]);return n;
    }
    inline std::optional<QJsonArray> unpricedFiringJobs(const Board &b,const World &w,int tick,std::int64_t &work)
    {
        QJsonArray jobs;
        for(const auto &u:w.bodies){
            const auto &kind=b.types[u.profile];if(kind.owner!=tick%2 || !kind.armed || u.ready>tick)continue;
            const auto reach=travel(b,w,u,work);
            if(!reach.complete)return {};
            for(const auto &target:w.bodies){
                const auto &victim=b.types[target.profile];if(victim.owner==kind.owner)continue;
                QJsonArray posts;
                for(int y=std::max(0,target.at.y-kind.maxRange);y<std::min(b.height,target.at.y+kind.maxRange+1);++y)
                    for(int x=std::max(0,target.at.x-kind.maxRange);x<std::min(b.width,target.at.x+kind.maxRange+1);++x){
                        if(!debit(work,1))return {};
                        const TilePoint at{x,y};const int range=manhattanDistance(at,target.at),n=cell(b,at);
                        if(range<kind.minRange || range>kind.maxRange || (at!=u.at && (!kind.mobileFire || reach.occupant[n]>=0)) ||
                           reach.cost[n]>std::min(kind.movement,u.fuel))continue;
                        posts.append(QJsonArray{x,y,reach.cost[n]});
                    }
                if(!posts.isEmpty())jobs.append(QJsonObject{{"shooter",u.id},{"type",kind.type},{"from",point(u.at)},
                    {"target",target.id},{"targetType",victim.type},{"at",point(target.at)},{"captureProgress",target.carry},
                    {"firingPosts",posts}});
            }
        }
        return jobs;
    }
    template<class Buyer>
    Simulation simulate(const Board &b,const Opening &opening,const Strategy &strategy,const QString &policy,
        Buyer &&buyer,std::int64_t &work)
    {
        Simulation out;auto &program=out.program;
        program.policy=policy.toStdString();program.opening=opening.identity.toStdString();
        program.strategy=QString("stock-income/founder-%1/birth-%2/factory-%3")
            .arg(strategy.actor).arg(strategy.birthSite).arg(strategy.factory).toStdString();
        const auto gap=[&](QString why){program.gap=why.toStdString();return std::move(out);};
        auto w=opening.afterOrders;
        std::string previous=b.economics.root;
        for(int t=0;t<=2*b.economics.horizon;++t){
            if(!debit(work,32+8LL*w.bodies.size()+8LL*b.sites.size()))return gap("FACTORY_PHASE_WORK_LIMIT");
            const int side=t%2,day=b.day+(t+b.player)/2;
            FactoryEconomy::Phase phase;phase.tick=t;phase.before=previous;
            MilliFunds income{};
            if(!projectedIncome(b,w,side,income) || (t && !RelativeStockDetail::checkedAdd(w.funds[side],income)))
                return gap("PAYDAY_ARITHMETIC");
            phase.startWallet=w.funds[side];QJsonArray orders,fire;
            const bool last=t==2*b.economics.horizon;
            if(!t){phase.completed=opening.completed;orders=opening.physicalTeam["orders"].toArray();}
            else if(!last){
                const auto options=unpricedFiringJobs(b,w,t,work);
                if(!options)return gap("FIRING_JOB_COVERAGE_WORK_LIMIT");
                fire=*options;
                const auto failure=moveSide(b,w,t,strategy,phase,orders,out,work);
                if(!failure.isEmpty())return gap(failure);
            }
            phase.orderReceipt=t?digest(QJsonObject{{"root",QString::fromStdString(previous)},{"tick",t},{"orders",orders}}).toStdString():
                opening.identity.toStdString();
            QJsonArray buys;QJsonObject buyerProof,projection;
            if(!last){
                for(const auto &u:w.bodies){
                    const int slot=b.cellSite[cell(b,u.at)];
                    if(slot>=0 && b.economics.sites[slot].production)phase.occupiedFactories.push_back(slot);
                }
                if(!side){
                    if(!t){buyerProof=opening.firstBuyer;projection=opening.firstProjection;}
                    else{
                        projection=snapshot(b,w);
                        if(w.funds[0]%MILLI_FUNDS_PER_FUND || w.funds[1]%MILLI_FUNDS_PER_FUND)
                            return gap("NONINTEGRAL_PROJECTED_NATIVE_WALLET");
                        projection["funds"]=qint64(w.funds[0]/MILLI_FUNDS_PER_FUND);
                        projection["opponentFunds"]=qint64(w.funds[1]/MILLI_FUNDS_PER_FUND);
                        projection["unitSlots"]=w.remainingSlots[0];projection["player"]=b.player;
                        projection["projectedDay"]=day;projection["futureDay"]=day;
                        projection["includeContinuationState"]=true;projection["previousBuyerState"]=w.buyerState;
                        projection["comparisonSeed"]=qint64(seed(b,t));
                        buyerProof=buyer(projection,policy,work);
                    }
                    if(!buyerProof["supported"].toBool() || !buyerProof["orders"].isArray() ||
                       !buyerProof["continuationState"].isArray())return gap(QString("ORDINARY_BUYER_T%1_%2").arg(t).arg(buyerProof["reason"].toString()));
                    w.buyerState=buyerProof["continuationState"].toArray();buys=buyerProof["orders"].toArray();
                    phase.buyer=t?FactoryEconomy::BuyerEvidence::PredeclaredOrdinaryPolicy:FactoryEconomy::BuyerEvidence::LiveReservation;
                    phase.buyerPolicy=((!t?QString("live-"):QString("conditional-"))+QString::number(!t?
                        buyerProof["seed"].toInteger():seed(b,t))).toStdString();
                    phase.buyerReceipt=digest(buyerProof).toStdString();
                }else{
                    const auto booked=b.opposingPurchases.find(t);
                    if(booked!=b.opposingPurchases.end())buys=booked->second;
                    phase.buyer=FactoryEconomy::BuyerEvidence::PredeclaredOrdinaryPolicy;
                    phase.buyerPolicy="common-opponent-ordinary-forecast";
                    QJsonArray openSites;
                    for(int j=0;j<int(b.sites.size());++j)if(w.owners[j]==1 && b.economics.sites[j].production &&
                        std::find(phase.occupiedFactories.begin(),phase.occupiedFactories.end(),j)==phase.occupiedFactories.end())
                        openSites.append(j);
                    const QJsonObject inputs{{"openSites",openSites},{"funds",qint64(w.funds[side])},{"slots",w.remainingSlots[side]}};
                    if(b.opposingSearchLimited)phase.buyerPolicy+="/"+digest(inputs).toStdString();
                    phase.buyerReceipt=digest(QJsonObject{{"policy",policy},{"tick",t},{"orders",buys},{"inputs",inputs}}).toStdString();
                    buyerProof=inputs;
                }
                std::set<int> unavailable(phase.occupiedFactories.begin(),phase.occupiedFactories.end());
                for(const auto &v:buys){
                    const auto o=v.toObject();const TilePoint at{o["x"].toInt(-1),o["y"].toInt(-1)};
                    if(!within(b,at))return gap("PURCHASE_OUTSIDE_BOARD");
                    const int site=b.cellSite[cell(b,at)];const auto type=o["type"].toString();
                    if(site<0 || !b.economics.sites[site].production || w.owners[site]!=side ||
                       !unavailable.insert(site).second || w.remainingSlots[side]<=0)
                        return gap(QString("PURCHASE_OCCUPANCY_OR_LIMIT_T%1_AT_%2_%3").arg(t).arg(at.x).arg(at.y));
                    const auto menu=b.sites[site].menu[side].find(type);const auto native=o["price"].toInteger(-1);
                    if(menu==b.sites[site].menu[side].end() || native<=0 || native>std::numeric_limits<MilliFunds>::max()/MILLI_FUNDS_PER_FUND ||
                       menu->second.second!=native*MILLI_FUNDS_PER_FUND || menu->second.second>w.funds[side])
                        return gap(QString("PRICE_OR_WALLET_T%1_%2").arg(t).arg(type));
                    const auto [profile,price]=menu->second;const auto &kind=b.types[profile];
                    const auto id=std::int64_t(w.nextId)+std::int64_t(t)*b.sites.size()+site;
                    if(id>=std::numeric_limits<int>::max() || kind.initialFuel<0)return gap("UNBOUND_RECRUIT_OR_ID");
                    Body born{int(id),profile,kind.initialFuel,0,t+2,-1,site,t,at};
                    --w.remainingSlots[side];w.funds[side]-=price;w.bodies.push_back(born);
                    // The native full-health recruit's capital at its birth
                    // location is Player::getCosts(type,spawn), the same
                    // per-site field authenticated against the ordinary quote.
                    phase.purchases.push_back({born.id,site,type.toStdString(),price,price,w.funds[side]});
                }
                if(!side && buyerProof["remainingFunds"].toInteger(-1)!=w.funds[0]/MILLI_FUNDS_PER_FUND)
                    return gap("NATIVE_BUYER_END_WALLET_MISMATCH");
            }
            phase.endWallet=w.funds[side];
            if(!side && !last){
                const quint32 draw=t?seed(b,t):quint32(opening.firstBuyer["seed"].toInteger());
                const auto refusal=bindFactoryProgramBuyer(phase,projection,policy,draw,buyerProof,b.siteIds);
                if(!refusal.isEmpty())return gap(QString("BUYER_BINDING_T%1_%2").arg(t).arg(refusal));
            }
            const auto state=snapshot(b,w);phase.after=digest(QJsonObject{{"state",state},{"tick",t},
                {"order",QString::fromStdString(phase.orderReceipt)},{"buyer",QString::fromStdString(phase.buyerReceipt)}}).toStdString();
            QJsonArray titles;
            for(const auto &claim:phase.completed)titles.append(QJsonObject{{"actor",claim.actor},{"site",claim.site},
                {"tile",point(b.sites[claim.site].at)},{"previousOwner",claim.previousOwner}});
            out.evidence.append(QJsonObject{{"tick",t},{"orders",orders},{"purchases",buys},{"completedTitles",titles},
                {"startWallet",qint64(phase.startWallet)},{"endWallet",qint64(phase.endWallet)},
                {"opposingPolicyInputs",side?buyerProof:QJsonObject{}},{"unpricedFire",fire},{"after",QString::fromStdString(phase.after)}});
            previous=phase.after;program.phases.push_back(std::move(phase));
        }
        return out;
    }
}
