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

FUtf8String UAngelIOCheckpointSubsystem::Load(const UActorComponent& Component, const FString& Script, int32 Version) const
{
    const auto* Record = Records.Find(MakeKey(Component, Script));
    return Record && Record->Version == Version ? Record->Stage : FUtf8String();
}

void UAngelIOCheckpointSubsystem::Save(const UActorComponent& Component, const FString& Script, int32 Version, FUtf8String Stage)
{
    Records.Add(MakeKey(Component, Script), FCheckpoint{Version, MoveTemp(Stage)});
}
