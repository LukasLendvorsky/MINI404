/*
	ScriptHost.cpp - Core handler responsible for handling
	scripting actions.

	Copyright 2020 VintagePC <https://github.com/vintagepc/>
    Ported to Mini404 in 2021

 	This file is part of Mini404.

	Mini404 is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	Mini404 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Mini404.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ScriptHost.h"
#include "../3rdParty/gsl-lite.hpp"
// #include <GL/freeglut_std.h> // glut menus
#include <cstddef>
#include <exception>    // for exception
#include <fstream>      // IWYU pragma: keep
#include <iostream>
#include <sstream>		// IWYU pragma: keep
#include <utility>      // for make_pair, pair

typedef void* script_handle;
typedef const void* script_args;

std::map<std::string, IScriptable*> ScriptHost::m_clients;

std::vector<std::string> ScriptHost::m_script, ScriptHost::m_scriptGL;
unsigned int ScriptHost::m_uiAVRFreq;
std::map<std::string, int> ScriptHost::m_mMenuIDs;
std::map<unsigned,IScriptable*> ScriptHost::m_mMenuBase2Client;
std::map<std::string, unsigned> ScriptHost::m_mClient2MenuBase;
std::map<std::string, std::vector<std::pair<std::string,int>>> ScriptHost::m_mClientEntries;
ScriptHost::State ScriptHost::m_state = ScriptHost::State::Idle;
int ScriptHost::m_iTimeoutCycles = -1, ScriptHost::m_iTimeoutCount = 0;
bool ScriptHost::m_bQuitOnTimeout = false;
bool ScriptHost::m_bMenuCreated = false;
bool ScriptHost::m_bIsInitialized = false;
bool ScriptHost::m_bIsExecHold = false;
bool ScriptHost::m_bIsTerminalEnabled = false;
std::atomic_uint ScriptHost::m_uiQueuedMenu {0};
std::atomic_uint ScriptHost::m_eCmdStatus {TermIdle};
std::atomic_uint ScriptHost::m_iLine {0};

uint64_t ScriptHost::m_uiWaitMs = 0;
uint64_t ScriptHost::m_uiCurrentMs = 0;

bool ScriptHost::m_bFocus = false;
std::atomic_bool ScriptHost::m_bCanAcceptInput;
std::string ScriptHost::m_strCmd;

std::set<std::string> ScriptHost::m_strGLAutoC;

// String mutex... if interactive, for terminal->host,
// if running a script, for host->terminal.
std::mutex ScriptHost::m_lckScript;

void * ScriptHost::m_pConsole = nullptr;


extern "C"{
	// Kinda hacky but can't include the qemu headers here in C++ land
	#define SHUTDOWN_CAUSE_HOST_SIGNAL 4
	extern void qemu_system_shutdown_request(int);

	extern void scriptcon_print_out(void* opaque, const char* msg);
}

void ScriptHost::PrintScriptHelp(bool bMarkdown)
{
	if (m_clients.empty())
	{
		return; // Early invocation before clients is needed for some printers.
		// Print no headers if the list is empty.
	}
	if (bMarkdown)
	{
		std::cout << "# Scripting options for the selected printer:\n";
	}
	else
	{
		std::cout << "Scripting options for the current context:\n";
	}
	for (auto &client : m_clients)
	{
		client.second->PrintRegisteredActions(bMarkdown);
	}
	std::cout << "End Scripting options\n";
}

ScriptHost::~ScriptHost() {
	for (auto &p: m_clients) {
		if (p.second->CanBeDeleted()) { // Don't delete things we shouldn't - ourselves, statics...
			//std::cout << "Freeing " << p.first << "\n";
			delete p.second;
		}
	}
}

void ScriptHost::LoadScript(const std::string &strFile)
{
	std::string strLn;
	std::ifstream fileIn(strFile);
	while (getline(fileIn, strLn))
	{
		if (strLn.empty() || strLn[0]=='#')
		{
			continue;
		}
		m_script.push_back(strLn);
	}
	m_iLine = 0;
	{
		std::lock_guard<std::mutex> lck(m_lckScript);
		m_scriptGL = m_script;
	}
	std::cout << "ScriptHost: Loaded " << m_script.size() << " lines from " << strFile << '\n';
}

bool ScriptHost::Init()
{
	GetHost()._Init();
	GetHost().m_strCmd.reserve(200);
	m_bIsInitialized = true;
	return true;
}

bool ScriptHost::Setup(const std::string &strScript,unsigned uiFreq)
{
	m_uiAVRFreq = uiFreq;
	if (!strScript.empty())
	{
		m_bCanAcceptInput = false;
		LoadScript(strScript);
	}
	else
	{
		m_bCanAcceptInput = true;
	}
	return ValidateScript();
}

void ScriptHost::_Init()
{
	RegisterAction("SetTimeoutMs","Sets a timeout for actions that wait for an event",ActSetTimeoutMs,{ArgType::Int});
	RegisterAction("SetQuitOnTimeout","If 1, quits when a timeout occurs. Exit code will be non-zero.",ActSetQuitOnTimeout,{ArgType::Bool});
	RegisterAction("Log","Print the std::string to stdout",ActLog,{ArgType::String});
    RegisterAction("WaitMs","Wait the specified number of milliseconds. (10 ms resolution)",ActWait,{ArgType::Int});
	m_clients[m_strName] = this;
}

// Parse line in the format Context::Action(arg1, arg2,...)
ScriptHost::LineParts_t ScriptHost::GetLineParts(const std::string &strLine)
{
	LineParts_t sParts;
	size_t iCtxEnd = strLine.find("::");
	size_t iArgBegin = strLine.find('(');
	size_t iArgEnd = strLine.find(')');
	std::vector<std::string> args;
	if (iCtxEnd == std::string::npos || iArgBegin == std::string::npos || iArgEnd == std::string::npos)
	{
		return sParts;
	}
	sParts.strCtxt = strLine.substr(0,iCtxEnd);
	sParts.strAct = strLine.substr(iCtxEnd+2, (iArgBegin - iCtxEnd)-2);
	std::string strTmp, strArgs = strLine.substr(iArgBegin+1, (iArgEnd-iArgBegin)-1);
	std::istringstream argsIn(strArgs);
	while (getline(argsIn, strTmp,','))
	{
		args.push_back(strTmp);
	}
	sParts.vArgs = args;
	sParts.isValid = true;
	return sParts;
}

IScriptable::LineStatus ScriptHost::ProcessAction(unsigned int ID, const std::vector<std::string> &vArgs)
{
	switch (ID)
	{
		case ActSetTimeoutMs:
		{
			int iTime = std::stoi(vArgs.at(0));
			m_iTimeoutCycles = iTime *(m_uiAVRFreq/1000);
			std::cout << "ScriptHost::SetTimeoutMs changed to " << iTime << " Ms (" << m_iTimeoutCycles << " cycles)\n";
			m_iTimeoutCount = 0;
			break;
		}
		case ActSetQuitOnTimeout:
		{
			m_bQuitOnTimeout = std::stoi(vArgs.at(0))!=0;
			break;
		}
		case ActLog:
		{
			std::cout << "ScriptLog: " << vArgs.at(0) << '\n';
			break;
		}
        case ActWait:
        {
            if (m_uiWaitMs == 0)
            {
                m_uiWaitMs = m_uiCurrentMs;
            }
            int64_t iDiff = (m_uiCurrentMs - m_uiWaitMs);
            if (iDiff < std::stoi(vArgs.at(0)))
            {
                return LineStatus::Waiting;
            } else {
                m_uiWaitMs = 0; // Done.
            }
        }
	}
	return LineStatus::Finished;
}

bool ScriptHost::ValidateScript()
{
	std::string strCtxt;
	std::cout << "Validating script...\n";
	bool bClean = true;
	auto fcnErr = [](const std::string &sMsg, const int iLine) { std::cout << "ScriptHost: Validation failed: "<< sMsg <<" on line " << iLine <<":" << m_script.at(iLine) << '\n';};
	for (size_t i=0; i<m_script.size(); i++)
	{
		LineParts_t sLine = ScriptHost::GetLineParts(m_script.at(i));
		strCtxt = sLine.strCtxt;
		if (!sLine.isValid)
		{
			bClean = false;
			fcnErr("Parse error: Line is not of the form Context::Action([arg1,arg2,...])",i);
			continue;
		}
		if (m_clients.count(strCtxt)==0)
		{
			bClean = false;
			fcnErr("Unknown context " + strCtxt, i);
			std::string strCtxts = "Available contexts:";
			for (auto &it: m_clients)
			{
				strCtxts += " " + it.first + ",";
			}
			strCtxts.pop_back();
			std::cout << strCtxts << '\n';
			continue;
		}
		if (m_clients.at(strCtxt)->m_ActionIDs.count(sLine.strAct)==0)
		{
			bClean = false;
			fcnErr(std::string("Unknown action ").append(strCtxt).append("::").append(sLine.strAct),i);
			std::cout << "Available actions:\n";
			m_clients.at(strCtxt)->PrintRegisteredActions();
			continue;
		}
		int ID = m_clients.at(strCtxt)->m_ActionIDs.at(sLine.strAct);
		const std::vector<ArgType> vArgTypes = m_clients.at(strCtxt)->m_ActionArgs.at(ID);
		if (vArgTypes.size()!=sLine.vArgs.size())
		{
			bClean = false;
			fcnErr("Argument count mismatch, expected "+ std::to_string(vArgTypes.size()),i);
						m_clients.at(strCtxt)->PrintRegisteredActions();
			continue;
		}
		for (size_t j=0; j<vArgTypes.size(); j++)
		{
			if (!CheckArg(vArgTypes.at(j),sLine.vArgs.at(j)))
			{
				bClean = false;
				fcnErr("Conversion error, expected \"" + GetArgTypeNames().at(vArgTypes.at(j)) + "\" but could not convert \"" + sLine.vArgs.at(j) + "\"",i);
				continue;
			}
		}

	}
	std::cout << "Script validation finished.\n";
	return bClean;
}

void ScriptHost::KeyCB(char key)
{
	if (!ScriptHost::m_bCanAcceptInput)
	{
		return;
	}
	auto &strCmd = GetHost().m_strCmd;
	switch (key)
	{
		case 0x1b: // Esc:
			strCmd.clear();
			/* FALLTHRU */
		case 0x7F: // Delete, backspace
		case 0x08:
			if (strCmd.size()>0)
			{
				strCmd.pop_back();
			}
			break;
		case 0x0d: // return;
			m_bCanAcceptInput = false;
			m_eCmdStatus = TermIdle;
			{
				std::lock_guard<std::mutex> lck (m_lckScript);
				m_script.push_back(m_strCmd);
			}
			m_bCanAcceptInput = true;
			break;
		case 0x9: // tab
		{
			auto pNext = m_strGLAutoC.upper_bound(m_strCmd);
			if (pNext != m_strGLAutoC.end())
			{
				auto posTrim = pNext->find_first_of('(');
				if (posTrim!=std::string::npos)
				{
					m_strCmd = pNext->substr(0,posTrim+1);
				}
				else
				{
					m_strCmd = *pNext;
				}
			}
		}
		break;
		default:
			strCmd.push_back(key);
	}
}

static constexpr char strOK[8] = "Success";
static constexpr char strFailed[6] = "Error";
static constexpr char strWait[8] = "Waiting";
static constexpr char strTimeout[10] = "Timed out";
static constexpr char strSyntax[22] = "Syntax/Argument Error";


/*
void ScriptHost::Draw()
{
	static const gsl::span<const char> pStatus[] = {strOK,
												strFailed,
												strWait,
												strTimeout,
												strSyntax};
	glPushMatrix();
		glTranslatef(3,12,0);
		glScalef(0.09,-0.14,0);
		glPushMatrix();
			// Show executing script, if in progress.
			glColor3f(.7,.7,1);
			if (m_iLine<m_scriptGL.size())
			{
				for (auto &c : m_scriptGL.at(m_iLine))
				{
					glutStrokeCharacter(GLUT_STROKE_MONO_ROMAN,c);
				}
			}
			else if (m_eCmdStatus != TermIdle) // Show result of last user command.
			{
				for (auto &c : pStatus[m_eCmdStatus-1])
				{
					glutStrokeCharacter(GLUT_STROKE_MONO_ROMAN, c);
				}
			}
		glPopMatrix();
		glTranslatef(0,-150,0);
		glColor3f(1,1,1);
		glutStrokeCharacter(GLUT_STROKE_MONO_ROMAN,'>');
		glPushMatrix();
			glColor3f(0.4,0.4,0.4);
			if (!m_strCmd.empty())
			{
				auto pNext = m_strGLAutoC.upper_bound(m_strCmd);
				if (pNext != m_strGLAutoC.end())
				{
					for (auto &c : *pNext)
					{
						glutStrokeCharacter(GLUT_STROKE_MONO_ROMAN,c);
					}
				}
			}
		glPopMatrix();
		glColor3f(1,1,1);
		for (auto &c : m_strCmd)
		{
			glutStrokeCharacter(GLUT_STROKE_MONO_ROMAN,c);
		}
		if (m_bFocus && ScriptHost::m_bCanAcceptInput)
		{
			glColor3f(0.5,0.5,0.5);
			glutStrokeCharacter(GLUT_STROKE_MONO_ROMAN,'_');
		}
	glPopMatrix();
}
*/
// Called from the execution context to process the menu action.
void ScriptHost::DispatchMenuCB()
{
	if (m_uiQueuedMenu !=0)
	{
		unsigned iID = m_uiQueuedMenu;
		m_uiQueuedMenu.store(0);
		IScriptable *pClient = m_mMenuBase2Client[(iID - iID%100)];
		pClient->ProcessMenu(iID%100);
	}
}

// Dispatches menu callbacks to the client.
void ScriptHost::MenuCB(int iID)
{
	//printf("Menu CB %d\n",iID);
	m_uiQueuedMenu.store(iID);
}

// Prefills/configures the autocompletion helper for the UI.
void ScriptHost::SetupAutocomplete()
{
	m_bIsTerminalEnabled = true;
	for (auto &c: m_clients)
	{
		const std::string& strName = c.first;
		// Insert just the prefix so that we don't show actions until *after* - for better tab-complete.
		//m_strGLAutoC.insert(strName + "::");
		for (auto &ActID : c.second->m_ActionIDs)
		{
			unsigned int ID = ActID.second;
			std::string strArgFmt = ActID.first;
			strArgFmt.push_back('(');
			if (c.second->m_ActionArgs[ID].size()>0)
			{
				for (auto &Arg : c.second->m_ActionArgs[ID])
				{
					strArgFmt += GetArgTypeNames().at(Arg) + ", ";
				}
				strArgFmt[strArgFmt.size()-2] = ')';
			}
			else
			{
				strArgFmt.push_back(')');
			}
			m_strGLAutoC.insert(strName + "::" + strArgFmt); // NOLINT - only incurred once at startup.
		}
	}
}

/*
void ScriptHost::CreateRootMenu(int iWinID)
{
	m_bMenuCreated = true;
	if (m_mMenuIDs.count("ScriptHost")!=0)
	{
		std::cerr << "Attempted to create a new root menu when one already exists. Ignoring...\n";
		return;
	}

	int iRootID = glutCreateMenu(ScriptHost::MenuCB);
	m_mMenuIDs["ScriptHost"] =  iRootID;
	glutSetWindow(iWinID);
	glutAttachMenu(m_mMenuIDs["ScriptHost"]);
	glutAttachMenu(GLUT_MIDDLE_BUTTON);

	// Run through the list of clients and add ones that registered before GLUT was ready:
	for (auto it = m_mMenuIDs.begin(); it!=m_mMenuIDs.end(); it++)
	{
		if (it->second != 0)
		{
			continue;
		}
		auto str = it->first;
		int iID = glutCreateMenu(ScriptHost::MenuCB);
		if (m_mClientEntries.count(str)==0)
		{
			glutAddMenuEntry("No options",m_mClient2MenuBase.at(str));
		}
		else
		{
			while (m_mClientEntries.at(str).size()>0)
			{
				auto entry = m_mClientEntries.at(str).back();
				glutAddMenuEntry(entry.first.c_str(), entry.second);
				m_mClientEntries.at(str).pop_back();
			}
		}

		m_mMenuIDs[str] = iID;
		glutSetMenu(iRootID);
		glutAddSubMenu(it->first.c_str(),iID);
	}
}
*/

bool ScriptHost::CheckArg(const ArgType &type, const std::string &val)
{
	try
	{
		switch (type)
		{
			case ArgType::Int:
				// cppcheck-suppress ignoredReturnValue
				std::stoi(val);
				return true;
			case ArgType::Float:
				// cppcheck-suppress ignoredReturnValue
				std::stof(val);
				return true;
			case ArgType::Bool:
				// cppcheck-suppress ignoredReturnValue
				std::stoi(val);
				return true;
			case ArgType::String:
				return true;
			case ArgType::uint32:
				// cppcheck-suppress ignoredReturnValue
				std::stoul(val);
				return true;
		}
	}
	catch(const std::exception &e)
	{
		return false;
	}
	return false;

}

void ScriptHost::ParseLine(unsigned int iLine)
{
	GetLineState().isValid = false;
	LineParts_t sLine;
	{
		std::lock_guard<std::mutex> lck(m_lckScript);
		sLine = ScriptHost::GetLineParts(m_script.at(iLine));
	}
	if (!sLine.isValid)
	{
		std::cout << "Failed to get parts\n";
		return;
	}

	GetLineState().iLine = iLine;
	if(!m_clients.count(sLine.strCtxt) || m_clients.at(sLine.strCtxt)==nullptr)
	{
		std::cout << "No client\n";
		return;
	}

	GetLineState().strCtxt = sLine.strCtxt;
	GetLineState().vArgs = sLine.vArgs;

	IScriptable *pClient = GetLineState().pClient = m_clients.at(sLine.strCtxt);

	if (!GetLineState().pClient->m_ActionIDs.count(sLine.strAct))
	{
		std::cout << "No action\n";
		return;
	}

	int iID = GetLineState().iActID = pClient->m_ActionIDs[sLine.strAct];

	if (GetLineState().vArgs.size()!=pClient->m_ActionArgs.at(iID).size())
	{
		std::cout << "Arg count mismatch\n";
		return;
	}
	GetLineState().isValid = true;
}

void ScriptHost::AddSubmenu(IScriptable *src)
{
	std::string strName = src->GetName();
	if (m_bMenuCreated)
	{
		std::cout << "Adding a menu entry after GLUT is up... TODO\n";
	}
	else if (!m_mMenuIDs.count(strName)) // GLUT isn't up yet, queue it for later.
	{
		m_mMenuIDs[strName] = 0;
		unsigned uiBase = 100U*m_mMenuIDs.size();
		m_mMenuBase2Client[uiBase] = src;
		m_mClient2MenuBase[strName] = uiBase;
		//printf("Registered %s with menu base %u\n",strName.c_str(),uiBase);
	}
}

void ScriptHost::AddMenuEntry(const std::string &strName, unsigned uiID, IScriptable* src)
{
	Expects(uiID<100); //NOLINT - complaint is in system include file
	auto strClient = src->GetName();
	unsigned uiBase = m_mClient2MenuBase.at(strClient);
	m_mClientEntries[strClient].push_back({strName, uiBase + uiID});

}


void ScriptHost::AddScriptable(const std::string &strName, IScriptable* src)
{
	if (m_clients.count(strName)==0)
	{
		m_clients[strName] = src;
		AddSubmenu(src);
	}
	else if (m_clients.at(strName)!=src)
	{
		int i=0;
		std::string strNew;
		std::cout << "# ScriptHost: NOTE: Duplicate context name (" << strName << ") with different pointer. Incrementing ID...\n";
		while (i<20)
		{
			i++;
			strNew = strName + std::to_string(i);
			if (m_clients.count(strNew)==0)
			{
				m_clients[strNew] = src;
				src->SetName(strNew);
				AddSubmenu(src);
				return;
			}
			else if (m_clients.at(strNew) == src)
			{
				return;
			}
		};
		std::cerr << "# ScriptHost: More than 20 duplicate identifiers. You should do something about that.\n";

	}
}

using LS = IScriptable::LineStatus;
void ScriptHost::OnMachineCycle(int64_t iGuestUs)
{
	std::string strLine; // Local copy to reduce mutex lock time.
	size_t scriptSize = 0;
    m_uiCurrentMs = iGuestUs/1000U;
	if (!m_bIsTerminalEnabled)
	{
		scriptSize = m_script.size();
		if (m_iLine>=scriptSize)
		{
			return; // Done.
		}
		strLine = m_script.at(m_iLine);
	}
	else
	{
		std::lock_guard<std::mutex> lck(m_lckScript);
		scriptSize = m_script.size();
		if (m_iLine>=scriptSize)
		{
			return; // Done.
		}
		strLine = m_script.at(m_iLine);
	}
	if (GetLineState().iLine != m_iLine || m_state == State::Idle)
	{
		m_state = State::Running;
		std::cout << "ScriptHost: Executing line " << strLine << "\n";
		ParseLine(m_iLine);
	}
	if (GetLineState().isValid)
	{
		LS lsResult = GetLineState().pClient->ProcessAction(GetLineState().iActID,GetLineState().vArgs);
		switch (lsResult)
		{
			case LS::Finished:
				if (m_bIsExecHold)
				{
					std::cerr << "FIXME: Exechold not implemented!\n";
					m_bIsExecHold = false;
					if (m_clients.count("Board") && m_clients.at("Board")->m_ActionIDs.count("Resume"))
					{
						int ID = m_clients.at("Board")->m_ActionIDs.at("Resume");
						LS lsUnpause = m_clients.at("Board")->ProcessAction(ID,{});
						if (lsUnpause !=LS::Finished)
						{
							std::cerr << "Client failed to resume after ExecHold - ID " << std::to_string(ID) << '\n';
						}
					}
					else
					{
						std::cerr << "Failed to resume after ExecHold!\n";
					}

				}
				m_iLine++; // This line is done, mobe on.
				m_iTimeoutCount = 0;
				m_eCmdStatus = TermSuccess;
				break;
			case LS::Unhandled:
				std::cout << "ScriptHost: Unhandled action, considering this an error.\n";
				/* FALLTHRU */
			case LS::Error:
			{
				std::cout << "ScriptHost: Script FAILED on line " << m_iLine << '\n';
				m_state = State::Error;
				m_iLine = scriptSize; // Error, end scripting.
				m_eCmdStatus = TermFailed;
				qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_SIGNAL);
				return;
			}
			case LS::HoldExec: // like waiting, but pauses board.
			{
				// TODO (vintagepc) : clean this up and also handle board1.
				if (m_clients.count("Board") && m_clients.at("Board")->m_ActionIDs.count("Pause"))
				{
					int ID = m_clients.at("Board")->m_ActionIDs.at("Pause");
					m_clients.at("Board")->ProcessAction(ID,{});
					m_bIsExecHold = true;
				}
			}
			/* FALLTHRU */
			case LS::Waiting:
			{
				if(m_iTimeoutCycles <0 || (m_iTimeoutCycles>=0 && ++m_iTimeoutCount<=m_iTimeoutCycles))
				{
					m_eCmdStatus = TermWaiting;
					break;
				}
				else
				{
					m_state = State::Timeout;
				}
			}
			/* FALLTHRU */
			case LS::Timeout:
			{
				m_state = State::Timeout;
				if (m_bQuitOnTimeout)
				{
					std::cout << "ScriptHost: Script TIMED OUT on " << strLine << ". Quitting...\n";
					m_iLine = scriptSize;
					qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_SIGNAL);
					return;
				}
				std::cout << "ScriptHost: Script TIMED OUT on #" << m_iLine << ": " << strLine << '\n';
				m_iLine++;
				m_iTimeoutCount = 0;
				m_eCmdStatus = TermTimedOut;
			}
			break;
			default:
				break;

		}
		if (m_iLine==scriptSize)
		{
			std::cout << "ScriptHost: Script FINISHED\n";
			m_bCanAcceptInput = true;
			m_state = State::Finished;
		}
	}
	else
	{
		std::cout << "# ScriptHost: ERROR: Invalid line/unrecognized command: " << m_iLine << ":" << strLine << '\n';
		m_state = State::Error;
		m_iLine = scriptSize;
		m_eCmdStatus = TermSyntax;
	}
}

void ScriptHost::AddScriptable_C(IScriptable* src)
{
	ScriptHost::AddScriptable(src->GetName() ,src);
	src->m_bRegistered = true;
}

std::set<std::string> ScriptHost::OnAutoComplete_C(std::string strCmd) {
	std::set<std::string> strMatches;
	auto pNext = m_strGLAutoC.upper_bound(strCmd);
	if (pNext == m_strGLAutoC.end()) {
		return strMatches;
	}
	while (pNext->rfind(strCmd,0)==0 && pNext!=m_strGLAutoC.end()) {
		strMatches.insert(*pNext);
		pNext++;
	}
	return strMatches;
}

void ScriptHost::OnCommand_C(std::string strCmd) {
	if (strCmd.empty())
	{
		return;
	}
	std::lock_guard<std::mutex> lck (m_lckScript);
	m_script.push_back(strCmd);
}

void ScriptHost::PrintToConsole_C(std::string strOut) {
	if (m_pConsole)
	{
		scriptcon_print_out(m_pConsole,strOut.c_str());
	}
	else
	{
		std::cerr << "Cannot print to console, it is NULL!\n";
	}
}

// C linkages
extern "C" {
    extern void scripthost_register_scriptable(script_handle src)
    {
        IScriptable *p = static_cast<IScriptable*>(src);
        ScriptHost::AddScriptable_C(p);
    }

    extern bool scripthost_setup(const char* strScript, void* pConsole)
    {
        ScriptHost::Init();
		ScriptHost::SetupAutocomplete();
		ScriptHost::SetConsole(pConsole);
		if (strScript!=nullptr)
		{
        	return ScriptHost::Setup(strScript, 0);
		}
		else
		{
			return false;
		}
    }

    extern int scripthost_run(int64_t iTime)
    {
        ScriptHost::OnMachineCycle(iTime);
		return ScriptHost::GetTermStatus();
    }

	extern int scripthost_get_int(script_args pArgs, uint8_t iIdx)
	{
		const std::vector<std::string> *pvArgs = static_cast<const std::vector<std::string>*>(pArgs);
		return std::stoi(pvArgs->at(iIdx));
	}

	extern const char* scripthost_get_string(script_args pArgs, uint8_t iIdx)
	{
		const std::vector<std::string> *pvArgs = static_cast<const std::vector<std::string>*>(pArgs);
		return pvArgs->at(iIdx).c_str();
	}

	extern bool scripthost_get_bool(script_args pArgs, uint8_t iIdx)
	{
		const std::vector<std::string> *pvArgs = static_cast<const std::vector<std::string>*>(pArgs);
		return std::stoi(pvArgs->at(iIdx))>0;
	}

	extern float scripthost_get_float(script_args pArgs, uint8_t iIdx)
	{
		const std::vector<std::string> *pvArgs = static_cast<const std::vector<std::string>*>(pArgs);
		return std::stof(pvArgs->at(iIdx));
	}

	extern void scripthost_autocomplete(void *p, const char* cmdline, void(*add_func)(void*,const char*)){
		std::set<std::string> strOpt = ScriptHost::OnAutoComplete_C(cmdline);
		for (auto &s : strOpt) {
			add_func(p,s.c_str());
		}
	}

	extern void scripthost_execute(const char* cmd) {
		ScriptHost::OnCommand_C(cmd);
	}

	extern void script_print_float(float fVal) {
		ScriptHost::PrintToConsole_C(std::to_string(fVal));
	}

	extern void script_print_int(int iVal) {
		ScriptHost::PrintToConsole_C(std::to_string(iVal));
	}
	extern void script_print_string(const char* pStr) {
		ScriptHost::PrintToConsole_C(pStr);
	}

}
