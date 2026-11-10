#ifndef PSX_SOURCE_FIXTURE_ASSERT_H
#define PSX_SOURCE_FIXTURE_ASSERT_H
#include <stdio.h>
#include <stdlib.h>
#undef assert
#define assert(x) ((x)?(void)0:psx_source_fixture_fail(#x,__LINE__))
#ifdef __cplusplus
[[noreturn]]
#else
_Noreturn
#endif
static void psx_source_fixture_fail(const char *text,int line){fprintf(stderr,"FAIL source fixture line %d: %s\n",line,text);exit(1);}
#undef abort
#define abort() psx_source_fixture_fail("unexpected boundary",__LINE__)
#endif
