#include "AngelIOMissionDetails.h"
#include "AngelIOScriptComponent.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Engine/Level.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GameFramework/Actor.h"
#include "IPropertyUtilities.h"
#include "Misc/DefaultValueHelper.h"
#include "ScopedTransaction.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
bool ReadValue(const FString& Text, EAngelIOType Type, FAngelIOValue& Out)
{
    Out.Type = Type;
    if (Type == EAngelIOType::Integer) return FDefaultValueHelper::ParseInt(Text, Out.Integer);
    if (Type == EAngelIOType::Float) return FDefaultValueHelper::ParseFloat(Text, Out.Float) && FMath::IsFinite(Out.Float);
    Out.Boolean = Text.Equals(TEXT("true"), ESearchCase::IgnoreCase);
    return Out.Boolean || Text.Equals(TEXT("false"), ESearchCase::IgnoreCase);
}

FText ValueText(const FAngelIOValue* Value)
{
    if (!Value) return FText::GetEmpty();
    if (Value->Type == EAngelIOType::Boolean) return FText::FromString(Value->Boolean ? TEXT("true") : TEXT("false"));
    return FText::FromString(Value->Type == EAngelIOType::Integer ? LexToString(Value->Integer) : LexToString(Value->Float));
}
}

void FAngelIOMissionDetails::CustomizeDetails(IDetailLayoutBuilder& Layout)
{
    TArray<TWeakObjectPtr<UObject>> Objects;
    Layout.GetObjectsBeingCustomized(Objects);
    if (Objects.Num() != 1) return;
    TWeakObjectPtr<UAngelIOScriptComponent> Mission = Cast<UAngelIOScriptComponent>(Objects[0].Get());
    if (!Mission.IsValid()) return;
    auto Utils = Layout.GetPropertyUtilities();
    auto Script = Layout.GetProperty(GET_MEMBER_NAME_CHECKED(UAngelIOScriptComponent, Script));
    Script->SetOnPropertyValueChanged(FSimpleDelegate::CreateLambda([Utils]{ Utils->RequestForceRefresh(); }));
    auto& Category = Layout.EditCategory(TEXT("Mission Contract"));
    FAngelIOProgram Program;
    if (Mission->Script.IsEmpty()) return;
    if (!Program.CompileFile(Mission->Script))
    {
        for (const auto& D : Program.Diagnostics())
            Category.AddCustomRow(FText::FromString(D.Message)).WholeRowContent()
                [SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("%s (%d) %s"), *Mission->Script, D.Line, *D.Message))).AutoWrapText(true)];
        return;
    }
    for (const auto& Port : Program.Ports())
    {
        const TCHAR* Type = Port.Type == EAngelIOType::Integer ? TEXT("int") : Port.Type == EAngelIOType::Float ? TEXT("float") : TEXT("bool");
        const FText Label = FText::FromString(FString::Printf(TEXT("%s %s (%s)"), Port.bInput ? TEXT("in") : TEXT("out"), *Port.Name, Type));
        auto& Row = Category.AddCustomRow(Label);
        Row.NameContent()[SNew(STextBlock).Text(Label)];
        if (!Port.bInput)
        {
            Row.ValueContent()[SNew(STextBlock).Text(FText::FromString(Port.DefaultExpression))];
            continue;
        }
        auto Inputs = Layout.GetProperty(GET_MEMBER_NAME_CHECKED(UAngelIOScriptComponent, InitialInputs));
        Row.ValueContent()[SNew(SEditableTextBox)
            .HintText(FText::FromString(Port.DefaultExpression))
            .ToolTipText(FText::FromString(TEXT("Script default: ") + Port.DefaultExpression + TEXT(". Clear the override to use it.")))
            .Text_Lambda([Mission, Port]{ return ValueText(Mission.IsValid() ? Mission->InitialInputs.Find(Port.Name) : nullptr); })
            .OnVerifyTextChanged_Lambda([Port, Type](const FText& Text, FText& Error) {
                FAngelIOValue Value;
                if (Text.IsEmpty() || ReadValue(Text.ToString(), Port.Type, Value)) return true;
                Error = FText::FromString(FString::Printf(TEXT("Expected %s"), Type));
                return false;
            })
            .OnTextCommitted_Lambda([Mission, Port, Inputs](const FText& Text, ETextCommit::Type) {
                if (!Mission.IsValid()) return;
                FAngelIOValue Value;
                if (!Text.IsEmpty() && !ReadValue(Text.ToString(), Port.Type, Value)) return;
                FScopedTransaction Transaction(NSLOCTEXT("AngelIO", "EditInput", "Edit mission input"));
                Mission->Modify();
                Inputs->NotifyPreChange();
                if (Text.IsEmpty()) Mission->InitialInputs.Remove(Port.Name);
                else Mission->InitialInputs.Add(Port.Name, Value);
                Inputs->NotifyPostChange(EPropertyChangeType::ValueSet);
                Inputs->NotifyFinishedChangingProperties();
            })];
    }
    TSet<FName> Shown;
    for (const auto& Target : Program.LiteralTargets())
    {
        const FName Alias(*Target.Name);
        if (Shown.Contains(Alias)) continue;
        Shown.Add(Alias);
        const auto Label = FText::FromString(TEXT("Target ") + Target.Name);
        auto Bindings = Layout.GetProperty(GET_MEMBER_NAME_CHECKED(UAngelIOScriptComponent, RoomTargets));
        Category.AddCustomRow(Label).NameContent()[SNew(STextBlock).Text(Label)].ValueContent()
        [SNew(SComboButton).ButtonContent()[SNew(STextBlock).Text_Lambda([Mission, Alias] {
            const FName* Bound = Mission.IsValid() ? Mission->RoomTargets.Find(Alias) : nullptr;
            return FText::FromString(Bound ? TEXT("Room / ") + Bound->ToString() : TEXT("World / ") + Alias.ToString());
        })].OnGetMenuContent_Lambda([Mission, Bindings, Alias, Target] {
            FMenuBuilder Menu(true, nullptr);
            auto Option = [&](FName Name) {
                Menu.AddMenuEntry(FText::FromString(Name.IsNone() ? TEXT("Use global name") : Name.ToString()), FText(), FSlateIcon(),
                    FUIAction(FExecuteAction::CreateLambda([Mission, Bindings, Alias, Name] {
                        if (!Mission.IsValid()) return;
                        FScopedTransaction Transaction(NSLOCTEXT("AngelIO", "BindTarget", "Bind mission target"));
                        Mission->Modify();
                        Bindings->NotifyPreChange();
                        if (Name.IsNone()) Mission->RoomTargets.Remove(Alias);
                        else Mission->RoomTargets.Add(Alias, Name);
                        Bindings->NotifyPostChange(EPropertyChangeType::ValueSet);
                        Bindings->NotifyFinishedChangingProperties();
                    })));
            };
            Option(NAME_None);
            TArray<FName> Names;
            if (Mission.IsValid() && Mission->GetOwner() && Mission->GetOwner()->GetLevel())
                for (AActor* Actor : Mission->GetOwner()->GetLevel()->Actors)
                    if (IsValid(Actor) && (!Target.OwnerClass || Actor->IsA(Target.OwnerClass)))
                        for (const auto* Endpoint : TInlineComponentArray<UAngelIOScriptComponent*>(Actor))
                            if (!Endpoint->GetTargetName().IsNone()) Names.AddUnique(Endpoint->GetTargetName());
            Names.Sort(FNameLexicalLess());
            for (FName Name : Names) Option(Name);
            return Menu.MakeWidget();
        })];
    }
}
