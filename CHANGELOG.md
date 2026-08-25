# MatterFlux Changelog

## [0.5.0] - 2026-08-25

MatterFlux 0.5.0 turns the fragment prototype into a one-click playable 2.5D
sample that works in Standalone and Listen Server modes.

### Added

- A replicated playable-world actor with stable soil, seeded multi-frequency
  Perlin terrain, floating platforms, a stream, trees, grass, two flower
  colors, collision, directional lighting, sky lighting, and sky atmosphere.
- A visible Character mesh, constrained 2.5D Character Movement, a slanted
  three-quarter spring-arm camera, runtime Enhanced Input mappings, jumping,
  camera pan/zoom, an owner-authorized break request, and server-authorized
  random-map regeneration.
- Deterministic layout and playable-default automation coverage.
- A one-page Chinese playable-demo guide for UE beginners.
- A fixed-seed, fixed-camera multi-frame capture command and a regional
  image-difference analyzer for repeatable render-stability checks.

### Changed

- The GameMode automatically creates the playable world on authority.
- Editor Play defaults to a one-player Listen Server, while Standalone
  remains available for the fastest offline run.
- Material clients now import versioned authoritative active-region
  snapshots instead of replaying fixed steps from an incomplete focus
  history, so late joins and missed updates self-correct.
- Streamed static fragment sources archive complete deterministic combustion
  state and unload even while burning or holding residue; returning to the
  chunk resumes the paused fire without resurrecting fuel.
- Project version metadata advances to `0.5.0`.
- Pixel-scale scene rendering now defaults to FXAA with camera motion blur
  disabled. Lumen and virtual shadows remain enabled, while the static sky
  light is captured once after procedural generation instead of every frame.
- Pristine non-colliding foliage, grass, and flower masks are rendered in
  deterministic chunk batches instead of one replicated Actor per source.
  Collision-bearing trunks remain Actors; cut, fire, combustion propagation,
  and detached-tree queries materialize the exact mask source on demand.
- Server world-cut commands now enter a bounded deterministic FIFO and execute
  under a per-frame budget. Simultaneous multiplayer casts keep their source
  transactions and results, without stacking every mask update into one frame.
- Damage materialization uses the existing fragment-source chunk index plus a
  conservative shape/source bounds query instead of testing every cached mask.
- Render-only debris materializes through a four-Actor-per-frame queue. Its
  initial payload keeps the source collision policy and omits unused convex
  collision contours.
- Material fixed-step scheduling, focus transitions, catch-up debt, logical
  steps, snapshot encoding and applied/rejected revisions now live behind
  `FSimulationRuntime`. The playable-world Actor retains only Unreal
  replication fields and presentation/lifecycle adaptation.
- Material visuals are partitioned by material and simulation chunk. Stable
  groups keep their ISM components and hashes; changed groups batch-update
  existing instances and only add or remove the tail instead of clearing a
  complete layer. Streaming-boundary refreshes are separated from snapshot
  compression and visual uploads by one stable-focus frame.
- Small generated worlds may prepare pristine decoration proxy chunks during
  loading under the configurable `FragmentSourceProxyCacheLimit`. Larger
  worlds keep the bounded on-demand path.
- Ground combustion now maintains sparse burning-cell and active-chunk
  indexes incrementally. Ground-to-logical-source propagation queries only
  active 64x64 combustion chunks through `FSourceSpatialIndex`, then retains
  the exact cell and height checks as its narrow phase.
- Multi-bound propagation now uses deterministic `FSourceSpatialIndex::QueryMany`
  deduplication, and visible ground fire/residue is gathered from sparse chunk
  sets instead of scanning all cells in the terrain window.
- Ground visible masks update only changed cells; the 512x384/8192-ignition
  regression improved from 25.15 ms to 0.72 ms.
- Logical Source entity meshes no longer retriangulate every combustion step.
  Final fuel/residue masks are merged back in 0.5-second chunk batches, while
  interactive cuts and materialization keep their immediate flush path.

### Fixed

- Temporal shimmer and ghosting on terrain steps, foliage, flowers, the
  player silhouette, and custom-depth outline. In the fixed 1280x720 seed-1337
  sequence, the strong temporal-change share in the character crop fell from
  1.01% to 0.30%, and the upper-ground crop fell from 2.98% to 0.70%.
- Non-colliding decoration sources and fragments still cooking procedural
  triangle/convex collision before disabling it. Only collision-enabled
  sources now create physics mesh data or start Chaos simulation.
- Periodic blank frames caused by clearing and rebuilding material, ground
  flame/smoke/residue, and streamed HISM instance arrays. Runtime refreshes
  now preserve the previous complete instance set until replacements are
  ready.
- First-time decoration proxy triangulation running synchronously on player
  chunk boundaries. The fixed forest now pays that bounded work during its
  loading phase; dirty chunks remain rebuildable after interaction.
- Ground fire propagation scanning every cached logical Source; batched spatial
  queries retain exact ground-cell and height narrow-phase checks.

### Verification

- `MatterFluxEditor Win64 Development` and `MatterFlux Win64 Development`
  build successfully with MSVC 14.44.35222.
- The focused ground-combustion slice passes 15/15 combustion tests, the
  Listen Host + Client PIE test, and the large-world performance gate. Its
  final 120-tick sample is 458.30 ms with 3 active chunk queries and 115/1,567
  logical-source candidates.
- `Automation RunTests MatterFlux` completes 148/148 tests with zero failures
  and zero tests not run (140 clean, 8 with expected rejection/PIE warnings).
- The final large-world gate records 0.94 ms maximum walk boundary Tick,
  1.95 ms sprint boundary Tick, and 1.63 ms high-speed boundary Tick; the
  fixed map builds in 1,760.19 ms.
- The 2–4 player near/far network scale matrix completes 6/6 scenarios. The
  four-player far case retains 113 peak fragments while reducing its observed
  worst frame from 867.28 ms to 326.53 ms.

## [0.4.0] - 2026-07-24 (development checkpoint)

MatterFlux 0.4.0 tightens the fragment materialization transaction and makes
the default playable map and fragment appearance part of the tested contract.

### Changed

- `AFragment2DActor::InitializeFromPayload` now reports whether visual mesh,
  simple collision, and physics state were initialized successfully.
- Fragment initialization completes for every candidate actor before the
  source mask and revision are committed.
- Source materials are copied to fragment actors and replicated
  `COND_InitialOnly`, with independent rep-notifies for geometry and material
  arrival order.
- `AFragment2DDamageRequestActor` no longer consumes a per-frame tick; editor
  property changes still use `PostEditChangeProperty`, while runtime and
  Blueprint callers invoke `ExecuteRequest` explicitly.
- Project version metadata advances to `0.4.0`.

### Fixed

- A zero-power damage event still assigning a random angular velocity.
- A valid payload that failed actor mesh/collision initialization committing
  the source damage transaction.
- Fragments falling back to the default material instead of keeping the
  source material on the server and clients.
- `/Game/Default` lacking a `PlayerStart`, which caused the staged game to
  fail spawning `MatterFluxCharacter` at the source actor's origin.
- Staged-game verification accepting missing-player-start and pawn-spawn
  collision warnings.

### Verification

- Added regression coverage for zero-power motion, post-spawn initialization
  rollback, source-material propagation, and the default map's `PlayerStart`.
- The dedicated-server/two-client PIE test now checks the replicated material
  property and the material actually applied to both client meshes.
- `Saved/Logs/ReleaseVerification/20260724-040-final/` records successful
  Editor and Game builds, 40/40 Automation tests, the dedicated-server/two-
  client PIE scenario, and `/Game/Default` Map Check with zero errors and zero
  warnings.
- The same run cooked 496 packages, staged Pak/IoStore output, launched the
  staged game, observed project version `0.4.0`, clean initialization and
  shutdown with exit code 0, and produced no PlayerStart/Pawn-spawn warning,
  crash output, residual process, or staged `ue.projectstore`.
- The Launcher UE 5.8 distribution still does not support the Win64 Server
  target, so this run explicitly skips that gate and remains partial/unsigned.

## [0.3.0] - 2026-07-24 (development checkpoint)

MatterFlux 0.3.0 adds a packaged-runtime verification gate and removes known
noise from the multiplayer regression fixture.

### Changed

- Fragment payload construction and reception now share a conservative initial
  replication budget for face vertices, triangle indices, contours, and
  contour vertices.
- Fragment actors use spatial relevancy and a finite 30-second default
  lifetime instead of remaining globally relevant forever.
- The multiplayer test grants its debug ability explicitly on the dedicated
  server; normal PlayerState and PlayerController defaults no longer expose
  the debug destruction path.
- Release verification now requires named geometry, transaction, replication
  budget, GAS-default, and multiplayer tests in addition to the total count.
- Release verification now cooks and stages a self-contained Win64
  Development build, launches the staged bootstrap executable, and requires a
  successful map load, clean engine shutdown, zero new crash directories, and
  no residual MatterFlux process.
- Cook/Stage output is isolated by `RunId` under
  `Saved/StagedBuilds/ReleaseVerification/`.
- The dedicated-server/two-client PIE test now tracks its dynamically spawned
  source by `SourceId`, explicitly selects the local PlayerController, waits
  for a locally controlled ASC ActorInfo, and declares intentional authority
  rejection plus transient-map NetGUID diagnostics as expected logs.
- Project version metadata advances to `0.3.0`.

### Fixed

- Damage state being committed before all fragment actors could be
  materialized. Candidate actors are now deferred-spawned before commit and
  cleaned up as a batch on failure.
- High-boundary masks producing initial-only payload arrays beyond the
  supported replication budget.
- Any owning client receiving and being able to activate the server-only
  fragment debug ability by default.
- Multiplayer tests requiring transient-map NetGUID warnings to occur; those
  warnings are now optional suppressions rather than success conditions.
- Release verification accepting any set of tests with the expected count
  even when a critical multiplayer or transaction test was missing.
- Bare UBT Game executable smoke tests hanging indefinitely when a stale
  `Saved/Cooked/Windows/ue.projectstore` points at an unavailable Zen server.
- Treating a bare Game executable plus incomplete cooked files as a valid
  packaged-runtime test, which could instead assert while loading default
  engine materials.
- `Start-Process`-based smoke checks losing the child exit code and reporting
  a false failure after a clean game shutdown.
- Expected transient PIE `FNetGUIDCache` and `WorldSettings` diagnostics being
  reported as unexplained multiplayer-test warnings.
- Multiplayer assertions depending on source iterator order instead of the
  specific `SourceId` spawned by the test.
- Multiplayer GAS activation using the first replicated PlayerController
  instead of explicitly selecting the client's local controller.

### Verification

- `Saved/Logs/ReleaseVerification/20260724-review-fix-final/` records successful
  Editor and Game builds, 36/36 Automation tests plus the required-test
  allowlist, dedicated-server/two-client PIE, and `/Game/Default` Map Check
  with zero errors and zero warnings.
- The same run cooked 496 packages, staged Pak/IoStore output, launched the
  staged game, observed clean initialization and shutdown with exit code 0,
  and left no crash output or residual process.
- `Saved/Logs/ReleaseVerification/20260724-030-cycle-final2/` records successful
  Editor and Game builds, 34/34 Automation tests, dedicated-server/two-client
  PIE, and `/Game/Default` Map Check with zero errors and zero warnings.
- The same run records UE 5.8 `BuildCookRun` cooking 496 packages, staging
  Pak/IoStore output, and exiting with code 0.
- Its staged `MatterFlux.exe` loaded `/Game/Default`, initialized the engine,
  exited with code 0, produced no new crash directory, left no process behind,
  and logged a clean `LogExit: Exiting.` marker.
- This Launcher engine still lacks a Win64 Server target, so the run is
  intentionally reported as partial/unsigned rather than a full release signoff.

## [0.2.0] - 2026-07-24 (development checkpoint)

MatterFlux 0.2.0 is a stability-focused minor release. It keeps the existing
fragment gameplay scope while tightening deterministic geometry, damage
transactions, replication, collision safety, and test infrastructure.

### Added

- Mask-to-contour/hole-to-constrained-triangulation geometry pipeline.
- Deterministic multi-contour fragment payloads and fragment GUID derivation.
- Editor-only `MatterFluxTests` module with fragment, GAS, project metadata,
  Forge integration, and dedicated-server/two-client PIE coverage.
- Beginner-oriented implementation guide in
  `Docs/MatterFlux_UE_Beginner_Guide.md`.
- Requirement-by-requirement release audit in
  `Docs/MatterFlux_0.2.0_Release_Audit.md`.
- One-command local release gate runner in
  `Scripts/Verify-MatterFluxRelease.ps1`.
- Project version metadata test for `0.2.0`.

### Changed

- Fragment payload replication is initial-only; replicated movement owns the
  actor transform after spawn.
- Damage processing is transactional and reports whether state was committed,
  including accepted breaks where all debris is filtered.
- Dynamic fragment collision uses validated convex contours; source collision
  uses its exact procedural triangle mesh.
- Network PIE verification now uses a separate 30-second timeout for startup,
  replication, and movement convergence instead of one global 20-second
  deadline.
- Geometry construction now invokes `FConstrainedDelaunay2d` directly so a
  triangulation failure and malformed output are rejected before commit.
- Bounded source and fragment actors stay network relevant so initial geometry,
  broken state, and physical movement cannot be lost to relevancy culling.
- The multiplayer PIE fixture now spawns its source on the dedicated server,
  then waits for both clients to receive the real network actor.
- Release verification now initializes the project AutoSDK and UBA root,
  limits compile concurrency for low-memory machines, and reports skipped
  build gates as partial verification rather than a signed release.
- Full release verification now checks the engine's installed-platform
  manifest before any build and immediately rejects distributions that do not
  advertise Win64 Development Server support. Source builds and Server-enabled
  Installed Builds continue to the real Server target build.
- The test module uses UE 5.8's `IWYUSupport.Full` API instead of the obsolete
  `bEnforceIWYU` property.

### Fixed

- Partial geometry/payload outputs escaping failed builds.
- Open boundary paths being accepted as closed contours.
- Locale-sensitive floating-point text in deterministic fragment signatures.
- Invalid or degenerate visual geometry leaving collision or physics enabled.
- Equal-area components with identical bounds inheriting caller input order
  instead of using a final geometry tie-break.
- Clockwise face triangles being accepted even though the generated front-face
  normals require counter-clockwise input.
- Malformed components below the debris-area threshold bypassing validation
  and being mistaken for normally filtered debris.
- Editor construction keeping a stale runtime mask when a fragment asset
  changed without changing its dimensions.
- Damage transactions allowing the signed revision counter to overflow.
- Fragment-count configuration bypassing the intended hard runtime budget of
  16 actors per break.
- Circle damage ignoring relative transform scale and cutting the wrong cells
  on scaled source actors.
- Valid-looking fragment geometry allowing non-finite transform, mass, or
  velocity state to reach collision and physics initialization.
- Direct non-authority damage calls reaching the source transaction.
- UnrealAngelScriptForge project-reference platform metadata differing from
  the plugin descriptor and prompting the editor to update the project.
- Duplicate Windows default-RHI configuration entry.
- Workspace-driven Forge tests blocking ordinary unattended regression runs
  for up to 120 seconds when no Forge Workspace was configured.
- Intentional transaction-rejection error logs causing the rollback Automation
  test to fail despite all state assertions passing.
- Hidden, collision-disabled broken sources becoming irrelevant before their
  revision and broken state reached clients.
- Fast-moving fragments closing client actor channels before both clients could
  compare the initial-only payload and converge movement.
- Source and fragment procedural meshes registering with the navigation system,
  causing empty-bounds warnings during construction and unnecessary dynamic
  navigation updates.
- Gameplay Ability System falling back to an implicit full `/Game` gameplay-cue
  scan because `GameplayCueNotifyPaths` was not explicitly configured.

### Verification

- A 2026-07-21 UBT run built `MatterFluxEditor` with MSVC 14.44.35222 and
  completed 95/95 actions.
- The 2026-07-21 Forge regression run completed 12/12 selected tests.
- On 2026-07-23, the changed runtime and test translation units compiled and
  linked directly with the project's MSVC 14.44 AutoSDK toolchain.
- `Saved/Logs/CodexMatterFlux33.log` records 33/33 `MatterFlux.*` Automation
  tests passing, including dedicated-server/two-client PIE.
- `Saved/Logs/CodexMapCheckFinal.log` records `/Game/Default` with zero errors
  and zero warnings and an editor exit code of 0.
- On 2026-07-24, formal current-source `MatterFluxEditor` and `MatterFlux`
  Win64 Development builds passed with MSVC 14.44.35222; the first full Game
  build completed 80/80 actions.
- `Saved/Logs/ReleaseVerification/20260724-server-preflight-skip-final/Automation.log`
  records 34/34 tests passing, including the navigation regression and
  dedicated-server/two-client PIE; the adjacent `MapCheck.log` records zero
  errors and zero warnings. Gameplay-cue path and empty-navigation-bounds
  warnings are both absent.
- The release runner's full mode rejects the current Launcher engine at its
  Server capability preflight in about 0.2 seconds, before creating a run
  directory or starting Editor/Game builds; `-SkipServer` still completes all
  four supported gates and explicitly leaves the release unsigned.
- The same `20260724-nav-final` run formally rebuilt the changed Game target;
  both Editor and Game completed with zero compiler warnings.
- The full Game build emitted 60 `C4191` warnings, all from the vendored
  UnrealAngelScriptForge AngelScript third-party sources; MatterFlux-owned C++
  compiled without warnings or errors.
- `MatterFluxServer` remains the only unsigned gate: Epic Launcher UE 5.8
  reports that Server targets are unsupported by this engine distribution.
  A source-built or Server-enabled Installed Build is required before release.
