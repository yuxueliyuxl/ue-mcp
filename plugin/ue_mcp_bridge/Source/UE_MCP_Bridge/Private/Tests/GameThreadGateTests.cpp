#if WITH_DEV_AUTOMATION_TESTS

#include "GameThreadExecutor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"

/**
 * The readiness gate, and the one class of call it must never close in front of.
 *
 * #968: a modal raised during editor startup (the "Restore Packages" prompt
 * after an unclean shutdown) blocks the game thread before the editor is ever
 * marked ready. Every handler then answered "Editor is still initializing" -
 * including respond_to_dialog and set_dialog_policy, the two calls that could
 * have dismissed it. The gate was held shut by the very dialog those calls
 * exist to clear, and the only escape was an OS kill, which discards whatever
 * the user was being asked about. Meanwhile get_engine_state described the
 * dialog down to its button labels, because it answers off the game thread.
 *
 * These run on the game thread, so ExecuteOnGameThread dispatches inline and
 * the assertions are about the gate itself rather than about ticker timing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPGameThreadReadyGateTest,
	"UE.MCP.Bridge.GameThread.ReadyGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPGameThreadReadyGateTest::RunTest(const FString& Parameters)
{
	FMCPGameThreadExecutor Executor;
	TestFalse(TEXT("a fresh executor has not been told the editor is ready"), Executor.IsEditorReady());

	bool bHandlerRan = false;
	FMCPGameThreadExecutor::FHandlerFunction Handler =
		[&bHandlerRan](const TSharedPtr<FJsonObject>&) -> TSharedPtr<FJsonValue>
		{
			bHandlerRan = true;
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("success"), true);
			return MakeShared<FJsonValueObject>(Result);
		};

	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	// An ordinary handler still waits. Refusing work that needs a built editor
	// is the whole point of the gate and none of this weakens it.
	TSharedPtr<FJsonValue> Gated = Executor.ExecuteOnGameThread(Handler, Params, 5.0f, /*bModalSafe*/ false);
	TestTrue(TEXT("an ordinary handler is refused before the editor is ready"), Gated.IsValid() && Gated->Type == EJson::Object);
	FString GatedError;
	if (Gated.IsValid() && Gated->Type == EJson::Object)
	{
		Gated->AsObject()->TryGetStringField(TEXT("error"), GatedError);
	}
	TestTrue(TEXT("and told why"), GatedError.Contains(TEXT("still initializing")));
	TestFalse(TEXT("and never reached the handler"), bHandlerRan);

	// A modal-safe handler runs. It is the escape hatch from a blocked game
	// thread, so nothing the block causes may stand in front of it.
	bHandlerRan = false;
	TSharedPtr<FJsonValue> Exempt = Executor.ExecuteOnGameThread(Handler, Params, 5.0f, /*bModalSafe*/ true);
	TestTrue(TEXT("a modal-safe handler runs before the editor is ready"), bHandlerRan);
	TestTrue(TEXT("and returns its own result, not the gate's error"),
		Exempt.IsValid() && Exempt->Type == EJson::Object && Exempt->AsObject()->HasField(TEXT("success")));

	FString ExemptError;
	if (Exempt.IsValid() && Exempt->Type == EJson::Object)
	{
		Exempt->AsObject()->TryGetStringField(TEXT("error"), ExemptError);
	}
	TestTrue(TEXT("with no initializing error attached"), ExemptError.IsEmpty());

	// Once the editor is ready both go through, so the exemption is about the
	// window before readiness and changes nothing after it.
	Executor.SetEditorReady();
	bHandlerRan = false;
	Executor.ExecuteOnGameThread(Handler, Params, 5.0f, /*bModalSafe*/ false);
	TestTrue(TEXT("an ordinary handler runs once the editor is ready"), bHandlerRan);

	return true;
}

#endif
