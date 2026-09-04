#if WITH_DEV_AUTOMATION_TESTS

#include "MCPEngineStatus.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

/**
 * What the status writer puts on disk, and what it refuses to put there.
 *
 * #990: every process that loads this plugin ran a writer thread that wrote
 * `status.json` through one shared `status.json.tmp`. A distributed HLOD build
 * runs 32 WorldPartitionBuilderCommandlet slots against one project directory,
 * so 32 processes deleted and renamed the same temp file four times a second.
 * One run produced 3,743 Error-severity log lines, and every slot ended with
 * "Failure - 112 error(s)" even where the HLOD work succeeded, which breaks
 * error-count success detection for CI.
 *
 * Nothing here binds a port or writes into the live state directory: these are
 * assertions about paths and about the commandlet decision, both of which are
 * pure.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPEngineStatusPathsTest,
	"UE.MCP.BridgeStatus.StatusPaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPEngineStatusPathsTest::RunTest(const FString& Parameters)
{
	const FString Shared = FMCPEngineStatus::StatusFilePath();
	const FString Own = FMCPEngineStatus::InstanceStatusFilePath();

	TestEqual(TEXT("the shared path keeps its historical name"),
		FPaths::GetCleanFilename(Shared), FString(TEXT("status.json")));

	// The per-process name is what makes two editors of one project describable
	// at the same time, and what a reader falls back across.
	TestEqual(TEXT("the per-process path is named for this pid"),
		FPaths::GetCleanFilename(Own),
		FString::Printf(TEXT("status.%u.json"), FPlatformProcess::GetCurrentProcessId()));
	TestNotEqual(TEXT("the two are different files"), Own, Shared);
	TestEqual(TEXT("and live in the same directory"), FPaths::GetPath(Own), FPaths::GetPath(Shared));
	TestEqual(TEXT("which is the bridge state directory"),
		FPaths::GetPath(Own), FMCPEngineStatus::StatusDir());

	// The publish decision. An editor publishes; a commandlet does not, because
	// nothing polls a commandlet's engine state and dozens of them share one
	// project directory.
	TestTrue(TEXT("publishing follows the commandlet flag, and only that"),
		FMCPEngineStatus::ShouldPublishStatus() == !IsRunningCommandlet());

	// The snapshot has to name its subject: a document that does not say which
	// process it describes cannot be told apart from one that does.
	const TSharedPtr<FJsonObject> Snapshot = FMCPEngineStatus::Get().Snapshot();
	TestTrue(TEXT("a snapshot exists"), Snapshot.IsValid());
	double Pid = 0.0;
	TestTrue(TEXT("the snapshot names its process"), Snapshot.IsValid() && Snapshot->TryGetNumberField(TEXT("pid"), Pid));
	TestEqual(TEXT("and it is this one"), (int32)Pid, (int32)FPlatformProcess::GetCurrentProcessId());

	return true;
}

#endif
