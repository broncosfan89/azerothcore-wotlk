# Deadmines Breakout MVP

This document defines the first-pass implementation for a zone event where the Deadmines "breaks loose" into Westfall, followed by a harder event version of the Deadmines dungeon.

## Goals

- Reuse an existing dungeon and zone in a way that feels like new content.
- Give players in an outdoor zone a shared event with clear progress and contribution.
- Follow the zone event with a time-limited dungeon escalation that offers better rewards.
- Keep the first implementation small enough to ship and iterate.

## Event Summary

- Event name: `Deadmines Breakout`
- Outdoor zone: `Westfall`
- Dungeon anchor: `The Deadmines`
- Outdoor quest: `Break the Defias Uprising`
- Follow-up quest: `Seal the Deadmines`

## Event Flow

### Phase 1: Announcement

- The server announces that the Deadmines has broken loose into Westfall.
- Players currently in Westfall receive the outdoor event quest automatically.
- Players who enter Westfall while the event is active also receive the quest automatically.
- A zone-wide progress tracker is exposed through broadcast text and optional UI notifications later.

Suggested broadcast:

```text
Westfall is under attack. Defias forces are pouring out of the Deadmines.
Drive them back and restore order to the zone.
```

### Phase 2: Outdoor Invasion

- Defias mobs spawn at fixed invasion hotspots instead of random scatter spawns.
- Each kill reduces a shared zone `threat` meter.
- Players build personal `contribution score` based on kills and participation.
- When threat reaches `0`, the zone invasion ends.

### Phase 3: Resolution

- The server announces that Westfall has been secured.
- Players who met a minimum contribution threshold complete the outdoor event quest.
- Reward tier is determined from contribution score.
- The follow-up dungeon quest is granted to qualified players.

### Phase 4: Broken Dungeon Window

- The Deadmines enters a harder event state for a limited time.
- Suggested duration: `45` minutes.
- Players with the follow-up quest can enter and complete the event version for bonus rewards.

## Westfall Hotspots

Use `4` invasion hotspots for the MVP.

### Hotspot A: Moonbrook

- Main breakout staging area.
- Highest mob density.
- Contains one elite lieutenant.

### Hotspot B: Sentinel Hill Outskirts

- Pressure on the zone hub.
- Focus on melee raiders and a small number of ranged attackers.
- Good place for a commander wave later in the event.

### Hotspot C: Jangolode Mine Road

- Ambush lane between Moonbrook and central Westfall.
- Best place for patrol-style Defias packs.

### Hotspot D: Saldean Farm Corridor

- Civilian/farm defense flavor.
- Lower density but steady pressure.
- Good location for an "escort the farmers" or "defend supplies" extension later.

## Enemy Roster

Start with existing Defias-themed NPCs or custom clones with simple tuning changes.

### Core Mobs

- `Defias Tunneler`
- `Defias Smuggler`
- `Defias Raider`
- `Defias Pillager`

### Elite Mobs

- `Defias Lieutenant`
- One lieutenant active per hotspot.

### Final Outdoor Commander

- `Defias Breakout Commander`
- Spawns when remaining threat falls below the final threshold.
- Suggested spawn point: road outside Sentinel Hill or Moonbrook center.

## Spawn Model

- Use wave-based spawns at fixed points.
- Keep total active creature count capped to avoid uncontrolled growth.
- Respawn logic should depend on remaining threat and current active mobs.

Suggested outdoor pacing:

- Event start: `4` active packs, one per hotspot.
- Mid-event: `+1` reinforcement pack at Moonbrook and Sentinel Hill.
- Final stage: outdoor commander plus reduced trash reinforcements.

## Threat Meter

Use a zone-level `threat` value instead of raw kill counting.

Suggested MVP values:

- Event starts at `300` threat.
- Standard mob kill: `-5` threat.
- Elite lieutenant kill: `-20` threat.
- Outdoor commander kill: `-40` threat.

Benefits:

- Easier to tune than hardcoded total mob counts.
- Safer if mobs evade, despawn, or get stuck.
- Lets later objectives remove threat without requiring kills.

## Player Contribution

Track per-player contribution during the active event.

Suggested MVP scoring:

- Standard mob kill participation: `1` point.
- Elite lieutenant participation: `5` points.
- Outdoor commander participation: `15` points.

Credit rules:

- Give contribution to all eligible players in range who tagged or meaningfully participated.
- Do not use last-hit logic.
- Grouped players should all be eligible if they are nearby.

Suggested minimum participation:

- `10` contribution points required to qualify for end rewards and the dungeon follow-up quest.

## Outdoor Quest

Quest name: `Break the Defias Uprising`

Suggested quest logic:

- Auto-accepted while event is active and player enters Westfall.
- Hidden objective tracking can be backed by contribution score rather than only visible kill counts.

Suggested visible objectives for MVP:

- Defeat Defias raiders in Westfall: `0/10`
- Defeat Defias lieutenants: `0/2`

Actual reward qualification should still use contribution score under the hood.

## Reward Tiers

Use broad tiers, not competitive ranking.

### Bronze

- Requirement: `10-24` contribution
- Rewards:
  - gold
  - experience
  - small event cache

### Silver

- Requirement: `25-49` contribution
- Rewards:
  - better cache
  - mastery XP token or direct mastery XP grant
  - increased gold/experience

### Gold

- Requirement: `50+` contribution
- Rewards:
  - best outdoor cache
  - extra mastery XP
  - event currency bundle
  - small cosmetic drop chance

## Follow-Up Dungeon Quest

Quest name: `Seal the Deadmines`

Grant conditions:

- Player qualified for the outdoor event reward.
- Player was in Westfall during the active event.

Objective:

- Complete the `Broken Deadmines` event version before the dungeon window expires.

## Broken Deadmines Rules

Keep the dungeon version harder through composition and pacing, not only larger numbers.

### Dungeon Modifiers

- Trash health/damage modestly increased.
- More patrol overlap in key halls.
- Added caster pressure in some packs.
- Defias lieutenants buff nearby trash.

### Boss Layer

Use one of these approaches for MVP:

- Add one new miniboss before VanCleef.
- Add one Defias commander to the foundry/ship path.
- Give VanCleef a breakout-themed reinforcement phase.

### Suggested MVP Modifiers

- All event trash: `+20%` health, `+15%` damage.
- Event lieutenants:
  - periodic rally buff on nearby Defias
  - short self-shield or enrage
- VanCleef event version:
  - summons `2` Defias reinforcements at fixed health thresholds

## Dungeon Rewards

The dungeon completion reward should clearly beat the outdoor reward.

Suggested reward package:

- event chest at final boss kill
- bonus gold
- higher experience
- mastery XP grant
- event currency
- low chance of cosmetic or vanity item

Optional later additions:

- Defias-themed transmog pieces
- mount or tabard fragments
- account-wide achievement progress

## Runtime Rules

- Only one breakout event should be active at a time in the MVP.
- Event cooldown should prevent immediate repeats.
- Suggested minimum cooldown: `3-6` hours.
- Broken dungeon window should automatically expire even if players do not engage.

## Recommended AzerothCore Architecture

Keep the first implementation code-driven, not DB-heavy.

### Core Manager

Add a world-level event manager responsible for:

- choosing when the event starts
- tracking active state
- tracking threat
- tracking player contribution
- starting and ending the dungeon window

Good fit:

- `WorldScript` plus a dedicated runtime manager class

### Outdoor Event Scripts

Needed pieces:

- zone event controller
- hotspot spawn definitions
- invasion creature behavior hooks
- commander spawn trigger

Good fit:

- custom scripts under `src/server/scripts/Custom/`

### Quest Handling

Needed pieces:

- auto-grant outdoor quest on zone entry during active event
- auto-grant follow-up quest on successful participation
- optional cleanup if player leaves or event expires

Good fit:

- `PlayerScript`

### Contribution Tracking

MVP can stay in memory:

- map of `playerGuid -> contribution score`
- map of `event creature guid -> threat value`

Persist only if needed later.

### Dungeon Event State

Needed pieces:

- runtime `Broken Deadmines active` flag
- modified creature/boss behavior only while event state is enabled
- reward chest or completion grant on final boss kill

Good fit:

- instance script checks against the world event manager

## Suggested Data To Externalize Later

Do not externalize this in the first pass unless iteration becomes slow.

Possible future data tables:

- event definitions
- hotspot coordinates
- mob pool composition
- reward thresholds
- dungeon modifier sets

## MVP Non-Goals

Do not include these in version one:

- multiple simultaneous zone breakouts
- dynamic map markers or custom UI
- escort objectives
- bot-specific event leadership logic
- per-faction split behavior
- complicated persistence or recovery after restart

## First Implementation Checklist

1. Add a runtime event manager for `Deadmines Breakout`.
2. Add Westfall hotspot spawn groups and threat accounting.
3. Add auto-accept outdoor quest for players entering Westfall during the event.
4. Add contribution tracking and outdoor reward tiers.
5. Add follow-up quest grant on successful participation.
6. Add `Broken Deadmines` runtime flag with modest dungeon stat tuning.
7. Add one event miniboss or enhanced VanCleef phase.
8. Add final dungeon reward chest.

## Recommended First Test Pass

- Trigger event manually through a GM command.
- Validate hotspot spawns and threat reduction.
- Validate contribution in solo and group cases.
- Validate event shutdown and reward tier assignment.
- Validate dungeon flag expiry after the configured window.
- Validate that the normal Deadmines still works when the event is inactive.
