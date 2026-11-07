#include "EWTypes.h"
#include "EWCommon.h"

#include "EdGraphSchema_K2.h"
#include "Engine/UserDefinedEnum.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/UObjectIterator.h"

namespace EW
{
	template <typename T>
	static T* FindTypeObject(const FString& InName, const TCHAR* Prefix, FString& OutError)
	{
		const FString Name = InName.TrimStartAndEnd();
		if (Name.StartsWith(TEXT("/Script/")))
		{
			if (T* Found = FindObject<T>(nullptr, *Name)) { return Found; }
		}
		else if (Name.StartsWith(TEXT("/")))
		{
			FString Resolved;
			UObject* Obj = ResolveObject(Name, false, Resolved, OutError);
			if (T* Found = Cast<T>(Obj)) { return Found; }
			if (Obj) { OutError = FString::Printf(TEXT("'%s' is a %s, not a %s."), *Name, *Obj->GetClass()->GetName(), *T::StaticClass()->GetName()); }
			return nullptr;
		}
		else
		{
			if (T* Found = FindFirstObject<T>(*Name, EFindFirstObjectOptions::None)) { return Found; }
			if (Name.StartsWith(Prefix) && Name.Len() > 1)
			{
				if (T* Found = FindFirstObject<T>(*Name.RightChop(1), EFindFirstObjectOptions::None)) { return Found; }
			}
		}
		TArray<FString> Names;
		for (TObjectIterator<T> It; It; ++It) { Names.Add(It->GetName()); }
		const TArray<FString> Sug = Suggest(Name, Names);
		OutError = FString::Printf(TEXT("No %s '%s'.%s"), *T::StaticClass()->GetName(), *Name,
			Sug.Num() ? *FString::Printf(TEXT(" Did you mean: %s?"), *FString::Join(Sug, TEXT(", "))) : TEXT(""));
		return nullptr;
	}

	UScriptStruct* FindStruct(const FString& Name, FString& OutError) { return FindTypeObject<UScriptStruct>(Name, TEXT("F"), OutError); }
	UEnum* FindEnum(const FString& Name, FString& OutError) { return FindTypeObject<UEnum>(Name, TEXT("E"), OutError); }

	static bool ParseSingle(const FString& InText, FEdGraphPinType& Out, FString& OutError)
	{
		const FString Text = InText.TrimStartAndEnd();
		FString Kind = Text, Arg;
		Text.Split(TEXT(":"), &Kind, &Arg);
		Kind = Kind.ToLower();
		Out.PinCategory = NAME_None;
		Out.PinSubCategory = NAME_None;
		Out.PinSubCategoryObject = nullptr;

		struct FSimple { const TCHAR* Name; FName Cat; FName Sub; UScriptStruct* Struct; };
		const FSimple Simple[] = {
			{ TEXT("bool"), UEdGraphSchema_K2::PC_Boolean, NAME_None, nullptr },
			{ TEXT("boolean"), UEdGraphSchema_K2::PC_Boolean, NAME_None, nullptr },
			{ TEXT("int"), UEdGraphSchema_K2::PC_Int, NAME_None, nullptr },
			{ TEXT("integer"), UEdGraphSchema_K2::PC_Int, NAME_None, nullptr },
			{ TEXT("int64"), UEdGraphSchema_K2::PC_Int64, NAME_None, nullptr },
			{ TEXT("float"), UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Float, nullptr },
			{ TEXT("double"), UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Double, nullptr },
			{ TEXT("byte"), UEdGraphSchema_K2::PC_Byte, NAME_None, nullptr },
			{ TEXT("string"), UEdGraphSchema_K2::PC_String, NAME_None, nullptr },
			{ TEXT("name"), UEdGraphSchema_K2::PC_Name, NAME_None, nullptr },
			{ TEXT("text"), UEdGraphSchema_K2::PC_Text, NAME_None, nullptr },
			{ TEXT("vector"), UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FVector>::Get() },
			{ TEXT("vector2d"), UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FVector2D>::Get() },
			{ TEXT("rotator"), UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FRotator>::Get() },
			{ TEXT("transform"), UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FTransform>::Get() },
			{ TEXT("color"), UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FLinearColor>::Get() },
			{ TEXT("linearcolor"), UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FLinearColor>::Get() },
		};
		if (Arg.IsEmpty())
		{
			for (const FSimple& S : Simple)
			{
				if (Kind == S.Name)
				{
					Out.PinCategory = S.Cat;
					Out.PinSubCategory = S.Sub;
					Out.PinSubCategoryObject = S.Struct;
					return true;
				}
			}
		}
		if (Kind == TEXT("object") || Kind == TEXT("class") || Kind == TEXT("softobject") || Kind == TEXT("softclass"))
		{
			if (Arg.IsEmpty()) { OutError = FString::Printf(TEXT("'%s' needs a class, e.g. %s:Actor."), *Kind, *Kind); return false; }
			TArray<FString> Sug;
			UClass* C = FindClass(Arg, OutError, &Sug);
			if (!C) { if (Sug.Num()) { OutError += TEXT(" Did you mean: ") + FString::Join(Sug, TEXT(", ")) + TEXT("?"); } return false; }
			Out.PinCategory = Kind == TEXT("object") ? UEdGraphSchema_K2::PC_Object : Kind == TEXT("class") ? UEdGraphSchema_K2::PC_Class
				: Kind == TEXT("softobject") ? UEdGraphSchema_K2::PC_SoftObject : UEdGraphSchema_K2::PC_SoftClass;
			Out.PinSubCategoryObject = C;
			return true;
		}
		if (Kind == TEXT("struct"))
		{
			UScriptStruct* S = FindStruct(Arg, OutError);
			if (!S) { return false; }
			Out.PinCategory = UEdGraphSchema_K2::PC_Struct;
			Out.PinSubCategoryObject = S;
			return true;
		}
		if (Kind == TEXT("enum"))
		{
			UEnum* E = FindEnum(Arg, OutError);
			if (!E) { return false; }
			Out.PinCategory = UEdGraphSchema_K2::PC_Byte;
			Out.PinSubCategoryObject = E;
			return true;
		}
		OutError = FString::Printf(TEXT("Unknown type '%s'. Use bool, int, int64, float, double, byte, string, name, text, vector, vector2d, rotator, transform, color, object:<Class>, class:<Class>, softobject:<Class>, softclass:<Class>, struct:<Struct>, enum:<Enum>; add [] for an array."), *Text);
		return false;
	}

	bool ParsePinType(const FString& InText, FEdGraphPinType& Out, FString& OutError)
	{
		FString Text = InText.TrimStartAndEnd();
		Out = FEdGraphPinType();
		Out.ContainerType = EPinContainerType::None;
		if (Text.EndsWith(TEXT("[]")))
		{
			Text.LeftChopInline(2);
			Out.ContainerType = EPinContainerType::Array;
		}
		else if (Text.StartsWith(TEXT("array:"), ESearchCase::IgnoreCase))
		{
			Text.RightChopInline(6);
			Out.ContainerType = EPinContainerType::Array;
		}
		else if (Text.StartsWith(TEXT("set:"), ESearchCase::IgnoreCase))
		{
			Text.RightChopInline(4);
			Out.ContainerType = EPinContainerType::Set;
		}
		else if (Text.StartsWith(TEXT("map:"), ESearchCase::IgnoreCase))
		{
			FString Key, Value;
			if (!Text.RightChop(4).Split(TEXT(","), &Key, &Value)) { OutError = TEXT("A map type is map:<keytype>,<valuetype>."); return false; }
			FEdGraphPinType ValueType;
			if (!ParseSingle(Value, ValueType, OutError)) { return false; }
			const EPinContainerType Container = EPinContainerType::Map;
			if (!ParseSingle(Key, Out, OutError)) { return false; }
			Out.ContainerType = Container;
			Out.PinValueType = FEdGraphTerminalType::FromPinType(ValueType);
			return true;
		}
		const EPinContainerType Container = Out.ContainerType;
		if (!ParseSingle(Text, Out, OutError)) { return false; }
		Out.ContainerType = Container;
		return true;
	}

	static FString SingleToString(FName Cat, FName Sub, const UObject* SubObj)
	{
		if (Cat == UEdGraphSchema_K2::PC_Real) { return Sub == UEdGraphSchema_K2::PC_Double ? TEXT("double") : TEXT("float"); }
		if (Cat == UEdGraphSchema_K2::PC_Struct)
		{
			if (SubObj == TBaseStructure<FVector>::Get()) { return TEXT("vector"); }
			if (SubObj == TBaseStructure<FVector2D>::Get()) { return TEXT("vector2d"); }
			if (SubObj == TBaseStructure<FRotator>::Get()) { return TEXT("rotator"); }
			if (SubObj == TBaseStructure<FTransform>::Get()) { return TEXT("transform"); }
			if (SubObj == TBaseStructure<FLinearColor>::Get()) { return TEXT("color"); }
			return FString::Printf(TEXT("struct:%s"), SubObj ? *SubObj->GetPathName() : TEXT("?"));
		}
		if (Cat == UEdGraphSchema_K2::PC_Byte && SubObj) { return FString::Printf(TEXT("enum:%s"), *SubObj->GetPathName()); }
		if (Cat == UEdGraphSchema_K2::PC_Enum && SubObj) { return FString::Printf(TEXT("enum:%s"), *SubObj->GetPathName()); }
		if (Cat == UEdGraphSchema_K2::PC_Object || Cat == UEdGraphSchema_K2::PC_Class || Cat == UEdGraphSchema_K2::PC_SoftObject || Cat == UEdGraphSchema_K2::PC_SoftClass)
		{
			return FString::Printf(TEXT("%s:%s"), *Cat.ToString().ToLower(), SubObj ? *SubObj->GetName() : TEXT("?"));
		}
		if (Cat == UEdGraphSchema_K2::PC_Boolean) { return TEXT("bool"); }
		return Cat.ToString().ToLower();
	}

	FString PinTypeToString(const FEdGraphPinType& T)
	{
		const FString Base = SingleToString(T.PinCategory, T.PinSubCategory, T.PinSubCategoryObject.Get());
		switch (T.ContainerType)
		{
		case EPinContainerType::Array: return Base + TEXT("[]");
		case EPinContainerType::Set: return TEXT("set:") + Base;
		case EPinContainerType::Map: return TEXT("map:") + Base + TEXT(",") +
			SingleToString(T.PinValueType.TerminalCategory, T.PinValueType.TerminalSubCategory, T.PinValueType.TerminalSubCategoryObject.Get());
		default: return Base;
		}
	}
}
