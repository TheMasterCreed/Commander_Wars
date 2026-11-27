#pragma once
#include <QJsonObject>
#include <optional>

namespace Coordinator
{
    struct FollowupDebtStamp
    {
        std::size_t unknown{};
        int openStock{};
        QJsonObject financial;
        friend bool operator==(const FollowupDebtStamp &,const FollowupDebtStamp &)=default;
    };

    // A local alternative can borrow its parent's known history. It must
    // still pay every obligation it introduces; siblings' hostile choices
    // cannot force a new debt into a different already-committed branch.
    inline bool followupWitnessClean(bool knownParent,bool currentEnvironmentBound,
                                    const FollowupDebtStamp &before,const FollowupDebtStamp &after)
    {
        return knownParent && currentEnvironmentBound && before==after;
    }

    struct FollowupPolarityWindow
    {
        bool knownParent{};
        std::optional<FollowupDebtStamp> atEnd;
        // Call only after all children have been backed up with their proper
        // owners, and the incomplete sibling itself is recorded as open.
        void finished(bool parent,const FollowupDebtStamp &stamp){knownParent=parent;atEnd=stamp;}
        bool canExport(int outerChoices,bool environmentBound,const FollowupDebtStamp &now)const
        { return outerChoices==1 && knownParent && environmentBound && atEnd && *atEnd==now; }
    };
}
