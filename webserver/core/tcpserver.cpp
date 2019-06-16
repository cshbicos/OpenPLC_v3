/*
 * This code allows the OpenPLC to set up a number of TCP Servers that 
 * communicate with random clients.
 * 
 * Copyright (C) 2019  Christoph Herrmann <csh.mrman@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <errno.h>
#include <unistd.h> 

#include <sys/types.h>  
#include <sys/socket.h> 
#include <sys/ioctl.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>


#include "ladder.h"

#define MAXTCPSERVERS 3
#define SELECT_TIMEOUT 10
#define TCPSERVER_START 1000

typedef struct{
    pthread_t listenThread;
    pthread_t writeThread;
    
    sem_t semRead;
    sem_t semReadEmpty;
    IEC_STRING currentReadValue;
    char currentLineCache[STR_MAX_LEN];
    int curLinePos;
    
    sem_t semWrite;
    sem_t semWriteEmpty;
    IEC_STRING currentWriteValue;
    
    bool running = false;
    pthread_mutex_t socketMutex;
    int maxSocketDescriptor;
    fd_set sockets;
    int listenerSocket;
} TcpServerThread;

typedef struct{
    int port;
    int serverNum; //used for PLC variables to fill
} TCPServerSetting;

TcpServerThread tcpThreads[MAXTCPSERVERS];

void sendRcvTCP()
{   
    for(int i=0;i<MAXTCPSERVERS;i++)
    {   
        if(tcpThreads[i].running == false)
            //the TCP server is not running
            continue;
        
        if(*(bool_memory[TCPSERVER_START + (i*2)][0]) != 0) //MX1000.0 register == PLC read ready
        {
            //receive ready from PLC side
            if(sem_trywait(&(tcpThreads[i].semRead)) == 0)
            {
                //there was also something to be read here
                
                //copy the new values into our registers...
                memcpy( string_memory[TCPSERVER_START + (i*2)], &(tcpThreads[i].currentReadValue), sizeof(IEC_STRING));
                *(bool_memory[TCPSERVER_START + (i*2)][0]) = 0; //MX1000.0 reset the PLC receive ready value
                *(bool_memory[TCPSERVER_START + (i*2)][1]) = 1; //MX1000.1 set the receive result ready value so the PLC knows something is here

                //the next item can be read
                sem_post(&(tcpThreads[i].semReadEmpty));
            }else{
                *(bool_memory[TCPSERVER_START + (i*2)][1]) = 0; //MX1000.1 unset the receive result ready value so the PLC knows nothing new was set
            }
        }
        
        if(*(bool_memory[TCPSERVER_START + (i*2)][2]) != 0) //MX1000.2 register == PLC write ready
        {
            //send ready from PLC side
            if(sem_trywait(&(tcpThreads[i].semWriteEmpty)) == 0)
            {
                //the sending buffer is ready to be filled with some more stuff...
                
                //copy the new values into our registers...
                memcpy( &(tcpThreads[i].currentWriteValue), string_memory[TCPSERVER_START + (i*2) + 1], sizeof(IEC_STRING));
                *(bool_memory[TCPSERVER_START + (i*2)][2]) = 0; //MX1000.2 reset the write ready value so the PLC can set more data to write
                
                //the next item can be written out to the world in the other thread
                sem_post(&(tcpThreads[i].semWrite));
            }
        }
    }
}




void handleTCPRcvMessage( int server, char * buffer, int readSize)
{   
    for(int curPos=0;curPos<readSize;curPos++){
        if(buffer[curPos] == '\r')
        {
            //never copy \r anywhere!
            continue;
        }else if(buffer[curPos] != '\n' && tcpThreads[server].curLinePos < STR_MAX_LEN)
        {
            //copy value unless we are end of line OR string is full...
            tcpThreads[server].currentLineCache[tcpThreads[server].curLinePos++] = buffer[curPos];
            continue;
        } 
        
        //either we got a '\n' or the telegram cache is full...
        if(sem_wait(&(tcpThreads[server].semReadEmpty)) == 0)
        {
            //we have an empty spot to read into - let's do it
            memcpy(&(tcpThreads[server].currentReadValue.body), tcpThreads[server].currentLineCache, tcpThreads[server].curLinePos );
            tcpThreads[server].currentReadValue.len = tcpThreads[server].curLinePos;
            tcpThreads[server].curLinePos = 0;
            
            sem_post(&(tcpThreads[server].semRead));
        }
    }
}

int createTCPSocket(int port)
{
    unsigned char log_msg[1000];
    int socket_fd;
    struct sockaddr_in server_addr;

    //Create TCP Socket
    socket_fd = socket(AF_INET,SOCK_STREAM,0);
    if (socket_fd<0)
    {
        sprintf(log_msg, "TCP Server: error creating stream socket => %s\n", strerror(errno));
        log(log_msg);
        return -1;
    }
    
    //Set SO_REUSEADDR
    int enable = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
    {
        perror("setsockopt(SO_REUSEADDR) failed");
        return -1;   
    }
    if( ioctl(socket_fd, FIONBIO, (char *)&enable) < 0)
    {
        perror("ioctrl failed");
        return -1;   
    }
    
    //Initialize Server Struct
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    //Bind socket
    if (bind(socket_fd,(struct sockaddr *)&server_addr,sizeof(server_addr)) < 0)
    {
        sprintf(log_msg, "TCP Server: error binding socket => %s\n", strerror(errno));
        log(log_msg);
        return -1;
    }
    
    // we accept max 2 pending connections
    listen(socket_fd, 2);
    sprintf(log_msg, "TCP Server: Listening on port %d\n", port);
    log(log_msg);

    return socket_fd;
}


void *runTCPListenThread( void *settingParam)
{
    TCPServerSetting * setting = (TCPServerSetting *) settingParam;

    unsigned char log_msg[1000];
    int newSocket;
    struct sockaddr_in clientAddress; socklen_t clientLen;
    fd_set workingSet;  
    int valread, rc;
    struct timeval timeout;
    clientLen = sizeof(clientAddress);
    
    
    char buffer[STR_MAX_LEN];
    
    timeout.tv_sec = SELECT_TIMEOUT;
    
    tcpThreads[setting->serverNum].listenerSocket = createTCPSocket(setting->port);
        
    pthread_mutex_lock(&(tcpThreads[setting->serverNum].socketMutex));
    tcpThreads[setting->serverNum].maxSocketDescriptor = tcpThreads[setting->serverNum].listenerSocket;
    FD_SET(tcpThreads[setting->serverNum].listenerSocket, &(tcpThreads[setting->serverNum].sockets));
    pthread_mutex_unlock(&(tcpThreads[setting->serverNum].socketMutex));
    
    while(tcpThreads[setting->serverNum].running)
    {
        memcpy(&workingSet, &(tcpThreads[setting->serverNum].sockets), sizeof(tcpThreads[setting->serverNum].sockets));
        
        rc = select( tcpThreads[setting->serverNum].maxSocketDescriptor + 1 , &workingSet , NULL , NULL , &timeout);

        if(rc < 0)   
        {   
            sprintf(log_msg, "TCP Server %d: Select() Error\n", setting->serverNum);
            log(log_msg); 
            continue;
        }
        
        if(rc == 0)
        {
            //timeout - run again
            continue;
        }
        
        int descReady = rc;
        for (int i=0; i<=tcpThreads[setting->serverNum].maxSocketDescriptor &&  descReady > 0; ++i)
        {
            if(!FD_ISSET(i, &workingSet))
                continue;
            
            //a readable descriptor was found, one less to look for :)
            descReady -= 1;
            if(i == tcpThreads[setting->serverNum].listenerSocket){
                //new connection!
                do{
                    if ((newSocket = accept(tcpThreads[setting->serverNum].listenerSocket, (struct sockaddr *)&clientAddress, &clientLen))<0)   
                    {   
                        if(errno != EWOULDBLOCK){
                            sprintf(log_msg, "TCP Server %d: New Client Error %d\n", setting->serverNum, errno);
                            log(log_msg);  
                        }
                        break;
                    } 
                    
                    sprintf(log_msg, "TCP Server %d: New Client connected from %s\n", setting->serverNum, inet_ntoa(clientAddress.sin_addr));
                    log(log_msg);  
                    pthread_mutex_lock(&(tcpThreads[setting->serverNum].socketMutex));
                    FD_SET(newSocket, &(tcpThreads[setting->serverNum].sockets));
                    if (newSocket > tcpThreads[setting->serverNum].maxSocketDescriptor)
                        tcpThreads[setting->serverNum].maxSocketDescriptor = newSocket;
                    pthread_mutex_unlock(&(tcpThreads[setting->serverNum].socketMutex));
                }while(newSocket != -1);  
            }else{
                //if the select didn't trigger because of a new connection
                bool closeConnection = false;
                do{
                    //try to read all data
                    rc = recv(i, &buffer, sizeof(buffer), 0);
                    if(rc <= 0)
                    {
                        if (errno != EWOULDBLOCK)
                        {
                            getpeername(i , (struct sockaddr*)&clientAddress, &clientLen);   
                            sprintf(log_msg, "TCP Server %d: Connection from %s disconnected", setting->serverNum, inet_ntoa(clientAddress.sin_addr));
                            log(log_msg); 
                            closeConnection = true;
                        }
                        break;
                    }
                    
                    //this is where we now write to the buffers so the PLC can receive our message
                    handleTCPRcvMessage(setting->serverNum, buffer, rc);
                    
                }while(true);
                if(closeConnection == true)
                {
                    close(i);
                    pthread_mutex_lock(&(tcpThreads[setting->serverNum].socketMutex));
                    FD_CLR(i, &(tcpThreads[setting->serverNum].sockets));
                    if (i == tcpThreads[setting->serverNum].maxSocketDescriptor)
                    {   
                        while (FD_ISSET(tcpThreads[setting->serverNum].maxSocketDescriptor, &(tcpThreads[setting->serverNum].sockets)) == false)
                            tcpThreads[setting->serverNum].maxSocketDescriptor -= 1;
                    }
                    pthread_mutex_unlock(&(tcpThreads[setting->serverNum].socketMutex));
                }
            }
        }
        
    }
    
    pthread_mutex_lock(&(tcpThreads[setting->serverNum].socketMutex));
    for (int i=0; i<=tcpThreads[setting->serverNum].maxSocketDescriptor;++i){
        if(FD_ISSET(i, &(tcpThreads[setting->serverNum].sockets)))
            close(i);
    }
    pthread_mutex_unlock(&(tcpThreads[setting->serverNum].socketMutex));
    
    close(tcpThreads[setting->serverNum].listenerSocket);
    sprintf(log_msg, "Terminating TCP Server thread %d\n", setting->serverNum);
    log(log_msg);
} 

void *runTCPWriteThread( void *settingsParam){
    TCPServerSetting * setting = (TCPServerSetting *) settingsParam;
    unsigned char log_msg[1000];
    fd_set workingSet;  
    struct timespec ts;
    
    
    while(tcpThreads[setting->serverNum].running)
    {
        
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += SELECT_TIMEOUT;
        //wait until we got something to write...
        if(sem_timedwait(&(tcpThreads[setting->serverNum].semWrite), &ts) == 0)
        {   
            //get all open sockets to write to...
            pthread_mutex_lock(&(tcpThreads[setting->serverNum].socketMutex));
            memcpy(&workingSet, &(tcpThreads[setting->serverNum].sockets), sizeof(tcpThreads[setting->serverNum].sockets));
            int maxDesc = tcpThreads[setting->serverNum].maxSocketDescriptor;
            pthread_mutex_unlock(&(tcpThreads[setting->serverNum].socketMutex));
               
            
            for (int i=0; i<=maxDesc; ++i)
            {
                if(!FD_ISSET(i, &workingSet))
                    continue;
                if(i == tcpThreads[setting->serverNum].listenerSocket)
                    continue;
                
                int rc = send(i, &(tcpThreads[setting->serverNum].currentWriteValue.body), tcpThreads[setting->serverNum].currentWriteValue.len, MSG_NOSIGNAL);
                if(rc < 0 && errno == EPIPE)
                {   
                    sprintf(log_msg, "TCP Server %d: Connection from client lost during send\n", setting->serverNum);
                    log(log_msg); 
                    close(i);
                    pthread_mutex_lock(&(tcpThreads[setting->serverNum].socketMutex));
                    FD_CLR(i, &(tcpThreads[setting->serverNum].sockets));
                    if (i == tcpThreads[setting->serverNum].maxSocketDescriptor)
                    {   
                        while (FD_ISSET(tcpThreads[setting->serverNum].maxSocketDescriptor, &(tcpThreads[setting->serverNum].sockets)) == false)
                            tcpThreads[setting->serverNum].maxSocketDescriptor -= 1;
                    }
                    pthread_mutex_unlock(&(tcpThreads[setting->serverNum].socketMutex));
                }
                
            }
            
            sem_post(&(tcpThreads[setting->serverNum].semWriteEmpty));
        }
    }
}



void startTCPServer(int server, int port){
    unsigned char log_msg[1000];
    
    if(server < 0 || server > (MAXTCPSERVERS-1)){
        sprintf(log_msg, "TCP Server %d cannot be created\n", server);
        log(log_msg);
        return;
    }
    
    if(tcpThreads[server].running == true){
        sprintf(log_msg, "TCP Server %d already running\n", server);
        log(log_msg);
        return;
    }
    
    if(port < 1024){
        sprintf(log_msg, "TCP Server %d needs a valid port. Port %d is not valid (<1024)\n", server, port);
        log(log_msg);
        return;
    }
    
    sprintf(log_msg, "TCP Server %d should be started on port %d\n", server,port);
    log(log_msg);
    
    TCPServerSetting setting = { port, server };
    
    //intitalize the tcp server 
    FD_ZERO(&(tcpThreads[server].sockets));
    tcpThreads[server].maxSocketDescriptor = 0;
    tcpThreads[server].running = true;
    tcpThreads[server].curLinePos = 0;
    pthread_mutex_init(&(tcpThreads[server].socketMutex),NULL);
    sem_init(&(tcpThreads[server].semRead), 0, 0);
    sem_init(&(tcpThreads[server].semReadEmpty), 0, 1);
    sem_init(&(tcpThreads[server].semWrite), 0, 0);
    sem_init(&(tcpThreads[server].semWriteEmpty), 0, 1);
    
    pthread_create(&(tcpThreads[server].listenThread), NULL, runTCPListenThread, &setting);
    pthread_create(&(tcpThreads[server].writeThread), NULL,  runTCPWriteThread, &setting);
}

void stopTCPServer(int server){
    unsigned char log_msg[1000];
    
    if(server < 0 || server > (MAXTCPSERVERS-1)){
        sprintf(log_msg, "TCP Server %d does not exist\n", server);
        log(log_msg);
        return;
    }
    
    if(tcpThreads[server].running != true){
        sprintf(log_msg, "TCP Server %d is not running\n", server);
        log(log_msg);
        return; 
    }

    tcpThreads[server].running = false;
    pthread_join(tcpThreads[server].listenThread, NULL);
    pthread_join(tcpThreads[server].writeThread, NULL);

    FD_ZERO(&(tcpThreads[server].sockets));
    tcpThreads[server].maxSocketDescriptor = 0;
    tcpThreads[server].curLinePos = 0;
    pthread_mutex_destroy(&(tcpThreads[server].socketMutex));
    sem_destroy(&(tcpThreads[server].semRead));
    sem_destroy(&(tcpThreads[server].semReadEmpty));
    sem_destroy(&(tcpThreads[server].semWrite));
    sem_destroy(&(tcpThreads[server].semWriteEmpty));
    
    sprintf(log_msg, "TCP Server %d was stopped\n", server);
    log(log_msg);
}

void stopTCPAllServer(){
    
    for(int i=0;i<MAXTCPSERVERS;i++)
        if(tcpThreads[i].running == true)
            stopTCPServer(i);
    
}

