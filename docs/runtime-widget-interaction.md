# Runtime widget interaction

`widget(action="invoke_runtime_function")` drives a live UMG widget while Play-In-Editor is running. It covers two shapes:

- **A UFUNCTION on the UserWidget.** Pass `functionName` with no `childName`. The function must take no parameters.
- **An interaction on a child widget.** Pass `childName`. The handler writes the new state through the widget's own setter and broadcasts the delegate the real interaction fires, so Blueprint logic bound to that event runs. Setting the property without the broadcast would leave the graph unrun, which is the reason this path exists.

Locate the widget with `widgetName` (exact instance name) or `className` (first match). `widget(action="list_runtime")` lists the live instances.

This is one interaction per call, and it calls the widget's own setter rather
than synthesizing an input event. For a timed sequence of interactions, or for
real key and axis events at the input layer, use `pie(inject_input*)` from the
[PIE testing](pie.md) tool. See [Widgets](widgets.md) for where that line sits.

## Supported child widgets

| Widget class | `value` | Delegates broadcast |
| --- | --- | --- |
| `Button` | not used | `OnClicked` (default), or `OnPressed` / `OnReleased` / `OnHovered` / `OnUnhovered` via `functionName` |
| `CheckBox` | `true` / `false`, `0` / `1`, `"checked"`, `"unchecked"`, `"undetermined"`, `"toggle"`. Omitted means toggle | `OnCheckStateChanged` |
| `Slider` | number, clamped to the slider's min/max | `OnValueChanged` |
| `SpinBox` | number | `OnValueChanged` then `OnValueCommitted` |
| `EditableText`, `EditableTextBox`, `MultiLineEditableText`, `MultiLineEditableTextBox` | string | `OnTextChanged` then `OnTextCommitted` |
| `ComboBoxString` | option string, or the option index as a number | `OnSelectionChanged` |

Any other class returns an error naming the class and listing the supported ones, so a UI pass never records a silent no-op as a pass.

## Selecting a single delegate

`functionName` alongside `childName` narrows the broadcast instead of naming a UFUNCTION on the parent. On a text field, `functionName: "OnTextChanged"` sets the text and fires only the change event, which is how you exercise live validation without the commit path. A name the child does not expose returns an error listing the delegates it does.

`commitMethod` picks the commit type for text fields and spin boxes: `OnEnter` (default), `OnUserMovedFocus`, `OnCleared`, `Default`.

## Examples

Toggle a checkbox and run its bound graph:

```json
{ "action": "invoke_runtime_function", "widgetName": "WBP_Settings_C_0", "childName": "CheckBox_0" }
```

Set a checkbox explicitly, move a slider, type a name, pick a combo entry:

```json
{ "action": "invoke_runtime_function", "className": "WBP_Settings", "childName": "InvertYCheck", "value": true }
{ "action": "invoke_runtime_function", "className": "WBP_Settings", "childName": "MasterVolume", "value": 0.35 }
{ "action": "invoke_runtime_function", "className": "WBP_Settings", "childName": "PlayerName", "value": "Ada" }
{ "action": "invoke_runtime_function", "className": "WBP_Settings", "childName": "QualityCombo", "value": "Epic" }
```

## Response

```json
{
  "success": true,
  "widget": "WBP_Settings_C_0",
  "child": "InvertYCheck",
  "childClass": "CheckBox",
  "interaction": "check",
  "previousValue": "unchecked",
  "value": "checked",
  "isChecked": true,
  "invoked": "OnCheckStateChanged",
  "delegates": ["OnCheckStateChanged"]
}
```

`invoked` holds the primary delegate name, `delegates` every delegate broadcast by the call. Numeric widgets also return `requestedValue` when clamping changed the applied value, and the combo box returns `selectedIndex`.

## Broadcast accounting

Each delegate fires exactly once per call. Some UMG setters broadcast on their own, so the handler only broadcasts by hand where the setter is silent:

- `UCheckBox::SetCheckedState`, `USpinBox::SetValue` and every `SetText` overload are silent. The handler broadcasts.
- `USlider::SetValue` broadcasts `OnValueChanged`, but only when the value actually moves. Driving the slider to the value it already holds is still a request to run the graph, so the handler broadcasts in that case.
- `UComboBoxString::SetSelectedOption` broadcasts `OnSelectionChanged` on a real change and no-ops when the option is already selected, where the handler broadcasts with `ESelectInfo::Direct`.
