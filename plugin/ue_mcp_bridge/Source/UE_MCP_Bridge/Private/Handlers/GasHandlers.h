#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Templates/Function.h"

class AActor;
class UAbilitySystemComponent;
class UClass;

/**
 * Attribute set registration on a live AbilitySystemComponent (#956).
 *
 * Declared here rather than kept file-local so the automation tests can pin
 * the one invariant the live-attribute actions stand on, and so there is
 * exactly one copy: the module is a unity build and a second file-local copy
 * of a helper is a redefinition on some grouping.
 */
namespace MCPGas
{

/**
 * Register the actor's own UAttributeSet subobjects on its ASC, the way
 * UAbilitySystemComponent::InitializeComponent does at BeginPlay.
 *
 * This exists because an actor spawned into the pure editor world never runs
 * that scan: there is no BeginPlay and no InitializeComponent in a world that
 * has not begun play. The ASC therefore has ZERO registered sets while the
 * actor plainly owns one, and the obvious fallback at that point ("the ASC has
 * none, so construct one") produces a SECOND, disconnected instance that no
 * gameplay code will ever consult. Adopting the actor's existing subobject is
 * the only answer that leaves ASC->GetSet<T>() pointing at the real one.
 *
 * Nested subobjects are included, unlike the engine's own scan, so a set
 * created as a subobject of the ASC rather than of the actor is found too.
 * Idempotent. Returns how many were newly registered.
 */
int32 AdoptOwnerAttributeSets(
	UAbilitySystemComponent* ASC,
	AActor* Actor,
	TArray<FString>& OutAdoptedClassNames);

/**
 * The FGameplayAttributeData property named on one attribute set class.
 * Accepts a bare property name ("Mana") and the qualified spellings
 * ("MySet.Mana" / "MySet:Mana") the other GAS actions take. Null on a miss.
 */
FStructProperty* FindAttributeDataProperty(UClass* SetClass, const FString& Name);

/** Every FGameplayAttributeData property name on a set class, comma separated. */
FString ListAttributeDataPropertyNames(UClass* SetClass);

}

class FGasHandlers
{
public:
	static void RegisterHandlers(class FMCPHandlerRegistry& Registry);

private:
	// Shared flow used by every GAS "create blueprint by parent class" handler.
	// Requires `name`, reads `packagePath` / `onConflict`, runs the existence
	// check + asset-tools create + compile + save + rollback record, and
	// invokes ExtraResultFields (if provided) to stamp handler-specific fields
	// onto the result before returning.
	static TSharedPtr<FJsonValue> CreateGasBlueprint(
		const TSharedPtr<FJsonObject>& Params,
		const FString& DefaultPackagePath,
		class UClass* ParentClass,
		const FString& FriendlyType,
		TFunction<void(TSharedPtr<FJsonObject>&)> ExtraResultFields = nullptr);

	static TSharedPtr<FJsonValue> CreateGameplayEffect(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> GetGasInfo(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> CreateGameplayAbility(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> CreateAttributeSet(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> CreateGameplayCue(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> AddAbilitySystemComponent(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> AddAttribute(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> SetAbilityTags(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> SetEffectModifier(const TSharedPtr<FJsonObject>& Params);

	// Wire an AttributeSet (with optional init DataTable) onto a Blueprint's ASC
	// component template via DefaultStartingData. Authoring; in GasHandlers.cpp.
	static TSharedPtr<FJsonValue> SetAscDefaults(const TSharedPtr<FJsonObject>& Params);

	// Runtime GAS control (operates on a live actor's AbilitySystemComponent,
	// PIE by default). Implemented in GasHandlers_Runtime.cpp.
	static TSharedPtr<FJsonValue> ApplyEffect(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> SetAttribute(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> GetAttribute(const TSharedPtr<FJsonObject>& Params);
	// InitAbilityActorInfo + optionally GetOrCreateAttributeSubobject on a live
	// actor, so a bridge-authored GAS actor has live attributes to test against.
	static TSharedPtr<FJsonValue> InitAsc(const TSharedPtr<FJsonObject>& Params);
	// #587: introspect a live ASC - granted ability specs (class, level, input,
	// active, dynamic tags) + owned gameplay tags.
	static TSharedPtr<FJsonValue> GetAscState(const TSharedPtr<FJsonObject>& Params);

	// #956: read and write the CURRENT value of one FGameplayAttributeData on
	// the attribute set instance actually REGISTERED on a live actor's ASC.
	// get_attribute / set_attribute above only see sets the ASC already knows
	// about, and an actor spawned into the editor world has none, because the
	// DSO scan that registers them runs in InitializeComponent and a world that
	// has not begun play never gets there. These two name the set explicitly
	// and reach the registered instance, adopting the actor's own subobject
	// when the ASC has not registered it yet. Implemented in
	// GasHandlers_Runtime.cpp.
	static TSharedPtr<FJsonValue> GetLiveAttributeValue(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonValue> SetLiveAttributeValue(const TSharedPtr<FJsonObject>& Params);
};
