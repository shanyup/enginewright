// Test fixture for the MVVM tools (tests/test_mvvm.py): a C++ view model with a FieldNotify int (Gold), a FieldNotify
// Text (Title) and an int that does not notify (Silver). Copy this file and EWTestViewModel.cpp into the test project's
// module (for example Source/EnginewrightTest/), enable the ModelViewViewModel and EnginewrightMVVM plugins in the
// .uproject, add "ModelViewViewModel" and "FieldNotification" to that module's Build.cs dependencies and rebuild.
// test_mvvm.py looks for /Script/EnginewrightTest.EWTestViewModel and skips the checks when it is missing.
#pragma once

#include "CoreMinimal.h"
#include "MVVMViewModelBase.h"
#include "EWTestViewModel.generated.h"

UCLASS()
class UEWTestViewModel : public UMVVMViewModelBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadWrite, FieldNotify, Setter, Getter, Category = "EWTest")
	int32 Gold = 0;

	UPROPERTY(BlueprintReadWrite, FieldNotify, Setter, Getter, Category = "EWTest")
	FText Title;

	/** Not FieldNotify: a one-way binding to it must be refused. */
	UPROPERTY(BlueprintReadWrite, Category = "EWTest")
	int32 Silver = 0;

	int32 GetGold() const { return Gold; }
	void SetGold(int32 NewGold);
	FText GetTitle() const { return Title; }
	void SetTitle(FText NewTitle);
};
