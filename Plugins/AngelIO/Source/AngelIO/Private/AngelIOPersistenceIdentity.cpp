#include "AngelIOPersistenceIdentity.h"
#include "AngelIOScriptComponent.h"
#include "AngelIOSettings.h"
#include "Engine/LevelStreaming.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "LevelUtils.h"
#include "LevelInstance/LevelInstanceInterface.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "Misc/Paths.h"

FAngelIOPersistenceIdentity FAngelIOPersistenceIdentity::From(const UAngelIOScriptComponent& Mission)
{
    FAngelIOPersistenceIdentity Id;
    const auto* Actor = Mission.GetOwner();
    const auto* Level = Actor ? Actor->GetLevel() : nullptr;
    auto* World = Mission.GetWorld();
    if (!World || !Level) return Id;
    auto Package = [World](const FString& Name)
    {
        return (World->StreamingLevelsPrefix.IsEmpty() ? Name :
            UWorld::StripPIEPrefixFromPackageName(Name, World->StreamingLevelsPrefix)).ToLower();
    };
    const auto* Stream = FLevelUtils::FindStreamingLevel(Level);
    Id.LegacyMap = Package(World->GetOutermost()->GetName());
    Id.LegacyRoom = Package(Stream ? Stream->GetWorldAssetPackageName() : Level->GetOutermost()->GetName());
    Id.LegacyOwner = Id.LegacyRoom + TEXT(":") + Mission.GetPathName(Level).ToLower();
    Id.LegacyOwners.Add(Id.LegacyOwner);
    Id.Placement = Mission.PersistenceId;
    Id.Owner = Id.LegacyOwner;
    Id.Map = Id.LegacyMap;
    if (!Id.Placement.IsValid()) return Id;
    if (const auto* MapId = GetDefault<UAngelIOSettings>()->PersistentMapIds.Find(FName(*Id.LegacyMap)); MapId && MapId->IsValid())
        Id.Map = TEXT("map/") + MapId->ToString(EGuidFormats::Digits);
    if (auto* Instances = World->GetSubsystem<ULevelInstanceSubsystem>())
        Instances->ForEachLevelInstanceAncestors(Actor, [&Id](const ILevelInstanceInterface* Instance)
        {
            Id.Room += TEXT("/") + Instance->GetLevelInstanceGuid().ToString(EGuidFormats::Digits);
            return true;
        });
    if (Id.Room.IsEmpty()) Id.Room = Stream ? TEXT("stream/") + Id.LegacyRoom : TEXT("persistent");
    Id.Owner = TEXT("placement/") + Id.Placement.ToString(EGuidFormats::Digits) + TEXT("@") + Id.Room;
    for (const FString& Alias : Mission.FormerPlacementPaths)
        if (!Alias.IsEmpty()) Id.LegacyOwners.AddUnique(Id.LegacyRoom + TEXT(":") + Alias.ToLower());
    return Id;
}

FString FAngelIOPersistenceIdentity::MissionKey(const UAngelIOScriptComponent& Mission, FString Script) const
{
    if (Placement.IsValid() && !Mission.PersistenceContract.IsNone())
        return TEXT("contract/") + Mission.PersistenceContract.ToString().ToLower();
    FPaths::NormalizeFilename(Script);
    FPaths::CollapseRelativeDirectories(Script);
    return Script.ToLower();
}

bool FAngelIOPersistenceIdentity::ConflictsWith(const FAngelIOPersistenceIdentity& Other) const
{
    if (Map == Other.Map && Owner == Other.Owner) return true;
    return LegacyMap == Other.LegacyMap && LegacyOwners.ContainsByPredicate([&Other](const FString& Alias)
        { return Other.LegacyOwners.Contains(Alias); });
}

bool FAngelIOPersistenceIdentity::CheckPeers(const UAngelIOScriptComponent& Mission, FString& Error) const
{
    if (!Placement.IsValid()) return true;
    for (const AActor* Actor : Mission.GetOwner()->GetLevel()->Actors)
        if (IsValid(Actor))
            for (const auto* Peer : TInlineComponentArray<UAngelIOScriptComponent*>(Actor))
                if (Peer != &Mission && ConflictsWith(From(*Peer)))
                {
                    Error = FString::Printf(TEXT("Ambiguous save identity: %s and %s"), *Mission.GetPathName(), *Peer->GetPathName());
                    return false;
                }
    return true;
}
