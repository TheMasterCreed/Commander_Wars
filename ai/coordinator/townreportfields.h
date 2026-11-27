#pragma once
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCborValue>
#include <array>
#include <bit>
#include <cstdint>
#include <type_traits>

// IDs are derived from the sealed renderer's literal publication keys.
namespace Coordinator
{
enum class TownReportField : unsigned
{
    entryTick,
    stops,
    cachedDate,
    requestedEntry,
    unit,
    birth,
    buildTick,
    approachShotScope,
    attackerUpperBound,
    attacksOnApproach,
    bookedApproachShot,
    earlierCaptureReused,
    earlyFireAgainstGuard,
    earlyFireStatus,
    guardApproachGunsCleared,
    guardApproachRouteExpansions,
    guardDepartureTick,
    guardFuelAfter,
    guardPendingTick,
    guardPendingTile,
    guardStatus,
    guardedCompleteReads,
    guardedFirstShotsRestored,
    guardedNextLane,
    guardedNextTick,
    guardedSuffixResumes,
    ifEarlyTitleHolderStays,
    ifRivalDeclinesEarlyOccupation,
    ifRivalGuardsWithoutCapturing,
    ifRivalStartsCapture,
    keepsOtherFundedJobsOpen,
    lateChallengeAfterEarlyFire,
    observedCompleteReads,
    observedFirstShotsRestored,
    observedNextLane,
    observedNextTick,
    observedSuffixResumes,
    openResponseReused,
    otherGunDuties,
    quietThrough,
    reusedGuardExchangeThrough,
    singleOriginalBlockerWalk,
    suffixWorkRetained,
    size
};

inline const QString &townReportKey(unsigned index)
{
    static const std::array<QString,43> names{
        QStringLiteral("entryTick"),
        QStringLiteral("stops"),
        QStringLiteral("cachedDate"),
        QStringLiteral("requestedEntry"),
        QStringLiteral("unit"),
        QStringLiteral("birth"),
        QStringLiteral("buildTick"),
        QStringLiteral("approachShotScope"),
        QStringLiteral("attackerUpperBound"),
        QStringLiteral("attacksOnApproach"),
        QStringLiteral("bookedApproachShot"),
        QStringLiteral("earlierCaptureReused"),
        QStringLiteral("earlyFireAgainstGuard"),
        QStringLiteral("earlyFireStatus"),
        QStringLiteral("guardApproachGunsCleared"),
        QStringLiteral("guardApproachRouteExpansions"),
        QStringLiteral("guardDepartureTick"),
        QStringLiteral("guardFuelAfter"),
        QStringLiteral("guardPendingTick"),
        QStringLiteral("guardPendingTile"),
        QStringLiteral("guardStatus"),
        QStringLiteral("guardedCompleteReads"),
        QStringLiteral("guardedFirstShotsRestored"),
        QStringLiteral("guardedNextLane"),
        QStringLiteral("guardedNextTick"),
        QStringLiteral("guardedSuffixResumes"),
        QStringLiteral("ifEarlyTitleHolderStays"),
        QStringLiteral("ifRivalDeclinesEarlyOccupation"),
        QStringLiteral("ifRivalGuardsWithoutCapturing"),
        QStringLiteral("ifRivalStartsCapture"),
        QStringLiteral("keepsOtherFundedJobsOpen"),
        QStringLiteral("lateChallengeAfterEarlyFire"),
        QStringLiteral("observedCompleteReads"),
        QStringLiteral("observedFirstShotsRestored"),
        QStringLiteral("observedNextLane"),
        QStringLiteral("observedNextTick"),
        QStringLiteral("observedSuffixResumes"),
        QStringLiteral("openResponseReused"),
        QStringLiteral("otherGunDuties"),
        QStringLiteral("quietThrough"),
        QStringLiteral("reusedGuardExchangeThrough"),
        QStringLiteral("singleOriginalBlockerWalk"),
        QStringLiteral("suffixWorkRetained")
    };
    return names[index];
}

inline int findOpeningField(const QString &key)
{
    if(key==QLatin1StringView("entryTick"))return 0;
    if(key==QLatin1StringView("stops"))return 1;
    if(key==QLatin1StringView("cachedDate"))return 2;
    if(key==QLatin1StringView("requestedEntry"))return 3;
    if(key==QLatin1StringView("unit"))return 4;
    if(key==QLatin1StringView("birth"))return 5;
    if(key==QLatin1StringView("buildTick"))return 6;
    for(unsigned i=7;i<static_cast<unsigned>(TownReportField::size);++i)
        if(townReportKey(i)==key)return static_cast<int>(i);
    return -1;
}

static_assert(static_cast<unsigned>(TownReportField::size)<=64);

class TownReportFields
{
public:
    struct Position {
        QJsonValue value;
        qsizetype measured{-1};
        qint64 integer{};
        enum class Input {Json,Integer,Boolean} input{Input::Json};
    };
    struct Destination {
        TownReportFields &fields;
        unsigned index;
        template<class T> void operator=(T &&value)
        {
            using Type=std::remove_cvref_t<T>;
            if constexpr(std::is_same_v<Type,bool>)
                fields.setInteger(index,value,Position::Input::Boolean);
            else if constexpr(std::is_integral_v<Type> && std::is_signed_v<Type> && sizeof(Type)<=sizeof(qint64))
                fields.setInteger(index,value,Position::Input::Integer);
            else
                fields.set(index,QJsonValue(std::forward<T>(value)));
        }
    };

    void begin(const QJsonObject &opening)
    {
        active = 0;
        QJsonObject other;
        for (auto it=opening.begin();it!=opening.end();++it)
        {
            const auto k=it.key();
            const int id=findOpeningField(k);
            if (id>=0) set(static_cast<unsigned>(id),it.value());
            else other.insert(k,it.value());
        }
        if ((!other.isEmpty() || !extra.isEmpty()) && !samePublication(other,extra))
        {
            for (auto it=extra.begin();it!=extra.end();++it)
                if (!other.contains(it.key())) row.remove(it.key());
            for (auto it=other.begin();it!=other.end();++it)
                if (!samePublication(extra.value(it.key()),it.value())) row.insert(it.key(),it.value());
            extra=std::move(other);
            extraBytes=-1;
        }
    }

    Destination operator[](TownReportField id) {return {*this,static_cast<unsigned>(id)};}

    QJsonObject finish()
    {
        for (auto missing=previous & ~active;missing;missing&=missing-1)
        {
            const auto i=static_cast<unsigned>(std::countr_zero(missing));
            row.remove(townReportKey(i));
            positions[i]={};
        }
        previous=active;
        return row;
    }

    // Sums exact Qt Compact value lengths. Every generated key is ASCII and needs
    // no escaping. Arbitrary opening keys retain the Qt writer as their oracle.
    qsizetype compactSize()
    {
        if(extraBytes<0) extraBytes=QJsonDocument(extra).toJson(QJsonDocument::Compact).size()-2;
        qsizetype total=2+extraBytes;
        auto count=extra.size();
        for(auto pending=previous;pending;pending&=pending-1)
        {
            const auto k=static_cast<unsigned>(std::countr_zero(pending));
            auto &p=positions[k];
            if(p.measured<0)
            {
                if(p.value.isObject()) p.measured=QJsonDocument(p.value.toObject()).toJson(QJsonDocument::Compact).size();
                else if(p.value.isArray()) p.measured=QJsonDocument(p.value.toArray()).toJson(QJsonDocument::Compact).size();
                else p.measured=QJsonDocument(QJsonArray{p.value}).toJson(QJsonDocument::Compact).size()-2;
            }
            total+=townReportKey(k).size()+3+p.measured;
            ++count;
        }
        if(count>0)total+=count-1;
        // Internal commas of extra are included in extraBytes already.
        if(extra.size()>1)total-=extra.size()-1;
        return total;
    }
private:
    static bool samePublication(const QJsonValue &a,const QJsonValue &b)
    {
        if(a!=b)return false;
        if(!a.isDouble() && !a.isArray() && !a.isObject())return true;
        // QJsonValue equality can coerce an integer and a binary64 to the
        // same rounded number. CBOR's recursive comparison preserves numeric
        // type and floating bit pattern, and JSON equality compares text.
        return QCborValue::fromJsonValue(a)==QCborValue::fromJsonValue(b);
    }
    void setInteger(unsigned i,qint64 value,Position::Input kind)
    {
        const auto bit=std::uint64_t{1}<<i;
        const bool present=(previous|active)&bit;
        active|=bit;
        auto &p=positions[i];
        if(present && p.input==kind && p.integer==value)return;
        p.input=kind;
        p.integer=value;
        p.value=kind==Position::Input::Boolean?QJsonValue(bool(value)):QJsonValue(value);
        p.measured=-1;
        row.insert(townReportKey(i),p.value);
    }
    void set(unsigned i,const QJsonValue &value)
    {
        const auto mask=std::uint64_t{1}<<i;
        const bool present=(previous|active)&mask;
        if(value.isUndefined())
        {
            if(present)row.remove(townReportKey(i));
            positions[i]={};
            previous&=~mask;
            active&=~mask;
            return;
        }
        active|=mask;
        auto &p=positions[i];
        if(present && samePublication(p.value,value))return;
        p.input=Position::Input::Json;
        p.value=value;
        p.measured=-1;
        row.insert(townReportKey(i),value);
    }
    std::array<Position,static_cast<unsigned>(TownReportField::size)> positions;
    std::uint64_t previous{},active{};
    QJsonObject row,extra;
    qsizetype extraBytes{};
};

class TownDeferredCoverageSize
{
public:
    void begin(const QJsonArray &coverage)
    {
        bytes=QJsonDocument(coverage).toJson(QJsonDocument::Compact).size();
    }
    void replace(const QJsonValue &placeholder,TownReportFields &source)
    {
        if(bytes<0)return;
        if(placeholder.isObject() && placeholder.toObject().isEmpty())
            bytes+=source.compactSize()-2;
        else
            bytes=-1;
    }
    qsizetype finish(const QJsonArray &coverage) const
    {
        return bytes>=0?bytes:QJsonDocument(coverage).toJson(QJsonDocument::Compact).size();
    }
private:
    qsizetype bytes{-1};
};

}
