#pragma once

#include "ai/coordinator/capturedanger.h"
#include <cstdint>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace Coordinator
{
    struct OpeningVolleySquare
    {
        int post{};
        int attackBase{}, counterBase{};
        TerrainDefenseRule attackerCover;
    };

    struct OpeningVolleyGun
    {
        int actor{}, startingHp{};
        std::vector<OpeningVolleySquare> attacks;
    };

    enum class OpeningVolleyAnswer { Survives, PossibleClearance, Incomplete };

    // Native HP is counted in tenths here: ten displayed HP equals 100.
    // The caller binds ordinary rank-free loaded weapon, terrain and action
    // rules, including all damaging actions, and supplies a superset of posts.
    inline OpeningVolleyAnswer boundOpeningVolley(const std::vector<OpeningVolleyGun> &guns,
        TerrainDefenseRule guardCover,int guardHp,std::int64_t &work)
    {
        const auto coverKnown=[](const TerrainDefenseRule &c){
            return c.stars>=0 && c.stars<=100 && c.defensePerStar>=0 && c.defensePerStar<=100 &&
                (c.formula==CaptureDamageFormula::AdvanceWars4 || c.formula==CaptureDamageFormula::AdvanceWars1To3);
        };
        if(guardHp<=0 || guardHp>100 || guns.size()>63 || !coverKnown(guardCover))return OpeningVolleyAnswer::Incomplete;
        std::map<int,int> posts;
        std::set<int> actors;
        for(const auto &gun:guns){
            if(gun.startingHp<=0 || gun.startingHp>100 || gun.actor<0 || !actors.insert(gun.actor).second)
                return OpeningVolleyAnswer::Incomplete;
            for(const auto &a:gun.attacks){
                if(a.post<0 || a.attackBase<0 || a.counterBase<0 || !coverKnown(a.attackerCover))
                    return OpeningVolleyAnswer::Incomplete;
                if(!posts.contains(a.post))posts.emplace(a.post,static_cast<int>(posts.size()));
            }
        }
        if(posts.size()>63)return OpeningVolleyAnswer::Incomplete;
        using Key=std::tuple<std::uint64_t,std::uint64_t,int>;
        std::set<Key> visited;
        bool limited=false;
        const auto upper=[](TerrainDefenseRule cover,int power,int attackSteps,int defenseSteps){
            if(power<=0)return 0;
            // Every native base-damage/fortune term is bounded under the
            // caller's ordinary rule contract. Ceil avoids understating a
            // floating-point boundary and grants extra HP loss to the enemy.
            cover.luck=10;
            const auto v=terrainReducedDamage(cover,static_cast<double>(power),attackSteps,defenseSteps);
            return std::min(100,static_cast<int>(std::ceil(std::clamp(v,0.,100.))));
        };
        const auto search=[&](auto &&self,std::uint64_t used,std::uint64_t occupied,int hp)->bool{
            if(work<1){limited=true;return false;}--work;
            if(!visited.emplace(used,occupied,hp).second)return false;
            for(std::size_t i=0;i<guns.size();++i){
                const auto bit=std::uint64_t{1}<<i;if(used&bit)continue;
                const auto &gun=guns[i];
                for(const auto &a:gun.attacks){
                    const auto postBit=std::uint64_t{1}<<posts.at(a.post);if(occupied&postBit)continue;
                    // The authenticated JOIN removes the donor and marks the
                    // recipient moved. Neither a joined nor a fired unit can
                    // donate a fresh activation in the same enemy turn.
                    std::set<std::pair<int,bool>> granted;
                    {
                        const int step=(gun.startingHp+9)/10,life=gun.startingHp;
                        const int greatest=upper(guardCover,a.attackBase,step,(hp+9)/10);
                        if(greatest>=hp)return true;
                        if(work<greatest+1){limited=true;return false;}work-=greatest+1;
                        // A smaller draw might keep enough defender HP to
                        // counter-kill and free this post. Preserve that option.
                        for(int d=0;d<=greatest;++d){
                            const int after=hp-d;
                            granted.emplace(after,false);
                            if(upper(a.attackerCover,a.counterBase,(after+9)/10,step)>=life)
                                granted.emplace(after,true);
                        }
                    }
                    for(const auto &[after,clearedPost]:granted){
                        if(self(self,used|bit,clearedPost?occupied:occupied|postBit,after))return true;
                        if(limited)return false;
                    }
                }
            }
            return false;
        };
        const auto possible=search(search,0,0,guardHp);
        return limited?OpeningVolleyAnswer::Incomplete:possible?OpeningVolleyAnswer::PossibleClearance:OpeningVolleyAnswer::Survives;
    }
}
