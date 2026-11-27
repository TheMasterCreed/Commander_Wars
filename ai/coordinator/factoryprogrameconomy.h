#pragma once

#include "ai/coordinator/datedformulaledger.h"
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string>

namespace Coordinator
{
    namespace FactoryEconomy
    {
        // Monetary values use MilliFunds throughout. A site number indexes the frozen
        // map. Native movers carry an identity through purchase, deployment and capture.
        enum class BuyerEvidence { Missing, LiveReservation, PredeclaredOrdinaryPolicy, CertifiedEquivalent };
        enum class Status { Open, NoNetGain, StrictEconomicGain };
        struct Site
        {
            int owner{-1}; // -1 neutral, 0 friendly, 1 opponent
            bool production{};
            std::array<MilliFunds,2> income{};
        };
        struct Board
        {
            std::string root, procedure;
            int horizon{};
            std::array<MilliFunds,2> openingFunds{};
            std::vector<Site> sites;
            std::map<int,int> originalActors; // native id -> side, including spent actors
            // Cash from other sources must be included here only when fixed on this closure.
            std::array<MilliFunds,2> fixedIncome{};
        };
        struct Build
        {
            int actor{-1}, site{-1};
            std::string type;
            MilliFunds price{}, nativeInitialCapital{}, walletAfter{};
        };
        // An authoritative end of capture; the producer verifies every physical order,
        // movement, carry and arrival. Other shared assumptions are named by policy key.
        struct Capture { int actor{-1}, site{-1}, previousOwner{-1}; };
        struct Effect
        {
            // Non-property formula terms at their original nominal value and cash date.
            // A purchase principal is never an effect. E.g. a checked native exchange
            // after accounting for its foregone opportunity and return damage.
            int actor{-1};
            MilliFunds nominal{}, timed{};
            std::string receipt;
        };
        struct Phase
        {
            int tick{};
            std::string before, after, orderReceipt;
            BuyerEvidence buyer{BuyerEvidence::Missing};
            std::string buyerPolicy, buyerReceipt;
            MilliFunds startWallet{}, serviceCashDelta{}, endWallet{};
            std::vector<Capture> completed;
            std::vector<int> occupiedFactories;
            std::vector<Build> purchases;
            std::vector<Effect> nonProperty;
        };
        struct Program
        {
            // strategy names a single implementable choice rule across buyer draws.
            std::string opening, policy, strategy, gap;
            std::vector<Phase> phases; // F0 (already paid), E0, F1, ... Fh payday only.
            // A legal displaced asset with no priced action needs a source-specific
            // economic upper within this procedure (e.g. a frozen noncapturer in
            // an unreachable sector). A trajectory containing only WAIT is insufficient.
            std::map<int,std::string> verifiedZeroOtherEconomicUse;
        };
        struct Arm
        {
            std::string opening, root, procedure, gap;
            // The common policy cohort incorporates competing original actors, home
            // builds and replacement cappers. Its two actual orders can differ.
            std::map<std::string,std::vector<Program>> policies;
        };
        struct Decision
        {
            Status status{Status::Open};
            std::string reason;
            MilliFunds referenceValue{}, challengerValue{}, marginal{};
            int buyTick{-1}, useTick{-1}, recruit{-1};
            std::string governingPolicy;
        };
        struct Birth
        {
            Build order;
            int tick{}, side{};
        };
        struct Use
        {
            int actor{}, tick{}, site{-1};
            MilliFunds positiveEffect{};
        };
        struct Ledger
        {
            std::string gap;
            MilliFunds score{};
            std::vector<std::vector<int>> owners;
            std::vector<Birth> births;
            std::vector<Use> uses;
            std::vector<std::string> buyerPolicies;
            std::vector<std::vector<int>> unoccupiedFriendlyFactories;
        };

        inline Ledger price(const Board &board,const std::string &opening,const Program &program,std::int64_t &work)
        {
            using RelativeStockDetail::checkedAdd;
            using RelativeStockDetail::checkedSubtract;
            Ledger result;
            const auto fail=[&](const std::string &reason){result.gap=reason;return result;};
            const auto pay=[&](std::int64_t amount){if(amount<0 || work<amount)return false;work-=amount;return true;};
            if (!program.gap.empty()) return fail(program.gap);
            if (opening.empty() || program.opening!=opening || board.root.empty() || board.procedure.empty() ||
                board.horizon<=0 || board.horizon>std::numeric_limits<int>::max()/2 ||
                program.phases.size()!=2*std::size_t(board.horizon)+1)
                return fail("UNBOUND_OR_INCOMPLETE_ECONOMIC_PROGRAM");
            if (board.sites.size()>std::size_t(std::numeric_limits<int>::max()) || !pay(1+board.sites.size()))
                return fail("ECONOMIC_PROGRAM_WORK_LIMIT");
            const auto validSite=[&](int slot){return slot>=0 && slot<static_cast<int>(board.sites.size());};
            std::vector<int> owners;
            for (const auto &site:board.sites)
            {
                if(site.owner < -1 || site.owner>1 || std::min(site.income[0],site.income[1])<0)
                    return fail("INVALID_PROPERTY_OWNER_OR_RATE");
                owners.push_back(site.owner);
            }
            auto banks=board.openingFunds;
            if (std::min(banks[0],banks[1])<0 || std::min(board.fixedIncome[0],board.fixedIncome[1])<0)
                return fail("UNBOUND_STARTING_WALLET");
            std::vector<NativePropertyPayday> receipts;
            std::vector<MilliFunds> otherFlows(board.horizon);
            MilliFunds otherNominal=0;
            std::map<int,Birth> newborns;
            std::set<int> alreadyActed;
            std::string before=board.root;
            for (int t=0;t<=2*board.horizon;++t)
            {
                const auto &p=program.phases[t];
                if(!pay(12+2*std::int64_t(owners.size())+p.completed.size()+p.purchases.size()+
                        p.nonProperty.size()+p.occupiedFactories.size()))
                    return fail("ECONOMIC_PROGRAM_WORK_LIMIT");
                const int side=t%2;
                if(p.tick!=t || p.before!=before || p.after.empty() || p.orderReceipt.empty() ||
                   (t==0 && p.orderReceipt!=opening))
                    return fail("DISCONTINUOUS_NATIVE_TEAM_ORDERS");
                before=p.after;
                result.owners.push_back(owners);
                MilliFunds income=board.fixedIncome[side];
                for(int i=0;i<static_cast<int>(owners.size());++i)
                    if(owners[i]==side && !checkedAdd(income,board.sites[i].income[side]))
                        return fail("ECONOMIC_OVERFLOW");
                if(t)
                {
                    receipts.push_back({t,side==1,income});
                    if(!checkedAdd(banks[side],income)) return fail("ECONOMIC_OVERFLOW");
                }
                if(!checkedAdd(banks[side],p.serviceCashDelta) || banks[side]<0 || p.startWallet!=banks[side])
                    return fail("WALLET_OR_SERVICE_PRECONDITION");
                if(p.serviceCashDelta) return fail("SERVICE_CASH_AND_MATERIAL_NOT_SEPARATELY_COVERED");
                const bool last=t==2*board.horizon;
                if(last && (!p.purchases.empty() || !p.completed.empty() || !p.nonProperty.empty() || p.serviceCashDelta))
                    return fail("POST_HORIZON_ACTION");
                if(!last && (p.buyer==BuyerEvidence::Missing || p.buyerPolicy.empty() || p.buyerReceipt.empty() ||
                        (t==0 && p.buyer==BuyerEvidence::PredeclaredOrdinaryPolicy)))
                    return fail("ORDINARY_BUYER_OR_MATCHED_POLICY_MISSING");
                result.buyerPolicies.push_back(p.buyerPolicy);
                std::set<int> activated,changed;
                const auto onRoster=[&](int id){
                    const auto original=board.originalActors.find(id);
                    const auto born=newborns.find(id);
                    return (original!=board.originalActors.end() && original->second==side) ||
                        (born!=newborns.end() && born->second.side==side && born->second.tick+2<=t);
                };
                for(const auto &event:p.completed)
                {
                    if(!validSite(event.site) || event.actor<0 || !activated.insert(event.actor).second ||
                       !changed.insert(event.site).second || owners[event.site]!=event.previousOwner || owners[event.site]==side)
                        return fail("INCOMPATIBLE_OR_REPEATED_CAPTURE");
                    if(!onRoster(event.actor))return fail("UNBOUND_OR_PREMATURE_CAPTURER");
                    alreadyActed.insert(event.actor);
                    owners[event.site]=side;
                    result.uses.push_back({event.actor,t,event.site,0});
                }
                for(const auto &effect:p.nonProperty)
                {
                    if(effect.receipt.empty() || effect.actor<0 || !activated.insert(effect.actor).second)
                        return fail("UNBOUND_OR_CONFLICTING_ECONOMIC_ACTION");
                    if(!onRoster(effect.actor))return fail("UNBOUND_OR_PREMATURE_ECONOMIC_ACTOR");
                    if(!checkedAdd(otherNominal,effect.nominal) ||
                       !checkedAdd(otherFlows[std::max(0,(t-1)/2)],effect.timed)) return fail("ECONOMIC_OVERFLOW");
                    alreadyActed.insert(effect.actor);
                    if(effect.nominal>0) result.uses.push_back({effect.actor,t,-1,effect.nominal});
                }
                std::set<int> unavailable;
                for(int slot:p.occupiedFactories)
                    if(!validSite(slot) || !board.sites[slot].production || !unavailable.insert(slot).second)
                        return fail("UNBOUND_NATIVE_FACTORY_OCCUPANCY");
                result.unoccupiedFriendlyFactories.emplace_back();
                if(side==0)
                    for(int i=0;i<static_cast<int>(owners.size());++i)
                        if(board.sites[i].production && owners[i]==0 && !changed.contains(i) && !unavailable.contains(i))
                            result.unoccupiedFriendlyFactories.back().push_back(i);
                for(const auto &purchase:p.purchases)
                {
                    if(!validSite(purchase.site) || !board.sites[purchase.site].production ||
                       owners[purchase.site]!=side || !unavailable.insert(purchase.site).second ||
                       changed.contains(purchase.site)) return fail("FACTORY_UNOWNED_OR_OCCUPIED");
                    if(purchase.actor<0 || board.originalActors.contains(purchase.actor) ||
                       alreadyActed.contains(purchase.actor) || newborns.contains(purchase.actor) ||
                       purchase.type.empty() || purchase.price<=0 || purchase.nativeInitialCapital!=purchase.price)
                        return fail("PURCHASE_PRINCIPAL_OR_ASSET_UNBOUND");
                    if(banks[side]<purchase.price || !checkedSubtract(banks[side],purchase.price) ||
                       banks[side]!=purchase.walletAfter) return fail("UNFUNDED_OR_SHIFTED_PURCHASE");
                    Birth birth{purchase,t,side};
                    newborns.emplace(purchase.actor,birth); result.births.push_back(birth);
                }
                if(p.endWallet!=banks[side]) return fail("UNBOOKED_EXPENDITURE");
            }
            const auto schedule=nativePropertySchedule(board.horizon,2*board.horizon,receipts);
            if(!schedule) return fail("UNDATED_ECONOMIC_LEDGER");
            for(std::size_t i=0;i<otherFlows.size();++i)
                if(!checkedAdd(otherFlows[i],schedule->perRound[i])) return fail("ECONOMIC_OVERFLOW");
            const auto timing=cashTimingAdjustment(otherFlows);
            if(!timing || !checkedAdd(otherNominal,schedule->nominal) || !checkedAdd(otherNominal,*timing))
                return fail("ECONOMIC_OVERFLOW");
            result.score=otherNominal;
            return result;
        }

        inline bool productive(const Board &board,const Ledger &factory,const Ledger &alternative,
                               const Program &reference,int site,Decision &result,std::int64_t &work)
        {
            using Key=std::pair<std::string,MilliFunds>;
            for(const auto &born:factory.births)
            {
                if(work<=0)return false;
                --work;
                if(born.side || born.order.site!=site)
                    continue;
                bool referenceHasOpportunity=false;
                for(int t=0;t<=born.tick;t+=2)
                {
                    const auto &sites=alternative.unoccupiedFriendlyFactories[t];
                    if(work<1+std::int64_t(sites.size())){work=0;return false;}
                    work-=1+std::int64_t(sites.size());
                    if(std::binary_search(sites.begin(),sites.end(),site))referenceHasOpportunity=true;
                }
                if(referenceHasOpportunity)continue;
                int count=0,throughDate=0;
                std::map<Key,int> basket;
                const auto scan=std::int64_t(factory.births.size()+alternative.births.size());
                if(work<scan){work=0;return false;} work-=scan;
                for(const auto &b:factory.births)
                {
                    if(b.side || b.tick>born.tick)continue;
                    ++throughDate;
                    if(b.tick==born.tick){++count;++basket[{b.order.type,b.order.price}];}
                }
                std::map<Key,std::vector<int>> rival;
                for(const auto &b:alternative.births)
                {
                    if(b.side || b.tick>born.tick)continue;
                    --throughDate;
                    if(b.tick==born.tick){--count;rival[{b.order.type,b.order.price}].push_back(b.order.actor);}
                }
                if(count<=0 || throughDate<=0) continue; // A port shift or catching up an older buy creates no extra body.
                // Explicitly price work foregone by any missing type from the same-date portfolio.
                bool accounted=true;
                for(const auto &[key,ids]:rival)
                {
                    const int matched=std::min<int>(basket[key],ids.size());
                    if(matched==static_cast<int>(ids.size()))continue;
                    for(int id:ids)
                    {
                        if(work<1+std::int64_t(alternative.uses.size())){work=0;return false;}
                        work-=1+std::int64_t(alternative.uses.size());
                        const auto proof=reference.verifiedZeroOtherEconomicUse.find(id);
                        if((proof==reference.verifiedZeroOtherEconomicUse.end() || proof->second.empty()) &&
                           std::none_of(alternative.uses.begin(),alternative.uses.end(),[&](const Use &u){return u.actor==id;}))
                            accounted=false;
                    }
                }
                if(!accounted) continue;
                for(const auto &use:factory.uses)
                {
                    if(work<=0)return false;
                    --work;
                    if(use.actor!=born.order.actor || use.tick<born.tick+2)continue;
                    bool payoff=use.positiveEffect>0;
                    const auto paid=use.tick+2;
                    if(use.site>=0 && paid<=2*board.horizon && board.sites[use.site].income[0]>0 &&
                       factory.owners[paid][use.site]==0 && alternative.owners[paid][use.site]!=0)
                        payoff=true;
                    if(payoff)
                    {
                        result.buyTick=born.tick;result.useTick=use.tick;result.recruit=born.order.actor;
                        return true;
                    }
                }
            }
            return false;
        }

        inline Decision compareFactoryPrograms(const Board &board,const Arm &reference,const Arm &challenger,
                                               int site,std::int64_t &work)
        {
            Decision out;
            const auto open=[&](std::string why){out.status=Status::Open;out.reason=std::move(why);return out;};
            if(!reference.gap.empty())return open(reference.gap);
            if(!challenger.gap.empty())return open(challenger.gap);
            if(reference.root!=board.root || challenger.root!=board.root || reference.procedure!=board.procedure ||
               challenger.procedure!=board.procedure || reference.opening==challenger.opening ||
               site<0 || std::size_t(site)>=board.sites.size() || !board.sites[site].production ||
               board.sites[site].owner==0 || reference.policies.empty() || reference.policies.size()!=challenger.policies.size())
                return open("UNBOUND_OR_DIFFERENT_ECONOMIC_PROGRAMS");
            std::map<std::string,Decision> common;
            bool firstPolicy=true;
            for(const auto &[policy,refs]:reference.policies)
            {
                const auto group=challenger.policies.find(policy);
                if(policy.empty() || group==challenger.policies.end() || refs.empty() || group->second.empty())
                    return open("COMPETING_ECONOMIC_POLICY_UNCOVERED");
                std::vector<std::pair<Ledger,const Program*>> left,right;
                for(const auto &[programs,arm,bank]:{std::tuple{&refs,&reference,&left},std::tuple{&group->second,&challenger,&right}})
                    for(const auto &p:*programs)
                    {
                        if(p.policy!=policy || p.strategy.empty())return open("MISMATCHED_BUYER_OR_RESPONSE_POLICY");
                        auto r=price(board,arm->opening,p,work);
                        if(!r.gap.empty())return open(r.gap);
                        bank->emplace_back(std::move(r),&p);
                    }
                const auto better=[](const auto &a,const auto &b){return a.first.score<b.first.score;};
                const auto &best=*std::max_element(left.begin(),left.end(),better);
                std::map<std::string,Decision> gains;
                std::set<std::string> strategies;
                const auto &stream=best.first.buyerPolicies;
                for(const auto &entry:left)
                    if(entry.first.buyerPolicies!=stream)return open("BUYER_POLICY_STREAM_MISMATCH");
                for(const auto &[candidate,p]:right)
                {
                    if(!strategies.insert(p->strategy).second)return open("AMBIGUOUS_ECONOMIC_STRATEGY");
                    if(work<=0)return open("ECONOMIC_PROGRAM_WORK_LIMIT");
                    --work;
                    Decision current;
                    if(candidate.buyerPolicies!=stream)return open("BUYER_POLICY_STREAM_MISMATCH");
                    bool witnessed=true;
                    for(const auto &[other,ref]:left)
                    {
                        if(other.score!=best.first.score)continue;
                        if(!productive(board,candidate,other,*ref,site,current,work)){witnessed=false;break;}
                    }
                    if(!witnessed){if(work<=0)return open("ECONOMIC_PROGRAM_WORK_LIMIT");continue;}
                    current.referenceValue=best.first.score; current.challengerValue=candidate.score;
                    current.marginal=candidate.score;
                    if(!RelativeStockDetail::checkedSubtract(current.marginal,best.first.score))
                        return open("ECONOMIC_OVERFLOW");
                    current.governingPolicy=policy;
                    if(!gains.emplace(p->strategy,current).second)return open("AMBIGUOUS_ECONOMIC_STRATEGY");
                }
                if(gains.empty())return open("NO_CERTIFIED_EXTRA_PRODUCTIVE_BUILD_OR_DISPLACED_JOB");
                if(firstPolicy){common=std::move(gains);firstPolicy=false;continue;}
                for(auto it=common.begin();it!=common.end();)
                {
                    auto found=gains.find(it->first);
                    if(found==gains.end())it=common.erase(it);
                    else
                    {
                        if(found->second.marginal<it->second.marginal)it->second=found->second;
                        ++it;
                    }
                }
                if(common.empty())return open("NO_SINGLE_IMPLEMENTABLE_ECONOMIC_STRATEGY");
            }
            out=std::max_element(common.begin(),common.end(),[](const auto &a,const auto &b){
                return a.second.marginal<b.second.marginal;
            })->second;
            out.status=out.marginal>0?Status::StrictEconomicGain:Status::NoNetGain;
            out.reason=out.marginal>0?"NET_DATED_ECONOMIC_GAIN":"NO_STRICT_NET_ECONOMIC_GAIN";
            return out;
        }
    }
}
