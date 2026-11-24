#pragma once

#include "AngelIOProgram.h"
#include "Widgets/SCompoundWidget.h"

class UAngelIOScriptComponent;
class UWorld;

class ANGELIOEDITOR_API SAngelIOMissionInspector : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SAngelIOMissionInspector) {}
    SLATE_END_ARGS()
    static const FName TabId;
    void Construct(const FArguments&);

private:
    friend class FAngelIOInspectorPIE;
    friend class FAngelIOCheckpointListen;
    struct FOperation
    {
        FString Id, Name, Target, Status;
        int64 LastSequence = 0;
        bool bOpen = false;
        FString Caption() const;
    };
    void SelectWorld(TWeakObjectPtr<UWorld> Value);
    void SelectMission(TWeakObjectPtr<UAngelIOScriptComponent> Value);
    void SelectOperation(FString Id);
    void Refresh();
    void CollectHistory(UAngelIOScriptComponent& Authority);
    void UpdateTimeline();
    FString CheckpointSummary() const;
    TSharedRef<SWidget> WorldMenu();
    TSharedRef<SWidget> MissionMenu();
    TSharedRef<SWidget> OperationMenu();

    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<UAngelIOScriptComponent> Mission, AuthorityMission;
    FAngelIOMissionInspection Inspection;
    TArray<FOperation> Operations;
    int64 HistoryRevision = -1;
    int64 Dropped = 0;
    FString PinnedId, DisplayedId;
    FString Origin, Location, Wait, OperationLabel, Timeline;
};
