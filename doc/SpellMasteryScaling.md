# Spell Mastery Scaling Tracker

This file is the canonical tuning reference for Spell Mastery output scaling.

## Update Rule

When any mastery tuning value changes in C++:

1. Update this file in the same change.
2. Include the affected spell section and the new numbers.
3. Keep formulas human-readable (full words, not letter shorthand).

## Definitions

- Iron level, Bronze level, Silver level, Gold level, Diamond level: effective tier level values for a spell.
- Total mastery levels: Iron level + Bronze level + Silver level + Gold level + Diamond level.

## Global Early-Access Scaling

Some spells are downscaled when learned early at low character level.

- Formula:
  - `scale = clamp((player level + 4) / (spell natural level + 4), 0.05, 1.0)`
- Source: `src/server/scripts/World/spell_mastery_fireball.cpp`

## Managed Spell Baselines

Configured in `src/server/scripts/World/spell_mastery_fireball.cpp`:

- Fireball baseline rank: Rank 3
- Pyroblast baseline rank: Rank 10
- Flamestrike baseline rank: Rank 3
- Chain Lightning baseline rank: Rank 1
- Lava Burst baseline rank: Rank 1
- Power Word: Shield baseline rank: Rank 1
- Consecration baseline rank: Rank 1
- Thunder Clap baseline rank: Rank 9
- Killing Spree baseline: base spell
- Haunt baseline rank: Rank 3
- Rejuvenation baseline rank: Rank 5
- Regrowth baseline rank: Rank 3
- Rip baseline rank: Rank 1
- Swipe (Cat) baseline rank: Rank 1

## Mage

Source: `src/server/scripts/World/spell_mastery_mage.cpp`

### Fireball

- Direct damage bonus percent:
  - `20 * Iron level + 20 * Bronze level + 20 * Silver level + 25 * Gold level + 30 * Diamond level`
- Silver splash damage percent:
  - `35 + (Silver level - 1) * (45 / 9)` (35% to 80%)
- Gold burn contribution percent (into ignite pool):
  - `10 + (Gold level - 1) * (15 / 9)` (10% to 25%)

### Pyroblast

- Direct damage bonus percent:
  - `2 * Total mastery levels`
- Silver splash damage percent:
  - `35 + (Silver level - 1) * (45 / 9)` (35% to 80%)
- Gold burn contribution percent:
  - `20 + (Gold level - 1) * (30 / 9)` (20% to 50%)

### Flamestrike

- Damage bonus percent (initial and periodic):
  - `20 * Iron level + 20 * Bronze level + 20 * Silver level + 25 * Gold level + 30 * Diamond level`
- Silver extra damage pass percent:
  - `8 + (Silver level - 1) * (14 / 9)` (8% to 22%)
- Gold burn damage percent:
  - `10 + (Gold level - 1) * (15 / 9)` (10% to 25%)

## Druid

Source: `src/server/scripts/World/spell_mastery_druid.cpp`

### Rejuvenation

- Periodic healing bonus percent:
  - `20 * Iron level + 20 * Bronze level + 20 * Silver level + 25 * Gold level + 30 * Diamond level`
- Silver splash healing percent:
  - `15 + (Silver level - 1) * (25 / 9)` (15% to 40%)
- Gold extra stack bonus healing percent:
  - `8` per extra stack
- Diamond bonus tick percent:
  - `20 + (Diamond level - 1) * (60 / 9)` (20% to 80%)

### Regrowth

- Direct and periodic healing bonus percent:
  - `20 * Iron level + 20 * Bronze level + 20 * Silver level + 25 * Gold level + 30 * Diamond level`
- Silver splash healing percent:
  - `12 + (Silver level - 1) * (20 / 9)` (12% to 32%)
- Gold extra stack bonus healing percent:
  - `10` per extra stack
- Diamond bonus direct-heal percent:
  - `20 + (Diamond level - 1) * (60 / 9)` (20% to 80%)

### Swipe (Cat)

- Direct damage bonus percent:
  - `4 * Total mastery levels`
- Gold bleed percent:
  - `10 + (Gold level - 1) * (30 / 9)` (10% to 40%)
- Diamond self-heal percent from dealt damage:
  - `8 + (Diamond level - 1) * (22 / 9)` (8% to 30%)

### Rip

- Periodic damage bonus percent:
  - `25 * Total mastery levels`
- Silver extra damage taken percent on affected targets:
  - `2 * Silver level`

## Warrior

Source: `src/server/scripts/World/spell_mastery_warrior.cpp`

### Thunder Clap

- Direct damage bonus percent:
  - `10 * Iron level + 10 * Bronze level + 10 * Silver level + 12.5 * Gold level + 15 * Diamond level`
- Gold echo damage percent:
  - `10 + (Gold level - 1) * (20 / 9)` (10% to 30%)
- Diamond Rend immediate tick bonus percent:
  - `10 + (Diamond level - 1) * (40 / 9)` (10% to 50%)

## Shaman

Source: `src/server/scripts/World/spell_mastery_shaman.cpp`

### Chain Lightning

- Direct damage bonus percent:
  - `2 * Total mastery levels`
- Gold nature vulnerability per stack percent:
  - `1 + (Gold level - 1) * (1.5 / 9)` (1% to 2.5% per stack)

### Lava Burst

- Direct damage bonus percent:
  - `1 * Total mastery levels`
- Silver fire vulnerability percent:
  - `2.5 * Silver level`

## Paladin

Source: `src/server/scripts/World/spell_mastery_paladin.cpp`

### Consecration

- Damage bonus percent:
  - `8 * Total mastery levels`
- Silver enemy outgoing damage reduction percent:
  - `1.5 * Silver level`
- Gold outgoing damage bonus while in consecration:
  - Per stack: `1.0 + (0.2 * Gold level)` percent

## Priest

Source: `src/server/scripts/World/spell_mastery_priest.cpp`

### Power Word: Shield

- Shield absorb bonus percent:
  - `56 * Total mastery levels`
- Silver heal-over-time per tick percent (based on shield amount):
  - `4 * Silver level`
- Gold reflect percent (of absorbed damage):
  - `15 + (6 * Gold level)`
- Diamond shield-end heal percent:
  - `minimum(100, 10 + 9 * Diamond level)`

## Rogue

Source: `src/server/scripts/World/spell_mastery_rogue.cpp`

### Killing Spree

- Strike damage bonus percent:
  - `8 * Total mastery levels`
- Gold bleed percent:
  - `20 + (Gold level - 1) * (30 / 9)` (20% to 50%)

## Warlock

Source: `src/server/scripts/World/spell_mastery_warlock.cpp`

### Haunt

- Direct damage bonus percent:
  - `10 * Total mastery levels`
- Bronze periodic amplification percent:
  - `20 + (Bronze level - 1) * (80 / 9)` (20% to 100%)
- Silver return heal percent:
  - `100 + (Silver level - 1) * (100 / 9)` (100% to 200%)
