#include <cstddef> // NULL
#include <arpa/inet.h> // struct sockaddr_in & inet_ntoa & ntohs

#include "tools.h"

#include <assert.h> // assert
#include <stdio.h>

unsigned long hash_ipaddr(struct sockaddr_in* addr)
{
	unsigned long res;

	assert( addr != NULL );

	//printf("PORT: %u\n", addr->sin_port);

	res = (((addr->sin_addr.s_addr >> 24) & 0xff) * 256)	+
			(((addr->sin_addr.s_addr >> 16) & 0xff) * 256)	+
			(((addr->sin_addr.s_addr >>  8) & 0xff)* 256)	+
			(addr->sin_addr.s_addr & 0xff) + addr->sin_port;

	return res;
}
