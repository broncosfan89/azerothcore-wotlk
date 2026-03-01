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

Some spells are dynamically scaled based on character level.

- Early-access downscaling formula:
  - `scale = clamp((player level + 4) / (spell natural level + 4), 0.05, 1.0)`
- Low-rank normalization (for rank 1 to rank 3 spells):
  - `scale = clamp(1 + (player level - spell natural level) * 0.08, 1.0, 8.0)`
- Source: `src/server/scripts/World/spell_mastery_fireball.cpp`

## Managed Spell Baselines

Configured in `src/server/scripts/World/spell_mastery_fireball.cpp`:

- Fireball baseline rank: Rank 3
- Pyroblast baseline rank: Rank 10
- Flamestrike baseline rank: Rank 3
- Frostbolt baseline rank: Rank 1
- Ice Lance baseline rank: Rank 1
- Blizzard baseline rank: Rank 1
- Cone of Cold baseline rank: Rank 1
- Arcane Blast baseline rank: Rank 1
- Arcane Missiles baseline rank: Rank 1
- Arcane Barrage baseline rank: Rank 1
- Arcane Explosion baseline rank: Rank 1
- Chain Lightning baseline rank: Rank 1
- Lava Burst baseline rank: Rank 1
- Power Word: Shield baseline rank: Rank 1
- Consecration baseline rank: Rank 1
- Thunder Clap baseline rank: Rank 9
- Killing Spree baseline: base spell
- Fan of Knives baseline rank: Rank 1
- Rupture baseline rank: Rank 1
- Volley baseline rank: Rank 1
- Aimed Shot baseline rank: Rank 1
- Serpent Sting baseline rank: Rank 1
- Haunt baseline rank: Rank 3
- Shadow Bolt baseline rank: Rank 1
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

### Frostbolt

- Iron direct damage bonus percent:
  - `8 * Iron level`
- Bronze crit chance bonus percent against chilled/frozen targets:
  - `2 * Bronze level`
- Silver mark bonus percent consumed by Ice Lance:
  - `20 + (Silver level - 1) * (40 / 9)` (20% to 60%)
- Gold bonus damage percent against chilled/frozen targets:
  - `10 + (Gold level - 1) * (20 / 9)` (10% to 30%)
- Diamond cast time multiplier:
  - `0.90 - (Diamond level - 1) * (0.50 / 9)` (90% to 40% of base cast time)

### Ice Lance

- Iron direct damage bonus percent:
  - `8 * Iron level`
- Bronze bonus damage percent against chilled/frozen targets:
  - `5 * Bronze level`
- Silver crit chance bonus percent against chilled/frozen targets:
  - `2 * Silver level`
- Gold ricochet percent:
  - `20 + (Gold level - 1) * (30 / 9)` (20% to 50%)
- Diamond second-lance bonus damage percent:
  - `20 + (Diamond level - 1) * (30 / 9)` (20% to 50%)

### Blizzard

- Iron direct damage bonus percent:
  - `6 * Iron level`
- Bronze radius multiplier:
  - `1 + 0.10 * Bronze level`
- Silver bonus damage percent against chilled/frozen targets:
  - `2 * Silver level`
- Gold hail proc chance percent:
  - `5 + (Gold level - 1) * (25 / 9)` (5% to 30%)
- Gold hail proc damage percent:
  - `40` of hit damage
- Diamond bonus damage percent against chilled/frozen targets:
  - `20 + (Diamond level - 1) * (40 / 9)` (20% to 60%)

### Cone of Cold

- Iron direct damage bonus percent:
  - `8 * Iron level`
- Bronze radius multiplier:
  - `1 + 0.05 * Bronze level`
- Silver frost vulnerability percent applied to target:
  - `2 * Silver level`
- Gold bonus damage percent against chilled/frozen targets:
  - `10 + (Gold level - 1) * (20 / 9)` (10% to 30%)
- Diamond second pulse percent:
  - `20 + (Diamond level - 1) * (30 / 9)` (20% to 50%)

### Arcane Blast

- Iron direct damage bonus percent:
  - `8 * Iron level`
- Bronze mana refund percent:
  - `4 + (Bronze level - 1) * (10 / 9)` (4% to 14%)
- Silver bonus damage per arcane charge percent:
  - `4 + (Silver level - 1) * (8 / 9)` (4% to 12%) per charge
- Gold bonus damage at 4 charges percent:
  - `10 + (Gold level - 1) * (20 / 9)` (10% to 30%)
- Diamond splash percent at 4 charges:
  - `20 + (Diamond level - 1) * (30 / 9)` (20% to 50%)

### Arcane Missiles

- Iron direct damage bonus percent:
  - `6 * Iron level`
- Bronze cast-time reduction:
  - `100 + 45 * (Bronze level - 1)` milliseconds
- Silver mana return percent:
  - `4 + (Silver level - 1) * (10 / 9)` (4% to 14%)
- Gold extra missile proc chance percent:
  - `5 + (Gold level - 1) * (25 / 9)` (5% to 30%)
- Gold extra missile proc damage percent:
  - `40` of hit damage
- Diamond cleave percent:
  - `20 + (Diamond level - 1) * (30 / 9)` (20% to 50%)

### Arcane Barrage

- Iron direct damage bonus percent:
  - `8 * Iron level`
- Bronze extra targets:
  - `minimum(3, 1 + floor((Bronze level - 1) / 3))`
- Silver bonus damage per arcane charge percent:
  - `4 + (Silver level - 1) * (8 / 9)` (4% to 12%) per charge
- Gold flat bonus damage percent:
  - `10 + (Gold level - 1) * (20 / 9)` (10% to 30%)
- Diamond charge reset chance percent:
  - `5 + (Diamond level - 1) * (25 / 9)` (5% to 30%)

### Arcane Explosion

- Iron direct damage bonus percent:
  - `6 * Iron level`
- Bronze mana refund percent:
  - `3 + (Bronze level - 1) * (9 / 9)` (3% to 12%)
- Silver radius multiplier:
  - `1 + 0.10 * Silver level`
- Gold clearcasting proc chance percent:
  - `5 + (Gold level - 1) * (20 / 9)` (5% to 25%)
- Diamond aftershock percent (every 4th cast):
  - `20 + (Diamond level - 1) * (30 / 9)` (20% to 50%)

## Druid

Source: `src/server/scripts/World/spell_mastery_druid.cpp`

### Rejuvenation

- Periodic healing bonus percent:
  - `40 * Iron level + 40 * Bronze level + 40 * Silver level + 50 * Gold level + 60 * Diamond level`
- Silver splash healing percent:
  - `15 + (Silver level - 1) * (25 / 9)` (15% to 40%)
- Gold extra stack bonus healing percent:
  - `8` per extra stack
- Diamond bonus tick percent:
  - `20 + (Diamond level - 1) * (60 / 9)` (20% to 80%)

### Regrowth

- Direct and periodic healing bonus percent:
  - `40 * Iron level + 40 * Bronze level + 40 * Silver level + 50 * Gold level + 60 * Diamond level`
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

### Fan of Knives

- Iron energy refund (effective cost from 50 down to 20 at Iron 10):
  - `3 * Iron level` energy refunded after cast
- Damage bonus percent (Bronze scaling continues through Diamond):
  - `2 * Total mastery levels`
- Silver radius scaling (8 yards to 20 yards at Silver 10):
  - Radius multiplier `1 + 1.5 * (Silver level / 10)`
- Gold combo point generation:
  - `minimum(5, Gold level)` combo points on first valid hit per cast
- Diamond poison application:
  - Applies Deadly Poison to each hit target

### Rupture

- Periodic damage bonus percent:
  - `25 * Total mastery levels`
- Silver extra damage taken percent (applied to rupture periodic damage):
  - `2 * Silver level`
- Bronze tick interval:
  - `500` milliseconds once Bronze is unlocked
- Gold duration bonus:
  - `500 * Gold level` milliseconds
- Diamond combo-point normalization:
  - Enables full-damage scaling at 1 combo point

## Hunter

Source: `src/server/scripts/World/spell_mastery_hunter.cpp`

### Volley

- Iron damage bonus percent:
  - `10 * Iron level`
- Bronze radius scaling:
  - Radius multiplier `1 + 1.5 * (Bronze level / 10)`
- Silver duration bonus:
  - `1000 * Silver level` milliseconds
- Gold tick-rate increase:
  - Tick interval `1000 - round(500 * Gold level / 10)` milliseconds, minimum 500 milliseconds
- Diamond AoE burst damage:
  - `20 + (Diamond level - 1) * (40 / 9)` percent (20% to 60%) to enemies within 6 yards of each target hit

### Aimed Shot

- Charge damage scaling percent (based on charge ratio):
  - `20 + 180 * charge ratio` (20% to 200%)
- Total mana cost multiplier (before Bronze reduction):
  - `0.25 + 1.75 * charge ratio` (25% to 200% of base mana cost)
- Iron direct damage bonus percent:
  - `5 * Iron level`
- Bronze mana cost reduction percent:
  - `2.5 * Bronze level`
- Silver crit chance at 50%+ charge:
  - `2 * Silver level`
- Gold bonus damage at 70%+ charge:
  - `10 + (Gold level - 1) * (20 / 9)` (10% to 30%)
- Diamond splash damage at 90%+ charge:
  - `20 + (Diamond level - 1) * (30 / 9)` (20% to 50%) in 8 yards

### Serpent Sting

- Iron mana regeneration:
  - `2 * Iron level` percent of max mana on successful application
- Bronze periodic damage bonus percent:
  - `6 * Bronze level`
- Silver tick interval:
  - `3000 - round(2000 * Silver level / 10)` milliseconds, minimum 1000 milliseconds
- Gold spread:
  - Up to `Gold level` nearby targets, each takes `20 + (Gold level - 1) * (20 / 9)` percent of each tick
- Diamond detonation on natural expiration:
  - AoE damage equal to `60 + 10 * (Diamond level - 1)` percent of tick damage

## Warlock

Source: `src/server/scripts/World/spell_mastery_warlock.cpp`

### Haunt

- Direct damage bonus percent:
  - `10 * Total mastery levels`
- Bronze periodic amplification percent:
  - `20 + (Bronze level - 1) * (80 / 9)` (20% to 100%)
- Silver return heal percent:
  - `100 + (Silver level - 1) * (100 / 9)` (100% to 200%)

### Shadow Bolt

- Iron direct damage bonus percent:
  - `8 * Iron level`
- Bronze mana refund percent (effective mana-cost reduction):
  - `6 + 2 * (Bronze level - 1)` (6% to 24%) of cast power cost
- Silver splash damage percent:
  - `20 + (Silver level - 1) * (30 / 9)` (20% to 50%) to nearby enemies
- Gold bonus DoT per tick:
  - Applies Corruption with at least `8 + (Gold level - 1) * (20 / 9)` percent of hit damage as per-tick value
- Diamond extra target hits:
  - Fires at up to `Diamond level` nearby additional targets
