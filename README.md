# AI Pathing Freeze Fix

Version 1.0

AI Pathing Freeze Fix is a lightweight SKSE plugin that protects against one specific Skyrim hard freeze related to Actor AI pathing.

This project does not attempt to be a universal freeze fix. It addresses one native engine failure path that was reproduced, traced, and verified during testing.

# License

AI Pathing Freeze Fix is licensed under the GNU General Public License v3.0 or later.

See LICENSE for details.

# Summary

In the freeze investigated for this project, an internal AI calculation can return `false` without replacing its output data.

The caller may then continue using the stale output.

In the failing cases captured during runtime testing, that stale output contained non finite floating point values such as `NaN`.

The invalid value was later consumed by an angle normalization routine. Because `NaN` does not behave like a normal floating point number during comparisons, the affected worker thread could become trapped in a loop that never converges.

The background task therefore never completes.

The main thread continues waiting for that task.

The visible result is a complete game freeze rather than a normal crash.


# The Failure Chain

The verified failure path is:

```text
Upstream AI calculation
        ↓
Returns false
        ↓
Output is not replaced
        ↓
Caller does not reject the failed output
        ↓
Stale output is consumed
        ↓
X or Y contains a non finite value
        ↓
NaN reaches angle normalization
        ↓
Worker thread becomes trapped in the floating point loop
        ↓
Background task never completes
        ↓
Main thread continues waiting
        ↓
Game hard freezes
```

An important detail is that not every `false` return is dangerous.

Runtime probes also captured failed calls where the output remained finite. Those cases did not need correction.

For that reason, AI Pathing Freeze Fix does not clear every result when the original function returns `false`.


# What the Fix Changes

The hook first calls the original game function.

If the original function succeeds, the result is left untouched.

If the original function returns `false`, the plugin checks the X and Y components of the output that Skyrim is about to continue using.

If both values are finite, the result is left untouched.

If X or Y is `NaN` or infinity, the plugin replaces the output with a finite fallback value of `[0,0,0]`.

The original Boolean return value is preserved.

The patch does not force a failed calculation to become a successful one.

The purpose of the replacement is only to prevent invalid stale floating point data from entering the verified infinite loop.


# Why the Fix Is Applied at This Point

I did not deliberately continue tracing where the original `NaN` value was first written.

That is a separate question from the failure fixed here.

Finding the first writer could require tracing additional AI state, object lifetime, cached data, pathing state, or other engine systems and could easily become another independent investigation.

For this project, the dangerous point was already established.

The upstream calculation can fail.

The failed output can remain unchanged.

The caller can continue consuming that output.

If the stale value is non finite, the following angle calculation can become trapped indefinitely.

This gives the patch a narrow and verifiable interception point.

The plugin therefore fixes the state immediately before it becomes dangerous instead of making assumptions about which earlier engine component or third party mod originally introduced the stale value.


# What Is Known

The worker thread can become trapped in a native floating point loop.

The value processed during the captured freeze was `NaN`.

The preceding function can return `false` without replacing its output.

The caller continues into the following angle calculation without rejecting that failed output.

Failed calls with finite output also exist and do not need correction.

Replacing only failed outputs containing non finite X or Y values prevents the captured failure path from reaching the infinite loop.


# What Is Not Known

The first writer of the stale `NaN` value has not been identified.

The upstream function is not proven to generate the `NaN` itself.

A specific AI package, actor, NavMesh record, animation system, skeleton system, physics system, or third party mod has not been identified as the original producer of the stale value.

This project intentionally does not make those claims.


# How the Problem Was Found

This plugin began with a hard freeze in my own Skyrim setup.

The game did not crash normally and did not produce a useful crash log. The entire process stopped responding and had to be terminated manually.

I initially treated it like a normal complex mod list conflict and used repeated binary style testing to narrow the problem down.

Some mods clearly changed the reproduction rate, but that did not establish them as the cause.

The first major discriminator came from Skyrim's AI console commands.

At a location where the freeze could be reproduced reliably, disabling Actor AI with `tai` prevented the freeze.

After waiting with AI disabled, enabling Actor AI again with `tai` while the player remained standing still could trigger the freeze immediately.

Disabling combat AI with `tcai` did not prevent the issue.

Disabling detection with `tdetect` did not prevent the issue.

The player did not need to move or cross a cell boundary.

Those tests narrowed the investigation toward Actor AI workload itself.


# Worker Thread Investigation

[**Worker Spin Lock Fix**](https://www.nexusmods.com/skyrimspecialedition/mods/180884) and its standalone diagnostic tools were extremely helpful during this stage of the investigation.

The diagnostic information made it much easier to inspect the native thread state during the freeze and helped narrow the search toward Skyrim's worker tasks.

Worker Spin Lock Fix was used as a diagnostic aid.

AI Pathing Freeze Fix does not copy, incorporate, or derive its repair logic from Worker Spin Lock Fix.

The actual repair used by this project was developed after further tracing with custom probes created specifically for this freeze.


# Using SkyTEST to Amplify the Problem

Once Actor AI became the main focus, I installed **SkyTEST Realistic Animals Lite ESL** to deliberately increase animal AI activity in my local testing environment.

This dramatically increased the reproduction rate.

In some test locations, loading a save and standing still for only a few seconds could be enough to reproduce the freeze.

That was extremely useful because an intermittent problem had become frequent enough to inspect repeatedly with custom native probes.

**Important:** I only used SkyTEST Realistic Animals Lite ESL to amplify the issue in my local test environment and make it easier to reproduce.

It is an excellent mod and is unrelated to the root cause of the freeze investigated here.

Its additional animal AI activity simply made the underlying problem occur much more frequently in my setup, which made it a very useful debugging tool.

The same bad execution path was also captured without SkyTEST enabled, only at a lower frequency.


# Custom Probe Investigation

After obtaining a reliable reproduction environment, I created several small SKSE probes to inspect the affected worker thread and the native data flow around the freeze.

The worker was eventually isolated to an angle normalization routine containing the following loop pattern:

```text
addss xmm0,xmm6
comiss xmm0,xmm7
jb ...
```

In the captured freeze:

```text
XMM0 = NaN
XMM6 = approximately 2π
XMM7 = 0
```

Adding a finite value to `NaN` still produces `NaN`.

The comparison against zero therefore never behaves like a normal finite angle comparison.

The loop cannot converge in the intended way and the worker can remain there indefinitely.


# Tracing the Value Backward

Further probes moved one call earlier in the data flow.

The important discovery was that the value was already invalid before the angle routine consumed it.

The upstream function was called with an output buffer.

In the failing cases, that function returned `false` and left the output unchanged.

The caller then continued and passed that same output into the angle routine.

Captured failing output included a stale pattern whose first component decoded as `NaN`.

Additional testing also captured benign `false` results with finite output.

That distinction became the basis of the final repair condition.


# Repair Condition

The release version repairs only this state:

```text
Original returned false
AND
output is valid to access
AND
X or Y is non finite
```

Everything else is left unchanged.

This means successful calculations are untouched.

Failed calculations with finite X and Y are untouched.

The original return value remains `false`.

The plugin does not attempt to reinterpret the original AI calculation or invent a successful path.


# Runtime Location Strategy

The release build does not hardcode the vulnerable caller address for one Skyrim executable.

The known angle routine is resolved through CommonLibSSE NG and Address Library using the SE and AE relocation pair:

```text
SE relocation ID 68820
AE relocation ID 70172
```

The plugin then scans Skyrim's executable text segment for direct calls to that resolved angle routine and checks the surrounding machine code structure.

The expected structure includes the same local output being passed through the preceding producer call and then into the angle calculation, together with the surrounding `movss` relationship observed during the investigation.

The angle routine itself is also validated for the known dangerous loop byte pattern.

Installation is allowed only when exactly one matching vulnerable call site is found.

If validation fails, if no match is found, or if more than one match is found, the plugin refuses to arm instead of guessing.


# Reference Values From AE 1.6.1170

These addresses are included only as investigation references.

They are not hardcoded as the release location strategy.

During testing on Skyrim AE 1.6.1170, the release scanner resolved:

```text
Vulnerable producer callsite
SkyrimSE.exe + 0x11B0CEA

Angle routine
SkyrimSE.exe + 0xD17400
```

The release build dynamically rediscovered the same locations at startup.


# Runtime Validation

A successful AE 1.6.1170 startup produced:

```text
AI Pathing Freeze Fix v1.0 loading runtime=1-6-1170-0 kind=AE.
AI Pathing Freeze Fix v1.0 installed runtime=1-6-1170-0 kind=AE callsite=SkyrimSE.exe+0x11B0CEA angle=SkyrimSE.exe+0xD17400.
```

With SkyTEST Realistic Animals Lite ESL enabled, the release build immediately began intercepting invalid failed outputs during normal AI activity.

The repair path also triggered without SkyTEST enabled during earlier testing, but at a lower frequency.


# Testing History

The original prototype was tested repeatedly on the known reproduction route.

Five deterministic reproduction rounds completed without the original hard freeze.

Three rounds were performed with SkyTEST Realistic Animals Lite ESL enabled.

Two rounds were performed without SkyTEST Realistic Animals Lite ESL.

A later normal play session ran for roughly one hour and intercepted multiple occurrences of the same invalid stale output pattern without reproducing the original freeze.

The final version 1.0 runtime locator was then validated on AE 1.6.1170 and rediscovered the exact callsite and angle routine previously identified through direct probing.


# Logging

The runtime log is written to:

```text
Documents\My Games\Skyrim Special Edition\SKSE\TMS_AIPathingFreezeFix.log
```

The first three repairs are written individually as warnings.

Example:

```text
AI FREEZE FIX repair=1 replacement=[0,0,0]
AI FREEZE FIX repair=2 replacement=[0,0,0]
AI FREEZE FIX repair=3 replacement=[0,0,0]
```

Later repairs are counted silently.

At most one summary is written every 60 seconds, and only when additional repairs have occurred.

Example:

```text
AI FREEZE FIX summary totalRepairs=24 repairsLastInterval=13
```

This keeps the log useful even in environments where the invalid state occurs frequently.


# Compatibility

The plugin is built with CommonLibSSE NG and Address Library.

The runtime location strategy is designed for SE and AE rather than a single hardcoded executable address.

AE 1.6.1170 is the runtime used for direct validation of version 1.0.

SE support is implemented through the CommonLibSSE NG, but SE 1.5.97 has not yet been personally runtime tested for this release.

No ESP is required.

No Papyrus scripts are required.


# Scope and Non Goals

AI Pathing Freeze Fix does not modify AI Packages.

It does not modify NavMesh records.

It does not modify animations.

It does not modify skeletons.

It does not modify actor records.

It does not disable Actor AI.

It does not restart Actor AI.

It does not globally patch Skyrim's task pool.

It does not globally replace the angle routine.

It does not attempt to fix every source of `NaN` in the engine.

It does not attempt to fix unrelated deadlocks, script problems, physics problems, corrupted assets, or other engine freezes.


# Performance

The normal hook path calls the original function and returns immediately when the calculation succeeds.

When the calculation fails, the plugin checks the relevant output values using an IEEE 754 bit test.

Only the verified dangerous state causes a replacement.

Repair statistics use atomic counters.

Detailed logging is deliberately limited so environments with frequent animal AI activity do not produce an excessively noisy log.


# Special Thanks

Special thanks to [**Worker Spin Lock Fix**](https://www.nexusmods.com/skyrimspecialedition/mods/180884) and its diagnostic tools.

They helped make the native worker state visible much earlier in the investigation and significantly reduced the time required to narrow down the freeze.

Special thanks to **SkyTEST Realistic Animals Lite ESL**.

It provided an extremely effective way to amplify animal AI workload in my local environment and made an intermittent freeze reproducible enough to investigate with custom probes.

SkyTEST is an excellent mod and is unrelated to the root cause of the freeze investigated here.


# Final Note

Skyrim can hard freeze for many completely unrelated reasons.

AI Pathing Freeze Fix protects only against the specific failed output and non finite floating point path documented above.

If a freeze is caused by something else, this plugin may have no effect.

The goal of this project is not to hide every possible engine problem.

The goal is to intercept one verified dangerous state at the narrowest practical point and allow Skyrim to continue safely.
