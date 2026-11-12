#include "ai/coordinator/guardedscriptfunction.h"

#include <cstddef>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <map>
#include <limits>

#include <QCryptographicHash>
#include <QByteArrayView>
#include <QJSEngine>
#include <QMetaClassInfo>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QPointF>
#include <QStringView>
#include <QThread>
#include <QtEndian>

#ifdef SCRIPTSOURCESUPPORT
#include <private/qjsvalue_p.h>
#include <private/qqmlpropertycache_p.h>
#include <private/qqmlbuiltinfunctions_p.h>
#include <private/qqmlsignalnames_p.h>
#include <private/qqmlvaluetype_p.h>
#include <private/qqmlvaluetypewrapper_p.h>
#include <private/qv4arraydata_p.h>
#include <private/qv4compileddata_p.h>
#include <private/qv4executablecompilationunit_p.h>
#include <private/qv4functionobject_p.h>
#include <private/qv4instr_moth_p.h>
#include <private/qv4property_p.h>
#include <private/qv4qobjectwrapper_p.h>
#include <private/qv4scopedvalue_p.h>
#include <private/qv4string_p.h>
#endif

namespace Coordinator
{
#ifdef SCRIPTSOURCESUPPORT
    struct GuardedScriptObservationAccess
    {
        static QV4::Value read(const GuardedScriptValueView & view)
        {
            if (view.m_kind == GuardedScriptValueKind::Unavailable)
            {
                return QV4::Value::undefinedValue();
            }
            return QV4::Value::fromReturnedValue(view.m_encoded);
        }

        static GuardedScriptValueView borrow(const QV4::Value & value)
        {
            GuardedScriptValueView view;
            view.m_encoded = value.asReturnedValue();
            if (value.isUndefined())
            {
                view.m_kind = GuardedScriptValueKind::Undefined;
            }
            else if (value.isNull())
            {
                view.m_kind = GuardedScriptValueKind::Null;
            }
            else if (value.isBoolean())
            {
                view.m_kind = GuardedScriptValueKind::Boolean;
            }
            else if (value.isNumber())
            {
                view.m_kind = GuardedScriptValueKind::Number;
            }
            else if (value.isString())
            {
                view.m_kind = GuardedScriptValueKind::String;
            }
            else
            {
                view.m_kind = GuardedScriptValueKind::Managed;
            }
            return view;
        }
    };
#endif

    namespace
    {
        constexpr std::int64_t FUNCTION_INSPECTION_WORK = 64;
        constexpr std::uint32_t MAX_COMPILATION_BYTES = 1024 * 1024;
        constexpr std::uint32_t HASH_BLOCK_BYTES = 64;
        constexpr std::int64_t IMAGE_BLOCK_PASSES = 4;
        constexpr std::size_t MAX_CACHED_COMPILATION_UNITS = 128;
        constexpr std::int64_t IMAGE_CACHE_CONSTRUCTION_WORK = 256;
        constexpr std::int64_t IMAGE_CACHE_TREE_WORK = 64;
        constexpr std::int64_t IMAGE_CACHE_RETENTION_WORK = 512;
        constexpr char SCRIPT_FUNCTION_CLASS_NAME[] = "ScriptFunction";
        constexpr char DATA_OBJECT_CLASS_NAME[] = "Object";
        constexpr char DATA_ARRAY_CLASS_NAME[] = "ArrayObject";
        constexpr char MATH_OBJECT_CLASS_NAME[] = "MathObject";
        constexpr char STRING_OBJECT_CLASS_NAME[] = "StringObject";
        constexpr char NUMBER_OBJECT_CLASS_NAME[] = "NumberObject";
        constexpr std::uint32_t MAX_PROPERTY_PROTOTYPES = 8;
        constexpr qsizetype MAX_PROPERTY_NAME = 128;
        constexpr std::int64_t PROPERTY_METADATA_WORK = 96;
        constexpr std::int64_t VALUE_CONSTRUCTOR_CONTROL_WORK = 256;
        const QString QT_NAMESPACE = QStringLiteral("Qt");
        constexpr std::int64_t PROPERTY_MEMBER_WORK = 16;
        constexpr std::int64_t PROPERTY_SPARSE_NODE_WORK = 64;
        constexpr std::int64_t CONTEXT_BYTE_PASSES = 4;
        constexpr std::int64_t CONTEXT_SCOPE_WORK = 96;
        constexpr std::uint32_t MAX_CONTEXT_SCOPES = 8;
        constexpr std::int64_t GLOBAL_INSPECTION_WORK = 64;
        constexpr std::size_t GLOBAL_INDEX_BUCKETS = 2 * GUARDED_SCRIPT_MAX_INDEXED_GLOBAL_NAMES;
        constexpr qsizetype GLOBAL_INDEX_PREFIX_CHARACTERS = 4;
        constexpr std::size_t GLOBAL_INDEX_PREFIX_WORDS = 64;
        constexpr std::size_t GLOBAL_INDEX_PREFIX_BITS = std::numeric_limits<std::uint64_t>::digits;
        constexpr std::int64_t GLOBAL_INDEX_PREFIX_STORAGE_PASSES = 4;
        constexpr std::int32_t MAX_GLOBAL_STORAGE_SLOTS = 65536;
        constexpr std::int64_t GLOBAL_INDEX_RETENTION_WORK = 256;
        constexpr std::int64_t GLOBAL_INDEX_CHARACTER_WORK = 8;
        constexpr std::size_t MAX_OWN_PROPERTY_INDEXES = 64;
        constexpr std::uint32_t MAX_OWN_INDEX_MEMBERS = 1024;
        constexpr std::int32_t MAX_OWN_INDEX_STORAGE_SLOTS = 4096;
        constexpr std::size_t OWN_INDEX_BUCKET_FACTOR = 2;
        constexpr std::int64_t OWN_INDEX_STORAGE_PASSES = 4;
        constexpr std::int64_t OWN_INDEX_CONTROL_WORK = 256;
        constexpr std::int64_t OWN_INDEX_REGISTRY_WORK = PROPERTY_MEMBER_WORK * MAX_OWN_PROPERTY_INDEXES;
        constexpr std::int64_t OWN_INDEX_ALIAS_COMPARE_WORK = 2 * sizeof(QChar) * (MAX_PROPERTY_NAME + 1);
        constexpr std::uint32_t GLOBAL_INDEX_HASH_OFFSET = 2166136261u;
        constexpr std::uint32_t GLOBAL_INDEX_HASH_PRIME = 16777619u;
        constexpr std::int64_t ARRAY_NAMED_MEMBER_WORK = 64;
        constexpr char ARRAY_LENGTH_NAME[] = "length";
        constexpr std::int64_t SCALAR_METADATA_WORK = 32;
        constexpr qsizetype MAX_SCALAR_STRING = 128;
        constexpr std::int64_t SCALAR_CHARACTER_WORK = 4;
        constexpr std::uint32_t MAX_COMPILATION_NAME = 1024;
        constexpr std::int64_t COMPILATION_NAME_WORK = 32;
        constexpr std::int64_t COMPILATION_NAME_CHARACTER_WORK = 4;
        constexpr std::uint32_t MAX_NATIVE_META_DEPTH = 8;
        constexpr std::int32_t MAX_NATIVE_META_MEMBERS = 1024;
        constexpr std::int64_t NATIVE_META_MEMBER_WORK = 1024;
        constexpr std::int64_t NATIVE_LIFETIME_WORK = 32;
        constexpr std::size_t MAX_NATIVE_METHOD_REQUIREMENTS = 128;
        constexpr std::int32_t MAX_NATIVE_METHOD_ARGUMENTS = 16;
        constexpr std::int64_t NATIVE_CONTRACT_CONTROL_WORK = 256;
        constexpr std::size_t MAX_NATIVE_CACHED_CONTRACTS = 32;
        constexpr std::int64_t NATIVE_CONTRACT_KEY_ENTRY_WORK = 64;
        constexpr std::int64_t NATIVE_CONTRACT_KEY_CHARACTER_WORK = 4;
        constexpr std::int64_t NATIVE_CONTRACT_NAME_WORK =
            NATIVE_CONTRACT_KEY_ENTRY_WORK + NATIVE_CONTRACT_KEY_CHARACTER_WORK * MAX_PROPERTY_NAME;
        constexpr std::int64_t NATIVE_NAMESPACE_CONTROL_WORK = 64;
        constexpr std::int64_t NATIVE_NAMESPACE_RETENTION_WORK = 512;
        constexpr std::int64_t NATIVE_NAMESPACE_COMPARE_PASSES = 4;
        const QString NATIVE_DESTROY_NAME = QStringLiteral("destroy");
        const QString NATIVE_STRING_NAME = QStringLiteral("toString");
        constexpr char NATIVE_WRAPPER_FACTORY_INFO[] = "qt_QmlJSWrapperFactoryMethod";

        bool payPropertyWork(GuardedScriptProperty & result, RefinementLedger & ledger, std::int64_t work)
        {
            if (!ledger.consume(work, RefinementWork::MasterPreparation))
            {
                result.status = GuardedScriptPropertyStatus::BudgetExhausted;
                return false;
            }
            result.chargedWork += work;
            return true;
        }

#ifdef SCRIPTSOURCESUPPORT
        constexpr int INSTRUCTION_ARGUMENTS[] = {
            FOR_EACH_MOTH_INSTR_ALL(MOTH_COLLECT_NARGS)
        };

        const QV4::CompiledData::String* compilationName(
            const QV4::CompiledData::Unit & image, std::uint32_t size)
        {
            const auto index = static_cast<std::uint32_t>(image.sourceFileIndex);
            const auto count = static_cast<std::uint32_t>(image.stringTableSize);
            const auto table = static_cast<std::uint32_t>(image.offsetToStringTable);
            if (index >= count || table > size || count > (size - table) / sizeof(quint32_le))
            {
                return nullptr;
            }
            const auto* bytes = reinterpret_cast<const char*>(&image);
            const auto* offsets = reinterpret_cast<const quint32_le*>(bytes + table);
            const auto offset = static_cast<std::uint32_t>(offsets[index]);
            if (offset > size || sizeof(QV4::CompiledData::String) > size - offset)
            {
                return nullptr;
            }
            const auto* name = reinterpret_cast<const QV4::CompiledData::String*>(bytes + offset);
            const auto characters = static_cast<std::int32_t>(name->size);
            if (characters < 0 || characters > static_cast<std::int32_t>(MAX_COMPILATION_NAME) ||
                static_cast<std::uint32_t>(characters) >
                    (size - offset - sizeof(QV4::CompiledData::String)) / sizeof(quint16_le))
            {
                return nullptr;
            }
            return name;
        }

        QString copyCompilationName(const QV4::CompiledData::String & name)
        {
            const auto count = static_cast<std::int32_t>(name.size);
            const auto* characters = reinterpret_cast<const quint16_le*>(&name + 1);
            QString copied(count, Qt::Uninitialized);
            for (std::int32_t index = 0; index < count; ++index)
            {
                copied[index] = QChar(static_cast<quint16>(characters[index]));
            }
            return copied;
        }

        bool usesContextOperands(QV4::Moth::Instr::Type type)
        {
            using Type = QV4::Moth::Instr::Type;
            switch (QV4::Moth::Instr::narrowInstructionType(type))
            {
                case Type::LoadLocal:
                case Type::StoreLocal:
                case Type::LoadScopedLocal:
                case Type::StoreScopedLocal:
                case Type::LoadClosure:
                case Type::StoreNameSloppy:
                case Type::StoreNameStrict:
                case Type::CallPossiblyDirectEval:
                case Type::DeleteName:
                case Type::LoadImport:
                case Type::LoadQmlContextPropertyLookup:
                case Type::CallQmlContextPropertyLookup:
                case Type::CreateCallContext:
                case Type::PushCatchContext:
                case Type::PushWithContext:
                case Type::PushBlockContext:
                case Type::CloneBlockContext:
                case Type::PushScriptContext:
                case Type::PopScriptContext:
                case Type::PopContext:
                case Type::CreateMappedArgumentsObject:
                case Type::CreateUnmappedArgumentsObject:
                case Type::CreateClass:
                case Type::DeclareVar:
                case Type::GetTemplateObject:
                case Type::LoadSuperProperty:
                case Type::StoreSuperProperty:
                case Type::LoadSuperConstructor:
                case Type::Yield:
                case Type::YieldStar:
                case Type::Resume:
                    return true;
                default:
                    return false;
            }
        }

        void retainProperty(
            GuardedScriptProperty & result, const QV4::Value & value, QV4::PropertyAttributes attributes)
        {
            if (attributes.isAccessor() || !attributes.isData())
            {
                result.status = GuardedScriptPropertyStatus::Unsupported;
                return;
            }
            result.value = GuardedScriptObservationAccess::borrow(value);
            result.status = GuardedScriptPropertyStatus::Found;
        }

        bool readIndexedOwnProperty(
            const QV4::Object &, const QString &, GuardedScriptProperty &,
            RefinementLedger &, GuardedScriptOwnIndexCache &);

        void readOwnProperty(
            const QV4::Object & object, const QString & name, uint arrayIndex,
            GuardedScriptProperty & result, RefinementLedger & ledger,
            GuardedScriptOwnIndexCache* cache = nullptr)
        {
            result.status = GuardedScriptPropertyStatus::Missing;
            if (arrayIndex != UINT_MAX)
            {
                auto* array = object.arrayData();
                if (array == nullptr)
                {
                    return;
                }
                if (array->type == QV4::Heap::ArrayData::Sparse)
                {
                    const auto work = PROPERTY_SPARSE_NODE_WORK *
                        static_cast<std::int64_t>(array->sparse->nEntries());
                    if (!payPropertyWork(result, ledger, work))
                    {
                        return;
                    }
                }
                else if (array->type != QV4::Heap::ArrayData::Simple)
                {
                    result.status = GuardedScriptPropertyStatus::Unsupported;
                    return;
                }
                QV4::Property property;
                QV4::PropertyAttributes attributes;
                if (array->getProperty(arrayIndex, &property, &attributes))
                {
                    retainProperty(result, property.value, attributes);
                }
                return;
            }
            if (cache != nullptr && readIndexedOwnProperty(object, name, result, ledger, *cache))
            {
                return;
            }
            const auto* shape = object.internalClass();
            const auto* table = shape->propertyTable.d;
            const auto perMember = PROPERTY_MEMBER_WORK + static_cast<std::int64_t>(name.size()) * sizeof(QChar);
            if (!payPropertyWork(result, ledger, perMember * table->alloc))
            {
                return;
            }
            // Raw member scans avoid shared identifier interning and hash-table probes.
            for (int slot = 0; slot < table->alloc; ++slot)
            {
                const auto & entry = table->entries[slot];
                const auto key = entry.identifier;
                if (!key.isValid() || entry.index >= shape->size)
                {
                    continue;
                }
                const auto index = entry.index;
                const auto attributes = shape->propertyData.at(index);
                if (attributes.isEmpty())
                {
                    continue;
                }
                if (!key.isString())
                {
                    continue;
                }
                const auto* text = key.asStringOrSymbol<QV4::Heap::String>();
                if (text->subtype >= QV4::Heap::String::StringType_Complex)
                {
                    result.status = GuardedScriptPropertyStatus::Unsupported;
                    return;
                }
                if (text->text().size == name.size() && text->toQString() == name)
                {
                    retainProperty(result, *object.propertyData(index), attributes);
                    return;
                }
            }
        }

        bool supportedNativeMetadata(
            const QObject & expected, const QMetaObject & permitted,
            GuardedScriptProperty & result, RefinementLedger & ledger)
        {
            const auto* privateData = QObjectPrivate::get(&expected);
            if (!payPropertyWork(result, ledger, NATIVE_LIFETIME_WORK) ||
                QQmlData::wasDeleted(privateData) || privateData->deleteLaterCalled)
            {
                return false;
            }
            const auto* qmlData = QQmlData::get(privateData);
            if (privateData->metaObject != nullptr || expected.metaObject() != &permitted ||
                (qmlData && (qmlData->context != nullptr || qmlData->outerContext != nullptr)))
            {
                return false;
            }
            std::array<const QMetaObject*, MAX_NATIVE_META_DEPTH> hierarchy{};
            const QMetaObject* meta = &permitted;
            std::size_t count = 0;
            while (meta != nullptr && count < hierarchy.size())
            {
                if (!payPropertyWork(result, ledger, PROPERTY_METADATA_WORK))
                {
                    return false;
                }
                hierarchy[count++] = meta;
                meta = meta->superClass();
            }
            if (meta != nullptr)
            {
                return false;
            }
            const auto* cache = qmlData ? qmlData->propertyCache.data() : nullptr;
            for (std::size_t depth = 0; depth < count; ++depth)
            {
                if (!payPropertyWork(result, ledger, PROPERTY_METADATA_WORK))
                {
                    return false;
                }
                meta = hierarchy[depth];
                const auto properties = meta->propertyCount();
                const auto methods = meta->methodCount();
                if (properties < 0 || properties > MAX_NATIVE_META_MEMBERS ||
                    methods < 0 || methods > MAX_NATIVE_META_MEMBERS ||
                    (cache && (cache->metaObject() != meta ||
                        cache->propertyCount() != properties || cache->methodCount() != methods)))
                {
                    return false;
                }
                if (cache)
                {
                    cache = cache->parent().data();
                }
            }
            return cache == nullptr;
        }

        bool nativeNameCollision(
            const QMetaObject & meta, const QString & name,
            GuardedScriptProperty & result, RefinementLedger & ledger)
        {
            const auto members = meta.propertyCount() + meta.methodCount();
            if (!payPropertyWork(result, ledger, NATIVE_META_MEMBER_WORK * (members + 2)))
            {
                return true;
            }
            if (name == NATIVE_DESTROY_NAME || name == NATIVE_STRING_NAME)
            {
                return true;
            }
            for (int index = 0; index < meta.propertyCount(); ++index)
            {
                const auto* text = meta.property(index).name();
                qsizetype size = 0;
                while (size <= MAX_PROPERTY_NAME && text[size] != '\0')
                {
                    ++size;
                }
                if (size > MAX_PROPERTY_NAME || QString::fromUtf8(text, size) == name)
                {
                    return true;
                }
            }
            for (int index = 0; index < meta.methodCount(); ++index)
            {
                const auto method = meta.method(index);
                const auto text = method.name();
                if (text.size() > MAX_PROPERTY_NAME)
                {
                    return true;
                }
                const auto memberName = QString::fromUtf8(text);
                if (memberName == name ||
                    (method.methodType() == QMetaMethod::Signal &&
                        QQmlSignalNames::signalNameToHandlerName(memberName) == name))
                {
                    return true;
                }
            }
            return false;
        }

        struct NativeMethodDispatch
        {
            std::int32_t arguments{0};
            std::int32_t index{-1};
            bool callable{false};
            bool selected{false};
        };

        struct NativeMethodName
        {
            bool forbidden{false};
            std::vector<NativeMethodDispatch> dispatches;
        };

        class NativeMethodNamespace final
        {
        public:
            NativeMethodName* entry(const QString & name, RefinementLedger & ledger)
            {
                if (!payLookup(name, ledger) ||
                    !ledger.consume(NATIVE_NAMESPACE_RETENTION_WORK, RefinementWork::MasterPreparation))
                {
                    return nullptr;
                }
                const auto found = m_names.try_emplace(name).first;
                m_maximumName = std::max(m_maximumName, name.size());
                return &found->second;
            }

            NativeMethodName* inspect(const QString & name, RefinementLedger & ledger, bool & funded)
            {
                funded = payLookup(name, ledger);
                if (!funded)
                {
                    return nullptr;
                }
                const auto found = m_names.find(name);
                if (found == m_names.end())
                {
                    return nullptr;
                }
                return &found->second;
            }

            NativeMethodName* find(const QString & name, RefinementLedger & ledger, bool & funded)
            {
                auto* found = inspect(name, ledger, funded);
                if (found == nullptr)
                {
                    return nullptr;
                }
                if (!ledger.consume(NATIVE_NAMESPACE_CONTROL_WORK * (found->dispatches.size() + 1),
                                    RefinementWork::MasterPreparation))
                {
                    funded = false;
                    return nullptr;
                }
                return found;
            }

        private:
            bool payLookup(const QString & name, RefinementLedger & ledger) const
            {
                std::int64_t depth = 1;
                for (auto remaining = m_names.size() + 1; remaining > 1; remaining >>= 1)
                {
                    ++depth;
                }
                const auto work = NATIVE_NAMESPACE_CONTROL_WORK +
                    NATIVE_NAMESPACE_COMPARE_PASSES * (2 * depth + 1) * (name.size() + m_maximumName + 1);
                return ledger.consume(work, RefinementWork::MasterPreparation);
            }

            std::map<QString, NativeMethodName> m_names;
            qsizetype m_maximumName{0};
        };

        GuardedNativeReceiverStatus seedNativeMethodNamespace(
            std::span<const GuardedNativeMethodRequirement> requirements,
            NativeMethodNamespace & names, RefinementLedger & ledger)
        {
            for (const auto & requirement : requirements)
            {
                if (requirement.name.empty() || requirement.name.size() > static_cast<std::size_t>(MAX_PROPERTY_NAME) ||
                    requirement.arguments < 0 || requirement.arguments > MAX_NATIVE_METHOD_ARGUMENTS)
                {
                    return GuardedNativeReceiverStatus::Unsupported;
                }
                for (const auto character : requirement.name)
                {
                    if ((character < 'a' || character > 'z') && (character < 'A' || character > 'Z') &&
                        (character < '0' || character > '9') && character != '_')
                    {
                        return GuardedNativeReceiverStatus::Unsupported;
                    }
                }
                const auto name = QString::fromLatin1(requirement.name.data(), requirement.name.size());
                if (names.entry(name, ledger) == nullptr)
                {
                    return GuardedNativeReceiverStatus::BudgetExhausted;
                }
            }
            return GuardedNativeReceiverStatus::Bound;
        }

        GuardedNativeReceiverStatus forbidNativeMethodName(
            const QString & name, NativeMethodNamespace & names, RefinementLedger & ledger)
        {
            bool funded = false;
            auto* entry = names.inspect(name, ledger, funded);
            if (!funded)
            {
                return GuardedNativeReceiverStatus::BudgetExhausted;
            }
            if (entry != nullptr)
            {
                entry->forbidden = true;
            }
            return GuardedNativeReceiverStatus::Bound;
        }

        GuardedNativeReceiverStatus prepareNativeMethodNamespace(
            const QMetaObject & metadata, NativeMethodNamespace & names, RefinementLedger & ledger)
        {
            for (const auto & reserved : {NATIVE_DESTROY_NAME, NATIVE_STRING_NAME})
            {
                const auto status = forbidNativeMethodName(reserved, names, ledger);
                if (status != GuardedNativeReceiverStatus::Bound)
                {
                    return status;
                }
            }
            for (std::int32_t index = 0; index < metadata.propertyCount(); ++index)
            {
                const auto* bytes = metadata.property(index).name();
                qsizetype length = 0;
                while (length <= MAX_PROPERTY_NAME && bytes[length] != '\0')
                {
                    ++length;
                }
                if (length > MAX_PROPERTY_NAME)
                {
                    return GuardedNativeReceiverStatus::Unsupported;
                }
                const auto status = forbidNativeMethodName(QString::fromUtf8(bytes, length), names, ledger);
                if (status != GuardedNativeReceiverStatus::Bound)
                {
                    return status;
                }
            }
            const auto deleteLaterIndex = QObject::staticMetaObject.indexOfSlot("deleteLater()");
            const auto objectMethods = QObject::staticMetaObject.methodCount();
            for (std::int32_t index = 0; index < metadata.methodCount(); ++index)
            {
                const auto method = metadata.method(index);
                const auto bytes = method.name();
                if (bytes.size() > MAX_PROPERTY_NAME)
                {
                    return GuardedNativeReceiverStatus::Unsupported;
                }
                const auto name = QString::fromUtf8(bytes);
                bool funded = false;
                auto* entry = names.inspect(name, ledger, funded);
                if (!funded)
                {
                    return GuardedNativeReceiverStatus::BudgetExhausted;
                }
                if (entry != nullptr)
                {
                    if (!ledger.consume(NATIVE_NAMESPACE_RETENTION_WORK, RefinementWork::MasterPreparation))
                    {
                        return GuardedNativeReceiverStatus::BudgetExhausted;
                    }
                    const bool callable = method.access() == QMetaMethod::Public &&
                        !(index < objectMethods && index == deleteLaterIndex) &&
                        (method.methodType() == QMetaMethod::Method || method.methodType() == QMetaMethod::Slot);
                    entry->dispatches.push_back({method.parameterCount(), index, callable, false});
                }
                if (method.methodType() == QMetaMethod::Signal)
                {
                    const auto status = forbidNativeMethodName(
                        QQmlSignalNames::signalNameToHandlerName(name), names, ledger);
                    if (status != GuardedNativeReceiverStatus::Bound)
                    {
                        return status;
                    }
                }
            }
            return GuardedNativeReceiverStatus::Bound;
        }

        bool primitiveArray(const QV4::Value & value, std::uint32_t & length)
        {
            const auto* array = value.as<QV4::Object>();
            constexpr auto lengthIndex = QV4::Heap::ArrayObject::LengthPropertyIndex;
            if (array == nullptr || std::strcmp(array->vtable()->className, DATA_ARRAY_CLASS_NAME) != 0 ||
                array->d()->prototype() != array->engine()->arrayPrototype()->d() ||
                array->internalClass()->size <= lengthIndex ||
                !array->internalClass()->propertyData.at(lengthIndex).isData())
            {
                return false;
            }
            const auto* stored = array->propertyData(lengthIndex);
            if (!stored->isNumber())
            {
                return false;
            }
            const auto observed = stored->asDouble();
            if (!std::isfinite(observed) || observed < 0 || observed > GUARDED_SCRIPT_MAX_ARRAY_LENGTH ||
                std::trunc(observed) != observed)
            {
                return false;
            }
            length = static_cast<std::uint32_t>(observed);
            return true;
        }
#endif
    }

    struct GuardedScriptGlobalIndex::Data
    {
        struct Entry
        {
            QString name;
            std::uint32_t hash{0};
            std::uint32_t tableSlot{UINT_MAX};
            std::uint32_t propertyIndex{UINT_MAX};
        };
        const QJSEngine* engine{nullptr};
        const RefinementLedger* ledger{nullptr};
        const void* object{nullptr};
        const void* shape{nullptr};
        const void* table{nullptr};
        std::int32_t allocated{0};
        std::uint32_t members{0};
        std::size_t count{0};
        qsizetype maximumName{0};
        std::array<Entry, GUARDED_SCRIPT_MAX_INDEXED_GLOBAL_NAMES> entries;
        std::array<std::int32_t, GLOBAL_INDEX_BUCKETS> buckets;
        std::array<std::uint64_t, GLOBAL_INDEX_PREFIX_WORDS> prefixes{};
    };

    namespace
    {
        std::uint32_t globalNameHash(const QString & name)
        {
            auto hash = GLOBAL_INDEX_HASH_OFFSET;
            for (const auto character : name)
            {
                hash ^= character.unicode();
                hash *= GLOBAL_INDEX_HASH_PRIME;
            }
            return hash;
        }

        std::int64_t globalPrefixWork(qsizetype length)
        {
            return GLOBAL_INDEX_CHARACTER_WORK * (std::min(length, GLOBAL_INDEX_PREFIX_CHARACTERS) + 1);
        }

        std::uint32_t globalPrefixBucket(QStringView name)
        {
            auto hash = GLOBAL_INDEX_HASH_OFFSET ^ static_cast<std::uint32_t>(name.size());
            const auto length = std::min(name.size(), GLOBAL_INDEX_PREFIX_CHARACTERS);
            for (qsizetype index = 0; index < length; ++index)
            {
                hash ^= name[index].unicode();
                hash *= GLOBAL_INDEX_HASH_PRIME;
            }
            return hash % (GLOBAL_INDEX_PREFIX_WORDS * GLOBAL_INDEX_PREFIX_BITS);
        }

        GuardedScriptProperty indexedPropertyWork(
            GuardedScriptProperty result, const RefinementLedger & ledger, std::int64_t before)
        {
            result.chargedWork = ledger.stats().spent() - before;
            return result;
        }

        template<typename Index>
        std::optional<std::size_t> indexedGlobalName(
            const Index & data, const QString & name, std::uint32_t hash,
            RefinementLedger & ledger, bool & funded)
        {
            const auto buckets = data.buckets.size();
            auto bucket = static_cast<std::size_t>(hash) % buckets;
            funded = true;
            for (std::size_t attempt = 0; attempt < buckets; ++attempt)
            {
                if (!ledger.consume(GLOBAL_INSPECTION_WORK, RefinementWork::MasterPreparation))
                {
                    funded = false;
                    return std::nullopt;
                }
                const auto index = data.buckets[bucket];
                if (index < 0)
                {
                    return bucket;
                }
                const auto & entry = data.entries[index];
                if (entry.hash == hash)
                {
                    if (!ledger.consume(2 * (name.size() + entry.name.size() + 1),
                                        RefinementWork::MasterPreparation))
                    {
                        funded = false;
                        return std::nullopt;
                    }
                    if (entry.name == name)
                    {
                        return bucket;
                    }
                }
                bucket = (bucket + 1) % buckets;
            }
            return std::nullopt;
        }
    }

    struct GuardedScriptOwnIndexData
    {
        struct Index
        {
            struct Entry
            {
                QString name;
                std::uint32_t hash{0};
                std::uint32_t tableSlot{UINT_MAX};
                std::uint32_t propertyIndex{0};
            };
            const void* shape{nullptr};
            const void* table{nullptr};
            const void* names{nullptr};
            const void* attributes{nullptr};
            std::int32_t allocated{0};
            std::uint32_t members{0};
            GuardedScriptPropertyStatus status{GuardedScriptPropertyStatus::Missing};
            std::vector<Entry> entries;
            std::vector<std::int32_t> buckets;
        };
        const QJSEngine* engine{nullptr};
        const RefinementLedger* ledger{nullptr};
        std::vector<Index> indexes;
    };

    struct GuardedScriptOwnIndexAccess
    {
        static GuardedScriptOwnIndexData* data(GuardedScriptOwnIndexCache & cache)
        {
            return cache.m_data.get();
        }
    };

    GuardedScriptOwnIndexCache::GuardedScriptOwnIndexCache(QJSEngine & engine, RefinementLedger & ledger)
    {
        const auto work = OWN_INDEX_CONTROL_WORK + OWN_INDEX_STORAGE_PASSES *
            (sizeof(GuardedScriptOwnIndexData) + MAX_OWN_PROPERTY_INDEXES *
                sizeof(GuardedScriptOwnIndexData::Index));
        if (!ledger.consume(work, RefinementWork::MasterPreparation))
        {
            return;
        }
        m_data = std::make_unique<GuardedScriptOwnIndexData>();
        m_data->engine = &engine;
        m_data->ledger = &ledger;
        m_data->indexes.reserve(MAX_OWN_PROPERTY_INDEXES);
        m_status = GuardedScriptPropertyStatus::Found;
    }

    GuardedScriptOwnIndexCache::~GuardedScriptOwnIndexCache() = default;

    GuardedScriptPropertyStatus GuardedScriptOwnIndexCache::status() const
    {
        return m_status;
    }

#ifdef SCRIPTSOURCESUPPORT
    namespace
    {
        bool prepareOwnIndex(
            const QV4::Object & object, GuardedScriptOwnIndexData::Index & data,
            GuardedScriptProperty & result, RefinementLedger & ledger)
        {
            const auto* shape = object.internalClass();
            const auto* table = shape->propertyTable.d;
            const auto buckets = std::max<std::size_t>(1, OWN_INDEX_BUCKET_FACTOR * shape->size);
            const auto work = OWN_INDEX_CONTROL_WORK + PROPERTY_MEMBER_WORK * (table->alloc + shape->size) +
                OWN_INDEX_STORAGE_PASSES * (shape->size * sizeof(GuardedScriptOwnIndexData::Index::Entry) +
                    buckets * sizeof(std::int32_t));
            if (!payPropertyWork(result, ledger, work))
            {
                return false;
            }
            data.shape = shape;
            data.table = table;
            data.names = shape->nameMap.d;
            data.attributes = shape->propertyData.d;
            data.allocated = table->alloc;
            data.members = shape->size;
            data.entries.resize(shape->size);
            data.buckets.assign(buckets, -1);
            for (std::int32_t slot = 0; slot < table->alloc; ++slot)
            {
                const auto & current = table->entries[slot];
                if (!current.identifier.isValid() || current.index >= shape->size ||
                    shape->propertyData.at(current.index).isEmpty() || !current.identifier.isString())
                {
                    continue;
                }
                const auto* text = current.identifier.asStringOrSymbol<QV4::Heap::String>();
                if (text->subtype >= QV4::Heap::String::StringType_Complex || text->text().size < 0)
                {
                    return false;
                }
                const auto length = text->text().size;
                if (length > MAX_PROPERTY_NAME)
                {
                    continue;
                }
                if (!payPropertyWork(result, ledger, GLOBAL_INDEX_CHARACTER_WORK * (length + 1)))
                {
                    return false;
                }
                auto name = text->toQString();
                auto & observed = data.entries[current.index];
                if (observed.tableSlot != UINT_MAX)
                {
                    if (!payPropertyWork(result, ledger, OWN_INDEX_ALIAS_COMPARE_WORK))
                    {
                        return false;
                    }
                    if (observed.name != name)
                    {
                        data.status = GuardedScriptPropertyStatus::Unsupported;
                        result.status = GuardedScriptPropertyStatus::Unsupported;
                        return false;
                    }
                    continue;
                }
                observed.hash = globalNameHash(name);
                observed.name = std::move(name);
                observed.tableSlot = static_cast<std::uint32_t>(slot);
                observed.propertyIndex = current.index;
            }
            // Logical member order is independent of Qt's physical hash-table placement.
            for (std::size_t index = 0; index < data.entries.size(); ++index)
            {
                const auto & entry = data.entries[index];
                if (entry.tableSlot == UINT_MAX)
                {
                    continue;
                }
                bool funded = false;
                const auto before = ledger.stats().spent();
                const auto bucket = indexedGlobalName(data, entry.name, entry.hash, ledger, funded);
                result.chargedWork += ledger.stats().spent() - before;
                if (!funded)
                {
                    result.status = GuardedScriptPropertyStatus::BudgetExhausted;
                    return false;
                }
                if (!bucket || data.buckets[*bucket] >= 0)
                {
                    data.status = GuardedScriptPropertyStatus::Unsupported;
                    result.status = GuardedScriptPropertyStatus::Unsupported;
                    return false;
                }
                data.buckets[*bucket] = static_cast<std::int32_t>(index);
            }
            data.status = GuardedScriptPropertyStatus::Found;
            return true;
        }

        bool readOwnIndex(
            const QV4::Object & object, const GuardedScriptOwnIndexData::Index & data,
            const QString & name, GuardedScriptProperty & result, RefinementLedger & ledger)
        {
            if (data.status == GuardedScriptPropertyStatus::Unsupported)
            {
                result.status = data.status;
                return true;
            }
            if (data.status != GuardedScriptPropertyStatus::Found)
            {
                return false;
            }
            if (!payPropertyWork(result, ledger, GLOBAL_INDEX_CHARACTER_WORK * (name.size() + 1)))
            {
                return true;
            }
            bool funded = false;
            const auto before = ledger.stats().spent();
            const auto bucket = indexedGlobalName(data, name, globalNameHash(name), ledger, funded);
            result.chargedWork += ledger.stats().spent() - before;
            if (!funded)
            {
                result.status = GuardedScriptPropertyStatus::BudgetExhausted;
                return true;
            }
            if (!bucket)
            {
                return false;
            }
            if (data.buckets[*bucket] < 0)
            {
                result.status = GuardedScriptPropertyStatus::Missing;
                return true;
            }
            const auto & entry = data.entries[data.buckets[*bucket]];
            const auto* shape = object.internalClass();
            const auto & current = shape->propertyTable.d->entries[entry.tableSlot];
            if (current.index != entry.propertyIndex || !current.identifier.isValid() ||
                !current.identifier.isString() || shape->propertyData.at(current.index).isEmpty())
            {
                return false;
            }
            const auto* text = current.identifier.asStringOrSymbol<QV4::Heap::String>();
            if (text->subtype >= QV4::Heap::String::StringType_Complex)
            {
                return false;
            }
            if (!payPropertyWork(result, ledger, GLOBAL_INDEX_CHARACTER_WORK * (name.size() + 1)))
            {
                return true;
            }
            if (text->text().size != name.size() || text->toQString() != name)
            {
                return false;
            }
            retainProperty(result, *object.propertyData(current.index), shape->propertyData.at(current.index));
            return true;
        }

        bool readIndexedOwnProperty(
            const QV4::Object & object, const QString & name, GuardedScriptProperty & result,
            RefinementLedger & ledger, GuardedScriptOwnIndexCache & cache)
        {
            auto* data = GuardedScriptOwnIndexAccess::data(cache);
            if (data == nullptr)
            {
                result.status = cache.status();
                return true;
            }
            if (!payPropertyWork(result, ledger, OWN_INDEX_REGISTRY_WORK))
            {
                return true;
            }
            if (data->ledger != &ledger || object.engine() != data->engine->handle())
            {
                result.status = GuardedScriptPropertyStatus::Unsupported;
                return true;
            }
            const auto* shape = object.internalClass();
            const auto* table = shape->propertyTable.d;
            if (table == nullptr || table->alloc < 0 || table->alloc > MAX_OWN_INDEX_STORAGE_SLOTS ||
                shape->size > MAX_OWN_INDEX_MEMBERS)
            {
                return false;
            }
            for (const auto & index : data->indexes)
            {
                if (index.shape == shape && index.table == table &&
                    index.names == shape->nameMap.d && index.attributes == shape->propertyData.d &&
                    index.allocated == table->alloc && index.members == shape->size)
                {
                    return readOwnIndex(object, index, name, result, ledger);
                }
            }
            if (data->indexes.size() >= MAX_OWN_PROPERTY_INDEXES)
            {
                return false;
            }
            data->indexes.emplace_back();
            const auto prepared = prepareOwnIndex(object, data->indexes.back(), result, ledger);
            if (!prepared)
            {
                return result.status == GuardedScriptPropertyStatus::BudgetExhausted ||
                    result.status == GuardedScriptPropertyStatus::Unsupported;
            }
            return readOwnIndex(object, data->indexes.back(), name, result, ledger);
        }
    }
#endif

    GuardedScriptPropertyStatus GuardedScriptGlobalIndex::status() const
    {
        return m_status;
    }

    GuardedScriptGlobalIndex GuardedScriptGlobalIndex::prepare(
        QJSEngine & engine, std::span<const QString> names, RefinementLedger & ledger)
    {
        GuardedScriptGlobalIndex result;
        if (!ledger.consume(GLOBAL_INSPECTION_WORK, RefinementWork::MasterPreparation))
        {
            result.m_status = GuardedScriptPropertyStatus::BudgetExhausted;
            return result;
        }
        if (names.empty() || names.size() > GUARDED_SCRIPT_MAX_INDEXED_GLOBAL_NAMES)
        {
            return result;
        }
#ifdef SCRIPTSOURCESUPPORT
        const auto* native = engine.handle();
        const auto* object = native ? native->globalObject : nullptr;
        if (object == nullptr || std::strcmp(object->vtable()->className, DATA_OBJECT_CLASS_NAME) != 0)
        {
            return result;
        }
        const auto* shape = object->internalClass();
        const auto* table = shape->propertyTable.d;
        if (table == nullptr || table->alloc < 0 || table->alloc > MAX_GLOBAL_STORAGE_SLOTS)
        {
            return result;
        }
        if (!ledger.consume(GLOBAL_INDEX_RETENTION_WORK * (names.size() + GLOBAL_INDEX_BUCKETS) +
                GLOBAL_INDEX_PREFIX_STORAGE_PASSES * sizeof(Data::prefixes) +
                PROPERTY_MEMBER_WORK * table->alloc, RefinementWork::MasterPreparation))
        {
            result.m_status = GuardedScriptPropertyStatus::BudgetExhausted;
            return result;
        }
        auto data = std::make_shared<Data>();
        data->engine = &engine;
        data->ledger = &ledger;
        data->object = object->d();
        data->shape = shape;
        data->table = table;
        data->allocated = table->alloc;
        data->members = shape->size;
        data->buckets.fill(-1);
        for (const auto & name : names)
        {
            if (name.isEmpty() || name.size() > MAX_PROPERTY_NAME || QV4::stringToArrayIndex(name) != UINT_MAX)
            {
                return result;
            }
            if (!ledger.consume(GLOBAL_INDEX_CHARACTER_WORK * name.size() + globalPrefixWork(name.size()),
                                RefinementWork::MasterPreparation))
            {
                result.m_status = GuardedScriptPropertyStatus::BudgetExhausted;
                return result;
            }
            const auto hash = globalNameHash(name);
            bool funded = false;
            const auto bucket = indexedGlobalName(*data, name, hash, ledger, funded);
            if (!funded)
            {
                result.m_status = GuardedScriptPropertyStatus::BudgetExhausted;
                return result;
            }
            if (!bucket || data->buckets[*bucket] >= 0)
            {
                return result;
            }
            data->buckets[*bucket] = static_cast<std::int32_t>(data->count);
            data->entries[data->count++] = {name, hash, UINT_MAX, UINT_MAX};
            data->maximumName = std::max(data->maximumName, name.size());
            const auto prefix = globalPrefixBucket(name);
            data->prefixes[prefix / GLOBAL_INDEX_PREFIX_BITS] |=
                std::uint64_t{1} << (prefix % GLOBAL_INDEX_PREFIX_BITS);
        }
        for (std::int32_t slot = 0; slot < table->alloc; ++slot)
        {
            const auto & entry = table->entries[slot];
            if (!entry.identifier.isValid() || entry.index >= shape->size ||
                shape->propertyData.at(entry.index).isEmpty() || !entry.identifier.isString())
            {
                continue;
            }
            const auto* text = entry.identifier.asStringOrSymbol<QV4::Heap::String>();
            if (text->subtype >= QV4::Heap::String::StringType_Complex)
            {
                return result;
            }
            const auto length = text->text().size;
            if (length > data->maximumName)
            {
                continue;
            }
            if (length < 0)
            {
                return result;
            }
            if (!ledger.consume(globalPrefixWork(length), RefinementWork::MasterPreparation))
            {
                result.m_status = GuardedScriptPropertyStatus::BudgetExhausted;
                return result;
            }
            const auto prefix = globalPrefixBucket(QStringView(text->text().data(), length));
            if ((data->prefixes[prefix / GLOBAL_INDEX_PREFIX_BITS] &
                    (std::uint64_t{1} << (prefix % GLOBAL_INDEX_PREFIX_BITS))) == 0)
            {
                continue;
            }
            if (!ledger.consume(GLOBAL_INDEX_CHARACTER_WORK * (length + 1),
                                RefinementWork::MasterPreparation))
            {
                result.m_status = GuardedScriptPropertyStatus::BudgetExhausted;
                return result;
            }
            const auto name = text->toQString();
            bool funded = false;
            const auto bucket = indexedGlobalName(*data, name, globalNameHash(name), ledger, funded);
            if (!funded)
            {
                result.m_status = GuardedScriptPropertyStatus::BudgetExhausted;
                return result;
            }
            if (bucket && data->buckets[*bucket] >= 0)
            {
                auto & observed = data->entries[data->buckets[*bucket]];
                if (observed.tableSlot != UINT_MAX)
                {
                    if (observed.propertyIndex != entry.index)
                    {
                        return result;
                    }
                    continue;
                }
                observed.tableSlot = static_cast<std::uint32_t>(slot);
                observed.propertyIndex = entry.index;
            }
        }
        result.m_data = std::move(data);
        result.m_status = GuardedScriptPropertyStatus::Found;
#else
        Q_UNUSED(engine)
        Q_UNUSED(names)
#endif
        return result;
    }

    GuardedScriptProperty GuardedScriptGlobalIndex::find(
        QJSEngine & engine, const QString & name, RefinementLedger & ledger) const
    {
        GuardedScriptProperty result;
        const auto before = ledger.stats().spent();
        if (!m_data || m_status != GuardedScriptPropertyStatus::Found ||
            m_data->engine != &engine || m_data->ledger != &ledger ||
            name.isEmpty() || name.size() > MAX_PROPERTY_NAME)
        {
            return result;
        }
        if (!ledger.consume(GLOBAL_INSPECTION_WORK + GLOBAL_INDEX_CHARACTER_WORK * name.size(),
                            RefinementWork::MasterPreparation))
        {
            result.status = GuardedScriptPropertyStatus::BudgetExhausted;
            return indexedPropertyWork(result, ledger, before);
        }
#ifdef SCRIPTSOURCESUPPORT
        const auto* native = engine.handle();
        const auto* object = native ? native->globalObject : nullptr;
        const auto* shape = object ? object->internalClass() : nullptr;
        const auto* table = shape ? shape->propertyTable.d : nullptr;
        if (object == nullptr || object->d() != m_data->object ||
            shape != m_data->shape || table != m_data->table ||
            table->alloc != m_data->allocated || shape->size != m_data->members)
        {
            return indexedPropertyWork(result, ledger, before);
        }
        bool funded = false;
        const auto bucket = indexedGlobalName(*m_data, name, globalNameHash(name), ledger, funded);
        if (!funded)
        {
            result.status = GuardedScriptPropertyStatus::BudgetExhausted;
        }
        else if (bucket && m_data->buckets[*bucket] >= 0)
        {
            const auto & entry = m_data->entries[m_data->buckets[*bucket]];
            if (entry.tableSlot != UINT_MAX)
            {
                const auto & current = table->entries[entry.tableSlot];
                if (current.index == entry.propertyIndex && current.identifier.isValid() &&
                    current.identifier.isString())
                {
                    const auto* text = current.identifier.asStringOrSymbol<QV4::Heap::String>();
                    if (text->subtype >= QV4::Heap::String::StringType_Complex)
                    {
                        return indexedPropertyWork(result, ledger, before);
                    }
                    if (ledger.consume(GLOBAL_INDEX_CHARACTER_WORK * (name.size() + 1),
                                       RefinementWork::MasterPreparation))
                    {
                        if (text->text().size == name.size() && text->toQString() == name)
                        {
                            retainProperty(result, *object->propertyData(entry.propertyIndex),
                                           shape->propertyData.at(entry.propertyIndex));
                        }
                    }
                    else
                    {
                        result.status = GuardedScriptPropertyStatus::BudgetExhausted;
                    }
                }
            }
            else
            {
                result = guardedScriptGlobalProperty(engine, name, ledger);
            }
        }
        else
        {
            result = guardedScriptGlobalProperty(engine, name, ledger);
        }
#else
        Q_UNUSED(engine)
        Q_UNUSED(name)
#endif
        result.chargedWork = ledger.stats().spent() - before;
        return result;
    }

    struct GuardedNativeMethodContract::Data
    {
        struct Method
        {
            QString name;
            std::int32_t arguments{0};
            std::int32_t index{-1};
        };
        const QMetaObject* metadata{nullptr};
        const RefinementLedger* ledger{nullptr};
        std::vector<Method> methods;
    };

    GuardedNativeReceiverStatus GuardedNativeMethodContract::status() const
    {
        return m_status;
    }

    std::size_t GuardedNativeMethodContract::size() const
    {
        if (!m_data)
        {
            return 0;
        }
        return m_data->methods.size();
    }

    namespace
    {
        GuardedNativeReceiverStatus prepareNativeMetadata(
            const QMetaObject & metadata, std::size_t count, RefinementLedger & ledger)
        {
            if (!ledger.consume(NATIVE_CONTRACT_CONTROL_WORK, RefinementWork::MasterPreparation))
            {
                return GuardedNativeReceiverStatus::BudgetExhausted;
            }
            const QMetaObject* ancestor = &metadata;
            bool qobjectMetadata = false;
            for (std::uint32_t depth = 0; depth < MAX_NATIVE_META_DEPTH && ancestor != nullptr; ++depth)
            {
                qobjectMetadata = qobjectMetadata || ancestor == &QObject::staticMetaObject;
                ancestor = ancestor->superClass();
            }
            if (ancestor != nullptr || !qobjectMetadata)
            {
                return GuardedNativeReceiverStatus::Unsupported;
            }
            const auto properties = metadata.propertyCount();
            const auto methods = metadata.methodCount();
            if (count == 0 || count > MAX_NATIVE_METHOD_REQUIREMENTS ||
                properties < 0 || properties > MAX_NATIVE_META_MEMBERS ||
                methods < 0 || methods > MAX_NATIVE_META_MEMBERS)
            {
                return GuardedNativeReceiverStatus::Unsupported;
            }
            const auto members = static_cast<std::int64_t>(properties) + methods;
            const auto work = NATIVE_META_MEMBER_WORK * (members + 2) +
                static_cast<std::int64_t>(count) * NATIVE_CONTRACT_NAME_WORK;
            if (!ledger.consume(work, RefinementWork::MasterPreparation))
            {
                return GuardedNativeReceiverStatus::BudgetExhausted;
            }
            return GuardedNativeReceiverStatus::Bound;
        }
    }

    GuardedNativeReceiverStatus guardedNativeNameAbsent(
        const QMetaObject & metadata, std::string_view name, RefinementLedger & ledger)
    {
        auto status = prepareNativeMetadata(metadata, 1, ledger);
        if (status != GuardedNativeReceiverStatus::Bound) { return status; }
#ifdef SCRIPTSOURCESUPPORT
        const std::array requested{GuardedNativeMethodRequirement{name, 0}};
        NativeMethodNamespace names;
        status = seedNativeMethodNamespace(requested, names, ledger);
        if (status == GuardedNativeReceiverStatus::Bound)
        {
            status = prepareNativeMethodNamespace(metadata, names, ledger);
        }
        if (status != GuardedNativeReceiverStatus::Bound) { return status; }
        bool funded = false;
        const auto* entry = names.find(QString::fromLatin1(name.data(), name.size()), ledger, funded);
        if (!funded) { return GuardedNativeReceiverStatus::BudgetExhausted; }
        if (entry && !entry->forbidden && entry->dispatches.empty())
        {
            return GuardedNativeReceiverStatus::Bound;
        }
#else
        Q_UNUSED(name)
#endif
        return GuardedNativeReceiverStatus::Unsupported;
    }

    GuardedNativeMethodContract GuardedNativeMethodContract::prepare(
        const QMetaObject & metadata, std::span<const GuardedNativeMethodRequirement> requirements,
        RefinementLedger & ledger)
    {
        GuardedNativeMethodContract result;
        result.m_status = prepareNativeMetadata(metadata, requirements.size(), ledger);
        if (result.m_status != GuardedNativeReceiverStatus::Bound)
        {
            return result;
        }
        result.m_status = GuardedNativeReceiverStatus::Unsupported;
#ifdef SCRIPTSOURCESUPPORT
        auto data = std::make_shared<Data>();
        data->metadata = &metadata;
        data->ledger = &ledger;
        data->methods.reserve(requirements.size());
        NativeMethodNamespace names;
        result.m_status = seedNativeMethodNamespace(requirements, names, ledger);
        if (result.m_status == GuardedNativeReceiverStatus::Bound)
        {
            result.m_status = prepareNativeMethodNamespace(metadata, names, ledger);
        }
        if (result.m_status != GuardedNativeReceiverStatus::Bound)
        {
            return result;
        }
        result.m_status = GuardedNativeReceiverStatus::Unsupported;
        for (const auto & requirement : requirements)
        {
            const auto name = QString::fromLatin1(requirement.name.data(), requirement.name.size());
            bool funded = false;
            auto* entry = names.find(name, ledger, funded);
            if (!funded)
            {
                result.m_status = GuardedNativeReceiverStatus::BudgetExhausted;
                return result;
            }
            if (entry == nullptr || entry->forbidden)
            {
                return result;
            }
            NativeMethodDispatch* matched = nullptr;
            for (auto & dispatch : entry->dispatches)
            {
                if (dispatch.arguments != requirement.arguments)
                {
                    continue;
                }
                if (matched != nullptr || !dispatch.callable || dispatch.selected)
                {
                    return result;
                }
                matched = &dispatch;
            }
            if (matched == nullptr)
            {
                return result;
            }
            matched->selected = true;
            data->methods.push_back({name, requirement.arguments, matched->index});
        }
        result.m_data = std::move(data);
        result.m_status = GuardedNativeReceiverStatus::Bound;
#else
        Q_UNUSED(metadata)
        Q_UNUSED(requirements)
#endif
        return result;
    }


    GuardedNativeReceiverStatus GuardedNativeMethodContract::canonicalOrder(RefinementLedger & ledger) const
    {
        if (!m_data || m_status != GuardedNativeReceiverStatus::Bound || m_data->ledger != &ledger)
        {
            return GuardedNativeReceiverStatus::Unsupported;
        }
        if (!ledger.consume(NATIVE_CONTRACT_CONTROL_WORK +
                NATIVE_CONTRACT_KEY_ENTRY_WORK * m_data->methods.size(), RefinementWork::MasterPreparation))
        {
            return GuardedNativeReceiverStatus::BudgetExhausted;
        }
        for (std::size_t index = 1; index < m_data->methods.size(); ++index)
        {
            const auto & left = m_data->methods[index - 1];
            const auto & right = m_data->methods[index];
            if (left.name.size() == right.name.size() && left.arguments == right.arguments &&
                !ledger.consume(NATIVE_CONTRACT_KEY_CHARACTER_WORK * left.name.size(), RefinementWork::MasterPreparation))
            {
                return GuardedNativeReceiverStatus::BudgetExhausted;
            }
            if (!guardedNativeMethodPrecedes(left, right)) { return GuardedNativeReceiverStatus::Unsupported; }
        }
        return GuardedNativeReceiverStatus::Bound;
    }

    GuardedNativeMethodContract GuardedNativeMethodContract::project(
        std::span<const GuardedNativeMethodRequirement> requirements, RefinementLedger & ledger) const
    {
        GuardedNativeMethodContract result;
        if (!m_data || m_status != GuardedNativeReceiverStatus::Bound || m_data->ledger != &ledger ||
            requirements.empty() || requirements.size() > MAX_NATIVE_METHOD_REQUIREMENTS ||
            m_data->methods.empty() || m_data->methods.size() > MAX_NATIVE_METHOD_REQUIREMENTS)
        {
            return result;
        }
        const auto storage = NATIVE_CONTRACT_CONTROL_WORK + 4 * sizeof(Data) +
            4 * requirements.size() * sizeof(Data::Method) + MAX_NATIVE_METHOD_REQUIREMENTS * sizeof(bool);
        if (!ledger.consume(storage, RefinementWork::MasterPreparation))
        {
            result.m_status = GuardedNativeReceiverStatus::BudgetExhausted;
            return result;
        }
        auto data = std::make_shared<Data>();
        data->metadata = m_data->metadata;
        data->ledger = &ledger;
        data->methods.reserve(requirements.size());
        std::array<bool, MAX_NATIVE_METHOD_REQUIREMENTS> selected{};
        for (const auto & requested : requirements)
        {
            if (requested.name.empty() || requested.name.size() > static_cast<std::size_t>(MAX_PROPERTY_NAME) ||
                requested.arguments < 0 || requested.arguments > MAX_NATIVE_METHOD_ARGUMENTS)
            {
                return result;
            }
            struct Key
            {
                QLatin1StringView name;
                std::int32_t arguments;
            };
            const Key key{QLatin1StringView(requested.name.data(), requested.name.size()), requested.arguments};
            std::size_t first = 0;
            auto last = m_data->methods.size();
            while (first < last)
            {
                if (!ledger.consume(NATIVE_CONTRACT_KEY_ENTRY_WORK, RefinementWork::MasterPreparation))
                {
                    result.m_status = GuardedNativeReceiverStatus::BudgetExhausted;
                    return result;
                }
                const auto middle = first + (last - first) / 2;
                const auto & stored = m_data->methods[middle];
                if (stored.name.size() == key.name.size() && stored.arguments == key.arguments &&
                    !ledger.consume(NATIVE_CONTRACT_KEY_CHARACTER_WORK * key.name.size(), RefinementWork::MasterPreparation))
                {
                    result.m_status = GuardedNativeReceiverStatus::BudgetExhausted;
                    return result;
                }
                if (guardedNativeMethodPrecedes(stored, key)) { first = middle + 1; }
                else { last = middle; }
            }
            if (!ledger.consume(NATIVE_CONTRACT_KEY_ENTRY_WORK +
                    NATIVE_CONTRACT_KEY_CHARACTER_WORK * requested.name.size(), RefinementWork::MasterPreparation))
            {
                result.m_status = GuardedNativeReceiverStatus::BudgetExhausted;
                return result;
            }
            if (first == m_data->methods.size() || selected[first] ||
                m_data->methods[first].arguments != requested.arguments || m_data->methods[first].name != key.name)
            {
                return result;
            }
            selected[first] = true;
            data->methods.push_back(m_data->methods[first]);
        }
        result.m_data = std::move(data);
        result.m_status = GuardedNativeReceiverStatus::Bound;
        return result;
    }

    GuardedNativeMethodContract GuardedNativeMethodCache::prepare(
        const QMetaObject & metadata, std::span<const GuardedNativeMethodRequirement> requirements,
        RefinementLedger & ledger)
    {
        GuardedNativeMethodContract refused;
        if (&ledger != &m_ledger || requirements.empty() || requirements.size() > MAX_NATIVE_METHOD_REQUIREMENTS)
        {
            return refused;
        }
        const auto keyWork = NATIVE_CONTRACT_CONTROL_WORK +
            NATIVE_CONTRACT_KEY_ENTRY_WORK * requirements.size();
        if (!ledger.consume(keyWork, RefinementWork::MasterPreparation))
        {
            refused.m_status = GuardedNativeReceiverStatus::BudgetExhausted;
            return refused;
        }
        for (const auto & requirement : requirements)
        {
            if (requirement.name.empty() || requirement.name.size() > static_cast<std::size_t>(MAX_PROPERTY_NAME))
            {
                return refused;
            }
        }
        for (const auto & contract : m_contracts)
        {
            if (!ledger.consume(NATIVE_CONTRACT_KEY_ENTRY_WORK, RefinementWork::MasterPreparation))
            {
                refused.m_status = GuardedNativeReceiverStatus::BudgetExhausted;
                return refused;
            }
            const auto & data = *contract.m_data;
            if (data.metadata != &metadata || data.methods.size() != requirements.size())
            {
                continue;
            }
            bool same = true;
            for (std::size_t index = 0; index < requirements.size(); ++index)
            {
                const auto & requested = requirements[index];
                const auto & stored = data.methods[index];
                if (stored.arguments != requested.arguments ||
                    stored.name.size() != static_cast<qsizetype>(requested.name.size()))
                {
                    same = false;
                    break;
                }
                const auto comparisonWork = NATIVE_CONTRACT_KEY_ENTRY_WORK +
                    NATIVE_CONTRACT_KEY_CHARACTER_WORK * requested.name.size();
                if (!ledger.consume(comparisonWork, RefinementWork::MasterPreparation))
                {
                    refused.m_status = GuardedNativeReceiverStatus::BudgetExhausted;
                    return refused;
                }
                if (stored.name != QLatin1StringView(requested.name.data(), requested.name.size()))
                {
                    same = false;
                    break;
                }
            }
            if (same)
            {
                return contract;
            }
        }
        GuardedNativeMethodContract contract;
        const GuardedNativeMethodUnion* declaration = nullptr;
        if (!m_unions.empty())
        {
            if (m_unions.size() > MAX_NATIVE_CACHED_CONTRACTS) { return refused; }
            if (!ledger.consume(NATIVE_CONTRACT_KEY_ENTRY_WORK * (m_unions.size() + m_descriptions.size() + 1),
                                RefinementWork::MasterPreparation))
            {
                refused.m_status = GuardedNativeReceiverStatus::BudgetExhausted;
                return refused;
            }
            for (const auto & candidate : m_unions)
            {
                if (candidate.metadata == &metadata)
                {
                    if (declaration != nullptr) { return refused; }
                    declaration = &candidate;
                }
            }
        }
        const GuardedNativeMethodContract* description = nullptr;
        if (declaration != nullptr)
        {
            for (const auto & stored : m_descriptions)
            {
                if (stored.m_data->metadata == &metadata) { description = &stored; break; }
            }
            if (description == nullptr && m_contracts.size() + m_descriptions.size() < MAX_NATIVE_CACHED_CONTRACTS)
            {
                auto prepared = GuardedNativeMethodContract::prepare(metadata, declaration->methods, ledger);
                if (prepared.status() != GuardedNativeReceiverStatus::Bound) { return prepared; }
                const auto canonical = prepared.canonicalOrder(ledger);
                if (canonical != GuardedNativeReceiverStatus::Bound)
                {
                    refused.m_status = canonical;
                    return refused;
                }
                const auto retention = NATIVE_CONTRACT_CONTROL_WORK +
                    m_unions.size() * sizeof(GuardedNativeMethodContract);
                if (!ledger.consume(retention, RefinementWork::MasterPreparation))
                {
                    refused.m_status = GuardedNativeReceiverStatus::BudgetExhausted;
                    return refused;
                }
                if (m_descriptions.empty()) { m_descriptions.reserve(m_unions.size()); }
                m_descriptions.push_back(std::move(prepared));
                description = &m_descriptions.back();
            }
        }
        contract = description ? description->project(requirements, ledger)
                               : GuardedNativeMethodContract::prepare(metadata, requirements, ledger);
        if (contract.status() != GuardedNativeReceiverStatus::Bound ||
            m_contracts.size() + m_descriptions.size() == MAX_NATIVE_CACHED_CONTRACTS)
        {
            return contract;
        }
        constexpr auto retention = NATIVE_CONTRACT_CONTROL_WORK + MAX_NATIVE_CACHED_CONTRACTS * sizeof(GuardedNativeMethodContract);
        if (!ledger.consume(retention, RefinementWork::MasterPreparation))
        {
            refused.m_status = GuardedNativeReceiverStatus::BudgetExhausted;
            return refused;
        }
        if (m_contracts.empty())
        {
            m_contracts.reserve(MAX_NATIVE_CACHED_CONTRACTS);
        }
        m_contracts.push_back(contract);
        return contract;
    }

    static GuardedNativeReceiverStatus authenticateNativeReceiver(
        QJSEngine & engine, const QJSValue* value, const GuardedScriptValueView* view,
        const QObject & expected, const QMetaObject & permitted, RefinementLedger & ledger)
    {
        GuardedScriptProperty charge;
        if (!ledger.consume(PROPERTY_METADATA_WORK, RefinementWork::MasterPreparation))
        {
            return GuardedNativeReceiverStatus::BudgetExhausted;
        }
        charge.chargedWork = PROPERTY_METADATA_WORK;
#ifdef SCRIPTSOURCESUPPORT
        QV4::Value borrowed = QV4::Value::undefinedValue();
        const QV4::QObjectWrapper* wrapper = nullptr;
        if (value != nullptr)
        {
            wrapper = QJSValuePrivate::asManagedType<QV4::QObjectWrapper>(value);
        }
        else if (view != nullptr)
        {
            borrowed = GuardedScriptObservationAccess::read(*view);
            wrapper = borrowed.as<QV4::QObjectWrapper>();
        }
        if (wrapper == nullptr || wrapper->vtable() != QV4::QObjectWrapper::staticVTable() ||
            wrapper->object() != &expected || wrapper->engine() != engine.handle() ||
            wrapper->engine()->qmlEngine() != nullptr)
        {
            return GuardedNativeReceiverStatus::Unsupported;
        }
        if (!supportedNativeMetadata(expected, permitted, charge, ledger))
        {
            if (charge.status == GuardedScriptPropertyStatus::BudgetExhausted)
            {
                return GuardedNativeReceiverStatus::BudgetExhausted;
            }
            return GuardedNativeReceiverStatus::Unsupported;
        }
        return GuardedNativeReceiverStatus::Bound;
#else
        Q_UNUSED(engine)
        Q_UNUSED(value)
        Q_UNUSED(view)
        Q_UNUSED(expected)
        Q_UNUSED(permitted)
        return GuardedNativeReceiverStatus::Unsupported;
#endif
    }

    GuardedNativeReceiverStatus GuardedNativeMethodContract::authenticate(
        QJSEngine & engine, const QJSValue & value, const QObject & expected, RefinementLedger & ledger) const
    {
        if (!m_data || m_status != GuardedNativeReceiverStatus::Bound || m_data->ledger != &ledger)
        {
            return GuardedNativeReceiverStatus::Unsupported;
        }
        return authenticateNativeReceiver(engine, &value, nullptr, expected, *m_data->metadata, ledger);
    }

    GuardedNativeReceiverStatus GuardedNativeMethodContract::authenticate(
        QJSEngine & engine, const GuardedScriptValueView & view, const QObject & expected,
        RefinementLedger & ledger) const
    {
        if (!m_data || m_status != GuardedNativeReceiverStatus::Bound || m_data->ledger != &ledger)
        {
            return GuardedNativeReceiverStatus::Unsupported;
        }
        return authenticateNativeReceiver(engine, nullptr, &view, expected, *m_data->metadata, ledger);
    }

    GuardedNativeReceiverStatus GuardedNativeMethodContract::authenticateReturned(
        QJSEngine & engine, const QObject & expected, RefinementLedger & ledger) const
    {
        if (!ledger.consume(PROPERTY_METADATA_WORK, RefinementWork::MasterPreparation))
        {
            return GuardedNativeReceiverStatus::BudgetExhausted;
        }
        if (!m_data || m_status != GuardedNativeReceiverStatus::Bound || m_data->ledger != &ledger ||
            engine.thread() != QThread::currentThread() || expected.thread() != QThread::currentThread())
        {
            return GuardedNativeReceiverStatus::Unsupported;
        }
#ifdef SCRIPTSOURCESUPPORT
        if (engine.handle()->qmlEngine() != nullptr)
        {
            return GuardedNativeReceiverStatus::Unsupported;
        }
        GuardedScriptProperty charge;
        if (!supportedNativeMetadata(expected, *m_data->metadata, charge, ledger))
        {
            if (charge.status == GuardedScriptPropertyStatus::BudgetExhausted)
            {
                return GuardedNativeReceiverStatus::BudgetExhausted;
            }
            return GuardedNativeReceiverStatus::Unsupported;
        }
        const auto infos = m_data->metadata->classInfoCount();
        if (infos < 0 || infos > MAX_NATIVE_META_MEMBERS)
        {
            return GuardedNativeReceiverStatus::Unsupported;
        }
        if (!ledger.consume(NATIVE_META_MEMBER_WORK * (infos + 1), RefinementWork::MasterPreparation))
        {
            return GuardedNativeReceiverStatus::BudgetExhausted;
        }
        for (std::int32_t index = 0; index < infos; ++index)
        {
            const auto* name = m_data->metadata->classInfo(index).name();
            qsizetype length = 0;
            while (length <= MAX_PROPERTY_NAME && name[length] != '\0')
            {
                ++length;
            }
            if (length > MAX_PROPERTY_NAME ||
                QByteArrayView(name, length) == QByteArrayView(NATIVE_WRAPPER_FACTORY_INFO))
            {
                return GuardedNativeReceiverStatus::Unsupported;
            }
        }
        const auto* data = QQmlData::get(QObjectPrivate::get(&expected));
        if (data == nullptr || !data->indestructible || !data->explicitIndestructibleSet)
        {
            return GuardedNativeReceiverStatus::Unsupported;
        }
        if (!data->jsWrapper.isUndefined())
        {
            if (data->jsEngineId != engine.handle()->m_engineId)
            {
                return GuardedNativeReceiverStatus::Unsupported;
            }
            const auto value = QV4::Value::fromReturnedValue(data->jsWrapper.value());
            const auto* wrapper = value.as<QV4::QObjectWrapper>();
            if (wrapper == nullptr || wrapper->vtable() != QV4::QObjectWrapper::staticVTable() ||
                wrapper->object() != &expected || wrapper->engine() != engine.handle())
            {
                return GuardedNativeReceiverStatus::Unsupported;
            }
        }
        else if (data->hasTaintedV4Object)
        {
            return GuardedNativeReceiverStatus::Unsupported;
        }
        return GuardedNativeReceiverStatus::Bound;
#else
        Q_UNUSED(engine)
        Q_UNUSED(expected)
        return GuardedNativeReceiverStatus::Unsupported;
#endif
    }

    // Mirrors every charge authenticateReturned can make for this contract's metadata.
    std::optional<std::int64_t> GuardedNativeMethodContract::returnedWork() const
    {
        if (!m_data || m_status != GuardedNativeReceiverStatus::Bound || m_data->metadata == nullptr)
        {
            return std::nullopt;
        }
        std::int64_t depth = 0;
        for (const auto* meta = m_data->metadata; meta != nullptr && depth < MAX_NATIVE_META_DEPTH;
             meta = meta->superClass())
        {
            ++depth;
        }
        const auto infos = std::clamp(m_data->metadata->classInfoCount(), 0, MAX_NATIVE_META_MEMBERS);
        return PROPERTY_METADATA_WORK + NATIVE_LIFETIME_WORK + 2 * depth * PROPERTY_METADATA_WORK +
            NATIVE_META_MEMBER_WORK * (infos + 1);
    }

    GuardedNativeReceiverObservation guardedScriptNativeReceiver(
        QJSEngine & engine, const GuardedScriptValueView & view, const QMetaObject & permitted,
        RefinementLedger & ledger)
    {
        GuardedNativeReceiverObservation result;
        const auto before = ledger.stats().spent();
        if (!ledger.consume(PROPERTY_METADATA_WORK, RefinementWork::MasterPreparation))
        {
            result.status = GuardedNativeReceiverStatus::BudgetExhausted;
            return result;
        }
        result.chargedWork = ledger.stats().spent() - before;
#ifdef SCRIPTSOURCESUPPORT
        if (engine.thread() != QThread::currentThread())
        {
            return result;
        }
        const auto value = GuardedScriptObservationAccess::read(view);
        const auto* wrapper = value.as<QV4::QObjectWrapper>();
        if (wrapper == nullptr || wrapper->vtable() != QV4::QObjectWrapper::staticVTable() ||
            wrapper->engine() != engine.handle() || wrapper->engine()->qmlEngine() != nullptr)
        {
            return result;
        }
        const auto* receiver = wrapper->object();
        if (receiver == nullptr || receiver->thread() != QThread::currentThread())
        {
            return result;
        }
        GuardedScriptProperty charge;
        if (!supportedNativeMetadata(*receiver, permitted, charge, ledger))
        {
            if (charge.status == GuardedScriptPropertyStatus::BudgetExhausted)
            {
                result.status = GuardedNativeReceiverStatus::BudgetExhausted;
            }
        }
        else
        {
            result.status = GuardedNativeReceiverStatus::Bound;
            result.receiver = receiver;
        }
        result.chargedWork = ledger.stats().spent() - before;
#else
        Q_UNUSED(engine)
        Q_UNUSED(view)
        Q_UNUSED(permitted)
#endif
        return result;
    }

    GuardedScriptPointValue guardedScriptPointValue(
        QJSEngine & engine, const GuardedScriptValueView & view, RefinementLedger & ledger)
    {
        constexpr std::int64_t POINT_VALUE_WORK = 256;
        GuardedScriptPointValue result;
        if (!ledger.consume(POINT_VALUE_WORK, RefinementWork::MasterPreparation))
        {
            result.status = GuardedNativeReceiverStatus::BudgetExhausted;
            return result;
        }
        result.chargedWork = POINT_VALUE_WORK;
#ifdef SCRIPTSOURCESUPPORT
        if (engine.thread() != QThread::currentThread())
        {
            return result;
        }
        const auto value = GuardedScriptObservationAccess::read(view);
        const auto* wrapper = value.as<QV4::QQmlValueTypeWrapper>();
        if (wrapper == nullptr || wrapper->vtable() != QV4::QQmlValueTypeWrapper::staticVTable() ||
            wrapper->engine() != engine.handle() || wrapper->engine()->qmlEngine() != nullptr ||
            wrapper->d()->isReference() || wrapper->d()->flags() != QV4::Heap::ReferenceObject::NoFlag ||
            wrapper->d()->metaObject() != &QQmlPointFValueType::staticMetaObject ||
            wrapper->d()->metaType() != QMetaType::fromType<QPointF>() || wrapper->d()->gadgetPtr() == nullptr)
        {
            return result;
        }
        const auto* point = static_cast<const QPointF*>(wrapper->d()->gadgetPtr());
        if (!std::isfinite(point->x()) || !std::isfinite(point->y()))
        {
            return result;
        }
        result.x = point->x();
        result.y = point->y();
        result.status = GuardedNativeReceiverStatus::Bound;
#else
        Q_UNUSED(engine)
        Q_UNUSED(view)
#endif
        return result;
    }

    GuardedNativeValueConstructorAgreement guardedScriptQtValueConstructors(
        QJSEngine & engine, RefinementLedger & ledger)
    {
        GuardedNativeValueConstructorAgreement result;
        const auto before = ledger.stats().spent();
        if (!ledger.consume(VALUE_CONSTRUCTOR_CONTROL_WORK, RefinementWork::MasterPreparation))
        {
            result.status = GuardedNativeReceiverStatus::BudgetExhausted;
            return result;
        }
        result.chargedWork = ledger.stats().spent() - before;
#ifdef SCRIPTSOURCESUPPORT
        const auto global = guardedScriptGlobalProperty(engine, QT_NAMESPACE, ledger);
        if (global.status != GuardedScriptPropertyStatus::Found)
        {
            if (global.status == GuardedScriptPropertyStatus::BudgetExhausted)
            {
                result.status = GuardedNativeReceiverStatus::BudgetExhausted;
            }
            result.chargedWork = ledger.stats().spent() - before;
            return result;
        }
        const auto observed = guardedScriptNativeReceiver(engine, global.value, QtObject::staticMetaObject, ledger);
        if (observed.status != GuardedNativeReceiverStatus::Bound || observed.receiver == nullptr)
        {
            result.status = observed.status;
            result.chargedWork = ledger.stats().spent() - before;
            return result;
        }
        constexpr std::array methods{
            GuardedNativeMethodRequirement{"point", 2},
            GuardedNativeMethodRequirement{"rect", 4},
        };
        result.methods = GuardedNativeMethodContract::prepare(QtObject::staticMetaObject, methods, ledger);
        result.status = result.methods.status();
        if (result.status == GuardedNativeReceiverStatus::Bound)
        {
            result.status = result.methods.authenticate(engine, global.value, *observed.receiver, ledger);
        }
        result.chargedWork = ledger.stats().spent() - before;
#else
        Q_UNUSED(engine)
#endif
        return result;
    }

    bool GuardedScriptFunctionImage::sameImplementation(const GuardedScriptFunctionImage & other) const
    {
        return status == GuardedScriptFunctionStatus::Verified &&
            other.status == GuardedScriptFunctionStatus::Verified && kind == other.kind &&
            !implementation.isEmpty() && implementation == other.implementation &&
            compilationName == other.compilationName && functionOffset == other.functionOffset;
    }

    GuardedScriptValueKind GuardedScriptValueView::kind() const
    {
        return m_kind;
    }

    std::optional<double> GuardedScriptValueView::number() const
    {
        if (m_kind != GuardedScriptValueKind::Number)
        {
            return std::nullopt;
        }
#ifdef SCRIPTSOURCESUPPORT
        return GuardedScriptObservationAccess::read(*this).asDouble();
#else
        return std::nullopt;
#endif
    }

    bool GuardedScriptValueView::isNull() const
    {
        return m_kind == GuardedScriptValueKind::Null;
    }

    struct GuardedScriptImageCacheData
    {
        const QJSEngine* engine{nullptr};
        const RefinementLedger* ledger{nullptr};
#ifdef SCRIPTSOURCESUPPORT
        struct Entry
        {
            QQmlRefPointer<QV4::ExecutableCompilationUnit> owner;
            const QV4::CompiledData::Unit* image{nullptr};
            std::uint32_t size{0};
            QByteArray implementation;
            QString compilationName;
        };
        std::map<std::uintptr_t, Entry> entries;
#endif
    };

    struct GuardedScriptImageCacheAccess
    {
        static GuardedScriptImageCacheData* data(GuardedScriptImageCache* cache)
        {
            return cache == nullptr ? nullptr : cache->m_data.get();
        }
    };

    GuardedScriptImageCache::GuardedScriptImageCache(QJSEngine & engine, RefinementLedger & ledger)
    {
        if (ledger.consume(IMAGE_CACHE_CONSTRUCTION_WORK, RefinementWork::MasterPreparation))
        {
            m_data = std::make_unique<GuardedScriptImageCacheData>();
            m_data->engine = &engine;
            m_data->ledger = &ledger;
            m_status = GuardedScriptFunctionStatus::Verified;
        }
    }

    GuardedScriptImageCache::~GuardedScriptImageCache() = default;

    GuardedScriptFunctionStatus GuardedScriptImageCache::status() const
    {
        return m_status;
    }

    static std::int64_t imageCacheLookupWork(std::size_t entries)
    {
        std::int64_t depth = 1;
        for (auto remaining = entries + 1; remaining > 1; remaining >>= 1)
        {
            ++depth;
        }
        return IMAGE_CACHE_TREE_WORK * (2 * depth + 1);
    }

    static GuardedScriptFunctionImage inspectScriptImage(
        const QJSValue* function, const GuardedScriptValueView* view, RefinementLedger & ledger,
        GuardedScriptImageCache* cache = nullptr)
    {
        GuardedScriptFunctionImage result;
        if (!ledger.consume(FUNCTION_INSPECTION_WORK, RefinementWork::MasterPreparation))
        {
            result.status = GuardedScriptFunctionStatus::BudgetExhausted;
            return result;
        }
        result.chargedWork = FUNCTION_INSPECTION_WORK;
        auto* cacheData = GuardedScriptImageCacheAccess::data(cache);
        if (cache != nullptr && cacheData == nullptr)
        {
            result.status = cache->status();
            return result;
        }
        if (cacheData != nullptr && cacheData->ledger != &ledger)
        {
            return result;
        }
#ifdef SCRIPTSOURCESUPPORT
        QV4::Value borrowed = QV4::Value::undefinedValue();
        const QV4::FunctionObject* object = nullptr;
        if (function != nullptr)
        {
            object = QJSValuePrivate::asManagedType<QV4::FunctionObject>(function);
        }
        else if (view != nullptr)
        {
            borrowed = GuardedScriptObservationAccess::read(*view);
            object = borrowed.as<QV4::FunctionObject>();
        }
        if (object == nullptr)
        {
            return result;
        }
        if (cacheData != nullptr && object->engine() != cacheData->engine->handle())
        {
            return result;
        }
        if (const auto* scripted = object->as<QV4::JavaScriptFunctionObject>())
        {
            const auto* code = scripted->function();
            if (code == nullptr || code->compiledFunction == nullptr ||
                code->kind != QV4::Function::Kind::JsUntyped ||
                std::strcmp(scripted->vtable()->className, SCRIPT_FUNCTION_CLASS_NAME) != 0)
            {
                return result;
            }
            const auto* image = code->executableCompilationUnit()->unitData();
            const auto size = static_cast<std::uint32_t>(image->unitSize);
            constexpr auto semanticStart = offsetof(QV4::CompiledData::Unit, flags);
            if (size < sizeof(QV4::CompiledData::Unit) || size > MAX_COMPILATION_BYTES ||
                !(image->flags & QV4::CompiledData::Unit::IsJavascript) ||
                (image->flags & QV4::CompiledData::Unit::IsESModule))
            {
                return result;
            }
            const auto imageStart = reinterpret_cast<std::uintptr_t>(image);
            const auto functionStart = reinterpret_cast<std::uintptr_t>(code->compiledFunction);
            if (functionStart < imageStart || functionStart - imageStart >= size)
            {
                return result;
            }
            const auto key = reinterpret_cast<std::uintptr_t>(code->executableCompilationUnit());
            if (cacheData != nullptr)
            {
                const auto lookupWork = imageCacheLookupWork(cacheData->entries.size());
                if (!ledger.consume(lookupWork, RefinementWork::MasterPreparation))
                {
                    result.status = GuardedScriptFunctionStatus::BudgetExhausted;
                    return result;
                }
                result.chargedWork += lookupWork;
                const auto cached = cacheData->entries.find(key);
                if (cached != cacheData->entries.end())
                {
                    const auto & entry = cached->second;
                    if (entry.image != image || entry.size != size ||
                        entry.owner.data() != code->executableCompilationUnit())
                    {
                        return result;
                    }
                    result.implementation = entry.implementation;
                    result.compilationName = entry.compilationName;
                    result.functionOffset = static_cast<std::uint32_t>(functionStart - imageStart);
                    result.kind = GuardedScriptFunctionKind::Script;
                    result.status = GuardedScriptFunctionStatus::Verified;
                    return result;
                }
                if (cacheData->entries.size() == MAX_CACHED_COMPILATION_UNITS)
                {
                    return result;
                }
            }
            const auto* name = compilationName(*image, size);
            if (name == nullptr)
            {
                return result;
            }
            const auto work = IMAGE_BLOCK_PASSES *
                static_cast<std::int64_t>((size + HASH_BLOCK_BYTES - 1) / HASH_BLOCK_BYTES);
            if (!ledger.consume(work, RefinementWork::MasterPreparation))
            {
                result.status = GuardedScriptFunctionStatus::BudgetExhausted;
                return result;
            }
            result.chargedWork += work;
            // Compiled code, not the replaceable filename-to-source-text cache, is authority.
            const auto* bytes = reinterpret_cast<const char*>(image);
            result.implementation = QCryptographicHash::hash(
                QByteArrayView(bytes + semanticStart, size - semanticStart), QCryptographicHash::Sha256);
            const auto nameWork = COMPILATION_NAME_WORK +
                COMPILATION_NAME_CHARACTER_WORK * static_cast<std::int32_t>(name->size);
            if (!ledger.consume(nameWork, RefinementWork::MasterPreparation))
            {
                result.status = GuardedScriptFunctionStatus::BudgetExhausted;
                return result;
            }
            result.chargedWork += nameWork;
            result.compilationName = copyCompilationName(*name);
            if (cacheData != nullptr)
            {
                const auto retentionWork = IMAGE_CACHE_RETENTION_WORK +
                    imageCacheLookupWork(cacheData->entries.size());
                if (!ledger.consume(retentionWork, RefinementWork::MasterPreparation))
                {
                    result.status = GuardedScriptFunctionStatus::BudgetExhausted;
                    return result;
                }
                result.chargedWork += retentionWork;
                GuardedScriptImageCacheData::Entry entry;
                entry.owner = QQmlRefPointer<QV4::ExecutableCompilationUnit>(code->executableCompilationUnit());
                entry.image = image;
                entry.size = size;
                entry.implementation = result.implementation;
                entry.compilationName = result.compilationName;
                cacheData->entries.emplace(key, std::move(entry));
            }
            result.functionOffset = static_cast<std::uint32_t>(functionStart - imageStart);
            result.kind = GuardedScriptFunctionKind::Script;
            result.status = GuardedScriptFunctionStatus::Verified;
            return result;
        }
        if (const auto* builtin = object->as<QV4::DynamicFunctionObject>())
        {
            if (builtin->vtable() != QV4::DynamicFunctionObject::staticVTable())
            {
                return result;
            }
            const auto entry = builtin->d()->jsCall;
            if (entry == nullptr)
            {
                return result;
            }
            result.implementation.resize(sizeof(entry));
            std::memcpy(result.implementation.data(), &entry, sizeof(entry));
            result.kind = GuardedScriptFunctionKind::Builtin;
            result.status = GuardedScriptFunctionStatus::Verified;
        }
#else
        Q_UNUSED(function)
        Q_UNUSED(view)
#endif
        return result;
    }

    static GuardedScriptProperty inspectScriptProperty(
        const QJSValue* object, const GuardedScriptValueView* view,
        const QString & name, RefinementLedger & ledger, GuardedScriptOwnIndexCache* cache)
    {
        GuardedScriptProperty result;
        if (name.isEmpty() || name.size() > MAX_PROPERTY_NAME)
        {
            return result;
        }
        const auto keyWork = static_cast<std::int64_t>(name.size()) * sizeof(QChar);
        if (!ledger.consume(keyWork, RefinementWork::MasterPreparation))
        {
            result.status = GuardedScriptPropertyStatus::BudgetExhausted;
            return result;
        }
        result.chargedWork = keyWork;
#ifdef SCRIPTSOURCESUPPORT
        QV4::Value cursor = QV4::Value::undefinedValue();
        const auto arrayIndex = QV4::stringToArrayIndex(name);
        for (std::uint32_t depth = 0; depth < MAX_PROPERTY_PROTOTYPES; ++depth)
        {
            if (!payPropertyWork(result, ledger, PROPERTY_METADATA_WORK))
            {
                return result;
            }
            result.prototypeDepth = depth;
            const QV4::Object* data = nullptr;
            if (depth == 0 && object != nullptr)
            {
                data = QJSValuePrivate::asManagedType<QV4::Object>(object);
            }
            else
            {
                if (depth == 0 && view != nullptr)
                {
                    cursor = GuardedScriptObservationAccess::read(*view);
                }
                data = cursor.as<QV4::Object>();
            }
            if (data == nullptr ||
                (std::strcmp(data->vtable()->className, DATA_OBJECT_CLASS_NAME) != 0 &&
                 std::strcmp(data->vtable()->className, DATA_ARRAY_CLASS_NAME) != 0))
            {
                result.status = GuardedScriptPropertyStatus::Unsupported;
                return result;
            }
            readOwnProperty(*data, name, arrayIndex, result, ledger, cache);
            if (result.status != GuardedScriptPropertyStatus::Missing)
            {
                return result;
            }
            const auto* prototype = data->d()->prototype();
            if (prototype == nullptr)
            {
                result.status = GuardedScriptPropertyStatus::Missing;
                return result;
            }
            cursor = QV4::Value::fromHeapObject(const_cast<QV4::Heap::Object*>(prototype));
        }
        result.status = GuardedScriptPropertyStatus::Unsupported;
#else
        Q_UNUSED(object)
        Q_UNUSED(view)
        Q_UNUSED(cache)
#endif
        return result;
    }

    GuardedScriptProperty guardedScriptBuiltinProperty(
        const GuardedScriptValueView & view, const QString & name, RefinementLedger & ledger)
    {
        GuardedScriptProperty result;
        if (name.isEmpty() || name.size() > MAX_PROPERTY_NAME)
        {
            return result;
        }
        if (!payPropertyWork(result, ledger, PROPERTY_METADATA_WORK + name.size() * sizeof(QChar)))
        {
            return result;
        }
#ifdef SCRIPTSOURCESUPPORT
        const auto value = GuardedScriptObservationAccess::read(view);
        const auto* object = value.as<QV4::Object>();
        if (object == nullptr)
        {
            return result;
        }
        const auto* className = object->vtable()->className;
        // Delegated callbacks may use an ordinary function's own apply or call property.
        const bool functionDispatch = (name == QStringLiteral("apply") || name == QStringLiteral("call")) &&
            (std::strcmp(className, SCRIPT_FUNCTION_CLASS_NAME) == 0 ||
             object->d() == object->engine()->functionPrototype()->d());
        if (!functionDispatch && std::strcmp(className, DATA_OBJECT_CLASS_NAME) != 0 &&
            std::strcmp(className, DATA_ARRAY_CLASS_NAME) != 0 &&
            std::strcmp(className, MATH_OBJECT_CLASS_NAME) != 0 &&
            std::strcmp(className, STRING_OBJECT_CLASS_NAME) != 0 &&
            std::strcmp(className, NUMBER_OBJECT_CLASS_NAME) != 0)
        {
            return result;
        }
        readOwnProperty(*object, name, QV4::stringToArrayIndex(name), result, ledger);
#else
        Q_UNUSED(view)
#endif
        return result;
    }

    static GuardedScriptContextInspection inspectContextOperands(
        const QJSValue* function, const GuardedScriptValueView* view, RefinementLedger & ledger)
    {
        GuardedScriptContextInspection result;
        if (!ledger.consume(FUNCTION_INSPECTION_WORK, RefinementWork::MasterPreparation))
        {
            result.status = GuardedScriptContextOperands::BudgetExhausted;
            return result;
        }
        result.chargedWork = FUNCTION_INSPECTION_WORK;
#ifdef SCRIPTSOURCESUPPORT
        QV4::Value borrowed = QV4::Value::undefinedValue();
        const QV4::FunctionObject* object = nullptr;
        if (function != nullptr)
        {
            object = QJSValuePrivate::asManagedType<QV4::FunctionObject>(function);
        }
        else if (view != nullptr)
        {
            borrowed = GuardedScriptObservationAccess::read(*view);
            object = borrowed.as<QV4::FunctionObject>();
        }
        const QV4::JavaScriptFunctionObject* scripted = nullptr;
        if (object != nullptr)
        {
            scripted = object->as<QV4::JavaScriptFunctionObject>();
        }
        if (scripted == nullptr ||
            std::strcmp(scripted->vtable()->className, SCRIPT_FUNCTION_CLASS_NAME) != 0)
        {
            return result;
        }
        const auto* code = scripted->function();
        if (code == nullptr || code->compiledFunction == nullptr || code->kind != QV4::Function::Kind::JsUntyped ||
            code->nFormals != code->compiledFunction->nFormals || code->detectedInjectedParameters)
        {
            return result;
        }
        const auto* unit = code->executableCompilationUnit()->unitData();
        const auto unitSize = static_cast<std::uint32_t>(unit->unitSize);
        const auto codeSize = static_cast<std::uint32_t>(code->compiledFunction->codeSize);
        result.bytecodeBytes = codeSize;
        const auto base = reinterpret_cast<std::uintptr_t>(unit);
        const auto start = reinterpret_cast<std::uintptr_t>(code->codeData);
        if (unitSize < sizeof(QV4::CompiledData::Unit) || unitSize > MAX_COMPILATION_BYTES ||
            !(unit->flags & QV4::CompiledData::Unit::IsJavascript) ||
            (unit->flags & QV4::CompiledData::Unit::IsESModule) || codeSize == 0 ||
            codeSize > unitSize || start < base || start - base > unitSize - codeSize ||
            code->codeData != code->compiledFunction->code())
        {
            return result;
        }
        auto* scope = scripted->scope();
        bool globalScope = false;
        for (std::uint32_t depth = 0; depth < MAX_CONTEXT_SCOPES; ++depth)
        {
            if (!ledger.consume(CONTEXT_SCOPE_WORK, RefinementWork::MasterPreparation))
            {
                result.status = GuardedScriptContextOperands::BudgetExhausted;
                return result;
            }
            result.chargedWork += CONTEXT_SCOPE_WORK;
            ++result.scopes;
            if (scope == nullptr)
            {
                return result;
            }
            if (scope->type == QV4::Heap::ExecutionContext::Type_GlobalContext)
            {
                if (scope != scripted->engine()->rootContext()->d() || scope->outer != nullptr ||
                    scope->activation != scripted->engine()->globalObject->d())
                {
                    return result;
                }
                globalScope = true;
                break;
            }
            if (scope->type != QV4::Heap::ExecutionContext::Type_CallContext || scope->activation != nullptr)
            {
                result.status = GuardedScriptContextOperands::ScopedOrDynamicOperands;
                return result;
            }
            const auto* call = static_cast<const QV4::Heap::CallContext*>(scope);
            if (call->nArgs != 0 || call->locals.size != 0 || call->locals.alloc != 0)
            {
                result.status = GuardedScriptContextOperands::ScopedOrDynamicOperands;
                return result;
            }
            scope = scope->outer;
        }
        if (!globalScope)
        {
            return result;
        }
        const auto work = CONTEXT_BYTE_PASSES * static_cast<std::int64_t>(codeSize);
        if (!ledger.consume(work, RefinementWork::MasterPreparation))
        {
            result.status = GuardedScriptContextOperands::BudgetExhausted;
            return result;
        }
        result.chargedWork += work;
        std::uint32_t offset = 0;
        while (offset < codeSize)
        {
            const auto* bytes = reinterpret_cast<const uchar*>(code->codeData + offset);
            if (*bytes == 1 && codeSize - offset < 2)
            {
                return result;
            }
            const auto type = QV4::Moth::Instr::unpack(bytes);
            const auto index = static_cast<std::uint32_t>(type);
            if (index >= std::size(INSTRUCTION_ARGUMENTS))
            {
                return result;
            }
            auto width = sizeof(qint8);
            if (QV4::Moth::Instr::isWide(type))
            {
                width = sizeof(qint32);
            }
            const auto length = QV4::Moth::Instr::encodedLength(type) + INSTRUCTION_ARGUMENTS[index] * width;
            if (length > codeSize - offset)
            {
                return result;
            }
            ++result.instructions;
            if (usesContextOperands(type))
            {
                result.status = GuardedScriptContextOperands::ScopedOrDynamicOperands;
                return result;
            }
            offset += static_cast<std::uint32_t>(length);
        }
        result.status = GuardedScriptContextOperands::GlobalScopeOnly;
#else
        Q_UNUSED(function)
        Q_UNUSED(view)
#endif
        return result;
    }

    GuardedScriptFunctionImage guardedScriptFunctionImage(
        const QJSValue & function, RefinementLedger & ledger, GuardedScriptImageCache* cache)
    {
        return inspectScriptImage(&function, nullptr, ledger, cache);
    }

    GuardedScriptFunctionImage guardedScriptFunctionImage(
        const GuardedScriptValueView & function, RefinementLedger & ledger, GuardedScriptImageCache* cache)
    {
        return inspectScriptImage(nullptr, &function, ledger, cache);
    }

    GuardedScriptProperty guardedScriptProperty(
        const QJSValue & object, const QString & name, RefinementLedger & ledger, GuardedScriptOwnIndexCache* cache)
    {
        return inspectScriptProperty(&object, nullptr, name, ledger, cache);
    }

    GuardedScriptProperty guardedScriptProperty(
        const GuardedScriptValueView & object, const QString & name, RefinementLedger & ledger,
        GuardedScriptOwnIndexCache* cache)
    {
        return inspectScriptProperty(nullptr, &object, name, ledger, cache);
    }

    GuardedScriptProperty guardedScriptGlobalProperty(
        QJSEngine & engine, const QString & name, RefinementLedger & ledger)
    {
        GuardedScriptProperty result;
        if (!ledger.consume(GLOBAL_INSPECTION_WORK, RefinementWork::MasterPreparation))
        {
            result.status = GuardedScriptPropertyStatus::BudgetExhausted;
            return result;
        }
        result.chargedWork = GLOBAL_INSPECTION_WORK;
#ifdef SCRIPTSOURCESUPPORT
        auto* native = engine.handle();
        if (native == nullptr || native->globalObject == nullptr)
        {
            return result;
        }
        const auto global = GuardedScriptObservationAccess::borrow(
            QV4::Value::fromHeapObject(native->globalObject->d()));
        auto observed = inspectScriptProperty(nullptr, &global, name, ledger, nullptr);
        observed.chargedWork += result.chargedWork;
        return observed;
#else
        Q_UNUSED(engine)
        Q_UNUSED(name)
        return result;
#endif
    }

    GuardedScriptAliasInspection guardedScriptGlobalAlias(
        QJSEngine & engine, const GuardedScriptValueView & view, RefinementLedger & ledger)
    {
        GuardedScriptAliasInspection result;
        if (!ledger.consume(GLOBAL_INSPECTION_WORK, RefinementWork::MasterPreparation))
        {
            result.status = GuardedScriptPropertyStatus::BudgetExhausted;
            return result;
        }
        result.chargedWork = GLOBAL_INSPECTION_WORK;
#ifdef SCRIPTSOURCESUPPORT
        const auto value = GuardedScriptObservationAccess::read(view);
        const auto* object = value.as<QV4::Object>();
        const auto* native = engine.handle();
        if (object != nullptr && native != nullptr && native->globalObject != nullptr &&
            object->d() == native->globalObject->d())
        {
            result.status = GuardedScriptPropertyStatus::Found;
        }
#else
        Q_UNUSED(engine)
        Q_UNUSED(view)
#endif
        return result;
    }

    GuardedScriptProperty guardedScriptIntrinsicPrototype(
        QJSEngine & engine, GuardedScriptIntrinsicPrototype kind, RefinementLedger & ledger)
    {
        GuardedScriptProperty result;
        if (!payPropertyWork(result, ledger, GLOBAL_INSPECTION_WORK))
        {
            return result;
        }
#ifdef SCRIPTSOURCESUPPORT
        auto* native = engine.handle();
        if (native == nullptr)
        {
            return result;
        }
        const QV4::Object* prototype = nullptr;
        switch (kind)
        {
            case GuardedScriptIntrinsicPrototype::Array:
                prototype = native->arrayPrototype();
                break;
            case GuardedScriptIntrinsicPrototype::String:
                prototype = native->stringPrototype();
                break;
            case GuardedScriptIntrinsicPrototype::Number:
                prototype = native->numberPrototype();
                break;
        }
        if (prototype == nullptr)
        {
            return result;
        }
        result.value = GuardedScriptObservationAccess::borrow(*prototype);
        result.status = GuardedScriptPropertyStatus::Found;
#else
        Q_UNUSED(engine)
        Q_UNUSED(kind)
#endif
        return result;
    }

    GuardedScriptAliasInspection guardedScriptIndexedArrayData(
        const GuardedScriptValueView & view, RefinementLedger & ledger)
    {
        GuardedScriptAliasInspection result;
        if (!ledger.consume(PROPERTY_METADATA_WORK, RefinementWork::MasterPreparation))
        {
            result.status = GuardedScriptPropertyStatus::BudgetExhausted;
            return result;
        }
        result.chargedWork = PROPERTY_METADATA_WORK;
#ifdef SCRIPTSOURCESUPPORT
        const auto value = GuardedScriptObservationAccess::read(view);
        std::uint32_t length = 0;
        if (!primitiveArray(value, length))
        {
            return result;
        }
        const auto* array = value.as<QV4::Object>();
        const auto* shape = array->internalClass();
        const auto* table = shape->propertyTable.d;
        const auto work = ARRAY_NAMED_MEMBER_WORK * static_cast<std::int64_t>(table->alloc);
        if (!ledger.consume(work, RefinementWork::MasterPreparation))
        {
            result.status = GuardedScriptPropertyStatus::BudgetExhausted;
            return result;
        }
        result.chargedWork += work;
        for (int slot = 0; slot < table->alloc; ++slot)
        {
            const auto & entry = table->entries[slot];
            if (!entry.identifier.isValid() || entry.index >= shape->size ||
                shape->propertyData.at(entry.index).isEmpty())
            {
                continue;
            }
            if (!entry.identifier.isString() ||
                entry.index != QV4::Heap::ArrayObject::LengthPropertyIndex)
            {
                return result;
            }
            const auto* name = entry.identifier.asStringOrSymbol<QV4::Heap::String>();
            if (name->subtype >= QV4::Heap::String::StringType_Complex ||
                name->text().size != static_cast<qsizetype>(sizeof(ARRAY_LENGTH_NAME) - 1) ||
                name->toQString() != QLatin1StringView(ARRAY_LENGTH_NAME))
            {
                return result;
            }
        }
        result.status = GuardedScriptPropertyStatus::Found;
#else
        Q_UNUSED(view)
#endif
        return result;
    }

    GuardedScriptArrayInspection guardedScriptArray(const GuardedScriptValueView & view, RefinementLedger & ledger)
    {
        GuardedScriptArrayInspection result;
        if (!ledger.consume(PROPERTY_METADATA_WORK, RefinementWork::MasterPreparation))
        {
            result.status = GuardedScriptPropertyStatus::BudgetExhausted;
            return result;
        }
        result.chargedWork = PROPERTY_METADATA_WORK;
#ifdef SCRIPTSOURCESUPPORT
        const auto value = GuardedScriptObservationAccess::read(view);
        if (primitiveArray(value, result.length))
        {
            result.status = GuardedScriptPropertyStatus::Found;
        }
#else
        Q_UNUSED(view)
#endif
        return result;
    }

    GuardedScriptProperty guardedScriptArrayElement(
        const GuardedScriptValueView & view, std::uint32_t index, RefinementLedger & ledger)
    {
        GuardedScriptProperty result;
        if (!ledger.consume(PROPERTY_METADATA_WORK, RefinementWork::MasterPreparation))
        {
            result.status = GuardedScriptPropertyStatus::BudgetExhausted;
            return result;
        }
        result.chargedWork = PROPERTY_METADATA_WORK;
#ifdef SCRIPTSOURCESUPPORT
        std::uint32_t length = 0;
        const auto value = GuardedScriptObservationAccess::read(view);
        if (!primitiveArray(value, length))
        {
            return result;
        }
        if (index >= length)
        {
            result.status = GuardedScriptPropertyStatus::Missing;
            return result;
        }
        readOwnProperty(*value.as<QV4::Object>(), QString{}, index, result, ledger);
#else
        Q_UNUSED(view)
        Q_UNUSED(index)
#endif
        return result;
    }

    GuardedScriptProperty guardedScriptQObjectProperty(
        const GuardedScriptValueView & view, const QObject & expected, const QMetaObject & permitted,
        const QString & name, RefinementLedger & ledger, GuardedScriptOwnIndexCache* cache)
    {
        GuardedScriptProperty result;
        if (name.isEmpty() || name.size() > MAX_PROPERTY_NAME)
        {
            return result;
        }
        if (!payPropertyWork(result, ledger,
                PROPERTY_METADATA_WORK + static_cast<std::int64_t>(name.size()) * sizeof(QChar)))
        {
            return result;
        }
#ifdef SCRIPTSOURCESUPPORT
        const auto value = GuardedScriptObservationAccess::read(view);
        const auto* wrapper = value.as<QV4::QObjectWrapper>();
        if (wrapper == nullptr || wrapper->vtable() != QV4::QObjectWrapper::staticVTable() ||
            wrapper->object() != &expected || wrapper->engine()->qmlEngine() != nullptr ||
            !supportedNativeMetadata(expected, permitted, result, ledger) ||
            nativeNameCollision(permitted, name, result, ledger))
        {
            return result;
        }
        readOwnProperty(*wrapper, name, QV4::stringToArrayIndex(name), result, ledger, cache);
#else
        Q_UNUSED(view)
        Q_UNUSED(expected)
        Q_UNUSED(permitted)
        Q_UNUSED(cache)
#endif
        return result;
    }

    GuardedScriptScalar guardedScriptScalar(const GuardedScriptValueView & view, RefinementLedger & ledger)
    {
        GuardedScriptScalar result;
        if (!ledger.consume(SCALAR_METADATA_WORK, RefinementWork::MasterPreparation))
        {
            result.status = GuardedScriptScalarStatus::BudgetExhausted;
            return result;
        }
        result.chargedWork = SCALAR_METADATA_WORK;
#ifdef SCRIPTSOURCESUPPORT
        const auto raw = GuardedScriptObservationAccess::read(view);
        switch (view.kind())
        {
            case GuardedScriptValueKind::Undefined:
            case GuardedScriptValueKind::Null:
                break;
            case GuardedScriptValueKind::Boolean:
                result.value = raw.booleanValue();
                break;
            case GuardedScriptValueKind::Number:
                if (!std::isfinite(raw.asDouble()))
                {
                    return result;
                }
                result.value = raw.asDouble();
                break;
            case GuardedScriptValueKind::String:
            {
                const auto* string = raw.as<QV4::String>();
                if (string == nullptr || string->d()->subtype >= QV4::Heap::String::StringType_Complex)
                {
                    return result;
                }
                const auto size = string->d()->text().size;
                if (size < 0 || size > MAX_SCALAR_STRING)
                {
                    return result;
                }
                const auto work = SCALAR_CHARACTER_WORK * static_cast<std::int64_t>(size);
                if (!ledger.consume(work, RefinementWork::MasterPreparation))
                {
                    result.status = GuardedScriptScalarStatus::BudgetExhausted;
                    return result;
                }
                result.chargedWork += work;
                const auto text = string->toQString();
                result.value = QString(text.constData(), text.size());
                break;
            }
            default:
                return result;
        }
        result.kind = view.kind();
        result.status = GuardedScriptScalarStatus::Verified;
#else
        Q_UNUSED(view)
#endif
        return result;
    }

    GuardedScriptContextInspection guardedScriptContextOperands(const QJSValue & function, RefinementLedger & ledger)
    {
        return inspectContextOperands(&function, nullptr, ledger);
    }

    GuardedScriptContextInspection guardedScriptContextOperands(
        const GuardedScriptValueView & function, RefinementLedger & ledger)
    {
        return inspectContextOperands(nullptr, &function, ledger);
    }
}
