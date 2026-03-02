# Spell Mastery Scaling Reference

This file is the canonical tuning reference for Spell Mastery output scaling.

## Update Rule

When mastery tuning changes in C++:

1. Update this file in the same change.
2. Include the affected spell section and new values.
3. Keep formulas human-readable.

## Definitions

- Iron level, Bronze level, Silver level, Gold level, Diamond level: effective tier levels for a spell.
- Total mastery levels: Iron + Bronze + Silver + Gold + Diamond.

## Global Early-Access Scaling

Some spells use dynamic early-access scaling.

- Early-access downscaling:
  - `scale = clamp((player level + 4) / (spell natural level + 4), 0.05, 1.0)`
- Low-rank normalization (rank 1 to rank 3):
  - `scale = clamp(1 + (player level - spell natural level) * 0.08, 1.0, 8.0)`

Source: `src/server/scripts/World/spell_mastery_fireball.cpp`

## Managed Spell Baselines (Current)

Configured in `src/server/scripts/World/spell_mastery_fireball.cpp`:

- Mage: Fireball (Rank 3), Pyroblast (Rank 1), Flamestrike (Rank 3), Frostbolt (Rank 1), Ice Lance (Rank 1), Blizzard (Rank 1), Cone of Cold (Rank 1), Arcane Blast (Rank 1), Arcane Missiles (Rank 1), Arcane Barrage (Rank 1), Arcane Explosion (Rank 1)
- Shaman: Chain Lightning (Rank 1), Lava Burst (Rank 1)
- Priest: Power Word: Shield (Rank 1), Penance (Rank 1), Flash Heal (Rank 1)
- Paladin: Consecration (Rank 1)
- Warrior: Thunder Clap (Rank 9 baseline rank cap), Revenge (Rank 1)
- Rogue: Killing Spree, Fan of Knives (Rank 1), Rupture (Rank 1)
- Hunter: Volley (Rank 1), Aimed Shot (Rank 1), Serpent Sting (Rank 1)
- Warlock: Haunt (Rank 3 baseline rank cap), Shadow Bolt (Rank 1), Chaos Bolt (Rank 1), Rain of Fire (Rank 1)
- Druid: Rejuvenation (Rank 5 baseline rank cap), Regrowth (Rank 3 baseline rank cap), Rip (Rank 1), Swipe (Cat) (Rank 1)

## Mage

Source: `src/server/scripts/World/spell_mastery_mage.cpp`

### Fireball

- Iron/Global: damage bonus = `20*Iron + 20*Bronze + 20*Silver + 25*Gold + 30*Diamond`.
- Bronze: crit chance `5% -> 23%`.
- Silver: splash damage `35% -> 80%`.
- Gold: burn contribution `10% -> 25%` of direct hit.
- Diamond: cast-time multiplier `0.05 -> 0.01`.

### Pyroblast

- Iron/Global: damage bonus = `2 * Total mastery levels`.
- Bronze: crit chance `5% -> 23%`.
- Silver: splash damage `35% -> 80%`.
- Gold: burn contribution `20% -> 50%`.
- Diamond: cast-time multiplier `0.05 -> 0.01`.

### Flamestrike

- Iron/Global: damage bonus = `20*Iron + 20*Bronze + 20*Silver + 25*Gold + 30*Diamond`.
- Bronze: radius multiplier `x1.05 -> x1.50`.
- Silver: extra damage pass `8% -> 22%`.
- Gold: burn damage `10% -> 25%`, max stacks up to `8`.
- Diamond: cast-time multiplier `0.80 -> 0.10`.

### Frostbolt

- Iron: damage bonus `8% -> 80%`.
- Bronze: crit vs chilled/frozen `2% -> 20%`.
- Silver: Ice Lance mark bonus `20% -> 60%`.
- Gold: bonus damage vs chilled/frozen `10% -> 30%`.
- Diamond: cast-time multiplier `0.90 -> 0.40`.

### Ice Lance

- Iron: damage bonus `8% -> 80%`.
- Bronze: bonus vs chilled/frozen `5% -> 50%`.
- Silver: crit vs chilled/frozen `2% -> 20%`.
- Gold: ricochet `20% -> 50%`.
- Diamond: second lance hit `20% -> 50%`.

### Blizzard

- Iron: damage bonus `6% -> 60%`.
- Bronze: radius multiplier `x1.10 -> x2.00`.
- Silver: bonus damage `2% -> 20%`.
- Gold: hail proc chance `5% -> 30%`.
- Diamond: bonus damage `20% -> 60%`.

### Cone of Cold

- Iron: damage bonus `8% -> 80%`.
- Bronze: radius multiplier `x1.05 -> x1.50`.
- Silver: vulnerability debuff `2% -> 20%`.
- Gold: bonus damage `10% -> 30%`.
- Diamond: second pulse `20% -> 50%`.

### Arcane Blast

- Iron: damage bonus `8% -> 80%`.
- Bronze: mana refund `4% -> 14%`.
- Silver: per-charge bonus `4% -> 12%`.
- Gold: 4-charge bonus `10% -> 30%`.
- Diamond: 4-charge splash `20% -> 50%`.

### Arcane Missiles

- Iron: damage bonus `6% -> 60%`.
- Bronze: cast-time/tick reduction `100ms -> 505ms`.
- Silver: mana return `4% -> 14%`.
- Gold: extra missile chance `5% -> 30%`.
- Diamond: cleave `20% -> 50%`.

### Arcane Barrage

- Iron: damage bonus `8% -> 80%`.
- Bronze: extra targets up to `3`.
- Silver: per-charge bonus `4% -> 12%`.
- Gold: flat bonus `10% -> 30%`.
- Diamond: charge reset chance `5% -> 30%`.

### Arcane Explosion

- Iron: damage bonus `6% -> 60%`.
- Bronze: mana refund `3% -> 12%`.
- Silver: radius multiplier `x1.10 -> x2.00`.
- Gold: clearcasting chance `5% -> 25%`.
- Diamond: aftershock `20% -> 50%`.

## Shaman

Source: `src/server/scripts/World/spell_mastery_shaman.cpp`

### Chain Lightning

- Iron/Global: damage bonus = `2 * Total mastery levels`.
- Bronze: bounce reduction penalty scales down `30% -> 0%`.
- Silver: extra targets `+1 -> +10`.
- Gold: nature-taken stack system:
  - max stacks = Gold level
  - per-stack bonus `1.0% -> 2.5%`.
- Diamond: instant reset behavior enabled.

### Lava Burst

- Iron: cooldown reduction up to `-6000ms`.
- Bronze/Global: damage bonus = `0.5 * Total mastery levels`.
- Silver: fire-taken debuff `2.5% -> 25%`.
- Gold: spread Flame Shock to `1 -> 10` targets.
- Diamond: Flame Shock burst behavior enabled.

## Priest

Source: `src/server/scripts/World/spell_mastery_priest.cpp`

### Power Word: Shield

- Iron/Global: shield bonus = `39.2 * Total mastery levels` percent.
- Bronze: Weakened Soul reduction up to `60%`.
- Silver: HoT per tick `2.8% -> 28%`.
- Gold: reflect `21% -> 75%`.
- Diamond: end-heal `10% -> 100%`, radius `11 -> 20`.

### Penance

- Iron/Global: throughput bonus = `4 * Total mastery levels` percent.
- Bronze: mana refund `3% -> 12%`.
- Silver: crit chance `3% -> 18%`.
- Gold:
  - execute damage bonus `8% -> 30%`
  - emergency heal bonus `10% -> 40%`.
- Diamond: echo amount `20% -> 50%`.

### Flash Heal

- Iron/Global: heal bonus = `3 * Total mastery levels` percent.
- Bronze: mana refund `2% -> 10%`.
- Silver: crit chance `2% -> 15%`.
- Gold: splash heal `10% -> 30%`.
- Diamond: emergency heal `15% -> 50%`.

## Paladin

Source: `src/server/scripts/World/spell_mastery_paladin.cpp`

### Consecration

- Iron/Global: damage bonus = `8 * Total mastery levels` percent.
- Bronze: radius multiplier `x1.05 -> x1.50`.
- Silver: enemy outgoing damage reduction `1.5% -> 15%`.
- Gold:
  - max stacks = Gold level
  - per-stack damage bonus = `1.0 + 0.2*Gold` percent.
- Diamond: heal from dealt hit damage `300% -> 900%`.

## Warrior

Source: `src/server/scripts/World/spell_mastery_warrior.cpp`

### Thunder Clap

- Iron/Global: damage bonus = `5*Iron + 5*Bronze + 5*Silver + 5*Gold + 5*Diamond`.
- Bronze: every 2 Bronze levels grants:
  - `+6%` radius (up to `+30%`)
  - `-400ms` cooldown (up to `-2000ms`).
- Silver: apply Rend to `1 -> 10` targets.
- Gold: echo pulse `10% -> 30%`.
- Diamond: immediate Rend tick `10% -> 50%`.

### Revenge

- Iron/Global: damage bonus = `5 * Total mastery levels` percent.
- Bronze: heal `1% -> 3%` max HP.
- Silver: damage reduction `3% -> 12%`, duration `3500ms -> 8000ms`.
- Gold: extra targets `1 -> 10`.
- Diamond: reflect `5% -> 15%`, duration `4500ms -> 9000ms`.

## Rogue

Source: `src/server/scripts/World/spell_mastery_rogue.cpp`

### Killing Spree

- Iron: attack count `6 -> 15` (base 5 plus Iron scaling, capped).
- Bronze/Global: damage bonus = `8 * Total mastery levels` percent.
- Silver: cooldown reduced to `45s`.
- Gold: bleed amount `20% -> 50%`.
- Diamond: extra strike chance `5% -> 30%`.

### Fan of Knives

- Iron: energy refund `3 -> 30`.
- Bronze/Global: damage bonus = `4 * Total mastery levels` percent.
- Silver: radius multiplier `x1.15 -> x2.50`.
- Gold: combo points `1 -> 5` on first valid hit.
- Diamond: apply poison on hit.

### Rupture

- Iron/Global: damage bonus = `5 * Total mastery levels` percent.
- Bronze: tick interval `2000ms -> 500ms`.
- Silver: self-heal from tick `2% -> 20%`.
- Gold: duration bonus `500ms -> 5000ms`.
- Diamond: full-damage scaling at 1 combo point enabled.

## Hunter

Source: `src/server/scripts/World/spell_mastery_hunter.cpp`

### Volley

- Iron/Global: damage bonus = `1.5 * Total mastery levels` percent.
- Bronze: radius multiplier `x1.05 -> x1.50`.
- Silver: faster tick interval `1000ms -> 750ms`.
- Gold: spread Serpent Sting to `1 -> 10` targets.
- Diamond: burst around hit target `5% -> 15%`.

### Aimed Shot

- Base charge behavior:
  - damage scale from charge ratio: `20% -> 200%`
  - mana cost multiplier from charge ratio: `25% -> 200%` (before Bronze reduction).
- Iron: damage bonus `5% -> 50%`.
- Bronze: mana cost reduction `2.5% -> 25%`.
- Silver: crit chance at half charge or higher `2% -> 20%`.
- Gold: high-charge bonus (70%+ charge) `10% -> 30%`.
- Diamond: splash at near-full charge (90%+ charge) `20% -> 50%`.

### Serpent Sting

- Iron: mana regen on application `2% -> 20%` of max mana.
- Bronze/Global: damage bonus = `30 * Total mastery levels` percent.
- Silver: faster tick interval `3000ms -> 1000ms`.
- Gold:
  - spread to `1 -> 10` targets
  - spread damage `20% -> 40%`.
- Diamond: natural-expire detonation `60% -> 150%` of tick damage.

## Warlock

Source: `src/server/scripts/World/spell_mastery_warlock.cpp`

### Haunt

- Iron/Global: impact damage bonus = `10 * Total mastery levels` percent.
- Bronze: periodic amplification `20% -> 100%`.
- Silver: return heal `100% -> 200%`.
- Gold: cooldown reduction up to `-5000ms`.
- Diamond: refresh and extend Warlock periodic DoTs (`1000ms -> 5000ms` extension).

### Shadow Bolt

- Iron/Global: damage bonus = `8 * Total mastery levels` percent.
- Bronze: mana refund `6% -> 24%`.
- Silver: splash damage `20% -> 50%`.
- Gold: scaling Shadow DoT per tick `0.5% -> 2.75%` of hit.
- Diamond: extra targets `1 -> 10`.

### Chaos Bolt

- Iron/Global: damage bonus = `6 * Total mastery levels` percent.
- Bronze: mana refund `5% -> 20%`.
- Silver: crit chance `5% -> 30%`.
- Gold: execute bonus `10% -> 40%`.
- Diamond: extra bolt targets `1 -> 4`.

### Rain of Fire

- Iron/Global: damage bonus = `5 * Total mastery levels` percent.
- Bronze: radius multiplier `x1.05 -> x1.50`.
- Silver: bonus vs your Corruption `8% -> 25%`.
- Gold:
  - splash chance `8% -> 30%`
  - splash damage `25% -> 50%`.
- Diamond: additional Corruption synergy bonus `15% -> 50%`.
- Extra scaling: direct spell power contribution = `15%` of caster spell power per hit.

## Druid

Source: `src/server/scripts/World/spell_mastery_druid.cpp`

### Rejuvenation

- Iron/Global heal ramp: `40*Iron + 40*Bronze + 40*Silver + 50*Gold + 60*Diamond`.
- Bronze:
  - duration bonus `300ms -> 3000ms`
  - extra ticks at Bronze level `5` and `10`.
- Silver: splash heal `15% -> 40%`.
- Gold:
  - stack heal bonus = `8%`
  - max stacks `2 -> 6`.
- Diamond: bonus tick `20% -> 80%`.

### Regrowth

- Iron/Global heal ramp (direct and HoT): `40*Iron + 40*Bronze + 40*Silver + 50*Gold + 60*Diamond`.
- Bronze:
  - duration bonus `250ms -> 2500ms`
  - extra ticks at Bronze level `5` and `10`.
- Silver: splash heal `12% -> 32%`.
- Gold:
  - stack heal bonus = `10%`
  - max stacks `2 -> 6`.
- Diamond: bonus direct heal `20% -> 80%`.

### Rip

- Iron/Global: damage bonus = `8 * Total mastery levels` percent.
- Bronze: tick interval becomes `500ms`.
- Silver: damage-taken debuff `1% -> 10%`.
- Gold: duration bonus `250ms -> 2500ms`.
- Diamond: full-damage scaling at 1 combo point enabled.

### Swipe (Cat)

- Iron: energy cost reduction `2 -> 20`.
- Bronze/Global: damage bonus = `4 * Total mastery levels` percent.
- Silver: energy refund `2 -> 11`.
- Gold: bleed amount `10% -> 40%`.
- Diamond: self-heal from hit damage `8% -> 30%`.
