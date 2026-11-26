#include "AngelIOCheckpointSubsystem.h"
#include "AngelIOPersistenceIdentity.h"
#include "AngelIOScriptComponent.h"
#include "Algo/AllOf.h"
#include "Components/ActorComponent.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "LevelUtils.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Misc/ScopeExit.h"

namespace
{
    struct FComponentAddress { FString Map, Owner; };

    FComponentAddress ComponentAddress(const UActorComponent& Component)
    {
        const ULevel* Level = Component.GetOwner()->GetLevel();
        const UWorld* World = Component.GetWorld();
        const ULevelStreaming* Stream = FLevelUtils::FindStreamingLevel(Level);
        auto Package = [World](const FString& Name)
        {
            return World->StreamingLevelsPrefix.IsEmpty() ? Name :
                UWorld::StripPIEPrefixFromPackageName(Name, World->StreamingLevelsPrefix);
        };
        const FString Room = Package(Stream ? Stream->GetWorldAssetPackageName() : Level->GetOutermost()->GetName());
        const FString Owner = Room + TEXT(":") + Component.GetPathName(Level);
        return {Package(World->GetOutermost()->GetName()).ToLower(), Owner.ToLower()};
    }

    FString NormalScript(FString Script)
    {
        FPaths::NormalizeFilename(Script);
        FPaths::CollapseRelativeDirectories(Script);
        return Script.ToLower();
    }
}

bool UAngelIOCheckpointSubsystem::FKey::Bind(FSQLitePreparedStatement& Statement, int32 First) const
{
    return Statement.IsValid() && Statement.SetBindingValueByIndex(First, Map) &&
        Statement.SetBindingValueByIndex(First + 1, Owner) && Statement.SetBindingValueByIndex(First + 2, Name);
}

bool UAngelIOCheckpointSubsystem::ResolveKey(const UActorComponent& Component, FString Name, bool bMission, bool bInspectOnly,
    FKey& Out, FString& Error)
{
    const auto* Mission = Cast<UAngelIOScriptComponent>(&Component);
    Name = bMission ? NormalScript(MoveTemp(Name)) : Name.ToLower();
    if (!Mission || !Mission->PersistenceId.IsValid())
    {
        const auto Address = ComponentAddress(Component);
        Out = {Address.Map, Address.Owner, Name};
        return true;
    }
    const auto Id = FAngelIOPersistenceIdentity::From(*Mission);
    if (!Id.CheckPeers(*Mission, Error)) return false;
    Out = {Id.Map, Id.Owner, bMission ? Id.MissionKey(*Mission, Name) : Name};
    TArray<FString> Names{Name};
    if (bMission)
        for (const auto& Former : Mission->FormerScripts) Names.AddUnique(NormalScript(Former));
    const FString Table = bMission ? TEXT("checkpoints") : TEXT("component_state");
    const FString Column = bMission ? TEXT("script") : TEXT("key");
    const FString Where = TEXT(" WHERE map=?1 AND owner=?2 AND ") + Column + TEXT("=?3");
    const bool Transaction = !bInspectOnly;
    bool bDone = false;
    if (Transaction && !Database.Execute(TEXT("BEGIN IMMEDIATE"))) { Error = Database.GetLastError(); return false; }
    ON_SCOPE_EXIT { if (Transaction && !bDone) Database.Execute(TEXT("ROLLBACK")); };
    auto Query = Database.PrepareStatement(*(TEXT("SELECT 1 FROM ") + Table + Where));
    TArray<FKey> Candidates{Out};
    for (const auto& OldOwner : Id.LegacyOwners)
        for (const auto& OldName : Names) Candidates.AddUnique({Id.LegacyMap, OldOwner, OldName});
    TOptional<FKey> Found;
    for (const auto& Key : Candidates)
    {
        Query.Reset();
        if (!Key.Bind(Query)) { Error = Database.GetLastError(); return false; }
        const auto Result = Query.Step();
        if (Result == ESQLitePreparedStatementStepResult::Done) continue;
        if (Result != ESQLitePreparedStatementStepResult::Row) { Error = Database.GetLastError(); return false; }
        if (Found) { Error = TEXT("Conflicting stable and legacy records for ") + Component.GetPathName(); return false; }
        Found = Key;
    }
    Query.Destroy();
    if (Found && !(Found.GetValue() == Out))
    {
        if (bInspectOnly) Out = *Found;
        else
        {
            auto Move = Database.PrepareStatement(*(TEXT("UPDATE ") + Table + TEXT(" SET map=?4, owner=?5, ") + Column + TEXT("=?6") + Where));
            if (!Found->Bind(Move) || !Out.Bind(Move, 4) || !Move.Execute()) { Error = Database.GetLastError(); return false; }
        }
    }
    bDone = !Transaction || Database.Execute(TEXT("COMMIT"));
    if (!bDone) Error = Database.GetLastError();
    return bDone;
}

bool UAngelIOCheckpointSubsystem::Open(FString& Error, bool bInspectOnly)
{
    if (Database.IsValid() && (!bReadOnly || bInspectOnly)) return true;
    if (Database.IsValid()) Database.Close();
    const UWorld* World = GetWorld();
    const FString Slot = World->URL.GetOption(TEXT("AngelIOCheckpointSlot="), TEXT("Default"));
    if (Slot.Len() > 64 || !Algo::AllOf(Slot, [](TCHAR C)
        { return FChar::IsAlnum(C) || C == TEXT('_') || C == TEXT('-'); }))
    {
        Error = TEXT("Invalid AngelIOCheckpointSlot (use up to 64 letters, digits, underscores or hyphens)");
        return false;
    }
    const FString Path = Slot.IsEmpty() ? FString(TEXT(":memory:")) :
        FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AngelIO/Checkpoints") /
            (World->WorldType == EWorldType::PIE ? TEXT("PIE") : TEXT("Game")) / (TEXT("slot-") + Slot + TEXT(".sqlite3")));
    if (bInspectOnly && (Slot.IsEmpty() || !FPaths::FileExists(Path))) return true;
    const bool Exists = FPaths::FileExists(Path);
    bReadOnly = bInspectOnly;
    if (!Database.Open(*Path, bInspectOnly ? ESQLiteDatabaseOpenMode::ReadOnly : ESQLiteDatabaseOpenMode::ReadWriteCreate))
    {
        Error = TEXT("Could not open checkpoint store: ") + Path;
        return false;
    }
    int32 Schema = -1;
    {
        auto Version = Database.PrepareStatement(TEXT("PRAGMA user_version"));
        if (Version.Step() == ESQLitePreparedStatementStepResult::Row) Version.GetColumnValueByIndex(0, Schema);
    }
    if (Schema < 0 || Schema > 1)
    {
        Error = FString::Printf(TEXT("Unsupported or unreadable checkpoint database schema %d"), Schema);
        Database.Close();
        return false;
    }
    if (bInspectOnly) return true;
    if (Schema == 0 && Exists)
    {
        const FString Backup = Path + TEXT(".pre-v1");
        if (!FPaths::FileExists(Backup))
        {
            const FString Temp = Backup + TEXT(".") + FGuid::NewGuid().ToString();
            bool bBackup;
            {
                auto Snapshot = Database.PrepareStatement(TEXT("VACUUM INTO ?1"));
                bBackup = Snapshot.SetBindingValueByIndex(1, Temp) && Snapshot.Execute();
            }
            if (!bBackup || (!IFileManager::Get().Move(*Backup, *Temp, false) && !FPaths::FileExists(Backup)))
            {
                Error = TEXT("Could not back up the legacy checkpoint database: ") + Backup;
                IFileManager::Get().Delete(*Temp);
                Database.Close();
                return false;
            }
        }
    }
    if (!Database.Execute(TEXT("PRAGMA synchronous=FULL")) || !Database.Execute(TEXT("BEGIN IMMEDIATE")) ||
        !Database.Execute(TEXT("CREATE TABLE IF NOT EXISTS checkpoints (")
            TEXT("map TEXT NOT NULL, owner TEXT NOT NULL, script TEXT NOT NULL, id BLOB NOT NULL, ")
            TEXT("writer BLOB NOT NULL, version INTEGER NOT NULL CHECK(version > 0), stage BLOB NOT NULL, ")
            TEXT("PRIMARY KEY(map, owner, script)) WITHOUT ROWID")) ||
        !Database.Execute(TEXT("CREATE TABLE IF NOT EXISTS component_state (")
            TEXT("map TEXT NOT NULL, owner TEXT NOT NULL, key TEXT NOT NULL, data BLOB NOT NULL CHECK(length(data)>0), ")
            TEXT("PRIMARY KEY(map,owner,key)) WITHOUT ROWID")) ||
        !Database.Execute(TEXT("CREATE TABLE IF NOT EXISTS action_receipts (map TEXT,owner TEXT,contract TEXT,action TEXT,")
            TEXT("version INTEGER NOT NULL,id BLOB NOT NULL,PRIMARY KEY(map,owner,contract,action)) WITHOUT ROWID")) ||
        !Database.Execute(TEXT("PRAGMA user_version=1")) || !Database.Execute(TEXT("COMMIT")))
    {
        Error = TEXT("Could not initialize checkpoint store: ") + Database.GetLastError();
        Database.Execute(TEXT("ROLLBACK"));
        Database.Close();
        return false;
    }
    TraceSlot = Slot.IsEmpty() ? TEXT(":memory:") + FGuid::NewGuid().ToString() : Slot.ToLower();
    return true;
}

FGuid UAngelIOCheckpointSubsystem::StateId(const UActorComponent& Component, const FString& Key) const
{
    auto Address = ComponentAddress(Component);
    if (const auto* Mission = Cast<UAngelIOScriptComponent>(&Component); Mission && Mission->PersistenceId.IsValid())
    {
        const auto Id = FAngelIOPersistenceIdentity::From(*Mission);
        Address = {Id.Map, Id.Owner};
    }
    const TCHAR* Domain = GetWorld()->WorldType == EWorldType::PIE ? TEXT("PIE") : TEXT("Game");
    return FGuid::NewDeterministicGuid(FString::Join(TArray<FString>{Domain, TraceSlot, Address.Map, Address.Owner, Key.ToLower()}, TEXT("|")));
}

bool UAngelIOCheckpointSubsystem::Load(const UActorComponent& Component, const FString& Script,
    FCheckpoint& Out, FString& Error, bool bInspectOnly)
{
    Out = {};
    if (!Open(Error, bInspectOnly)) return false;
    if (!Database.IsValid()) return true;
    FKey Key;
    if (!ResolveKey(Component, Script, true, bInspectOnly, Key, Error)) return false;
    auto Query = Database.PrepareStatement(TEXT("SELECT id, writer, version, stage FROM checkpoints WHERE map=?1 AND owner=?2 AND script=?3"));
    if (Key.Bind(Query))
    {
        const auto Result = Query.Step();
        if (Result == ESQLitePreparedStatementStepResult::Done) return true;
        if (Result == ESQLitePreparedStatementStepResult::Row)
        {
            TArray<uint8> Stage;
            if (Query.GetColumnValueByIndex(0, Out.Id) && Out.Id.IsValid() &&
                Query.GetColumnValueByIndex(1, Out.SavedBy) && Out.SavedBy.IsValid() &&
                Query.GetColumnValueByIndex(2, Out.Version) && Out.Version > 0 &&
                Query.GetColumnValueByIndex(3, Stage) && !Stage.IsEmpty() && !Stage.Contains(0))
            {
                Out.Stage = FUtf8String(Stage.Num(), reinterpret_cast<const UTF8CHAR*>(Stage.GetData()));
                return true;
            }
            Error = TEXT("Invalid mission checkpoint record");
            return false;
        }
    }
    Error = TEXT("Could not read mission checkpoint: ") + Database.GetLastError();
    return false;
}

bool UAngelIOCheckpointSubsystem::Save(const UActorComponent& Component, const FString& Script,
    FCheckpoint& Record, FString& Error)
{
    if (!Open(Error, false)) return false;
    // One SQLite transaction. Keep the existing ID across writers, version changes and restarts.
    FKey Key;
    if (!ResolveKey(Component, Script, true, false, Key, Error)) return false;
    auto Write = Database.PrepareStatement(TEXT("INSERT INTO checkpoints(map,owner,script,id,writer,version,stage) ")
        TEXT("VALUES(?1,?2,?3,?4,?5,?6,?7) ON CONFLICT(map,owner,script) DO UPDATE SET ")
        TEXT("writer=excluded.writer, version=excluded.version, stage=excluded.stage RETURNING id"));
    if (Key.Bind(Write) && Write.SetBindingValueByIndex(4, FGuid::NewGuid()) &&
        Write.SetBindingValueByIndex(5, Record.SavedBy) && Write.SetBindingValueByIndex(6, Record.Version) &&
        Write.SetBindingValueByIndex(7, *Record.Stage, Record.Stage.Len()) &&
        Write.Step() == ESQLitePreparedStatementStepResult::Row && Write.GetColumnValueByIndex(0, Record.Id) &&
        Write.Step() == ESQLitePreparedStatementStepResult::Done)
        return true;
    Error = TEXT("Could not commit mission checkpoint: ") + Database.GetLastError();
    return false;
}

bool UAngelIOCheckpointSubsystem::LoadState(const UActorComponent& Component, const FString& Key, TArray<uint8>& Data, FString& Error)
{
    if (!Open(Error, false)) return false;
    auto Query = Database.PrepareStatement(TEXT("SELECT data FROM component_state WHERE map=?1 AND owner=?2 AND key=?3"));
    FKey Address;
    if (!ResolveKey(Component, Key, false, false, Address, Error)) return false;
    if (Address.Bind(Query))
    {
        const auto Result = Query.Step();
        if (Result == ESQLitePreparedStatementStepResult::Done) return true;
        if (Result == ESQLitePreparedStatementStepResult::Row && Query.GetColumnValueByIndex(0, Data) && !Data.IsEmpty()) return true;
    }
    Error = TEXT("Could not read component state: ") + Database.GetLastError();
    return false;
}

bool UAngelIOCheckpointSubsystem::SaveState(const UActorComponent& Component, const FString& Key, TConstArrayView<uint8> Data, FString& Error)
{
    if (!Open(Error, false)) return false;
    auto Write = Database.PrepareStatement(TEXT("INSERT INTO component_state(map,owner,key,data) VALUES(?1,?2,?3,?4) ")
        TEXT("ON CONFLICT(map,owner,key) DO UPDATE SET data=excluded.data"));
    FKey Address;
    if (!ResolveKey(Component, Key, false, false, Address, Error)) return false;
    if (Address.Bind(Write) && Write.SetBindingValueByIndex(4, Data) && Write.Execute()) return true;
    Error = TEXT("Could not commit component state: ") + Database.GetLastError();
    return false;
}

bool UAngelIOCheckpointSubsystem::RecordOnce(const UActorComponent& Component, const FString& Script, const FString& Action,
    FGuid& Receipt, FString& Error, bool* Created)
{
    if (!Open(Error, false)) return false;
    FKey Key;
    if (!ResolveKey(Component, Script, true, false, Key, Error)) return false;
    auto Write = Database.PrepareStatement(TEXT("INSERT INTO action_receipts(map,owner,contract,action,version,id) ")
        TEXT("VALUES(?1,?2,?3,?4,1,?5) ON CONFLICT(map,owner,contract,action) DO UPDATE SET id=id RETURNING version,id"));
    const FGuid Proposed = FGuid::NewGuid();
    int32 Version = 0;
    if (Key.Bind(Write) && Write.SetBindingValueByIndex(4, Action) && Write.SetBindingValueByIndex(5, Proposed) &&
        Write.Step() == ESQLitePreparedStatementStepResult::Row && Write.GetColumnValueByIndex(0, Version) && Version == 1 &&
        Write.GetColumnValueByIndex(1, Receipt) && Receipt.IsValid() && Write.Step() == ESQLitePreparedStatementStepResult::Done)
    {
        if (Created) *Created = Receipt == Proposed;
        return true;
    }
    Error = TEXT("Invalid or incompatible action receipt: ") + Action + TEXT(" ") + Database.GetLastError();
    return false;
}

void UAngelIOCheckpointSubsystem::Deinitialize()
{
    CloseSlot();
    Super::Deinitialize();
}
