#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class STextBlock;

/**
 * Registers and unregisters the UE-MCP connection indicator on the level
 * editor toolbar (the "User" strip, right of the play controls and left of
 * the settings gear).
 */
namespace FMCPToolbarStatus
{
	/** Add the indicator. Safe to call twice; the second call is a no-op. */
	void Install();

	/** Remove the indicator. Safe to call twice; the second call is a no-op. */
	void Remove();
}

/**
 * The indicator itself: a colored dot plus one line of text, refreshed once a
 * second from the bridge server's live counters.
 *
 * State:
 *   - red    "UE-MCP · Error"            - the bridge failed to start/stopped;
 *   - amber  "UE-MCP · Listening :9877"  - listening, no WebSocket client yet;
 *   - green  "UE-MCP · Connected (N)"    - N clients completed the handshake.
 *
 * Hovering shows the port, where the port number came from, and the status
 * snapshot path; clicking copies the same summary to the clipboard. Both are
 * cheap, read-only views of counters the socket threads already maintain, so
 * this widget never touches the socket or the game thread executor.
 */
class SMCPServerStatusWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMCPServerStatusWidget) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	/** Runs once a second; returns Continue forever. */
	EActiveTimerReturnType RefreshStatus(double InCurrentTime, float InDeltaTime);

	void UpdateStatus();
	FReply OnClicked();

	TSharedPtr<STextBlock> Dot;
	TSharedPtr<STextBlock> StatusText;
};
