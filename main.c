#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

int main(int argc, const char** argv)
{
  const char* udpxyAddress = NULL;
  uint16_t udpxyPort = 4022;
  const char* streamMulticastGroup = NULL;
  uint16_t streamMulticastPort = 0;
  for (int i = 1; i < argc; ++i)
  {
    if (strcmp(argv[i], "--udpxy-address") == 0)
      udpxyAddress = argv[++i];
    else if (strcmp(argv[i], "--udpxy-port") == 0)
      udpxyPort = atoi(argv[++i]);
    else if (strcmp(argv[i], "--stream-mgroup") == 0)
      streamMulticastGroup = argv[++i];
    else if (strcmp(argv[i], "--stream-port") == 0)
      streamMulticastPort = atoi(argv[++i]);
  }

  int tcpReceiveSocket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (tcpReceiveSocket == -1)
    exitError("cannot create TCP socket");

  struct sockaddr_in tcpSockaddr;
  if (!buildSockaddr4(udpxyAddress, udpxyPort, &tcpSockaddr))
  {
    close(tcpReceiveSocket);
    exitError("cannot build address for TCP socket");
  }

  if (connect(tcpReceiveSocket, (struct sockaddr*)&tcpSockaddr, sizeof tcpSockaddr) == -1)
  {
    close(tcpReceiveSocket);
    exitError("TCP connect failed");
  }

  const char* udpxyFormat = "GET /udp/%s:%u HTTP/1.0\r\n\r\n";
  int udpxyRequestSize = snprintf(NULL, 0, udpxyFormat, streamMulticastGroup, streamMulticastPort) + 1;
  char udpxyRequest[udpxyRequestSize];
  snprintf(udpxyRequest, udpxyRequestSize, udpxyFormat, streamMulticastGroup, streamMulticastPort);
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
  if (!buildSockaddr4(streamMulticastGroup, streamMulticastPort, &udpSockaddr))
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
