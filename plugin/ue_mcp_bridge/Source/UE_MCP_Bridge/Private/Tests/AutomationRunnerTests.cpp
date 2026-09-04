#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

/**
 * The invariant run_automation_tests depends on to stop taking the editor down.
 *
 * #993: FAutomationTestFramework::StopTest asserts LatentCommands.IsEmpty()
 * inside InternalStopTest, and the handler called it unconditionally after a
 * drain loop bounded by a fixed iteration count. A CQTest test that starts
 * multi-client PIE still had commands queued at that point, so the assert fired
 * and terminated the editor; the caller saw a WebSocket close 1006 instead of a
 * test result.
 *
 * The handler now asks IsLatentCommandQueueEmpty() and, when the answer is no,
 * clears the queue with DequeueAllCommands() and reports the test as abandoned
 * rather than stopping it mid-queue. Both halves of that are asserted here:
 * that the framework really does report a queued command, and that dequeuing
 * really does restore the precondition StopTest requires.
 */
class FMCPNeverCompletingLatentCommand : public IAutomationLatentCommand
{
public:
	virtual bool Update() override
	{
		// The shape of a latent command waiting on something that only engine
		// frames can deliver: it never completes from inside a handler that
		// holds the game thread.
		return false;
	}
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPAutomationLatentQueueTest,
	"UE.MCP.Bridge.Automation.LatentQueueGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPAutomationLatentQueueTest::RunTest(const FString& Parameters)
{
	FAutomationTestFramework& Framework = FAutomationTestFramework::Get();

	// Whatever happens below, this test must not be the thing that leaves a
	// command queued: its own StopTest would then hit the same assert.
	ON_SCOPE_EXIT
	{
		FAutomationTestFramework::Get().DequeueAllCommands();
	};

	TestTrue(TEXT("nothing is queued before the test enqueues anything"),
		Framework.IsLatentCommandQueueEmpty());

	Framework.EnqueueLatentCommand(MakeShared<FMCPNeverCompletingLatentCommand>());
	TestFalse(TEXT("a queued latent command is visible to the guard"),
		Framework.IsLatentCommandQueueEmpty());

	// Draining does not help: this is the command that cannot finish, which is
	// exactly the case the handler has to survive rather than assert on.
	Framework.ExecuteLatentCommands();
	TestFalse(TEXT("a command that never completes stays queued through a drain"),
		Framework.IsLatentCommandQueueEmpty());

	// And the recovery. This is the only reason StopTest is reachable at all
	// once a test has outstanding latent work.
	Framework.DequeueAllCommands();
	TestTrue(TEXT("dequeuing restores the precondition StopTest asserts on"),
		Framework.IsLatentCommandQueueEmpty());

	return true;
}

#endif
