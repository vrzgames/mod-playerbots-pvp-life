/*
 * PvP Life
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PvPLifeMgr.h"

#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Random.h"
#include "SharedDefines.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <shared_mutex>

namespace
{
    std::string Lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    std::vector<std::string> Tokenize(std::string const& text)
    {
        std::istringstream stream(text);
        std::vector<std::string> out;
        std::string token;
        while (stream >> token)
            out.push_back(token);
        return out;
    }

    char const* TypeName(PvPLife::ActivityType type)
    {
        using namespace PvPLife;
        switch (type)
        {
            case ActivityType::Skirmish: return "Skirmish";
            case ActivityType::Duel: return "Duel";
            case ActivityType::ForTheHorde: return "ForTheHorde";
            case ActivityType::ForTheAlliance: return "ForTheAlliance";
        }
        return "Unknown";
    }

    char const* TeamName(PvPLife::TeamSide team)
    {
        using namespace PvPLife;
        switch (team)
        {
            case TeamSide::Alliance: return "Alliance";
            case TeamSide::Horde: return "Horde";
            default: return "Any";
        }
    }

    bool ParseTeam(std::string token, PvPLife::TeamSide& out)
    {
        using namespace PvPLife;
        token = Lower(token);
        if (token == "a" || token == "ally" || token == "alliance")
        {
            out = TeamSide::Alliance;
            return true;
        }
        if (token == "h" || token == "horde")
        {
            out = TeamSide::Horde;
            return true;
        }
        if (token == "any" || token == "both")
        {
            out = TeamSide::Any;
            return true;
        }
        return false;
    }

    bool ParseType(std::string token, PvPLife::ActivityType& out)
    {
        using namespace PvPLife;
        token = Lower(token);
        if (token == "skirmish" || token == "world" || token == "worldpvp")
        {
            out = ActivityType::Skirmish;
            return true;
        }
        if (token == "duel")
        {
            out = ActivityType::Duel;
            return true;
        }
        if (token == "forthehorde" || token == "fth")
        {
            out = ActivityType::ForTheHorde;
            return true;
        }
        if (token == "forthealliance" || token == "fta" || token == "fortheally")
        {
            out = ActivityType::ForTheAlliance;
            return true;
        }
        return false;
    }
}

namespace PvPLife
{
    Manager& Manager::Instance()
    {
        static Manager instance;
        return instance;
    }

    void Manager::LoadConfig()
    {
        _enable = sConfigMgr->GetOption<bool>("PvPLife.Enable", true);
        _debug = sConfigMgr->GetOption<bool>("PvPLife.Debug", false);
        _announce = sConfigMgr->GetOption<bool>("PvPLife.Announce", false);
        _startupDelaySeconds = sConfigMgr->GetOption<uint32>("PvPLife.StartupDelaySeconds", 90);
        _tickSeconds = std::max<uint32>(5, sConfigMgr->GetOption<uint32>("PvPLife.TickSeconds", 20));

        _usePlayerbotConfig = sConfigMgr->GetOption<bool>("PvPLife.Bots.UsePlayerbotConfig", true);
        _botAccountPrefix = sConfigMgr->GetOption<std::string>("PvPLife.Bots.AccountPrefix", "auto");
        _botAccountMin = sConfigMgr->GetOption<uint32>("PvPLife.Bots.AccountMin", 0);
        _botAccountMax = sConfigMgr->GetOption<uint32>("PvPLife.Bots.AccountMax", 0);
        _botQueryLimit = std::max<uint32>(50, sConfigMgr->GetOption<uint32>("PvPLife.Bots.QueryLimit", 500));
        _skipGroupedBots = sConfigMgr->GetOption<bool>("PvPLife.Bots.SkipGrouped", true);
        _returnBots = sConfigMgr->GetOption<bool>("PvPLife.Bots.ReturnAfterActivity", true);
        _useMovePoint = sConfigMgr->GetOption<bool>("PvPLife.Movement.UseMovePoint", true);
        _positionJitter = sConfigMgr->GetOption<uint32>("PvPLife.Movement.PositionJitter", 10);
        _arrivalStaggerMin = sConfigMgr->GetOption<uint32>("PvPLife.Movement.ArrivalStaggerMinSeconds", 2);
        _arrivalStaggerMax = sConfigMgr->GetOption<uint32>("PvPLife.Movement.ArrivalStaggerMaxSeconds", 12);
        _moveAfterArrivalMin = sConfigMgr->GetOption<uint32>("PvPLife.Movement.MoveAfterArrivalMinSeconds", 6);
        _moveAfterArrivalMax = sConfigMgr->GetOption<uint32>("PvPLife.Movement.MoveAfterArrivalMaxSeconds", 20);
        _maxBotsPerSide = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Bots.MaxPerSide", 30), 1, 40);

        _alwaysActiveWorldPvp = sConfigMgr->GetOption<bool>("PvPLife.World.AlwaysActive", true);
        _minActiveSkirmishes = sConfigMgr->GetOption<uint32>("PvPLife.World.MinActiveHotspots", 1);
        _maxActiveSkirmishes = std::max(_minActiveSkirmishes,
            sConfigMgr->GetOption<uint32>("PvPLife.World.MaxActiveHotspots", 3));
        _randomSkirmishChance = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("PvPLife.World.RandomStartChance", 35));

        _duelEnable = sConfigMgr->GetOption<bool>("PvPLife.Duel.Enable", true);
        _duelAlwaysActive = sConfigMgr->GetOption<bool>("PvPLife.Duel.AlwaysActive", true);
        _duelSpellId = sConfigMgr->GetOption<uint32>("PvPLife.Duel.SpellId", 7266);
        _duelPairLimit = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PvPLife.Duel.PairLimit", 12), 1, 40);
        _duelPairDelayMin = sConfigMgr->GetOption<uint32>("PvPLife.Duel.PairDelayMinSeconds", 5);
        _duelPairDelayMax = sConfigMgr->GetOption<uint32>("PvPLife.Duel.PairDelayMaxSeconds", 18);
        _challengeRealPlayers = sConfigMgr->GetOption<bool>("PvPLife.Duel.ChallengeRealPlayers", true);
        _realPlayerChallengeChance = std::min<uint32>(100,
            sConfigMgr->GetOption<uint32>("PvPLife.Duel.PlayerChallengeChance", 25));
        _realPlayerScanRadius = sConfigMgr->GetOption<uint32>("PvPLife.Duel.PlayerScanRadius", 55);
        _realPlayerMaxLevelDifference = sConfigMgr->GetOption<uint32>("PvPLife.Duel.MaxLevelDifference", 5);
        _realPlayerCooldownSeconds = sConfigMgr->GetOption<uint32>("PvPLife.Duel.PlayerChallengeCooldownSeconds", 900);
        _botChallengeCooldownSeconds = sConfigMgr->GetOption<uint32>("PvPLife.Duel.BotChallengeCooldownSeconds", 180);
        _botDuelCooldownSeconds = sConfigMgr->GetOption<uint32>("PvPLife.Duel.BotVsBotCooldownSeconds", 45);
        _botDuelMaxLevelDifference = sConfigMgr->GetOption<uint32>("PvPLife.Duel.BotVsBotMaxLevelDifference", 5);

        _forTheHordeEnable = sConfigMgr->GetOption<bool>("PvPLife.ForTheHorde.Enable", true);
        _forTheAllianceEnable = sConfigMgr->GetOption<bool>("PvPLife.ForTheAlliance.Enable", true);
        _factionCampaignChance = std::min<uint32>(100,
            sConfigMgr->GetOption<uint32>("PvPLife.FactionCampaign.ChancePerTick", 12));
        _factionCampaignGlobalCooldown = sConfigMgr->GetOption<uint32>("PvPLife.FactionCampaign.GlobalCooldownSeconds", 3600);

        _botChatEnable = sConfigMgr->GetOption<bool>("PvPLife.BotChat.Enable", true);
        _botChatYellChance = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("PvPLife.BotChat.YellChance", 70));
        _botChatWorldChance = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("PvPLife.BotChat.WorldChance", 35));

        _pvpCombatStrategies = sConfigMgr->GetOption<std::string>("PvPLife.Strategies.Combat", "+pvp,+boost,+dps debuff,-passive,-stay");
        _pvpNonCombatStrategies = sConfigMgr->GetOption<std::string>("PvPLife.Strategies.NonCombat", "+pvp,+boost,-passive,-stay");
        _duelNonCombatStrategies = sConfigMgr->GetOption<std::string>("PvPLife.Strategies.DuelNonCombat", "+duel,+pvp,+boost,-passive,-stay");

        if (_arrivalStaggerMax < _arrivalStaggerMin)
            _arrivalStaggerMax = _arrivalStaggerMin;
        if (_moveAfterArrivalMax < _moveAfterArrivalMin)
            _moveAfterArrivalMax = _moveAfterArrivalMin;
        if (_duelPairDelayMax < _duelPairDelayMin)
            _duelPairDelayMax = _duelPairDelayMin;

        ImportPlayerbotDefaults();
        _botAccountCache.clear();
        _startupElapsedMs = 0;
        _timerMs = 0;

        if (_enable && !VerifyDatabase())
            _enable = false;

        LOG_INFO("module", "[PvPLife] enable={} alwaysWorld={} duel={} FTH={} FTA={} botChat={} announce={} prefix='{}'",
            _enable ? 1 : 0, _alwaysActiveWorldPvp ? 1 : 0, _duelEnable ? 1 : 0,
            _forTheHordeEnable ? 1 : 0, _forTheAllianceEnable ? 1 : 0,
            _botChatEnable ? 1 : 0, _announce ? 1 : 0, _botAccountPrefix);
    }

    void Manager::ImportPlayerbotDefaults()
    {
        if (!_usePlayerbotConfig)
            return;

        std::string configuredPrefix = sConfigMgr->GetOption<std::string>("AiPlayerbot.RandomBotAccountPrefix", "rndbot");
        uint32 loginDelay = sConfigMgr->GetOption<uint32>("AiPlayerbot.DisabledWithoutRealPlayerLoginDelay", 30);
        std::string lowered = Lower(_botAccountPrefix);
        if (lowered.empty() || lowered == "auto" || lowered == "playerbot" || lowered == "playerbots")
            _botAccountPrefix = configuredPrefix;
        _startupDelaySeconds = std::max(_startupDelaySeconds, loginDelay);
    }

    bool Manager::VerifyDatabase()
    {
        if (!WorldDatabase.Query("SHOW TABLES LIKE 'pvp_life_zone'"))
        {
            LOG_ERROR("module", "[PvPLife] Missing WORLD table pvp_life_zone. Apply data/sql/manual/world_pvp_life.sql");
            return false;
        }
        if (!WorldDatabase.Query("SHOW TABLES LIKE 'pvp_life_chat'"))
        {
            LOG_ERROR("module", "[PvPLife] Missing WORLD table pvp_life_chat. Apply data/sql/manual/world_pvp_life.sql");
            return false;
        }
        return true;
    }

    std::string Manager::ZoneSelectSql(std::string const& suffix) const
    {
        return "SELECT id,name,enabled,activity_type,attacker_team,defender_team,min_level,max_level,map_id,"
               "rally_x,rally_y,rally_z,rally_o,target_x,target_y,target_z,target_o,"
               "attackers_min,attackers_max,defenders_min,defenders_max,duration_min,duration_max,weight,"
               "cooldown_seconds,last_start,challenge_players,bot_chat FROM pvp_life_zone " + suffix;
    }

    Zone Manager::ReadZone(Field* f)
    {
        Zone z;
        z.Id = f[0].Get<uint32>();
        z.Name = f[1].Get<std::string>();
        z.Enabled = f[2].Get<uint8>() != 0;
        z.Type = static_cast<ActivityType>(f[3].Get<uint8>());
        z.AttackerTeam = static_cast<TeamSide>(f[4].Get<uint8>());
        z.DefenderTeam = static_cast<TeamSide>(f[5].Get<uint8>());
        z.MinLevel = f[6].Get<uint8>();
        z.MaxLevel = f[7].Get<uint8>();
        z.MapId = f[8].Get<uint32>();
        z.RallyX = f[9].Get<float>(); z.RallyY = f[10].Get<float>(); z.RallyZ = f[11].Get<float>(); z.RallyO = f[12].Get<float>();
        z.TargetX = f[13].Get<float>(); z.TargetY = f[14].Get<float>(); z.TargetZ = f[15].Get<float>(); z.TargetO = f[16].Get<float>();
        z.AttackersMin = f[17].Get<uint32>(); z.AttackersMax = f[18].Get<uint32>();
        z.DefendersMin = f[19].Get<uint32>(); z.DefendersMax = f[20].Get<uint32>();
        z.DurationMin = f[21].Get<uint32>(); z.DurationMax = f[22].Get<uint32>();
        z.Weight = f[23].Get<uint32>(); z.CooldownSeconds = f[24].Get<uint32>(); z.LastStart = f[25].Get<uint32>();
        z.ChallengePlayers = f[26].Get<uint8>() != 0;
        z.BotChat = f[27].Get<uint8>() != 0;
        return z;
    }

    std::vector<Zone> Manager::LoadZones(bool readyOnly, int typeFilter)
    {
        std::vector<Zone> zones;
        std::string where = "WHERE enabled=1";
        if (typeFilter >= 0)
            where += " AND activity_type=" + std::to_string(typeFilter);
        if (readyOnly)
        {
            uint32 now = GameTime::GetGameTime().count();
            where += " AND (last_start=0 OR last_start+cooldown_seconds<=" + std::to_string(now) + ")";
        }
        where += " ORDER BY weight DESC,id ASC";
        QueryResult result = WorldDatabase.Query(ZoneSelectSql(where).c_str());
        if (!result)
            return zones;
        do
        {
            Zone z = ReadZone(result->Fetch());
            if (z.Weight > 0)
                zones.push_back(z);
        } while (result->NextRow());
        return zones;
    }

    bool Manager::LoadZoneByName(std::string name, Zone& out)
    {
        WorldDatabase.EscapeString(name);
        QueryResult result = WorldDatabase.Query(ZoneSelectSql("WHERE name='" + name + "' LIMIT 1").c_str());
        if (!result)
            return false;
        out = ReadZone(result->Fetch());
        return true;
    }

    TeamSide Manager::TeamForRace(uint8 race) const
    {
        switch (race)
        {
            case RACE_HUMAN:
            case RACE_DWARF:
            case RACE_NIGHTELF:
            case RACE_GNOME:
            case RACE_DRAENEI:
                return TeamSide::Alliance;
            case RACE_ORC:
            case RACE_UNDEAD_PLAYER:
            case RACE_TAUREN:
            case RACE_TROLL:
            case RACE_BLOODELF:
                return TeamSide::Horde;
            default:
                return TeamSide::Any;
        }
    }

    Player* Manager::FindPlayer(uint32 guidLow) const
    {
        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(guidLow);
        if (Player* p = ObjectAccessor::FindConnectedPlayer(guid))
            return p;
        return ObjectAccessor::FindPlayer(guid);
    }

    bool Manager::IsConfiguredBotAccount(uint32 accountId)
    {
        if (!accountId)
            return false;
        auto cached = _botAccountCache.find(accountId);
        if (cached != _botAccountCache.end())
            return cached->second;

        bool rangeOk = _botAccountMax > 0 && _botAccountMax >= _botAccountMin && accountId >= _botAccountMin && accountId <= _botAccountMax;
        bool prefixOk = false;
        if (!_botAccountPrefix.empty())
        {
            QueryResult result = LoginDatabase.Query("SELECT username FROM account WHERE id={} LIMIT 1", accountId);
            if (result)
            {
                std::string username = Lower(result->Fetch()[0].Get<std::string>());
                std::string prefix = Lower(_botAccountPrefix);
                prefixOk = username.rfind(prefix, 0) == 0;
            }
        }
        bool ok = rangeOk || prefixOk;
        _botAccountCache[accountId] = ok;
        return ok;
    }

    bool Manager::IsRealPlayer(Player* player) const
    {
        if (!player || !player->GetSession())
            return false;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        return !ai || ai->IsRealPlayer();
    }

    bool Manager::IsSafeBot(Player* player) const
    {
        if (!player || !player->GetSession() || !player->IsInWorld() || player->IsBeingTeleported())
            return false;
        if (!const_cast<Manager*>(this)->IsConfiguredBotAccount(player->GetSession()->GetAccountId()))
            return false;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        if (!ai || ai->IsRealPlayer() || ai->HasRealPlayerMaster())
            return false;
        if (!ai->AllowActivity(ALL_ACTIVITY))
            return false;
        if (!player->IsAlive() || player->IsInCombat() || player->IsInFlight() || player->duel)
            return false;
        if (player->InBattleground() || player->InBattlegroundQueue())
            return false;
        if (player->GetMap() && player->GetMap()->Instanceable())
            return false;
        if (_skipGroupedBots && player->GetGroup())
            return false;
        return true;
    }

    bool Manager::IsParticipant(uint32 guidLow) const
    {
        for (ActiveEvent const& e : _activeEvents)
            for (Participant const& p : e.Members)
                if (p.Bot.GuidLow == guidLow)
                    return true;
        return false;
    }

    std::vector<BotCandidate> Manager::LoadCandidates(TeamSide team, uint8 minLevel, uint8 maxLevel,
        std::unordered_set<uint32> const& excluded)
    {
        std::vector<BotCandidate> candidates;
        if (maxLevel < minLevel)
            return candidates;

        QueryResult result = CharacterDatabase.Query(
            "SELECT guid,account,name,level,race,class FROM characters WHERE online=1 AND level BETWEEN {} AND {} ORDER BY RAND() LIMIT {}",
            minLevel, maxLevel, _botQueryLimit);
        if (!result)
            return candidates;

        do
        {
            Field* f = result->Fetch();
            BotCandidate b;
            b.GuidLow = f[0].Get<uint32>();
            b.AccountId = f[1].Get<uint32>();
            b.Name = f[2].Get<std::string>();
            b.Level = f[3].Get<uint8>();
            b.Race = f[4].Get<uint8>();
            b.Class = f[5].Get<uint8>();
            b.Team = TeamForRace(b.Race);

            if (excluded.find(b.GuidLow) != excluded.end() || IsParticipant(b.GuidLow))
                continue;
            if (team != TeamSide::Any && b.Team != team)
                continue;
            if (!IsConfiguredBotAccount(b.AccountId))
                continue;
            if (!IsSafeBot(FindPlayer(b.GuidLow)))
                continue;
            candidates.push_back(b);
        } while (result->NextRow());

        return candidates;
    }

    float Manager::Jitter() const
    {
        if (!_positionJitter)
            return 0.0f;
        return static_cast<float>(irand(-static_cast<int32>(_positionJitter), static_cast<int32>(_positionJitter)));
    }

    uint32 Manager::RandomCount(uint32 minCount, uint32 maxCount) const
    {
        minCount = std::min(minCount, _maxBotsPerSide);
        maxCount = std::min(maxCount, _maxBotsPerSide);
        if (maxCount < minCount)
            maxCount = minCount;
        return urand(minCount, maxCount);
    }

    void Manager::ApplyPvpStrategies(Player* player, bool duelMode) const
    {
        if (!player)
            return;
        player->SetPvP(true);
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
        {
            ai->ChangeStrategy(_pvpCombatStrategies, BOT_STATE_COMBAT);
            ai->ChangeStrategy(duelMode ? _duelNonCombatStrategies : _pvpNonCombatStrategies, BOT_STATE_NON_COMBAT);
        }
    }

    void Manager::ScheduleMove(uint64 eventId, uint32 guidLow, uint32 mapId, float x, float y, float z, float o,
        uint32 delaySeconds, bool teleport)
    {
        MoveStep step;
        step.EventId = eventId;
        step.GuidLow = guidLow;
        step.ExecuteAt = GameTime::GetGameTime().count() + delaySeconds;
        step.MapId = mapId;
        step.X = x; step.Y = y; step.Z = z; step.O = o;
        step.Teleport = teleport;
        _moveQueue.push_back(step);
    }

    void Manager::ScheduleDuel(uint64 eventId, uint32 challengerGuid, uint32 targetGuid, uint32 delaySeconds,
        bool realPlayerTarget)
    {
        DuelStep step;
        step.EventId = eventId;
        step.ChallengerGuidLow = challengerGuid;
        step.TargetGuidLow = targetGuid;
        step.ExecuteAt = GameTime::GetGameTime().count() + delaySeconds;
        step.TargetIsRealPlayer = realPlayerTarget;
        _duelQueue.push_back(step);
    }

    void Manager::MoveBot(Player* player, uint32 mapId, float x, float y, float z, float o, bool teleport) const
    {
        if (!player)
            return;
        if (teleport || player->GetMapId() != mapId || !_useMovePoint)
            player->TeleportTo(mapId, x, y, z, o);
        else
            player->GetMotionMaster()->MovePoint(0, x, y, z);
    }

    bool Manager::StartZone(Zone const& zone, ChatHandler* handler, bool forced)
    {
        if (!zone.Enabled && !forced)
            return false;
        if (zone.Type == ActivityType::Duel && !_duelEnable && !forced)
            return false;
        if (zone.Type == ActivityType::ForTheHorde && !_forTheHordeEnable && !forced)
            return false;
        if (zone.Type == ActivityType::ForTheAlliance && !_forTheAllianceEnable && !forced)
            return false;

        for (ActiveEvent const& e : _activeEvents)
            if (e.EventZone.Id == zone.Id)
                return false;

        std::unordered_set<uint32> excluded;
        for (ActiveEvent const& e : _activeEvents)
            for (Participant const& p : e.Members)
                excluded.insert(p.Bot.GuidLow);

        uint32 attackerNeed = RandomCount(zone.AttackersMin, zone.AttackersMax);
        uint32 defenderNeed = RandomCount(zone.DefendersMin, zone.DefendersMax);
        std::vector<BotCandidate> attackers = LoadCandidates(zone.AttackerTeam, zone.MinLevel, zone.MaxLevel, excluded);
        if (attackers.size() > attackerNeed)
            attackers.resize(attackerNeed);
        for (BotCandidate const& b : attackers)
            excluded.insert(b.GuidLow);
        std::vector<BotCandidate> defenders = LoadCandidates(zone.DefenderTeam, zone.MinLevel, zone.MaxLevel, excluded);
        if (defenders.size() > defenderNeed)
            defenders.resize(defenderNeed);

        if (attackers.size() < attackerNeed || defenders.size() < defenderNeed)
        {
            if (_debug)
                LOG_INFO("module", "[PvPLife] '{}' lacks bots: attackers {}/{} defenders {}/{}",
                    zone.Name, attackers.size(), attackerNeed, defenders.size(), defenderNeed);
            if (handler)
                handler->PSendSysMessage("PvPLife: not enough eligible bots for {} (A {}/{} D {}/{}).",
                    zone.Name, attackers.size(), attackerNeed, defenders.size(), defenderNeed);
            return false;
        }

        uint32 now = GameTime::GetGameTime().count();
        ActiveEvent event;
        event.EventId = _nextEventId++;
        event.EventZone = zone;
        event.StartedAt = now;
        uint32 dmin = std::max<uint32>(1, zone.DurationMin);
        uint32 dmax = std::max(dmin, zone.DurationMax);
        event.EndsAt = now + urand(dmin, dmax) * MINUTE;

        auto add = [&](BotCandidate const& bot, bool attacker, uint32 index)
        {
            Player* player = FindPlayer(bot.GuidLow);
            if (!IsSafeBot(player))
                return;

            Participant part;
            part.Bot = bot;
            part.Attacker = attacker;
            part.OriginalMap = player->GetMapId();
            part.OriginalX = player->GetPositionX();
            part.OriginalY = player->GetPositionY();
            part.OriginalZ = player->GetPositionZ();
            part.OriginalO = player->GetOrientation();
            event.Members.push_back(part);

            ApplyPvpStrategies(player, zone.Type == ActivityType::Duel);
            uint32 arrival = urand(_arrivalStaggerMin, _arrivalStaggerMax) + index;
            float sx = (attacker ? zone.RallyX : zone.TargetX) + Jitter();
            float sy = (attacker ? zone.RallyY : zone.TargetY) + Jitter();
            float sz = attacker ? zone.RallyZ : zone.TargetZ;
            float so = attacker ? zone.RallyO : zone.TargetO;
            ScheduleMove(event.EventId, bot.GuidLow, zone.MapId, sx, sy, sz, so, arrival, true);

            if (zone.Type != ActivityType::Duel && attacker)
            {
                uint32 moveDelay = arrival + urand(_moveAfterArrivalMin, _moveAfterArrivalMax);
                ScheduleMove(event.EventId, bot.GuidLow, zone.MapId,
                    zone.TargetX + Jitter(), zone.TargetY + Jitter(), zone.TargetZ, zone.TargetO, moveDelay, false);
            }
        };

        uint32 idx = 0;
        for (BotCandidate const& b : attackers)
            add(b, true, idx++);
        idx = 0;
        for (BotCandidate const& b : defenders)
            add(b, false, idx++);

        if (zone.Type == ActivityType::Duel)
        {
            uint32 base = _arrivalStaggerMax + 3;
            uint32 pairs = 0;
            std::unordered_set<uint32> usedDefenders;
            for (BotCandidate const& attacker : attackers)
            {
                if (pairs >= _duelPairLimit)
                    break;
                for (BotCandidate const& defender : defenders)
                {
                    if (usedDefenders.count(defender.GuidLow))
                        continue;
                    uint32 levelDiff = std::abs(static_cast<int32>(attacker.Level) - static_cast<int32>(defender.Level));
                    if (levelDiff > _botDuelMaxLevelDifference)
                        continue;
                    ScheduleDuel(event.EventId, attacker.GuidLow, defender.GuidLow,
                        base + urand(_duelPairDelayMin, _duelPairDelayMax) + pairs * 2);
                    usedDefenders.insert(defender.GuidLow);
                    ++pairs;
                    break;
                }
            }
        }

        _activeEvents.push_back(event);
        WorldDatabase.Execute("UPDATE pvp_life_zone SET last_start={} WHERE id={}", now, zone.Id);

        if (zone.BotChat && _botChatEnable)
            ScheduleEventChat(_activeEvents.back());

        std::string label = std::string(TypeName(zone.Type)) + " at " + zone.Name;
        LOG_INFO("module", "[PvPLife] START #{} {} attackers={} defenders={} duration={}m",
            event.EventId, label, attackers.size(), defenders.size(), (event.EndsAt - event.StartedAt) / MINUTE);
        if (_announce)
            Announce("[World PvP] " + label);
        if (handler)
            handler->PSendSysMessage("PvPLife: started #{} {} ({} + {} bots).",
                event.EventId, label, attackers.size(), defenders.size());
        return true;
    }

    void Manager::EndEvent(ActiveEvent const& event, std::string const& reason)
    {
        LOG_INFO("module", "[PvPLife] END #{} {} reason={}", event.EventId, event.EventZone.Name, reason);
        for (Participant const& part : event.Members)
        {
            Player* p = FindPlayer(part.Bot.GuidLow);
            if (!p || !p->GetSession() || p->IsBeingTeleported())
                continue;
            if (p->IsInCombat())
                p->CombatStop(true);
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
                ai->ResetStrategies();
            if (_returnBots)
                p->TeleportTo(part.OriginalMap, part.OriginalX, part.OriginalY, part.OriginalZ, part.OriginalO);
        }
    }

    void Manager::ExpireEvents()
    {
        uint32 now = GameTime::GetGameTime().count();
        std::vector<ActiveEvent> keep;
        for (ActiveEvent const& e : _activeEvents)
        {
            if (e.EndsAt <= now)
                EndEvent(e, "timer");
            else
                keep.push_back(e);
        }
        _activeEvents.swap(keep);
    }

    void Manager::ProcessMoveQueue()
    {
        uint32 now = GameTime::GetGameTime().count();
        std::vector<MoveStep> keep;
        for (MoveStep const& step : _moveQueue)
        {
            if (step.ExecuteAt > now)
            {
                keep.push_back(step);
                continue;
            }
            Player* p = FindPlayer(step.GuidLow);
            if (!p || p->IsBeingTeleported())
            {
                MoveStep retry = step;
                retry.ExecuteAt = now + 2;
                keep.push_back(retry);
                continue;
            }
            MoveBot(p, step.MapId, step.X, step.Y, step.Z, step.O, step.Teleport);
        }
        _moveQueue.swap(keep);
    }

    void Manager::ProcessDuelQueue()
    {
        uint32 now = GameTime::GetGameTime().count();
        std::vector<DuelStep> keep;
        for (DuelStep const& step : _duelQueue)
        {
            if (step.ExecuteAt > now)
            {
                keep.push_back(step);
                continue;
            }

            Player* challenger = FindPlayer(step.ChallengerGuidLow);
            Player* target = FindPlayer(step.TargetGuidLow);
            if (!challenger || !target || challenger->IsBeingTeleported() || target->IsBeingTeleported())
                continue;
            if (!challenger->IsAlive() || !target->IsAlive() || challenger->duel || target->duel || challenger->IsInCombat() || target->IsInCombat())
                continue;
            if (!IsConfiguredBotAccount(challenger->GetSession()->GetAccountId()))
                continue;
            if (!step.TargetIsRealPlayer && !IsConfiguredBotAccount(target->GetSession()->GetAccountId()))
                continue;

            if (challenger->GetMapId() != target->GetMapId() || challenger->GetDistance(target) > 30.0f)
            {
                DuelStep retry = step;
                if (retry.RetryCount++ < 12)
                {
                    retry.ExecuteAt = now + 3;
                    keep.push_back(retry);
                    if (challenger->GetMapId() == target->GetMapId())
                        challenger->GetMotionMaster()->MovePoint(0, target->GetPositionX() + 3.0f, target->GetPositionY() + 3.0f, target->GetPositionZ());
                }
                continue;
            }

            challenger->SetFacingToObject(target);
            target->SetFacingToObject(challenger);
            challenger->CastSpell(target, _duelSpellId, true);
            if (step.TargetIsRealPlayer)
            {
                _botChallengeCooldown[challenger->GetGUID().GetCounter()] = now + _botChallengeCooldownSeconds;
                _playerChallengeCooldown[target->GetGUID().GetCounter()] = now + _realPlayerCooldownSeconds;
            }
            else
            {
                _botDuelCooldown[challenger->GetGUID().GetCounter()] = now + _botDuelCooldownSeconds;
                _botDuelCooldown[target->GetGUID().GetCounter()] = now + _botDuelCooldownSeconds;
            }
        }
        _duelQueue.swap(keep);
    }

    std::string Manager::RandomChatLine(ActivityType type, ChatChannel channel, TeamSide speakerTeam)
    {
        QueryResult result = WorldDatabase.Query(
            "SELECT text,weight FROM pvp_life_chat WHERE enabled=1 AND activity_type={} AND channel={} AND (speaker_team={} OR speaker_team=2) ORDER BY id",
            static_cast<uint32>(type), static_cast<uint32>(channel), static_cast<uint32>(speakerTeam));
        if (!result)
            return {};

        struct Entry { std::string Text; uint32 Weight; };
        std::vector<Entry> entries;
        uint32 total = 0;
        do
        {
            Field* f = result->Fetch();
            Entry e{ f[0].Get<std::string>(), std::max<uint32>(1, f[1].Get<uint32>()) };
            total += e.Weight;
            entries.push_back(e);
        } while (result->NextRow());

        uint32 roll = urand(1, total);
        uint32 cursor = 0;
        for (Entry const& e : entries)
        {
            cursor += e.Weight;
            if (roll <= cursor)
                return e.Text;
        }
        return entries.empty() ? std::string() : entries.front().Text;
    }

    void Manager::ScheduleEventChat(ActiveEvent const& event)
    {
        if (event.EventZone.Type != ActivityType::ForTheHorde && event.EventZone.Type != ActivityType::ForTheAlliance)
            return;

        std::vector<uint32> defenders;
        for (Participant const& p : event.Members)
            if (!p.Attacker)
                defenders.push_back(p.Bot.GuidLow);
        if (defenders.empty())
            return;

        TeamSide team = event.EventZone.DefenderTeam;
        uint32 base = _arrivalStaggerMax + 8;
        if (urand(1, 100) <= _botChatYellChance)
        {
            std::string line = RandomChatLine(event.EventZone.Type, ChatChannel::Yell, team);
            if (!line.empty())
                _chatQueue.push_back({ event.EventId, defenders[urand(0, static_cast<uint32>(defenders.size() - 1))],
                    GameTime::GetGameTime().count() + base + urand(1, 8), ChatChannel::Yell, line });
        }
        if (urand(1, 100) <= _botChatWorldChance)
        {
            std::string line = RandomChatLine(event.EventZone.Type, ChatChannel::World, team);
            if (!line.empty())
                _chatQueue.push_back({ event.EventId, defenders[urand(0, static_cast<uint32>(defenders.size() - 1))],
                    GameTime::GetGameTime().count() + base + urand(8, 25), ChatChannel::World, line });
        }
    }

    void Manager::ProcessChatQueue()
    {
        uint32 now = GameTime::GetGameTime().count();
        std::vector<ChatStep> keep;
        for (ChatStep const& step : _chatQueue)
        {
            if (step.ExecuteAt > now)
            {
                keep.push_back(step);
                continue;
            }
            Player* speaker = FindPlayer(step.SpeakerGuidLow);
            if (!speaker)
                continue;
            PlayerbotAI* ai = GET_PLAYERBOT_AI(speaker);
            if (!ai)
                continue;
            if (step.Channel == ChatChannel::Yell)
                ai->Yell(step.Text);
            else
                ai->SayToWorld(step.Text);
        }
        _chatQueue.swap(keep);
    }

    void Manager::MaintainBotDuels()
    {
        if (!_duelEnable)
            return;

        uint32 now = GameTime::GetGameTime().count();
        std::unordered_set<uint32> queued;
        for (DuelStep const& step : _duelQueue)
        {
            queued.insert(step.ChallengerGuidLow);
            queued.insert(step.TargetGuidLow);
        }

        for (ActiveEvent const& event : _activeEvents)
        {
            if (event.EventZone.Type != ActivityType::Duel)
                continue;

            std::vector<uint32> available;
            for (Participant const& participant : event.Members)
            {
                uint32 guidLow = participant.Bot.GuidLow;
                if (queued.count(guidLow))
                    continue;

                Player* bot = FindPlayer(guidLow);
                if (!bot || !bot->IsAlive() || bot->IsBeingTeleported() || bot->duel || bot->IsInCombat())
                    continue;

                auto duelCd = _botDuelCooldown.find(guidLow);
                if (duelCd != _botDuelCooldown.end() && duelCd->second > now)
                    continue;
                auto playerCd = _botChallengeCooldown.find(guidLow);
                if (playerCd != _botChallengeCooldown.end() && playerCd->second > now)
                    continue;

                available.push_back(guidLow);
            }

            uint32 scheduled = 0;
            while (available.size() >= 2 && scheduled < _duelPairLimit)
            {
                uint32 firstIndex = urand(0, static_cast<uint32>(available.size() - 1));
                uint32 first = available[firstIndex];
                Player* a = FindPlayer(first);
                if (!a)
                {
                    available.erase(available.begin() + firstIndex);
                    continue;
                }

                std::vector<uint32> compatibleIndexes;
                for (uint32 i = 0; i < available.size(); ++i)
                {
                    if (i == firstIndex)
                        continue;
                    Player* candidate = FindPlayer(available[i]);
                    if (!candidate || a->GetTeamId() != candidate->GetTeamId())
                        continue;
                    uint32 levelDiff = std::abs(static_cast<int32>(a->GetLevel()) - static_cast<int32>(candidate->GetLevel()));
                    if (levelDiff <= _botDuelMaxLevelDifference)
                        compatibleIndexes.push_back(i);
                }

                if (compatibleIndexes.empty())
                {
                    available.erase(available.begin() + firstIndex);
                    continue;
                }

                uint32 secondIndex = compatibleIndexes[urand(0, static_cast<uint32>(compatibleIndexes.size() - 1))];
                uint32 second = available[secondIndex];

                if (secondIndex > firstIndex)
                {
                    available.erase(available.begin() + secondIndex);
                    available.erase(available.begin() + firstIndex);
                }
                else
                {
                    available.erase(available.begin() + firstIndex);
                    available.erase(available.begin() + secondIndex);
                }

                ScheduleDuel(event.EventId, first, second, urand(_duelPairDelayMin, _duelPairDelayMax));
                queued.insert(first);
                queued.insert(second);
                ++scheduled;
            }
        }
    }

    void Manager::ProcessRealPlayerChallenges()
    {
        if (!_duelEnable || !_challengeRealPlayers)
            return;

        uint32 now = GameTime::GetGameTime().count();

        for (ActiveEvent const& event : _activeEvents)
        {
            if (event.EventZone.Type != ActivityType::Duel || !event.EventZone.ChallengePlayers)
                continue;

            for (Participant const& participant : event.Members)
            {
                Player* bot = FindPlayer(participant.Bot.GuidLow);
                if (!bot || bot->duel || bot->IsInCombat() || bot->IsBeingTeleported())
                    continue;
                auto botCd = _botChallengeCooldown.find(participant.Bot.GuidLow);
                if (botCd != _botChallengeCooldown.end() && botCd->second > now)
                    continue;
                auto duelCd = _botDuelCooldown.find(participant.Bot.GuidLow);
                if (duelCd != _botDuelCooldown.end() && duelCd->second > now)
                    continue;
                if (urand(1, 100) > _realPlayerChallengeChance)
                    continue;

                std::vector<uint32> targets;
                {
                    std::shared_lock<std::shared_mutex> playerLock(*HashMapHolder<Player>::GetLock());
                    HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
                    for (auto const& pair : players)
                    {
                        Player* player = pair.second;
                        if (!IsRealPlayer(player) || !player->IsAlive() || player->IsGameMaster() || player->duel || player->IsInCombat())
                            continue;
                        if (player->GetMapId() != bot->GetMapId() || player->GetTeamId() != bot->GetTeamId())
                            continue;
                        if (bot->GetDistance(player) > static_cast<float>(_realPlayerScanRadius))
                            continue;
                        uint32 diff = std::abs(static_cast<int32>(bot->GetLevel()) - static_cast<int32>(player->GetLevel()));
                        if (diff > _realPlayerMaxLevelDifference)
                            continue;
                        uint32 playerGuid = player->GetGUID().GetCounter();
                        auto playerCd = _playerChallengeCooldown.find(playerGuid);
                        if (playerCd != _playerChallengeCooldown.end() && playerCd->second > now)
                            continue;
                        targets.push_back(playerGuid);
                    }
                }

                if (targets.empty())
                    continue;
                uint32 targetGuid = targets[urand(0, static_cast<uint32>(targets.size() - 1))];
                ScheduleDuel(event.EventId, bot->GetGUID().GetCounter(), targetGuid, urand(1, 4), true);
                _botChallengeCooldown[bot->GetGUID().GetCounter()] = now + _botChallengeCooldownSeconds;
                _playerChallengeCooldown[targetGuid] = now + _realPlayerCooldownSeconds;
            }
        }
    }

    void Manager::MaintainPersistentWorldPvp()
    {
        if (!_alwaysActiveWorldPvp)
            return;
        uint32 active = 0;
        std::unordered_set<uint32> activeIds;
        for (ActiveEvent const& e : _activeEvents)
        {
            if (e.EventZone.Type == ActivityType::Skirmish)
                ++active;
            activeIds.insert(e.EventZone.Id);
        }
        if (active >= _minActiveSkirmishes)
            return;

        std::vector<Zone> zones = LoadZones(true, static_cast<int>(ActivityType::Skirmish));
        zones.erase(std::remove_if(zones.begin(), zones.end(), [&](Zone const& z) { return activeIds.count(z.Id) != 0; }), zones.end());

        while (!zones.empty() && active < _minActiveSkirmishes && active < _maxActiveSkirmishes)
        {
            uint32 totalWeight = 0;
            for (Zone const& z : zones)
                totalWeight += std::max<uint32>(1, z.Weight);

            uint32 roll = urand(1, std::max<uint32>(1, totalWeight));
            size_t chosen = 0;
            uint32 cursor = 0;
            for (size_t i = 0; i < zones.size(); ++i)
            {
                cursor += std::max<uint32>(1, zones[i].Weight);
                if (roll <= cursor)
                {
                    chosen = i;
                    break;
                }
            }

            Zone zone = zones[chosen];
            zones.erase(zones.begin() + chosen);
            if (StartZone(zone))
            {
                ++active;
                activeIds.insert(zone.Id);
            }
        }
    }

    void Manager::MaintainPersistentDuelZones()
    {
        if (!_duelEnable || !_duelAlwaysActive)
            return;
        std::unordered_set<uint32> activeIds;
        for (ActiveEvent const& e : _activeEvents)
            if (e.EventZone.Type == ActivityType::Duel)
                activeIds.insert(e.EventZone.Id);

        std::vector<Zone> zones = LoadZones(true, static_cast<int>(ActivityType::Duel));
        for (Zone const& z : zones)
            if (activeIds.find(z.Id) == activeIds.end())
                StartZone(z);
    }

    void Manager::TryStartRandomSkirmish()
    {
        uint32 active = 0;
        std::unordered_set<uint32> activeIds;
        for (ActiveEvent const& e : _activeEvents)
        {
            if (e.EventZone.Type == ActivityType::Skirmish)
                ++active;
            activeIds.insert(e.EventZone.Id);
        }
        if (active >= _maxActiveSkirmishes || urand(1, 100) > _randomSkirmishChance)
            return;

        std::vector<Zone> zones = LoadZones(true, static_cast<int>(ActivityType::Skirmish));
        zones.erase(std::remove_if(zones.begin(), zones.end(), [&](Zone const& z) { return activeIds.count(z.Id) != 0; }), zones.end());
        if (zones.empty())
            return;

        uint32 total = 0;
        for (Zone const& z : zones) total += z.Weight;
        uint32 roll = urand(1, std::max<uint32>(1, total));
        uint32 cursor = 0;
        for (Zone const& z : zones)
        {
            cursor += z.Weight;
            if (roll <= cursor)
            {
                StartZone(z);
                return;
            }
        }
    }

    void Manager::TryStartFactionCampaign()
    {
        if ((!_forTheHordeEnable && !_forTheAllianceEnable) || urand(1, 100) > _factionCampaignChance)
            return;
        uint32 now = GameTime::GetGameTime().count();
        if (_lastFactionCampaign && _lastFactionCampaign + _factionCampaignGlobalCooldown > now)
            return;

        std::vector<Zone> zones;
        if (_forTheHordeEnable)
        {
            std::vector<Zone> fth = LoadZones(true, static_cast<int>(ActivityType::ForTheHorde));
            zones.insert(zones.end(), fth.begin(), fth.end());
        }
        if (_forTheAllianceEnable)
        {
            std::vector<Zone> fta = LoadZones(true, static_cast<int>(ActivityType::ForTheAlliance));
            zones.insert(zones.end(), fta.begin(), fta.end());
        }
        if (zones.empty())
            return;

        while (!zones.empty())
        {
            uint32 totalWeight = 0;
            for (Zone const& z : zones)
                totalWeight += std::max<uint32>(1, z.Weight);

            uint32 roll = urand(1, std::max<uint32>(1, totalWeight));
            size_t chosen = 0;
            uint32 cursor = 0;
            for (size_t i = 0; i < zones.size(); ++i)
            {
                cursor += std::max<uint32>(1, zones[i].Weight);
                if (roll <= cursor)
                {
                    chosen = i;
                    break;
                }
            }

            Zone zone = zones[chosen];
            zones.erase(zones.begin() + chosen);
            if (StartZone(zone))
            {
                _lastFactionCampaign = now;
                return;
            }
        }
    }

    void Manager::Announce(std::string const& text) const
    {
        if (!_announce)
            return;
        sWorldSessionMgr->SendServerMessage(SERVER_MSG_STRING, text);
    }

    void Manager::Update(uint32 diff)
    {
        if (!_enable)
            return;

        if (_startupElapsedMs < _startupDelaySeconds * IN_MILLISECONDS)
        {
            _startupElapsedMs += diff;
            return;
        }

        ProcessMoveQueue();
        ProcessDuelQueue();
        ProcessChatQueue();
        ExpireEvents();

        if (_timerMs <= diff)
        {
            _timerMs = _tickSeconds * IN_MILLISECONDS;
            MaintainPersistentDuelZones();
            MaintainPersistentWorldPvp();
            TryStartRandomSkirmish();
            TryStartFactionCampaign();
            MaintainBotDuels();
            ProcessRealPlayerChallenges();
        }
        else
            _timerMs -= diff;
    }

    void Manager::PrintStatus(ChatHandler* handler) const
    {
        handler->PSendSysMessage("PvPLife: active={} moves={} duels={} chat={}",
            _activeEvents.size(), _moveQueue.size(), _duelQueue.size(), _chatQueue.size());
        uint32 now = GameTime::GetGameTime().count();
        for (ActiveEvent const& e : _activeEvents)
        {
            uint32 left = e.EndsAt > now ? e.EndsAt - now : 0;
            handler->PSendSysMessage("#{} {} [{}] lvl {}-{} bots={} eta={}m",
                e.EventId, e.EventZone.Name, TypeName(e.EventZone.Type), e.EventZone.MinLevel, e.EventZone.MaxLevel,
                e.Members.size(), left / MINUTE);
        }
    }

    void Manager::PrintHelp(ChatHandler* handler) const
    {
        handler->PSendSysMessage("PvPLife commands:");
        handler->PSendSysMessage(".pvplife status");
        handler->PSendSysMessage(".pvplife start <zone>");
        handler->PSendSysMessage(".pvplife stop <id|all>");
        handler->PSendSysMessage(".pvplife zone list");
        handler->PSendSysMessage(".pvplife zone create <name> <skirmish|duel|forthehorde|forthealliance> <attacker> <defender> <minLvl> <maxLvl>");
        handler->PSendSysMessage(".pvplife zone rally <name> | target <name>   (saves GM position)");
        handler->PSendSysMessage(".pvplife zone counts <name> <attMin> <attMax> <defMin> <defMax>");
        handler->PSendSysMessage(".pvplife zone duration <name> <minM> <maxM>");
        handler->PSendSysMessage(".pvplife zone weight <name> <weight> | cooldown <name> <seconds>");
        handler->PSendSysMessage(".pvplife zone challenge <name> <0|1> | chat <name> <0|1> | enable <name> <0|1>");
    }

    bool Manager::HandleZoneCommand(ChatHandler* handler, std::vector<std::string> const& args)
    {
        if (args.size() < 2)
        {
            PrintHelp(handler);
            return true;
        }
        std::string action = Lower(args[1]);
        if (action == "list")
        {
            QueryResult result = WorldDatabase.Query(ZoneSelectSql("ORDER BY activity_type,name").c_str());
            if (!result)
            {
                handler->PSendSysMessage("PvPLife: no zones configured.");
                return true;
            }
            do
            {
                Zone z = ReadZone(result->Fetch());
                handler->PSendSysMessage("#{} {} [{}] en={} {}->{} lvl={}-{} bots={}-{} / {}-{} cd={} challenge={} chat={}",
                    z.Id, z.Name, TypeName(z.Type), z.Enabled ? 1 : 0, TeamName(z.AttackerTeam), TeamName(z.DefenderTeam),
                    z.MinLevel, z.MaxLevel, z.AttackersMin, z.AttackersMax, z.DefendersMin, z.DefendersMax,
                    z.CooldownSeconds, z.ChallengePlayers ? 1 : 0, z.BotChat ? 1 : 0);
            } while (result->NextRow());
            return true;
        }
        if (action == "create")
        {
            if (args.size() < 8)
            {
                handler->PSendSysMessage("Syntax: .pvplife zone create <name> <type> <attacker> <defender> <minLvl> <maxLvl>");
                return true;
            }
            Player* gm = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
            if (!gm)
            {
                handler->PSendSysMessage("This command needs an in-game GM position.");
                return true;
            }
            ActivityType type;
            TeamSide attacker, defender;
            if (!ParseType(args[3], type) || !ParseTeam(args[4], attacker) || !ParseTeam(args[5], defender))
            {
                handler->PSendSysMessage("Bad type/team value.");
                return true;
            }
            std::string name = args[2];
            WorldDatabase.EscapeString(name);
            uint32 minLevel = std::clamp<uint32>(std::stoul(args[6]), 1, 80);
            uint32 maxLevel = std::clamp<uint32>(std::stoul(args[7]), minLevel, 80);
            WorldDatabase.Execute(
                "INSERT INTO pvp_life_zone (name,enabled,activity_type,attacker_team,defender_team,min_level,max_level,map_id,"
                "rally_x,rally_y,rally_z,rally_o,target_x,target_y,target_z,target_o) VALUES ('{}',1,{},{},{},{},{},{},{},{},{},{},{},{},{},{}) "
                "ON DUPLICATE KEY UPDATE activity_type=VALUES(activity_type),attacker_team=VALUES(attacker_team),defender_team=VALUES(defender_team),"
                "min_level=VALUES(min_level),max_level=VALUES(max_level)",
                name, static_cast<uint32>(type), static_cast<uint32>(attacker), static_cast<uint32>(defender), minLevel, maxLevel,
                gm->GetMapId(), gm->GetPositionX(), gm->GetPositionY(), gm->GetPositionZ(), gm->GetOrientation(),
                gm->GetPositionX(), gm->GetPositionY(), gm->GetPositionZ(), gm->GetOrientation());
            handler->PSendSysMessage("PvPLife: created/updated {}. Set rally and target positions next.", args[2]);
            return true;
        }
        if ((action == "rally" || action == "target") && args.size() >= 3)
        {
            Player* gm = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
            if (!gm)
            {
                handler->PSendSysMessage("This command needs an in-game GM position.");
                return true;
            }
            std::string name = args[2];
            WorldDatabase.EscapeString(name);
            if (action == "rally")
            {
                WorldDatabase.Execute("UPDATE pvp_life_zone SET map_id={},rally_x={},rally_y={},rally_z={},rally_o={} WHERE name='{}'",
                    gm->GetMapId(), gm->GetPositionX(), gm->GetPositionY(), gm->GetPositionZ(), gm->GetOrientation(), name);
            }
            else
            {
                WorldDatabase.Execute("UPDATE pvp_life_zone SET map_id={},target_x={},target_y={},target_z={},target_o={} WHERE name='{}'",
                    gm->GetMapId(), gm->GetPositionX(), gm->GetPositionY(), gm->GetPositionZ(), gm->GetOrientation(), name);
            }
            handler->PSendSysMessage("PvPLife: saved {} point for {}.", action, args[2]);
            return true;
        }
        if (action == "counts" && args.size() >= 7)
        {
            std::string name = args[2]; WorldDatabase.EscapeString(name);
            WorldDatabase.Execute("UPDATE pvp_life_zone SET attackers_min={},attackers_max={},defenders_min={},defenders_max={} WHERE name='{}'",
                std::stoul(args[3]), std::stoul(args[4]), std::stoul(args[5]), std::stoul(args[6]), name);
            return true;
        }
        if (action == "duration" && args.size() >= 5)
        {
            std::string name = args[2]; WorldDatabase.EscapeString(name);
            WorldDatabase.Execute("UPDATE pvp_life_zone SET duration_min={},duration_max={} WHERE name='{}'",
                std::stoul(args[3]), std::stoul(args[4]), name);
            return true;
        }
        if ((action == "weight" || action == "cooldown" || action == "challenge" || action == "chat" || action == "enable") && args.size() >= 4)
        {
            std::string name = args[2]; WorldDatabase.EscapeString(name);
            uint32 value = std::stoul(args[3]);
            if (action == "weight")
                WorldDatabase.Execute("UPDATE pvp_life_zone SET weight={} WHERE name='{}'", value, name);
            else if (action == "cooldown")
                WorldDatabase.Execute("UPDATE pvp_life_zone SET cooldown_seconds={} WHERE name='{}'", value, name);
            else if (action == "challenge")
                WorldDatabase.Execute("UPDATE pvp_life_zone SET challenge_players={} WHERE name='{}'", value ? 1 : 0, name);
            else if (action == "chat")
                WorldDatabase.Execute("UPDATE pvp_life_zone SET bot_chat={} WHERE name='{}'", value ? 1 : 0, name);
            else
                WorldDatabase.Execute("UPDATE pvp_life_zone SET enabled={} WHERE name='{}'", value ? 1 : 0, name);
            return true;
        }
        PrintHelp(handler);
        return true;
    }

    bool Manager::HandleCommand(ChatHandler* handler, std::string const& text)
    {
        std::vector<std::string> args = Tokenize(text);
        if (!args.empty() && (Lower(args[0]) == ".pvplife" || Lower(args[0]) == "pvplife"))
            args.erase(args.begin());
        if (args.empty() || Lower(args[0]) == "help")
        {
            PrintHelp(handler);
            return true;
        }
        std::string cmd = Lower(args[0]);
        if (cmd == "status")
        {
            PrintStatus(handler);
            return true;
        }
        if (cmd == "zone")
            return HandleZoneCommand(handler, args);
        if (cmd == "start" && args.size() >= 2)
        {
            Zone zone;
            if (!LoadZoneByName(args[1], zone))
            {
                handler->PSendSysMessage("PvPLife: zone '{}' not found.", args[1]);
                return true;
            }
            StartZone(zone, handler, true);
            return true;
        }
        if (cmd == "stop")
        {
            if (args.size() < 2 || Lower(args[1]) == "all")
            {
                for (ActiveEvent const& e : _activeEvents)
                    EndEvent(e, "gm-stop");
                _activeEvents.clear();
                _moveQueue.clear();
                _duelQueue.clear();
                _chatQueue.clear();
                handler->PSendSysMessage("PvPLife: all activities stopped.");
                return true;
            }
            uint64 id = std::stoull(args[1]);
            bool found = false;
            std::vector<ActiveEvent> keep;
            for (ActiveEvent const& e : _activeEvents)
            {
                if (e.EventId == id)
                {
                    EndEvent(e, "gm-stop");
                    found = true;
                }
                else
                    keep.push_back(e);
            }
            _activeEvents.swap(keep);
            _moveQueue.erase(std::remove_if(_moveQueue.begin(), _moveQueue.end(), [id](MoveStep const& s) { return s.EventId == id; }), _moveQueue.end());
            _duelQueue.erase(std::remove_if(_duelQueue.begin(), _duelQueue.end(), [id](DuelStep const& s) { return s.EventId == id; }), _duelQueue.end());
            _chatQueue.erase(std::remove_if(_chatQueue.begin(), _chatQueue.end(), [id](ChatStep const& s) { return s.EventId == id; }), _chatQueue.end());
            handler->PSendSysMessage("PvPLife: {} #{}.", found ? "stopped" : "did not find", id);
            return true;
        }
        PrintHelp(handler);
        return true;
    }
}
