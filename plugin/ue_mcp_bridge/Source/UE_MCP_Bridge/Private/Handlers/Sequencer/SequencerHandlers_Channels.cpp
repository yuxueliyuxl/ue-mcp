// Channel-level read and write on any Level Sequence section, addressed by
// track and channel name through the section's channel proxy (#1407), plus
// section removal (#1232).

#include "SequencerHandlers.h"
#include "SequencerHandlers_Internal.h"
#include "HandlerSequencerTracks.h"
#include "HandlerUtils.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "Sections/MovieSceneParameterSection.h"
#include "Tracks/MovieSceneMaterialParameterCollectionTrack.h"
#include "Materials/MaterialParameterCollection.h"
#include "Editor.h"
#include "ScopedTransaction.h"

namespace UEMCP::SequencerChannels
{
	using namespace UEMCP::SequencerTracks;

	/** The section a channel call addresses, read from its parameters before anything can fail. */
	struct FSectionSelector
	{
		FString Path;
		bool bHasPath = false;
		FString TrackType;
		FString TrackName;
		int32 TrackIndex = 0;
		FString BindingGuid;
		FString BindingName;
		FString ActorLabel;
		FString ActorPath;
		int32 SectionIndex = 0;
		bool bHasSectionIndex = false;

		void Read(const TSharedPtr<FJsonObject>& Params)
		{
			bHasPath = TryGetStringParam(Params, TEXT("sequencePath"), Path);
			TrackType = OptionalString(Params, TEXT("trackType"));
			TrackName = OptionalString(Params, TEXT("trackName"));
			TryGetNumberParam(Params, TEXT("trackIndex"), TrackIndex);
			BindingGuid = OptionalString(Params, TEXT("bindingGuid"));
			BindingName = OptionalString(Params, TEXT("bindingName"));
			ActorLabel = OptionalString(Params, TEXT("actorLabel"));
			ActorPath = OptionalString(Params, TEXT("actorPath"));
			bHasSectionIndex = TryGetNumberParam(Params, TEXT("sectionIndex"), SectionIndex);
		}
	};

	struct FResolvedSection
	{
		ULevelSequence* Sequence = nullptr;
		UMovieScene* MovieScene = nullptr;
		UMovieSceneTrack* Track = nullptr;
		UMovieSceneSection* Section = nullptr;
		FGuid Binding;
	};

	/** Resolve sequence, binding scope and track. Never creates a binding or a track. */
	TSharedPtr<FJsonValue> ResolveTrackOnly(const TSharedPtr<FJsonObject>& Params, const FSectionSelector& Sel, FResolvedSection& Out)
	{
		TSharedPtr<FJsonValue> LoadErr;
		Out.Sequence = LoadSequence(Sel.Path, Sel.bHasPath, LoadErr);
		if (!Out.Sequence) return LoadErr;
		Out.MovieScene = Out.Sequence->GetMovieScene();
		if (!Out.MovieScene) return MCPError(TEXT("Sequence has no MovieScene"));

		if (Sel.TrackType.IsEmpty() && Sel.TrackName.IsEmpty())
		{
			return MCPError(FString::Printf(TEXT("Pass trackType (%s) or trackName to choose the track"), *TrackTypeHelp()));
		}

		FTrackQuery Query;
		Query.TrackType = Sel.TrackType;
		Query.TrackName = Sel.TrackName;
		Query.TrackIndex = Sel.TrackIndex;

		FString Err;
		if (!Sel.BindingGuid.IsEmpty() || !Sel.BindingName.IsEmpty())
		{
			if (!FindBindingByNameOrGuid(Out.MovieScene, Sel.BindingGuid, Sel.BindingName, Query.Binding, Err)) return MCPError(Err);
			Query.bHasBinding = true;
		}
		else if (!Sel.ActorLabel.IsEmpty() || !Sel.ActorPath.IsEmpty())
		{
			REQUIRE_EDITOR_WORLD(World);
			TSharedPtr<FJsonValue> ActorErr;
			AActor* Actor = MCPResolveActor(World, Params, ActorErr);
			if (!Actor) return ActorErr;
			if (!FindActorBinding(Out.Sequence, Out.MovieScene, Actor, World, Query.Binding))
			{
				return MCPError(FString::Printf(TEXT("Actor '%s' has no binding in this sequence. Bindings: %s"),
					*Actor->GetActorLabel(), *ListBindings(Out.MovieScene)));
			}
			Query.bHasBinding = true;
		}

		Out.Track = FindTrack(Out.MovieScene, Query, Err);
		if (!Out.Track) return MCPError(Err);
		Out.Binding = Query.Binding;
		return nullptr;
	}

	TSharedPtr<FJsonValue> ResolveSection(const TSharedPtr<FJsonObject>& Params, const FSectionSelector& Sel, FResolvedSection& Out)
	{
		if (TSharedPtr<FJsonValue> Err = ResolveTrackOnly(Params, Sel, Out)) return Err;
		const TArray<UMovieSceneSection*>& Sections = Out.Track->GetAllSections();
		if (Sections.Num() == 0)
		{
			return MCPError(TEXT("The track has no sections. editor(add_sequence_section) adds one."));
		}
		if (Sel.SectionIndex < 0 || Sel.SectionIndex >= Sections.Num() || !Sections[Sel.SectionIndex])
		{
			return MCPError(FString::Printf(TEXT("sectionIndex %d out of range (sections=%d)"), Sel.SectionIndex, Sections.Num()));
		}
		Out.Section = Sections[Sel.SectionIndex];
		return nullptr;
	}

	void SetSelectorFields(const TSharedPtr<FJsonObject>& Result, const FResolvedSection& R, int32 SectionIndex)
	{
		Result->SetStringField(TEXT("sequencePath"), R.Sequence->GetPathName());
		Result->SetObjectField(TEXT("track"), DescribeTrack(R.Track));
		if (R.Binding.IsValid()) Result->SetStringField(TEXT("bindingGuid"), R.Binding.ToString());
		Result->SetNumberField(TEXT("sectionIndex"), SectionIndex);
		Result->SetStringField(TEXT("sectionClass"), R.Section ? R.Section->GetClass()->GetName() : FString());
	}

	/** One time field of a keyframe entry as a tick: tick, frame (display) or seconds. */
	bool ReadKeyTime(const TSharedPtr<FJsonObject>& Kf, const FFrameRate& Tick, const FFrameRate& Display, FFrameNumber& Out)
	{
		double V = 0.0;
		if (Kf->TryGetNumberField(TEXT("tick"), V))
		{
			Out = FFrameNumber(static_cast<int32>(FMath::RoundToDouble(V)));
			return true;
		}
		if (Kf->TryGetNumberField(TEXT("frame"), V))
		{
			Out = FFrameRate::TransformTime(FFrameTime::FromDecimal(V), Display, Tick).RoundToFrame();
			return true;
		}
		if (Kf->TryGetNumberField(TEXT("seconds"), V))
		{
			Out = Tick.AsFrameTime(V).RoundToFrame();
			return true;
		}
		return false;
	}

	TSharedPtr<FJsonObject> KeyObject(const FKeyRead& Key, EChannelKind Kind, const FFrameRate& Tick, const FFrameRate& Display)
	{
		TSharedPtr<FJsonObject> Obj = UEMCP::SequencerInfo::MakeKeyTimeObject(Key.Time, Tick, Display);
		Obj->SetField(TEXT("value"), KindValue(Kind, Key.Value));
		Obj->SetStringField(TEXT("interpolation"), Key.Interp);
		return Obj;
	}

	/**
	 * Add a missing parameter to a parameter section so its curves exist, then
	 * empty them so only the caller's keys land. The type comes from
	 * parameterType, else from the track's Material Parameter Collection.
	 */
	TSharedPtr<FJsonValue> CreateParameter(
		UMovieSceneParameterSection* Section, UMovieSceneTrack* Track, const FString& Channel, const FString& ParameterType, FString& OutCreatedType)
	{
		FString Base = Channel;
		FString Component;
		Channel.Split(TEXT("."), &Base, &Component, ESearchCase::IgnoreCase, ESearchDir::FromStart);
		if (Base.IsEmpty()) Base = Channel;
		const FName ParamName(*Base);

		FString Type = ParameterType.ToLower();
		if (Type.IsEmpty())
		{
			if (UMovieSceneMaterialParameterCollectionTrack* MPCTrack = Cast<UMovieSceneMaterialParameterCollectionTrack>(Track))
			{
				UMaterialParameterCollection* MPC = MPCTrack->MPC;
				if (!MPC) return MCPError(TEXT("The Material Parameter Collection track has no collection assigned"));
				if (MPC->GetScalarParameterByName(ParamName)) Type = TEXT("scalar");
				else if (MPC->GetVectorParameterByName(ParamName)) Type = TEXT("color");
				else
				{
					TArray<FString> Names;
					for (const FCollectionScalarParameter& P : MPC->ScalarParameters) Names.Add(P.ParameterName.ToString() + TEXT(" (scalar)"));
					for (const FCollectionVectorParameter& P : MPC->VectorParameters) Names.Add(P.ParameterName.ToString() + TEXT(" (vector: .R/.G/.B/.A)"));
					return MCPError(FString::Printf(TEXT("'%s' is not a parameter of %s. Its parameters: %s"),
						*Base, *MPC->GetPathName(), Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("none")));
				}
			}
			else
			{
				return MCPError(FString::Printf(TEXT("Channel '%s' does not exist on this parameter section. Pass parameterType (scalar | bool | vector | vector2d | color) to add it."), *Channel));
			}
		}

		const FFrameNumber Zero(0);
		if (Type == TEXT("scalar") || Type == TEXT("float"))
		{
			Section->AddScalarParameterKey(ParamName, Zero, 0.f);
		}
		else if (Type == TEXT("bool"))
		{
			Section->AddBoolParameterKey(ParamName, Zero, false);
		}
		else if (Type == TEXT("vector"))
		{
			Section->AddVectorParameterKey(ParamName, Zero, FVector::ZeroVector);
		}
		else if (Type == TEXT("vector2d"))
		{
			Section->AddVector2DParameterKey(ParamName, Zero, FVector2D::ZeroVector);
		}
		else if (Type == TEXT("color"))
		{
			Section->AddColorParameterKey(ParamName, Zero, FLinearColor::Black);
		}
		else
		{
			return MCPError(FString::Printf(TEXT("Unknown parameterType '%s'. Use scalar, bool, vector, vector2d or color."), *ParameterType));
		}
		OutCreatedType = Type;

		// The add above keyed every component at frame 0; the caller's keys are the only ones wanted.
		TArray<FChannelRef> Fresh;
		GatherChannels(Section, Fresh);
		for (const FChannelRef& Ref : Fresh)
		{
			if (Ref.Name.Equals(Base, ESearchCase::IgnoreCase) || Ref.Name.StartsWith(Base + TEXT("."), ESearchCase::IgnoreCase)) ClearKeys(Ref);
		}
		return nullptr;
	}
}

// get_sequence_channel_keys (#1407): every key of one channel, or of every
// channel on the section, read through the channel proxy. Parameter sections
// (Material Parameter Collection tracks) are covered by parameter name.
TSharedPtr<FJsonValue> FSequencerHandlers::GetChannelKeys(const TSharedPtr<FJsonObject>& Params)
{
	using namespace UEMCP::SequencerTracks;
	using namespace UEMCP::SequencerChannels;
	FSectionSelector Sel;
	Sel.Read(Params);
	const FString ChannelName = OptionalString(Params, TEXT("channel"));
	double SampleStart = 0.0, SampleEnd = 0.0, SampleStep = 1.0;
	const bool bHasSampleStart = TryGetNumberParam(Params, TEXT("sampleStartFrame"), SampleStart);
	const bool bHasSampleEnd = TryGetNumberParam(Params, TEXT("sampleEndFrame"), SampleEnd);
	TryGetNumberParam(Params, TEXT("sampleStep"), SampleStep);

	FResolvedSection R;
	if (TSharedPtr<FJsonValue> Err = ResolveSection(Params, Sel, R)) return Err;

	const bool bSample = bHasSampleStart || bHasSampleEnd;
	if (bSample && !(bHasSampleStart && bHasSampleEnd))
	{
		return MCPError(TEXT("Pass both sampleStartFrame and sampleEndFrame to sample a channel"));
	}
	if (bSample && (SampleStep <= 0.0 || SampleEnd < SampleStart))
	{
		return MCPError(TEXT("sampleStep must be positive and sampleEndFrame must not precede sampleStartFrame"));
	}
	constexpr int32 MaxSamples = 10000;
	const int32 SampleCount = bSample ? FMath::FloorToInt((SampleEnd - SampleStart) / SampleStep) + 1 : 0;
	if (SampleCount > MaxSamples)
	{
		return MCPError(FString::Printf(TEXT("That range is %d samples; the limit is %d. Raise sampleStep or narrow the range."), SampleCount, MaxSamples));
	}

	TArray<FChannelRef> Channels;
	GatherChannels(R.Section, Channels);
	TArray<FChannelRef*> Selected;
	if (!ChannelName.IsEmpty())
	{
		FChannelRef* Found = FindChannel(Channels, ChannelName);
		if (!Found)
		{
			return MCPError(FString::Printf(TEXT("Channel '%s' not found on the section. Channels: %s"), *ChannelName, *ListChannels(Channels)));
		}
		Selected.Add(Found);
	}
	else
	{
		for (FChannelRef& Ref : Channels) Selected.Add(&Ref);
	}

	const FFrameRate Tick = R.MovieScene->GetTickResolution();
	const FFrameRate Display = R.MovieScene->GetDisplayRate();
	constexpr int32 MaxKeysPerChannel = 4096;

	TArray<TSharedPtr<FJsonValue>> ChannelsJson;
	for (FChannelRef* Ref : Selected)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Ref->Name);
		if (Ref->Aliases.Num() > 0) Obj->SetArrayField(TEXT("aliases"), MCPStringListToJson(Ref->Aliases));
		Obj->SetStringField(TEXT("type"), Ref->TypeName);
		Obj->SetStringField(TEXT("valueKind"), KindName(Ref->Kind));
		if (Ref->Kind == EChannelKind::Unsupported)
		{
			Obj->SetNumberField(TEXT("keyCount"), Ref->Channel->GetNumKeys());
			Obj->SetStringField(TEXT("note"), TEXT("Values of this channel type are not readable here; key times are in get_sequence_info."));
			ChannelsJson.Add(MakeShared<FJsonValueObject>(Obj));
			continue;
		}

		double DefaultValue = 0.0;
		const bool bHasDefault = ReadDefault(*Ref, DefaultValue);
		Obj->SetBoolField(TEXT("hasDefault"), bHasDefault);
		if (bHasDefault) Obj->SetField(TEXT("default"), KindValue(Ref->Kind, DefaultValue));

		TArray<FKeyRead> Keys;
		ReadKeys(*Ref, Keys);
		TArray<TSharedPtr<FJsonValue>> KeysJson;
		for (int32 i = 0; i < Keys.Num() && i < MaxKeysPerChannel; ++i)
		{
			KeysJson.Add(MakeShared<FJsonValueObject>(KeyObject(Keys[i], Ref->Kind, Tick, Display)));
		}
		Obj->SetNumberField(TEXT("keyCount"), Keys.Num());
		Obj->SetArrayField(TEXT("keys"), KeysJson);
		Obj->SetBoolField(TEXT("keysTruncated"), Keys.Num() > MaxKeysPerChannel);

		if (bSample)
		{
			TArray<TSharedPtr<FJsonValue>> Samples;
			for (int32 i = 0; i < SampleCount; ++i)
			{
				const double DisplayFrame = SampleStart + i * SampleStep;
				const FFrameTime TickTime = FFrameRate::TransformTime(FFrameTime::FromDecimal(DisplayFrame), Display, Tick);
				double Value = 0.0;
				TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
				S->SetNumberField(TEXT("displayFrame"), DisplayFrame);
				S->SetNumberField(TEXT("seconds"), Tick.AsSeconds(TickTime));
				if (EvaluateChannel(*Ref, TickTime, Value)) S->SetField(TEXT("value"), KindValue(Ref->Kind, Value));
				else S->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
				Samples.Add(MakeShared<FJsonValueObject>(S));
			}
			Obj->SetArrayField(TEXT("samples"), Samples);
		}
		ChannelsJson.Add(MakeShared<FJsonValueObject>(Obj));
	}

	auto Result = MCPSuccess();
	SetSelectorFields(Result, R, Sel.SectionIndex);
	TSharedPtr<FJsonObject> SectionRange = MakeShared<FJsonObject>();
	UEMCP::SequencerInfo::SetTimingRangeFields(*SectionRange, R.Section->GetRange(), Tick, Display);
	Result->SetObjectField(TEXT("sectionRange"), SectionRange);
	Result->SetArrayField(TEXT("channels"), ChannelsJson);
	Result->SetNumberField(TEXT("channelCount"), ChannelsJson.Num());
	if (bSample)
	{
		Result->SetStringField(TEXT("sampleNote"), TEXT("samples evaluate the channel curve itself, ignoring the section's range, easing and blending"));
	}
	return MCPResult(Result);
}

// set_sequence_channel_keys (#1407): write keys and the default of one named
// channel on any section. A Material Parameter Collection parameter that the
// section does not animate yet is added first.
TSharedPtr<FJsonValue> FSequencerHandlers::SetChannelKeys(const TSharedPtr<FJsonObject>& Params)
{
	using namespace UEMCP::SequencerTracks;
	using namespace UEMCP::SequencerChannels;
	FSectionSelector Sel;
	Sel.Read(Params);
	FString ChannelName;
	TSharedPtr<FJsonValue> ChannelErr = RequireString(Params, TEXT("channel"), ChannelName);
	const TArray<TSharedPtr<FJsonValue>>* Keyframes = nullptr;
	const bool bHasKeyframes = TryGetArrayParam(Params, TEXT("keyframes"), Keyframes) && Keyframes;
	const FString InterpText = OptionalString(Params, TEXT("interpolation"), TEXT("cubic"));
	const bool bClearExisting = OptionalBool(Params, TEXT("clearExisting"));
	const TSharedPtr<FJsonValue> DefaultParam = TryGetParam(Params, TEXT("defaultValue"));
	const bool bClearDefault = OptionalBool(Params, TEXT("clearDefault"));
	const FString ParameterType = OptionalString(Params, TEXT("parameterType"));

	if (auto PieErr = MCPRefuseDuringPlayInEditor(TEXT("set_sequence_channel_keys"))) return PieErr;

	FResolvedSection R;
	if (TSharedPtr<FJsonValue> Err = ResolveSection(Params, Sel, R)) return Err;
	if (ChannelErr) return ChannelErr;

	ERichCurveInterpMode DefaultInterp = RCIM_Cubic;
	if (!ParseInterp(InterpText, DefaultInterp))
	{
		return MCPError(FString::Printf(TEXT("Unknown interpolation '%s'. Use cubic, linear or constant."), *InterpText));
	}
	double NewDefault = 0.0;
	const bool bSetDefault = DefaultParam.IsValid() && DefaultParam->Type != EJson::Null;
	if (bSetDefault && !ReadKindValue(DefaultParam, NewDefault))
	{
		return MCPError(TEXT("defaultValue must be a number or a boolean"));
	}
	if (bSetDefault && bClearDefault) return MCPError(TEXT("Pass defaultValue or clearDefault, not both"));
	if (!bHasKeyframes && !bSetDefault && !bClearDefault && !bClearExisting)
	{
		return MCPError(TEXT("Nothing to write: pass keyframes, defaultValue, clearDefault or clearExisting"));
	}

	const FFrameRate Tick = R.MovieScene->GetTickResolution();
	const FFrameRate Display = R.MovieScene->GetDisplayRate();

	// Validate every key before touching the section, so a bad entry writes nothing.
	struct FPendingKey { FFrameNumber Time; double Value; ERichCurveInterpMode Interp; };
	TArray<FPendingKey> Pending;
	if (bHasKeyframes)
	{
		for (int32 i = 0; i < Keyframes->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>* Kf = nullptr;
			if (!(*Keyframes)[i].IsValid() || !(*Keyframes)[i]->TryGetObject(Kf) || !Kf)
			{
				return MCPError(FString::Printf(TEXT("keyframes[%d] is not an object"), i));
			}
			FPendingKey Key;
			if (!ReadKeyTime(*Kf, Tick, Display, Key.Time))
			{
				return MCPError(FString::Printf(TEXT("keyframes[%d] needs a time: seconds, frame (display rate) or tick"), i));
			}
			if (!ReadKindValue((*Kf)->TryGetField(TEXT("value")), Key.Value))
			{
				return MCPError(FString::Printf(TEXT("keyframes[%d].value must be a number or a boolean"), i));
			}
			Key.Interp = DefaultInterp;
			FString PerKey;
			if ((*Kf)->TryGetStringField(TEXT("interpolation"), PerKey) && !ParseInterp(PerKey, Key.Interp))
			{
				return MCPError(FString::Printf(TEXT("keyframes[%d].interpolation '%s' is not cubic, linear or constant"), i, *PerKey));
			}
			Pending.Add(Key);
		}
	}

	R.Section->Modify();

	TArray<FChannelRef> Channels;
	GatherChannels(R.Section, Channels);
	FChannelRef* Ref = FindChannel(Channels, ChannelName);
	FString CreatedParameterType;
	if (!Ref)
	{
		UMovieSceneParameterSection* ParamSection = Cast<UMovieSceneParameterSection>(R.Section);
		if (!ParamSection)
		{
			return MCPError(FString::Printf(TEXT("Channel '%s' not found on the section. Channels: %s"), *ChannelName, *ListChannels(Channels)));
		}
		if (TSharedPtr<FJsonValue> CreateErr = CreateParameter(ParamSection, R.Track, ChannelName, ParameterType, CreatedParameterType)) return CreateErr;
		Channels.Reset();
		GatherChannels(R.Section, Channels);
		Ref = FindChannel(Channels, ChannelName);
		if (!Ref)
		{
			return MCPError(FString::Printf(TEXT("Added parameter '%s' as %s, but no channel named '%s' exists on it. Channels: %s"),
				*ChannelName, *CreatedParameterType, *ChannelName, *ListChannels(Channels)));
		}
	}
	if (Ref->Kind == EChannelKind::Unsupported)
	{
		return MCPError(FString::Printf(TEXT("Channel '%s' is a %s, which this action does not write"), *Ref->Name, *Ref->TypeName));
	}

	// The state before the write, in this action's own terms, for the rollback.
	TArray<FKeyRead> Before;
	ReadKeys(*Ref, Before);
	double PreviousDefault = 0.0;
	const bool bHadDefault = ReadDefault(*Ref, PreviousDefault);

	if (bClearExisting) ClearKeys(*Ref);
	for (const FPendingKey& Key : Pending) WriteKey(*Ref, Key.Time, Key.Value, Key.Interp);
	if (bSetDefault) WriteDefault(*Ref, NewDefault);
	if (bClearDefault) Ref->Channel->ClearDefault();

	R.Section->MarkAsChanged();
	R.Sequence->GetOutermost()->MarkPackageDirty();

	// Read back what the channel now holds rather than echoing the request.
	TArray<FKeyRead> After;
	ReadKeys(*Ref, After);
	TArray<TSharedPtr<FJsonValue>> AfterJson;
	for (const FKeyRead& Key : After) AfterJson.Add(MakeShared<FJsonValueObject>(KeyObject(Key, Ref->Kind, Tick, Display)));

	int32 Verified = 0;
	TArray<TSharedPtr<FJsonValue>> Mismatches;
	for (const FPendingKey& Key : Pending)
	{
		const FKeyRead* Found = After.FindByPredicate([&Key](const FKeyRead& K) { return K.Time == Key.Time; });
		const double Tolerance = Ref->Kind == EChannelKind::Float ? 1e-4 : 1e-9;
		if (Found && FMath::IsNearlyEqual(Found->Value, Ref->Kind == EChannelKind::Float ? (double)(float)Key.Value : Key.Value, Tolerance))
		{
			++Verified;
		}
		else
		{
			TSharedPtr<FJsonObject> Miss = UEMCP::SequencerInfo::MakeKeyTimeObject(Key.Time, Tick, Display);
			Miss->SetNumberField(TEXT("requested"), Key.Value);
			Miss->SetField(TEXT("readBack"), Found ? KindValue(Ref->Kind, Found->Value) : MakeShared<FJsonValueNull>());
			Mismatches.Add(MakeShared<FJsonValueObject>(Miss));
		}
	}

	auto Result = MCPSuccess();
	MCPSetUpdated(Result);
	SetSelectorFields(Result, R, Sel.SectionIndex);
	Result->SetStringField(TEXT("channel"), Ref->Name);
	Result->SetStringField(TEXT("valueKind"), KindName(Ref->Kind));
	if (!CreatedParameterType.IsEmpty()) Result->SetStringField(TEXT("createdParameter"), CreatedParameterType);
	Result->SetNumberField(TEXT("keysWritten"), Pending.Num());
	Result->SetNumberField(TEXT("keysVerified"), Verified);
	Result->SetNumberField(TEXT("keyCountBefore"), Before.Num());
	Result->SetNumberField(TEXT("keyCount"), After.Num());
	Result->SetArrayField(TEXT("keys"), AfterJson);
	double DefaultNow = 0.0;
	const bool bHasDefaultNow = ReadDefault(*Ref, DefaultNow);
	Result->SetBoolField(TEXT("hasDefault"), bHasDefaultNow);
	if (bHasDefaultNow) Result->SetField(TEXT("default"), KindValue(Ref->Kind, DefaultNow));
	if (Mismatches.Num() > 0)
	{
		Result->SetArrayField(TEXT("mismatches"), Mismatches);
		Result->SetStringField(TEXT("warning"), TEXT("Some keys did not read back with the value written; see mismatches"));
	}

	// Restoring is a full rewrite of this channel with the keys and default it had.
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("sequencePath"), R.Sequence->GetPathName());
	if (!Sel.TrackType.IsEmpty()) Payload->SetStringField(TEXT("trackType"), Sel.TrackType);
	if (!Sel.TrackName.IsEmpty()) Payload->SetStringField(TEXT("trackName"), Sel.TrackName);
	Payload->SetNumberField(TEXT("trackIndex"), Sel.TrackIndex);
	if (R.Binding.IsValid()) Payload->SetStringField(TEXT("bindingGuid"), R.Binding.ToString());
	Payload->SetNumberField(TEXT("sectionIndex"), Sel.SectionIndex);
	Payload->SetStringField(TEXT("channel"), Ref->Name);
	Payload->SetBoolField(TEXT("clearExisting"), true);
	TArray<TSharedPtr<FJsonValue>> PriorKeys;
	for (const FKeyRead& Key : Before)
	{
		TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>();
		K->SetNumberField(TEXT("tick"), Key.Time.Value);
		K->SetField(TEXT("value"), KindValue(Ref->Kind, Key.Value));
		if (FCString::Strcmp(Key.Interp, TEXT("none")) != 0) K->SetStringField(TEXT("interpolation"), Key.Interp);
		PriorKeys.Add(MakeShared<FJsonValueObject>(K));
	}
	Payload->SetArrayField(TEXT("keyframes"), PriorKeys);
	if (bHadDefault) Payload->SetField(TEXT("defaultValue"), KindValue(Ref->Kind, PreviousDefault));
	else Payload->SetBoolField(TEXT("clearDefault"), true);
	MCPSetRollback(Result, TEXT("set_sequence_channel_keys"), Payload);
	Result->SetBoolField(TEXT("rollbackLossy"), true);
	Result->SetStringField(TEXT("rollbackNote"),
		TEXT("The rollback rewrites this channel with the keys and default it held before, each with its interpolation mode. Custom tangents are not restored, ")
		TEXT("and a parameter this call added to a parameter section stays on the section with the old (empty) key set."));
	return MCPResult(Result);
}

// remove_sequence_section (#1232): delete one section from a track by index.
TSharedPtr<FJsonValue> FSequencerHandlers::RemoveSection(const TSharedPtr<FJsonObject>& Params)
{
	using namespace UEMCP::SequencerTracks;
	using namespace UEMCP::SequencerChannels;
	FSectionSelector Sel;
	Sel.Read(Params);

	if (auto PieErr = MCPRefuseDuringPlayInEditor(TEXT("remove_sequence_section"))) return PieErr;

	FResolvedSection R;
	if (TSharedPtr<FJsonValue> Err = ResolveSection(Params, Sel, R)) return Err;
	if (!Sel.bHasSectionIndex)
	{
		return MCPError(TEXT("Pass sectionIndex: removing a section by default would pick one for you"));
	}

	const FFrameRate Tick = R.MovieScene->GetTickResolution();
	const FFrameRate Display = R.MovieScene->GetDisplayRate();
	TSharedPtr<FJsonObject> RemovedRange = MakeShared<FJsonObject>();
	UEMCP::SequencerInfo::SetTimingRangeFields(*RemovedRange, R.Section->GetRange(), Tick, Display);
	const FString RemovedClass = R.Section->GetClass()->GetName();
	const int32 Before = R.Track->GetAllSections().Num();

	{
		// One transaction, so the editor undo of it is the inverse.
		FScopedTransaction Transaction(NSLOCTEXT("UEMCP", "RemoveSequenceSection", "Remove Sequence Section"));
		R.Track->Modify();
		R.Section->Modify();
		R.Track->RemoveSection(*R.Section);
	}
	R.Sequence->GetOutermost()->MarkPackageDirty();

	const int32 After = R.Track->GetAllSections().Num();
	if (After != Before - 1)
	{
		return MCPError(FString::Printf(TEXT("The track still holds %d section(s) after the removal (had %d)"), After, Before));
	}

	auto Result = MCPSuccess();
	MCPSetUpdated(Result);
	Result->SetStringField(TEXT("sequencePath"), R.Sequence->GetPathName());
	Result->SetObjectField(TEXT("track"), DescribeTrack(R.Track));
	if (R.Binding.IsValid()) Result->SetStringField(TEXT("bindingGuid"), R.Binding.ToString());
	Result->SetNumberField(TEXT("removedSectionIndex"), Sel.SectionIndex);
	Result->SetStringField(TEXT("removedSectionClass"), RemovedClass);
	Result->SetObjectField(TEXT("removedRange"), RemovedRange);
	Result->SetNumberField(TEXT("sectionCountBefore"), Before);
	Result->SetNumberField(TEXT("sectionCount"), After);
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("steps"), 1);
	Payload->SetStringField(TEXT("direction"), TEXT("undo"));
	MCPSetRollback(Result, TEXT("undo_redo_steps"), Payload);
	Result->SetStringField(TEXT("rollbackNote"), TEXT("The rollback is the editor undo of this removal's transaction, so it restores the section only while that transaction is still the latest on the undo stack."));
	return MCPResult(Result);
}
