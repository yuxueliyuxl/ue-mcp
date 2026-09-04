# Spatial instructions

A human says "turn the hood a bit clockwise". An editor needs an axis, an angle,
a coordinate frame, and a promise about what must not move. This page is how one
becomes the other.

It exists because the failure it prevents is silent. A raw relative-Euler edit on
an attached or socketed component changes axes nobody asked about, reports
success, and looks fine in the response. The mistake only surfaces in a
screenshot, usually later, usually to a person.

## The six-field sentence

Every spatial request normalizes to six fields. If all six are known, the
transform is unambiguous.

<img src="images/spatial-instruction-contract.svg" alt="A human visual cue is normalized into target, frame, viewpoint, operation, amount and constraints, then applied and verified" style="max-width:100%">

| Field | Answers | Example |
|---|---|---|
| Target | Which actor, and which component on it | `Player` / `LeatherHood` |
| Frame | Whose axes | `actor` |
| Viewpoint | Looking from where | looking down |
| Operation | Translate, rotate, or scale | rotate about up |
| Amount | How much, in real units | 15 degrees clockwise |
| Constraints | What must not change | preserve location and scale |

Most requests supply three or four of these and imply the rest. Infer what is
obvious. Ask exactly one question when the missing field would change the
result, which in practice means frame or viewpoint.

## Frame: whose axes are these?

"Move forward" means the +X direction **of the selected frame**. The same words
produce four different moves.

<img src="images/spatial-reference-frames.svg" alt="World, actor, parent and component coordinate frames compared" style="max-width:100%">

- **world** is fixed to the level and never follows the character. Use it for
  level-aligned moves: "line it up with the wall".
- **actor** follows the character. This is what a person means by left, right,
  forward and back when they are talking about a character.
- **parent** is the attachment parent's frame. Use it when the component should
  move consistently with whatever it hangs off.
- **component** is the component's own current orientation. Use it for "push it
  further along the way it is already pointing".

When a person says "the character's right", they mean `frame="actor"`. They do
not mean the camera, even though the camera is what they are looking at.

## Viewpoint: clockwise is incomplete without it

Clockwise is not a property of a rotation. It is a property of a rotation
**plus** the side you are viewing it from. The same rotation is clockwise and
counterclockwise depending on where you stand.

<img src="images/spatial-viewpoint-clock.svg" alt="A top-down viewpoint defining a clockwise rotation about the actor up axis" style="max-width:100%">

So "rotate it clockwise" is not yet an instruction. "Looking down on the
character, rotate it 15 degrees clockwise" is: it names the axis (actor up) and
the sign together.

If a request says clockwise without a viewpoint, that is the one question worth
asking.

## Phrase book

<img src="images/spatial-communication-quick-reference.svg" alt="Quick reference mapping human spatial phrasing to machine parameters" style="max-width:100%">

## Doing it

`level(nudge_component)` takes the normalized fields directly. It composes
quaternion deltas rather than writing Euler angles, which is what keeps an
attached or socketed component from drifting on axes that were never mentioned.

```text
level(action="nudge_component",
      actorLabel="Player",
      componentName="LeatherHood",
      frame="actor",
      axisRotation={ axis: "up", degrees: -15 },
      world="editor")
```

It reports both the relative and the world transform afterwards, so the result
can be checked rather than assumed.

Translation is in centimetres along the frame's forward, right and up. Scale is a
uniform multiplier. Rotation is an axis and an angle, never three Euler numbers.

## Verifying

A spatial change is only done when someone can see that it is done. Read the
returned transform, and for anything a human will look at, capture an image.

Frame the capture so the contact region fills roughly half to three quarters of
the shot, and take it from more than one angle: front, side, top, three-quarter.
A single wide shot or a dark shot proves nothing, and accepting one is how a
wrong result gets reported as a right one.

When the target is attached to a character, stop the actor and freeze the parent
skeletal pose before the baseline capture, and hold both across every subsequent
probe. Otherwise the pose moves between captures and the comparison measures the
animation rather than the edit.

## When to ask

Ask when the answer changes the transform:

- clockwise or counterclockwise with no viewpoint named
- left or right when it is genuinely unclear whether the character or the camera
  is the reference
- a component that is attached, where the frame decides whether the parent
  carries the change

Do not ask when context settles it. "Move the character's arm forward 5 cm" has
a target, a frame, an operation and an amount, and the missing viewpoint does not
affect a translation.
