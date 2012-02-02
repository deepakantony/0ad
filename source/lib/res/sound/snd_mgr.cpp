/* Copyright (c) 2010 Wildfire Games
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * OpenAL sound engine. handles sound I/O, buffer suballocation and
 * voice management/prioritization.
 */

#include "precompiled.h"
#include "snd_mgr.h"

#include <sstream>	// to extract snd_open's definition file contents
#include <string>
#include <vector>
#include <algorithm>
#include <deque>
#include <cmath>
#include <cstdio>
#include <cfloat>

#include "lib/alignment.h"
#include "lib/res/h_mgr.h"
#include "lib/file/vfs/vfs.h"


// for DLL-load hack in alc_init
#if OS_WIN
# include "lib/sysdep/os/win/win.h"
#endif

#include "lib/timer.h"
#include "lib/app_hooks.h"
#include "lib/sysdep/cpu.h"	// cpu_CAS

#if CONFIG2_AUDIO

#include "ogg.h"
#include "lib/external_libraries/openal.h"


static const size_t maxBufferSize = 64*KiB;

// components:
// - alc*: OpenAL context
//   readies OpenAL for use; allows specifying the device.
// - al_listener*: OpenAL listener
//   owns position/orientation and master gain.
// - al_buf*: OpenAL buffer suballocator
//   for convenience; also makes sure all have been freed at exit.
// - al_src*: OpenAL source suballocator
//   avoids high source alloc cost. also enforces user-set source limit.
// - al_init*: OpenAL startup mechanism
//   allows deferred init (speeding up start time) and runtime reset.
// - snd_dev*: device enumeration
//   lists names of all available devices (for sound options screen).
// - hsd_list*: list of SndData instances
//   ensures all are freed when desired (despite being cached).
// - snd_data*: sound data provider
//   holds audio data (clip or stream) and returns OpenAL buffers on request.
// - list*: list of active sounds.
//   sorts by priority for voice management, and has each VSrc update itself.
// - vsrc*: audio source
//   owns source properties and queue, references SndData.
// - vm*: voice management
//   grants the currently most 'important' sounds a hardware voice.


// indicates OpenAL is ready for use. checked by other components
// when deciding if they can pass settings changes to OpenAL directly,
// or whether they need to be saved until init.
static bool al_initialized = false;


// used by snd_dev_set to reset OpenAL after device has been changed.
static Status al_reinit();

// used by al_shutdown to free all VSrc and SndData objects, respectively,
// so that they release their OpenAL sources and buffers.
static Status list_free_all();
static void hsd_list_free_all();


static void al_ReportError(ALenum err, const char* caller, int line)
{
	ENSURE(al_initialized);

	debug_printf(L"OpenAL error: %hs; called from %hs (line %d)\n", alGetString(err), caller, line);
	DEBUG_WARN_ERR(ERR::LOGIC);
}

/**
 * check if OpenAL indicates an error has occurred. it can only report one
 * error at a time, so this is called before and after every OpenAL request.
 *
 * @param caller Name of calling function (typically passed via __func__)
 * @param line line number of the call site (typically passed via __LINE__)
 * (identifies the exact call site since there may be several per caller)
 */
static void al_check(const char* caller, int line)
{
	ALenum err = alGetError();
	if(err != AL_NO_ERROR)
		al_ReportError(err, caller, line);
}

// convenience version that automatically passes in function name.
#define AL_CHECK al_check(__func__, __LINE__)


//-----------------------------------------------------------------------------
// OpenAL context: readies OpenAL for use; allows specifying the device,
// in case there are problems with OpenAL's default choice.
//-----------------------------------------------------------------------------

// default (0): use OpenAL default device.
// note: that's why this needs to be a pointer instead of an array.
static const char* alc_dev_name = 0;


/**
 * tell OpenAL to use the specified device in future.
 *
 * @param alc_new_dev_name Device name.
 * @return Status
 *
 * name = 0 reverts to OpenAL's default choice, which will also
 * be used if this routine is never called.
 *
 * the device name is typically taken from a config file at init-time;
 * the snd_dev * enumeration routines below are used to present a list
 * of choices to the user in the options screen.
 *
 * if OpenAL hasn't yet been initialized (i.e. no sounds have been opened),
 *   this just stores the device name for use when init does occur.
 *   note: we can't check now if it's invalid (if so, init will fail).
 * otherwise, we shut OpenAL down (thereby stopping all sounds) and
 * re-initialize with the new device. that's fairly time-consuming,
 * so preferably call this routine before sounds are loaded.
 */
Status snd_dev_set(const char* alc_new_dev_name)
{
	// requesting a specific device
	if(alc_new_dev_name)
	{
		// already using that device - done. (don't re-init)
		if(alc_dev_name && !strcmp(alc_dev_name, alc_new_dev_name))
			return INFO::OK;

		// store name (need to copy it, since alc_init is called later,
		// and it must then still be valid)
		static char buf[32];
		strcpy_s(buf, sizeof(buf), alc_new_dev_name);
		alc_dev_name = buf;
	}
	// requesting default device
	else
	{
		// already using default device - done. (don't re-init)
		if(alc_dev_name == 0)
			return INFO::OK;

		alc_dev_name = 0;
	}

	// no-op if not initialized yet, otherwise re-init.
	return al_reinit();
}


static ALCcontext* alc_ctx = 0;
static ALCdevice* alc_dev = 0;


/**
 * free the OpenAL context and device.
 */
static void alc_shutdown()
{
	if(alc_ctx)
	{
		alcMakeContextCurrent(0);
		alcDestroyContext(alc_ctx);
	}

	if(alc_dev)
		alcCloseDevice(alc_dev);
}


/**
 * Ready OpenAL for use by setting up a device and context.
 *
 * @return Status
 */
static Status alc_init()
{
	Status ret = INFO::OK;

	alc_dev = alcOpenDevice((alcString)alc_dev_name);
	if(alc_dev)
	{
		alc_ctx = alcCreateContext(alc_dev, 0);	// no attrlist needed
		if(alc_ctx)
			alcMakeContextCurrent(alc_ctx);
	}

	// check if init succeeded.
	// some OpenAL implementations don't indicate failure here correctly;
	// we need to check if the device and context pointers are actually valid.
	ALCenum err = alcGetError(alc_dev);
	if(err != ALC_NO_ERROR || !alc_dev || !alc_ctx)
	{
		debug_printf(L"alc_init failed. alc_dev=%p alc_ctx=%p alc_dev_name=%hs err=%d\n", alc_dev, alc_ctx, alc_dev_name, err);
// FIXME Hack to get around exclusive access to the sound device
#if OS_UNIX
		ret = INFO::OK;
#else
		ret = ERR::FAIL;
#endif
	}

	// make note of which sound device is actually being used
	// (e.g. DS3D, native, MMSYSTEM) - needed when reporting OpenAL bugs.
	const char* dev_name = (const char*)alcGetString(alc_dev, ALC_DEVICE_SPECIFIER);
	wchar_t buf[200];
	swprintf_s(buf, ARRAY_SIZE(buf), L"SND| alc_init: success, using %hs\n", dev_name);
	ah_log(buf);

	return ret;
}


//-----------------------------------------------------------------------------
// listener: owns position/orientation and master gain.
// if they're set before al_initialized, we pass the saved values to
// OpenAL immediately after init (instead of waiting until next update).
//-----------------------------------------------------------------------------

static float al_listener_gain = 1.0f;
static float al_listener_position[3] = { 0.0f, 0.0f, 0.0f };
static float al_listener_velocity[3] = { 0.0f, 0.0f, 0.0f };

// float view_direction[3], up_vector[3]; passed directly to OpenAL
static float al_listener_orientation[6] = { 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f };


/**
 * send the current listener properties to OpenAL.
 *
 * also called from al_init.
 */
static void al_listener_latch()
{
	if(al_initialized)
	{
		AL_CHECK;

		alListenerf(AL_GAIN, al_listener_gain);
		alListenerfv(AL_POSITION, al_listener_position);
		alListenerfv(AL_VELOCITY, al_listener_velocity);
		alListenerfv(AL_ORIENTATION, al_listener_orientation);

		AL_CHECK;
	}
}


/**
 * set amplitude modifier, which is effectively applied to all sounds.
 * in layman's terms, this is the global "volume".
 *
 * @param gain Modifier: must be non-negative;
 * 1 -> unattenuated, 0.5 -> -6 dB, 0 -> silence.
 * @return Status
 */
Status snd_set_master_gain(float gain)
{
	if(gain < 0)
		WARN_RETURN(ERR::INVALID_PARAM);

	al_listener_gain = gain;

	// position will get sent too.
	// this isn't called often, so we don't care.
	al_listener_latch();

	return INFO::OK;
}


/**
 * set position of the listener (corresponds to camera in graphics).
 * coordinates are in world space; the system doesn't matter.
 *
 * @param pos position support vector
 * @param dir view direction
 * @param up up vector
 */
static void al_listener_set_pos(const float pos[3], const float dir[3], const float up[3])
{
	int i;
	for(i = 0; i < 3; i++)
		al_listener_position[i] = pos[i];
	for(i = 0; i < 3; i++)
		al_listener_orientation[i] = dir[i];
	for(i = 0; i < 3; i++)
		al_listener_orientation[3+i] = up[i];

	al_listener_latch();
}


/**
 * get distance between listener and point.
 * this is used to determine sound priority.
 *
 * @param point position support vector
 * @return float euclidean distance squared
 */
static float al_listener_dist_2(const float point[3])
{
	const float dx = al_listener_position[0] - point[0];
	const float dy = al_listener_position[1] - point[1];
	const float dz = al_listener_position[2] - point[2];
	return dx*dx + dy*dy + dz*dz;
}


//-----------------------------------------------------------------------------
// AL buffer suballocator: allocates buffers as needed (alGenBuffers is fast).
// this interface is a bit more convenient than the OpenAL routines, and we
// verify that all buffers have been freed at exit.
//-----------------------------------------------------------------------------

static int al_bufs_outstanding;


/**
 * allocate a new buffer, and fill it with the specified data.
 *
 * @param data raw sound data buffer
 * @param size size of buffer in bytes
 * @param al_fmt AL_FORMAT_ * describing the sound data
 * @param al_freq sampling frequency (typically 22050 Hz)
 * @return ALuint buffer name
 */
static ALuint al_buf_alloc(ALvoid* data, ALsizei size, ALenum fmt, ALsizei freq)
{
	AL_CHECK;
	ALuint al_buf;
	alGenBuffers(1, &al_buf);
	alBufferData(al_buf, fmt, data, size, freq);
	AL_CHECK;

	ENSURE(alIsBuffer(al_buf));
	al_bufs_outstanding++;

	return al_buf;
}


/**
 * free the buffer and its contained sound data.
 *
 * @param al_buf buffer name
 */
static void al_buf_free(ALuint al_buf)
{
	// no-op if 0 (needed in case SndData_reload fails -
	// sd->al_buf will not have been set)
	if(!al_buf)
		return;

	ENSURE(alIsBuffer(al_buf));

	AL_CHECK;
	alDeleteBuffers(1, &al_buf);
	AL_CHECK;

	al_bufs_outstanding--;
}


/**
 * make sure all buffers have been returned to us via al_buf_free.
 * called from al_shutdown.
 */
static void al_buf_shutdown()
{
	ENSURE(al_bufs_outstanding == 0);
}


//-----------------------------------------------------------------------------
// AL source suballocator: allocate all available sources up-front and
// pass them out as needed (alGenSources is quite slow, taking 3..5 ms per
// source returned). also responsible for enforcing user-specified limit
// on total number of sources (to reduce mixing cost on low-end systems).
//-----------------------------------------------------------------------------

// regardless of HW capabilities, we won't use more than this ("enough").
// necessary in case OpenAL doesn't limit #sources (e.g. if SW mixing).
static const size_t AL_SRC_MAX = 64;

// (allow changing at runtime)
static size_t al_src_maxNumToUse = AL_SRC_MAX;

static size_t al_src_numPreallocated;

enum AllocationState
{
	kAvailable = 0,	// (must match zero-initialization of al_srcs_allocationStates)
	kInUse
};

// note: we want to catch double-free bugs and ensure all sources
// are released at exit, but OpenAL doesn't specify an always-invalid
// source name, so we need a separate array of AllocationState.
static ALuint al_srcs[AL_SRC_MAX];
static intptr_t al_srcs_allocationStates[AL_SRC_MAX];

/**
 * grab as many sources as possible up to the limit.
 * called from al_init.
 */
static void al_src_init()
{
	// grab as many sources as possible and count how many we get.
	for(size_t i = 0; i < al_src_maxNumToUse; i++)
	{
		ALuint al_src;
		alGenSources(1, &al_src);
		// we've reached the limit, no more are available.
		if(alGetError() != AL_NO_ERROR)
			break;
		ENSURE(alIsSource(al_src));
		al_srcs[i] = al_src;
		al_src_numPreallocated++;
	}

	// limit user's cap to what we actually got.
	// (in case snd_set_max_src was called before this)
	if(al_src_maxNumToUse > al_src_numPreallocated)
		al_src_maxNumToUse = al_src_numPreallocated;

	// make sure we got the minimum guaranteed by OpenAL.
	ENSURE(al_src_numPreallocated >= 16);
}


/**
 * release all sources on free list.
 * all sources must already have been released via al_src_free.
 * called from al_shutdown.
 */
static void al_src_shutdown()
{
	for(size_t i = 0; i < al_src_numPreallocated; i++)
		ENSURE(al_srcs_allocationStates[i] == kAvailable);

	AL_CHECK;
	alDeleteSources((ALsizei)al_src_numPreallocated, al_srcs);
	AL_CHECK;

	al_src_numPreallocated = 0;
}


/**
 * try to allocate a source.
 *
 * @return whether a source was allocated (see al_srcs_allocationStates).
 * @param al_src receives the new source name iff true is returned.
 */
static bool al_src_alloc(ALuint& al_src)
{
	for(size_t i = 0; i < al_src_numPreallocated; i++)
	{
		if(cpu_CAS(&al_srcs_allocationStates[i], kAvailable, kInUse))
		{
			al_src = al_srcs[i];
			return true;
		}
	}

	return false;	// no more to give
}


/**
 * mark a source as free and available for reuse.
 *
 * @param al_src source name
 */
static void al_src_free(ALuint al_src)
{
	ENSURE(alIsSource(al_src));

	const ALuint* pos = std::find(al_srcs, al_srcs+al_src_numPreallocated, al_src);
	if(pos != al_srcs+al_src_numPreallocated)	// found it
	{
		const size_t i = pos - al_srcs;
		ENSURE(cpu_CAS(&al_srcs_allocationStates[i], kInUse, kAvailable));
	}
	else
		DEBUG_WARN_ERR(ERR::LOGIC);	// al_src wasn't in al_srcs
}


/**
 * set maximum number of voices to play simultaneously,
 * to reduce mixing cost on low-end systems.
 * this limit may be ignored if e.g. there's a stricter
 * implementation- defined ceiling anyway.
 *
 * @param limit max. number of sources
 * @return Status
 */
Status snd_set_max_voices(size_t limit)
{
	// valid if cap is legit (less than what we allocated in al_src_init),
	// or if al_src_init hasn't been called yet. note: we accept anything
	// in the second case, as al_src_init will sanity-check al_src_cap.
	if(!al_src_numPreallocated || limit < al_src_numPreallocated)
	{
		al_src_maxNumToUse = limit;
		return INFO::OK;
	}
	// user is requesting a cap higher than what we actually allocated.
	// that's fine (not an error), but we won't set the cap, since it
	// determines how many sources may be returned.
	// there's no return value to indicate this because the cap is
	// precisely that - an upper limit only, we don't care if it can't be met.
	else
		return INFO::OK;
}


//-----------------------------------------------------------------------------
// OpenAL startup mechanism: allows deferring init until sounds are actually
// played, therefore speeding up perceived game start time.
// also resets OpenAL when settings (e.g. device) are changed at runtime.
//-----------------------------------------------------------------------------

/**
 * master OpenAL init; makes sure all subsystems are ready for use.
 * called from each snd_open; no harm if called more than once.
 *
 * @return Status
 */
static Status al_init()
{
	// only take action on first call, OR when re-initializing.
	if(al_initialized)
		return INFO::OK;

	RETURN_STATUS_IF_ERR(alc_init());

	al_initialized = true;

	// these can't fail:
	al_src_init();
	al_listener_latch();

	alDistanceModel(AL_LINEAR_DISTANCE_CLAMPED);

	return INFO::OK;
}


/**
 * shut down all module subsystems.
 */
static void al_shutdown()
{
	// was never initialized - nothing to do.
	if(!al_initialized)
		return;

	// somewhat tricky: go through gyrations to free OpenAL resources.

	// .. free all active sounds so that they release their source.
	//    the SndData reference is also removed,
	//    but these remain open, since they are cached.
	list_free_all();

	// .. actually free all (still cached) SndData instances.
	hsd_list_free_all();

	// .. all sources and buffers have been returned to their suballocators.
	//    now free them all.
	al_src_shutdown();
	al_buf_shutdown();

	alc_shutdown();

	al_initialized = false;
}


/**
 * re-initialize OpenAL. currently only required for changing devices.
 *
 * @return Status
 */
static Status al_reinit()
{
	// not yet initialized. settings have been saved, and will be
	// applied by the component init routines called from al_init.
	if(!al_initialized)
		return INFO::OK;

	// re-init (stops all currently playing sounds)
	al_shutdown();
	return al_init();
}


// prevent OpenAL from being initialized when snd_init is called.
static bool snd_disabled = false;

/**
 * extra layer on top of al_init that allows 'disabling' sound.
 * called from each snd_open.
 *
 * @return Status from al_init, or ERR::AGAIN if sound disabled
 */
static Status snd_init()
{
	// (note: each VSrc_reload and therefore snd_open will fail)
	if(snd_disabled)
		return ERR::AGAIN;	// NOWARN

	return al_init();
}


Status snd_disable(bool disabled)
{
	snd_disabled = disabled;

	if(snd_disabled)
	{
		ENSURE(!al_initialized);	// already initialized => disable is pointless
		return INFO::OK;
	}
	else
		return snd_init();	// note: won't return ERR::AGAIN, since snd_disabled == false
}


/**
 * free all resources and shut down the sound system.
 * call before h_mgr_shutdown.
 */
void snd_shutdown()
{
	al_shutdown();	// calls list_free_all
}


//-----------------------------------------------------------------------------
// device enumeration: list all devices and allow the user to choose one,
// in case the default device has problems.
//-----------------------------------------------------------------------------

// set by snd_dev_prepare_enum; used by snd_dev_next.
// consists of back-to-back C strings, terminated by an extra '\0'.
// (this is taken straight from OpenAL; the spec says the format may change).
static const char* devs;

/**
 * Prepare to enumerate all device names (this resets the list returned by
 * snd_dev_next).
 *
 * may be called each time the device list is needed.
 *
 * @return Status; always successful unless the requisite device
 * enumeration extension isn't available. in the latter case,
 * a "cannot enum device" message should be presented to the user,
 * and snd_dev_set need not be called; OpenAL will use its default device.
 */
Status snd_dev_prepare_enum()
{
	if(alcIsExtensionPresent(0, (alcString)"ALC_ENUMERATION_EXT") != AL_TRUE)
		WARN_RETURN(ERR::NOT_SUPPORTED);

	devs = (const char*)alcGetString(0, ALC_DEVICE_SPECIFIER);
	return INFO::OK;
}


/**
 * Get next device name.
 *
 * do not call unless snd_dev_prepare_enum succeeded!
 * not thread-safe! (static data from snd_dev_prepare_enum is used)
 *
 * @return const char* device name, or 0 if all have been returned
 */
const char* snd_dev_next()
{
	if(!*devs)
		return 0;
	const char* dev = devs;
	devs += strlen(dev)+1;
	return dev;
}


//-----------------------------------------------------------------------------
// sound data provider: holds audio data (clip or stream) and returns
// OpenAL buffers on request.
//-----------------------------------------------------------------------------

// rationale for separate VSrc (instance) and SndData resources:
// - we need to be able to fade out and cancel loops.
//   => VSrc isn't fire and forget; we need to access sounds at runtime.
// - allowing access via direct pointer is unsafe
//   => Handle-based access is required
// - we don't want to reload sound data on every play()
//   => need either a separate caching mechanism or one central data resource.
// - we want to support reloading (for consistency if not necessity)
//   => can't hack via h_find / setting fn_key to 0; need a separate instance.

/**
 * rationale for supporting both clips and streams:
 * streams avoid delays while reading+decompressing large files, but
 * playing multiple instances of them would require separate positions etc.
 * since the same clip is often played concurrently and we can't guarantee
 * they will never exceed the size of a stream, it makes sense to support a
 * separate "clip" data type that allocates enough storage and avoids needing
 * the stream position/list of buffers.
 */
enum SndDataType
{
	SD_CLIP,
	SD_STREAM
};

/**
 * Holder for sound data - either a clip, or stream.
 */
struct SndData
{
	ALenum al_fmt;
	ALsizei al_freq;

	SndDataType type;

	ALuint al_buf;	// valid if type == SD_CLIP
	OggStreamPtr ogg;	// valid if type == SD_STREAM
};

H_TYPE_DEFINE(SndData);


//-----------------------------------------------------------------------------
// SndData instance list: ensures all allocated since last al_shutdown
// are freed when desired (they are cached => extra work needed).

// rationale: all SndData objects (actually, their OpenAL buffers) must be
// freed during al_shutdown, to prevent leaks. we can't rely on list*
// to free all VSrc, and thereby their associated SndData objects -
// completed sounds are no longer in the list.
//
// nor can we use the h_mgr_shutdown automatic leaked resource cleanup:
// we need to be able to al_shutdown at runtime
// (when resetting OpenAL, after e.g. device change).
//
// h_mgr support is required to forcibly close SndData objects,
// since they are cached (kept open). it requires that this come after
// H_TYPE_DEFINE(SndData), since h_force_free needs the handle type.
//
// we never need to delete single entries: hsd_list_free_all
// (called by al_shutdown) frees each entry and clears the entire list.

typedef std::vector<Handle> Handles;
static Handles hsd_list;

/**
 * Add hsd to the list.
 * called from SndData_reload; will later be removed via hsd_list_free_all.
 * @param hsd Handle to SndData
 */
static void hsd_list_add(Handle hsd)
{
	hsd_list.push_back(hsd);
}


/**
 * Free all sounds on list.
 * called by al_shutdown (at exit, or when reinitializing OpenAL).
 * (definition must come after H_TYPE_DEFINE(SndData))
 */
static void hsd_list_free_all()
{
	for(Handles::iterator it = hsd_list.begin(); it != hsd_list.end(); ++it)
	{
		Handle& hsd = *it;

		(void)h_force_free(hsd, H_SndData);
		// ignore errors - if hsd was a stream, and its associated source
		// was active when al_shutdown was called, it will already have been
		// freed (list_free_all would free the source; it then releases
		// its SndData reference, which closes the instance because it's
		// RES_UNIQUE).
		//
		// NB: re-initializing the sound library (e.g. after changing
		// HW settings) requires all handles to be freed, even if cached.
		// hence, we use h_force_free. unfortunately this causes the
		// handle's tag to be ignored. it is conceivable that the wrong
		// handle could be freed here.
		//
		// we rule this out with the following argument. either we're
		// called when re-initializing sound or at exit. in the former
		// case, h_force_free does check the handle type: only sounds are
		// ever freed. we don't care if the wrong one is closed since all
		// must be stomped upon. in the latter case, it definitely doesn't
		// matter what we free. hence, no problem.
	}

	// leave its memory intact, so we don't have to reallocate it later
	// if we are now reinitializing OpenAL (not exiting).
	hsd_list.resize(0);
}


static void SndData_init(SndData* UNUSED(sd), va_list UNUSED(args))
{
}

static void SndData_dtor(SndData* sd)
{
	if(sd->type == SD_CLIP)
		al_buf_free(sd->al_buf);
	else
		sd->ogg.reset();
}

static Status SndData_reload(SndData* sd, const PIVFS& vfs, const VfsPath& pathname, Handle hsd)
{
#if 0 // HACK: streaming disabled because it breaks archives

	// (OGG streaming requires a real POSIX pathname - see OpenOggStream)
	fs::wpath real_pathname;
	RETURN_STATUS_IF_ERR(vfs->GetRealPath(pathname, real_pathname));

	// currently only supports OGG; WAV is no longer supported. writing our own loader is infeasible
	// due to a seriously watered down spec with many incompatible variants.
	// pulling in an external library (e.g. freealut) is deemed not worth the
	// effort - OGG should be better in all cases.

	RETURN_STATUS_IF_ERR(OpenOggStream(real_pathname, sd->ogg));
	const size_t size = fs::file_size(real_pathname);
#else
	RETURN_STATUS_IF_ERR(OpenOggNonstream(vfs, pathname, sd->ogg));
	FileInfo fileInfo;
	RETURN_STATUS_IF_ERR(vfs->GetFileInfo(pathname, &fileInfo));
	const size_t size = fileInfo.Size();
#endif

	sd->al_freq = sd->ogg->SamplingRate();
	sd->al_fmt  = sd->ogg->Format();

	// HACK - it would be nicer for callers to confirm they won't
	// open the same (streamed) file multiple times,
	// but that's not possible with the current JSI_Sound.
	sd->type = (size > 500*KiB)? SD_STREAM : SD_CLIP;

	if(sd->type == SD_CLIP)
	{
		std::vector<u8> data(50*MiB);	// max. size of any clip (anything larger should be streamed)
		const Status ret = sd->ogg->GetNextChunk(&data[0], data.size());
		RETURN_STATUS_IF_ERR(ret);
		const size_t size = (size_t)ret;
		ENSURE(size != 0);	// must have read something
		ENSURE(size != data.size());	// shouldn't be limited by buffer size
		sd->al_buf = al_buf_alloc(&data[0], (ALsizei)size, sd->al_fmt, sd->al_freq);
		sd->ogg.reset();
	}
	else
		sd->al_buf = 0;

	// note: to avoid polluting hsd_list with invalid handles, we ensure
	// all of the above succeeded before adding to the list.
	// (c.f. topic#10719, "Problem freeing sounds loaded by JavaScript")
	hsd_list_add(hsd);

	return INFO::OK;
}

static Status SndData_validate(const SndData* sd)
{
	if(sd->al_fmt == 0)
		WARN_RETURN(ERR::_11);
	if((size_t)sd->al_freq > 100000)	// suspicious
		WARN_RETURN(ERR::_12);
	if(sd->type == SD_CLIP)
	{
		if(!sd->al_buf)
			WARN_RETURN(ERR::_13);
	}
	else if(sd->type == SD_STREAM)
	{
		if(!sd->ogg)
			WARN_RETURN(ERR::_14);
	}
	else
		WARN_RETURN(ERR::_15);	// invalid type
	return INFO::OK;
}


static Status SndData_to_string(const SndData* sd, wchar_t* buf)
{
	const wchar_t* type = (sd->type == SD_CLIP)? L"clip" : L"stream";
	swprintf_s(buf, H_STRING_LEN, L"%ls; al_buf=%u", type, sd->al_buf);
	return INFO::OK;
}


/**
 * open and return a handle to a sound file's data.
 *
 * @return Handle or Status on failure
 */
static Handle snd_data_load(const PIVFS& vfs, const VfsPath& pathname)
{
	return h_alloc(H_SndData, vfs, pathname);
}

/**
 * Free the sound.
 *
 * @param hsd Handle to SndData; set to 0 afterwards.
 * @return Status
 */
static Status snd_data_free(Handle& hsd)
{
	return h_free(hsd, H_SndData);
}


//-----------------------------------------------------------------------------

/**
 * Get the sound's AL buffer (typically to play it)
 *
 * @param hsd Handle to SndData.
 * @param al_buf buffer name.
 * @return Status, most commonly:
 * INFO::OK = buffer has been returned; more are expected to be available.
 * INFO::ALL_COMPLETE = buffer has been returned but is the last one (EOF).
 */
static Status snd_data_buf_get(Handle hsd, ALuint& al_buf)
{
	H_DEREF(hsd, SndData, sd);
	if(sd->type == SD_CLIP)
	{
		al_buf = sd->al_buf;
		return INFO::ALL_COMPLETE;	// "EOF"
	}

	if(!sd->ogg)
		WARN_RETURN(ERR::INVALID_HANDLE);
	u8 data[maxBufferSize];
	const Status ret = sd->ogg->GetNextChunk(data, maxBufferSize);
	RETURN_STATUS_IF_ERR(ret);
	const size_t size = (size_t)ret;
	al_buf = al_buf_alloc(data, (ALsizei)size, sd->al_fmt, sd->al_freq);

	return (size < maxBufferSize)? INFO::ALL_COMPLETE : INFO::OK;
}


/**
 * Indicate the sound's buffer is no longer needed.
 *
 * @param hsd Handle to SndData.
 * @param al_buf buffer name
 * @return Status
 */
static Status snd_data_buf_free(Handle hsd, ALuint al_buf)
{
	H_DEREF(hsd, SndData, sd);

	if(sd->type == SD_CLIP)
	{
		// no-op (caller will later release hsd reference;
		// when hsd actually unloads, sd->al_buf will be freed).
	}
	else
		al_buf_free(al_buf);

	return INFO::OK;
}


//-----------------------------------------------------------------------------
// fading
//-----------------------------------------------------------------------------

/**
 * control block for a fade operation.
 */
struct FadeInfo
{
	double start_time;
	FadeType type;
	float length;
	float initial_val;
	float final_val;
};

static float fade_factor_linear(float t)
{
	return t;
}

static float fade_factor_exponential(float t)
{
	// t**3
	return t*t*t;
}

static float fade_factor_s_curve(float t)
{
	// cosine curve
	const double pi = 3.14159265358979323846;
	float y = cos(t*pi + pi);
	// map [-1,1] to [0,1]
	return (y + 1.0f) / 2.0f;
}


/**
 * fade() return value; indicates if the fade operation is complete.
 */
enum FadeRet
{
	FADE_NO_CHANGE,
	FADE_CHANGED,
	FADE_TO_0_FINISHED
};

/**
 * Carry out the requested fade operation.
 *
 * This is called for each active VSrc; if they have no fade operation
 * active, nothing happens. Note: as an optimization, we could make a
 * list of VSrc with fade active and only call this for those;
 * not yet necessary, though.
 *
 * @param fi Describes the fade operation
 * @param cur_time typically returned via timer_Time()
 * @param out_val Output gain value, i.e. the current result of the fade.
 * @return FadeRet
 */
static FadeRet fade(FadeInfo& fi, double cur_time, float& out_val)
{
	// no fade in progress - abort immediately. this check is necessary to
	// avoid division-by-zero below.
	if(fi.type == FT_NONE)
		return FADE_NO_CHANGE;

	ENSURE(0.0f <= fi.initial_val && fi.initial_val <= 1.0f);
	ENSURE(0.0f <= fi.final_val && fi.final_val <= 1.0f);

	// end reached - if fi.length is 0, but the fade is "in progress", do the
	// processing here, and skip the dangerous division
	if(fi.type == FT_ABORT || (cur_time >= fi.start_time + fi.length))
	{
		// make sure exact value is hit
		out_val = fi.final_val;

		// special case: we were fading out; caller will free the sound.
		if(fi.final_val == 0.0f)
			return FADE_TO_0_FINISHED;

		// wipe out all values amd mark as no longer actively fading
		memset(&fi, 0, sizeof(fi));
		fi.type = FT_NONE;

		return FADE_CHANGED;
	}

	// how far into the fade are we? [0,1]
	const float t = (cur_time - fi.start_time) / fi.length;
	ENSURE(0.0f <= t && t <= 1.0f);

	float factor;
	switch(fi.type)
	{
	case FT_LINEAR:
		factor = fade_factor_linear(t);
		break;
	case FT_EXPONENTIAL:
		factor = fade_factor_exponential(t);
		break;
	case FT_S_CURVE:
		factor = fade_factor_s_curve(t);
		break;

	// initialize with anything at all, just so that the calculation
	// below runs through; we reset out_val after that.
	case FT_ABORT:
		factor = 0.0f;
		break;

	NODEFAULT;
	}

	out_val = fi.initial_val + factor*(fi.final_val - fi.initial_val);

	return FADE_CHANGED;
}


/**
 * Is the fade operation currently active?
 *
 * @param FadeInfo
 * @return bool
 */
static bool fade_is_active(FadeInfo& fi)
{
	return (fi.type != FT_NONE);
}


//-----------------------------------------------------------------------------
// virtual sound source: a sound the user wants played.
// owns source properties, buffer queue, and references SndData.
//-----------------------------------------------------------------------------

// rationale: combine Src and VSrc - best interface, due to needing hsd,
// buffer queue (# processed) in update

enum VSrcFlags
{
	// (we can't just test if al_src is zero because that might be a
	// valid source name!)
	VS_HAS_AL_SRC  = 1,

	// SndData has reported EOF. will close down after last buffer completes.
	VS_EOF         = 2,

	// this VSrc was added via list_add and needs to be removed with
	// list_remove in VSrc_dtor.
	// not set if load fails somehow (avoids list_remove "not found" error).
	VS_IN_LIST     = 4,

	VS_SHOULD_STOP = 8,

	VS_ALL_FLAGS = VS_HAS_AL_SRC|VS_EOF|VS_IN_LIST|VS_SHOULD_STOP
};

/**
 * control block for a virtual source, which represents a sound that the
 * application wants played. it may or may not be played, depending on
 * priority and whether an actual OpenAL source is available.
 */
struct VSrc
{
	bool HasSource() const
	{
		if((flags & VS_HAS_AL_SRC) == 0)
			return false;
		ENSURE(alIsSource(al_src));
		return true;
	}

	/// handle to this VSrc, so that it can close itself.
	Handle hvs;

	/// associated sound data
	Handle hsd;

	// AL source properties (set via snd_set*)
	ALfloat pos[3];
	ALfloat gain;	/// [0,inf)
	ALfloat pitch;	/// (0,1]
	ALboolean loop;
	ALboolean relative;

	/// controls vsrc_update behavior (VSrcFlags)
	size_t flags;

	// valid iff HasSource()
	ALuint al_src;

	// priority for voice management
	float static_pri;	/// as given by snd_play
	float cur_pri;		/// holds newly calculated value

	FadeInfo fade;
};

H_TYPE_DEFINE(VSrc);

static void VSrc_init(VSrc* vs, va_list UNUSED(args))
{
	vs->flags = 0;
	vs->fade.type = FT_NONE;
}

static void list_remove(VSrc* vs);
static Status vsrc_reclaim(VSrc* vs);

static void VSrc_dtor(VSrc* vs)
{
	// only remove if added (not the case if load failed)
	if(vs->flags & VS_IN_LIST)
	{
		list_remove(vs);
		vs->flags &= ~VS_IN_LIST;
	}

	// these are safe, even if reload (partially) failed:
	vsrc_reclaim(vs);
	(void)snd_data_free(vs->hsd);
}

static Status VSrc_reload(VSrc* vs, const PIVFS& vfs, const VfsPath& pathname, Handle hvs)
{
	// cannot wait till play(), need to init here:
	// must load OpenAL so that snd_data_load can check for OGG extension.
	Status err = snd_init();
	// .. don't complain if sound is disabled; fail silently.
	if(err == ERR::AGAIN)
		return err;
	// .. catch genuine errors during init.
	RETURN_STATUS_IF_ERR(err);

	VfsPath dataPathname;

	// pathname is a definition file containing the data file name and
	// its gain.
	if(pathname.Extension() == L".txt")
	{
		shared_ptr<u8> buf; size_t size;
		RETURN_STATUS_IF_ERR(vfs->LoadFile(pathname, buf, size));
		std::wistringstream def(std::wstring((wchar_t*)buf.get(), (int)size));

		def >> dataPathname;
		def >> vs->gain;
		vs->gain /= 100.0f;	// is stored as percent
	}
	// read the sound file directly and assume default gain (1.0).
	else
	{
		dataPathname = pathname;
		vs->gain = 1.0f;
	}

	// note: vs->gain can legitimately be > 1.0 - don't clamp.

	vs->pitch = 1.0f;

	vs->hvs = hvs;	// allows calling snd_free when done playing.

	vsrc_reclaim(vs);

	vs->hsd = snd_data_load(vfs, dataPathname);
	RETURN_STATUS_IF_ERR(vs->hsd);

	return INFO::OK;
}

static bool IsValidBoolean(ALboolean b)
{
	return (b == AL_FALSE || b == AL_TRUE);
}

static Status VSrc_validate(const VSrc* vs)
{
	// al_src can legitimately be 0 (if vs is low-pri)
	if(vs->flags & ~VS_ALL_FLAGS)
		WARN_RETURN(ERR::_1);
	// no limitations on <pos>
	if(!(0.0f <= vs->gain && vs->gain <= 1.0f))
		WARN_RETURN(ERR::_2);
	if(!(0.0f < vs->pitch && vs->pitch <= 2.0f))
		WARN_RETURN(ERR::_3);
	if(!IsValidBoolean(vs->loop) || !IsValidBoolean(vs->relative))
		WARN_RETURN(ERR::_4);
	// <static_pri> and <cur_pri> have no invariant we could check.
	return INFO::OK;
}

static Status VSrc_to_string(const VSrc* vs, wchar_t* buf)
{
	swprintf_s(buf, H_STRING_LEN, L"al_src = %u", vs->al_src);
	return INFO::OK;
}


/**
 * open and return a handle to a sound instance.
 *
 * @param pathname. if a text file (extension ".txt"),
 * it is assumed to be a definition file containing the
 * sound file name and its gain (0.0 .. 1.0).
 * otherwise, it is taken to be the sound file name and
 * gain is set to the default of 1.0 (no attenuation).
 * @return Handle or Status on failure
 */
Handle snd_open(const PIVFS& vfs, const VfsPath& pathname)
{
	// note: RES_UNIQUE forces each instance to get a new resource
	// (which is of course what we want).
	return h_alloc(H_VSrc, vfs, pathname, RES_UNIQUE);
}


/**
 * Free the sound; if it was playing, it will be stopped.
 * Note: sounds are closed automatically when done playing;
 * this is provided for completeness only.
 *
 * @param hvs Handle to VSrc. will be set to 0 afterwards.
 * @return Status
 */
Status snd_free(Handle& hvs)
{
	if(!hvs)
		return INFO::OK;
	return h_free(hvs, H_VSrc);
}


//-----------------------------------------------------------------------------
// list of active sounds. used by voice management component,
// and to have each VSrc update itself (queue new buffers).

// VSrc fields are used -> must come after struct VSrc

// sorted in descending order of current priority
// (we sometimes remove low pri items, which requires moving down
// everything that comes after them, so we want those to come last).
//
// don't use list, to avoid lots of allocs (expect thousands of VSrcs).
typedef std::deque<VSrc*> VSrcs;
typedef VSrcs::iterator VSrcIt;
static VSrcs vsrcs;

// don't need to sort now - caller will list_SortByDescendingPriority() during update.
static void list_add(VSrc* vs)
{
	vsrcs.push_back(vs);
}


/**
 * call back for each VSrc entry in the list.
 *
 * @param end_idx if not the default value of 0, stop before that entry.
 */
template<class Func>
static void list_foreach(Func callback, size_t numToSkip = 0, size_t end_idx = 0)
{
	const VSrcIt begin = vsrcs.begin() + numToSkip;
	const VSrcIt end = end_idx? begin+end_idx : vsrcs.end();

	// can't use std::for_each: some entries may have been deleted
	// (i.e. set to 0) since last update.
	for(VSrcIt it = begin; it != end; ++it)
	{
		VSrc* vs = *it;
		if(vs)
			callback(vs);
	}
}

struct GreaterPriority
{
	bool operator()(VSrc* vs1, VSrc* vs2) const
	{
		return vs1->cur_pri > vs2->cur_pri;
	}
};

/// sort list by decreasing 'priority' (most important first)
static void list_SortByDescendingPriority()
{
	std::sort(vsrcs.begin(), vsrcs.end(), GreaterPriority());
}


/**
 * scan list and remove the given VSrc (by setting it to 0; list will be
 * pruned later (see rationale below).
 * O(N)!
 *
 * @param vs VSrc to remove.
 */
static void list_remove(VSrc* vs)
{
	for(size_t i = 0; i < vsrcs.size(); i++)
	{
		if(vsrcs[i] == vs)
		{
			// found it; several ways we could remove:
			// - shift everything else down (slow) -> no
			// - fill the hole with e.g. the last element
			//   (vsrcs would no longer be sorted by priority) -> no
			// - replace with 0 (will require prune_removed and
			//   more work in foreach) -> best alternative
			vsrcs[i] = 0;
			return;
		}
	}

	DEBUG_WARN_ERR(ERR::LOGIC);	// VSrc not found
}


struct IsNull
{
	bool operator()(VSrc* vs) const
	{
		return (vs == 0);
	}
};

/**
 * remove entries that were set to 0 by list_remove, so that
 * code below can grant the first al_src_cap entries a source.
 */
static void list_prune_removed()
{
	VSrcIt new_end = remove_if(vsrcs.begin(), vsrcs.end(), IsNull());
	vsrcs.erase(new_end, vsrcs.end());
}


static void vsrc_free(VSrc* vs)
{
	snd_free(vs->hvs);
}

static Status list_free_all()
{
	list_foreach(vsrc_free);
	return INFO::OK;
}


//-----------------------------------------------------------------------------

/**
 * Send the VSrc properties to OpenAL (when we actually have a source).
 * called by snd_set * and vsrc_grant.
 *
 * @param VSrc*
 */
static void vsrc_latch(VSrc* vs)
{
	if(!vs->HasSource())
		return;

	float rolloff = 1.0f;
	float referenceDistance = 125.0f;
	float maxDistance = 500.0f;
	if(vs->relative)
	{
		rolloff = 0.0f;
		referenceDistance = 1.0f;
		maxDistance = FLT_MAX;
	}

	AL_CHECK;

	alSourcefv(vs->al_src, AL_POSITION,           vs->pos);
	alSource3f(vs->al_src, AL_VELOCITY,           0.0f, 0.0f, 0.0f);
	alSourcei (vs->al_src, AL_SOURCE_RELATIVE,    vs->relative);
	alSourcef (vs->al_src, AL_ROLLOFF_FACTOR,     rolloff);
	alSourcef (vs->al_src, AL_REFERENCE_DISTANCE, referenceDistance);
	alSourcef (vs->al_src, AL_MAX_DISTANCE,       maxDistance);
	alSourcef (vs->al_src, AL_GAIN,               vs->gain);
	alSourcef (vs->al_src, AL_PITCH,              vs->pitch);
	alSourcei (vs->al_src, AL_LOOPING,            vs->loop);

	//alSourcei (vs->al_src, AL_MIN_GAIN,           0.0f);
	//alSourcei (vs->al_src, AL_MAX_GAIN,           1.0f);
	//alSource3f(vs->al_src, AL_DIRECTION,          0.0f, 0.0f, 0.0f);
	//alSourcef (vs->al_src, AL_CONE_INNER_ANGLE,   360.0f);
	//alSourcef (vs->al_src, AL_CONE_OUTER_ANGLE,   360.0f);
	//alSourcef (vs->al_src, AL_CONE_OUTER_GAIN,    0.0f);
	//alSourcef (vs->al_src, AL_SEC_OFFSET,         0.0f);
	//alSourcef (vs->al_src, AL_SAMPLE_OFFSET,      0.0f);
	//alSourcef (vs->al_src, AL_BYTE_OFFSET,        0.0f);

	ALenum err = alGetError();
	if(err != AL_NO_ERROR)
	{
		debug_printf(L"vsrc_latch: one of the below is invalid:\n");
		debug_printf(L"  al_src: 0x%x\n", vs->al_src);
		debug_printf(L"  position: %f %f %f\n", vs->pos[0], vs->pos[1], vs->pos[2]);
		debug_printf(L"  velocity: %f %f %f\n", 0.0f, 0.0f, 0.0f);
		debug_printf(L"  relative: %d\n", (int)vs->relative);
		debug_printf(L"  rolloff: %f\n", rolloff);
		debug_printf(L"  ref dist: %f\n", referenceDistance);
		debug_printf(L"  max dist: %f\n", maxDistance);
		debug_printf(L"  gain: %f\n", vs->gain);
		debug_printf(L"  pitch: %f\n", vs->pitch);
		debug_printf(L"  loop: %d\n", (int)vs->loop);

		al_ReportError(err, __func__, __LINE__);
	}
}


/**
 * Dequeue any of the VSrc's sound buffers that are finished playing.
 *
 * @param VSrc*
 * @return int number of entries that were removed.
 */
static int vsrc_deque_finished_bufs(VSrc* vs)
{
	ENSURE(vs->HasSource());	// (otherwise there's no sense in calling this function)

	AL_CHECK;
	int num_processed;
	alGetSourcei(vs->al_src, AL_BUFFERS_PROCESSED, &num_processed);
	AL_CHECK;

	for(int i = 0; i < num_processed; i++)
	{
		ALuint al_buf;
		alSourceUnqueueBuffers(vs->al_src, 1, &al_buf);
		snd_data_buf_free(vs->hsd, al_buf);
	}

	AL_CHECK;
	return num_processed;
}


/**
 * Update the VSrc - perform fade (if active), queue/unqueue buffers.
 * Called once a frame.
 * must be a functor so that each call receives the same time (avoids repeated
 * calls to timer_Time and inconsistencies when crossfading)
 */
class VsrcUpdater
{
public:
	VsrcUpdater(double time)
		: time(time)
	{
	}

	Status operator()(VSrc* vs) const
	{
		if(!vs->HasSource())
			return INFO::OK;

		FadeRet fade_ret = fade(vs->fade, time, vs->gain);
		// auto-free after fadeout.
		if(fade_ret == FADE_TO_0_FINISHED)
		{
			vsrc_free(vs);
			return INFO::OK;	// don't continue - <vs> has been freed.
		}
		// fade in progress; latch current gain value.
		else if(fade_ret == FADE_CHANGED)
			vsrc_latch(vs);

		int num_queued;
		alGetSourcei(vs->al_src, AL_BUFFERS_QUEUED, &num_queued);
		AL_CHECK;

		int num_processed = vsrc_deque_finished_bufs(vs);
		UNUSED2(num_processed);

		if(vs->flags & VS_EOF)
		{
			// no more buffers left, and EOF reached - done playing.
			if(num_queued == 0)
			{
				snd_free(vs->hvs);
				return INFO::OK;
			}
		}
		// can still read from SndData
		else
		{
			// get next buffer
			ALuint al_buf;
			Status ret = snd_data_buf_get(vs->hsd, al_buf);
			RETURN_STATUS_IF_ERR(ret);
			if(ret == INFO::ALL_COMPLETE)	// no further buffers will be forthcoming
				vs->flags |= VS_EOF;

			alSourceQueueBuffers(vs->al_src, 1, &al_buf);
			AL_CHECK;

			// HACK: OpenAL stops the source if reloading took too long
			ALint state;
			alGetSourcei(vs->al_src, AL_SOURCE_STATE, &state);
			if((state == AL_STOPPED) && !(vs->flags & VS_SHOULD_STOP))
				alSourcePlay(vs->al_src);
		}

		return INFO::OK;
	}

private:
	double time;
};


/**
 * Try to give the VSrc an AL source so that it can (re)start playing.
 * called by snd_play and voice management.
 *
 * @return Status (ERR::FAIL if no AL source is available)
 */
static Status vsrc_grant(VSrc* vs)
{
	if(vs->HasSource())	// already playing
		return INFO::OK;

	// try to allocate a source. snd_play calls us in the hope that a source
	// happens to be free, but if not, just skip the remaining steps and
	// wait for the next update.
	if(!al_src_alloc(vs->al_src))
		return ERR::FAIL;	// NOWARN
	vs->flags |= VS_HAS_AL_SRC;

	// pass (user-specifiable) properties on to OpenAL.
	vsrc_latch(vs);

	// queue up some buffers (enough to start playing, at least).
	VsrcUpdater updater(timer_Time());
	updater(vs);
	AL_CHECK;

	alSourcePlay(vs->al_src);
	AL_CHECK;
	return INFO::OK;
}


/**
 * stop playback, and reclaim the OpenAL source.
 * called when closing the VSrc, or when voice management decides
 * this VSrc must yield to others of higher priority.
 */
static Status vsrc_reclaim(VSrc* vs)
{
	if(!vs->HasSource())
		return ERR::FAIL;	// NOWARN

	// clear the source's buffer queue (necessary because buffers cannot
	// be deleted at shutdown while still attached to a source).
	// note: OpenAL 1.1 says all buffers become "processed" when the
	// source is stopped (so vsrc_deque_finished_bufs ought to have the
	// desired effect), but that isn't the case on some Linux
	// implementations (OpenALsoft and PulseAudio with on-board NVidia).
	// wiping out the entire queue by attaching the null buffer is safer,
	// but still doesn't cause versions of OpenALsoft older than 2009-08-11
	// to correctly reset AL_BUFFERS_PROCESSED. in "Re: [Openal-devel]
	// Questionable "invalid value" from alSourceUnqueueBuffers", the
	// developer recommended working around this bug by rewinding the
	// source instead of merely issuing alSourceStop.
	// reference: http://trac.wildfiregames.com/ticket/297
	vs->loop = false;
	vsrc_latch(vs);

	vs->flags |= VS_SHOULD_STOP;

	alSourceStop(vs->al_src);

	vsrc_deque_finished_bufs(vs);
	alSourcei(vs->al_src, AL_BUFFER, AL_NONE);

	alSourceRewind(vs->al_src);

	al_src_free(vs->al_src);
	vs->flags &= ~VS_HAS_AL_SRC;

	return INFO::OK;
}


//-----------------------------------------------------------------------------
// snd_mgr API
//-----------------------------------------------------------------------------

/**
 * Request the sound be played.
 *
 * Once done playing, the sound is automatically closed (allows
 * fire-and-forget play code).
 * if no hardware voice is available, this sound may not be played at all,
 * or in the case of looped sounds, start later.
 *
 * @param hvs Handle to VSrc
 * @param static_pri (min 0 .. max 1, default 0) indicates which sounds are
 * considered more important; this is attenuated by distance to the
 * listener (see snd_update).
 * @return Status
 */
Status snd_play(Handle hvs, float static_pri)
{
	H_DEREF(hvs, VSrc, vs);

	// note: vs->hsd is valid, otherwise snd_open would have failed
	// and returned an invalid handle (caught above).

	vs->static_pri = static_pri;
	list_add(vs);
	vs->flags |= VS_IN_LIST;

	// optimization (don't want to do full update here - too slow)
	// either we get a source and playing begins immediately,
	// or it'll be taken care of on next update.
	vsrc_grant(vs);
	return INFO::OK;
}


/**
 * Change 3d position of the sound source.
 *
 * May be called at any time; fails with invalid handle return if
 * the sound has already been closed (e.g. it never played).
 *
 * @param hvs Handle to VSrc
 * @param x,y,z coordinates (interpretation: see below)
 * @param relative if true, (x,y,z) is treated as relative to the listener;
 * otherwise, it is the position in world coordinates (default).
 * @return Status
 */
Status snd_set_pos(Handle hvs, float x, float y, float z, bool relative)
{
	H_DEREF(hvs, VSrc, vs);

	vs->pos[0] = x; vs->pos[1] = y; vs->pos[2] = z;
	vs->relative = relative;

	vsrc_latch(vs);
	return INFO::OK;
}


/**
 * Change gain (amplitude modifier) of the sound source.
 *
 * should not be called during a fade (see note in implementation);
 * fails with invalid handle return if the sound has already been
 * closed (e.g. it never played).
 *
 * @param hvs Handle to VSrc
 * @param gain modifier; must be non-negative;
 * 1 -> unattenuated, 0.5 -> -6 dB, 0 -> silence.
 * @return Status
 */
Status snd_set_gain(Handle hvs, float gain)
{
	H_DEREF(hvs, VSrc, vs);

	if(!(0.0f <= gain && gain <= 1.0f))
		WARN_RETURN(ERR::INVALID_PARAM);

	// if fading, gain changes would be overridden during the next
	// snd_update. attempting this indicates a logic error. we abort to
	// avoid undesired jumps in gain that might surprise (and deafen) users.
	if(fade_is_active(vs->fade))
		WARN_RETURN(ERR::LOGIC);

	vs->gain = gain;

	vsrc_latch(vs);
	return INFO::OK;
}


/**
 * Change pitch shift of the sound source.
 *
 * may be called at any time; fails with invalid handle return if
 * the sound has already been closed (e.g. it never played).
 *
 * @param hvs Handle to VSrc
 * @param pitch shift: 1.0 means no change; each doubling/halving equals a
 * pitch shift of +/-12 semitones (one octave). zero is invalid.
 * @return Status
 */
Status snd_set_pitch(Handle hvs, float pitch)
{
	H_DEREF(hvs, VSrc, vs);

	if(pitch <= 0.0f)
		WARN_RETURN(ERR::INVALID_PARAM);

	vs->pitch = pitch;

	vsrc_latch(vs);
	return INFO::OK;
}


/**
 * Enable/disable looping on the sound source.
 * used to implement variable-length sounds (e.g. while building).
 *
 * may be called at any time; fails with invalid handle return if
 * the sound has already been closed (e.g. it never played).
 *
 * notes:
 * - looping sounds are not discarded if they cannot be played for lack of
 *   a hardware voice at the moment play was requested.
 * - once looping is again disabled and the sound has reached its end,
 *   the sound instance is freed automatically (as if never looped).
 *
 * @param hvs Handle to VSrc
 */
Status snd_set_loop(Handle hvs, bool loop)
{
	H_DEREF(hvs, VSrc, vs);

	vs->loop = loop;

	vsrc_latch(vs);
	return INFO::OK;
}


/**
 * Fade the sound source in or out over time.
 * Its gain starts at \<initial_gain\> immediately and is moved toward
 * \<final_gain\> over \<length\> seconds.
 *
 * may be called at any time; fails with invalid handle return if
 * the sound has already been closed (e.g. it never played).
 *
 * note that this function doesn't busy-wait until the fade is complete;
 * any number of fades may be active at a time (allows cross-fading).
 * each snd_update calculates a new gain value for all pending fades.
 * it is safe to start another fade on the same sound source while
 * one is currently in progress; the old one is dropped.
 *
 * @param hvs Handle to VSrc
 * @param initial_gain gain. if < 0 (an otherwise illegal value), the sound's
 * current gain is used as the start value (useful for fading out).
 * @param final_gain gain. if 0, the sound is freed when the fade completes or
 * is aborted, thus allowing fire-and-forget fadeouts. no cases are
 * foreseen where this is undesirable, and it is easier to implement
 * than an extra set-free-after-fade-flag function.
 * @param length duration of fade [s]
 * @param type determines the fade curve: linear, exponential or S-curve.
 * for guidance on which to use, see
 * http://www.transom.org/tools/editing_mixing/200309.stupidfadetricks.html
 * you can also pass FT_ABORT to stop fading (if in progress) and
 * set gain to the final_gain parameter passed here.
 * @return Status
 */
Status snd_fade(Handle hvs, float initial_gain, float final_gain,
	float length, FadeType type)
{
	H_DEREF(hvs, VSrc, vs);

	if(type != FT_LINEAR && type != FT_EXPONENTIAL && type != FT_S_CURVE && type != FT_ABORT)
		WARN_RETURN(ERR::INVALID_PARAM);

	// special case - set initial value to current gain (see above).
	if(initial_gain < 0.0f)
		initial_gain = vs->gain;

	const double cur_time = timer_Time();

	FadeInfo& fi = vs->fade;
	fi.type        = type;
	fi.start_time  = cur_time;
	fi.initial_val = initial_gain;
	fi.final_val   = final_gain;
	fi.length      = length;

	(void)fade(fi, cur_time, vs->gain);
	vsrc_latch(vs);

	return INFO::OK;
}


/** --TODO: Test to ensure this works (not currently necessary for intensity)
 * find out if a sound is still playing
 *
 * @param hvs - handle to the snd to check
 * @return bool true if playing
 **/
bool snd_is_playing(Handle hvs)
{
	// (can't use H_DEREF due to bool return value)
	VSrc* vs = H_USER_DATA(hvs, VSrc);

	// sound has played and was already freed or is otherwise not loaded.
	if(!vs)
		return false;

	// 'just' finished playing
	if(!vs->HasSource())
		return false;

	return true;
}


//-----------------------------------------------------------------------------
// voice management: grants the currently most 'important' sounds
// a hardware voice.
//-----------------------------------------------------------------------------

/// length of vector squared (avoids costly sqrt)
static float magnitude_2(const float v[3])
{
	return v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
}


/**
 * Determine new priority of the VSrc based on distance to listener and
 * static priority.
 * Called via list_foreach.
 */
static void calc_cur_pri(VSrc* vs)
{
	const float MAX_DIST_2 = 1000.0f;
	const float falloff = 10.0f;

	float d_2;	// euclidean distance to listener (squared)
	if(vs->relative)
		d_2 = magnitude_2(vs->pos);
	else
		d_2 = al_listener_dist_2(vs->pos);

	// scale priority down exponentially
	float e = d_2 / MAX_DIST_2;	// 0.0f (close) .. 1.0f (far)

	// assume farther away than OpenAL cutoff - no sound contribution
	float cur_pri = -1.0f;
	if(e < 1.0f)
		cur_pri = vs->static_pri / pow(falloff, e);
	vs->cur_pri = cur_pri;
}


/**
 * convenience function that strips all unimportant VSrc of their AL source.
 * called via list_foreach; also immediately frees discarded clips.
 */
static void reclaim(VSrc* vs)
{
	vsrc_reclaim(vs);

	if(!vs->loop)
		snd_free(vs->hvs);
}


/**
 * update voice management, i.e. recalculate priority and assign AL sources.
 * no-op if OpenAL not yet initialized.
 */
static Status vm_update()
{
	list_prune_removed();

	// update current priorities (a function of static priority and distance).
	list_foreach(calc_cur_pri);

	list_SortByDescendingPriority();

	// partition list; the first ones will be granted a source
	// (if they don't have one already), after reclaiming all sources from
	// the remainder of the VSrc list entries.
	size_t first_unimportant = std::min((size_t)vsrcs.size(), al_src_maxNumToUse);
	list_foreach(reclaim, first_unimportant, 0);
	list_foreach(vsrc_grant, 0, first_unimportant);

	return INFO::OK;
}


//-----------------------------------------------------------------------------

/**
 * perform housekeeping (e.g. streaming); call once a frame.
 *
 * @param pos position support vector. if NULL, all parameters are
 * ignored and listener position unchanged; this is useful in case the
 * world isn't initialized yet.
 * @param dir view direction
 * @param up up vector
 * @return Status
 */
Status snd_update(const float* pos, const float* dir, const float* up)
{
	// there's no sense in updating anything if we weren't initialized
	// yet (most notably, if sound is disabled). we check for this to
	// avoid confusing the code below. the caller should complain if
	// this fails, so report success here (everything will work once
	// sound is re-enabled).
	if(!al_initialized)
		return INFO::OK;

	if(pos)
		al_listener_set_pos(pos, dir, up);

	vm_update();

	// for each source: add / remove buffers; carry out fading.
	list_foreach(VsrcUpdater(timer_Time()));

	return INFO::OK;
}


#else	// CONFIG2_AUDIO

// Stub implementations of snd_mgr API:

Status snd_dev_prepare_enum() { return ERR::NOT_SUPPORTED; }
const char* snd_dev_next() { return NULL; }
Status snd_dev_set(const char* alc_new_dev_name) { return INFO::OK; }
Status snd_set_max_voices(size_t limit) { return INFO::OK; }
Status snd_set_master_gain(float gain) { return INFO::OK; }
Handle snd_open(const PIVFS& vfs, const VfsPath& pathname) { return ERR::FAIL; }
Status snd_free(Handle& hvs) { return INFO::OK; }
Status snd_play(Handle hvs, float static_pri) { return INFO::OK; }
Status snd_set_pos(Handle hvs, float x, float y, float z, bool relative) { return INFO::OK; }
Status snd_set_gain(Handle hs, float gain) { return INFO::OK; }
Status snd_set_pitch(Handle hs, float pitch) { return INFO::OK; }
Status snd_set_loop(Handle hvs, bool loop) { return INFO::OK; }
Status snd_fade(Handle hvs, float initial_gain, float final_gain, float length, FadeType type) { return INFO::OK; }
Status snd_disable(bool disabled) { return INFO::OK; }
Status snd_update(const float* pos, const float* dir, const float* up) { return INFO::OK; }
bool snd_is_playing(Handle hvs) { return false; }
void snd_shutdown() { }

#endif	// CONFIG2_AUDIO
