#include "Modules/ModuleManager.h"
#include "SAngelIOMissionInspector.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

class FAngelIOEditorModule : public IModuleInterface
{
public:
    void StartupModule() override
    {
        if (IsRunningCommandlet()) return;
        FGlobalTabmanager::Get()->RegisterNomadTabSpawner(SAngelIOMissionInspector::TabId,
            FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
            {
                return SNew(SDockTab).TabRole(ETabRole::NomadTab)[SNew(SAngelIOMissionInspector)];
            }))
            .SetDisplayName(NSLOCTEXT("AngelIO", "Inspector", "AngelIO Mission Inspector"))
            .SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
            .SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"));
    }
    void ShutdownModule() override
    {
        if (FSlateApplication::IsInitialized() && !IsRunningCommandlet())
            FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(SAngelIOMissionInspector::TabId);
    }
};
IMPLEMENT_MODULE(FAngelIOEditorModule, AngelIOEditor)
