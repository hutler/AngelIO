#include "Modules/ModuleManager.h"
#include "SAngelIOMissionInspector.h"
#include "AngelIOMapValidation.h"
#include "AngelIOScriptComponent.h"
#include "Editor.h"
#include "Logging/MessageLog.h"
#include "Misc/UObjectToken.h"
#include "ToolMenus.h"
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
        UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FAngelIOEditorModule::RegisterMenus));
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
        {
            UToolMenus::UnRegisterStartupCallback(this);
            UToolMenus::UnregisterOwner(this);
            FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(SAngelIOMissionInspector::TabId);
        }
    }
private:
    void RegisterMenus()
    {
        FToolMenuOwnerScoped Owner(this);
        auto* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
        Menu->FindOrAddSection("AngelIO").AddMenuEntry("AngelIOValidateMap",
            NSLOCTEXT("AngelIO", "ValidateMap", "Validate AngelIO Missions"),
            NSLOCTEXT("AngelIO", "ValidateMapTip", "Check scripts, initial inputs and named targets in the current editor map."),
            FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&ValidateEditorMap)));
    }
    static void ValidateEditorMap()
    {
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!World) return;
        const auto Report = AngelIO::ValidateMap(*World);
        FMessageLog Log("MapCheck");
        Log.NewPage(FText::FromString(TEXT("AngelIO ") + Report.Map));
        Log.Info(FText::FromString(FString::Printf(TEXT("%s: checked %d mission components, %d diagnostics."),
            *Report.Map, Report.MissionsChecked, Report.Diagnostics.Num())));
        for (const auto& D : Report.Diagnostics)
        {
            const auto Severity = D.Severity == TEXT("error") ? EMessageSeverity::Error :
                D.Severity == TEXT("warning") ? EMessageSeverity::Warning : EMessageSeverity::Info;
            auto Entry = Log.Message(Severity);
            Entry->AddToken(FUObjectToken::Create(D.Component));
            const FString Location = D.Line ? FString::Printf(TEXT(":%d"), D.Line) : FString();
            Entry->AddToken(FTextToken::Create(FText::FromString(FString::Printf(TEXT("%s%s [%s] %s"),
                *D.Component->Script, *Location, *D.Field, *D.Message))));
        }
        Log.Open(EMessageSeverity::Info, true);
    }
};
IMPLEMENT_MODULE(FAngelIOEditorModule, AngelIOEditor)
