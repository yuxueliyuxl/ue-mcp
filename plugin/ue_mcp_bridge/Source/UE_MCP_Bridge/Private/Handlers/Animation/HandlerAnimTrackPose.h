#pragma once

// Raw bone-track reads and rotation wire shapes shared by read_bone_track,
// sample_pose, transform_bone_tracks and compose_pose_sequence.

#include "CoreMinimal.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "ReferenceSkeleton.h"
#include "HandlerUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace MCPAnimTrackPose
{
	/** How a read reports rotations: euler {pitch,yaw,roll}, quaternion {x,y,z,w}, or both. */
	enum class ERotationFormat : uint8 { Euler, Quaternion, Both };

	inline bool ParseRotationFormat(const FString& Token, ERotationFormat& Out)
	{
		if (Token.IsEmpty() || Token.Equals(TEXT("euler"), ESearchCase::IgnoreCase)) { Out = ERotationFormat::Euler; return true; }
		if (Token.Equals(TEXT("quaternion"), ESearchCase::IgnoreCase) || Token.Equals(TEXT("quat"), ESearchCase::IgnoreCase)) { Out = ERotationFormat::Quaternion; return true; }
		if (Token.Equals(TEXT("both"), ESearchCase::IgnoreCase)) { Out = ERotationFormat::Both; return true; }
		return false;
	}

	/** rotation is the Euler or the quaternion form; both adds rotationQuat beside the Euler one. */
	inline void WriteRotation(const TSharedPtr<FJsonObject>& Obj, const FQuat& Q, ERotationFormat Format)
	{
		if (Format == ERotationFormat::Quaternion)
		{
			Obj->SetObjectField(TEXT("rotation"), MCPQuatToJsonObject(Q));
			return;
		}
		Obj->SetObjectField(TEXT("rotation"), MCPRotatorToJsonObject(Q.Rotator()));
		if (Format == ERotationFormat::Both)
		{
			Obj->SetObjectField(TEXT("rotationQuat"), MCPQuatToJsonObject(Q));
		}
	}

	/** A rotation given as {x,y,z,w} or {pitch,yaw,roll}. A quaternion needs all four
	 *  components and a non-zero length, and comes back normalized. */
	inline bool ReadRotationObject(const TSharedPtr<FJsonObject>& Obj, FQuat& Out, FString& OutError)
	{
		if (!Obj.IsValid())
		{
			OutError = TEXT("rotation must be an object, {x,y,z,w} or {pitch,yaw,roll}");
			return false;
		}
		double X, Y, Z, W;
		if (Obj->HasField(TEXT("w")))
		{
			if (!Obj->TryGetNumberField(TEXT("x"), X) || !Obj->TryGetNumberField(TEXT("y"), Y)
				|| !Obj->TryGetNumberField(TEXT("z"), Z) || !Obj->TryGetNumberField(TEXT("w"), W))
			{
				OutError = TEXT("a quaternion rotation needs numeric x, y, z and w");
				return false;
			}
			FQuat Q(X, Y, Z, W);
			if (Q.SizeSquared() < UE_SMALL_NUMBER)
			{
				OutError = TEXT("a quaternion rotation must not be zero length");
				return false;
			}
			Q.Normalize();
			Out = Q;
			return true;
		}
		double Pitch = 0, Yaw = 0, Roll = 0;
		const bool bAny = Obj->TryGetNumberField(TEXT("pitch"), Pitch) | Obj->TryGetNumberField(TEXT("yaw"), Yaw) | Obj->TryGetNumberField(TEXT("roll"), Roll);
		if (!bAny)
		{
			OutError = TEXT("rotation must be {x,y,z,w} or {pitch,yaw,roll}");
			return false;
		}
		Out = FRotator(Pitch, Yaw, Roll).Quaternion();
		return true;
	}

	/** The local transform an AnimSequence holds for one bone at Time: evaluated from
	 *  its track when it has one, otherwise the bone's reference pose. */
	inline FTransform LocalBoneAtTime(const UAnimSequence* Seq, const FReferenceSkeleton& Ref, int32 BoneIndex, double Time)
	{
		const FName Bone = Ref.GetBoneName(BoneIndex);
		const IAnimationDataModel* Model = Seq ? Seq->GetDataModel() : nullptr;
		if (Model && Model->IsValidBoneTrackName(Bone))
		{
			return Model->EvaluateBoneTrackTransform(Bone, Model->GetFrameRate().AsFrameTime(Time), EAnimInterpolationType::Linear);
		}
		const TArray<FTransform>& RefPose = Ref.GetRefBonePose();
		return RefPose.IsValidIndex(BoneIndex) ? RefPose[BoneIndex] : FTransform::Identity;
	}

	/** The same bone composed through its parent chain into component space. */
	inline FTransform ComponentBoneAtTime(const UAnimSequence* Seq, const FReferenceSkeleton& Ref, int32 BoneIndex, double Time)
	{
		FTransform Out = FTransform::Identity;
		for (int32 Index = BoneIndex; Index != INDEX_NONE; Index = Ref.GetParentIndex(Index))
		{
			Out = Out * LocalBoneAtTime(Seq, Ref, Index, Time);
		}
		return Out;
	}

	/** Every key of a bone track, or the reference pose repeated NumKeys times when
	 *  the sequence has no track for that bone. */
	inline TArray<FTransform> TrackKeysOrReference(const UAnimSequence* Seq, const FReferenceSkeleton& Ref, int32 BoneIndex, int32 NumKeys)
	{
		TArray<FTransform> Keys;
		const FName Bone = Ref.GetBoneName(BoneIndex);
		const IAnimationDataModel* Model = Seq ? Seq->GetDataModel() : nullptr;
		if (Model && Model->IsValidBoneTrackName(Bone))
		{
			Model->GetBoneTrackTransforms(Bone, Keys);
		}
		const int32 Had = Keys.Num();
		const FTransform Fill = Had > 0 ? Keys.Last() : Ref.GetRefBonePose()[BoneIndex];
		Keys.SetNum(FMath::Max(NumKeys, 0));
		for (int32 Index = Had; Index < Keys.Num(); ++Index)
		{
			Keys[Index] = Fill;
		}
		return Keys;
	}

	/** Load an AnimSequence, or the error that names what the path held instead. */
	inline TSharedPtr<FJsonValue> LoadSequence(const FString& Path, UAnimSequence*& Out)
	{
		UObject* Loaded = MCPLoadAssetObject(Path);
		Out = Cast<UAnimSequence>(Loaded);
		if (Out) return nullptr;
		return Loaded ? MCPAssetWrongTypeError(Path, Loaded, TEXT("AnimSequence")) : MCPAssetNotFoundError(Path);
	}

	/** The time a frame index plays at, refusing one outside the sequence. */
	inline bool FrameToTime(const UAnimSequence* Seq, int32 Frame, double& OutTime, FString& OutError)
	{
		const IAnimationDataModel* Model = Seq ? Seq->GetDataModel() : nullptr;
		if (!Model)
		{
			OutError = TEXT("the sequence has no data model");
			return false;
		}
		const int32 NumFrames = Model->GetNumberOfFrames();
		if (Frame < 0 || Frame > NumFrames)
		{
			OutError = FString::Printf(TEXT("frame %d is outside '%s', which has frames 0 to %d"), Frame, *Seq->GetName(), NumFrames);
			return false;
		}
		const double Rate = Model->GetFrameRate().AsDecimal();
		OutTime = Rate > 0.0 ? Frame / Rate : 0.0;
		return true;
	}
}
