/*
*   Copyright (C) 2016 by Jonathan Naylor G4KLX
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 2 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program; if not, write to the Free Software
*   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "YSFReflector.h"
#include "StopWatch.h"
#include "Network.h"
#include "Version.h"
#include "Log.h"

#if defined(_WIN32) || defined(_WIN64)
#include <Windows.h>
#else
#include <sys/time.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
const char* DEFAULT_INI_FILE = "YSFReflector.ini";
#else
const char* DEFAULT_INI_FILE = "/etc/YSFReflector.ini";
#endif

#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <ctime>
#include <cstring>

int main(int argc, char** argv)
{
	const char* iniFile = DEFAULT_INI_FILE;
	if (argc > 1) {
		for (int currentArg = 1; currentArg < argc; ++currentArg) {
			std::string arg = argv[currentArg];
			if ((arg == "-v") || (arg == "--version")) {
				::fprintf(stdout, "YSFReflector version %s\n", VERSION);
				return 0;
			}
			else if (arg.substr(0, 1) == "-") {
				::fprintf(stderr, "Usage: YSFReflector [-v|--version] [filename]\n");
				return 1;
			}
			else {
				iniFile = argv[currentArg];
			}
		}
	}

	CYSFReflector* reflector = new CYSFReflector(std::string(iniFile));
	reflector->run();
	delete reflector;

	return 0;
}

CYSFReflector::CYSFReflector(const std::string& file) :
m_conf(file),
m_repeaters()
{
}

CYSFReflector::~CYSFReflector()
{
}

void CYSFReflector::run()
{
	bool ret = m_conf.read();
	if (!ret) {
		::fprintf(stderr, "YSFRefector: cannot read the .ini file\n");
		return;
	}

	ret = ::LogInitialise(m_conf.getLogFilePath(), m_conf.getLogFileRoot(), m_conf.getLogFileLevel(), m_conf.getLogDisplayLevel());
	if (!ret) {
		::fprintf(stderr, "YSFReflector: unable to open the log file\n");
		return;
	}

#if !defined(_WIN32) && !defined(_WIN64)
	bool m_daemon = m_conf.getDaemon();
	if (m_daemon) {
		// Create new process
		pid_t pid = ::fork();
		if (pid == -1) {
			::LogWarning("Couldn't fork() , exiting");
			return -1;
		}
		else if (pid != 0)
			exit(EXIT_SUCCESS);

		// Create new session and process group
		if (::setsid() == -1) {
			::LogWarning("Couldn't setsid(), exiting");
			return -1;
		}

		// Set the working directory to the root directory
		if (::chdir("/") == -1) {
			::LogWarning("Couldn't cd /, exiting");
			return -1;
		}

		::close(STDIN_FILENO);
		::close(STDOUT_FILENO);
		::close(STDERR_FILENO);

		//If we are currently root...
		if (getuid() == 0) {
			struct passwd* user = ::getpwnam("mmdvm");
			if (user == NULL) {
				::LogError("Could not get the mmdvm user, exiting");
				return -1;
			}

			uid_t mmdvm_uid = user->pw_uid;
			gid_t mmdvm_gid = user->pw_gid;

			//Set user and group ID's to mmdvm:mmdvm
			if (setgid(mmdvm_gid) != 0) {
				::LogWarning("Could not set mmdvm GID, exiting");
				return -1;
			}

			if (setuid(mmdvm_uid) != 0) {
				::LogWarning("Could not set mmdvm UID, exiting");
				return -1;
			}

			//Double check it worked (AKA Paranoia) 
			if (setuid(0) != -1) {
				::LogWarning("It's possible to regain root - something is wrong!, exiting");
				return -1;
			}
		}
	}
#endif

	CNetwork network(m_conf.getNetworkPort(), m_conf.getName(), m_conf.getDescription(), m_conf.getNetworkDebug());

	ret = network.open();
	if (!ret)
		return;

	CStopWatch stopWatch;
	stopWatch.start();

	CTimer pollTimer(1000U, 5U);
	pollTimer.start();

	LogMessage("Starting YSFReflector-%s", VERSION);

	CTimer watchdogTimer(1000U, 0U, 1500U);

	unsigned char tag[YSF_CALLSIGN_LENGTH];
	unsigned char src[YSF_CALLSIGN_LENGTH];
	unsigned char dst[YSF_CALLSIGN_LENGTH];

	for (;;) {
		unsigned char buffer[200U];

		unsigned int len = network.readData(buffer);
		if (len > 0U) {
			if (!watchdogTimer.isRunning()) {
				::memcpy(tag, buffer + 4U, YSF_CALLSIGN_LENGTH);

				if (::memcmp(buffer + 14U, "          ", YSF_CALLSIGN_LENGTH) != 0)
					::memcpy(src, buffer + 14U, YSF_CALLSIGN_LENGTH);
				else
					::memcpy(src, "??????????", YSF_CALLSIGN_LENGTH);

				if (::memcmp(buffer + 24U, "          ", YSF_CALLSIGN_LENGTH) != 0)
					::memcpy(dst, buffer + 24U, YSF_CALLSIGN_LENGTH);
				else
					::memcpy(dst, "??????????", YSF_CALLSIGN_LENGTH);

				LogMessage("Received data from %10.10s to %10.10s at %10.10s", src, dst, buffer + 4U);
			} else {
				if (::memcmp(tag, buffer + 4U, YSF_CALLSIGN_LENGTH) == 0) {
					bool changed = false;

					if (::memcmp(buffer + 14U, "          ", YSF_CALLSIGN_LENGTH) != 0 && ::memcmp(src, "??????????", YSF_CALLSIGN_LENGTH) == 0) {
						::memcpy(src, buffer + 14U, YSF_CALLSIGN_LENGTH);
						changed = true;
					}

					if (::memcmp(buffer + 24U, "          ", YSF_CALLSIGN_LENGTH) != 0 && ::memcmp(dst, "??????????", YSF_CALLSIGN_LENGTH) == 0) {
						::memcpy(dst, buffer + 24U, YSF_CALLSIGN_LENGTH);
						changed = true;
					}

					if (changed)
						LogMessage("Received data from %10.10s to %10.10s at %10.10s", src, dst, buffer + 4U);
				}
			}

			// Only accept transmission from an already accepted repeater
			if (::memcmp(tag, buffer + 4U, YSF_CALLSIGN_LENGTH) == 0) {
				watchdogTimer.start();

				std::string callsign = std::string((char*)(buffer + 4U), YSF_CALLSIGN_LENGTH);
				CYSFRepeater* rpt = findRepeater(callsign);
				if (rpt != NULL) {
					for (std::vector<CYSFRepeater*>::const_iterator it = m_repeaters.begin(); it != m_repeaters.end(); ++it) {
						if ((*it)->m_callsign != callsign)
							network.writeData(buffer, (*it)->m_address, (*it)->m_port);
					}
				}

				if (buffer[34U] == 0x01U) {
					LogMessage("Received end of transmission");
					watchdogTimer.stop();
				}
			}
		}

		// Refresh/add repeaters based on their polls
		std::string callsign;
		in_addr address;
		unsigned int port;
		bool ret = network.readPoll(callsign, address, port);
		if (ret) {
			CYSFRepeater* rpt = findRepeater(callsign);
			if (rpt == NULL) {
				LogMessage("Adding %s", callsign.c_str());
				rpt = new CYSFRepeater;
				rpt->m_timer.start();
				rpt->m_callsign = callsign;
				rpt->m_address  = address;
				rpt->m_port     = port;
				m_repeaters.push_back(rpt);
				network.setCount(m_repeaters.size());
			} else {
				rpt->m_timer.start();
				rpt->m_address = address;
				rpt->m_port    = port;
			}
		}

		unsigned int ms = stopWatch.elapsed();
		stopWatch.start();

		network.clock(ms);

		pollTimer.clock(ms);
		if (pollTimer.hasExpired()) {
			for (std::vector<CYSFRepeater*>::const_iterator it = m_repeaters.begin(); it != m_repeaters.end(); ++it)
				network.writePoll((*it)->m_address, (*it)->m_port);
			pollTimer.start();
		}

		// Remove any repeaters that haven't reported for a while
		for (std::vector<CYSFRepeater*>::iterator it = m_repeaters.begin(); it != m_repeaters.end(); ++it)
			(*it)->m_timer.clock(ms);

		for (std::vector<CYSFRepeater*>::iterator it = m_repeaters.begin(); it != m_repeaters.end(); ++it) {
			if ((*it)->m_timer.hasExpired()) {
				LogMessage("Removing %s", (*it)->m_callsign.c_str());
				m_repeaters.erase(it);
				network.setCount(m_repeaters.size());
				break;
			}
		}

		watchdogTimer.clock(ms);
		if (watchdogTimer.isRunning() && watchdogTimer.hasExpired()) {
			LogMessage("Network watchdog has expired");
			watchdogTimer.stop();
		}

		if (ms < 5U) {
#if defined(_WIN32) || defined(_WIN64)
			::Sleep(5UL);		// 5ms
#else
			::usleep(5000);		// 5ms
#endif
		}
	}

	network.close();

	::LogFinalise();
}

CYSFRepeater* CYSFReflector::findRepeater(const std::string& callsign) const
{
	for (std::vector<CYSFRepeater*>::const_iterator it = m_repeaters.begin(); it != m_repeaters.end(); ++it) {
		if ((*it)->m_callsign == callsign)
			return *it;
	}

	return NULL;
}
