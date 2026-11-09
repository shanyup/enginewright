#include "EWTestNativeWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/TextBlock.h"

void UEWTestNativeWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (!WidgetTree || WidgetTree->RootWidget) { return; }
	UBorder* Root = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Root"));
	Root->SetBrushColor(FLinearColor(0.1f, 0.35f, 0.8f, 1.f));
	Root->SetPadding(FMargin(24.f));
	UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Label"));
	Label->SetText(FText::FromString(TEXT("Native C++ widget")));
	Root->SetContent(Label);
	WidgetTree->RootWidget = Root;
}
