#ifndef SDL_NET_ADAPTER_H
#define SDL_NET_ADAPTER_H

#include <stdbool.h>

#include "gekkonet.h"

#ifdef __cplusplus
extern "C" {
#endif

struct NET_DatagramSocket;

GekkoNetAdapter* SDLNetAdapter_Create(struct NET_DatagramSocket* sock);
void SDLNetAdapter_Destroy();

#ifdef __cplusplus
}
#endif

#endif
