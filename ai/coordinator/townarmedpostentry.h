#pragma once

// Included after all town-assault value types have been declared.
namespace Coordinator
{
    namespace ArmedTownDetail
    {
        struct Roster
        {
            std::vector<TilePoint> positions;
            std::vector<double> health;
            std::vector<std::array<int,2>> rounds;
            std::vector<int> fuel;
            std::vector<bool> known;
        };
        struct Field {std::vector<int> cost,parent;};
        inline QJsonArray point(TilePoint p){return {p.x,p.y};}
        inline int cell(const CombatTerrain &t,TilePoint p){return p.y*t.width+p.x;}
        inline bool inside(const CombatTerrain &t,TilePoint p){return p.x>=0 && p.y>=0 && p.x<t.width && p.y<t.height;}
        inline bool useRound(std::array<int,2> &r,int w){if(w<0||w>1||r[w]==0)return false;if(r[w]>0)--r[w];return true;}
        inline bool better(int side,MilliFunds a,MilliFunds b){return side?a<b:a>b;}
        inline void openReason(TownArmedMenu &o,const QString &reason,int who=NO_UNIT,TilePoint at=INVALID_TILE,int tick=-1)
        {
            QJsonObject why{{"reason",reason}};if(who!=NO_UNIT)why["unit"]=who;if(at!=INVALID_TILE)why["at"]=point(at);if(tick>=0)why["tick"]=tick;
            if(!o.open.contains(why))o.open.append(why);
        }
        // An actor can be partway through an already purchased or captured
        // itinerary. Its eventual destination does not identify its post today.
        inline bool recoverDatedPosts(const CombatTerrain &t,const StockCaptureTraffic &traffic,
            Roster &state,int tick,std::int64_t &work,TownArmedMenu &out)
        {
            const int n=static_cast<int>(state.positions.size());
            std::vector<bool> missing(n),ambiguous(n);
            bool any=false;
            for(int i=0;i<n;++i)if(state.health[i]>0 && !inside(t,state.positions[i]))
            {
                missing[i]=true;
                any=true;
            }
            if(!any)return true;
            std::vector<TilePoint> post(n,INVALID_TILE);
            for(int sq=0;sq<traffic.cellCount();++sq)
            {
                const auto &uses=traffic.uses(sq);
                const auto cost=1+static_cast<std::int64_t>(uses.size());
                if(work<cost){openReason(out,"DATED_POSITION_WORK_LIMIT",NO_UNIT,INVALID_TILE,tick);return false;}
                work-=cost;
                for(const auto &use:uses)
                {
                    const int i=use.actor;
                    if(i<0 || i>=n || !missing[i] || !use.stopped || tick<use.first || tick>use.last)continue;
                    const TilePoint tile{sq%t.width,sq/t.width};
                    if(use.side!=int(t.units[i].enemy) || (post[i]!=INVALID_TILE && post[i]!=tile))ambiguous[i]=true;
                    post[i]=tile;
                }
            }
            bool complete=true;
            for(int i=0;i<n;++i)if(missing[i])
            {
                if(ambiguous[i] || post[i]==INVALID_TILE)
                {
                    complete=false;
                    openReason(out,ambiguous[i]?"CONFLICTING_DATED_POSTS":"UNDATED_TARGET_POSITION",t.units[i].engineUnitId,INVALID_TILE,tick);
                }
                else state.positions[i]=post[i];
            }
            return complete;
        }
        inline Field routes(const TownAssaultBoard &b,const StockCaptureTraffic &traffic,const Roster &s,int actor,int tick,std::int64_t &work,bool shooting=true)
        {
            Field f;const auto &t=b.terrain;const int n=static_cast<int>(s.positions.size());const auto start=s.positions[actor];
            if(!inside(t,start)||t.width<=0||t.height<=0||t.width>std::numeric_limits<int>::max()/t.height)return f;
            const int area=t.width*t.height;const std::int64_t setup=3LL*area+n;if(work<setup){work=0;return f;}work-=setup;
            f.cost.assign(area,-1);f.parent.assign(area,-1);const int from=cell(t,start);f.cost[from]=0;
            const auto &g=t.units[actor];const int gridId=shooting||g.grid>=0?g.grid:g.travelGrid;
            if(gridId<0 || (shooting && !b.units[actor].canMoveAndFire) || !s.known[actor] || b.rights[actor].mobileFrom>tick)return f;
            if(gridId>=static_cast<int>(t.grids.size()))return {};const auto &grid=t.grids[gridId];if(grid.width()!=t.width||grid.height()!=t.height)return {};
            std::vector<bool> enemy(area,false);
            for(int other=0;other<n;++other)if(s.health[other]>0&&t.units[other].enemy!=g.enemy&&inside(t,s.positions[other]))enemy[cell(t,s.positions[other])]=true;
            const int movement=s.fuel[actor]<0?g.movement:std::min(s.fuel[actor],g.movement);
            using Visit=std::pair<int,int>;std::priority_queue<Visit,std::vector<Visit>,std::greater<Visit>> queue;queue.emplace(0,from);
            while(!queue.empty()){
                if(work<5){work=0;return {};}work-=5;const auto [cost,sq]=queue.top();queue.pop();if(f.cost[sq]!=cost)continue;
                const TilePoint p{sq%t.width,sq/t.width};for(auto dir:ENTRY_DIRECTIONS){const auto d=entryStep(dir);const TilePoint q{p.x+d.dx,p.y+d.dy};
                    if(!inside(t,q))continue;const auto w=grid.cost(q.x,q.y,dir);if(w<0 || w>movement-cost)continue;const int dest=cell(t,q);
                    if((f.cost[dest]>=0&&f.cost[dest]<=cost+w)||(!g.ignoresCollision&&(enemy[dest]||!traffic.open(actor,int(g.enemy),dest,tick,false))))continue;
                    f.cost[dest]=cost+w;f.parent[dest]=sq;queue.emplace(f.cost[dest],dest);
                }
            }
            return f;
        }
        inline TownAssaultMove motion(const TownAssaultBoard &b,const Roster &s,int actor,TilePoint p,int tick,const Field &f)
        {
            StockCapturePath path;path.entryTick=tick;const int from=cell(b.terrain,s.positions[actor]),to=cell(b.terrain,p);path.fuel=f.cost[to];std::vector<int> chain;
            for(int x=to;x!=from && x>=0;x=f.parent[x])chain.push_back(x);std::reverse(chain.begin(),chain.end());path.steps={{tick,p,std::move(chain)}};
            return {actor,int(b.terrain.units[actor].enemy),tick,tick,s.positions[actor],p,std::move(path)};
        }
        inline bool occupied(const Roster &s,int actor,TilePoint p){for(int i=0;i<static_cast<int>(s.positions.size());++i)if(i!=actor&&s.health[i]>0&&s.positions[i]==p)return true;return false;}
        inline std::optional<TownAssaultHit> shoot(const TownAssaultBoard &b,Roster &s,int gun,int victim,TilePoint from,int at,std::int64_t &work)
        {
            const auto &t=b.terrain;const auto to=s.positions[victim];const int dist=manhattanDistance(from,to);
            if(!s.known[gun] || !s.known[victim] || s.health[gun]<=0 || s.health[victim]<=0 || gun==victim ||
               t.units[gun].enemy==t.units[victim].enemy || !inside(t,from) || !inside(t,to) || dist<b.units[gun].minRange || dist>b.units[gun].maxRange ||
               t.units[gun].engineUnitId<0 || t.units[victim].engineUnitId<0 || !t.production || !t.production->contactDamage)return {};
            const NativeContactPiece a{gun,int(t.units[gun].enemy),{},from,int(std::lround(10*s.health[gun])),s.rounds[gun]};
            const NativeContactPiece v{victim,int(t.units[victim].enemy),{},to,int(std::lround(10*s.health[victim])),s.rounds[victim]};
            auto q=t.production->contactDamage(a,v,work);if(!q||!std::isfinite(q->attack)||!std::isfinite(q->counter)||q->attack<0)return {};
            auto al=a.ammo,vl=v.ammo;if(!useRound(al,q->weapon)||(q->counter>=0&&!useRound(vl,q->counterWeapon)))return {};
            auto gAfter=engineHpAfterReply(s.health[gun],engineShotDamage(std::max(0.,q->counter)));
            auto tAfter=engineHpAfterReply(s.health[victim],engineShotDamage(q->attack));
            TownAssaultHit hit{gun,victim,at,q->weapon,q->counterWeapon,from,to,s.health[gun],gAfter,s.health[victim],tAfter,
                combatCapitalLoss(t.units[victim].replacementCost,s.health[victim],tAfter),combatCapitalLoss(t.units[gun].replacementCost,s.health[gun],gAfter)};
            s.rounds[gun]=al;s.rounds[victim]=vl;s.health[gun]=gAfter;s.health[victim]=tAfter;return hit;
        }
        inline void append(TownAssaultLine &line,const TownAssaultHit &hit,int side)
        {line.shots.push_back(hit);line.losses.emplace_back(hit.tick,(side?1:-1)*(hit.targetLoss-hit.counterLoss));}
        inline QJsonObject describe(const TownAssaultBoard &b,const TownAssaultHit &hit)
        {return {{"gun",b.terrain.units[hit.actor].engineUnitId},{"target",b.terrain.units[hit.target].engineUnitId},{"tick",hit.tick},{"from",point(hit.from)},
            {"to",point(hit.to)},{"weapon",hit.weapon},{"counterWeapon",hit.counterWeapon},{"shooterBefore",hit.actorBefore},{"shooterAfter",hit.actorAfter},
            {"targetBefore",hit.targetBefore},{"targetAfter",hit.targetAfter},{"targetCapitalLost",static_cast<qint64>(hit.targetLoss)},{"shooterCapitalLost",static_cast<qint64>(hit.counterLoss)}};}

        // Answer a particular revealed WAIT using a gun already at its firing
        // post. The other actors retain their orders. A join, capture, service
        // or teammate intervention is a separate continuation.
        inline std::optional<MilliFunds> retainedWithdrawalShot(const TownAssaultBoard &b,const Roster &state,
            const StockCaptureTraffic &traffic,const TownAssaultLine &withdrawal,const TownAssaultValue &value,
            int cap,int when,QJsonObject &receipt,std::int64_t &work)
        {
            if(!b.canRemove || !b.canRemove(cap,when) || when>b.cutoff)return {};
            const auto &terrain=b.terrain;
            std::optional<MilliFunds> best;
            for(int gun=0;gun<static_cast<int>(state.positions.size());++gun)
            {
                if(work<=0)break;
                --work;
                const auto from=state.positions[gun];
                const auto &right=b.rights[gun];
                if(terrain.units[gun].enemy==terrain.units[cap].enemy || state.health[gun]<=0 ||
                   !state.known[gun] || !inside(terrain,from) || terrain.units[gun].needsNativeTownExchange ||
                   right.readyTick<0 || right.readyTick>when || right.lastFreeTick<b.cutoff || right.displacedValue ||
                   !b.at || b.at(gun,when-1)==INVALID_TILE || b.at(gun,when-1)!=b.at(gun,when) ||
                   !traffic.open(gun,int(terrain.units[gun].enemy),cell(terrain,from),when,true))continue;
                const int distance=manhattanDistance(from,state.positions[cap]);
                if(distance<b.units[gun].minRange || distance>b.units[gun].maxRange)continue;
                if(b.conditionalClaims)
                {
                    const auto duty=b.conditionalClaims->find(gun);
                    if(duty!=b.conditionalClaims->end() && std::any_of(duty->second.begin(),duty->second.end(),
                        [&](const auto &claim){return claim.claimant!=cap && claim.through>=when;}))continue;
                }
                bool booked=false;
                for(int sq=0;sq<traffic.cellCount() && !booked;++sq)
                {
                    const auto &uses=traffic.uses(sq);
                    const auto cost=1+static_cast<std::int64_t>(uses.size());
                    if(work<cost){work=0;return best;}
                    work-=cost;
                    for(const auto &use:uses)if(use.actor==gun && use.first<=when && use.last>=when &&
                        (!use.stopped || (use.first==when && sq!=cell(terrain,from)))){booked=true;break;}
                }
                if(booked)continue;
                auto replay=state;
                const auto shot=shoot(b,replay,gun,cap,from,when,work);
                if(!shot || shot->targetAfter>0 || shot->actorAfter!=shot->actorBefore || shot->counterLoss!=0)continue;
                auto line=withdrawal;
                append(line,*shot,1-line.side);
                line.capperHp=0;
                const auto price=value(line);
                if(price && (!best || better(1-line.side,*price,*best)))
                {
                    best=*price;
                    receipt["lethalStandingReply"]=describe(b,*shot);
                    receipt["retainedReplyScore"]=static_cast<qint64>(*price);
                    receipt["defenderAmmoAfter"]=QJsonArray{replay.rounds[gun][0],replay.rounds[gun][1]};
                }
            }
            return best;
        }
    }

    inline TownArmedMenu priceTownArmedPostEntry(const TownAssaultBoard &b,const TownAssaultLine &attack,
        const TownAssaultLine &counter,int tick,const TownAssaultValue &value,std::int64_t &work)
    {
        using namespace ArmedTownDetail;TownArmedMenu out;const int cap=attack.capper,side=attack.side,n=static_cast<int>(b.units.size());
        if(n!=int(b.terrain.units.size())||n!=int(b.load.size())||n!=int(b.positions.size())||n!=int(b.hp.size())||cap<0||cap>=n || tick!=attack.entryTick+2 || tick>b.cutoff || attack.blocker!=NO_UNIT || !attack.shots.empty() || attack.moves.size()!=1 ||
           !b.terrain.production || !b.terrain.production->contactDamage || int(b.rights.size())!=n || !b.at || cap>=int(b.hp.size()) ||
           b.rights[cap].readyTick<0||b.rights[cap].readyTick>tick||b.rights[cap].lastFreeTick<tick || b.terrain.units[cap].needsNativeTownExchange)return out;
        out.attempted=true;
        const auto &t=b.terrain;if(t.width<=0||t.height<=0||t.width>std::numeric_limits<int>::max()/t.height)return out;const int area=t.width*t.height;
        if(work<area+8LL*n){openReason(out,"POST_ENTRY_STATE_WORK",NO_UNIT,attack.town,tick);return out;}work-=area+8LL*n;Roster state{{b.positions.begin(),b.positions.end()},{b.hp.begin(),b.hp.end()},
            {b.load.begin(),b.load.end()},std::vector<int>(n),std::vector<bool>(n,false)};
        auto traffic=b.traffic;auto prefix=counter;prefix.finishTick=-1;
        std::erase_if(prefix.losses,[&](const auto &l){return l.first>=tick;});std::erase_if(prefix.installments,[&](const auto &s){return s.first>=tick;});
        std::erase_if(prefix.shots,[&](const auto &s){return s.tick>=tick;});std::erase_if(prefix.moves,[&](const auto &m){return m.actionTick>=tick;});
        if(std::any_of(counter.moves.begin(),counter.moves.end(),[&](const auto &m){return m.fromTick<tick && m.actionTick>=tick;})){
            openReason(out,"CROSSING_BOOKED_MOVE",NO_UNIT,attack.town,tick);return out;}
        for(int i=0;i<n;++i){state.fuel[i]=b.rights[i].fuel;state.known[i]=state.health[i]>0 && b.rights[i].readyTick>=0 && b.rights[i].readyTick<=tick;state.positions[i]=b.at?b.at(i,tick):b.positions[i];}
        std::vector<TilePoint> lastStop(n,INVALID_TILE);
        for(const auto &m:prefix.moves){if(m.actor<0||m.actor>=n||m.side!=int(t.units[m.actor].enemy)||!inside(t,m.origin)||!inside(t,m.destination)||m.path.fuel<0)return out;
            if(lastStop[m.actor]!=INVALID_TILE&&lastStop[m.actor]!=m.origin)return out;
            lastStop[m.actor]=m.destination;
            for(const auto &st:m.path.steps)if(st.tile!=m.origin){traffic.vacate(m.actor,cell(t,m.origin),st.tick);break;}
            traffic.reserve(cell(t,m.destination),m.actionTick,b.cutoff,m.side,m.actor,true);state.positions[m.actor]=m.destination;state.known[m.actor]=true;
            if(state.fuel[m.actor]>=0){if(m.path.fuel>state.fuel[m.actor])return out;state.fuel[m.actor]-=m.path.fuel;}}
        for(const auto &s:prefix.shots){if(s.actor<0||s.actor>=n||s.target<0||s.target>=n || std::abs(state.health[s.actor]-s.actorBefore)>.000001 ||
            std::abs(state.health[s.target]-s.targetBefore)>.000001 || !useRound(state.rounds[s.actor],s.weapon) || (s.counterWeapon>=0&&!useRound(state.rounds[s.target],s.counterWeapon)))return out;
            state.health[s.actor]=s.actorAfter;state.health[s.target]=s.targetAfter;if(!s.actorAfter)traffic.vacate(s.actor,cell(t,s.from),s.tick);if(!s.targetAfter)traffic.vacate(s.target,cell(t,s.to),s.tick);}
        if(!b.terrain.certifyRetainedWalkProfile || !b.terrain.certifyRetainedWalkProfile(cap,tick,work))
            openReason(out,"RETAINED_DATED_MOVEMENT_UNCERTIFIED",t.units[cap].engineUnitId,attack.town,tick);
        prefix.capperHp=state.health[cap];if(prefix.capperHp<=0||state.positions[cap]!=attack.town||!state.known[cap])return out;
        out.hp=prefix.capperHp;out.remainingFuel=state.fuel[cap];out.remainingAmmo=state.rounds[cap];out.tick=tick;
        if(!recoverDatedPosts(t,traffic,state,tick,work,out))return out;
        auto range=routes(b,traffic,state,cap,tick,work);if(range.cost.empty()){openReason(out,"POST_ENTRY_MOVEMENT_WORK",NO_UNIT,attack.town,tick);return out;}
        const auto withdraw=value(prefix);if(!withdraw){openReason(out,"POST_ENTRY_WITHDRAWAL_WALLET",NO_UNIT,attack.town,tick);return out;}
        out.best=*withdraw;out.withdraw=*withdraw;bool sameFullLoad=true;
        Field walking;
        if(!b.units[cap].canMoveAndFire){walking=routes(b,traffic,state,cap,tick,work,false);if(walking.cost.empty()){openReason(out,"WITHDRAWAL_MOVEMENT_WORK",t.units[cap].engineUnitId,attack.town,tick);return out;}}
        const auto &exitRange=b.units[cap].canMoveAndFire?range:walking;
        std::set<int> relevant;for(int sq=0;sq<area;++sq)if(range.cost[sq]>=0){const TilePoint f{sq%t.width,sq/t.width};if(occupied(state,cap,f)||!traffic.open(cap,side,sq,tick,true))continue;
            const int radius=std::min(b.units[cap].maxRange,t.width+t.height);
            for(int y=std::max(0,f.y-radius);y<std::min(t.height,f.y+radius+1);++y)for(int x=std::max(0,f.x-radius);x<std::min(t.width,f.x+radius+1);++x){
                if(work<=0){openReason(out,"POST_ENTRY_RANGE_WORK",NO_UNIT,f,tick);return out;}--work;const TilePoint p{x,y};const int d=manhattanDistance(f,p);if(d>=b.units[cap].minRange && d<=radius)relevant.insert(cell(t,p));}}
        // A body on an accepted paid route has its own HP, ammunition and action budget. Never score it using an unrelated stock index.
        for(int sq:relevant)for(const auto &use:traffic.uses(sq))if(use.actor>=n && use.first<=tick && tick<=use.last && use.stopped && use.side!=side)
            openReason(out,"PAID_TARGET_REQUIRES_DATED_MATCHUP",NO_UNIT,{sq%t.width,sq/t.width},tick);
        for(int sq=0;sq<area;++sq){if(range.cost[sq]<0)continue;const TilePoint f{sq%t.width,sq/t.width};if(occupied(state,cap,f)||!traffic.open(cap,side,sq,tick,true))continue;
            for(int j=0;j<n;++j){if(work<=0){openReason(out,"POST_ENTRY_WORK_LIMIT",NO_UNIT,f,tick);return out;}--work;
                if(state.health[j]<=0||int(t.units[j].enemy)==side||!inside(t,state.positions[j]))continue;const int d=manhattanDistance(f,state.positions[j]);
                if(d<b.units[cap].minRange||d>b.units[cap].maxRange)continue;
                if(!state.known[j] || t.units[j].needsNativeTownExchange){openReason(out,"UNPRICED_DATED_TARGET",t.units[j].engineUnitId,state.positions[j],tick);continue;}
                const auto w=supportWeapon(t,cap,j,d,state.rounds[cap]);const auto full=supportWeapon(t,cap,j,d,std::array<int,2>{-1,-1});
                if(full.base>w.base || (full.base>0 && full.slot!=w.slot))sameFullLoad=false;
                if(w.base<=0||w.slot<0)continue;
                auto branch=state;auto proposed=prefix;const auto step=motion(b,state,cap,f,tick,range);
                auto shot=shoot(b,branch,cap,j,f,tick,work);if(!shot){openReason(out,"STOCK_EXCHANGE_OR_COMMITMENT_UNRESOLVED",t.units[j].engineUnitId,f,tick);continue;}
                const bool displaced=(shot->actorAfter<=0 && b.canRemove && !b.canRemove(cap,tick)) ||
                    (shot->targetAfter<shot->targetBefore && b.canRemove && !b.canRemove(j,tick)) || b.rights[cap].displacedValue>0;
                if(displaced){auto observed=describe(b,*shot);observed["status"]="REPRICE_EXISTING_COMMITMENT";out.actions.append(observed);openReason(out,"ACTIVE_FIRE_STOCK_COMMITMENT",t.units[j].engineUnitId,f,tick);continue;}
                append(proposed,*shot,side);proposed.moves.push_back(step);proposed.capperHp=branch.health[cap];branch.positions[cap]=f;
                if(branch.fuel[cap]>=0)branch.fuel[cap]-=step.path.fuel;
                const auto base=value(proposed);if(!base){openReason(out,"CHANGED_WALLET",t.units[j].engineUnitId,f,tick);continue;}
                QJsonObject receipt=describe(b,*shot);QJsonArray cells;for(int v:step.path.steps.front().cells)cells.append(point({v%t.width,v/t.width}));receipt["path"]=cells;
                receipt["moveFuel"]=step.path.fuel;receipt["cashValueIfNoLaterAction"]=static_cast<qint64>(*base);receipt["carryAfterAction"]=branch.health[cap]>0 && f==attack.town&&!prefix.installments.empty()?prefix.installments.back().second:0;
                auto upper=*base;bool bounded=branch.health[cap]<=0 || tick==b.cutoff;
                receipt["ammoAfter"]=QJsonArray{branch.rounds[cap][0],branch.rounds[cap][1]};receipt["targetAmmoAfter"]=QJsonArray{branch.rounds[j][0],branch.rounds[j][1]};
                if(!bounded){
                    auto replyBoard=traffic;replyBoard.vacate(cap,cell(t,attack.town),tick);replyBoard.reserve(sq,tick,b.cutoff,side,cap,true);
                    if(branch.health[j]<=0)replyBoard.vacate(j,cell(t,state.positions[j]),tick);
                    for(int g=0;g<n && work>0;++g){if(int(t.units[g].enemy)==side||branch.health[g]<=0||!branch.known[g]||!inside(t,branch.positions[g])||
                            b.rights[g].readyTick<0||b.rights[g].readyTick>tick+1||b.rights[g].lastFreeTick<tick+1 || t.units[g].needsNativeTownExchange)continue;
                        // Calendar::open permits the querying unit to occupy its own future fields. Its retained orders must also leave the activation free.
                        const int when=tick+1;
                        if(!b.at || b.at(g,tick)==INVALID_TILE || b.at(g,tick)!=b.at(g,when))continue;
                        if(b.conditionalClaims){const auto duty=b.conditionalClaims->find(g);if(duty!=b.conditionalClaims->end() &&
                            std::any_of(duty->second.begin(),duty->second.end(),[&](const auto &c){return c.claimant!=cap && c.through>=when;}))continue;}
                        bool booked=false;
                        for(int c=0;c<area&&!booked;++c){const auto &uses=b.traffic.uses(c);if(work<1+std::int64_t(uses.size())){work=0;break;}work-=1+uses.size();
                            for(const auto &use:uses)if(use.actor==g && use.first<=when && when<=use.last && (!use.stopped || (use.first==when && c!=cell(t,b.at(g,tick))))){booked=true;break;}}
                        if(booked||work<=0)continue;
                        auto field=routes(b,replyBoard,branch,g,when,work);if(field.cost.empty())break;
                        for(int k=0;k<area && work>0;++k){if(field.cost[k]<0)continue;const TilePoint pos{k%t.width,k/t.width};const int distance=manhattanDistance(pos,f);
                            if(distance<b.units[g].minRange||distance>b.units[g].maxRange||occupied(branch,g,pos)||!replyBoard.open(g,1-side,k,tick+1,true))continue;
                            auto replied=branch;auto follow=shoot(b,replied,g,cap,pos,tick+1,work);if(!follow||follow->targetAfter>0 || (b.canRemove&&(!b.canRemove(cap,tick+1)||(follow->actorAfter<=0&&!b.canRemove(g,tick+1)))))continue;
                            auto answer=proposed;answer.moves.push_back(motion(b,branch,g,pos,tick+1,field));append(answer,*follow,1-side);answer.capperHp=0;
                            if(b.rights[g].displacedValue>0 && std::any_of(prefix.shots.begin(),prefix.shots.end(),[&](const auto &earlier){return earlier.actor==g;})){
                                openReason(out,"REPEATED_DEFENDER_DUTY_NEEDS_PRICE",t.units[g].engineUnitId,pos,tick+1);continue;}
                            if(b.rights[g].displacedValue>0)answer.losses.emplace_back(tick+1,(side?-1:1)*b.rights[g].displacedValue);
                            const auto price=value(answer);if(!price)continue;if(!bounded||better(1-side,*price,upper)){upper=*price;receipt["lethalNextReply"]=describe(b,*follow);receipt["nextReplyValue"]=static_cast<qint64>(*price);}bounded=true;
                        }
                    }
                }
                if(!bounded)openReason(out,"SURVIVING_FIRE_FOLLOWUP_UNRESOLVED",t.units[j].engineUnitId,f,tick+2);
                else if(better(side,upper,*out.best))out.best=upper;
                receipt["boundedByCompletedAction"]=bounded;out.actions.append(receipt);
            }
        }
        // A no-fire withdrawal differs from firing on the way out: the defender
        // keeps its HP and rounds. Replay the actual stop before testing a shot.
        if(tick<b.cutoff)
        {
            auto left=work/3;
            const auto offered=left;
            for(int sq=0;sq<area && left>0;++sq)
            {
                --left;
                const TilePoint end{sq%t.width,sq/t.width};
                if(end==attack.town || exitRange.cost[sq]<0 || occupied(state,cap,end) ||
                   !traffic.open(cap,side,sq,tick,true) || !traffic.open(cap,side,sq,tick+1,true))continue;
                const auto move=motion(b,state,cap,end,tick,exitRange);
                auto withdrawn=state;
                withdrawn.positions[cap]=end;
                if(withdrawn.fuel[cap]>=0)withdrawn.fuel[cap]-=move.path.fuel;
                auto line=prefix;
                line.moves.push_back(move);
                QJsonArray cells;
                for(int step:move.path.steps.front().cells)cells.append(point({step%t.width,step/t.width}));
                QJsonObject receipt{{"kind","WITHDRAW_WITHOUT_FIRING"},{"unit",t.units[cap].engineUnitId},
                    {"tick",tick},{"to",point(end)},{"path",cells},{"fuelAfter",withdrawn.fuel[cap]},
                    {"hp",withdrawn.health[cap]},{"scope","OTHER_ACTORS_RETAIN_DATED_ORDERS"}};
                retainedWithdrawalShot(b,withdrawn,traffic,line,value,cap,tick+1,receipt,left);
                out.jobs.append(receipt);
            }
            work-=offered-left;
        }
        if(b.withdrawalJob){const auto other=b.withdrawalJob(cap,attack.town,tick,state.health[cap],work);if(!other.reason.isEmpty())openReason(out,other.reason,other.actor,other.tile,other.tick);}
        else openReason(out,"WITHDRAWAL_JOB_LEDGER_UNAVAILABLE",t.units[cap].engineUnitId,attack.town,tick);
        // If the capper survives a later normal activation it can choose again; current-action evidence alone is not a terminal value.
        double later=state.health[cap];for(const auto &s:counter.shots)if(s.tick<=tick+1&&s.target==cap)later=s.targetAfter;
        if(later>0&&tick+2<=b.cutoff&&(counter.finishTick<0||counter.finishTick>tick+1))openReason(out,"LATER_CAP_OR_OTHER_ACTION",t.units[cap].engineUnitId,attack.town,tick+2);
        // Check specific alternatives available to a survivor that leaves the property. A live but distant unit is not an automatic new fight.
        const int ownTurns=(b.cutoff-tick)/2,rate=replyCaptureRate(b.units[cap].captureRate,b.units[cap].hpRounded-ceilHpSteps(state.health[cap]),false);
        out.jobs.append(QJsonObject{{"kind","FRESH_PROPERTY_WITHOUT_SUPPORT"},{"rate",rate},{"firstCompletionIfEnteredNow",rate>0?tick+2*(widenedCaptureTurns(0,rate,b.threshold)-1):-1},{"cutoff",b.cutoff}});
        for(const auto &p:b.postEntryProperties){if(work<1){openReason(out,"POST_ENTRY_JOBS_WORK",NO_UNIT,p.tile,tick);return out;}--work;
            const auto at=p.tile;if(!inside(t,at)||at==attack.town||exitRange.cost[cell(t,at)]<0||occupied(state,cap,at)||!traffic.open(cap,side,cell(t,at),tick,true))continue;
            const auto owner=productionOwnerAt(p,tick+1);
            const auto us=side?OwnerSign::Enemy:OwnerSign::Ours;
            if(owner!=us && rate>0){const auto finish=tick+2*(widenedCaptureTurns(0,rate,b.threshold)-1);
                out.jobs.append(QJsonObject{{"kind","UNASSISTED_CAPTURE_FROM_WITHDRAWAL"},{"site",point(at)},{"entry",tick},
                    {"moveFuel",exitRange.cost[cell(t,at)]},{"carryOnEntry",rate},{"finishWithUnchangedHp",static_cast<qint64>(finish)},
                    {"withinHorizon",finish<=b.cutoff}});
                if(finish<=b.cutoff)openReason(out,"ALTERNATE_OWNERSHIP_AND_DEFENSE_NEED_PRICE",t.units[cap].engineUnitId,at,tick);
            }
            if(ownTurns>0 && owner==(side?OwnerSign::Enemy:OwnerSign::Ours)&&state.health[cap]<10 &&
                    (!t.canRepairOnTerrain||t.canRepairOnTerrain(cap,at))){
                openReason(out,"WITHDRAW_TO_POTENTIAL_REPAIR",t.units[cap].engineUnitId,at,tick+2);
                out.jobs.append(QJsonObject{{"kind","REPAIR_AFTER_WITHDRAWAL"},{"site",point(at)},{"moveFuel",exitRange.cost[cell(t,at)]},{"earliestService",tick+2}});
            }
        }
        if(b.postEntryProperties.empty() && ownTurns>0)openReason(out,"DATED_PROPERTY_SERVICE_LEDGER_UNAVAILABLE",NO_UNIT,attack.town,tick);
        for(int j=0;j<n;++j){if(j==cap||state.health[j]<=0||!inside(t,state.positions[j]))continue;const auto at=state.positions[j];
            if(int(t.units[j].enemy)==side){if(exitRange.cost[cell(t,at)]>=0 && (t.units[j].transportCapacity ||
                    (b.units[j].unitId==b.units[cap].unitId && ceilHpSteps(state.health[j])<10)))openReason(out,
                    t.units[j].transportCapacity?"OPTIONAL_TRANSPORT":"OPTIONAL_JOIN",t.units[j].engineUnitId,at,tick);
                if(ownTurns>0 && (t.units[j].transportCapacity||t.units[j].supportsUnits||b.units[j].unitId==b.units[cap].unitId))
                    openReason(out,"FUTURE_FRIENDLY_SUPPORT_AFTER_WITHDRAWAL",t.units[j].engineUnitId,at,tick+2);
            }else if(ownTurns>0){
                // An optimistic transit allowance is a relevance screen, never an awarded future shot. Zero-cost or unknown terrain keeps the candidate open.
                const auto cur=std::max(t.units[cap].movement,t.units[cap].movementBeforeFuel),other=std::max(t.units[j].movement,t.units[j].movementBeforeFuel);
                const auto d=manhattanDistance(attack.town,at);const auto minCost=[&](int id){const int k=t.units[id].grid;return k>=0&&k<int(t.grids.size())&&t.movementCache&&t.movementCache->minimumEntryCost.contains(&t.grids[k])?t.movementCache->minimumEntryCost.at(&t.grids[k]):0;};
                const int mc=minCost(cap),mj=minCost(j);if(mc<=0||mj<=0||d<=(1+ownTurns)*cur/mc+ownTurns*other/mj+b.units[cap].maxRange)
                    openReason(out,"FUTURE_STOCK_TARGET_AFTER_WITHDRAWAL",t.units[j].engineUnitId,at,tick+2);
            }
        }
        if(ownTurns>0)for(int k=0;k<int(b.recruits.size());++k){const auto &g=b.recruits[k];if(!g.unit||g.buildTick>b.cutoff)continue;
            const bool same=g.side==side;
            if(same && !g.unit->transportCapacity && !g.unit->supportsUnits && g.unit->type!=b.units[cap].unitId)continue;
            const int idx=t.units[cap].grid;int cm=0;if(idx>=0 && idx<int(t.grids.size()) && t.movementCache && t.movementCache->minimumEntryCost.contains(&t.grids[idx]))cm=t.movementCache->minimumEntryCost.at(&t.grids[idx]);
            const int native=std::max(t.units[cap].movement,t.units[cap].movementBeforeFuel),other=std::max(0,g.unit->movement);
            const int dates=std::max(0,(b.cutoff-g.buildTick)/2),sep=std::min(manhattanDistance(attack.town,g.factory),manhattanDistance(attack.town,g.destination));
            if(cm>0&&g.unit->minimumMoveCost>0&&sep>(ownTurns+1)*native/cm+dates*other/g.unit->minimumMoveCost+b.units[cap].maxRange)continue;
            // An unbooked protected recruit is not free material. Keep its named birth, post and readiness if it may gain or receive an action.
            out.jobs.append(QJsonObject{{"kind",same?"PAID_SUPPORT_RETAINS_OWN_ORDERS":"PAID_TARGET_RETAINS_OWN_ORDERS"},{"type",g.unit->type},{"birth",point(g.factory)},{"post",point(g.destination)},{"ready",g.readyTick}});
            openReason(out,same?"FUTURE_PAID_SUPPORT_AFTER_WITHDRAWAL":"FUTURE_PAID_TARGET_AFTER_WITHDRAWAL",NO_UNIT,g.factory,std::max(tick+2,g.readyTick));}
        // Arrival refutations are reused across all paths to this town/date. A particular path's remaining fuel or empty slot cannot exclude a better approach.
        const bool enoughFuel=state.fuel[cap]<0 || (t.units[cap].movementBeforeFuel>=0 && state.fuel[cap]>=t.units[cap].movementBeforeFuel && t.units[cap].movement==t.units[cap].movementBeforeFuel);
        out.allArrivalResources=tick==b.cutoff && enoughFuel && sameFullLoad;
        if(work<=0)openReason(out,"POST_ENTRY_WORK_LIMIT",NO_UNIT,attack.town,tick);out.enumerated=work>0;return out;
    }
}
