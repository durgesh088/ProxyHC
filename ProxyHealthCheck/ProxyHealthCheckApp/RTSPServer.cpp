/**********
This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the
Free Software Foundation; either version 3 of the License, or (at your
option) any later version. (See <http://www.gnu.org/copyleft/lesser.html>.)

This library is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
more details.

You should have received a copy of the GNU Lesser General Public License
along with this library; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
**********/
// Copyright (c) 1996-2017, Live Networks, Inc.  All rights reserved
// A test program that demonstrates how to stream - via unicast RTP
// - various kinds of file on demand, using a built-in RTSP server.
// main program

#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"
//#include "../liveMedia/include/RTSPServer.hh"
#include <iostream>
#include "../groupsock/include/GroupsockHelper.hh"
#include <pthread.h>
#include <list>
#include <iterator>
#include <unistd.h>
#include <fstream>
#include <unistd.h>
#include <fcntl.h>
#include "../liveMedia/include/RTSPServer.hh"
#include <csignal>


volatile char RTSP_EVENT = 0;

volatile bool fIsDisconnected = true;

#define LISTEN_BACKLOG_SIZE 2000

using namespace std;

UsageEnvironment* env;

// To make the second and subsequent client for each stream reuse the same
// input stream as the first client (rather than playing the file from the
// start for each client), change the following "False" to "True":
Boolean reuseFirstSource = False;

// To stream *only* MPEG-1 or 2 video "I" frames
// (e.g., to reduce network bandwidth),
// change the following "False" to "True":
Boolean iFramesOnly = False;

static void announceStream(RTSPServer* rtspServer, ServerMediaSession* sms,
			   char const* streamName, char const* inputFileName); // fwd

static char newDemuxWatchVariable;

static MatroskaFileServerDemux* matroskaDemux;
static void onMatroskaDemuxCreation(MatroskaFileServerDemux* newDemux, void* /*clientData*/) {
  matroskaDemux = newDemux;
  newDemuxWatchVariable = 1;
}

static OggFileServerDemux* oggDemux;
static void onOggDemuxCreation(OggFileServerDemux* newDemux, void* /*clientData*/) {
  oggDemux = newDemux;
  newDemuxWatchVariable = 1;
}

void handleRegisterResponse(RTSPServer* rtspServer, unsigned requestId, int resultCode, char* resultString);

class DerivedRTSPServer : public RTSPServer {
  public:
    class DerivedRTSPClientSession : public RTSPClientSession {
      public:
        virtual void handleCmd_TEARDOWN(RTSPClientConnection* ourClientConnection, ServerMediaSubsession* subsession);    
        DerivedRTSPClientSession( RTSPServer& ourServer, u_int32_t sessionId );
    }; 

    class DerivedRTSPClientConnection : public RTSPClientConnection {
      public:
        virtual void handleCmd_DESCRIBE(char const* urlPreSuffix, char const* urlSuffix, char const* fullRequestStr);
        DerivedRTSPClientConnection( RTSPServer& ourServer, int clientSocket, struct sockaddr_in clientAddr);
    };

    void closeSMS(ServerMediaSession* sms);
    RTSPServer::RTSPClientSession* createNewClientSession(u_int32_t sessionId);
    RTSPServer::RTSPClientConnection* createNewClientConnection(int clientSocket, struct sockaddr_in clientAddr);
    DerivedRTSPServer(UsageEnvironment& env,int ourSocket, Port ourPort,UserAuthenticationDatabase* authDatabase,unsigned reclamationSeconds);
    void workerThreadImpl();

    //bool fIsDisconnected;
    bool fIsRegistered;
    ServerMediaSession* sms;
    char const* proxyServer;
    char const* streamName;
};


RTSPServer::RTSPClientSession* DerivedRTSPServer::createNewClientSession(u_int32_t sessionId) {
    return new DerivedRTSPClientSession( *this, sessionId );
} 

RTSPServer::RTSPClientConnection* DerivedRTSPServer::createNewClientConnection(int clientSocket, struct sockaddr_in clientAddr) {
    return new DerivedRTSPClientConnection( *this, clientSocket, clientAddr);
}

DerivedRTSPServer::DerivedRTSPClientSession::DerivedRTSPClientSession( RTSPServer& ourServer, u_int32_t sessionId )
    : RTSPClientSession( ourServer, sessionId ) {
    std::cout<<"Constructor of DerivedRTSPClientSession\n";
}

DerivedRTSPServer::DerivedRTSPClientConnection::DerivedRTSPClientConnection( RTSPServer& ourServer, int clientSocket, struct sockaddr_in clientAddr): RTSPClientConnection( ourServer, clientSocket, clientAddr ) {
    std::cout<<"Constructor of DerivedRTSPClientConnection\n";
}

void DerivedRTSPServer::DerivedRTSPClientSession::handleCmd_TEARDOWN(RTSPServer::RTSPClientConnection* ourClientConnection, ServerMediaSubsession* subsession) {
  fprintf(stderr,"In handleCmdTEARDOWN\n");
  //fIsDisconnected = true;
  std::cout<<"set fIsDisconnected to "<<fIsDisconnected<<"\n";
  RTSPServer::RTSPClientSession::handleCmd_TEARDOWN( ourClientConnection, subsession );
} 

void DerivedRTSPServer::DerivedRTSPClientConnection::handleCmd_DESCRIBE(char const* urlPreSuffix, char const* urlSuffix, char const* fullRequestStr) {
  fprintf(stderr,"In DerivedRTSPServer::DerivedRTSPClientConnection::handleCmd_DESCRIBE()\n");
  ofstream outputFile;
  outputFile.open("/home/ec2-user/HC/ProxyServer/Registration_success");
  RTSPServer::RTSPClientConnection::handleCmd_DESCRIBE( urlPreSuffix, urlSuffix, fullRequestStr );
  //outputFile << (DerivedRTSPServer*)rtspServer->proxyServer << endl;
  outputFile.close();
} 

void DerivedRTSPServer::closeSMS(ServerMediaSession* sms) {
  GenericMediaServer::closeAllClientSessionsForServerMediaSession( sms );
  GenericMediaServer::deleteServerMediaSession(sms);
}

DerivedRTSPServer::DerivedRTSPServer(UsageEnvironment& env,int ourSocket, Port ourPort,UserAuthenticationDatabase* authDatabase,unsigned reclamationSeconds):RTSPServer(env,ourSocket,ourPort,authDatabase,reclamationSeconds){
  fIsDisconnected = true;
  sms = NULL;
}

DerivedRTSPServer* rtspServer ;
ServerMediaSession* sms;

int setUpOurSocket(UsageEnvironment& env, Port& ourPort) {
  int ourSocket = -1;

  do {
    // The following statement is enabled by default.
    // Don't disable it (by defining ALLOW_SERVER_PORT_REUSE) unless you know what you're doing.
#if !defined(ALLOW_SERVER_PORT_REUSE) && !defined(ALLOW_RTSP_SERVER_PORT_REUSE)
    // ALLOW_RTSP_SERVER_PORT_REUSE is for backwards-compatibility #####
    NoReuse dummy(env); // Don't use this socket if there's already a local server using it
#endif

    ourSocket = setupStreamSocket(env, ourPort);
    if (ourSocket < 0) break;

#ifdef DEBUG
    fprintf(stderr,"ourSocket : %d",ourSocket);
#endif

    // Make sure we have a big send buffer:
    if (!increaseSendBufferTo(env, ourSocket, 50*1024)) break;

    // Allow multiple simultaneous connections:
    if (listen(ourSocket, LISTEN_BACKLOG_SIZE) < 0) {
      env.setResultErrMsg("listen() failed: ");
      break;
    }

    if (ourPort.num() == 0) {
      // bind() will have chosen a port for us; return it also:
      if (!getSourcePort(env, ourSocket, ourPort)) break;
    }

    return ourSocket;
  } while (0);

  if (ourSocket != -1) ::closeSocket(ourSocket);
  return -1;
}

static void* workerThread ( void* arg ) {
    ((DerivedRTSPServer*)arg)->workerThreadImpl();
    return NULL;
}

static void signal_handler( int signum ) {
    fprintf( stderr,"caught signal %s(%d)\n", strsignal(signum), signum );
    //rtspServer->deregisterStream(sms,rtspServer->proxyServer, 8554,NULL,"intellivision","intellivision",rtspServer->streamName);
    rtspServer->closeSMS(sms);
    //rtspServer->closeAllClientSessionsForServerMediaSession( sms );
    RTSP_EVENT = 1;
}

int main(int argc, char** argv) {
  // Begin by setting up our usage environment:
  if(argc < 3) {
    std::cout<<"Usage : "<<argv[0]<<" <proxyServer> <streamName>\n";
    exit(0);
  }

  TaskScheduler* scheduler = BasicTaskScheduler::createNew();
  env = BasicUsageEnvironment::createNew(*scheduler);

  UserAuthenticationDatabase* authDB = NULL;
#ifdef ACCESS_CONTROL
  // To implement client access control to the RTSP server, do the following:
  authDB = new UserAuthenticationDatabase;
  authDB->addUserRecord("user", "pass"); // replace these with real strings
  // Repeat the above with each <username>, <password> that you wish to allow
  // access to the server.
#endif
  Port ourPort = 8554;
  int ourSocket = setUpOurSocket( *env, ourPort );
  // Create the RTSP server:
  rtspServer = new DerivedRTSPServer(*env, ourSocket, 8554, authDB, 65);
  //RTSPServer* rtspServer = RTSPServer::createNew(*env, 8554, authDB);
  if (rtspServer == NULL) {
    *env << "Failed to create RTSP server: " << env->getResultMsg() << "\n";
    exit(1);
  }

  char const* descriptionString
    = "Session streamed by \"Health Check RTSPServer\"";


  // A Matroska ('.mkv') file, with video+audio+subtitle streams:
  
  char const* streamName = "healthCheckFileStream";
  char const* inputFileName = "test.mkv";
  sms = ServerMediaSession::createNew(*env, streamName, streamName,
			             descriptionString);

  newDemuxWatchVariable = 0;
  MatroskaFileServerDemux::createNew(*env, inputFileName, onMatroskaDemuxCreation, NULL);
  env->taskScheduler().doEventLoop(&newDemuxWatchVariable);

  Boolean sessionHasTracks = False;
  ServerMediaSubsession* smss;
  while ((smss = matroskaDemux->newServerMediaSubsession()) != NULL) {
    sms->addSubsession(smss);
    sessionHasTracks = True;
  }
  if (sessionHasTracks) {
    rtspServer->addServerMediaSession(sms);
  }
  // otherwise, because the stream has no tracks, we don't add a ServerMediaSession to the server.

  rtspServer->sms = sms;
  announceStream(rtspServer, sms, streamName, inputFileName);
  
  // Also, attempt to create a HTTP server for RTSP-over-HTTP tunneling.
  // Try first with the default HTTP port (80), and then with the alternative HTTP
  // port numbers (8000 and 8080).

  /*if (rtspServer->setUpTunnelingOverHTTP(80) || rtspServer->setUpTunnelingOverHTTP(8000) || rtspServer->setUpTunnelingOverHTTP(8080)) {
    *env << "\n(We use port " << rtspServer->httpServerPortNum() << " for optional RTSP-over-HTTP tunneling.)\n";
  } else {
    *env << "\n(RTSP-over-HTTP tunneling is not available.)\n";
  }*/

  /*int count = 1;
  std::cout<<"Adding Proxy Servers to list\n";
  while(count < argc) {
    std::cout<<"Server : "<<argv[count]<<"\n"; 
    rtspServer->proxyServers.push_back(argv[count]);
    count++;
  }*/

  signal( SIGINT, signal_handler );
 
  rtspServer->proxyServer = argv[1];
  rtspServer->streamName = argv[2]; 
  rtspServer->registerStream(sms,rtspServer->proxyServer, 8554,NULL,
                                                 "intellivision","intellivision",True,rtspServer->streamName);
  //sleep(10);
  //rtspServer->deregisterStream(sms,rtspServer->proxyServer, 8554,NULL,"intellivision","intellivision",rtspServer->streamName);

  
  //pthread_t ftid;
  //pthread_create( &ftid, NULL, workerThread, rtspServer);
  //registerStream(sms,rtspServer->proxyServer, 8554,NULL,"intellivision","intellivision",True,rtspServer->streamName);
  env->taskScheduler().doEventLoop((char*) &RTSP_EVENT); // does not return

  return 0; // only to prevent compiler warning
}

void handleRegisterResponse(RTSPServer* rtspServer, unsigned requestId, int resultCode, char* resultString) {
  fprintf(stderr,"In handleRegisterResponse (requestId : %d, resultCode : %d, resultString : %s)\n",
                                                                     requestId,resultCode,resultString);
  if(resultCode == 0) {
    fprintf(stderr,"\nRegistration successful - hrr\n");
    //ofstream outputFile;
    //outputFile.open("/home/ec2-user/HC/ProxyServer/Registration_success");
    //outputFile << (DerivedRTSPServer*)rtspServer->proxyServer << endl;
    //outputFile.close();
  }
}

void DerivedRTSPServer::workerThreadImpl() {

  while(1) {
    if(fIsDisconnected) {
      std::cout<<"fIsDisconnected : "<<fIsDisconnected<<"\n";
      /*for(list<char const*>::iterator server = proxyServers.begin(); server != proxyServers.end(); server++)
      {*/
        while(1) {
          unsigned registeredStreamCount = fRegisterOrDeregisterRequestCounter;      
          unsigned currentCount;
          registerStream(sms,proxyServer, 8554,(responseHandlerForREGISTER*)handleRegisterResponse,
                                                 "intellivision","intellivision",True,streamName);
          if(currentCount == (registeredStreamCount+1)) {
            fprintf(stderr,"\nAdded REGISTER request to queue : %s\n",proxyServer);
            fIsDisconnected = false;
            break;
          }
          else {
            fprintf(stderr,"\n Failed to add REGISTER request to queue : Retrying...");
            sleep(1);
          }
        }
      //}
      sleep(1);
    }
  }
}

static void announceStream(RTSPServer* rtspServer, ServerMediaSession* sms,
			   char const* streamName, char const* inputFileName) {
  char* url = rtspServer->rtspURL(sms);
  UsageEnvironment& env = rtspServer->envir();
  env << "\n\"" << streamName << "\" stream, from the file \""
      << inputFileName << "\"\n";
  env << "Play this stream using the URL \"" << url << "\"\n";
  delete[] url;
}
