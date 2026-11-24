#include "AngelIOCheckpointSubsystem.h"
#include "Components/ActorComponent.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "GameFramework/Actor.h"
#include "LevelUtils.h"
#include "Misc/Paths.h"

UAngelIOCheckpointSubsystem::FKey UAngelIOCheckpointSubsystem::MakeKey(const UActorComponent& Component, FString Script)
{
    const ULevel* Level = Component.GetOwner()->GetLevel();
    const auto* Stream = FLevelUtils::FindStreamingLevel(Level);
    const FString Room = Stream ? Stream->GetWorldAssetPackageName() : Level->GetOutermost()->GetName();
    FPaths::NormalizeFilename(Script);
    FPaths::CollapseRelativeDirectories(Script);
    return {Room + TEXT(":") + Component.GetPathName(Level), MoveTemp(Script)};
}

const UAngelIOCheckpointSubsystem::FCheckpoint* UAngelIOCheckpointSubsystem::Find(const UActorComponent& Component, const FString& Script) const
{
    return Records.Find(MakeKey(Component, Script));
}

const UAngelIOCheckpointSubsystem::FCheckpoint& UAngelIOCheckpointSubsystem::Save(const UActorComponent& Component,
    const FString& Script, int32 Version, FUtf8String Stage, const FGuid& Instance)
{
    auto& Record = Records.FindOrAdd(MakeKey(Component, Script));
    Record.SavedBy = Instance;
    Record.Version = Version;
    Record.Stage = MoveTemp(Stage);
    return Record;
}
