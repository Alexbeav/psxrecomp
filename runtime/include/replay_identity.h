#pragma once
#include "input_route_v3_file.h"
#ifdef __cplusplus
extern "C" {
#endif
void replay_identity_set_product(const char *,const char *,const char *);
void replay_identity_set_disc(const char *,const char *);
int replay_identity_prefetch_disc_digest(void);
int replay_identity_identity(InputRouteV3 *,int,int,char *,size_t);
#ifdef __cplusplus
}
#endif
