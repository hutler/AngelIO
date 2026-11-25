#include "AngelIOCheckCommandlet.h"
#include "AngelIOProgram.h"
#include "AngelIOMapValidation.h"
#include "AngelIOScriptComponent.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Serialization/JsonSerializer.h"

UAngelIOCheckCommandlet::UAngelIOCheckCommandlet()
{
    IsClient = false;
    IsServer = false;
    IsEditor = true;
    LogToConsole = false;
}

namespace
{
int32 WriteReport(TSharedRef<FJsonObject> Root, const FString& Report, bool bValid)
{
    Root->SetBoolField(TEXT("valid"), bValid);
    FString Text;
    auto Writer = TJsonWriterFactory<>::Create(&Text);
    if (!FJsonSerializer::Serialize(Root, Writer) ||
        !FFileHelper::SaveStringToFile(Text, *Report, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        return 2;
    return bValid ? 0 : 1;
}

int32 CheckMap(const FString& Map, const FString& Output)
{
    FAngelIOMapReport Report;
    Report.Map = Map;
    FString Filename;
    UWorld* World = nullptr;
    if (FPackageName::IsValidLongPackageName(Map) && FPackageName::DoesPackageExist(Map, &Filename) &&
        Filename.EndsWith(FPackageName::GetMapPackageExtension()))
        World = UEditorLoadingAndSavingUtils::LoadMap(Filename);
    if (World) Report = AngelIO::ValidateMap(*World);
    else Report.Diagnostics.Add({TEXT("map_load"), TEXT("error"), TEXT("Could not load map package: ") + Map});
    auto Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("map"), Report.Map);
    Root->SetNumberField(TEXT("missionsChecked"), Report.MissionsChecked);
    TArray<TSharedPtr<FJsonValue>> Diagnostics;
    for (const auto& D : Report.Diagnostics)
    {
        auto Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("code"), D.Code);
        Json->SetStringField(TEXT("severity"), D.Severity);
        Json->SetStringField(TEXT("message"), D.Message);
        Json->SetStringField(TEXT("field"), D.Field);
        Json->SetStringField(TEXT("component"), D.Component ? D.Component->GetPathName() : FString());
        Json->SetStringField(TEXT("actor"), D.Component && D.Component->GetOwner() ? D.Component->GetOwner()->GetPathName() : FString());
        Json->SetStringField(TEXT("script"), D.Component ? D.Component->Script : FString());
        Json->SetNumberField(TEXT("line"), D.Line);
        Json->SetNumberField(TEXT("column"), D.Column);
        Diagnostics.Add(MakeShared<FJsonValueObject>(Json));
    }
    Root->SetArrayField(TEXT("diagnostics"), Diagnostics);
    return WriteReport(Root, Output, Report.IsValid());
}
}

int32 UAngelIOCheckCommandlet::Main(const FString& Params)
{
    FString Script, Report, Map;
    const bool bApiOnly = FParse::Param(*Params, TEXT("ApiOnly"));
    if (!FParse::Value(*Params, TEXT("Report="), Report)) return 2;
    if (FParse::Value(*Params, TEXT("Map="), Map)) return CheckMap(Map, Report);
    if (!bApiOnly && !FParse::Value(*Params, TEXT("Script="), Script))
    {
        UE_LOG(LogTemp, Error, TEXT("Expected -Script=<relative .as> -Report=<output.json>"));
        return 2;
    }
    FAngelIOProgram Program;
    const bool bValid = bApiOnly ? Program.Compile(TEXT("API"), TEXT("void Run() {}")) : Program.CompileFile(Script);
    auto Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("script"), Script);
    TArray<TSharedPtr<FJsonValue>> Declarations;
    for (const auto& Decl : Program.ExternalDeclarations())
        Declarations.Add(MakeShared<FJsonValueString>(Decl));
    Root->SetArrayField(TEXT("externalDeclarations"), Declarations);
    TArray<TSharedPtr<FJsonValue>> Ports, Diagnostics;
    for (const auto& P : Program.Ports())
    {
        auto Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("name"), P.Name);
        Json->SetStringField(TEXT("defaultExpression"), P.DefaultExpression);
        Json->SetStringField(TEXT("direction"), P.bInput ? TEXT("in") : TEXT("out"));
        Json->SetStringField(TEXT("type"), P.Type == EAngelIOType::Integer ? TEXT("int") :
            P.Type == EAngelIOType::Float ? TEXT("float") : TEXT("bool"));
        Ports.Add(MakeShared<FJsonValueObject>(Json));
    }
    Root->SetArrayField(TEXT("ports"), Ports);
    for (bool bIncoming : {true, false})
    {
        TArray<TSharedPtr<FJsonValue>> Names;
        for (const auto& Pair : Program.Signals(bIncoming))
            Names.Add(MakeShared<FJsonValueString>(Pair.Value));
        Root->SetArrayField(bIncoming ? TEXT("inputSignals") : TEXT("outputSignals"), Names);
    }
    for (const auto& D : Program.Diagnostics())
    {
        auto Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("severity"), D.Severity);
        Json->SetStringField(TEXT("message"), D.Message);
        Json->SetNumberField(TEXT("line"), D.Line);
        Json->SetNumberField(TEXT("column"), D.Column);
        Diagnostics.Add(MakeShared<FJsonValueObject>(Json));
    }
    Root->SetArrayField(TEXT("diagnostics"), Diagnostics);
    return WriteReport(Root, Report, bValid);
}
