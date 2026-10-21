#ifndef _NETINET_IN_H
#define _NETINET_IN_H

#include <sys/socket.h>

uint16_t htons(uint16_t);
uint16_t ntohs(uint16_t);
uint32_t htonl(uint32_t);
uint32_t ntohl(uint32_t);

#endif
