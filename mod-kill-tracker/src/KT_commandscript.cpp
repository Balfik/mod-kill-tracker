// GM command ".killtracker reload" - clears the in-memory kill-stats cache
// (KT_ResetCache() in kill_milestones.cpp). Useful if someone manually
// edited mq_kill_stats/mq_kill_stats_by_creature via mysql while the
// worldserver was already running - without this the player would see
// stale numbers until their next relog.

#include "Chat.h"
#include "CommandScript.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"

#include "kill_milestones.h"

using namespace Acore::ChatCommands;

class kt_commandscript : public CommandScript
{
public:
    kt_commandscript() : CommandScript("kt_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable killTrackerCommandTable =
        {
            { "reload", HandleReloadCommand, rbac::RBAC_PERM_COMMAND_RELOAD_ALL, Console::Yes },
        };

        static ChatCommandTable commandTable =
        {
            { "killtracker", killTrackerCommandTable },
            // A public (SEC_PLAYER - any player) command with no subtable -
            // called from the CLIENT addon KillTrackerHUD.lua (the [New
            // Contract] button on the Contract tab) via SendChatMessage
            // (".kthudreroll", "SAY") - the dot-command is intercepted by the
            // server BEFORE being broadcast as normal chat
            // (ChatHandler::ParseCommands in HandleMessagechatOpcode), so
            // nearby players never see it. Lets the player reroll the daily
            // contract right on the spot, without visiting the NPC - the
            // same KT_RerollContract used by the gossip menu.
            { "kthudreroll", HandleHudRerollCommand, SEC_PLAYER, Console::No },
        };
        return commandTable;
    }

    static bool HandleReloadCommand(ChatHandler* handler)
    {
        KT_ResetCache();
        handler->PSendSysMessage("|cff00ff00[Hunt Chronicler]|r kill-stats cache cleared - will be re-read from the DB on the next kill/menu open.");
        return true;
    }

    static bool HandleHudRerollCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return true;

        if (KT_RerollContract(player))
            handler->PSendSysMessage("|cff00ff00[Hunt Chronicler]|r daily contract reloaded.");
        else
            handler->PSendSysMessage(
                "|cff8b0000[Hunt Chronicler]|r couldn't reload the contract - either it's already been completed today, "
                "or there are no suitable targets in the current zone.");
        return true;
    }
};

void AddKillTrackerCommandScripts()
{
    new kt_commandscript();
}
