// Test fixture for widget_render (tests/test_umg.py): a C++ UserWidget that builds its whole tree in code, the way
// many games write their UI. Copy this file and EWTestNativeWidget.cpp into the test project's module (for example
// Source/EnginewrightTest/), add "UMG", "Slate" and "SlateCore" to that module's Build.cs dependencies and rebuild.
// test_umg.py looks for /Script/EnginewrightTest.EWTestNativeWidget and skips the check when it is missing.
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "EWTestNativeWidget.generated.h"

UCLASS()
class UEWTestNativeWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual void NativeOnInitialized() override;
};
