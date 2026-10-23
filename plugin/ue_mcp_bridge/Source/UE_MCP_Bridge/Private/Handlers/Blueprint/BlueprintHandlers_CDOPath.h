#pragma once

// Property paths on a Blueprint class default object, shared by
// set_class_default and get_variable_default (#1293).

#include "CoreMinimal.h"
#include "HandlerJsonProperty.h"
#include "HandlerClassResolve.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Blueprint.h"
#include "UObject/UObjectIterator.h"

/** Resolve a path on a CDO: a property, a dotted path through structs, array
 *  elements and subobjects (CharacterMovement.MaxWalkSpeed). The first token
 *  may also name a default subobject (CollisionCylinder.CapsuleRadius). With
 *  bOwnedOnly, a path that leaves the CDO's own subobjects is refused. */
inline bool MCPResolveCDOPath(UObject* CDO, const FString& Path, bool bOwnedOnly,
	FProperty*& OutProp, void*& OutAddr, UObject*& OutOwner, FString& OutError)
{
	if (!CDO) { OutError = TEXT("no class default object"); return false; }
	UObject* Root = CDO;
	FString Rest = Path;
	FString Head, Tail;
	if (Path.Split(TEXT("."), &Head, &Tail) && !CDO->GetClass()->FindPropertyByName(FName(*Head)))
	{
		if (UObject* Sub = CDO->GetDefaultSubobjectByName(FName(*Head)))
		{
			Root = Sub;
			Rest = Tail;
		}
	}
	if (!MCPJsonProperty::ResolveDottedPath(Root, Rest, OutProp, OutAddr, OutOwner, OutError)) return false;
	if (bOwnedOnly && OutOwner && OutOwner != CDO && !OutOwner->IsIn(CDO))
	{
		OutError = FString::Printf(
			TEXT("'%s' leads to %s, which is not one of this Blueprint's own default subobjects; write it on that object instead"),
			*Path, *OutOwner->GetPathName());
		return false;
	}
	return true;
}

/** Blueprint classes deriving from Parent: every one the Asset Registry knows
 *  plus unsaved ones in memory. bLoad loads registry entries not yet in memory. */
inline TArray<UClass*> MCPBlueprintChildClasses(UClass* Parent, bool bLoad, int32 MaxClasses = 500)
{
	TArray<UClass*> Out;
	if (!Parent) return Out;
	TSet<UClass*> Seen;
	auto Accept = [&](UClass* Cls)
	{
		if (!Cls || Cls == Parent || Seen.Contains(Cls) || Out.Num() >= MaxClasses) return;
		if (!Cls->IsChildOf(Parent) || Cls->HasAnyClassFlags(CLASS_NewerVersionExists)) return;
		if (MCPClassResolve::IsTransientClassName(Cls->GetName())) return;
		if (!UBlueprint::GetBlueprintFromClass(Cls)) return;
		Seen.Add(Cls);
		Out.Add(Cls);
	};

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TSet<FTopLevelAssetPath> Derived;
	Registry.GetDerivedClassNames({ Parent->GetClassPathName() }, {}, Derived);
	for (const FTopLevelAssetPath& Path : Derived)
	{
		const FString ClassPath = Path.ToString();
		UClass* Cls = FindObject<UClass>(nullptr, *ClassPath);
		if (!Cls && bLoad) Cls = LoadObject<UClass>(nullptr, *ClassPath);
		Accept(Cls);
	}
	for (TObjectIterator<UClass> It; It; ++It) Accept(*It);

	Out.Sort([](const UClass& A, const UClass& B) { return A.GetPathName() < B.GetPathName(); });
	return Out;
}
