#if defined(WOWDECOMP_PLATFORM_WEB)

// Coming back to the game after switching apps (docs/mobile-plan.md). iOS suspends a backgrounded page, and the world
// server's connection dies within about 30 s. Stock, the client then drops the player to the login screen with
// "Disconnected". A drop noticed while the page is hidden, or shortly after it comes back, reconnects instead, once the
// player is looking at the page again, and at most once per return:
//   1. The world server is asked to take the stored login again (CMSG_AUTH_SESSION with the session key the client
//      still holds from the login), the way Change Realm reconnects. No password, no auth server round trip.
//   2. The character list, as character select asks for it.
//   3. The character that was in the world enters it again.
// Each step is the stock one with its stock dialog ("Logging in to game server", "Retrieving character list", the
// loading screen). If one fails, its stock error shows and the player carries on from there by hand.

#include <atomic>
#include <cstdint>
#include <emscripten.h>
#include <emscripten/html5.h>
#include "WoW/Source/Glue/CGlueMgr.h"
#include "WoW/Source/Glue/CharacterSelection.h"
#include "WoW/Source/Glue/CharacterSelectionDisplay.h"
#include "WoW/Source/Ui/ScriptEvents.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "WoW/Source/WowSvcs/WowSvcsClient/ClientServices.h"

namespace {
    enum ResumeState { RESUME_NONE, RESUME_WAITING, RESUME_CONNECTING, RESUME_LISTING };

    // How long after the page comes back a drop still counts as one from the background: the socket's close often only
    // arrives once the page runs again.
    const double kReturnWindowMs = 30000.0;

    // Written by the page (main thread), read by the game thread.
    std::atomic<int32_t> s_hidden;
    std::atomic<int32_t> s_returns;
    std::atomic<int64_t> s_returnedAt;

    ResumeState s_state;
    WOWGUID s_guid;
    int32_t s_usedReturn;

    void OpenStatusDialog() {
        FrameScript_SignalEvent(GLUE_EVENT_OPEN_STATUS_DIALOG, "%s%s", "CANCEL", FrameScript_GetText("GAME_SERVER_LOGIN", -1, GENDER_NOT_APPLICABLE));
    }

    // CGlueMgr::ChangeRealm's reconnect, minus its realm list request: that goes to the auth server, which the client
    // logged off from when it entered the world (CGlueMgr::EnterWorld). The realm's address is still known.
    void StartReconnect() {
        ClientConnection* connection = ClientServices::Connection();

        CGlueMgr::m_idleState = IDLE_ACCOUNT_LOGIN;
        CGlueMgr::m_showedDisconnect = 0;
        OpenStatusDialog();

        CGlueMgr::ResetQueueState();
        connection->m_realmConnectPending = 1;
        connection->Initiate(COP_CONNECT, 7, nullptr);
        ClientServices::ConnectToSelectedServer();
    }
}

// From the page (main thread, index.html): it was hidden or shown again.
extern "C" EMSCRIPTEN_KEEPALIVE void WebResumeVisible(int32_t visible) {
    if (!visible) {
        s_hidden = 1;
    } else if (s_hidden.exchange(0)) {
        s_returnedAt = static_cast<int64_t>(emscripten_date_now());
        s_returns++;
    }
}

// CGlueMgr::NetDisconnectHandler, after the game has been torn down. guid is the character that was in the world, or
// empty at character select. Returns true when the drop is taken over here, so the stock "Disconnected" is not shown.
bool WebResumeAfterDisconnect(WOWGUID guid) {
    bool hidden = s_hidden;
    bool justReturned = s_returns != s_usedReturn && emscripten_date_now() - s_returnedAt < kReturnWindowMs;

    if (s_state != RESUME_NONE || (!hidden && !justReturned)) {
        return false;
    }

    s_guid = guid;
    s_state = RESUME_WAITING;
    OpenStatusDialog();
    return true;
}

// The top of CGlueMgr::Idle, every glue frame.
void WebResumeIdle() {
    static bool listening;

    if (!listening) {
        listening = true;
        MAIN_THREAD_ASYNC_EM_ASM({ window.wowVisible = function(visible) { _WebResumeVisible(visible ? 1 : 0); }; });
    }

    switch (s_state) {
        case RESUME_WAITING: {
            // Only once the player is back.
            if (s_hidden) {
                return;
            }

            s_usedReturn = s_returns;
            s_state = RESUME_CONNECTING;
            StartReconnect();
            return;
        }

        case RESUME_CONNECTING: {
            if (CGlueMgr::m_idleState == IDLE_ACCOUNT_LOGIN) {
                return;
            }

            // Logged in, character select is up and has asked for the list (GetCharacterListUpdate in its OnShow).
            if (CGlueMgr::m_idleState == IDLE_CHARACTER_LIST) {
                s_state = RESUME_LISTING;
                return;
            }

            WOWCS_OPS op;
            const char* text;
            int32_t result;
            int32_t errorCode;
            ClientConnection* connection = ClientServices::Connection();

            if (CGlueMgr::m_idleState == IDLE_NONE && connection->m_connected && connection->PollStatus(op, &text, result, errorCode) && op == COP_AUTHENTICATE && result) {
                CGlueMgr::GetCharacterList();
                s_state = RESUME_LISTING;
            } else {
                s_state = RESUME_NONE;
            }

            return;
        }

        case RESUME_LISTING: {
            if (CGlueMgr::m_idleState == IDLE_CHARACTER_LIST) {
                return;
            }

            s_state = RESUME_NONE;

            if (CGlueMgr::m_idleState != IDLE_NONE || !s_guid) {
                return;
            }

            for (uint32_t i = 0; i < CCharacterSelection::s_characterList.Count(); i++) {
                if (CCharacterSelection::s_characterList[i].m_characterInfo.m_guid == s_guid) {
                    CCharacterSelection::s_selectionIndex = static_cast<int32_t>(i);
                    CGlueMgr::EnterWorld();
                    return;
                }
            }

            return;
        }

        default: {
            return;
        }
    }
}

#endif
