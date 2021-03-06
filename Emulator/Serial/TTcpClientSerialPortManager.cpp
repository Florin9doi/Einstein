// ==============================
// File:			TTcpClientSerialPortManager.cp
// Project:			Einstein
//
// Copyright 2020 by Matthias Melcher (mm@matthiasm.com).
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
// ==============================
// $Id$
// ==============================

#include "TTcpClientSerialPortManager.h"
#include "app/TPathHelper.h"

// POSIX
#include <sys/types.h>
#include <signal.h>
#include <string.h>

#if !TARGET_OS_WIN32
#include <unistd.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#endif

#include "Emulator/Log/TLog.h"
#include "Emulator/TInterruptManager.h"
#include "Emulator/TMemory.h"

/*
 The TCP Client class emulates a serial port by connecting to the server
 whenever needed through a TCP network connection.

 The actual client runs in a thread and has to watch two file descriptor.
 The first descriptor is the end of a pipe to exchange commend with the main
 thread. The second connection is the actual socket.

 The pipe is created by the main thread and stays alive with the helper
 thread from a call to run() until the class is destroyed.

 The socket is not created until NewtonOS decides to touch the serial port. The
 socket will not disconnect from the client side, but must handle a
 disconnection from the server gracefully. We can consider a timeout for a
 client disconnect if there is no more traffic on the serial port as NewtonOS
 does send a keep-alive message regularly if the connection should be kept.

 The class can be destroyed if the user changes settings in the preferences
 panel (this is not yet (3-21-2020) implemented).
 */


// -------------------------------------------------------------------------- //
//  * TTcpClientSerialPortManager()
// Emulate a serial connection using a TCP client socket.
// -------------------------------------------------------------------------- //
TTcpClientSerialPortManager::TTcpClientSerialPortManager(
													 TLog* inLog,
													 TSerialPorts::EPortIndex inPortIx)
:	TBasicSerialPortManager(inLog, inPortIx),
	mCommandPipe{ -1, -1 },
	mTcpSocket( -1 ),
	mWorkerThreadIsRunning( false ),
	mWorkerThread( 0L ),
	mIsConnected( false ),
	mReconnectTimeout( 0 )
{
	mServer = strdup("127.0.0.1");
	mPort = 3679;
}


// -------------------------------------------------------------------------- //
//  * ~TTcpClientSerialPortManager( void )
// -------------------------------------------------------------------------- //
TTcpClientSerialPortManager::~TTcpClientSerialPortManager()
{
	if (mWorkerThreadIsRunning) {
		Disconnect();
		TriggerEvent('q');
		pthread_join(mWorkerThread, nullptr);
		mWorkerThreadIsRunning = false;
	}
	if (mCommandPipe[0]!=-1)
		::close(mCommandPipe[0]);
	if (mCommandPipe[1]!=-1)
		::close(mCommandPipe[1]);
	if (mServer)
		free(mServer);
}

// -------------------------------------------------------------------------- //
//  * run( TInterruptManager*, TDMAManager*, TMemory* )
// -------------------------------------------------------------------------- //
void TTcpClientSerialPortManager::run(TInterruptManager* inInterruptManager,
								  TDMAManager* inDMAManager,
								  TMemory* inMemory)
{
	mInterruptManager = inInterruptManager;
	mDMAManager = inDMAManager;
	mMemory = inMemory;

	if (mWorkerThreadIsRunning) {
		printf("***** Error: TTcpClientSerialPortManager::run worker thread is already running.\n");
		return;
	}

	if (mCommandPipe[0]!=-1 || mCommandPipe[1]!=-1) {
		printf("***** Error: TTcpClientSerialPortManager::run trying to open command pipe again.\n");
		return;
	}

	// create the thread communication pipe
	int err = pipe(mCommandPipe);
	if (err==-1) {
		printf("***** TTcpClientSerialPortManager::run: Error opening pipe - %s (%d).\n", strerror(errno), errno);
		return;
	}

	// create the thread and let it run until we send it the quit signal
	int ptErr = ::pthread_create( &mWorkerThread, nullptr, &SHandleDMA, this );
	if (ptErr==-1) {
		printf("***** TTcpClientSerialPortManager::run: Error creating pthread - %s (%d).\n", strerror(errno), errno);
		return;
	}

	mWorkerThreadIsRunning = true;
}

// -------------------------------------------------------------------------- //
// DMA or interrupts trigger a command that must be handled by a derived class.
// -------------------------------------------------------------------------- //
void TTcpClientSerialPortManager::TriggerEvent(KUInt8 cmd)
{
	if (mCommandPipe[0]==-1 || mCommandPipe[1]==-1) {
		printf("***** Error: TTcpClientSerialPortManager::TriggerEvent called without pipes.\n");
		return;
	}

	::write(mCommandPipe[1], &cmd, 1);
}


// -------------------------------------------------------------------------- //
// * Disconnect
//		Disconnect from the server.
// -------------------------------------------------------------------------- //
bool
TTcpClientSerialPortManager::Disconnect()
{
	bool wasConnected = mIsConnected;
	if (mTcpSocket!=-1) {
		time_t now;
		time(&now);
		mReconnectTimeout = now+3;
		::close(mTcpSocket);
		mTcpSocket = -1;
	}
	mIsConnected = false;
	return wasConnected;
}


// -------------------------------------------------------------------------- //
// * Connect
//		Create a TCP socket and try to connect to the server.
// -------------------------------------------------------------------------- //
bool
TTcpClientSerialPortManager::Connect()
{
	Disconnect();

	// reject reconnect request if we have not hit the timeout yet
	time_t now;
	time(&now);
	if (now < mReconnectTimeout)
		return false;

	printf("***** connect?\n");

	// Create a new socket
	mTcpSocket = ::socket(PF_INET, SOCK_STREAM, 0);
	if (mTcpSocket==-1) {
		printf("***** TTcpClientSerialPortManager::Connect: Error creating socket - %s (%d).\n", strerror(errno), errno);
		return false;
	}

	// Create the address information to our server
	struct sockaddr_in server{};
	memset(&server, 0, sizeof(struct sockaddr_in));
	server.sin_family = AF_INET;
	server.sin_port = htons(static_cast<uint16_t>(mPort));

	if (inet_pton(AF_INET, mServer, &server.sin_addr)<=0)
	{
		printf("***** TTcpClientSerialPortManager::Connect: Error in inet_pton.\n");
		Disconnect();
		return false;
	}

	// Try to connect to the server
	int err = ::connect(mTcpSocket, (struct sockaddr *)&server, sizeof(server));
	if (err==-1) {
		printf("***** TTcpClientSerialPortManager::Connect: Error connecting to server - %s (%d).\n", strerror(errno), errno);
		Disconnect();
		return false;
	}

	mIsConnected = true;
	printf("***** connected!\n");

	return true;
}



static void sigpipe_handler(int unused)
{
	// don't do anything. The server disconnected faster than the client would realize that.
}


// -------------------------------------------------------------------------- //
//  * HandleDMA()
//		This endless loop watches DMA registers as they are changed by the
//		OS, and read and writes data via the TCP socket.
//		It can also trigger interrupts when buffers empty, fill, or overflow.
// -------------------------------------------------------------------------- //
void
TTcpClientSerialPortManager::HandleDMA()
{
	static struct sigaction action { sigpipe_handler };
	sigaction(SIGPIPE, &action, nullptr);

	// thread loops and handles pipe, port, and DMA
	struct timeval timeout{};
	for (;;)
	{
		bool needTimer = false;
		fd_set watchFDs;

		FD_ZERO(&watchFDs);
		FD_SET(mCommandPipe[0], &watchFDs);
		if (IsConnected())
			FD_SET(mTcpSocket, &watchFDs);

		if (mTxDMAControl&0x00000002) { // DMA is enabled
			if (mTxDMADataCountdown) {
				needTimer = true;
				timeout.tv_sec = 0;
				timeout.tv_usec = 260; // one byte at 38400bps serial port speed
			}
		}

		int ret = select(FD_SETSIZE, &watchFDs, 0L, 0L, needTimer ? &timeout : 0L);
		if (ret==-1) {
			printf("***** TTcpClientSerialPortManager::HandleDMA: Error waiting for sockets - %s (%d).\n", strerror(errno), errno);
			continue;
		}
		
		// handle receiving DMA
		if (IsConnected() && FD_ISSET(mTcpSocket, &watchFDs))
			HandleDMAReceive();

		// handle transmitting DMA
		HandleDMASend();

		// handle commands from the command pipe
		if (FD_ISSET(mCommandPipe[0], &watchFDs)) {
			KUInt8 cmd = 'e';
			::read(mCommandPipe[0], &cmd, 1);
			switch (cmd) {
				case 'q':	// quit this thread
					Disconnect();
					return;
				case 'c':
					//printf("Update control\n");
					// we may want to do something smart when the serial controls change
					break;
				default:
					printf("***** TTcpClientSerialPortManager::HandleDMA: Error reading pipe - '%c': %s.\n", cmd, strerror(errno));
					break;
			}
		}

	}
}


// -------------------------------------------------------------------------- //
//  * HandleDMASend()
//		Send a single byte. We are throtteling data transfor to match a
//		maximum of 38400bps which is the maximum that NewtonSO can handle.
// -------------------------------------------------------------------------- //
void
TTcpClientSerialPortManager::HandleDMASend()
{
	if (mTxDMAControl&0x00000002) { // DMA is enabled
		if (mTxDMADataCountdown) {
			// write a byte
			KUInt8 data = 0;
			mMemory->ReadBP(mTxDMAPhysicalData, data);
			if (!IsConnected())
				Connect();
			// TODO: if we can't get a connection at this point, should we flush the entire buffer?
			if (IsConnected()) {
				::write(mTcpSocket, &data, 1);
				//printf("Sending 0x%02x\n", data);
			} else {
				//printf("Sending to null 0x%02x\n", data);
			}
			mTxDMAPhysicalData++;
			mTxDMABufferSize--;
			if (mTxDMABufferSize==0) {
				mTxDMAPhysicalData = mTxDMAPhysicalBufferStart;
			}
			mTxDMADataCountdown--;
			if (mTxDMADataCountdown==0) {
				// trigger a "send buffer empty" interrupt
				mTxDMAEvent = 0x00000080;
				mInterruptManager->RaiseInterrupt(0x00000100);
			}
		}
	}
}


// -------------------------------------------------------------------------- //
//  * HandleDMAReceive()
// -------------------------------------------------------------------------- //
void
TTcpClientSerialPortManager::HandleDMAReceive()
{
	// The original Newton hardware and OS
	// did have timing issues whan the server PC communicated faster as
	// expected in the year 1996, which leaconnecting to d to CPU cycle burning software
	// like "slowdown.exe".

	// read up to 1024 bytes that come in through the serial port
	KUInt8 buf[1026];
	int n = (int)::read(mTcpSocket, buf, 1024);
	if (n==-1) {
		printf("***** TTcpClientSerialPortManager::HandleDMAReceive: Error reading from TCP/IP socket - %s (%d).\n", strerror(errno), errno);
		Disconnect();
	} else if (n==0) {
		printf("***** Server side disconnect.\n");
		Disconnect();
	} else {
		usleep(n * 100); // up to 1/10th of a second, so that we do not overwhelm the Newton
		for (KUInt32 i=0; i<n; i++) {
			KUInt8 data = buf[i];
			//printf("Received 0x%02x\n", data);
			mMemory->WriteBP(mRxDMAPhysicalData, data);
			mRxDMAPhysicalData++;
			mRxDMABufferSize--;
			if (mRxDMABufferSize==0) {
				mRxDMAPhysicalData = mRxDMAPhysicalBufferStart;
			}
			mRxDMADataCountdown--;
			if (mRxDMADataCountdown==0) {
				// buffer overflow?
			}
		}
		mRxDMAEvent = 0x00000040;
		mInterruptManager->RaiseInterrupt(0x00000080); // 0x00000180
	}
}

///
/// GIve NewtonScrip access to our list of options
///
void TTcpClientSerialPortManager::NSGetOptions(TNewt::RefArg frame)
{
	using namespace TNewt;
	char buf[32];
	snprintf(buf, 30, "%d", mPort);
	SetFrameSlot(frame, RefVar(MakeSymbol("tcpServer")), RefVar(MakeString(mServer)));
	SetFrameSlot(frame, RefVar(MakeSymbol("tcpPort")), RefVar(MakeString(buf)));
}

///
/// Set options from NewtonScript
///
void TTcpClientSerialPortManager::NSSetOptions(TNewt::RefArg inFrame)
{
	using namespace TNewt;
	char server[256] = { 0 };
	char portStr[80] = { 0 };
	KSInt32 port = 0;
	bool setServer = false;
	bool setPort = false;
	bool mustReconnect = false;

	NewtRef frame = inFrame.Ref();
	NewtRef tcpServerRef = GetFrameSlotRef(frame, MakeSymbol("tcpServer"));
	if (RefIsString(tcpServerRef)) {
		RefToString(tcpServerRef, server, sizeof(server));
		setServer = true;
	}
	NewtRef tcpPortRef = GetFrameSlot(frame, MakeSymbol("tcpPort"));
	if (RefIsString(tcpPortRef)) {
		RefToString(tcpPortRef, portStr, sizeof(portStr));
		port = atoi(portStr);
		if (port==0) port = 3679;
		setPort = true;
	}

	printf("INFO: TTcpClientSerialPortManager::NSSetOptions: (\"%s\", %d)\n", mServer, mPort);
	if (setServer) {
		if (strcmp(mServer, server)!=0) {
			if (mServer) ::free(mServer);
			mServer = strdup(server);
			mustReconnect = true;
		}
		printf("INFO:             Setting server to \"%s\".\n", server);
	}
	if (setPort) {
		if (mPort!=port) {
			mPort = port;
			mustReconnect = true;
		}
		printf("INFO:             Setting port to %d.\n", port);
	}

	if (mustReconnect) {
		Disconnect(); // force the server to reconnect
		printf("INFO: TTcpClientSerialPortManager::NSSetOptions: must reconnect\n");
	}
}

void TTcpClientSerialPortManager::SetServerAddress(const char *inAddress)
{
	if (mServer) ::free(mServer);
	mServer = strdup(inAddress);
}

void TTcpClientSerialPortManager::SetServerPort(int inPort)
{
	mPort = inPort;
}

char *TTcpClientSerialPortManager::GetServerAddressDup()
{
	return strdup(mServer);
}

int TTcpClientSerialPortManager::GetServerPort()
{
	return mPort;
}


// ================================================================== //
// You never finish a program, you just stop working on it.           //
//  - Anonymous                                                       //
//                                                                    //
// So true!                                                           //
//  - Matthias Melcher                                                //
// ================================================================== //
