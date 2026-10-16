> **Update:** `main` was rolled back to Build 35 (`6490315`) and then restored to the Build 40 code (`9e5339a`).
> The seven asset-pack bypass commits after Build 40 (`0a4c472` to `b66bcd6`) are not in the code, but they are
> still in git history.

# Handoff: Las Tools / Red Dead Redemption stuck on a black screen

## The goal

Las Tools (GitHub `Freecranegami/Las-Tools`, local clone `/home/claude/las-tools`, branch `main`, Android app labelled "AdrenoTest", package `com.freecrane.adrenotest`) runs Android games in a separate `:guest` process. It loads a custom Turnip Vulkan driver through libadrenotools (no root) so games that need newer Vulkan can run.

The user's phone is a POCO on Android 13 (API 33). Its stock Adreno driver exposes only Vulkan 1.1.

Immediate target: **Red Dead Redemption mobile**.
- Real package: `com.rockstargames.rdr`. It is imported under the name `com.netflix.NGP.Kamo`.
- It is a base APK plus 6 split APKs: data0, data1, game0, game1, game2, game3.
- It needs Vulkan 1.2.
- The user says it runs offline when installed normally on other phones.

How the user and I work together:
1. I push a build to GitHub Actions, which publishes the APK to the "latest" pre-release.
2. The user installs it over the existing app. The game does not need reinstalling.
3. The user launches the game, then uses the in-app Guest log, "Copy log", and pastes it.
4. The user closes the game themselves after a long black screen. A `SIGTRAP` at the very end of a log is just that shutdown, not a crash.

Rules I keep to:
- Do not bypass Google Play licence or PairIP checks.
- Do not fake Play's asset-delivery service in a way that grants entitlement.
- Allowed: honest identity fixes for the guest, and making the guest see no Play Store service to bind (equivalent to an offline sideload).
- Do not send `make-rdr-run.sh` (the user said to stop).
- Always `git pull` before editing, because another session also pushes to this repo.

## Current state

**Latest commit on `main`: `9e5339a` (Build 40).** It is pushed, but CI has not been confirmed green and the user has not yet tested it.

The game launches, the guest activity is created, and the game's native code runs. The screen stays black and nothing crashes. Audio keeps ticking in the log, so the game is alive.

**Where it is stuck (Build 39 log, the key finding):**
- The game's startup thread (`Thread-10`) loops in sleep-and-check. The call chain is `AndroidAssetPackFileSystem_c` → `MountManager_c::DoesFileExist` → `fiPackfile::Init` → `sagApplicationDerived::MountArchive` → `sagShellDerived::Init`.
- In plain words, the game tries to open its data archives, asks the asset-pack layer whether the files are available, and waits for an answer that never arrives.
- Every other game thread is idle, waiting for work. The game never reaches graphics setup, so the Vulkan driver is not the current blocker.
- `libvulkan.so` and the Adreno GL libraries are mapped, but that is only loading, not use.

**What Play Core says without Play (consistent across builds):**
- `getPackLocation` returns null for every one of the six pack names.
- `getPackStates` fails with "Failed to bind to the service".
- The game polls Play roughly every 8 seconds and later calls `startDownload([data1])`.
- Play Core logs "Failed to cast exception to ApiException".

**What is not known yet:** how a normal install makes Play Core report the split APKs as available. The splits are on disk and every asset in them is readable through the game's AssetManager. Build 40's log should show whether `getPackLocations()` and `getAssetLocation(...)` return anything.

**Already solved (inferred from logs):**
- The game activity is created with the game's own identity.
- `s_activity` and `s_context` are seeded.
- The Play Core `getInstance` exception from the `setComponentEnabledSetting` calls is answered.
- The GameManager permission error is fixed.
- FindClass works from native threads the game creates.
- The `GetObjectClass(NULL)` abort is gone.
- The PairIP null-pointer error seen in builds up to about 18/27 has not appeared in recent logs. Whether PairIP is actually passed has not been proven.

## Active files

Kotlin is in `app/src/main/kotlin/com/freecrane/adrenotest/`. Native code is in `app/src/main/cpp/`.

| File | Role |
|---|---|
| `cpp/probe.c` (builds `libprobe.so`) | Vulkan probe code, the FindClass hook (`installFindClassHook`), the JNI diagnostic hooks (now `#if 0`'d out), and the **thread snapshot** (`Native.snapshot`). The snapshot logs graphics libraries from `/proc/self/maps`, then each non-idle thread's state, wait point and native stack, using a `SIGRTMIN+4` handler with `_Unwind_Backtrace` and `dladdr`. |
| `MainActivity.kt` | `object Native` declares the external functions, including `installFindClassHook` and `snapshot`. |
| `GuestRuntime.kt` | `prepare()` logs splits and assets, installs the FindClass hook, starts the snapshot thread (10 s and 25 s after launch), and exposes `splitNames`. |
| `GuestInspect.kt` | `dump`, `probePlay`, `reflectPlay`, and `packReport` (per-pack `getPackLocation`, `getPackStates`, and as of Build 40 `getPackLocations` and `getAssetLocation`). |
| `GuestAssetPacks.kt` | `wrap(real)` class dump for Play Core asset-pack classes. It currently only dumps and is not wired to change answers. |
| `GuestPm.kt` | `IPackageManager` proxy for the guest, including `blocksPlay(intent)`, `getPackageInfo` with `splitNames`, and component-enabled-setting answers. |
| `GuestContext.kt` | Wraps the game's context, including `bindService` refusal for Play, GameManager from the host app, and static-field seeding. |
| `HostApp.kt` | Guest-process application class, lifecycle callbacks, and host `PackageManager` hook. Line ~63 in `onActivityPreCreated` calls `GuestInspect.dump`, `probePlay`, `reflectPlay`, `packReport`. |
| `GuestLog.kt` | The log viewer. It filters on "important" and "drop" tokens and trims long logs to the first 3000 and last 11000 characters. `snap[` is in the important list. |

Untouched and not likely relevant: `GuestLoader.kt`, `GuestNativeActivity.kt`, `GuestActivity.kt`, `Importer.kt`, `Util.kt`, `GuestFactory.kt`, `GuestProxyActivity.kt`, `HiddenApi.kt`, the manifest and `build.gradle.kts` (compileSdk 34, minSdk 26, targetSdk 28, versionCode 3 / "0.3").

## Failed attempts

- **Misdiagnoses I corrected:**
  - I said PairIP was passed. That is unproven.
  - I said the game gives up and kills itself. In fact the user closes it, which causes the `SIGTRAP`.
  - I said data1 had to be delivered by Play. The user's offline all-splits install contradicts that.
- **JNI diagnostic hooks (Builds 32 and 35):** they seemed to produce a greyscale gradient, then crashed in framework code (GC `SIGSEGV` in Build 32, `ArrayMap.get` null dereference in Build 35). The cause was a bug of mine: `after_obj()` in `probe.c` had no `return r;`, so every hooked object-returning JNI call returned garbage. Fixed in Build 37. With the fix, the screen is plain black with no crash, so the gradient was a side effect of the bug and not progress.
- **Bisecting the hooks (Build 36):** with only the ID-lookup hooks on, the screen was black. This was consistent with the hooks making no difference.
- **Refusing the Play bind versus letting it connect:** neither changes the black screen. The game keeps polling Play and sits there.
- **Earlier build errors, all fixed:**
  - a short-SHA wait bug in the CI wait step
  - a Kotlin class-literal error in `HiddenApi.kt`
  - a stray `()` in `HostApp.kt`
  - a duplicate `@Volatile` on `splitNames`
  - Build 34's first push forgot to call `packReport` because the other session had changed `HostApp.kt` (fixed in `36ad9d4`)

## Changes made (recent, newest first)

- **Build 40 (`9e5339a`):** native symbol names in the snapshot widened from 40 to 110 characters, stack depth 22 frames, buffer 2400 bytes. `packReport` now also logs `getPackLocations()` and `getAssetLocation(pack, asset)` for data0/data1/game3.
- **Build 39 (`11f2d2b`):** snapshot omits idle helper threads (job workers, streaming, audio, network, Looper) so the log survives the viewer's trimming.
- **Build 38 (`b438762`):** added `Native.snapshot`, started from `GuestRuntime.prepare` at 10 s and 25 s. Turned the JNI object hooks off with `#if 0`. Added the `snap[` token to `GuestLog`.
- **Build 37 (`53e62ec`):** added the missing `return r;` in `after_obj`, hooks all on (superseded by Build 38).
- **Build 36 (`3c0a86a`):** only the ID-lookup hooks on, to bisect.
- **Build 35 (`1920cd5`) and earlier:** FindClass fallback for native threads, `packReport`, the GameManager fix, `setComponentEnabledSetting` handling, and `bindService` refusal for Play.
- `handoff.md` (this file) is new and not committed.

## Specific next steps

1. **Check CI for Build 40.** Confirm the GitHub Actions run for `9e5339a` is green and the APK is in the "latest" pre-release. Nothing has been compiled locally because there is no Android SDK here, so CI is the compile check.
2. **Get the user to test Build 40.** Install over the existing app, launch the game, wait about 30 seconds, then copy the Guest log and paste it. The lines to read are:
   - `pack report: getPackLocations = ...`
   - `pack report: assetLocation(...) = ...`
   - `snap[25s]: tid ... 'Thread-10'` with its full function names
3. **Decide the fix from those lines:**
   - If `getPackLocations` or `getAssetLocation` already return something non-null, find out why `getPackLocation` returns null for the same packs. Look at what Play Core's local check reads (`PackageInfo` split fields, `ApplicationInfo` metadata, SharedPreferences) and fix that guest identity input honestly.
   - If they all return null, work out what a normal install gives Play Core for packs that are installed as splits. A likely candidate is the install-time pack metadata that Play Core reads from the app's own manifest or `ApplicationInfo`. The decompiled `com.google.android.play.core.assetpacks.l` shows the real logic, but the game APK is not in this sandbox (only my own `adrenotest.apk` builds are), so it would need to be on the user's phone, or its dex pulled from a log or an attachment.
4. **Keep within the rules.** Reporting packs as present because their split APKs are genuinely on disk matches what a normal install does. Do not make Play Core report packs as delivered by Play, and do not touch the licence or PairIP checks.
5. **Once the game gets past the archive-mount loop,** expect the next blocker to be graphics. Look for `vkshim` or Vulkan lines in the log, and for `libvulkan.so` use beyond the loaded mapping. That is where the Turnip driver and the Vulkan 1.1 versus 1.2 problem will finally matter.
6. **Housekeeping:**
   - `git pull` before editing, since another session pushes here (it added `GuestInspect.reflectPlay`).
   - Commit messages end with `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01MogdN58uDnrucKZbjLvtGn`.
   - If the snapshot noise grows, remove the `Native.snapshot` thread in `GuestRuntime.prepare` once it has served its purpose.
