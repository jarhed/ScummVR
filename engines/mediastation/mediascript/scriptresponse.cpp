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

#include "mediastation/mediascript/scriptresponse.h"
#include "mediastation/debugchannels.h"
#include "mediastation/mediastation.h"

namespace MediaStation {

ScriptResponse::ScriptResponse(Chunk &chunk) {
	_type = static_cast<EventType>(chunk.readTypedUint16());
	debugC(5, kDebugLoading, "%s: %s (%d)", __func__, eventTypeToStr(_type), static_cast<uint>(_type));

	_argumentValue = ScriptValue(&chunk);
	_code = new CodeChunk(chunk);
}

ScriptValue ScriptResponse::execute(uint actorId) {
	// TODO: The actorId is only passed in for debug visibility, there should be
	// a better way to handle that.
	Common::String actorName = g_engine->formatActorName(actorId, true);
	Common::String actorAndType = Common::String::format("%s (%s)", actorName.c_str(), eventTypeToStr(_type));
	Common::String argValue = Common::String::format("(%s)", _argumentValue.getDebugString().c_str());
	debugC(5, kDebugScript, "\n********** SCRIPT RESPONSE %s %s **********", actorAndType.c_str(), argValue.c_str());

	// The only argument that can be provided to a script response is the argument value.
	ScriptValue returnValue = _code->execute();

	debugC(5, kDebugScript, "********** END SCRIPT RESPONSE %s %s **********", actorAndType.c_str(), argValue.c_str());
	return returnValue;
}

ScriptResponse::~ScriptResponse() {
	delete _code;
	_code = nullptr;
}

} // End of namespace MediaStation
