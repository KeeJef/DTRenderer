#ifndef DTRENDERER_DEBUG_H
#define DTRENDERER_DEBUG_H

#include "dqn.h"
#define DTR_DEBUG 1
#ifdef DTR_DEBUG
	#define DTR_DEBUG_RENDER             0

	#define DTR_DEBUG_PROFILING          1
	#ifdef DTR_DEBUG_PROFILING
		#define BUILD_WITH_EASY_PROFILER 1
		#include "external/easy/profiler.h"

		#define DTR_DEBUG_PROFILE_START() profiler::startListen()
		#define DTR_DEBUG_PROFILE_END()   profiler::stopListen()

		#define DTR_DEBUG_TIMED_BLOCK(name)           EASY_BLOCK(name)
		#define DTR_DEBUG_TIMED_NONSCOPED_BLOCK(name) EASY_NONSCOPED_BLOCK(name)
		#define DTR_DEBUG_TIMED_END_BLOCK()           EASY_END_BLOCK()
		#define DTR_DEBUG_TIMED_FUNCTION()            EASY_FUNCTION()
	#else
		#define DTR_DEBUG_PROFILE_START()
		#define DTR_DEBUG_PROFILE_END()

		#define DTR_DEBUG_TIMED_BLOCK(name)
		#define DTR_DEBUG_TIMED_NONSCOPED_BLOCK(name)
		#define DTR_DEBUG_TIMED_END_BLOCK()
		#define DTR_DEBUG_TIMED_FUNCTION()
	#endif

#endif

typedef struct PlatformRenderBuffer PlatformRenderBuffer;
typedef struct DTRFont              DTRFont;
typedef struct DTRState             DTRState;
typedef struct PlatformInput        PlatformInput;
typedef struct PlatformMemory       PlatformMemory;

typedef struct DTRDebug
{
	DTRFont              *font;
	PlatformRenderBuffer *renderBuffer;

	DqnV4 displayColor;
	DqnV2 displayP;
	i32   displayYOffset;

	u64 setPixelsPerFrame;
	u64 totalSetPixels;
} DTRDebug;

extern DTRDebug globalDebug;

void DTRDebug_PushText(const char *const formatStr, ...);
void DTRDebug_Update(DTRState *const state,
                     PlatformRenderBuffer *const renderBuffer,
                     PlatformInput *const input, PlatformMemory *const memory);
#endif
