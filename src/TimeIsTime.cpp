/*
 * Azerothcore Module:     TimeIsTime
 * Author:                 Nathan Handley, Dunjeon
 * Contributing Author(s): lasyan3, vratam @ RegWorks
 * Version:                20250608
 * License:                GNU Affero General Public License v3.0.
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "Chat.h"
#include "GameTime.h"
#include "Opcodes.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <ctime>

static bool   stimeistime_enable,
              stimeistime_announce,
              stimeistime_realign_date;
static float  stimeistime_speed_rate,
              stimeistime_hour_offset,
              stimeistime_realign_lookahead;
static uint32 stimeistime_time_start;

// Key used to hang the per-player accelerated clock off Player::CustomData.
static char const* const TIMEISTIME_CLOCK_KEY = "TimeIsTimeClock";

// How often (ms of real time) a player's accelerated clock is polled for a day turnover.
// The packed time sent to the client only carries minute resolution, so a one second
// poll is finer than the client can represent even at the highest sane speed rates.
static constexpr uint32 TIMEISTIME_CHECK_INTERVAL = 1000;

// Tracks where a given player's client-side clock currently sits. The client ticks its
// own copy forward at the rate we hand it, so we only need the value last pushed plus
// the real time at which it was pushed to reconstruct it at any later moment.
struct TimeIsTimeClock : public DataMap::Base
{
    time_t baseClientTime = 0;  // accelerated game time last sent to the client
    time_t baseRealTime   = 0;  // real epoch second at which it was sent
    int32  lastDayIndex   = 0;  // local calendar day the client clock was last seen on
    uint32 checkTimer     = 0;  // ms accumulator throttling the day turnover poll
    bool   initialized    = false;
};

// Collapses a timestamp to a monotonic local-calendar day number, so day turnovers can be
// detected without mday wrapping at the end of a month or year.
static int32 TimeIsTimeLocalDayIndex(time_t when)
{
    tm local = Acore::Time::TimeBreakdown(when);
    return (local.tm_year * 1000) + local.tm_yday;
}

// The accelerated clock value a freshly connecting player should be handed.
static time_t TimeIsTimeBaseClientTime()
{
    if (stimeistime_time_start > 0)
        return time_t(stimeistime_time_start) + time_t(stimeistime_hour_offset * 3600.0f);

    time_t gameTime = GameTime::GetGameTime().count();
    time_t uptime   = GameTime::GetUptime().count();

    return (gameTime - uptime) + time_t(double(uptime) * double(stimeistime_speed_rate));
}

// Reconstructs where a player's client-side clock sits right now.
static time_t TimeIsTimeCurrentClientTime(TimeIsTimeClock const* clock, time_t realNow)
{
    return clock->baseClientTime + time_t(double(realNow - clock->baseRealTime) * double(stimeistime_speed_rate));
}

// Swaps the calendar date of an accelerated clock value for the server's real date, keeping
// the accelerated time of day untouched. The server date is sampled a configurable distance
// into the future so that a turnover landing a hair before real midnight still resolves to
// the day it is about to become rather than the one it is leaving.
static time_t TimeIsTimeRealignToServerDate(time_t clientTime, time_t realNow)
{
    time_t lookahead = time_t(stimeistime_realign_lookahead * 60.0f);
    if (lookahead < 0)
        lookahead = 0;

    tm client = Acore::Time::TimeBreakdown(clientTime);
    tm server = Acore::Time::TimeBreakdown(realNow + lookahead);

    client.tm_year  = server.tm_year;
    client.tm_mon   = server.tm_mon;
    client.tm_mday  = server.tm_mday;
    client.tm_isdst = -1;

    time_t realigned = mktime(&client);
    if (realigned == time_t(-1))
        return clientTime;

    return realigned;
}

static void TimeIsTimeSendTimeSpeed(Player* player, time_t clientTime)
{
    WorldPacket data(SMSG_LOGIN_SETTIMESPEED, 4 + 4 + 4);
    data.AppendPackedTime(clientTime);
    data << float(0.01666667f) * stimeistime_speed_rate;
    data << uint32(0);

    player->GetSession()->SendPacket(&data);
}

class TimeIsTimeBeforeConfigLoad : public WorldScript {
public:

    TimeIsTimeBeforeConfigLoad() : WorldScript("TimeIsTimeBeforeConfigLoad") { }

    void OnBeforeConfigLoad(bool /*reload*/) override {
        stimeistime_enable = sConfigMgr->GetOption<bool>("TimeIsTime.Enable", true);
        stimeistime_announce = sConfigMgr->GetOption<bool>("TimeIsTime.Announce", true);
        stimeistime_speed_rate = sConfigMgr->GetOption<float>("TimeIsTime.SpeedRate", 1.0);
        stimeistime_hour_offset = sConfigMgr->GetOption<float>("TimeIsTime.HourOffset", 0.0);
        stimeistime_time_start = sConfigMgr->GetOption<int>("TimeIsTime.TimeStart", 0);
        stimeistime_realign_date = sConfigMgr->GetOption<bool>("TimeIsTime.RealignDate", true);
        stimeistime_realign_lookahead = sConfigMgr->GetOption<float>("TimeIsTime.RealignDateLookahead", 1.0);
    }
};

class TimeIsTime : public PlayerScript {
public:

    TimeIsTime() : PlayerScript("TimeIsTime", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_SEND_INITIAL_PACKETS_BEFORE_ADD_TO_MAP,
        PLAYERHOOK_ON_UPDATE
    }) { }

    void OnPlayerLogin(Player* player) override {
        if (stimeistime_enable && stimeistime_announce)
            ChatHandler(player->GetSession()).SendSysMessage("This server is running the |cff4CFF00TimeIsTime |rmodule");
    }

    void OnPlayerSendInitialPacketsBeforeAddToMap(Player* player, WorldPacket& data) override {
        if (!stimeistime_enable)
            return;

        time_t real_now   = GameTime::GetGameTime().count();
        time_t speed_time = TimeIsTimeBaseClientTime();

        // Without this the date handed out at login is whatever the accelerated clock has
        // drifted to, which is already ahead of the server by the time the first accelerated
        // midnight has passed.
        if (stimeistime_realign_date)
            speed_time = TimeIsTimeRealignToServerDate(speed_time, real_now);

        data.Initialize(SMSG_LOGIN_SETTIMESPEED, 4 + 4 + 4);
        data.AppendPackedTime(speed_time);
        data << float(0.01666667f) * stimeistime_speed_rate;
        data << uint32(0);

        player->GetSession()->SendPacket(&data);

        TimeIsTimeClock* clock = player->CustomData.GetDefault<TimeIsTimeClock>(TIMEISTIME_CLOCK_KEY);
        clock->baseClientTime = speed_time;
        clock->baseRealTime   = real_now;
        clock->lastDayIndex   = TimeIsTimeLocalDayIndex(speed_time);
        clock->checkTimer     = 0;
        clock->initialized    = true;
    }

    void OnPlayerUpdate(Player* player, uint32 p_time) override {
        if (!stimeistime_enable || !stimeistime_realign_date)
            return;

        TimeIsTimeClock* clock = player->CustomData.Get<TimeIsTimeClock>(TIMEISTIME_CLOCK_KEY);
        if (!clock || !clock->initialized)
            return;

        clock->checkTimer += p_time;
        if (clock->checkTimer < TIMEISTIME_CHECK_INTERVAL)
            return;

        clock->checkTimer = 0;

        time_t real_now   = GameTime::GetGameTime().count();
        time_t client_now = TimeIsTimeCurrentClientTime(clock, real_now);

        int32 day_index = TimeIsTimeLocalDayIndex(client_now);
        if (day_index == clock->lastDayIndex)
            return;

        // The accelerated clock just rolled over midnight. Pin the date back to the server's
        // real one, which leaves the calendar parked until real time catches up.
        time_t realigned = TimeIsTimeRealignToServerDate(client_now, real_now);

        clock->lastDayIndex = TimeIsTimeLocalDayIndex(realigned);

        // At a speed rate of 1.0 the turnover already lands on the correct date, so there is
        // nothing to correct and the client is left alone.
        if (realigned == client_now)
            return;

        TimeIsTimeSendTimeSpeed(player, realigned);

        clock->baseClientTime = realigned;
        clock->baseRealTime   = real_now;
    }
};

void AddTimeIsTimeScripts() {
    new TimeIsTimeBeforeConfigLoad();
    new TimeIsTime();
}
