# Changelog

## Current

- Added native shared bot reservation support for conflict-free City Life compatibility.
- Added configurable total minimum and maximum bot population for every hotspot.
- Added per-zone configuration switches while keeping GM force-start available for testing.
- Added partial-team fallback and clearer selection diagnostics for smaller bot populations.
- Updated the Stormwind and Orgrimmar duel-zone seed locations.
- Reserved duel participants from random teleportation and anchored idle bots near their assigned duel positions.
- Added AzerothCore Playerbot and `mod-playerbots` dependency metadata and links.

## v0.1.1-test

- Fixed compatibility with the current mod-playerbots `ActivityType` enum by fully qualifying PvP Life activity values.
- Updated real-player checks for the current Playerbots API (`IsRealPlayer(Player*)` and `HasGameClientMaster()`).
- Added explicit game-time conversion to avoid MSVC narrowing errors in queued chat events.

## v0.1.0-test

- Initial PvP Life module.
- Persistent normal World PvP hotspots.
- Persistent Stormwind/Orgrimmar duel-zone framework.
- Bot-vs-bot duel scheduling with repeated pairing, cooldowns, and configurable maximum level difference.
- Bot-to-real-player duel challenges with level check and anti-spam cooldowns.
- For the Horde / For the Alliance faction activity types.
- Optional randomized defender yell/world chat.
- Optional server announcements, disabled by default.
- DB-driven zones and chat lines.
- Staggered arrivals, movement jitter and randomized activity duration/counts.
- Playerbots activity-state protection to avoid taking bots that are not available for general activity.
- GM status/start/stop/zone-edit commands.
