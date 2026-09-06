# PvP Life

Repository/module name: `mod-playerbots-pvp-life`

Created by **iCore**.

An AzerothCore + mod-playerbots module focused on making the open world feel active through persistent, configurable bot PvP rather than isolated scripted battles.

 It is intentionally split into independent systems so each part can be enabled, tuned, or disabled without affecting the others.

### Included systems

- **Persistent World PvP**: keeps a configurable number of PvP hotspots active.
- **Duel Zones**: near-continuous bot-vs-bot duel activity around configured locations such as Stormwind and Orgrimmar; idle bots are repeatedly re-paired after a cooldown.
- **Bot -> real player duel challenges**: bots can challenge nearby real players with strict anti-spam cooldowns and a configurable level-difference check.
- **For the Horde**: Horde bots can push into Alliance territory/capital areas while Alliance bots act as defenders.
- **For the Alliance**: Alliance bots can push into Horde territory/capital areas while Horde bots act as defenders.
- **Natural defender chat**: optional bot `/yell` and world-chat style reactions such as `horde in sw` or `ally in org`.
- **Optional announcements**: server-wide scripted announcements exist for debugging/information but are disabled by default.
- **Randomisation**: bot counts, duration, arrival timing, movement and position jitter are varied to reduce scripted-looking behaviour.
- **Activity protection**: bots in combat, battlegrounds, instances, flight, duels, or (by default) groups are rejected; the selector also respects Playerbots' own `AllowActivity(ALL_ACTIVITY)` state.
- **Return handling**: participants can be sent back to their previous location when their activity ends.

## Requirements

- AzerothCore WotLK using the `mod-playerbots` compatible Playerbot core branch.
- `mod-playerbots` installed and working.
- C++17-capable build environment used by current AzerothCore.

## Installation

1. Copy/clone the folder into:

   `azerothcore-wotlk/modules/mod-playerbots-pvp-life`

2. Apply to the **WORLD** database:

   `data/sql/manual/world_pvp_life.sql`

3. Re-run CMake and rebuild the server. Current AzerothCore discovers the module source files and `conf/*.conf.dist` automatically. The module loader entry point is derived from the exact folder name `mod-playerbots-pvp-life`.

4. Copy the generated config `.dist` to a normal `.conf` if your setup does not do this automatically, then edit as required:

   `mod_playerbots_pvp_life.conf`

5. Start worldserver and check the log for `[PvPLife]`.

## Main configuration

The most important defaults are:

```ini
PvPLife.Enable = 1
PvPLife.Announce = 0

PvPLife.World.AlwaysActive = 1
PvPLife.World.MinActiveHotspots = 1
PvPLife.World.MaxActiveHotspots = 3

PvPLife.Duel.Enable = 1
PvPLife.Duel.AlwaysActive = 1
PvPLife.Duel.ChallengeRealPlayers = 1
PvPLife.Duel.MaxLevelDifference = 5
PvPLife.Duel.BotVsBotMaxLevelDifference = 5
PvPLife.Duel.PlayerChallengeCooldownSeconds = 900

PvPLife.ForTheHorde.Enable = 1
PvPLife.ForTheAlliance.Enable = 1

PvPLife.BotChat.Enable = 1
```

### Level check example

With:

```ini
PvPLife.Duel.MaxLevelDifference = 5
```

a level 10 bot can challenge only players within level 5-15. It cannot challenge a level 80 player. Bot-vs-bot duel pairing has the same idea through the separate `BotVsBotMaxLevelDifference` setting.

## Database-driven zones

`pvp_life_zone` stores the locations and behaviour of each activity.

Activity types:

- `0` = normal World PvP skirmish
- `1` = Duel Zone
- `2` = For the Horde
- `3` = For the Alliance

Team values:

- `0` = Alliance
- `1` = Horde
- `2` = Any

The SQL includes starter locations for:

- Stormwind duel area
- Orgrimmar duel area
- Stranglethorn / Nesingwary
- Gurubashi Arena
- Gadgetzan / Tanaris
- Dark Portal (Azeroth)
- Dark Portal (Outland)
- Shattrath outskirts
- K3 / Storm Peaks
- Horde roaming near Goldshire
- Alliance roaming near Durotar
- optional Orgrimmar zeppelin hotspot (disabled until tuned)
- For the Horde -> Stormwind
- For the Alliance -> Orgrimmar

**The seed coordinates are starting points.** Use the GM position commands below to tune exact rally/target spots on your own server/navmesh.

## GM commands

Root command:

`.pvplife`

Useful commands:

```text
.pvplife status
.pvplife start <zoneName>
.pvplife stop all
.pvplife stop <eventId>

.pvplife zone list
.pvplife zone create <name> <skirmish|duel|forthehorde|forthealliance> <alliance|horde|any> <alliance|horde|any> <minLevel> <maxLevel>
.pvplife zone rally <name>
.pvplife zone target <name>
.pvplife zone counts <name> <attackersMin> <attackersMax> <defendersMin> <defendersMax>
.pvplife zone duration <name> <minMinutes> <maxMinutes>
.pvplife zone weight <name> <value>
.pvplife zone cooldown <name> <seconds>
.pvplife zone challenge <name> <0|1>
.pvplife zone chat <name> <0|1>
.pvplife zone enable <name> <0|1>
```

`zone rally` and `zone target` save the executing GM's current map and position, making coordinate tuning quick in-game.

## Behaviour notes

### Duel zones

The duel system is meant to look like a populated PvP-server duel area, not a duel factory. Bots arrive with staggered timing and only a limited number of pairs are scheduled at once. Real-player challenges additionally require:

- same faction as the duel-zone bot,
- the player to be nearby,
- both characters to be alive and free to duel,
- level difference within the configured limit,
- player and bot challenge cooldowns to be clear.

### For the Horde / For the Alliance

These are deliberately part of the normal world-PvP framework, with faction-specific attackers and defenders rather than a separate minigame. Server announcements are off by default; optional defender yell/world-chat messages provide the visible clue that something is happening.


1. module loads and WORLD tables are detected;
2. `.pvplife status` works;
3. force-start `StormwindDuel` / `OrgrimmarDuel`;
4. verify bot-vs-bot duel requests;
5. verify real-player challenge level check and cooldown;
6. normal hotspots;
7. force-start `ForTheHorde_Stormwind` and `ForTheAlliance_Orgrimmar`;
8. tune rally/target coordinates and bot counts from actual server behaviour.

## Status

`v1.0-` — initial PvP Life implementation for compile/live 
