#include "AngelIOMapValidation.h"
#include "AngelIOScriptComponent.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameMapsSettings.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/PackageName.h"

namespace
{
    const TCHAR* TypeName(EAngelIOType Type)
    {
        switch (Type)
        {
        case EAngelIOType::Integer: return TEXT("int");
        case EAngelIOType::Float: return TEXT("float");
        case EAngelIOType::Boolean: return TEXT("bool");
        default: return TEXT("unknown");
        }
    }

    UClass* MapGameMode(UWorld& World)
    {
        if (const auto* Settings = World.GetWorldSettings())
            if (Settings->DefaultGameMode) return Settings->DefaultGameMode.Get();
        const FString PrefixMode = UGameMapsSettings::GetGameModeForMapName(FPackageName::GetShortName(World.GetOutermost()));
        if (!PrefixMode.IsEmpty())
            if (auto* Mode = LoadClass<AGameModeBase>(nullptr, *PrefixMode)) return Mode;
        return LoadClass<AGameModeBase>(nullptr, *UGameMapsSettings::GetGlobalDefaultGameMode());
    }
}

bool FAngelIOMapReport::IsValid() const
{
    return !Diagnostics.ContainsByPredicate([](const auto& D) { return D.Severity == TEXT("error"); });
}

FAngelIOMapReport AngelIO::ValidateMap(UWorld& World)
{
    FAngelIOMapReport Report;
    Report.Map = World.GetOutermost()->GetName();
    TMap<FName, TArray<const AActor*>> Targets;
    TArray<const UAngelIOScriptComponent*> Missions;
    TArray<ULevel*> Levels = World.GetLevels();
    Levels.AddUnique(World.PersistentLevel);
    for (const auto* Level : Levels)
    {
        if (!Level) continue;
        for (const AActor* Actor : Level->Actors)
        {
            if (!IsValid(Actor) || Actor->IsEditorOnly()) continue;
            for (const auto* Component : TInlineComponentArray<UAngelIOScriptComponent*>(Actor))
            {
                if (Component->IsEditorOnly()) continue;
                Missions.Add(Component);
                if (!Component->GetTargetName().IsNone())
                    Targets.FindOrAdd(Component->GetTargetName()).AddUnique(Actor);
            }
        }
    }
    if (const auto* Mode = MapGameMode(World))
    {
        TArray<const UAngelIOScriptComponent*> Panels;
        AActor::GetActorClassDefaultComponents(Mode->GetDefaultObject<AGameModeBase>()->PlayerControllerClass, Panels);
        Missions.Append(Panels);
    }

    TMap<FString, TSharedPtr<FAngelIOProgram>> Programs;
    for (const auto* Mission : Missions)
    {
        if (Mission->Script.IsEmpty() && !Mission->bStartOnBeginPlay && Mission->InitialInputs.IsEmpty()) continue;
        ++Report.MissionsChecked;
        auto Error = [&](FString Code, FString Field, FString Message, int32 Line = 0)
        {
            Report.Diagnostics.Add({MoveTemp(Code), TEXT("error"), MoveTemp(Message), Mission, MoveTemp(Field), Line});
        };
        if (Mission->Script.IsEmpty())
        {
            Error(TEXT("script_missing"), TEXT("Script"), TEXT("Assign a mission script to this component."));
            continue;
        }
        auto& Program = Programs.FindOrAdd(Mission->Script);
        if (!Program)
        {
            Program = MakeShared<FAngelIOProgram>();
            Program->CompileFile(Mission->Script);
        }
        for (const auto& D : Program->Diagnostics())
            Report.Diagnostics.Add({TEXT("script_compile"), D.Severity, D.Message, Mission, TEXT("Script"), D.Line, D.Column});
        if (Program->State() != EAngelIOState::Compiled) continue;

        for (const auto& Input : Mission->InitialInputs)
        {
            const auto* Port = Program->Ports().FindByPredicate([&](const FAngelIOPort& P)
                { return P.bInput && P.Name.Equals(Input.Key, ESearchCase::CaseSensitive); });
            if (!Port)
                Error(TEXT("input_unknown"), Input.Key, FString::Printf(TEXT("No input named '%s' in this script."), *Input.Key));
            else if (Port->Type != Input.Value.Type)
                Error(TEXT("input_type"), Input.Key, FString::Printf(TEXT("Input '%s' expects %s, configured as %s."),
                    *Input.Key, TypeName(Port->Type), TypeName(Input.Value.Type)));
        }
        for (const auto& Target : Program->LiteralTargets())
        {
            TArray<FString> Matches;
            const FName Alias(*Target.Name, FNAME_Find);
            const FName* Bound = Mission->RoomTargets.Find(Alias);
            if (Target.Name.Len() < NAME_SIZE)
                if (const auto* Actors = Targets.Find(Bound ? *Bound : Alias))
                    for (const auto* Actor : *Actors)
                        if ((!Bound || (Mission->GetOwner() && Actor->GetLevel() == Mission->GetOwner()->GetLevel())) &&
                            (!Target.OwnerClass || Actor->IsA(Target.OwnerClass))) Matches.Add(Actor->GetPathName());
            // A global WaitActor can intentionally name an unloaded room.
            if (!Bound && Target.Function == TEXT("WaitActor") && Matches.IsEmpty()) continue;
            if (Matches.IsEmpty())
                Error(TEXT("target_missing"), Target.Name, FString::Printf(TEXT("%s('%s') has no matching placed actor."),
                    *Target.Function, *Target.Name), Target.Line);
            else if (Target.bRequireSingleActor && Matches.Num() > 1)
            {
                Matches.Sort();
                Error(TEXT("target_ambiguous"), Target.Name, FString::Printf(TEXT("%s('%s') matches multiple actors: %s"),
                    *Target.Function, *Target.Name, *FString::Join(Matches, TEXT(", "))), Target.Line);
            }
        }
    }
    Report.Diagnostics.StableSort([](const auto& A, const auto& B)
    {
        const int32 ByComponent = A.Component->GetPathName().Compare(B.Component->GetPathName());
        if (ByComponent != 0) return ByComponent < 0;
        if (A.Line != B.Line) return A.Line < B.Line;
        if (A.Column != B.Column) return A.Column < B.Column;
        return A.Field.Compare(B.Field, ESearchCase::CaseSensitive) < 0;
    });
    return Report;
}
