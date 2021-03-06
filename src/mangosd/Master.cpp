/*
 * Copyright (C) 2005-2012 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/** \file
    \ingroup mangosd
*/

#ifndef WIN32
    #include "PosixDaemon.h"
#endif

#include "WorldSocketMgr.h"
#include "Common.h"
#include "Master.h"
#include "WorldSocket.h"
#include "WorldRunnable.h"
#include "World.h"
#include "Log.h"
#include "MapManager.h"
#include "Timer.h"
#include "Policies/Singleton.h"
#include "SystemConfig.h"
#include "Config/Config.h"
#include "Database/DatabaseEnv.h"
#include "CliRunnable.h"
#include "RASocket.h"
#include "Util.h"
#include "revision_sql.h"
#include "MaNGOSsoap.h"
#include "MassMailMgr.h"
#include "DBCStores.h"

#include <ace/OS_NS_signal.h>
#include <ace/TP_Reactor.h>
#include <ace/Dev_Poll_Reactor.h>
#include <ace/Stack_Trace.h>

#ifdef WIN32
#include "ServiceWin32.h"
extern int m_ServiceStatus;
#endif

INSTANTIATE_SINGLETON_1( Master );

volatile uint32 Master::m_masterLoopCounter = 0;

class FreezeDetectorRunnable : public ACE_Based::Runnable
{
public:
    FreezeDetectorRunnable() { _delaytime = 0; }
    uint32 m_loops, m_lastchange;
    uint32 w_loops, w_lastchange;
    uint32 _delaytime;
    void SetDelayTime(uint32 t) { _delaytime = t; }
    void run(void)
    {
        if(!_delaytime)
            return;
        sLog.outString("Starting up anti-freeze thread (%u seconds max stuck time)...",_delaytime/1000);
        m_loops = 0;
        w_loops = 0;
        m_lastchange = 0;
        w_lastchange = 0;
        while (!World::IsStopped())
        {
            ACE_Based::Thread::Sleep(sWorld.getConfig(CONFIG_UINT32_VMSS_FREEZECHECKPERIOD));

            if (sWorld.getConfig(CONFIG_BOOL_VMSS_ENABLE))
                sMapMgr.GetMapUpdater()->FreezeDetect();

            uint32 curtime = WorldTimer::getMSTime();

            // normal work
            uint32 worldLoopCounter = World::m_worldLoopCounter.value();
            if (w_loops != worldLoopCounter)
            {
                w_lastchange = curtime;
                w_loops      = worldLoopCounter;
            }
            // possible freeze
            else if (WorldTimer::getMSTimeDiff(w_lastchange, curtime) > _delaytime)
            {
                sLog.outError("World Thread hangs, kicking out server!");
                *((uint32 volatile*)NULL) = 0;              // bang crash
            }
        }
        sLog.outString("Anti-freeze thread exiting without problems.");
    }
};

class RARunnable : public ACE_Based::Runnable
{
private:
    ACE_Reactor *m_Reactor;
    RASocket::Acceptor *m_Acceptor;
public:
    RARunnable()
    {
        ACE_Reactor_Impl* imp = 0;

        #if defined (ACE_HAS_EVENT_POLL) || defined (ACE_HAS_DEV_POLL)

        imp = new ACE_Dev_Poll_Reactor ();

        imp->max_notify_iterations (128);
        imp->restart (1);

        #else

        imp = new ACE_TP_Reactor ();
        imp->max_notify_iterations (128);

        #endif

        m_Reactor = new ACE_Reactor (imp, 1 /* 1= delete implementation so we don't have to care */);

        m_Acceptor = new RASocket::Acceptor;

    }

    ~RARunnable()
    {
        delete m_Reactor;
        delete m_Acceptor;
    }

    void run ()
    {
        uint16 raport = sConfig.GetIntDefault ("Ra.Port", 3443);
        std::string stringip = sConfig.GetStringDefault ("Ra.IP", "0.0.0.0");

        ACE_INET_Addr listen_addr(raport, stringip.c_str());

        if (m_Acceptor->open (listen_addr, m_Reactor, ACE_NONBLOCK) == -1)
        {
            sLog.outError ("MaNGOS RA can not bind to port %d on %s", raport, stringip.c_str ());
        }

        sLog.outString ("Starting Remote access listner on port %d on %s", raport, stringip.c_str ());

        while (!m_Reactor->reactor_event_loop_done())
        {
            ACE_Time_Value interval (0, 10000);

            if (m_Reactor->run_reactor_event_loop (interval) == -1)
                break;

            if(World::IsStopped())
            {
                m_Acceptor->close();
                break;
            }
        }
        sLog.outString("RARunnable thread ended");
    }
};

Master::Master()
{
}

Master::~Master()
{
}

/// Main function
int Master::Run()
{
    /// worldd PID file creation
    std::string pidfile = sConfig.GetStringDefault("PidFile", "");
    if(!pidfile.empty())
    {
        uint32 pid = CreatePIDFile(pidfile);
        if( !pid )
        {
            sLog.outError( "BOOT: Cannot create PID file %s.", pidfile.c_str() );
            Log::WaitBeforeContinueIfNeed();
            return 1;
        }

        sLog.outString( "BOOT: Daemon PID: %u", pid );
    }

    ///- Start the databases
    if (!_StartDB())
    {
        Log::WaitBeforeContinueIfNeed();
        return 1;
    }

    ///- Initialize the World
    sWorld.SetInitialWorldSettings();

    #ifndef WIN32
    detachDaemon();
    #endif
    //server loaded successfully => enable async DB requests
    //this is done to forbid any async transactions during server startup!
    CharacterDatabase.AllowAsyncTransactions();
    WorldDatabase.AllowAsyncTransactions();
    LoginDatabase.AllowAsyncTransactions();

    ///- Catch termination signals
    _HookSignals();

    ///- Launch WorldRunnable thread
    ACE_Based::Thread world_thread(new WorldRunnable);
    world_thread.setPriority(ACE_Based::Highest);

    // set realmbuilds depend on mangosd expected builds, and set server online
    {
        std::string builds = AcceptableClientBuildsListStr();
        LoginDatabase.escape_string(builds);
        LoginDatabase.DirectPExecute("UPDATE realmlist SET realmflags = realmflags & ~(%u), population = 0, realmbuilds = '%s'  WHERE id = '%u'", REALM_FLAG_OFFLINE, builds.c_str(), sWorld.getConfig(CONFIG_UINT32_REALMID));
    }

    ACE_Based::Thread* cliThread = NULL;

#ifdef WIN32
    if (sConfig.GetBoolDefault("Console.Enable", true) && (m_ServiceStatus == -1)/* need disable console in service mode*/)
#else
    if (sConfig.GetBoolDefault("Console.Enable", true))
#endif
    {
        ///- Launch CliRunnable thread
        cliThread = new ACE_Based::Thread(new CliRunnable);
    }

    ACE_Based::Thread* rar_thread = NULL;
    if(sConfig.GetBoolDefault ("Ra.Enable", false))
    {
        rar_thread = new ACE_Based::Thread(new RARunnable);
    }

    ///- Handle affinity for multiple processors and process priority on Windows
    #ifdef WIN32
    {
        HANDLE hProcess = GetCurrentProcess();

        uint32 Aff = sConfig.GetIntDefault("UseProcessors", 0);
        if(Aff > 0)
        {
            ULONG_PTR appAff;
            ULONG_PTR sysAff;

            if(GetProcessAffinityMask(hProcess,&appAff,&sysAff))
            {
                ULONG_PTR curAff = Aff & appAff;            // remove non accessible processors

                if(!curAff )
                {
                    sLog.outError("BOOT: Processors marked in UseProcessors bitmask (hex) %x not accessible for mangosd. Accessible processors bitmask (hex): %x",Aff,appAff);
                }
                else
                {
                    if(SetProcessAffinityMask(hProcess,curAff))
                        sLog.outString("BOOT: Using processors (bitmask, hex): %x", curAff);
                    else
                        sLog.outError("BOOT: Can't set used processors (hex): %x",curAff);
                }
            }
            sLog.outString();
        }

        bool Prio = sConfig.GetBoolDefault("ProcessPriority", false);

//        if(Prio && (m_ServiceStatus == -1)/* need set to default process priority class in service mode*/)
        if(Prio)
        {
            if(SetPriorityClass(hProcess,HIGH_PRIORITY_CLASS))
                sLog.outString("BOOT: mangosd process priority class set to HIGH");
            else
                sLog.outError("BOOT: Can't set mangosd process priority class.");
            sLog.outString();
        }
    }
    #endif

    ///- Start soap serving thread
    ACE_Based::Thread* soap_thread = NULL;

    if(sConfig.GetBoolDefault("SOAP.Enabled", false))
    {
        MaNGOSsoapRunnable *runnable = new MaNGOSsoapRunnable();

        runnable->setListenArguments(sConfig.GetStringDefault("SOAP.IP", "127.0.0.1"), sConfig.GetIntDefault("SOAP.Port", 7878));
        soap_thread = new ACE_Based::Thread(runnable);
    }

    ///- Start up freeze catcher thread
    ACE_Based::Thread* freeze_thread = NULL;
    if(uint32 freeze_delay = sConfig.GetIntDefault("MaxCoreStuckTime", 0))
    {
        FreezeDetectorRunnable *fdr = new FreezeDetectorRunnable();
        fdr->SetDelayTime(freeze_delay*1000);
        freeze_thread = new ACE_Based::Thread(fdr);
        freeze_thread->setPriority(ACE_Based::Highest);
    }

    ///- Launch the world listener socket
    uint16 wsport = sWorld.getConfig (CONFIG_UINT32_PORT_WORLD);
    std::string bind_ip = sConfig.GetStringDefault ("BindIP", "0.0.0.0");

    if (sWorldSocketMgr->StartNetwork (wsport, bind_ip) == -1)
    {
        sLog.outError ("BOOT: Failed to start network");
        Log::WaitBeforeContinueIfNeed();
        World::StopNow(ERROR_EXIT_CODE);
        // go down and shutdown the server
    }

    sWorldSocketMgr->Wait ();

    ///- Stop freeze protection before shutdown tasks
    if (freeze_thread)
    {
        freeze_thread->destroy();
        delete freeze_thread;
    }

    ///- Stop soap thread
    if(soap_thread)
    {
        soap_thread->wait();
        soap_thread->destroy();
        delete soap_thread;
    }

    ///- Set server offline in realmlist
    LoginDatabase.DirectPExecute("UPDATE realmlist SET realmflags = realmflags | %u WHERE id = '%u'", REALM_FLAG_OFFLINE, sWorld.getConfig(CONFIG_UINT32_REALMID));

    ///- Remove signal handling before leaving
    _UnhookSignals();

    // when the main thread closes the singletons get unloaded
    // since worldrunnable uses them, it will crash if unloaded after master
    world_thread.wait();

    if(rar_thread)
    {
        rar_thread->wait();
        rar_thread->destroy();
        delete rar_thread;
    }

    ///- Clean account database before leaving
    clearOnlineAccounts();

    // send all still queued mass mails (before DB connections shutdown)
    sMassMailMgr.Update(true);

    ///- Wait for DB delay threads to end
    CharacterDatabase.HaltDelayThread();
    WorldDatabase.HaltDelayThread();
    LoginDatabase.HaltDelayThread();

    sLog.outString( "BOOT: Halting process..." );

    if (cliThread)
    {
        #ifdef WIN32

        // this only way to terminate CLI thread exist at Win32 (alt. way exist only in Windows Vista API)
        //_exit(1);
        // send keyboard input to safely unblock the CLI thread
        INPUT_RECORD b[5];
        HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
        b[0].EventType = KEY_EVENT;
        b[0].Event.KeyEvent.bKeyDown = TRUE;
        b[0].Event.KeyEvent.uChar.AsciiChar = 'X';
        b[0].Event.KeyEvent.wVirtualKeyCode = 'X';
        b[0].Event.KeyEvent.wRepeatCount = 1;

        b[1].EventType = KEY_EVENT;
        b[1].Event.KeyEvent.bKeyDown = FALSE;
        b[1].Event.KeyEvent.uChar.AsciiChar = 'X';
        b[1].Event.KeyEvent.wVirtualKeyCode = 'X';
        b[1].Event.KeyEvent.wRepeatCount = 1;

        b[2].EventType = KEY_EVENT;
        b[2].Event.KeyEvent.bKeyDown = TRUE;
        b[2].Event.KeyEvent.dwControlKeyState = 0;
        b[2].Event.KeyEvent.uChar.AsciiChar = '\r';
        b[2].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
        b[2].Event.KeyEvent.wRepeatCount = 1;
        b[2].Event.KeyEvent.wVirtualScanCode = 0x1c;

        b[3].EventType = KEY_EVENT;
        b[3].Event.KeyEvent.bKeyDown = FALSE;
        b[3].Event.KeyEvent.dwControlKeyState = 0;
        b[3].Event.KeyEvent.uChar.AsciiChar = '\r';
        b[3].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
        b[3].Event.KeyEvent.wVirtualScanCode = 0x1c;
        b[3].Event.KeyEvent.wRepeatCount = 1;
        DWORD numb;
        BOOL ret = WriteConsoleInput(hStdIn, b, 4, &numb);

        cliThread->wait();

        #else

        cliThread->destroy();

        #endif

        delete cliThread;
    }

    ///- Exit the process with specified return value
    return World::GetExitCode();
}

/// Initialize connection to the databases
bool Master::_StartDB()
{
    ///- Get world database info from configuration file
    std::string dbstring = sConfig.GetStringDefault("WorldDatabaseInfo", "");
    int nConnections = sConfig.GetIntDefault("WorldDatabaseConnections", 1);
    if(dbstring.empty())
    {
        sLog.outError("BOOT: Database not specified in configuration file");
        return false;
    }
    sLog.outString("BOOT: World Database total connections: %i", nConnections + 1);

#ifdef MANGOSR2_SINGLE_THREAD
    if (nConnections > 1)
    {
        sLog.outError(" Your OS (%s) not support set WorldDatabaseConnections > 1! Resetted to 1", MANGOSR2_SINGLE_THREAD);
        nConnections = 1;
    }
#endif

    ///- Initialise the world database
    if(!WorldDatabase.Initialize(dbstring.c_str(), nConnections))
    {
        sLog.outError("BOOT: Cannot connect to world database %s",dbstring.c_str());
        return false;
    }

    if(!WorldDatabase.CheckRequiredField("db_version",REVISION_DB_MANGOS))
    {
        ///- Wait for already started DB delay threads to end
        WorldDatabase.HaltDelayThread();
        return false;
    }

    dbstring = sConfig.GetStringDefault("CharacterDatabaseInfo", "");
    nConnections = sConfig.GetIntDefault("CharacterDatabaseConnections", 1);
    if(dbstring.empty())
    {
        sLog.outError("BOOT: Character Database not specified in configuration file");

        ///- Wait for already started DB delay threads to end
        WorldDatabase.HaltDelayThread();
        return false;
    }
    sLog.outString("BOOT: Character Database total connections: %i", nConnections + 1);

#ifdef MANGOSR2_SINGLE_THREAD
    if (nConnections > 1)
    {
        sLog.outError("BOOT: Your OS (%s) not support set CharacterDatabaseConnections > 1! Resetted to 1", MANGOSR2_SINGLE_THREAD);
        nConnections = 1;
    }
#endif

    ///- Initialise the Character database
    if(!CharacterDatabase.Initialize(dbstring.c_str(), nConnections))
    {
        sLog.outError("BOOT: Cannot connect to Character database %s",dbstring.c_str());

        ///- Wait for already started DB delay threads to end
        WorldDatabase.HaltDelayThread();
        return false;
    }

    if(!CharacterDatabase.CheckRequiredField("character_db_version",REVISION_DB_CHARACTERS))
    {
        ///- Wait for already started DB delay threads to end
        WorldDatabase.HaltDelayThread();
        CharacterDatabase.HaltDelayThread();
        return false;
    }

    ///- Get login database info from configuration file
    dbstring = sConfig.GetStringDefault("LoginDatabaseInfo", "");
    nConnections = sConfig.GetIntDefault("LoginDatabaseConnections", 1);
    if(dbstring.empty())
    {
        sLog.outError("BOOT: Login database not specified in configuration file");

        ///- Wait for already started DB delay threads to end
        WorldDatabase.HaltDelayThread();
        CharacterDatabase.HaltDelayThread();
        return false;
    }

    ///- Initialise the login database
    sLog.outString("BOOT: Login Database total connections: %i", nConnections + 1);

#ifdef MANGOSR2_SINGLE_THREAD
    if (nConnections > 1)
    {
        sLog.outError("BOOT: Your OS (%s) not support set LoginDatabaseConnections > 1! Resetted to 1", MANGOSR2_SINGLE_THREAD);
        nConnections = 1;
    }
#endif

    if(!LoginDatabase.Initialize(dbstring.c_str(), nConnections))
    {
        sLog.outError("BOOT: Cannot connect to login database %s",dbstring.c_str());

        ///- Wait for already started DB delay threads to end
        WorldDatabase.HaltDelayThread();
        CharacterDatabase.HaltDelayThread();
        return false;
    }

    if(!LoginDatabase.CheckRequiredField("realmd_db_version",REVISION_DB_REALMD))
    {
        ///- Wait for already started DB delay threads to end
        WorldDatabase.HaltDelayThread();
        CharacterDatabase.HaltDelayThread();
        LoginDatabase.HaltDelayThread();
        return false;
    }

    ///- Get the realm Id from the configuration file
    uint32 realmID = sConfig.GetIntDefault("RealmID", 0);
    if(!realmID)
    {
        sLog.outError("BOOT: Realm ID not defined in configuration file");

        ///- Wait for already started DB delay threads to end
        WorldDatabase.HaltDelayThread();
        CharacterDatabase.HaltDelayThread();
        LoginDatabase.HaltDelayThread();
        return false;
    }

    sLog.outString("BOOT: Realm running as realm ID %d", realmID);

    ///- Clean the database before starting
    clearOnlineAccounts();

    sWorld.LoadDBVersion();

    sLog.outString("BOOT: Using World DB: %s", sWorld.GetDBVersion());
    sLog.outString("BOOT: Using creature EventAI: %s", sWorld.GetCreatureEventAIVersion());
    return true;
}

/// Clear 'online' status for all accounts with characters in this realm
void Master::clearOnlineAccounts()
{
    // Cleanup online status for characters hosted at current realm
    /// \todo Only accounts with characters logged on *this* realm should have online status reset. Move the online column from 'account' to 'realmcharacters'?
    LoginDatabase.PExecute("UPDATE account SET active_realm_id = 0 WHERE active_realm_id = '%u'", sWorld.getConfig(CONFIG_UINT32_REALMID));

    CharacterDatabase.Execute("UPDATE characters SET online = 0 WHERE online<>0");

    // Battleground instance ids reset at server restart
    CharacterDatabase.Execute("UPDATE character_battleground_data SET instance_id = 0");
}

/// Handle termination signals
void Master::_OnSignal(int s)
{
    switch (s)
    {
        case SIGINT:
            World::StopNow(RESTART_EXIT_CODE);
            signal(s, _OnSignal);
            break;
        case SIGTERM:
        #ifdef _WIN32
        case SIGBREAK:
        #endif
            World::StopNow(SHUTDOWN_EXIT_CODE);
            signal(s, _OnSignal);
            break;
        case SIGSEGV:
        case SIGABRT:
        case SIGFPE:
        #ifdef __FreeBSD__
        case SIGBUS:
        #endif
        {
            if (sWorld.getConfig(CONFIG_BOOL_VMSS_ENABLE))
            {
                ACE_thread_t const threadId = ACE_OS::thr_self();

                sLog.outError("VMSS:: Signal %.2u received from thread "I64FMT".\r\n",s,threadId);
                ACE_Stack_Trace _StackTrace;
                std::string StackTrace = _StackTrace.c_str();
                if (Map* map = sMapMgr.GetMapUpdater()->getObject(threadId))
                {
                    MapStatisticData const* pMBData = sMapMgr.GetMapUpdater()->GetMapStatisticData(map);
                    uint32 counter = pMBData ? pMBData->breaksCount : 0;

                    sLog.outError("VMSS:: crushed thread is update map %u, instance %u, counter %u",map->GetId(), map->GetInstanceId(), counter);
                    sLog.outError("VMSS:: BackTrace for map %u: ",map->GetId());

                    size_t found = 0;
                    while (found < StackTrace.size())
                    {
                        size_t next = StackTrace.find_first_of("\n",found);
                        std::string to_log = StackTrace.substr(found, (next - found));
                        if (to_log.size() > 1)
                            sLog.outError("VMSS:%u: %s",map->GetId(), to_log.c_str());
                        found = next+1;
                    }
                    sLog.outError("VMSS:: /BackTrace for map %u: ",map->GetId());

                    if (sWorld.getConfig(CONFIG_BOOL_VMSS_CONTINENTS_SKIP) && map->IsContinent())
                    {
                        sLog.outError("VMSS:: Thread "I64FMT" is virtual map server for continent, but continents processing disabled. Stopping world.",threadId);
                        signal(s, SIG_DFL);
                        ACE_OS::kill(getpid(), s);
                    }
                    else
                    {
                        if (!sWorld.getConfig(CONFIG_BOOL_VMSS_TRYSKIPFIRST) || counter > 0)
                            map->SetBroken(true);

                        sMapMgr.GetMapUpdater()->MapBrokenEvent(map);

                        if (counter > sWorld.getConfig(CONFIG_UINT32_VMSS_MAXTHREADBREAKS))
                        {
                            sLog.outError("VMSS:: Limit of map restarting (map %u instance %u) exceeded. Stopping world!",map->GetId(), map->GetInstanceId());
                            signal(s, SIG_DFL);
                            ACE_OS::kill(getpid(), s);
                        }
                        else
                        {
                            sLog.outError("VMSS:: Restarting virtual map server (map %u instance %u). Count of restarts: %u",map->GetId(), map->GetInstanceId(), sMapMgr.GetMapUpdater()->GetMapStatisticData(map)->breaksCount);
                            sMapMgr.GetMapUpdater()->kill_thread(threadId, false);
                            ACE_OS::thr_exit();
                        }
                    }
                }
                else
                {
                    sLog.outError("VMSS:: Thread "I64FMT" is not virtual map server. Stopping world.",threadId);
                    sLog.outError("VMSS:: BackTrace: ");
                    size_t found = 0;
                    while (found < StackTrace.size())
                    {
                        size_t next = StackTrace.find_first_of("\n",found);
                        std::string to_log = StackTrace.substr(found, (next - found));
                        if (to_log.size() > 1)
                            sLog.outError("VMSS:T: %s",to_log.c_str());
                        found = next+1;
                    }
                    sLog.outError("VMSS:: /BackTrace");
                    signal(s, SIG_DFL);
                    ACE_OS::kill(getpid(), s);
                }
            }
            else
            {
                signal(s, SIG_DFL);
                ACE_OS::kill(getpid(), s);
            }
            break;
        }
        default:
            signal(s, SIG_DFL);
            break;
    }
}

/// Define hook '_OnSignal' for all termination signals
void Master::_HookSignals()
{
    signal(SIGINT,   _OnSignal);
    signal(SIGTERM,  _OnSignal);
    #ifdef _WIN32
    signal(SIGBREAK, _OnSignal);
    #endif
    signal(SIGSEGV,  _OnSignal);
    signal(SIGABRT,  _OnSignal);
    signal(SIGFPE ,  _OnSignal);
    #ifdef __FreeBSD__
    signal(SIGBUS ,  _OnSignal);
    #endif
}

/// Unhook the signals before leaving
void Master::_UnhookSignals()
{
    signal(SIGINT,   SIG_DFL);
    signal(SIGTERM,  SIG_DFL);
    #ifdef _WIN32
    signal(SIGBREAK, SIG_DFL);
    #endif
    signal(SIGSEGV,  SIG_DFL);
    signal(SIGABRT,  SIG_DFL);
    signal(SIGFPE ,  SIG_DFL);
    #ifdef __FreeBSD__
    signal(SIGBUS ,  SIG_DFL);
    #endif
}
