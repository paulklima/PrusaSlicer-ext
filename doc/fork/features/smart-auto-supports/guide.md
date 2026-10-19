# Support planner: user guide

PrusaSlicer-ext, feature "Support planner" (layer analysis for paint-on supports). This guide
describes the first version, based on PrusaSlicer 2.9.6.

The support planner adds a section to the **Paint-on supports** tool. With one click it analyses
the sliced layers of your object and paints support enforcers only where the print really needs
support. It is made for figurines and other organic models printed with **organic supports**.

Contents:
1. [What the support planner does](#1-what-the-support-planner-does)
2. [Before you start](#2-before-you-start)
3. [Quick start](#3-quick-start)
4. [The panel](#4-the-panel)
5. [Colours on the model](#5-colours-on-the-model)
6. [The report window](#6-the-report-window)
7. [Working with your own paint and other tools](#7-working-with-your-own-paint-and-other-tools)
8. [Saving projects](#8-saving-projects)
9. [Limits and tips](#9-limits-and-tips)

## 1. What the support planner does

With automatic supports off, organic support grows only where you paint enforcers. The support
planner does this painting for you. It compares each layer of the sliced object with the layer
below. Then it paints the downward faces of the parts that would fail without support. The slicer
builds the organic branches from this paint, just as it does from your own paint.

### Terms used in this guide

- **Island**: a part that starts in mid-air. Examples: the lowest point of a hanging hand, a chin,
  the tip of a tail, a separate floating part. Islands always need support.
  - Tiny downward bumps (less than about 1 mm³) are ignored if they join the body again within
    about 1 mm.
  - A thin hanging tip gets support along its thin lower part, until it is about 3 mm thick
    (at most 16 mm high).
  - A part that starts just above the plate is not an island. Tree support cannot start that low
    (on the layers up to the top contact distance plus one layer: about 0.5 mm with 0.2 mm layers
    and 0.2 mm contact distance), and the part is laid onto the plate like the first layer. A toe
    tip that floats 0.1 mm above the bed counts as standing on the bed.
  - Every island gets its own support, also when it later grows together with other islands
    before it reaches the body (the knuckles of a toe hanging from the foot).
  - In this version these values are fixed.
- **Overhang**: material that sticks out beyond the layer below.
  - A little per layer prints fine; that is the **Self-supporting angle**.
  - Anything beyond that **adds up from layer to layer**. A short chamfer stays small. A long,
    shallow slope (the underside of an outstretched arm, a wide hat brim) hangs further and
    further from the place that holds it.
  - This added-up distance is the **reach**. Where the reach exceeds **Max overhang reach**,
    the support planner paints. Support starts the count again from zero.
- **Bridge**: a ceiling between two walls that the slicer prints as straight lines from one side
  to the other. A bridge holds by tension. Bridges up to **Max bridge span** are kept free; the
  span is measured along the direction in which the slicer will print the lines.
- **Dome and hole ceilings**: a ring of material that closes around a single opening. Examples:
  the top of a hollow dome or head, a funnel, the step of a counterbore. Such a ring holds itself
  up to **Max dome opening** and is kept free.
- **On the plate / on the model**: an organic branch starts either on the build plate or on a
  lower part of the model. Branches on the model leave marks and are harder to remove. The
  support planner therefore puts support where the branches can reach the build plate, wherever that
  is possible.

### Compared with "Automatic painting"

The existing **Automatic painting** button is unchanged. The differences:

| | Automatic painting | Support planner |
|---|---|---|
| Dome tops, hole ceilings | often painted | kept free up to Max dome opening |
| Bridges | fixed limit of about 16 mm, shorter on curved parts | kept free up to Max bridge span (default 25 mm) |
| Your own paint | erased | kept, and taken into account |
| What was not painted | not shown | listed in a report, with a marker on the model |

### What it deliberately does not do

- It does not paint bridges or dome and hole ceilings that hold themselves.
- In this version, **long bridges** (longer than Max bridge span) are reported but not painted.
- It **never overwrites your own paint** (enforcers and blockers).
- It does not change the existing tools. Brush, Smart fill, Highlight overhang by angle,
  Automatic painting and Remove all selection work as before.
- It does not build support itself. The slicer's organic support does that, from the paint.
- It does not check closed cavities, the stability of the part, or other objects on the plate
  (see [section 9](#9-limits-and-tips)).

## 2. Before you start

### Requirements

- An FFF printer profile.
- **Advanced or Expert mode**: Paint-on supports is not available in Simple mode.
- One object selected.
- The support settings in the table below. The panel checks them for the selected object: your
  print profile plus the object's own settings.

| Setting (Print Settings, Support material) | Needed | Why |
|---|---|---|
| Generate support material | on | otherwise the slicer ignores the paint |
| Auto generated supports | off | otherwise the slicer adds support everywhere by angle |
| Style | Organic | the support planner predicts how organic branches grow |
| Enforce support for the first n layers | 0 | it adds support below that height anyway |
| Top contact Z distance | more than 0 ("Fix…" sets 0.2) | at 0 the slicer prints no bridges, so the bridges that the support planner keeps free would not print as it expects |
| Support on build plate only | off for "Prefer build plate", on for "Build plate only" | it must match the panel's **Placement** choice |

### The warning line and "Fix…"

If a setting does not match, the panel shows a line such as **"! 2 support settings do not
match"**. Hover over the line to see which settings, and hover over **"Fix…"** next to it to see
the changes.

"Fix…" opens a dialog. It lists what does not match, and under **"Set for this object only:"** the
exact changes, for example "Auto generated supports: off" or "Style: Organic". The buttons:

- **Apply** changes the settings.
- **Apply and paint** changes them and starts painting right away.
- **Cancel** closes the dialog and changes nothing.

What "Apply" changes:
- only the **selected object**. The values are added as the object's own settings, shown under
  the object in the object list. Your print profile stays as it is;
- with **one undo step**, "Support planner settings". Ctrl+Z reverts it.

### Clicking "Analyse and paint" while settings do not match

You get a dialog with three buttons:

- **Apply and paint** makes the same changes as "Fix…", then paints.
- **Paint anyway** paints with the current settings. The slicer may then ignore or change the
  painted support. PrusaSlicer remembers this answer for the object until you restart it.
- **Cancel** stops; the dialog appears again next time.

### Interface layers are only suggested after a run

**Top interface layers** (dense layers between the support and the model) are not part of the
check above.
- **Why they are useful:** they give nicer surfaces under flat overhangs, and most models print
  well with them.
- **The catch:** they keep the support about 0.5 mm away from steep slopes and thin hanging tips,
  so these areas cannot be supported.

Only the analysis can tell whether your model has such areas. So the panel suggests a change only
**after a run** that found some:

> ! 4 areas (steep slopes, hanging tips) cannot be supported while Top interface layers > 0  [Fix…]

This "Fix…" sets "Top interface layers" to 0 for this object only, together with any other
setting that does not match. The dialog lists the side effects:
- no dense interface under any flat overhang;
- bottom interfaces also switch off;
- short on-model branches are removed (more "Support on the model may be removed" entries).

Use "Apply and paint" there to analyse again with the new setting. Ctrl+Z reverts it.

## 3. Quick start

1. Select the object and open **Paint-on supports** (key **L**, or the left toolbar).
2. Click **"Support planner »"** next to "Automatic painting". This opens the section **"Support
   planner (layer analysis)"** at the end of the window and scrolls to it.
3. If the line "! … support settings do not match" appears, click **"Fix…"** and then **Apply**.
4. Keep the default values for your first try. After an update from an earlier version, click
   **Reset to defaults** (under Advanced) once to get the current defaults.
5. Click **"Analyse and paint"**.
   - The panel shows "Slicing… 3 s", then a bar "Analysing layers… 42 %", each with a **Cancel**
     button.
   - The analysis itself usually takes a few seconds.
6. When it is done, a summary appears, for example "Painted 6 areas (islands 1, overhangs 5) on
   downward faces — press 2 for the bottom view."
   - Press **2** to look at the object from below.
   - Islands are painted amber, overhangs cyan.
7. If a line "Report: … problems, … notes" appears, click **"Show report"** and look at the
   problems first ([section 6](#6-the-report-window)).
8. Close the tool, click **Slice now** and check the **Preview**. The organic branches should
   stand under the painted areas, on the build plate.
9. Not happy with the result?
   - Change a value in the panel and click "Analyse and paint" again. Each run replaces the
     previous planned support.
   - To keep an area free, paint a blocker there.
   - To add support, paint an enforcer there.

## 4. The panel

The section **"Support planner (layer analysis)"** sits at the end of the Paint-on supports window.
Click its header to open or close it.
- The values are remembered for all objects and projects. They are stored in PrusaSlicer's own
  settings, not in the project.
- Each slider has an edit button next to it, for typing an exact value.
- Changing a value does not repaint by itself. Click "Analyse and paint" again.
- With the section open the window gets taller. In this fork it stays within the 3D view and
  scrolls.

### Fields

| Field | Default | Range | What it does | When to change it |
|---|---|---|---|---|
| **Support for**: Islands, Overhangs | both checked | – | Which causes get painted. Unchecked causes are still analysed and listed in the report. | Uncheck one to paint only the other kind. Switching one off never changes the other's paint. |
| **Self-supporting angle** | 45° (not taken from your profile) | 0–89°, 0 = auto | Slopes steeper than this angle (measured from horizontal) print without support. "auto" lets each layer overhang by half an extrusion width, as the slicer does (about 42° at 0.2 mm layers with a 0.4 mm nozzle). | Higher means more support. Use your printer's safe overhang angle. |
| **Max overhang reach** | 2.5 mm | 0.5–20 mm | How far an overhang may reach, added up over the layers, before it gets support. | **Smaller means more support**, larger means less. Below about 1.5 mm (default organic settings), a note in the report warns that painted areas may get too few branch tips. |
| **Max bridge span** | 25 mm | 5–200 mm | The longest bridge that prints without support. Longer bridges are only reported in this version. It also limits how wide the self-holding ring around a dome opening may be (half of this value). | Raise it if your printer bridges further; lower it if long bridges sag (bridges longer than the value are then reported). |
| **Paint** | Also where it may droop (recommended) | Where it would fail, Also where it may droop | Which needs get painted. *Would fail*: islands, and overhangs that reach more than 1.5 times Max overhang reach. *May droop*: the overhang is likely to droop or curl. | "Where it would fail" paints less, further out on each overhang. It lists the rest as "May droop, not painted (Paint: where it would fail)". |
| **Placement** | Prefer build plate | Prefer build plate, Build plate only | *Prefer build plate*: support stands on the plate wherever possible, and on the model only where a part would otherwise fail. *Build plate only*: never on the model; areas that cannot reach the plate are only reported. | Use "Build plate only" if you never want marks from branches on the model. Each choice needs a matching "Support on build plate only" setting ("Fix…" sets it). |

Open **Advanced** for the following:

| Field | Default | Range | What it does | When to change it |
|---|---|---|---|---|
| **Reach reset height** | 1.0 mm | 0–5 mm | How much plain material above an overhang stops it from passing its reach on. Think of stairs: if the step between two ledges is taller than this, each ledge starts fresh. If it is lower, the ledges add up like one long slope. 0 judges each layer alone; long shallow slopes then never get support. | Raise it for stepped undersides (layered hair, robes) that droop in test prints. Lower it if such steps get too much support. |
| **Max dome opening** | 25 mm | 0–200 mm | The largest opening (longest width) of a dome, funnel or counterbore ceiling that holds itself. It counts only for a ring around a single opening; slots and ceilings with several holes are judged as overhangs or bridges. | Lower it if large dome tops sag in your prints. 0 turns this exemption off. |
| **Reach counts below**: "the self-supporting angle" | checked | – | When checked, the Self-supporting angle is used. | Uncheck it to set an angle of its own. |
| Reach counts below slider | 45° (at most the Self-supporting angle) | 10° up to the Self-supporting angle | Slopes steeper than this do not add to the reach. | Lower it to tolerate long slopes that are just a little flatter than the Self-supporting angle, without changing the Self-supporting angle. |
| **Reset to defaults** | – | – | Resets **all** panel values to the defaults in these tables. | After an update from an earlier version: your stored values stay until you click it. |

### Why these defaults

The defaults are set for a **Prusa MK4S with a 0.4 mm nozzle**, with a safety margin. They are
starting values until test prints confirm them ([section 9](#the-defaults-are-starting-values)).
- **Self-supporting angle 45°**: the MK4S profiles set the slicer's "Overhang threshold" to 35°
  (SPEED) or 40° (STRUCTURAL, DETAIL), tuned
  for PrusaSlicer's own automatic supports. 45° is 10° safer than 35°. The panel therefore no
  longer takes the profile's "Overhang threshold". 0 = auto can still be chosen.
- **Max overhang reach 2.5 mm**: OrcaSlicer's 3 mm limit counts per layer. Here the reach adds up
  over the layers, so the default stays 0.5 mm below it.
- **Max bridge span 25 mm**: bridges measured on Prusa printers held up to 25 mm (the longest
  tested), and 28 mm worked on an MK3S. There is no measured maximum for the MK4S.
- **Max dome opening 25 mm**: the same as the bridge span. There is no data that ring-shaped
  ceilings hold further.

The panel keeps the values it stored before an update (for example the angle taken from your
profile in an earlier version, or 3.0 / 40 / 40 mm). Click **Reset to defaults** (under Advanced)
to use the defaults above.

### Buttons and lines below the fields

| What you see | Meaning |
|---|---|
| "! 2 support settings do not match" + "Fix…" | See [section 2](#2-before-you-start). |
| "! Instances differ in rotation: computed for instance 2" | The object has instances with different rotations; only one is analysed ([section 7](#7-working-with-your-own-paint-and-other-tools)). |
| **Analyse and paint** | Slices the object if needed, analyses its layers and paints enforcers where support is needed. It replaces earlier planned support and keeps your own paint. When the button is greyed out, hover over it to see why. |
| **Remove planned support** | Removes all planned support and keeps your own paint. The result and the report are cleared too. One undo step. Greyed out when the object has no planned support. |
| "Paint a blocker to keep an area free; erased planned support returns on the next run." | A reminder, see [section 7](#7-working-with-your-own-paint-and-other-tools). |
| "Slicing… 3 s" / "Analysing layers… 42 %" + **Cancel** | A run is in progress. **Cancel** stops it and leaves the paint unchanged. |
| "Painted 6 areas (islands 1, overhangs 5) on downward faces — press 2 for the bottom view." | The result of the last run. |
| "Nothing needs support with these settings." | Nothing was painted and there are no problems. The report may still have notes, for example "Other objects on the plate are not considered". |
| "Nothing was painted." | Nothing was painted, but the report lists problems. |
| "! 4 areas (steep slopes, hanging tips) cannot be supported while Top interface layers > 0" + "Fix…" | See [section 2](#2-before-you-start). Like the result line ("Painted …", "Nothing …"), it is hidden while the result is stale. |
| "Report: 2 problems, 5 notes" + **Show report** / **Hide report** | Opens or closes the report window. |
| "Result is stale: the paint changed — analyse again." | The paint changed after the run (your painting, undo, another tool). The report may no longer match. If the settings changed as well, only this line is shown. |
| "Result is stale: settings changed — analyse again." | A panel value, a relevant print setting, or the object's rotation, scale or parts changed. Moving the object on the bed does not count. |
| "Planned support present — analyse again for the report." | The object has planned support but no result in this session: from a loaded project, a copied object, or an undone "Remove planned support". The report is not saved; analyse again to get it. |

### Messages after a failed or discarded run

| Message | What to do |
|---|---|
| "The support planner needs a valid print setup." followed by the slicer's reason | Fix the reason the slicer gives, then analyse again. |
| "The object has no printable instance." | Make at least one instance printable. |
| "The object is not part of the print (is it outside of the bed?)." | Move the object onto the bed. |
| "Slicing failed, see the slicing notification. Fix it and analyse again." | Read the slicer's notification. |
| "Slicing was interrupted (the object or the settings changed). Analyse again." | Analyse again. |
| "Timed out while waiting for the slices." | Slicing took more than 15 minutes; analyse again. |
| "The object or its paint changed during the analysis; the result was discarded. Analyse again." | Don't edit the object while the analysis runs. |
| "The settings or the object's placement changed during the analysis; the result was discarded. Analyse again." | Same. |
| "Another background task is running." | Wait until the other task has finished, then analyse again. |
| Any other text | An error occurred; PrusaSlicer may also show it in a separate message. Analyse again. |

When "Analyse and paint" is greyed out, its tooltip gives the reason:
- "The support planner is running.";
- "Another background task is running.";
- "Automatic painting is still running.";
- "The object has no printable instance.".

## 5. Colours on the model

| Colour | Meaning |
|---|---|
| Blue | your own enforcer |
| Red | your own blocker |
| **Amber** | planned support for an **island** (also where an island and an overhang overlap) |
| **Cyan** | planned support for an **overhang** |

- Planned support only goes on faces that point downwards. Press **2** for the bottom view.
- The slicer treats planned support exactly like your blue enforcers.
- Stock PrusaSlicer shows planned support as plain blue enforcers ([section 8](#8-saving-projects)).

## 6. The report window

Click **"Show report"**. The window **"Support plan"** opens to the right of the
Paint-on supports window. It lists everything the run could not handle as planned, in two groups:

- **Problems (N)**, open by default: areas that will probably print badly or not at all.
- **Notes (N)**, collapsed: information you may want to check.

### How to read a row

- **Coloured dot**: how serious the need is. Red = would fail, orange = may droop, grey = possible
  or a general note.
- **Text**: what happened (the tables below).
- **"Z 22.6 mm"**: the height where the area starts. General notes have no height.
- **Tooltip** (hover over a row):
  - the layer(s) and height;
  - the position on the bed ("Bed X … mm, Y … mm");
  - the area;
  - how many nearby areas were merged into this row;
  - "Cause: island / overhang / long bridge — would fail / may droop / possible";
  - often a hint on what you can do.
- Each table shows up to 10 rows and scrolls. Each group lists at most 50 rows; the rest are
  counted as "+N more".
- The two notes about settings ("A raft counts as manual support", "Max overhang reach is below
  half …") appear as plain text at the top of Notes, without a dot, height or tooltip.

### Marker and "Clip at this height"

- **Click a row** with a height to mark it in the 3D view: a yellow ring with a short line pointing
  down. The marker is drawn through the model, so it stays visible from any side. Its size follows
  the area. Click the row again, or close the report, to remove it.
- **"Clip at this height"** hides everything above the marker. You then see the cut through the
  object at the height where the problem starts.
  - To see the whole object again, drag the "Clipping of view" slider in the Paint-on supports
    window back to 0.
  - The button works for a selected row that has a height. If it stays greyed out, an instance
    with a different rotation is selected; the tooltip says "Computed for instance N: select it
    to see the marker."

### Problems

| Entry | Meaning | What you can do |
|---|---|---|
| "Cannot be supported from the plate" | Needs support, but no position works. The tooltip says why: "No support branch can reach this area." / "Painting here does not support this area any better." / "Too close to the bed for tree support." / the interface-layer gap. Areas that could only be supported from the model are listed here too: those that may droop always, those that would fail with "Build plate only". | Interface gap: use the "Fix…" line in the panel. Otherwise: choose "Prefer build plate", paint an enforcer yourself, or rotate the model. |
| "Kept free by your blocker" (islands) | An island lies under your blocker, so it will print in mid-air. | Shrink or remove your blocker there. |
| "Your paint here will be dropped by the slicer" | Support from **your own** enforcer cannot be built here, so the slicer removes it without telling you. The tooltip says why: no branch can reach the area, it is too close to the bed, or the interface-layer gap. | Move or remove that paint. With "Build plate only", "Prefer build plate" lets branches stand on the model. Interface gap: use the "Fix…" line in the panel. |
| "Paint does not fully cover this area" | A check after painting found a part that the actual paint does not support. | Paint an enforcer there yourself, and check the preview. |
| "Support on the model (no plate position)" | An area that would fail, with no position on the plate. Its support stands on the model. | Accept the marks, or rotate the model. |
| "Support on the model may be removed (branch < 1 mm)" | The branch on the model would be shorter than 1 mm. The slicer deletes such branches silently. | Check the preview; paint by hand or rotate the model if support is missing there. |
| "Plate support may merge onto the model" | A branch from the plate passes close to a branch on the model and may join it, ending on the model. | Check the preview. |

### Notes

| Entry | Meaning | What you can do |
|---|---|---|
| "Some tips of this area stand on the model" | Part of the painted area gets its tips on the model. | Check the preview; usually fine. |
| "Kept free by your blocker" (overhangs) | An overhang under your blocker stays unsupported, as you chose. | Nothing, or shrink the blocker. |
| "May droop, not painted (Paint: where it would fail)" | Areas that may droop and were not painted because Paint is "Where it would fail". | Choose "Also where it may droop" to paint them too. |
| "Long bridge, not painted in this version" | A bridge longer than Max bridge span. | Paint an enforcer strip across the bridge yourself, or raise Max bridge span if your printer can bridge it. |
| "Islands not painted (unchecked)" / "Overhangs not painted (unchecked)" | Areas that would be painted if the cause were checked. | Check the cause under Support for. |
| "Painted areas rely on support that was not painted (unchecked causes, long bridges)" | The painted areas were computed on the assumption that the unchecked causes and long bridges get support. | Check the cause, or support the long bridges yourself. |
| "Thin hanging part without downward faces" | A thin hanging part (needle, fin) with no downward faces that could be painted. | Check the preview; if it needs support, add it yourself, for example with a support enforcer volume. |
| "Rests on an island that cannot be supported" | The material that grows from an island listed under Problems. It is listed once here instead of as many new islands; other islands on the same part are painted as usual. | Solve the island's problem; this area then gets support too. |
| "Computed for instance N only" | Instances of this object are rotated differently; only one was analysed. | See [section 7](#7-working-with-your-own-paint-and-other-tools). |
| "Other objects on the plate are not considered" | The analysis looked at this object alone. | Check the preview. |
| "A raft counts as manual support" | You print on a raft. The raft counts as support you chose yourself. | Nothing. |
| "Max overhang reach is below half the support tip spacing; painted areas may get too few tips" | Max overhang reach is so small that the slicer's branch tips may be too far apart for it. | Raise Max overhang reach. |

## 7. Working with your own paint and other tools

### Your own paint always wins

- **Enforcers** you paint are kept. The support planner counts them as support, so it paints
  less around them. A good routine: first paint the spots you know need support, then run the
  support planner.
- **Blockers** you paint keep an area free. The support planner paints nothing inside them and
  keeps a small gap around them, because organic tips spread a little around the paint.
- Support enforcer and blocker **volumes** (modifiers) are taken into account in the same way.
- Painting over planned support by hand (brush, Smart fill, "Enforce" by angle) turns it into your
  own paint.

### Keep an area free: use a blocker, not the eraser

Erasing planned support (Shift + left mouse button) removes it only **until the next run**: the
next run paints it again. To keep an area free for good, paint a **blocker** there.

### Running again

- A run **replaces all planned support** with the paint of the causes that are checked now.
  Unchecking "Overhangs" and running again removes the overhang paint and leaves the island paint
  as it was.
- Running again with the same settings gives the same paint.
- **"Remove planned support"** removes all planned support and keeps your own paint.

### Tools that erase everything

- **"Automatic painting"** erases **all** paint, your own and the planned support, after asking. The
  planner's result is then shown as stale. Automatic painting and the support planner cannot run
  at the same time.
- **"Remove all selection"** also removes all paint, including planned support.

### Undo and redo

| Action | Undo step |
|---|---|
| a run that changed the paint | "Planned support" |
| "Remove planned support" | "Remove planned support" |
| "Apply" in a "Fix…" dialog | "Support planner settings" |

The paint after an undo is exactly the undone state. The panel then says:
- after undoing a run: "Result is stale: the paint changed — analyse again.";
- after undoing "Remove planned support": "Planned support present — analyse again for the
  report." (the report is not restored);
- after undoing "Apply": the old settings are back, and the panel shows "Result is stale:
  settings changed — analyse again." if they differ from those of the last run.

### While a run is in progress

- Clicks on the object do not paint. You can still turn and zoom the view: drag beside the
  object.
- "Automatic painting", "Remove all selection", the angle "Enforce" and "Fix…" are greyed out.
- Closing the tool, selecting another object, or loading a project cancels the run.

### Changes to the model or the settings

- The paint stays where it is when you change something. The panel shows a stale hint, and you
  decide when to analyse again.
- Moving the object on the bed does not matter. Rotating or scaling it, changing layer height,
  extrusion widths or support settings does: analyse again.
- Edits that change the mesh, such as Cut or Split, remove the paint, the same as for your own
  paint. Analyse again afterwards.
- A copied and pasted object keeps its paint. Its report is not copied, so it shows "Planned
  support present — analyse again for the report."

### Instances and beds

- The paint belongs to the object, so **all instances carry the same paint**.
- The analysis uses the **selected instance**. If it is not printable, the first printable one is
  used.
- Instances with the same rotation and scale share the result.
- Instances rotated (or scaled, mirrored) differently:
  - only one of them is analysed. The panel says "! Instances differ in rotation: computed for
    instance 2" before the run, and the report has the note "Computed for instance N only";
  - check the others in the preview;
  - if they need different support, use separate objects (copy and paste) instead of instances,
    and run the support planner on each.
- With background processing off, the slicer may work on another instance first. The analysis then
  follows that instance and says so in the same note.
- If the object stands on another bed, that bed becomes the active one for the run.

## 8. Saving projects

| | In this fork | In stock PrusaSlicer 2.9.x |
|---|---|---|
| Opening a 3MF saved by this fork | Planned support keeps its colours and stays planned support (a run replaces it). The panel shows "Planned support present — analyse again for the report." | Planned support appears as plain blue enforcers. The slicer builds the same support. |
| Saving the 3MF again | Planned support is kept as planned support. | Planned support becomes plain enforcers for good. Back in this fork, it is then your own (blue) paint, and a run no longer replaces it. |

- The report and the panel values are **not** saved in the project.

## 9. Limits and tips

### The defaults are starting values

The defaults are chosen for a Prusa MK4S with a 0.4 mm nozzle, with a safety margin
([Why these defaults](#why-these-defaults)), but they have not yet been confirmed with test prints.
This applies to the Self-supporting angle, Max overhang reach, Max bridge span, Max dome opening,
Reach reset height, and the fixed internal values (tolerated bumps, thin tips, the "would fail"
level at 1.5 times the reach). Print a test piece with your printer and filament, then adjust:
- overhang edges droop or curl: lower **Max overhang reach**, or choose "Also where it may droop";
- too much support on long, even slopes: raise **Max overhang reach**; if these slopes are only a
  little flatter than the Self-supporting angle, lower **Reach counts below** instead;
- dome tops sag: lower **Max dome opening**;
- bridges sag: lower **Max bridge span**, and support the reported long bridges yourself.

### What is not covered in this version

- **Long bridges** are reported, not painted.
- **Closed cavities**: hollow spaces with no opening are not detected. With "Prefer build plate",
  support for areas that would fail may end up inside a closed cavity, where it can never be
  removed. Check the preview of hollow models.
- **Stability** of the part: thin necks that may break and tall parts that may tip over are not
  checked.
- **Other objects** on the plate are ignored (the report says so).
- **Faces that do not point downwards** cannot be painted, for example vertical needles and fins
  (reported as "Thin hanging part without downward faces").
- The support planner **predicts** what organic support will build. Always check the sliced preview
  before printing.

### When the result looks wrong

1. Check the panel for the warning line "… support settings do not match" and use "Fix…".
2. Open the report and click the problem rows to see where they are.
3. Slice and look at the **Preview**: where do the branches stand, what is missing?
4. Too much support:
   - choose "Where it would fail";
   - raise Max overhang reach;
   - check the Self-supporting angle;
   - put blockers on areas you want free.
5. Too little support:
   - lower Max overhang reach;
   - choose "Also where it may droop";
   - paint enforcers yourself where needed;
   - check the after-run "Top interface layers" line.
6. Support on the model you do not want: choose "Build plate only" and read what the report lists
   instead, or rotate the model.

### Speed and cancelling

- First the object is sliced (up to the layer slices only), then it is analysed. The analysis
  usually takes 1–3 s for a model with about 1000 layers, and a few seconds for very large meshes.
- A progress notification "Analysing layers for support" appears as well.
- **Cancel** works at any time, while slicing and while analysing. Nothing is changed then.
