/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "mediastation/audio.h"
#include "mediastation/debugchannels.h"
#include "mediastation/actors/sound.h"
#include "mediastation/mediastation.h"

namespace MediaStation {

SoundActor::~SoundActor() {
	unregisterWithStreamManager();
	if (_streamFeed != nullptr) {
		g_engine->getStreamFeedManager()->closeStreamFeed(_streamFeed);
		_streamFeed = nullptr;
	}
}

void SoundActor::readParameter(Chunk &chunk, ActorHeaderSectionType paramType) {
	switch (paramType) {
	case kActorHeaderActorId: {
		// We already have this actor's ID, so we will just verify it is the same
		// as the ID we have already read.
		uint32 duplicateActorId = chunk.readTypedUint16();
		if (duplicateActorId != _id) {
			warning("[%s] %s: Duplicate actor ID %s does not match", debugName(), __func__, g_engine->formatActorName(duplicateActorId).c_str());
		}
		break;
	}

	case kActorHeaderChannelIdent:
		_channelIdent = chunk.readTypedChannelIdent();
		registerWithStreamManager();
		break;

	case kActorHeaderDiscardAfterUse:
		_discardAfterUse = static_cast<bool>(chunk.readTypedByte());
		break;

	case kActorHeaderSoundInfo:
		_sequence.readParameters(chunk);
		break;

	case kActorHeaderCachingEnabled:
		// This controls some caching behavior in the original, but since that is not currently
		// implemented here, just throw it away.
		chunk.readTypedByte();
		break;

	case kActorHeaderInstallType:
		// In the original, this controls behavior if the files are NOT installed. But since
		// the "installation" is just copying from the CD-ROM, we can treat the game as always
		// installed. So just throw away this value.
		chunk.readTypedByte();
		break;

	default:
		Actor::readParameter(chunk, paramType);
	}
}

void SoundActor::process() {
	if (_playState != kSoundPlaying) {
		return;
	}

	processTimeScriptResponses();
	if (!_sequence.isActive()) {
		_playState = kSoundStopped;
		_sequence.stop();
		runScriptResponseIfExists(kSoundEndEvent);
	}
}
void SoundActor::readChunk(Chunk &chunk) {
	_isLoadedFromChunk = true;
	_sequence.readChunk(chunk);
}

ScriptValue SoundActor::callMethod(BuiltInMethod methodId, Common::Array<ScriptValue> &args) {
	ScriptValue returnValue;

	switch (methodId) {
	case kSpatialShowMethod:
		// WORKAROUND: No-op to avoid triggering error on Dalmatians
		// timer_6c06_AnsweringMachine, which calls SpatialShow on a sound.
		// Since the engine is currently flagging errors on unimplemented
		// methods for easier debugging, a no-op is used here to avoid the error.
		ARGCOUNTCHECK(0);
		break;

	case kTimePlayMethod:
		ARGCOUNTCHECK(0);
		start();
		break;

	case kTimeStopMethod:
		ARGCOUNTCHECK(0);
		stop();
		break;

	case kPauseMethod:
		ARGCOUNTCHECK(0);
		pause();
		break;

	case kResumeMethod: {
		ARGCOUNTRANGE(0, 1);
		bool shouldRestart = false;
		if (args.size() == 1) {
			shouldRestart = args[0].asBool();
		}
		resume(shouldRestart);
		break;
	}

	case kIsPlayingMethod:
		returnValue.setToBool(_playState == kSoundPlaying || _playState == kSoundPaused);
		break;

	case kIsPausedMethod:
		returnValue.setToBool(_playState == kSoundPaused);
		break;

	default:
		returnValue = Actor::callMethod(methodId, args);
	}
	return returnValue;
}

void SoundActor::start() {
	if (_loadIsComplete) {
		if (_playState == kSoundPlaying || _playState == kSoundPaused) {
			stop();
		}

		openStream();
		_playState = kSoundPlaying;
		_startTime = g_system->getMillis();
		_lastProcessedTime = 0;
		_sequence.play();
		runScriptResponseIfExists(kSoundBeginEvent);
	} else {
		warning("[%s] %s: Attempted to play sound before it was loaded", debugName(), __func__);
	}
}

void SoundActor::stop() {
	if (_playState == kSoundPlaying || _playState == kSoundPaused) {
		_playState = kSoundStopped;
		_sequence.stop();
		runScriptResponseIfExists(kSoundStoppedEvent);
	}
}

void SoundActor::pause() {
	if (_playState == kSoundPlaying) {
		_sequence.pause();
		_playState = kSoundPaused;
		// There don't seem to be script events to trigger in this instance.
	}
}

void SoundActor::resume(bool restart) {
	if (_playState == kSoundPaused) {
		_sequence.resume();
	} else if (restart) {
		start();
	}
	// There don't seem to be script events to trigger in this instance.
}

void SoundActor::openStream() {
	if (_streamFeed == nullptr && !_isLoadedFromChunk) {
		_streamFeed = g_engine->getStreamFeedManager()->openStreamFeed(_id);
		_streamFeed->readData();
	}
}

} // End of namespace MediaStation
