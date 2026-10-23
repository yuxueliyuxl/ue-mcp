// What a Level Sequence's bindings resolve to: in the editor world, after a
// retarget (#1356), and per runtime player in a PIE or game world (#1232).

#include "SequencerHandlers.h"
#include "SequencerHandlers_Internal.h"
#include "HandlerSequencerTracks.h"
#include "HandlerUtils.h"

#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "DefaultLevelSequenceInstanceData.h"
#include "MovieScene.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieSceneSequencePlayer.h"
#include "UniversalObjectLocatorResolveParams.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "Engine/Level.h"
#include "EngineUtils.h"
#include "Editor.h"

namespace UEMCP::SequencerBindings
{
	using namespace UEMCP::SequencerTracks;

	TSharedPtr<FJsonObject> DescribeBoundObject(UObject* Object)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Object->GetName());
		Obj->SetStringField(TEXT("path"), Object->GetPathName());
		Obj->SetStringField(TEXT("class"), Object->GetClass()->GetName());
		FTransform World;
		bool bHasTransform = false;
		if (AActor* Actor = Cast<AActor>(Object))
		{
			Obj->SetStringField(TEXT("actorLabel"), Actor->GetActorLabel());
			World = Actor->GetActorTransform();
			bHasTransform = true;
		}
		else if (USceneComponent* Comp = Cast<USceneComponent>(Object))
		{
			if (AActor* Owner = Comp->GetOwner()) Obj->SetStringField(TEXT("ownerActorLabel"), Owner->GetActorLabel());
			World = Comp->GetComponentTransform();
			bHasTransform = true;
		}
		if (bHasTransform)
		{
			Obj->SetObjectField(TEXT("location"), MCPVec3ToJsonObject(World.GetLocation()));
			Obj->SetObjectField(TEXT("rotation"), MCPRotatorToJsonObject(World.Rotator()));
			Obj->SetObjectField(TEXT("scale"), MCPVec3ToJsonObject(World.GetScale3D()));
		}
		return Obj;
	}

	/** Objects one binding resolves to in a world. A child possessable resolves against its parent's object. */
	TArray<UObject*> LocateInWorld(ULevelSequence* Sequence, UMovieScene* MovieScene, const FGuid& Guid, UWorld* World, int32 Depth = 0)
	{
		TArray<UObject*> Result;
		if (Depth > 16) return Result;
		UObject* Context = World;
		if (const FMovieScenePossessable* P = MovieScene->FindPossessable(Guid))
		{
			if (P->GetParent().IsValid())
			{
				TArray<UObject*> Parents = LocateInWorld(Sequence, MovieScene, P->GetParent(), World, Depth + 1);
				if (Parents.Num() == 0) return Result;
				Context = Parents[0];
			}
		}
		TArray<UObject*, TInlineAllocator<1>> Found;
		LocateBound(Sequence, Guid, Context, Found);
		for (UObject* O : Found)
		{
			// A deleted actor stays resolvable while the undo buffer holds it, so it
			// counts only while its level still lists it (#1356).
			if (!IsValid(O)) continue;
			if (const AActor* Actor = Cast<AActor>(O))
			{
				const ULevel* Level = Actor->GetLevel();
				if (Actor->IsActorBeingDestroyed() || !Level || !Level->Actors.Contains(Actor)) continue;
			}
			Result.Add(O);
		}
		return Result;
	}

	FString BindingName(UMovieScene* MovieScene, const FGuid& Guid)
	{
		if (const FMovieScenePossessable* P = MovieScene->FindPossessable(Guid)) return P->GetName();
		if (const FMovieSceneSpawnable* S = MovieScene->FindSpawnable(Guid)) return S->GetName();
		return FString();
	}

	void CollectBindingGuids(UMovieScene* MovieScene, TArray<FGuid>& Out, TMap<FGuid, FString>& Types)
	{
		for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
		{
			const FGuid G = MovieScene->GetPossessable(i).GetGuid();
			Out.Add(G);
			Types.Add(G, TEXT("possessable"));
		}
		for (int32 i = 0; i < MovieScene->GetSpawnableCount(); ++i)
		{
			const FGuid G = MovieScene->GetSpawnable(i).GetGuid();
			Out.Add(G);
			Types.Add(G, TEXT("spawnable"));
		}
	}

	TArray<TSharedPtr<FJsonValue>> PathsOf(const TArray<UObject*>& Objects)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (UObject* O : Objects) Out.Add(MakeShared<FJsonValueString>(O->GetPathName()));
		return Out;
	}

	/** The component a child possessable should follow on the new actor: mapped name, then binding name, then a unique class match. */
	UActorComponent* FindChildComponent(AActor* Actor, const FMovieScenePossessable& Child, const FString& MappedName)
	{
		const FString Wanted = MappedName.IsEmpty() ? Child.GetName() : MappedName;
		if (UActorComponent* ByName = MCPFindComponentByName<UActorComponent>(Actor, Wanted)) return ByName;
		if (!MappedName.IsEmpty()) return nullptr;
#if WITH_EDITORONLY_DATA
		const UClass* ChildClass = Child.GetPossessedObjectClass();
		if (ChildClass)
		{
			UActorComponent* Only = nullptr;
			for (UActorComponent* Comp : Actor->GetComponents())
			{
				if (Comp && Comp->IsA(ChildClass))
				{
					if (Only) return nullptr;
					Only = Comp;
				}
			}
			return Only;
		}
#endif
		return nullptr;
	}
}

// resolve_sequence_bindings (#1356): what every binding resolves to in the
// editor world right now. A possessable that resolves to nothing is stale.
TSharedPtr<FJsonValue> FSequencerHandlers::ResolveBindings(const TSharedPtr<FJsonObject>& Params)
{
	using namespace UEMCP::SequencerTracks;
	using namespace UEMCP::SequencerBindings;

	FString Path;
	const bool bHasPath = TryGetStringParam(Params, TEXT("sequencePath"), Path);
	const FString GuidText = OptionalString(Params, TEXT("bindingGuid"));
	const FString NameText = OptionalString(Params, TEXT("bindingName"));

	if (auto PieErr = MCPRefuseDuringPlayInEditor(TEXT("resolve_sequence_bindings"))) return PieErr;

	TSharedPtr<FJsonValue> LoadErr;
	ULevelSequence* Sequence = LoadSequence(Path, bHasPath, LoadErr);
	if (!Sequence) return LoadErr;
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene) return MCPError(TEXT("Sequence has no MovieScene"));
	REQUIRE_EDITOR_WORLD(World);

	TArray<FGuid> Guids;
	TMap<FGuid, FString> Types;
	CollectBindingGuids(MovieScene, Guids, Types);
	if (!GuidText.IsEmpty() || !NameText.IsEmpty())
	{
		FGuid Only;
		FString Err;
		if (!FindBindingByNameOrGuid(MovieScene, GuidText, NameText, Only, Err)) return MCPError(Err);
		Guids = { Only };
	}

	int32 Stale = 0;
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FGuid& Guid : Guids)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), BindingName(MovieScene, Guid));
		Obj->SetStringField(TEXT("guid"), Guid.ToString());
		const FString Type = Types.FindRef(Guid);
		Obj->SetStringField(TEXT("type"), Type);
		if (const FMovieScenePossessable* P = MovieScene->FindPossessable(Guid))
		{
			if (P->GetParent().IsValid()) Obj->SetStringField(TEXT("parentGuid"), P->GetParent().ToString());
#if WITH_EDITORONLY_DATA
			if (const UClass* Cls = P->GetPossessedObjectClass()) Obj->SetStringField(TEXT("possessedClass"), Cls->GetName());
#endif
		}
		TArray<TSharedPtr<FJsonValue>> Bound;
		const TArray<UObject*> Objects = Type == TEXT("possessable") ? LocateInWorld(Sequence, MovieScene, Guid, World) : TArray<UObject*>();
		for (UObject* O : Objects) Bound.Add(MakeShared<FJsonValueObject>(DescribeBoundObject(O)));
		Obj->SetArrayField(TEXT("boundObjects"), Bound);
		const bool bStale = Type == TEXT("possessable") && Objects.Num() == 0;
		Obj->SetBoolField(TEXT("resolved"), Objects.Num() > 0);
		Obj->SetBoolField(TEXT("stale"), bStale);
		if (Type == TEXT("spawnable"))
		{
			Obj->SetStringField(TEXT("note"), TEXT("Spawnables have no object until the sequence plays; get_sequence_runtime_bindings reports what a player spawned."));
		}
		Stale += bStale ? 1 : 0;
		Out.Add(MakeShared<FJsonValueObject>(Obj));
	}

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("sequencePath"), Sequence->GetPathName());
	Result->SetStringField(TEXT("world"), World->GetPathName());
	Result->SetArrayField(TEXT("bindings"), Out);
	Result->SetNumberField(TEXT("bindingCount"), Out.Num());
	Result->SetNumberField(TEXT("staleCount"), Stale);
	if (Stale > 0)
	{
		Result->SetStringField(TEXT("hint"), TEXT("A stale possessable points at an actor this world no longer holds. editor(rebind_possessable) retargets it to a current actor and keeps its tracks."));
	}
	return MCPResult(Result);
}

// rebind_possessable (#1356): point an existing possessable, and its child
// component possessables, at a current actor. The binding guid is kept, so its
// tracks and every reference to it (camera cuts, attach sections) stay valid.
TSharedPtr<FJsonValue> FSequencerHandlers::RebindPossessable(const TSharedPtr<FJsonObject>& Params)
{
	using namespace UEMCP::SequencerTracks;
	using namespace UEMCP::SequencerBindings;

	FString Path;
	const bool bHasPath = TryGetStringParam(Params, TEXT("sequencePath"), Path);
	const FString GuidText = OptionalString(Params, TEXT("bindingGuid"));
	const FString NameText = OptionalString(Params, TEXT("bindingName"));
	const FString ActorLabel = OptionalString(Params, TEXT("actorLabel"));
	const FString ActorPath = OptionalString(Params, TEXT("actorPath"));
	const bool bChildren = OptionalBool(Params, TEXT("includeChildren"), true);
	const bool bRename = OptionalBool(Params, TEXT("renameBinding"));
	const TSharedPtr<FJsonObject>* ComponentMap = nullptr;
	TryGetObjectParam(Params, TEXT("componentMap"), ComponentMap);

	if (auto PieErr = MCPRefuseDuringPlayInEditor(TEXT("rebind_possessable"))) return PieErr;

	TSharedPtr<FJsonValue> LoadErr;
	ULevelSequence* Sequence = LoadSequence(Path, bHasPath, LoadErr);
	if (!Sequence) return LoadErr;
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene) return MCPError(TEXT("Sequence has no MovieScene"));
	REQUIRE_EDITOR_WORLD(World);

	if (GuidText.IsEmpty() && NameText.IsEmpty())
	{
		return MCPError(TEXT("Pass bindingGuid or bindingName to choose the possessable to retarget"));
	}
	FGuid Guid;
	FString Err;
	if (!FindBindingByNameOrGuid(MovieScene, GuidText, NameText, Guid, Err)) return MCPError(Err);
	FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Guid);
	if (!Possessable)
	{
		return MCPError(FString::Printf(TEXT("Binding %s is a spawnable; only possessables bind to level actors"), *Guid.ToString()));
	}
	if (Possessable->GetParent().IsValid())
	{
		return MCPError(TEXT("That binding is a child (component) possessable. Rebind its parent actor binding; children follow it."));
	}

	TSharedPtr<FJsonValue> ActorErr;
	AActor* Actor = MCPResolveActor(World, Params, ActorErr);
	if (!Actor) return ActorErr;
	if (!Sequence->CanPossessObject(*Actor, World))
	{
		return MCPError(FString::Printf(TEXT("The sequence cannot possess '%s'"), *Actor->GetActorLabel()));
	}

	// Before: what the binding pointed at, for the report and the rollback.
	const TArray<UObject*> BeforeObjects = LocateInWorld(Sequence, MovieScene, Guid, World);
	const FString PreviousName = Possessable->GetName();

	Sequence->Modify();
	MovieScene->Modify();

	Sequence->UnbindPossessableObjects(Guid);
	Sequence->BindPossessableObject(Guid, *Actor, World);
#if WITH_EDITORONLY_DATA
	Possessable->SetPossessedObjectClass(Actor->GetClass());
#endif
	if (bRename)
	{
		const FString NewName = Actor->GetActorLabel();
		Possessable->SetName(NewName);
#if !UE_MCP_HAS_5_7_API
		if (FMovieSceneBinding* Binding = MovieScene->FindBinding(Guid)) Binding->SetName(NewName);
#endif
#if WITH_EDITORONLY_DATA
		MovieScene->SetObjectDisplayName(Guid, FText::FromString(NewName));
#endif
	}

	// Children: each component possessable follows its component on the new actor.
	TArray<TSharedPtr<FJsonValue>> ChildrenJson;
	int32 ChildFailures = 0;
	if (bChildren)
	{
		for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
		{
			FMovieScenePossessable& Child = MovieScene->GetPossessable(i);
			if (Child.GetParent() != Guid) continue;
			const FGuid ChildGuid = Child.GetGuid();
			FString Mapped;
			if (ComponentMap && ComponentMap->IsValid())
			{
				if (!(*ComponentMap)->TryGetStringField(Child.GetName(), Mapped)) (*ComponentMap)->TryGetStringField(ChildGuid.ToString(), Mapped);
			}
			TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
			C->SetStringField(TEXT("name"), Child.GetName());
			C->SetStringField(TEXT("guid"), ChildGuid.ToString());
			UActorComponent* Comp = FindChildComponent(Actor, Child, Mapped);
			if (!Comp)
			{
				C->SetBoolField(TEXT("rebound"), false);
				C->SetStringField(TEXT("error"), FString::Printf(TEXT("No component on '%s' matches '%s'. Name it in componentMap {\"%s\": \"<component name>\"}."),
					*Actor->GetActorLabel(), *(Mapped.IsEmpty() ? Child.GetName() : Mapped), *Child.GetName()));
				++ChildFailures;
				ChildrenJson.Add(MakeShared<FJsonValueObject>(C));
				continue;
			}
			Sequence->UnbindPossessableObjects(ChildGuid);
			Sequence->BindPossessableObject(ChildGuid, *Comp, Actor);
#if WITH_EDITORONLY_DATA
			Child.SetPossessedObjectClass(Comp->GetClass());
#endif
			const TArray<UObject*> Located = LocateInWorld(Sequence, MovieScene, ChildGuid, World);
			const bool bVerified = Located.Contains(Comp);
			C->SetStringField(TEXT("component"), Comp->GetName());
			C->SetBoolField(TEXT("rebound"), true);
			C->SetBoolField(TEXT("verified"), bVerified);
			C->SetArrayField(TEXT("resolvesTo"), PathsOf(Located));
			if (!bVerified) ++ChildFailures;
			ChildrenJson.Add(MakeShared<FJsonValueObject>(C));
		}
	}

	Sequence->GetOutermost()->MarkPackageDirty();

	// Verification: the binding must now resolve to exactly the requested actor.
	const TArray<UObject*> AfterObjects = LocateInWorld(Sequence, MovieScene, Guid, World);
	const bool bVerified = AfterObjects.Num() == 1 && AfterObjects[0] == Actor;
	int32 TrackCount = 0;
	if (const FMovieSceneBinding* Binding = MovieScene->FindBinding(Guid)) TrackCount = Binding->GetTracks().Num();

	if (!bVerified)
	{
		TSharedPtr<FJsonObject> ErrObj = MCPErrorObject(FString::Printf(
			TEXT("Rebound binding %s to '%s', but it resolves to %d object(s) instead of that actor"),
			*Guid.ToString(), *Actor->GetActorLabel(), AfterObjects.Num()));
		ErrObj->SetArrayField(TEXT("resolvesTo"), PathsOf(AfterObjects));
		return MCPResult(ErrObj);
	}

	auto Result = MCPSuccess();
	MCPSetUpdated(Result);
	Result->SetStringField(TEXT("sequencePath"), Sequence->GetPathName());
	Result->SetStringField(TEXT("bindingGuid"), Guid.ToString());
	Result->SetStringField(TEXT("bindingName"), Possessable->GetName());
	Result->SetStringField(TEXT("previousName"), PreviousName);
	Result->SetArrayField(TEXT("previouslyResolvedTo"), PathsOf(BeforeObjects));
	Result->SetBoolField(TEXT("wasStale"), BeforeObjects.Num() == 0);
	Result->SetStringField(TEXT("actorPath"), Actor->GetPathName());
	Result->SetStringField(TEXT("actorLabel"), Actor->GetActorLabel());
	Result->SetArrayField(TEXT("resolvesTo"), PathsOf(AfterObjects));
	Result->SetBoolField(TEXT("verified"), true);
	Result->SetNumberField(TEXT("trackCount"), TrackCount);
	Result->SetArrayField(TEXT("children"), ChildrenJson);
	Result->SetBoolField(TEXT("childrenVerified"), ChildFailures == 0);
	Result->SetBoolField(TEXT("unchanged"), BeforeObjects.Num() == 1 && BeforeObjects[0] == Actor && !bRename);
	if (ChildFailures > 0)
	{
		Result->SetStringField(TEXT("warning"), FString::Printf(TEXT("%d child possessable(s) were not rebound or did not verify; see children"), ChildFailures));
	}
	Result->SetStringField(TEXT("saveNote"), TEXT("The sequence is marked dirty, not saved. asset(save) writes it."));

	AActor* PreviousActor = BeforeObjects.Num() == 1 ? Cast<AActor>(BeforeObjects[0]) : nullptr;
	if (PreviousActor && PreviousActor != Actor)
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("sequencePath"), Sequence->GetPathName());
		Payload->SetStringField(TEXT("bindingGuid"), Guid.ToString());
		Payload->SetStringField(TEXT("actorPath"), PreviousActor->GetPathName());
		Payload->SetBoolField(TEXT("includeChildren"), bChildren);
		MCPSetRollback(Result, TEXT("rebind_possessable"), Payload);
		Result->SetBoolField(TEXT("rollbackLossy"), bRename);
		if (bRename) Result->SetStringField(TEXT("rollbackNote"), TEXT("The rollback rebinds the previous actor but keeps the new binding name."));
	}
	else if (!PreviousActor)
	{
		MCPSetNoRollback(Result, TEXT("The binding resolved to no single actor before this call (it was stale), so there is nothing to rebind back to."));
	}
	else
	{
		MCPSetNoRollback(Result, TEXT("The binding already resolved to this actor, so there is no earlier target to restore."));
	}
	return MCPResult(Result);
}

// get_sequence_runtime_bindings (#1232): every LevelSequenceActor in a world,
// its player state and instance-data origin, and the objects each binding of
// that player resolves to. This is what tells an actor spawned by a player
// apart from the duplicates an open Sequencer editor spawns.
TSharedPtr<FJsonValue> FSequencerHandlers::GetRuntimeBindings(const TSharedPtr<FJsonObject>& Params)
{
	using namespace UEMCP::SequencerTracks;
	using namespace UEMCP::SequencerBindings;

	const FString Scope = OptionalString(Params, TEXT("world"), TEXT("auto"));
	UWorld* World = ResolveWorldFromParams(Params, TEXT("auto"));
	const FString SequenceFilter = OptionalString(Params, TEXT("sequencePath"));
	const FString ActorFilter = OptionalString(Params, TEXT("actorLabel"));

	if (!World)
	{
		return MCPError(FString::Printf(TEXT("No %s world is available. Start PIE, or pass world 'editor'."), *Scope));
	}

	TArray<TSharedPtr<FJsonValue>> ActorsJson;
	for (TActorIterator<ALevelSequenceActor> It(World); It; ++It)
	{
		ALevelSequenceActor* SeqActor = *It;
		if (!SeqActor) continue;
		if (!ActorFilter.IsEmpty() && !SeqActor->GetActorLabel().Equals(ActorFilter, ESearchCase::IgnoreCase) && !SeqActor->GetName().Equals(ActorFilter, ESearchCase::IgnoreCase)) continue;
		ULevelSequence* Sequence = SeqActor->GetSequence();
		if (!SequenceFilter.IsEmpty())
		{
			if (!Sequence) continue;
			const FString SeqPath = Sequence->GetPathName();
			if (!SeqPath.Equals(SequenceFilter, ESearchCase::IgnoreCase) && !SeqPath.StartsWith(SequenceFilter + TEXT("."), ESearchCase::IgnoreCase)
				&& !Sequence->GetName().Equals(SequenceFilter, ESearchCase::IgnoreCase)) continue;
		}

		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("actorLabel"), SeqActor->GetActorLabel());
		A->SetStringField(TEXT("actorPath"), SeqActor->GetPathName());
		A->SetStringField(TEXT("sequencePath"), Sequence ? Sequence->GetPathName() : FString());
		A->SetBoolField(TEXT("overrideInstanceData"), SeqActor->bOverrideInstanceData != 0);
		if (UDefaultLevelSequenceInstanceData* Instance = Cast<UDefaultLevelSequenceInstanceData>(SeqActor->DefaultInstanceData))
		{
			TSharedPtr<FJsonObject> I = MakeShared<FJsonObject>();
			I->SetObjectField(TEXT("transformOrigin"), MCPTransformToJsonObject(Instance->TransformOrigin));
			I->SetStringField(TEXT("transformOriginActor"), Instance->TransformOriginActor ? Instance->TransformOriginActor->GetPathName() : FString());
			A->SetObjectField(TEXT("instanceData"), I);
		}

		ULevelSequencePlayer* Player = SeqActor->GetSequencePlayer();
		A->SetBoolField(TEXT("hasPlayer"), Player != nullptr);
		if (Player && Sequence)
		{
			A->SetBoolField(TEXT("isPlaying"), Player->IsPlaying());
			A->SetBoolField(TEXT("isPaused"), Player->IsPaused());
			const FQualifiedFrameTime Now = Player->GetCurrentTime();
			TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
			T->SetNumberField(TEXT("frame"), Now.Time.AsDecimal());
			T->SetNumberField(TEXT("seconds"), Now.AsSeconds());
			UEMCP::SequencerInfo::SetFrameRateFields(*T, Now.Rate);
			A->SetObjectField(TEXT("currentTime"), T);

			UMovieScene* MovieScene = Sequence->GetMovieScene();
			TArray<FGuid> Guids;
			TMap<FGuid, FString> Types;
			if (MovieScene) CollectBindingGuids(MovieScene, Guids, Types);
			TArray<TSharedPtr<FJsonValue>> BindingsJson;
			for (const FGuid& Guid : Guids)
			{
				TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
				B->SetStringField(TEXT("name"), BindingName(MovieScene, Guid));
				B->SetStringField(TEXT("guid"), Guid.ToString());
				B->SetStringField(TEXT("type"), Types.FindRef(Guid));
				if (const FMovieScenePossessable* P = MovieScene->FindPossessable(Guid))
				{
					if (P->GetParent().IsValid()) B->SetStringField(TEXT("parentGuid"), P->GetParent().ToString());
				}
				TArray<TSharedPtr<FJsonValue>> Bound;
				for (UObject* O : Player->GetBoundObjects(FMovieSceneObjectBindingID(UE::MovieScene::FRelativeObjectBindingID(Guid))))
				{
					if (O) Bound.Add(MakeShared<FJsonValueObject>(DescribeBoundObject(O)));
				}
				B->SetArrayField(TEXT("boundObjects"), Bound);
				BindingsJson.Add(MakeShared<FJsonValueObject>(B));
			}
			A->SetArrayField(TEXT("bindings"), BindingsJson);
		}
		ActorsJson.Add(MakeShared<FJsonValueObject>(A));
	}

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("world"), World->GetPathName());
	Result->SetStringField(TEXT("worldType"), World->IsPlayInEditor() ? TEXT("pie") : (World->IsGameWorld() ? TEXT("game") : TEXT("editor")));
	Result->SetArrayField(TEXT("sequenceActors"), ActorsJson);
	Result->SetNumberField(TEXT("sequenceActorCount"), ActorsJson.Num());
	return MCPResult(Result);
}
