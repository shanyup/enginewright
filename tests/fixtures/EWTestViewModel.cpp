#include "EWTestViewModel.h"

void UEWTestViewModel::SetGold(int32 NewGold)
{
	UE_MVVM_SET_PROPERTY_VALUE(Gold, NewGold);
}

void UEWTestViewModel::SetTitle(FText NewTitle)
{
	UE_MVVM_SET_PROPERTY_VALUE(Title, NewTitle);
}
