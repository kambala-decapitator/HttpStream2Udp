#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/igmp.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/igmp.h>
#endif

#define IGMP_BUFFER_SIZE 64
#define BUFFER_SIZE 4096
#define UDPXY_REQUEST_FORMAT "GET /udp/%s:%u HTTP/1.0\r\n\r\n"

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

struct in_addr getInaddr4ForInterface(const char* interface, int socketFd)
{
  struct ifreq ifr = {{0}};
  ifr.ifr_addr.sa_family = AF_INET;
  strncpy(ifr.ifr_name, interface, IFNAMSIZ - 1);
  if (ioctl(socketFd, SIOCGIFADDR, &ifr) < 0)
    return (struct in_addr){0};
  return ((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr;
}

int changeMulticastMembership(in_addr_t group, struct in_addr inAddr, int socketFd, int option)
{
  struct ip_mreq mreq = {{0}};
  mreq.imr_multiaddr.s_addr = group;
  mreq.imr_interface = inAddr;
  return setsockopt(socketFd, IPPROTO_IP, option, &mreq, sizeof mreq);
}

int joinMulticastGroup(in_addr_t group, struct in_addr inAddr, int socketFd)
{
  return changeMulticastMembership(group, inAddr, socketFd, IP_ADD_MEMBERSHIP);
}

int udpSendSocket;
uint16_t streamMulticastPort = 0;
struct sockaddr_in udpSockaddr = {0};

void prepareUdpSocket(const char* streamMulticastInterface)
{
  udpSendSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udpSendSocket < 0)
  {
    // close(tcpReceiveSocket);
    exitError("cannot create UDP socket");
  }

  const uint8_t disableMulticastLoopback = 0;
  setsockopt(udpSendSocket, IPPROTO_IP, IP_MULTICAST_LOOP, &disableMulticastLoopback, sizeof disableMulticastLoopback);

  struct in_addr udpSendSocketInterface = getInaddr4ForInterface(streamMulticastInterface, udpSendSocket);
  if (udpSendSocketInterface.s_addr == 0)
  {
    // close(tcpReceiveSocket);
    close(udpSendSocket);
    exitError("selected interface not found for UDP socket");
  }
  if (setsockopt(udpSendSocket, IPPROTO_IP, IP_MULTICAST_IF, &udpSendSocketInterface, sizeof udpSendSocketInterface) < 0)
  {
    // close(tcpReceiveSocket);
    close(udpSendSocket);
    exitError("cannot set interface for UDP socket");
  }

  udpSockaddr.sin_family = AF_INET;
  udpSockaddr.sin_port = htons(streamMulticastPort);
}

int tcpReceiveSocket;
struct sockaddr_in tcpSockaddr;

void prepareTcpSocket(const char* udpxyAddress, uint16_t udpxyPort, const char* udpxyInterface)
{
  tcpReceiveSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (tcpReceiveSocket < 0)
    exitError("cannot create TCP socket");

  if (!buildSockaddr4(udpxyAddress, udpxyPort, &tcpSockaddr))
  {
    close(tcpReceiveSocket);
    exitError("cannot build address for TCP socket");
  }

  struct in_addr tcpReceiveSocketBoundInaddr = getInaddr4ForInterface(udpxyInterface, tcpReceiveSocket);
  if (tcpReceiveSocketBoundInaddr.s_addr == 0)
  {
    close(tcpReceiveSocket);
    exitError("selected interface not found for TCP socket");
  }

  struct sockaddr_in tcpReceiveSocketLocalSockaddr = {0};
  tcpReceiveSocketLocalSockaddr.sin_family = AF_INET;
  tcpReceiveSocketLocalSockaddr.sin_addr = tcpReceiveSocketBoundInaddr;
  if (bind(tcpReceiveSocket, (struct sockaddr*)&tcpReceiveSocketLocalSockaddr, sizeof tcpReceiveSocketLocalSockaddr) < 0)
  {
    close(tcpReceiveSocket);
    exitError("cannot bind TCP socket");
  }
}

ssize_t sendDataToUdp(const void* data, ssize_t size)
{
  return sendto(udpSendSocket, data, size, 0, (struct sockaddr*)&udpSockaddr, sizeof udpSockaddr);
}

void connectTcpSocket()
{
  if (connect(tcpReceiveSocket, (struct sockaddr*)&tcpSockaddr, sizeof tcpSockaddr) < 0)
  {
    close(tcpReceiveSocket);
    exitError("TCP connect failed");
  }
}

uint32_t streamMulticastGroup;

void* processUdpxyStream(void* unused)
{
  static pthread_once_t connectTcpSocketOnce = PTHREAD_ONCE_INIT;
  if (pthread_once(&connectTcpSocketOnce, connectTcpSocket) != 0)
    perror("error calling connectTcpSocket() once");

  // TODO: connect() instead of passing in sendto() ?
  udpSockaddr.sin_addr.s_addr = streamMulticastGroup;

  char multicastGroupStr[INET_ADDRSTRLEN] = {0};
  inet_ntop(AF_INET, &streamMulticastGroup, multicastGroupStr, INET_ADDRSTRLEN);

  int udpxyRequestSize = snprintf(NULL, 0, UDPXY_REQUEST_FORMAT, multicastGroupStr, streamMulticastPort) + 1;
  char udpxyRequest[udpxyRequestSize];
  snprintf(udpxyRequest, udpxyRequestSize, UDPXY_REQUEST_FORMAT, multicastGroupStr, streamMulticastPort);
  if (send(tcpReceiveSocket, udpxyRequest, strlen(udpxyRequest), 0) < 0)
  {
    close(tcpReceiveSocket);
    exitError("TCP send failed");
  }

  const char* streamPrefix = "application/octet-stream\r\n\r\n";
  bool streamFound = false;
  for (;;)
  {
    char udpxyResponse[BUFFER_SIZE];
    ssize_t responseBytes = recv(tcpReceiveSocket, udpxyResponse, BUFFER_SIZE, 0);
    if (responseBytes < 0)
    {
      perror("ERROR receiving stream from udpxy");
      continue;
    }
    if (responseBytes == 0)
      continue;

    if (streamFound)
    {
      sendDataToUdp(udpxyResponse, responseBytes);
      continue;
    }

    char* streamPrefixInResponse = strstr(udpxyResponse, streamPrefix);
    if (streamPrefixInResponse)
    {
      streamFound = true;
      size_t streamStartPos = streamPrefixInResponse - udpxyResponse + strlen(streamPrefix);
      sendDataToUdp(udpxyResponse + streamStartPos, responseBytes - streamStartPos);
    }
  }
  return NULL;
}

int main(int argc, const char** argv)
{
  int igmpSocket = socket(AF_INET, SOCK_RAW, IPPROTO_IGMP); // TODO close on failure
  if (igmpSocket < 0)
    exitError("cannot create IGMP socket");

  const char* udpxyInterface = NULL;
  const char* udpxyAddress = NULL;
  uint16_t udpxyPort = 4022;
  const char* streamMulticastInterface = NULL;

  for (int i = 1; i < argc; ++i)
  {
    if (strcmp(argv[i], "--udpxy-interface") == 0)
      udpxyInterface = argv[++i];
    else if (strcmp(argv[i], "--udpxy-address") == 0)
      udpxyAddress = argv[++i];
    else if (strcmp(argv[i], "--udpxy-port") == 0)
      udpxyPort = atoi(argv[++i]);
    else if (strcmp(argv[i], "--stream-interface") == 0)
      streamMulticastInterface = argv[++i];
    else if (strcmp(argv[i], "--stream-port") == 0)
      streamMulticastPort = atoi(argv[++i]);
  }

#ifdef __linux__
  int bindIgmpSocketToInterfaceResult = setsockopt(igmpSocket, SOL_SOCKET, SO_BINDTODEVICE, streamMulticastInterface, sizeof streamMulticastInterface);
#else
  int bindIgmpSocketToInterfaceResult = 0;
#endif
  if (bindIgmpSocketToInterfaceResult < 0)
  {
    close(igmpSocket);
    exitError("cannot bind IGMP socket to interface");
  }

  struct in_addr igmpSocketBoundInaddr = getInaddr4ForInterface(streamMulticastInterface, igmpSocket);
  if (igmpSocketBoundInaddr.s_addr == 0)
  {
    close(igmpSocket);
    exitError("IGMP socket has no address on this interface");
  }
  if (joinMulticastGroup(IGMPV3_ALL_MCR, igmpSocketBoundInaddr, igmpSocket) < 0)
  {
    close(igmpSocket);
    exitError("cannot join all multicast routers group");
  }

  prepareTcpSocket(udpxyAddress, udpxyPort, udpxyInterface);
  prepareUdpSocket(streamMulticastInterface);

  pthread_attr_t threadAttr;
  pthread_attr_init(&threadAttr);
  pthread_attr_setdetachstate(&threadAttr, PTHREAD_CREATE_DETACHED);

  uint8_t joinGroupRequestCount = 0, leaveGroupRequestCount = 0; // TODO: verify group address
  bool threadCreated = false;
  pthread_t streamingThread;
  struct pollfd pollData = { .fd = igmpSocket, .events = POLLIN };
  for (;;)
  {
    if (poll(&pollData, 1, -1) < 0)
    {
      perror("poll failed");
      continue;
    }

    char igmpBuf[IGMP_BUFFER_SIZE] = {0};
    ssize_t igmpResponseSize = recv(igmpSocket, igmpBuf, IGMP_BUFFER_SIZE, 0);
    if (igmpResponseSize < 0)
    {
      perror("cannot receive IGMP response");
      continue;
    }
    if (igmpResponseSize == 0)
      continue;

    struct ip* igmpPacketHeader = (struct ip*)igmpBuf;
    // ignore message from myself
    if (igmpPacketHeader->ip_src.s_addr == igmpSocketBoundInaddr.s_addr)
      continue;

    const unsigned int igmpHeaderStart = igmpPacketHeader->ip_hl << 2;
    struct igmp* igmpHeader = (struct igmp*)(igmpBuf + igmpHeaderStart);
    if (igmpHeader->igmp_type != IGMPV3_HOST_MEMBERSHIP_REPORT)
      continue;

    struct igmpv3_report* igmp3Report = (struct igmpv3_report*)igmpHeader;
    const unsigned int groupRecordsStart = igmpHeaderStart + sizeof(struct igmpv3_report);
    for (uint16_t i = 0, groupRecords = ntohs(igmp3Report->ngrec); i < groupRecords; ++i)
    {
      // TODO: assuming that groupRecord->grec_nsrcs == 0
      struct igmpv3_grec* groupRecord = (struct igmpv3_grec*)(igmpBuf + groupRecordsStart + i*sizeof(struct igmpv3_grec));
      switch (groupRecord->grec_type)
      {
      case IGMPV3_CHANGE_TO_INCLUDE:
        if (++leaveGroupRequestCount != 2)
          break;
        leaveGroupRequestCount = 0;

        if (threadCreated && pthread_cancel(streamingThread) != 0)
          perror("pthread_cancel failed");
        break;
      case IGMPV3_CHANGE_TO_EXCLUDE:
        if (++joinGroupRequestCount != 2)
          break;
        joinGroupRequestCount = 0;

        streamMulticastGroup = groupRecord->grec_mca;
        threadCreated = pthread_create(&streamingThread, &threadAttr, processUdpxyStream, NULL) == 0;
        if (!threadCreated)
          perror("pthread_create failed");
        break;
      }
    }
  }

  pthread_attr_destroy(&threadAttr);
  close(igmpSocket);
  close(tcpReceiveSocket);
  close(udpSendSocket);
  return 0;
}
