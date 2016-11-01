/**
 * WarfaceBot, a blind XMPP client for Warface (FPS)
 * Copyright (C) 2015, 2016 Levak Borok <levak92@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <wb_tools.h>
#include <wb_session.h>
#include <wb_xmpp.h>
#include <wb_xmpp_wf.h>
#include <wb_mission.h>
#include <wb_log.h>

#include <stdlib.h>

struct cb_args
{
    f_gameroom_quickplay_cb cb;
    void *args;

    char *mission_key;
    char *mission_name;
    enum e_room_type type;
    char *game_mode;
    char *uid;
    int channel_switches;

    int tries;
};

/* Forward declaration */
static void _xmpp_iq_gameroom_quickplay(const char *uid,
                                        const char *mission_key,
                                        enum e_room_type type,
                                        const char *game_mode,
                                        int channel_switches,
                                        int tries,
                                        f_gameroom_quickplay_cb cb,
                                        void *args);

static void _quickplay_updated_list(void *args)
{
    struct cb_args *a = (struct cb_args *) args;
    struct mission *m = mission_list_get(a->mission_name);

    if (m != NULL)
    {
        _xmpp_iq_gameroom_quickplay(
            a->uid,
            m->mission_key,
            a->type,
            a->game_mode,
            a->channel_switches,
            a->tries,
            a->cb,
            a->args);
    }
    else
    {
        eprintf("Failed to open quickplay room (Expired missions)\n");
    }

    free(a->mission_key);
    a->mission_key = NULL;
    free(a->mission_name);
    a->mission_name = NULL;
    free(a->game_mode);
    a->game_mode = NULL;
    free(a->uid);
    a->uid = NULL;
    free(a);
}

static void xmpp_iq_gameroom_quickplay_cb(const char *msg,
                                          enum xmpp_msg_type type,
                                          void *args)
{
    /* Answer :
       <iq to='masterserver@warface/pve_2' type='get'>
        <query xmlns='urn:cryonline:k01'>
         <gameroom_quickplay/>
        </query>
       </iq>
     */

    struct cb_args *a = (struct cb_args *) args;

    if (type & XMPP_TYPE_ERROR)
    {
        int code = get_info_int(msg, "code='", "'", NULL);
        int custom_code = get_info_int(msg, "custom_code='", "'", NULL);
        const char *reason = NULL;

        switch (code)
        {
            case 1006:
                reason = "QoS limit reached";
                break;
            case 8:
                switch (custom_code)
                {
                    case 1: /* Expired mission, update and try again */
                        if (++a->tries < 2)
                        {
                            struct mission *m =
                                mission_list_get_by_key(a->mission_key);

                            if (m != NULL)
                            {
                                a->mission_name = strdup(m->name);
                                mission_list_update(
                                    _quickplay_updated_list,
                                    args);

                                return;
                            }
                        }

                        reason = "Expired mission";
                        break;
                    case 12:
                        reason = "Invalid mission";
                        break;
                    case 27:
                        reason = "Ranked season over";
                        break;
                    default:
                        break;
                }
                break;
            default:
                break;
        }

        if (reason != NULL)
            eprintf("Failed to open quickplay room (%s)\n",
                    reason);
        else
            eprintf("Failed to open quickplay room (%i:%i)\n",
                    code,
                    custom_code);
    }
    else
    {
        if (a->cb)
            a->cb(a->args);
    }

    free(a->mission_key);
    a->mission_key = NULL;
    free(a->mission_name);
    a->mission_name = NULL;
    free(a->game_mode);
    a->game_mode = NULL;
    free(a->uid);
    a->uid = NULL;
    free(a);
}

static void _concat_players(s_qp_player *p, void *args)
{
    if (p->accepted)
    {
        char **group = (char **) args;
        char *s;

        FORMAT(s, "%s<player profile_id='%s'/>", *group, p->profile_id);
        free(*group);
        *group = s;
    }
}

static void _xmpp_iq_gameroom_quickplay(const char *uid,
                                        const char *mission_key,
                                        enum e_room_type type,
                                        const char *game_mode,
                                        int channel_switches,
                                        int tries,
                                        f_gameroom_quickplay_cb cb,
                                        void *args)
{
    if (uid == NULL)
        return;

    char *query_mode;
    if (type == ROOM_PVE_QUICKPLAY)
    {
        if (mission_key == NULL)
            return;

        FORMAT(query_mode,
               "mission_id='%s' missions_hash='%i' content_hash='%i'",
               mission_key,
               session.wf.missions.hash,
               session.wf.missions.content_hash);
    }
    else if (type == ROOM_PVP_RATING)
    {
        query_mode = strdup("mission_id='' game_mode=''");
    }
    else if (mission_key != NULL)
    {
        FORMAT(query_mode, "mission_id='%s'", mission_key);
    }
    else if (game_mode != NULL)
    {
        FORMAT(query_mode, "game_mode='%s'", game_mode);
    }
    else
        return;

    struct cb_args *a = calloc(1, sizeof (struct cb_args));

    a->uid = strdup(uid);
    a->cb = cb;
    a->args = args;
    a->tries = tries;

    a->channel_switches = channel_switches;
    a->type = type;

    if (mission_key != NULL)
        a->mission_key = strdup(mission_key);

    if (game_mode != NULL)
        a->game_mode = strdup(game_mode);

    char *player_group = strdup("");
    if (session.quickplay.group != NULL)
    {
        list_foreach(session.quickplay.group,
                     (f_list_callback) _concat_players,
                     &player_group);
    }

    /* Request a new game room */
    xmpp_send_iq_get(
        JID_MS(session.online.channel),
        xmpp_iq_gameroom_quickplay_cb, a,
        "<query xmlns='urn:cryonline:k01'>"
        " <gameroom_quickplay"
        "     team_id='0' status='%d'"
        "     class_id='%d' room_type='%d'"
        "     channel_switches='%d' uid='%s' timestamp='0'"
        "     %s>"
        "  <group>%s</group>"
        " </gameroom_quickplay>"
        "</query>",
        GAMEROOM_READY,
        session.profile.curr_class,
        type,
        channel_switches,
        uid,
        query_mode,
        player_group);

    free(query_mode);
    free(player_group);
}

void xmpp_iq_gameroom_quickplay(const char *uid,
                                const char *mission_key,
                                enum e_room_type type,
                                const char *game_mode,
                                int channel_switches,
                                f_gameroom_quickplay_cb cb,
                                void *args)
{
    _xmpp_iq_gameroom_quickplay(
        uid,
        mission_key,
        type,
        game_mode,
        channel_switches,
        0,
        cb,
        args);
}
