# mod-kill-tracker — Kill Tracking NPC + Companion Addons for AzerothCore WotLK 3.3.5a

A kill-tracking NPC module for **AzerothCore**, built for World of Warcraft **3.3.5a (WotLK)** private servers, plus two companion client addons.

The module adds a custom NPC, the **Hunt Chronicler**, who tracks every kill a player lands and grants a long list of permanent stat buffs, cosmetic rewards, and repeatable objectives as the player kills more enemies. Two Lua addons complement it on the client side: an on-screen HUD panel showing live progress, and a small helper that turns the NPC's chat-printed waypoints into real TomTom pins automatically.

This repository is a cleaned-up, English-only export of the module. It has no server-specific data baked in (no seeded characters, no hardcoded kill counts) — a fresh install starts every player at zero, same as any other module.

## Requirements

- **AzerothCore**, WotLK 3.3.5a core (a revision supporting the standard module system: ScriptMgr, GameEventMgr, LFGMgr, WorldSession)
- - A **World of Warcraft 3.3.5a (build 12340)** client, for the addons
  - - Optional: the **TomTom** addon, for KillTrackerWaypoint to auto-place map pins (KillTrackerHUD's own "Set Route" button also works without TomTom, since it calls the same TomTom slash command directly)
   
    - ## What the module does
   
    - - **Kill counting** — every kill (the player's own killing blow) is counted and split into normal / elite-rare / boss, tracked per character. Kills by party members within XP-sharing range also count toward the shared systems below (daily quota, daily contract, boss credit, elite credit), even if they are not the one who lands the killing blow.
      - - **Tier buffs, every 400 kills** — every 400 total kills unlocks a new permanent tier, up to tier 100 (40,000 kills). Each tier grants a flat, stacking bonus: +100 armor, +50 attack power (melee/ranged/spell), and +3% attack speed (melee/ranged/cast). Tiers never expire and are never removed; the bonus at tier N is simply N times the per-tier amount.
        - - **Cosmetic titles, every 10 tiers** — unlocks one of 10 flavor titles (shown in the NPC menu and chat, not a real client-side Title) plus a gold reward that scales with the title number.
          - - **Boss bonus** — every unique boss (creature rank WORLDBOSS) killed for the first time grants a permanent +5% to all stats, tracked independently per boss so party members who help kill a boss get credit too. The NPC's Boss Bonuses menu lists every spawned boss on the server, grouped by continent and location, shows which ones are already credited, and can print a TomTom pin (or the nearest known dungeon entrance) when you click one.
            - - **Dungeon/raid clear bonus** — clearing a dungeon or raid wing for the first time (defined the same way the standard Dungeon Finder reward system defines it, via the world database's `instance_encounters` table) grants a permanent +3% to all stats. The Cleared Dungeons menu lists everything by continent, with a manual toggle in case the automatic tracker cannot see a particular wing.
              - - **Elite/rare bonus** — every 50 elite or rare kills (any non-normal, non-boss creature) grants two parallel permanent bonuses: +3% to all stats, and +3% hit chance (melee/ranged combined) plus +3% spell hit chance. Caps at tier 100 (5,000 such kills).
                - - **Daily kill quota** — killing 100 enemies within a server day grants a permanent +1% XP buff, stacking up to +100%. The NPC and HUD both show today's progress.
                  - - **Daily contract** — once a day, the NPC (or the addon button) picks a random killable creature in the player's current zone as a one-time bounty, rewarding XP and gold on completion. It can be rerolled if an unsuitable target comes up, as long as it has not been completed yet that day. Completed contracts are kept in a history list in the NPC menu.
                    - - **Weekly contract** — once a week, a random tracked boss becomes a bonus target, worth twice the XP of an equivalent daily contract plus a flat gold reward. If the random boss is too high level for the player, the easiest available boss is substituted instead.
                      - - **Kill streak** — fifty kills in a row without dying grants a 15-minute temporary buff (+15% damage from all sources). Dying resets the streak counter.
                        - - **Skill and profession bonus** — for every 10 skill points in a tracked skill (primary and secondary professions, plus weapon and defense skills), the player gets +1 (a flat point, not a percent) to every stat. This recalculates live whenever a skill changes.
                          - - **Cosmetic trophies** — every 20 tiers adds a purely cosmetic legendary-looking weapon to the player's bag, with the appearance (not the stats) of a well-known legendary item.
                            - - **Leaderboard and top kills** — the NPC menu shows a server-wide leaderboard by total kills, and a personal most-killed-creature-types list. The server also announces in chat when a player overtakes another player on the leaderboard.
                              - - **GM command** — `.killtracker reload` clears the in-memory stats cache, useful after manually editing the database tables while the server is running.
                               
                                - ## What the addons do
                               
                                - ### KillTrackerHUD
                               
                                - An on-screen panel with several tabs:
                               
                                - - **Stats** — kill totals, current tier and progress bar, streak, daily quota, favorite farming zone, and boss/dungeon/elite counts with their own progress bars.
                                  - - **Contract** — today's daily contract, a "Set Route" button that feeds TomTom directly, and a "New Contract" reroll button.
                                    - - **Weekly** — the weekly bonus contract, same layout.
                                      - - **Bonuses** — all of the percent bonuses above, summed and grouped by which stat they affect, plus the flat skill bonus and a readable summary of the kill-tracker's own tier bonus.
                                       
                                        - The panel flashes and plays a sound when a new tier is reached or a contract is completed, so nothing is missed.
                                       
                                        - The addon receives its data over the same system-message channel the server already uses for ordinary chat, filtered out of the visible chat window by a standard, documented Blizzard API (this project deliberately avoids sending raw addon-channel packets, since an earlier attempt at that caused a client crash on some platforms).
                                       
                                        - Slash commands: `/kthud` shows or hides the panel; `/kthud reset` returns it to its default screen position.
                                       
                                        - ### KillTrackerWaypoint
                                       
                                        - A small, focused addon: it listens for the `/way ...` messages the Hunt Chronicler NPC prints in chat (for the daily contract target or a clicked boss location) and feeds them straight into TomTom's own command handler, the same as if you had typed the command yourself. No UI of its own. Requires TomTom to do anything (KillTrackerHUD's "Set Route" button uses the same mechanism and has the same requirement).
                                       
                                        - ## Installation
                                       
                                        - ### Server module
                                       
                                        - - Copy the `mod-kill-tracker` folder into your AzerothCore checkout's `modules` directory, so you end up with `modules/mod-kill-tracker`.
                                          - - Re-run `cmake` (module source files changed, so a plain rebuild is not enough) and rebuild `worldserver`.
                                            - - Apply the SQL files: everything under `data/sql/db-world` goes into your world database, everything under `data/sql/db-characters` goes into your characters database. If you use AzerothCore's built-in DB updater, this happens automatically on the next worldserver start; otherwise apply them by hand with the `mysql` client.
                                              - - Copy `conf/kill_tracker.conf.dist` next to your other module `.conf.dist` files (or merge `KillTracker.Enable = 1` into `worldserver.conf`) and restart.
                                                - - The NPC is not spawned automatically. Stand where you want it and run:
                                                 
                                                  - ```
                                                    .npc add 601070
                                                    ```

                                                    ### Client addons

                                                    Copy the `KillTrackerHUD` and `KillTrackerWaypoint` folders into your WoW 3.3.5a client's `Interface/AddOns` directory, so each one contains its own `.toc` file directly inside `Interface/AddOns/KillTrackerHUD` and `Interface/AddOns/KillTrackerWaypoint`. Enable both (and TomTom, if you want map pins) at the character select AddOns screen.

                                                    ## Disclaimer

                                                    This is a fan-made module for private, non-commercial AzerothCore servers, not affiliated with or endorsed by Blizzard Entertainment or the AzerothCore project. World of Warcraft is a trademark of Blizzard Entertainment.

                                                    The software is provided **"as is," without warranty of any kind**, express or implied — see the [LICENSE](LICENSE) for the full text. Always back up your database before applying new SQL files or modules to a live server.

                                                    ## License

                                                    MIT — see [LICENSE](LICENSE).
                                                    
