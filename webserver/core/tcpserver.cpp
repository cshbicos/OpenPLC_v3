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
#include <string.h>
#include <errno.h>
#include <unistd.h> 

#include <sys/types.h>  
#include <sys/socket.h> 
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "iec_std_lib.h"
#include "ladder.h"

#define MAXTCPSERVERS 3
#define SELECT_TIMEOUT 10

typedef struct{
    pthread_t thread;
    bool running = false;
} TcpServerThread;

typedef struct{
    int port;
    int serverNum; //used for PLC variables to fill
} TCPServerSetting;

TcpServerThread tcpThreads[MAXTCPSERVERS];

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
    SetSocketBlockingEnabled(socket_fd, false);
    
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


void runTCPServer(TCPServerSetting * setting)
{
    unsigned char log_msg[1000];
    int listenSocket, newSocket;
    int maxSocketDescriptor;
    struct sockaddr_in clientAddress; socklen_t clientLen;
    fd_set masterSet, workingSet;  
    int valread, rc;
    struct timeval timeout;
    
    IEC_STRING buffer;
    
    timeout.tv_sec = SELECT_TIMEOUT;
    
    listenSocket = createTCPSocket(setting->port);
    
    FD_ZERO(&masterSet);
    maxSocketDescriptor = listenSocket;
    FD_SET(listenSocket, &masterSet);
    
    while(tcpThreads[setting->serverNum].running)
    {
        memcpy(&workingSet, &masterSet, sizeof(masterSet));
        
        rc = select( maxSocketDescriptor + 1 , &workingSet , NULL , NULL , &timeout);
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
        for (int i=0; i<=maxSocketDescriptor &&  descReady > 0; ++i)
        {
            if(!FD_ISSET(i, &workingSet))
                continue;
            
            //a readable descriptor was found, one less to look for :)
            descReady -= 1;
            if(i == listenSocket){
                //new connection!
                do{
                    if ((newSocket = accept(listenSocket,  
                    (struct sockaddr *)&clientAddress, &clientLen))<0)   
                    {   
                        sprintf(log_msg, "TCP Server %d: New Client Error\n", setting->serverNum);
                        log(log_msg); 
                        break;
                    } 
                    FD_SET(newSocket, &masterSet);
                    if (newSocket > maxSocketDescriptor)
                        maxSocketDescriptor = newSocket;
                }while(newSocket != -1);  
            }else{
                //if the select didn't trigger because of a new connection
                bool closeConnection = false;
                do{
                    //try to read all data
                    rc = recv(i, buffer, sizeof(buffer), 0);
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
                    
                    //this is where we now write to the PLC our newly obtained information
                    
                }while(true);
                if(closeConnection == true)
                {
                    close(i);
                    FD_CLR(i, &masterSet);
                    if (i == maxSocketDescriptor)
                    {
                        while (FD_ISSET(maxSocketDescriptor, &masterSet) == false)
                            maxSocketDescriptor -= 1;
                    }
                }
            }
        }
        
    }
    
    for (int i=0; i<=maxSocketDescriptor;++i){
        if(FD_ISSET(i, &masterSet))
            close(i);
    }
    
    close(listenSocket);
    sprintf(log_msg, "Terminating TCP Server thread %d\n", setting->serverNum);
    log(log_msg);
} 



void *startThread( void *settings){
    runTCPServer( (TCPServerSetting *) settings );
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
    
    TCPServerSetting setting = { port, server };
    tcpThreads[server].running = true;
    pthread_create(&(tcpThreads[server].thread), NULL, startThread, &setting);
    
}

void stopTCPServer(int server){
    
}

void stopTCPAllServer(){
    
    
}

