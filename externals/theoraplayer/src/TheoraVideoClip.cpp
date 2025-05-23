/************************************************************************************
This source file is part of the Theora Video Playback Library
For latest info, see http://libtheoraplayer.googlecode.com
*************************************************************************************
Copyright (c) 2008-2014 Kresimir Spes (kspes@cateia.com)
This program is free software; you can redistribute it and/or modify it under
the terms of the BSD license: http://opensource.org/licenses/BSD-3-Clause
*************************************************************************************/
#include <math.h>
#include "TheoraVideoClip.h"
#include "TheoraVideoManager.h"
#include "TheoraVideoFrame.h"
#include "TheoraFrameQueue.h"
#include "TheoraAudioInterface.h"
#include "TheoraTimer.h"
#include "TheoraDataSource.h"
#include "TheoraUtil.h"
#include "TheoraException.h"

TheoraVideoClip::TheoraVideoClip(TheoraDataSource* data_source,
								 TheoraOutputMode output_mode,
								 int nPrecachedFrames,
								 bool usePower2Stride):
	mFrameQueue(NULL),
	mAudioInterface(NULL),
	mStream(NULL),
	mAssignedWorkerThread(NULL),
	mUseAlpha(false),
	mWaitingForCache(false),
	mNumDroppedFrames(0),
	mNumDisplayedFrames(0),
	mNumPrecachedFrames(nPrecachedFrames),
	mThreadAccessCount(0),
	mSeekFrame(-1),
	mDuration(-1),
	mFrameDuration(0),
	mPriority(1),
	mName(data_source->repr()),
	mWidth(0),
	mHeight(0),
	mStride(usePower2Stride),
	mNumFrames(-1),
	mFPS(1),
	mSubFrameWidth(0),
	mSubFrameHeight(0),
	mSubFrameOffsetX(0),
	mSubFrameOffsetY(0),
	mAudioGain(1.0f),
	mOutputMode(TH_UNDEFINED),
	mRequestedOutputMode(output_mode),
	mFirstFrameDisplayed(false),
	mAutoRestart(false),
	mEndOfFile(false),
	mRestarted(false),
	mIteration(0),
	mPlaybackIteration(0),
	mAudioMutex(NULL)
{
	mThreadAccessMutex = new TheoraMutex();
	mTimer = mDefaultTimer = new TheoraTimer();

	setOutputMode(output_mode);
}

TheoraVideoClip::~TheoraVideoClip()
{
	// wait untill a worker thread is done decoding the frame
	TheoraMutex::ScopeLock lock(mThreadAccessMutex);

	delete mDefaultTimer;

	if (mStream) delete mStream;

	if (mFrameQueue) delete mFrameQueue;

	if (mAudioInterface)
	{
		TheoraMutex::ScopeLock audioMutexLock(mAudioMutex); // ensure a thread isn't using this mutex
		delete mAudioInterface; // notify audio interface it's time to call it a day
		audioMutexLock.release();
		delete mAudioMutex;
	}
	
	lock.release();
	delete mThreadAccessMutex;
}

TheoraTimer* TheoraVideoClip::getTimer()
{
	return mTimer;
}

void TheoraVideoClip::setTimer(TheoraTimer* timer)
{
	if (!timer) mTimer = mDefaultTimer;
	else mTimer = timer;
}

void TheoraVideoClip::resetFrameQueue()
{
	mFrameQueue->clear();
	mPlaybackIteration = mIteration = 0;
}

void TheoraVideoClip::restart()
{
	mEndOfFile = true; //temp, to prevent threads to decode while restarting
	TheoraMutex::ScopeLock lock(mThreadAccessMutex);
	_restart();
	mTimer->seek(0);
	mFirstFrameDisplayed = false;
	resetFrameQueue();
	mEndOfFile = false;
	mRestarted = false;
	mSeekFrame = -1;
	lock.release();
}

void TheoraVideoClip::update(float timeDelta)
{
	if (mTimer->isPaused())
	{
		mTimer->update(0); // update timer in case there is some code that needs to execute each frame
		return;
	}
	float time = mTimer->getTime(), speed = mTimer->getSpeed();
	if (time + timeDelta * speed >= mDuration)
	{
		if (mAutoRestart && mRestarted)
		{
			float seekTime = time + timeDelta * speed;
			for (;seekTime >= mDuration;)
			{
				seekTime -= mDuration;
				++mPlaybackIteration;
			}

			mTimer->seek(seekTime);
		}
		else
		{
			if (time != mDuration)
			{
				mTimer->update((mDuration - time) / speed);
			}
		}
	}
	else
	{
		mTimer->update(timeDelta);
	}
}

float TheoraVideoClip::updateToNextFrame()
{
	TheoraVideoFrame* f = mFrameQueue->getFirstAvailableFrame();
	if (!f) return 0;

	float time = f->mTimeToDisplay - mTimer->getTime();
	update(time);
	return time;
}

TheoraFrameQueue* TheoraVideoClip::getFrameQueue()
{
	return mFrameQueue;
}

void TheoraVideoClip::popFrame()
{
	++mNumDisplayedFrames;
	
	// after transfering frame data to the texture, free the frame
	// so it can be used again
	if (!mFirstFrameDisplayed)
	{
		TheoraMutex::ScopeLock lock(mFrameQueue->getMutex());
		mFrameQueue->_pop(1);
		mFirstFrameDisplayed = true;
		lock.release();
	}
	else
	{
		mFrameQueue->pop();
	}
}

int TheoraVideoClip::getWidth()
{
	return mUseAlpha ? mWidth / 2 : mWidth;
}

int TheoraVideoClip::getHeight()
{
	return mHeight;
}

int TheoraVideoClip::getSubFrameWidth()
{
	return mUseAlpha ? mWidth / 2 : mSubFrameWidth;
}

int TheoraVideoClip::getSubFrameHeight()
{
	return mUseAlpha ? mHeight : mSubFrameHeight;
}

int TheoraVideoClip::getSubFrameOffsetX()
{
	return mUseAlpha ? 0 : mSubFrameOffsetX;
}

int TheoraVideoClip::getSubFrameOffsetY()
{
	return mUseAlpha ? 0 : mSubFrameOffsetY;
}

float TheoraVideoClip::getAbsPlaybackTime()
{
	return mTimer->getTime() + mPlaybackIteration * mDuration;
}

int TheoraVideoClip::discardOutdatedFrames(float absTime)
{
	int nReady = mFrameQueue->_getReadyCount();
	// only drop frames if you have more frames to show. otherwise even the late frame will do..
	if (nReady == 1) return 0;
	float time = absTime;

	int nPop = 0;
	TheoraVideoFrame* frame;
	float timeToDisplay;
	
	std::list<TheoraVideoFrame*>& queue = mFrameQueue->_getFrameQueue();
	foreach_l (TheoraVideoFrame*, queue)
	{
		frame = *it;
		if (!frame->mReady) break;
		timeToDisplay = frame->mTimeToDisplay + frame->mIteration * mDuration;
		if (time > timeToDisplay + mFrameDuration)
		{
			++nPop;
			if (nReady - nPop == 1) break; // always leave at least one in the queue
		}
		else break;
	}
	
	if (nPop > 0)
	{
#ifdef _DEBUG_FRAMEDROP
		std::string log = getName() + ": dropped frame ";
	
		int i = nPop;
		foreach_l (TheoraVideoFrame*, queue)
		{
			log += str((int) (*it)->getFrameNumber());
			if (i-- > 1)
			{
				log += ", ";
			}
			else break;
		}
		th_writelog(log);
#endif
		mNumDroppedFrames += nPop;
		mFrameQueue->_pop(nPop);
	}
	
	return nPop;
}

TheoraVideoFrame* TheoraVideoClip::getNextFrame()
{
	TheoraVideoFrame* frame;
	// if we are about to seek, then the current frame queue is invalidated
	// (will be cleared when a worker thread does the actual seek)
	if (mSeekFrame != -1) return NULL;
	
	TheoraMutex::ScopeLock lock(mFrameQueue->getMutex());
	float time = getAbsPlaybackTime();
	discardOutdatedFrames(time);
	
	frame = mFrameQueue->_getFirstAvailableFrame();
	if (frame != NULL)
	{
		if (frame->mTimeToDisplay + frame->mIteration * mDuration > time && mFirstFrameDisplayed)
		{
			frame = NULL; // frame is ready but it's not yet time to display it, except when we haven't displayed any frames yet
		}
	}
	
	lock.release();
	return frame;
}

std::string TheoraVideoClip::getName()
{
	return mName;
}

bool TheoraVideoClip::isBusy()
{
	return mAssignedWorkerThread || mOutputMode != mRequestedOutputMode;
}

TheoraOutputMode TheoraVideoClip::getOutputMode()
{
	return mOutputMode;
}

void TheoraVideoClip::setOutputMode(TheoraOutputMode mode)
{
	if (mode == TH_UNDEFINED)
	{
		throw TheoraGenericException("Invalid output mode: TH_UNDEFINED for video: " + mName);
	}
	if (mOutputMode == mode) return;
	mRequestedOutputMode = mode;
	mUseAlpha = (mode == TH_RGBA   ||
				 mode == TH_ARGB   ||
				 mode == TH_BGRA   ||
				 mode == TH_ABGR   ||
				 mode == TH_GREY3A ||
				 mode == TH_AGREY3 ||
				 mode == TH_YUVA   ||
				 mode == TH_AYUV);
	if (mAssignedWorkerThread)
	{
		TheoraMutex::ScopeLock lock(mThreadAccessMutex);
		// discard current frames and recreate them
		mFrameQueue->setSize(mFrameQueue->getSize());
		lock.release();

	}
	mOutputMode = mRequestedOutputMode;
}

float TheoraVideoClip::getTimePosition()
{
	return mTimer->getTime();
}

int TheoraVideoClip::getNumPrecachedFrames()
{
	return mFrameQueue->getSize();
}

void TheoraVideoClip::setNumPrecachedFrames(int n)
{
	if (mFrameQueue->getSize() != n)
		mFrameQueue->setSize(n);
}

int TheoraVideoClip::_getNumReadyFrames()
{
	if (mSeekFrame != -1) return 0;
	return mFrameQueue->_getReadyCount();
}

int TheoraVideoClip::getNumReadyFrames()
{
	if (mSeekFrame != -1) return 0; // we are about to seek, consider frame queue empty even though it will be emptied upon seek
	return mFrameQueue->getReadyCount();
}

float TheoraVideoClip::getDuration()
{
	return mDuration;
}

float TheoraVideoClip::getFPS()
{
	return mFPS;
}

void TheoraVideoClip::play()
{
	mTimer->play();
}

void TheoraVideoClip::pause()
{
	mTimer->pause();
}

bool TheoraVideoClip::isPaused()
{
	return mTimer->isPaused();
}

bool TheoraVideoClip::isDone()
{
	if (mEndOfFile)
	{
		TheoraVideoFrame* frame = mFrameQueue->getFirstAvailableFrame();
		if (frame == NULL || frame->mTimeToDisplay >= mDuration) // in some cases, there could be a diference between the reported video duration and timestamp on the last frame(s)
		{
			return true;
		}

	}
	return false;
}

void TheoraVideoClip::stop()
{
	pause();
	resetFrameQueue();
	mFirstFrameDisplayed = false;
	seek(0);
}

void TheoraVideoClip::setPlaybackSpeed(float speed)
{
	mTimer->setSpeed(speed);
}

float TheoraVideoClip::getPlaybackSpeed()
{
	return mTimer->getSpeed();
}

void TheoraVideoClip::seek(float time)
{
	seekToFrame((int) (time * getFPS()));
}

void TheoraVideoClip::seekToFrame(int frame)
{
	if      (frame < 0)          mSeekFrame = 0;
	else if (frame > mNumFrames) mSeekFrame = mNumFrames;
	else                         mSeekFrame = frame;

	mFirstFrameDisplayed = false;
	mEndOfFile = false;
}

float TheoraVideoClip::waitForCache(float desired_cache_factor, float max_wait_time)
{
	mWaitingForCache = true;
	bool paused = mTimer->isPaused();
	if (!paused) mTimer->pause();
	int elapsed = 0, nReady = 0, frameQueueSize = getNumPrecachedFrames();
    if (mNumFrames < frameQueueSize)
    {
        frameQueueSize = mNumFrames;
    }
	int desired_num_precached_frames = (int) ceil(desired_cache_factor * frameQueueSize);
	for (;;)
	{
		nReady = getNumReadyFrames();
		if (nReady >= desired_num_precached_frames) break;
		_psleep(10);
		elapsed += 10;
		if (elapsed >= max_wait_time * 1000) break;
	}
	if (!paused) mTimer->play();
	mWaitingForCache = false;

	return (float) nReady / (float) frameQueueSize;
}

float TheoraVideoClip::getPriority()
{
	return mPriority;
}

void TheoraVideoClip::setPriority(float priority)
{
	mPriority = priority;
}

float TheoraVideoClip::getPriorityIndex()
{
	float priority = (float) getNumReadyFrames();
	if (mTimer->isPaused()) priority += getNumPrecachedFrames() / 2;
	
	return priority;
}

void TheoraVideoClip::setAudioInterface(TheoraAudioInterface* iface)
{
	mAudioInterface = iface;
	if (iface && !mAudioMutex) mAudioMutex = new TheoraMutex;
	if (!iface && mAudioMutex)
	{
		delete mAudioMutex;
		mAudioMutex = NULL;
	}
}

TheoraAudioInterface* TheoraVideoClip::getAudioInterface()
{
	return mAudioInterface;
}

void TheoraVideoClip::setAudioGain(float gain)
{
	if (gain > 1) mAudioGain=1;
	if (gain < 0) mAudioGain=0;
	else          mAudioGain=gain;
}

float TheoraVideoClip::getAudioGain()
{
	return mAudioGain;
}

void TheoraVideoClip::setAutoRestart(bool value)
{
	mAutoRestart = value;
	if (value) mEndOfFile = false;
}
