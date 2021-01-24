#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STREAM_ADDRESS "239.255.0.4"
#define BUFFER_SIZE 4096

void exitError(const char* msg)
{
  perror(msg);
  exit(EXIT_FAILURE);
}

bool buildSockaddr4(const char* address, uint16_t port, struct sockaddr_in* pSockaddr)
{
  if (!pSockaddr)
    return false;

  memset(pSockaddr, 0, sizeof *pSockaddr);
  pSockaddr->sin_family = AF_INET;
  pSockaddr->sin_port = htons(port);
  return inet_pton(AF_INET, address, &pSockaddr->sin_addr) != -1;
}

int main(void)
{
  int tcpReceiveSocket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (tcpReceiveSocket == -1)
    exitError("cannot create TCP socket");

  struct sockaddr_in tcpSockaddr;
  if (!buildSockaddr4("localhost", 4022, &tcpSockaddr))
  {
    close(tcpReceiveSocket);
    exitError("cannot build address for TCP socket");
  }

  if (connect(tcpReceiveSocket, (struct sockaddr*)&tcpSockaddr, sizeof tcpSockaddr) == -1)
  {
    close(tcpReceiveSocket);
    exitError("TCP connect failed");
  }

  const char* udpxyRequest = "GET /udp/" STREAM_ADDRESS ":5500 HTTP/1.0\r\n\r\n";
  if (send(tcpReceiveSocket, udpxyRequest, strlen(udpxyRequest), 0) == -1)
  {
    close(tcpReceiveSocket);
    exitError("TCP send failed");
  }

  int udpSendSocket = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udpSendSocket == -1)
  {
    close(tcpReceiveSocket);
    exitError("cannot create UDP socket");
  }

  struct sockaddr_in udpSockaddr;
  if (!buildSockaddr4("localhost", 12345, &udpSockaddr))
  {
    close(tcpReceiveSocket);
    close(udpSendSocket);
    exitError("cannot build address for UDP socket");
  }

  char udpxyResponse[BUFFER_SIZE];
  const char* streamPrefix = "application/octet-stream\r\n\r\n";
  bool streamFound = false;
  for (;;)
  {
    memset(udpxyResponse, 0, BUFFER_SIZE);
    ssize_t responseBytes = recv(tcpReceiveSocket, udpxyResponse, BUFFER_SIZE, 0);
    if (responseBytes < 0)
    {
      perror("ERROR receiving stream from udpxy");
      continue;
    }
    if (responseBytes == 0)
      break;

    if (streamFound)
    {
      sendto(udpSendSocket, udpxyResponse, responseBytes, 0, (struct sockaddr*)&udpSockaddr, sizeof udpSockaddr);
      continue;
    }

    char* streamPrefixInResponse = strstr(udpxyResponse, streamPrefix);
    if (streamPrefixInResponse)
    {
      streamFound = true;
      int streamStartPos = streamPrefixInResponse - udpxyResponse + strlen(streamPrefix);
      sendto(udpSendSocket, udpxyResponse + streamStartPos, responseBytes - streamStartPos, 0, (struct sockaddr*)&udpSockaddr, sizeof udpSockaddr);
    }
  }

  close(tcpReceiveSocket);
  close(udpSendSocket);
  return 0;
}
