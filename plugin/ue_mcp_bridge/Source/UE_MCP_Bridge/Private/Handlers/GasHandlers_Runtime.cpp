// Runtime GAS control: apply a GameplayEffect, and get/set attributes on a
// live actor's AbilitySystemComponent. These are the agnostic "affect a stat"
// test stimuli - they drive the game's OWN effects and attributes rather than
// assuming a damage pipeline, so they work for any GAS game. Non-GAS games set
// reflection-exposed stats via level.set_actor_property or call their own
// functions via editor.invoke_function instead.

#include "GasHandlers.h"
#include "HandlerUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "AttributeSet.h"
#include "GameplayEffect.h"
#include "GameplayEffectTypes.h"
#include "GameplayTagContainer.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

namespace MCPGas
{

int32 AdoptOwnerAttributeSets(
	UAbilitySystemComponent* ASC,
	AActor* Actor,
	TArray<FString>& OutAdoptedClassNames)
{
	if (!ASC || !Actor) return 0;

	TArray<UObject*> Subobjects;
	GetObjectsWithOuter(Actor, Subobjects);

	int32 Adopted = 0;
	for (UObject* Object : Subobjects)
	{
		UAttributeSet* Set = Cast<UAttributeSet>(Object);
		if (!IsValid(Set)) continue;
		// Already registered: leave the existing entry alone. Re-adding would
		// not duplicate it, but skipping keeps the "newly registered" count
		// honest, and that count is what the result reports.
		if (ASC->GetSpawnedAttributes().Contains(Set)) continue;
		ASC->AddSpawnedAttribute(Set);
		OutAdoptedClassNames.Add(Set->GetClass()->GetName());
		++Adopted;
	}
	return Adopted;
}

FStructProperty* FindAttributeDataProperty(UClass* SetClass, const FString& Name)
{
	if (!SetClass) return nullptr;
	const FString SetName = SetClass->GetName();
	for (TFieldIterator<FProperty> It(SetClass); It; ++It)
	{
		FStructProperty* SProp = CastField<FStructProperty>(*It);
		if (!SProp || SProp->Struct != FGameplayAttributeData::StaticStruct()) continue;
		const FString PropName = SProp->GetName();
		if (PropName == Name
			|| (SetName + TEXT(".") + PropName) == Name
			|| (SetName + TEXT(":") + PropName) == Name)
		{
			return SProp;
		}
	}
	return nullptr;
}

FString ListAttributeDataPropertyNames(UClass* SetClass)
{
	TArray<FString> Names;
	if (SetClass)
	{
		for (TFieldIterator<FProperty> It(SetClass); It; ++It)
		{
			FStructProperty* SProp = CastField<FStructProperty>(*It);
			if (SProp && SProp->Struct == FGameplayAttributeData::StaticStruct())
			{
				Names.Add(SProp->GetName());
			}
		}
	}
	return Names.Num() > 0 ? FString::Join(Names, TEXT(", ")) : FString(TEXT("(none)"));
}

}

namespace
{
	// Resolve the world for this call. Defaults to "auto" (prefer PIE), since
	// runtime GAS control is almost always exercised during Play-In-Editor.
	UWorld* ResolveRuntimeWorld(const TSharedPtr<FJsonObject>& Params)
	{
		return ResolveWorldScope(OptionalString(Params, TEXT("world"), TEXT("auto")));
	}

	// Find the actor in the resolved world and return its
	// AbilitySystemComponent. On any failure writes a structured error to
	// OutError and returns nullptr.
	UAbilitySystemComponent* ResolveASC(
		const TSharedPtr<FJsonObject>& Params,
		AActor*& OutActor,
		TSharedPtr<FJsonValue>& OutError)
	{
		FString ActorLabel;
		if (auto Err = RequireStringAlt(Params, TEXT("actorLabel"), TEXT("actorPath"), ActorLabel))
		{
			OutError = Err;
			return nullptr;
		}

		UWorld* World = ResolveRuntimeWorld(Params);
		if (!World)
		{
			OutError = MCPError(TEXT("No world available. For PIE actors, start Play-In-Editor first."));
			return nullptr;
		}

		// #956: label, internal name, or full object path, in that fixed order.
		// A verification actor spawned into the editor world often has no label
		// worth guessing, so the path has to be a first-class way to name it.
		// #983: actorPath is its own parameter now, and a label that names more
		// than one actor is refused rather than answered from one of them.
		FMCPActorSelector ActorSel;
		ActorSel.Match = EMCPActorMatch::LabelNameOrPath;
		ActorSel.WorldLabel = World->IsPlayInEditor() ? TEXT("PIE") : TEXT("editor");
		AActor* Actor = MCPResolveActor(World, Params, OutError, ActorSel);
		if (!Actor) return nullptr;
		ActorLabel = Actor->GetActorLabel();
		OutActor = Actor;

		UAbilitySystemComponent* ASC =
			UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Actor);
		if (!ASC)
		{
			OutError = MCPError(FString::Printf(
				TEXT("Actor '%s' has no AbilitySystemComponent (not a GAS actor)"), *ActorLabel));
			return nullptr;
		}
		return ASC;
	}

	// Resolve a FGameplayAttribute by name against the ASC's spawned attribute
	// sets. Accepts a bare property name ("Health") or qualified forms
	// ("HealthSet.Health" / "HealthSet:Health"). Returns an invalid attribute
	// on miss; writes the matched set name to OutSetName on hit.
	FGameplayAttribute FindAttributeByName(
		UAbilitySystemComponent* ASC,
		const FString& Name,
		FString& OutSetName)
	{
		for (const UAttributeSet* Set : ASC->GetSpawnedAttributes())
		{
			if (!Set) continue;
			UClass* SetClass = Set->GetClass();
			const FString SetName = SetClass->GetName();
			for (TFieldIterator<FProperty> It(SetClass); It; ++It)
			{
				FStructProperty* SProp = CastField<FStructProperty>(*It);
				if (!SProp || SProp->Struct != FGameplayAttributeData::StaticStruct()) continue;
				const FString PropName = SProp->GetName();
				if (PropName == Name
					|| (SetName + TEXT(".") + PropName) == Name
					|| (SetName + TEXT(":") + PropName) == Name)
				{
					OutSetName = SetName;
					return FGameplayAttribute(SProp);
				}
			}
		}
		return FGameplayAttribute();
	}

	// Append one attribute's name/base/current to a JSON object.
	void WriteAttributeRow(
		TSharedPtr<FJsonObject> Obj,
		UAbilitySystemComponent* ASC,
		const FGameplayAttribute& Attr)
	{
		Obj->SetStringField(TEXT("attribute"), Attr.GetName());
		Obj->SetNumberField(TEXT("baseValue"), ASC->GetNumericAttributeBase(Attr));
		Obj->SetNumberField(TEXT("currentValue"), ASC->GetNumericAttribute(Attr));
	}

	// ── Registered attribute sets (#956) ──────────────────────────────
	//
	// UAbilitySystemComponent::GetAttributeSet / GetSet<T>() search
	// SpawnedAttributes, and SpawnedAttributes IS the registry the gameplay
	// code consults. Nothing else is. An actor's own UPROPERTY pointer to a
	// CreateDefaultSubobject-created set is NOT proof the ASC knows about it.
	//
	// Those two only agree once InitializeComponent has run. It is
	// InitializeComponent that scans the owner's default subobjects and calls
	// AddSpawnedAttribute on every UAttributeSet it finds, and an actor spawned
	// into the pure editor world never reaches it: there is no BeginPlay and no
	// InitializeComponent in a world that has not begun play. So in the editor
	// world the ASC has ZERO registered sets while the actor plainly has one.
	//
	// The trap that follows, and the reason this comment is this long: the
	// obvious fallback at that point is "the ASC has none, so make one", which
	// constructs a SECOND, disconnected instance next to the actor's own. Every
	// read and write then lands on an object no gameplay code will ever look
	// at, no error is reported, and the verification says the change worked. So
	// we adopt the actor's EXISTING subobject into the ASC instead, which is
	// exactly what InitializeComponent would have done, and we only ever hand
	// back what GetAttributeSet returns afterwards.

	/**
	 * The instance the gameplay code consults, and nothing else.
	 *
	 * Always answered by ASC->GetAttributeSet (the non-template form of
	 * ASC->GetSet<T>()). When the ASC has not registered one and adoption is
	 * allowed, the actor's own subobject is registered first and the ASC is
	 * asked again, so the pointer that comes back is still the registered one.
	 * A new set is never constructed here.
	 */
	UAttributeSet* ResolveRegisteredAttributeSet(
		UAbilitySystemComponent* ASC,
		AActor* Actor,
		UClass* SetClass,
		bool bAllowAdopt,
		bool& bOutAdopted,
		TArray<FString>& OutAdoptedClassNames,
		TSharedPtr<FJsonValue>& OutError)
	{
		bOutAdopted = false;
		if (const UAttributeSet* Registered = ASC->GetAttributeSet(SetClass))
		{
			// const_cast is the only way to a mutable registered set: the ASC
			// hands out const pointers and keeps no non-const accessor. The
			// object is not const, only the view of it.
			return const_cast<UAttributeSet*>(Registered);
		}

		if (bAllowAdopt && MCPGas::AdoptOwnerAttributeSets(ASC, Actor, OutAdoptedClassNames) > 0)
		{
			if (const UAttributeSet* Registered = ASC->GetAttributeSet(SetClass))
			{
				bOutAdopted = true;
				return const_cast<UAttributeSet*>(Registered);
			}
		}

		OutError = MCPError(FString::Printf(
			TEXT("No '%s' is registered on '%s' AbilitySystemComponent%s. ")
			TEXT("A set the actor owns is only registered once InitializeComponent runs, which never happens in a world that has not begun play. ")
			TEXT("Start PIE, or call gas(action=\"init_asc\") with attributeSet=\"%s\" to register it."),
			*SetClass->GetName(),
			*Actor->GetActorLabel(),
			bAllowAdopt ? TEXT(", and the actor owns no instance of it either") : TEXT(" (registerOwnerSets was false)"),
			*SetClass->GetName()));
		return nullptr;
	}

	// Resolve a UClass deriving from Base from a content path or short class name.
	// Handles native classes, Blueprint generated classes (path + "_C"), and a
	// Blueprint-asset fallback. Returns nullptr unless the result is a Base subclass.
	UClass* ResolveClassDeriving(const FString& Spec, UClass* Base)
	{
		auto Ok = [Base](UClass* C) { return C && Base && C->IsChildOf(Base); };

		if (Spec.Contains(TEXT("/")))
		{
			if (UClass* C = LoadObject<UClass>(nullptr, *Spec); Ok(C)) return C;
			FString AssetName;
			Spec.Split(TEXT("/"), nullptr, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			const FString ClassPath = Spec + TEXT(".") + AssetName + TEXT("_C");
			if (UClass* C = LoadObject<UClass>(nullptr, *ClassPath); Ok(C)) return C;
			if (UBlueprint* BP = LoadAssetByPath<UBlueprint>(Spec))
			{
				if (Ok(BP->GeneratedClass)) return BP->GeneratedClass;
			}
			return nullptr;
		}

		UClass* C = FindClassByShortName(Spec);
		return Ok(C) ? C : nullptr;
	}
}

TSharedPtr<FJsonValue> FGasHandlers::ApplyEffect(const TSharedPtr<FJsonObject>& Params)
{
	MCP_CHECK_GAME_THREAD();

	FString EffectSpec;
	if (auto Err = RequireStringAlt(Params, TEXT("effectClass"), TEXT("effectPath"), EffectSpec)) return Err;

	AActor* Actor = nullptr;
	TSharedPtr<FJsonValue> Err;
	UAbilitySystemComponent* ASC = ResolveASC(Params, Actor, Err);
	if (!ASC) return Err;

	UClass* EffectClass = ResolveClassDeriving(EffectSpec, UGameplayEffect::StaticClass());
	if (!EffectClass)
	{
		return MCPError(FString::Printf(
			TEXT("GameplayEffect class not found: %s (pass a content path or class name)"), *EffectSpec));
	}

	const float Level = static_cast<float>(OptionalNumber(Params, TEXT("level"), 1.0));

	FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
	Context.AddInstigator(Actor, Actor);
	FGameplayEffectSpecHandle SpecHandle = ASC->MakeOutgoingSpec(EffectClass, Level, Context);
	if (!SpecHandle.IsValid() || !SpecHandle.Data.IsValid())
	{
		return MCPError(TEXT("Failed to build a GameplayEffectSpec for the effect"));
	}

	// SetByCaller magnitudes: { "<tag-or-name>": <number> }. Prefer a gameplay
	// tag when the key resolves to one; otherwise use the FName overload.
	const TSharedPtr<FJsonObject>* SetByCaller = nullptr;
	TArray<FString> AppliedKeys;
	if (Params->TryGetObjectField(TEXT("setByCaller"), SetByCaller) && SetByCaller && (*SetByCaller).IsValid())
	{
		for (const auto& KV : (*SetByCaller)->Values)
		{
			double Mag = 0.0;
			if (!KV.Value.IsValid() || !KV.Value->TryGetNumber(Mag)) continue;
			const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*KV.Key), /*ErrorIfNotFound*/ false);
			if (Tag.IsValid())
			{
				SpecHandle.Data->SetSetByCallerMagnitude(Tag, static_cast<float>(Mag));
			}
			else
			{
				SpecHandle.Data->SetSetByCallerMagnitude(FName(*KV.Key), static_cast<float>(Mag));
			}
			AppliedKeys.Add(FString(*KV.Key));
		}
	}

	const FActiveGameplayEffectHandle Active = ASC->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data);

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("actorLabel"), Actor->GetActorLabel());
	Result->SetStringField(TEXT("effect"), EffectClass->GetPathName());
	Result->SetNumberField(TEXT("level"), Level);
	Result->SetBoolField(TEXT("applied"), Active.WasSuccessfullyApplied());
	// Duration/Infinite effects produce a live handle; instant effects don't.
	Result->SetBoolField(TEXT("durationActive"), Active.IsValid());
	if (AppliedKeys.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Keys;
		for (const FString& K : AppliedKeys) Keys.Add(MakeShared<FJsonValueString>(K));
		Result->SetArrayField(TEXT("setByCaller"), Keys);
	}
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FGasHandlers::SetAttribute(const TSharedPtr<FJsonObject>& Params)
{
	MCP_CHECK_GAME_THREAD();

	FString AttrName;
	if (auto Err = RequireString(Params, TEXT("attribute"), AttrName)) return Err;

	double NewValue = 0.0;
	if (!Params->TryGetNumberField(TEXT("value"), NewValue))
	{
		return MCPError(TEXT("Missing required parameter 'value'"));
	}

	AActor* Actor = nullptr;
	TSharedPtr<FJsonValue> Err;
	UAbilitySystemComponent* ASC = ResolveASC(Params, Actor, Err);
	if (!ASC) return Err;

	FString SetName;
	const FGameplayAttribute Attr = FindAttributeByName(ASC, AttrName, SetName);
	if (!Attr.IsValid())
	{
		return MCPError(FString::Printf(
			TEXT("Attribute '%s' not found on '%s'. Use get_attribute with no 'attribute' to list available ones."),
			*AttrName, *Actor->GetActorLabel()));
	}

	const float OldBase = ASC->GetNumericAttributeBase(Attr);
	// SetNumericAttributeBase recalculates CurrentValue through the aggregator,
	// so dependent modifiers stay consistent (unlike a raw property write).
	ASC->SetNumericAttributeBase(Attr, static_cast<float>(NewValue));

	auto Result = MCPSuccess();
	MCPSetUpdated(Result);
	Result->SetStringField(TEXT("actorLabel"), Actor->GetActorLabel());
	Result->SetStringField(TEXT("attributeSet"), SetName);
	Result->SetStringField(TEXT("attribute"), Attr.GetName());
	Result->SetNumberField(TEXT("previousBaseValue"), OldBase);
	Result->SetNumberField(TEXT("baseValue"), ASC->GetNumericAttributeBase(Attr));
	Result->SetNumberField(TEXT("currentValue"), ASC->GetNumericAttribute(Attr));
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FGasHandlers::GetAttribute(const TSharedPtr<FJsonObject>& Params)
{
	MCP_CHECK_GAME_THREAD();

	AActor* Actor = nullptr;
	TSharedPtr<FJsonValue> Err;
	UAbilitySystemComponent* ASC = ResolveASC(Params, Actor, Err);
	if (!ASC) return Err;

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("actorLabel"), Actor->GetActorLabel());

	const FString AttrName = OptionalString(Params, TEXT("attribute"));
	if (!AttrName.IsEmpty())
	{
		FString SetName;
		const FGameplayAttribute Attr = FindAttributeByName(ASC, AttrName, SetName);
		if (!Attr.IsValid())
		{
			return MCPError(FString::Printf(
				TEXT("Attribute '%s' not found on '%s'"), *AttrName, *Actor->GetActorLabel()));
		}
		Result->SetStringField(TEXT("attributeSet"), SetName);
		WriteAttributeRow(Result, ASC, Attr);
		return MCPResult(Result);
	}

	// No attribute named: enumerate every attribute across all spawned sets.
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const UAttributeSet* Set : ASC->GetSpawnedAttributes())
	{
		if (!Set) continue;
		UClass* SetClass = Set->GetClass();
		const FString SetName = SetClass->GetName();
		for (TFieldIterator<FProperty> It(SetClass); It; ++It)
		{
			FStructProperty* SProp = CastField<FStructProperty>(*It);
			if (!SProp || SProp->Struct != FGameplayAttributeData::StaticStruct()) continue;
			const FGameplayAttribute Attr(SProp);
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("attributeSet"), SetName);
			WriteAttributeRow(Row, ASC, Attr);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
	}
	Result->SetArrayField(TEXT("attributes"), Rows);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	return MCPResult(Result);
}

// ── Live attribute values on the REGISTERED set (#956) ───────────────────────
//
// gas(get_attribute) / gas(set_attribute) walk whatever the ASC already has
// registered and match by attribute name. That is the right shape once a game
// has begun play and its sets are registered. It answers nothing at all for an
// actor spawned into the editor world for a verification pass, because nothing
// is registered there yet.
//
// These two name the attribute set explicitly, resolve the instance THROUGH THE
// ASC, and register the actor's own subobject when the ASC has not (see the
// long note on ResolveRegisteredAttributeSet above for why constructing a new
// set instead is the trap that silently reads and writes an object no gameplay
// code consults). Both report the registered instance's object path, so a
// caller can prove which object was touched.

namespace
{
	/** Params shared by both live-attribute actions. */
	struct FLiveAttributeRequest
	{
		AActor* Actor = nullptr;
		UAbilitySystemComponent* ASC = nullptr;
		UAttributeSet* Set = nullptr;
		FGameplayAttribute Attribute;
		bool bAdopted = false;
		TArray<FString> AdoptedClassNames;
	};

	/** Everything both actions do before they diverge. */
	bool ResolveLiveAttributeRequest(
		const TSharedPtr<FJsonObject>& Params,
		FLiveAttributeRequest& Out,
		TSharedPtr<FJsonValue>& OutError)
	{
		FString SetSpec;
		if (auto Err = RequireString(Params, TEXT("attributeSet"), SetSpec))
		{
			OutError = Err;
			return false;
		}
		FString AttrName;
		if (auto Err = RequireString(Params, TEXT("attribute"), AttrName))
		{
			OutError = Err;
			return false;
		}

		Out.ASC = ResolveASC(Params, Out.Actor, OutError);
		if (!Out.ASC) return false;

		UClass* SetClass = ResolveClassDeriving(SetSpec, UAttributeSet::StaticClass());
		if (!SetClass)
		{
			OutError = MCPError(FString::Printf(
				TEXT("AttributeSet class not found: %s (pass a content path or a class name)"), *SetSpec));
			return false;
		}

		const bool bAllowAdopt = OptionalBool(Params, TEXT("registerOwnerSets"), true);
		Out.Set = ResolveRegisteredAttributeSet(
			Out.ASC, Out.Actor, SetClass, bAllowAdopt, Out.bAdopted, Out.AdoptedClassNames, OutError);
		if (!Out.Set) return false;

		// Match against the REGISTERED instance's class, not the requested one:
		// the registered set may be a subclass of what the caller named, and its
		// own properties are the ones the aggregator reads.
		FStructProperty* AttrProp = MCPGas::FindAttributeDataProperty(Out.Set->GetClass(), AttrName);
		if (!AttrProp)
		{
			OutError = MCPError(FString::Printf(
				TEXT("Attribute '%s' not found on '%s'. Available: %s"),
				*AttrName,
				*Out.Set->GetClass()->GetName(),
				*MCPGas::ListAttributeDataPropertyNames(Out.Set->GetClass())));
			return false;
		}
		Out.Attribute = FGameplayAttribute(AttrProp);
		return true;
	}

	/** Identity + both values of the attribute, read off the registered set. */
	void WriteLiveAttributeFields(TSharedPtr<FJsonObject> Obj, const FLiveAttributeRequest& Req)
	{
		Obj->SetStringField(TEXT("actorLabel"), Req.Actor->GetActorLabel());
		Obj->SetStringField(TEXT("actorPath"), Req.Actor->GetPathName());
		Obj->SetStringField(TEXT("attributeSet"), Req.Set->GetClass()->GetName());
		// The proof of which object was touched. A second, disconnected instance
		// would show a different path here.
		Obj->SetStringField(TEXT("attributeSetInstance"), Req.Set->GetPathName());
		Obj->SetStringField(TEXT("attribute"), Req.Attribute.GetName());
		Obj->SetBoolField(TEXT("registeredOnAsc"), true);
		Obj->SetBoolField(TEXT("registeredByThisCall"), Req.bAdopted);
		if (Req.AdoptedClassNames.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Adopted;
			for (const FString& Name : Req.AdoptedClassNames)
			{
				Adopted.Add(MakeShared<FJsonValueString>(Name));
			}
			Obj->SetArrayField(TEXT("registeredSets"), Adopted);
		}

		// Read straight off the registered instance. GetNumericValue is the
		// engine's own accessor for the current value of the attribute data.
		Obj->SetNumberField(TEXT("currentValue"), Req.Attribute.GetNumericValue(Req.Set));
		if (const FGameplayAttributeData* Data = Req.Attribute.GetGameplayAttributeData(Req.Set))
		{
			Obj->SetNumberField(TEXT("baseValue"), Data->GetBaseValue());
		}
		// The aggregator's view of the same attribute. Identical to the values
		// above until an active GameplayEffect is modifying it, and the pair is
		// what tells you an effect really landed.
		Obj->SetNumberField(TEXT("aggregatorBaseValue"), Req.ASC->GetNumericAttributeBase(Req.Attribute));
		Obj->SetNumberField(TEXT("aggregatorCurrentValue"), Req.ASC->GetNumericAttribute(Req.Attribute));
	}
}

TSharedPtr<FJsonValue> FGasHandlers::GetLiveAttributeValue(const TSharedPtr<FJsonObject>& Params)
{
	MCP_CHECK_GAME_THREAD();

	FLiveAttributeRequest Req;
	TSharedPtr<FJsonValue> Err;
	if (!ResolveLiveAttributeRequest(Params, Req, Err)) return Err;

	auto Result = MCPSuccess();
	WriteLiveAttributeFields(Result, Req);
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FGasHandlers::SetLiveAttributeValue(const TSharedPtr<FJsonObject>& Params)
{
	MCP_CHECK_GAME_THREAD();

	double NewValue = 0.0;
	if (!Params->TryGetNumberField(TEXT("value"), NewValue))
	{
		return MCPError(TEXT("Missing required parameter 'value'"));
	}

	// "current" writes the FGameplayAttributeData's current value in place,
	// which is what verifying a mid-combat state needs. "base" goes through the
	// ASC so the aggregator recomputes the current value from it, which is what
	// a durable change needs. They are not interchangeable, so the caller says.
	const FString ValueType = OptionalString(Params, TEXT("valueType"), TEXT("current")).ToLower();
	if (ValueType != TEXT("current") && ValueType != TEXT("base"))
	{
		return MCPError(FString::Printf(
			TEXT("Unknown valueType '%s'. Use \"current\" (write the attribute data in place) or \"base\" (write through the ASC aggregator)."),
			*ValueType));
	}

	FLiveAttributeRequest Req;
	TSharedPtr<FJsonValue> Err;
	if (!ResolveLiveAttributeRequest(Params, Req, Err)) return Err;

	const float PreviousCurrent = Req.Attribute.GetNumericValue(Req.Set);
	const float PreviousBase = Req.ASC->GetNumericAttributeBase(Req.Attribute);

	if (ValueType == TEXT("base"))
	{
		// Recalculates the current value through the aggregator, so active
		// modifiers stay consistent.
		Req.ASC->SetNumericAttributeBase(Req.Attribute, static_cast<float>(NewValue));
	}
	else
	{
		// SetNumericValueChecked takes a mutable reference because the set's
		// PreAttributeChange is allowed to clamp the value, so the number that
		// lands can differ from the number asked for. That is why the result
		// reports what was actually stored rather than echoing the request.
		float Applied = static_cast<float>(NewValue);
		Req.Attribute.SetNumericValueChecked(Applied, Req.Set);
	}

	auto Result = MCPSuccess();
	MCPSetUpdated(Result);
	WriteLiveAttributeFields(Result, Req);
	Result->SetStringField(TEXT("valueType"), ValueType);
	Result->SetNumberField(TEXT("requestedValue"), NewValue);
	Result->SetNumberField(TEXT("previousCurrentValue"), PreviousCurrent);
	Result->SetNumberField(TEXT("previousBaseValue"), PreviousBase);
	return MCPResult(Result);
}

// #587 get_asc_state - introspect a live ASC: granted ability specs (class,
// level, input id, active state, dynamic source tags) plus the ASC's owned
// gameplay tags. The read half of #587 (input injection lives in pie-studio).
TSharedPtr<FJsonValue> FGasHandlers::GetAscState(const TSharedPtr<FJsonObject>& Params)
{
	MCP_CHECK_GAME_THREAD();

	AActor* Actor = nullptr;
	TSharedPtr<FJsonValue> Err;
	UAbilitySystemComponent* ASC = ResolveASC(Params, Actor, Err);
	if (!ASC) return Err;

	// Granted / activatable ability specs.
	TArray<TSharedPtr<FJsonValue>> Abilities;
	for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities())
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("class"), Spec.Ability ? Spec.Ability->GetClass()->GetName() : TEXT("None"));
		A->SetNumberField(TEXT("level"), Spec.Level);
		A->SetNumberField(TEXT("inputID"), Spec.InputID);
		A->SetStringField(TEXT("handle"), Spec.Handle.ToString());
		A->SetBoolField(TEXT("active"), Spec.IsActive());
		A->SetNumberField(TEXT("activeCount"), Spec.ActiveCount);

		TArray<TSharedPtr<FJsonValue>> DynTags;
		for (const FGameplayTag& T : Spec.GetDynamicSpecSourceTags())
		{
			DynTags.Add(MakeShared<FJsonValueString>(T.ToString()));
		}
		A->SetArrayField(TEXT("dynamicTags"), DynTags);
		Abilities.Add(MakeShared<FJsonValueObject>(A));
	}

	// Owned gameplay tags currently on the ASC.
	FGameplayTagContainer Owned;
	ASC->GetOwnedGameplayTags(Owned);
	TArray<TSharedPtr<FJsonValue>> OwnedJson;
	for (const FGameplayTag& T : Owned)
	{
		OwnedJson.Add(MakeShared<FJsonValueString>(T.ToString()));
	}

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("actorLabel"), Actor->GetActorLabel());
	Result->SetArrayField(TEXT("abilities"), Abilities);
	Result->SetNumberField(TEXT("abilityCount"), Abilities.Num());
	Result->SetArrayField(TEXT("ownedTags"), OwnedJson);
	Result->SetNumberField(TEXT("ownedTagCount"), OwnedJson.Num());
	return MCPResult(Result);
}

TSharedPtr<FJsonValue> FGasHandlers::InitAsc(const TSharedPtr<FJsonObject>& Params)
{
	MCP_CHECK_GAME_THREAD();

	AActor* Actor = nullptr;
	TSharedPtr<FJsonValue> Err;
	UAbilitySystemComponent* ASC = ResolveASC(Params, Actor, Err);
	if (!ASC) return Err;

	// Establish owner/avatar so abilities activate and effect contexts target
	// correctly. Safe to call again; a game's own pawn may also init the ASC.
	ASC->InitAbilityActorInfo(Actor, Actor);

	// #956: register the actor's OWN attribute set subobjects first, the way
	// InitializeComponent would have at BeginPlay. Skipping this step and going
	// straight to "the ASC has none, so construct one" is what produced a
	// second, disconnected instance next to the actor's own: reads and writes
	// then landed on an object the gameplay code never consults, and nothing
	// reported an error. Adoption is idempotent and costs one hash walk.
	TArray<FString> AdoptedSets;
	MCPGas::AdoptOwnerAttributeSets(ASC, Actor, AdoptedSets);

	// Optionally guarantee an attribute set exists on the ASC. This is what lets
	// a bridge-authored test actor have live attributes without shipping an init
	// DataTable. A set is only constructed when the actor genuinely owns none of
	// that class, and the result says which of the two happened.
	FString CreatedSet;
	bool bConstructedSet = false;
	const FString AttrSetSpec = OptionalString(Params, TEXT("attributeSet"));
	if (!AttrSetSpec.IsEmpty())
	{
		UClass* AttrSetClass = ResolveClassDeriving(AttrSetSpec, UAttributeSet::StaticClass());
		if (!AttrSetClass)
		{
			return MCPError(FString::Printf(
				TEXT("AttributeSet class not found: %s (pass a content path or class name)"), *AttrSetSpec));
		}
		const UAttributeSet* Existing = ASC->GetAttributeSet(AttrSetClass);
		if (!Existing)
		{
			UAttributeSet* NewSet = NewObject<UAttributeSet>(Actor, AttrSetClass);
			ASC->AddSpawnedAttribute(NewSet);
			CreatedSet = NewSet->GetClass()->GetName();
			bConstructedSet = true;
		}
		else
		{
			CreatedSet = Existing->GetClass()->GetName();
		}
	}

	// Count attributes now live across all spawned sets.
	int32 AttrCount = 0;
	for (const UAttributeSet* Set : ASC->GetSpawnedAttributes())
	{
		if (!Set) continue;
		for (TFieldIterator<FProperty> It(Set->GetClass()); It; ++It)
		{
			FStructProperty* SProp = CastField<FStructProperty>(*It);
			if (SProp && SProp->Struct == FGameplayAttributeData::StaticStruct()) ++AttrCount;
		}
	}

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("actorLabel"), Actor->GetActorLabel());
	Result->SetBoolField(TEXT("initialized"), true);
	if (!CreatedSet.IsEmpty())
	{
		Result->SetStringField(TEXT("attributeSet"), CreatedSet);
		// The caller needs to know which happened. A constructed set starts at
		// its class defaults; an adopted one carries whatever the actor has.
		Result->SetBoolField(TEXT("attributeSetConstructed"), bConstructedSet);
	}
	if (AdoptedSets.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Adopted;
		for (const FString& Name : AdoptedSets) Adopted.Add(MakeShared<FJsonValueString>(Name));
		Result->SetArrayField(TEXT("registeredOwnerSets"), Adopted);
	}
	Result->SetNumberField(TEXT("attributeCount"), AttrCount);
	return MCPResult(Result);
}
