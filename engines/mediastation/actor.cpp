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

#include "common/util.h"

#include "mediastation/actor.h"
#include "mediastation/actors/camera.h"
#include "mediastation/actors/stage.h"
#include "mediastation/debugchannels.h"
#include "mediastation/mediascript/scriptconstants.h"
#include "mediastation/mediastation.h"

namespace MediaStation {

const char *actorTypeToStr(ActorType type) {
	switch (type) {
	case kActorTypeEmpty:
		return "Empty";
	case kActorTypeScreen:
		return "Screen";
	case kActorTypeStage:
		return "Stage";
	case kActorTypePath:
		return "Path";
	case kActorTypeSound:
		return "Sound";
	case kActorTypeTimer:
		return "Timer";
	case kActorTypeImage:
		return "Image";
	case kActorTypeHotspot:
		return "Hotspot";
	case kActorTypeCursor:
		return "Cursor";
	case kActorTypeSprite:
		return "Sprite";
	case kActorTypeLKZazu:
		return "LKZazu";
	case kActorTypeLKConstellations:
		return "LKConstellations";
	case kActorTypeDocument:
		return "Document";
	case kActorTypeImageSet:
		return "ImageSet";
	case kActorTypeMovie:
		return "Movie";
	case kActorTypeStreamMovieProxy:
		return "StreamMovieProxy";
	case kActorTypePalette:
		return "Palette";
	case kActorTypePrinter:
		return "Printer";
	case kActorTypeText:
		return "Text";
	case kActorTypeFont:
		return "Font";
	case kActorTypeCamera:
		return "Camera";
	case kActorTypeCanvas:
		return "Canvas";
	case kActorTypeXsnd:
		return "Xsnd";
	case kActorTypeXsndMidi:
		return "XsndMidi";
	case kActorTypeRecorder:
		return "Recorder";
	case kActorTypeFunction:
		return "Function";
	default:
		return "UNKNOWN";
	}
}

void Actor::setId(uint id) {
	_id = id;
	updateDebugName();
}

void Actor::updateDebugName() {
	_debugName = g_engine->formatActorName(this);
}

const char *Actor::debugName() const {
	return _debugName.c_str();
}

Actor::~Actor() {
	for (auto it = _scriptResponses.begin(); it != _scriptResponses.end(); ++it) {
		Common::Array<ScriptResponse *> &responsesForType = it->_value;
		for (ScriptResponse *response : responsesForType) {
			delete response;
		}
		responsesForType.clear();
	}
	_scriptResponses.clear();
}

void Actor::initFromParameterStream(Chunk &chunk) {
	ActorHeaderSectionType paramType = kActorHeaderEmptySection;
	while (true) {
		paramType = static_cast<ActorHeaderSectionType>(chunk.readTypedUint16());
		if (paramType == 0) {
			break;
		} else {
			readParameter(chunk, paramType);
		}
	}
}

void Actor::readParameter(Chunk &chunk, ActorHeaderSectionType paramType) {
	switch (paramType) {
	case kActorHeaderScriptResponse: {
		ScriptResponse *scriptResponse = new ScriptResponse(chunk);
		Common::Array<ScriptResponse *> &scriptResponsesForType = _scriptResponses.getOrCreateVal(scriptResponse->_type);

		// This is not a hashmap because we don't want to have to hash ScriptValues.
		for (ScriptResponse *existingScriptResponse : scriptResponsesForType) {
			if (existingScriptResponse->_argumentValue == scriptResponse->_argumentValue) {
				error("[%s] %s: Script response for %s (%s) already exists", debugName(), __func__,
					eventTypeToStr(scriptResponse->_type), scriptResponse->_argumentValue.getDebugString().c_str());
			}
		}
		scriptResponsesForType.push_back(scriptResponse);
		break;
	}

	default:
		error("[%s] %s: Got unimplemented actor parameter 0x%x", debugName(), __func__, static_cast<uint>(paramType));
	}
}

void Actor::loadIsComplete() {
	if (_loadIsComplete) {
		warning("[%s] %s: Already loaded", debugName(), __func__);
	}
	_loadIsComplete = true;
}

ScriptValue Actor::callMethod(BuiltInMethod methodId, Common::Array<ScriptValue> &args) {
	warning("[%s] %s: Got unimplemented method call 0x%x (%s)",
		debugName(), __func__, static_cast<uint>(methodId), builtInMethodToStr(methodId));
	return ScriptValue();
}

void Actor::processTimeScriptResponses() {
	// TODO: Replace with a queue.
	uint currentTime = g_system->getMillis();
	const Common::Array<ScriptResponse *> &_timeResponses = _scriptResponses.getValOrDefault(kTimerEvent);
	for (ScriptResponse *timeEvent : _timeResponses) {
		// Indeed float, not time.
		double timeEventInFractionalSeconds = timeEvent->_argumentValue.asFloat();
		uint timeEventInMilliseconds = timeEventInFractionalSeconds * 1000;
		bool timeEventAlreadyProcessed = timeEventInMilliseconds < _lastProcessedTime;
		bool timeEventNeedsToBeProcessed = timeEventInMilliseconds < currentTime - _startTime;
		if (!timeEventAlreadyProcessed && timeEventNeedsToBeProcessed) {
			debugC(5, kDebugScript, "%s: Running On Time response for time %d ms (lastProcessedTime: %d, currentTime: %d)",
				__func__, timeEventInMilliseconds, _lastProcessedTime, currentTime);
			timeEvent->execute(_id);
		}
	}
	_lastProcessedTime = currentTime - _startTime;
}

void Actor::runScriptResponseIfExists(EventType eventType, const ScriptValue &arg) {
	const Common::Array<ScriptResponse *> &scriptResponses = _scriptResponses.getValOrDefault(eventType);
	for (ScriptResponse *scriptResponse : scriptResponses) {
		const ScriptValue &argToCheck = scriptResponse->_argumentValue;

		if (arg.getType() != argToCheck.getType()) {
			warning("[%s] %s: Got script response arg type %s, expected %s", debugName(), __func__,
				scriptValueTypeToStr(arg.getType()), scriptValueTypeToStr(argToCheck.getType()));
			continue;
		}

		if (arg == argToCheck) {
			debugC(5, kDebugScript, "[%s] %s: Executing response for event type %s", debugName(), __func__, eventTypeToStr(eventType));
			scriptResponse->execute(_id);
			return;
		}
	}

	debugC(5, kDebugScript, "[%s] %s: No script response for event type %s", debugName(), __func__, eventTypeToStr(eventType));
}

void Actor::runScriptResponseIfExists(EventType eventType) {
	ScriptValue scriptValue;
	runScriptResponseIfExists(eventType, scriptValue);
}

SpatialEntity::~SpatialEntity() {
	if (_parentStage != nullptr) {
		_parentStage->removeChildSpatialEntity(this);
	}
}

ScriptValue SpatialEntity::callMethod(BuiltInMethod methodId, Common::Array<ScriptValue> &args) {
	ScriptValue returnValue;
	switch (methodId) {
	case kSpatialMoveToMethod: {
		ARGCOUNTCHECK(2);
		int16 x = static_cast<int16>(args[0].asFloat());
		int16 y = static_cast<int16>(args[1].asFloat());
		moveTo(x, y);
		break;
	}

	case kSpatialMoveToByOffsetMethod: {
		ARGCOUNTCHECK(2);
		int16 dx = static_cast<int16>(args[0].asFloat());
		int16 dy = static_cast<int16>(args[1].asFloat());
		int16 newX = _boundingBox.left + dx;
		int16 newY = _boundingBox.top + dy;
		moveTo(newX, newY);
		break;
	}

	case kSpatialZMoveToMethod: {
		ARGCOUNTCHECK(1);
		int zIndex = static_cast<int>(args[0].asFloat());
		setZIndex(zIndex);
		break;
	}

	case kSpatialCenterMoveToMethod: {
		ARGCOUNTCHECK(2);
		int16 x = static_cast<int16>(args[0].asFloat());
		int16 y = static_cast<int16>(args[1].asFloat());
		moveToCentered(x, y);
		break;
	}

	case kGetLeftXMethod:
		ARGCOUNTCHECK(0);
		returnValue.setToFloat(_boundingBox.left);
		break;

	case kGetTopYMethod:
		ARGCOUNTCHECK(0);
		returnValue.setToFloat(_boundingBox.top);
		break;

	case kGetWidthMethod:
		ARGCOUNTCHECK(0);
		returnValue.setToFloat(_boundingBox.width());
		break;

	case kGetHeightMethod:
		ARGCOUNTCHECK(0);
		returnValue.setToFloat(_boundingBox.height());
		break;

	case kGetCenterXMethod: {
		ARGCOUNTCHECK(0);
		int centerX = _boundingBox.left + (_boundingBox.width() / 2);
		returnValue.setToFloat(centerX);
		break;
	}

	case kGetCenterYMethod: {
		ARGCOUNTCHECK(0);
		int centerY = _boundingBox.top + (_boundingBox.height() / 2);
		returnValue.setToFloat(centerY);
		break;
	}

	case kGetZCoordinateMethod:
		ARGCOUNTCHECK(0);
		returnValue.setToFloat(_zIndex);
		break;

	case kIsPointInsideMethod: {
		ARGCOUNTCHECK(2);
		int16 xToCheck = static_cast<int16>(args[0].asFloat());
		int16 yToCheck = static_cast<int16>(args[1].asFloat());
		Common::Point pointToCheck(xToCheck, yToCheck);
		bool pointIsInside = getBbox().contains(pointToCheck);
		returnValue.setToBool(pointIsInside);
		break;
	}

	case kSetDissolveFactorMethod: {
		ARGCOUNTCHECK(1);
		double dissolveFactor = args[0].asFloat();
		setDissolveFactor(dissolveFactor);
		break;
	}

	case kGetMouseXOffsetMethod: {
		Common::Point mouseOffset;
		currentMousePosition(mouseOffset);
		mouseOffset -= _originalBoundingBox.origin();
		returnValue.setToFloat(static_cast<double>(mouseOffset.x));
		break;
	}

	case kGetMouseYOffsetMethod: {
		Common::Point mouseOffset;
		currentMousePosition(mouseOffset);
		mouseOffset -= _originalBoundingBox.origin();
		returnValue.setToFloat(static_cast<double>(mouseOffset.y));
		break;
	}

	case kIsVisibleMethod:
		ARGCOUNTCHECK(0);
		returnValue.setToBool(isVisible());
		break;

	case kSetMousePositionMethod: {
		ARGCOUNTCHECK(2);
		int16 x = static_cast<int16>(args[0].asFloat());
		int16 y = static_cast<int16>(args[1].asFloat());
		setMousePosition(x, y);
		break;
	}

	case kGetXScaleMethod1:
	case kGetXScaleMethod2:
		ARGCOUNTCHECK(0);
		returnValue.setToFloat(_scaleX);
		break;

	case kSetScaleMethod:
		ARGCOUNTCHECK(1);
		invalidateLocalBounds();
		_scaleX = _scaleY = args[0].asFloat();
		invalidateLocalBounds();
		break;

	case kSetXScaleMethod:
		ARGCOUNTCHECK(1);
		invalidateLocalBounds();
		_scaleX = args[0].asFloat();
		invalidateLocalBounds();
		break;

	case kGetYScaleMethod:
		ARGCOUNTCHECK(0);
		returnValue.setToFloat(_scaleY);
		break;

	case kSetYScaleMethod:
		ARGCOUNTCHECK(1);
		invalidateLocalBounds();
		_scaleY = args[0].asFloat();
		invalidateLocalBounds();
		break;

	default:
		Actor::callMethod(methodId, args);
	}
	return returnValue;
}

void SpatialEntity::readParameter(Chunk &chunk, ActorHeaderSectionType paramType) {
	switch (paramType) {
	case kActorHeaderBoundingBox:
		_originalBoundingBox = chunk.readTypedRect();
		setAdjustedBounds(kWrapNone);
		break;

	case kActorHeaderDissolveFactor:
		_dissolveFactor = chunk.readTypedDouble();
		break;

	case kActorHeaderZIndex:
		_zIndex = chunk.readTypedGraphicUnit();
		break;

	case kActorHeaderTransparency:
		_hasTransparency = static_cast<bool>(chunk.readTypedByte());
		break;

	case kActorHeaderChildActorId:
		_stageId = chunk.readTypedUint16();
		break;

	case kActorHeaderScaleXAndY:
		_scaleX = _scaleY = chunk.readTypedDouble();
		break;

	case kActorHeaderScaleX:
		_scaleX = chunk.readTypedDouble();
		break;

	case kActorHeaderScaleY:
		_scaleY = chunk.readTypedDouble();
		break;

	default:
		Actor::readParameter(chunk, paramType);
	}
}

void SpatialEntity::loadIsComplete() {
	Actor::loadIsComplete();
	if (_stageId != 0) {
		StageActor *pendingParentStage = static_cast<StageActor *>(g_engine->getActorByIdAndType(_stageId, kActorTypeStage));
		pendingParentStage->addChildSpatialEntity(this);
	}
}

void SpatialEntity::currentMousePosition(Common::Point &point) {
	if (_parentStage != nullptr) {
		_parentStage->currentMousePosition(point);
	}
}

void SpatialEntity::invalidateMouse() {
	// TODO: Invalidate the mouse properly when we have custom events.
	// For now, we simulate the mouse update event with a mouse moved event.
	Common::Event mouseEvent;
	mouseEvent.type = Common::EVENT_MOUSEMOVE;
	mouseEvent.mouse = g_system->getEventManager()->getMousePos();
	g_system->getEventManager()->pushEvent(mouseEvent);
}

void SpatialEntity::moveTo(int16 x, int16 y) {
	Common::Point dest(x, y);
	debugC(3, kDebugGraphics, "[%s] %s: (%d, %d) -> (%d, %d)", debugName(), __func__,
		_originalBoundingBox.origin().x, _originalBoundingBox.origin().y, x, y);

	if (dest == _boundingBox.origin()) {
		// We aren't actually moving anywhere.
		return;
	}

	if (isVisible()) {
		invalidateLocalBounds();
	}
	_originalBoundingBox.moveTo(dest);
	setAdjustedBounds(kWrapNone);
	if (isVisible()) {
		invalidateLocalBounds();
	}
	if (interactsWithMouse()) {
		invalidateMouse();
	}
}

void SpatialEntity::moveToCentered(int16 x, int16 y) {
	int16 targetX = x - (_boundingBox.width() / 2);
	int16 targetY = y - (_boundingBox.height() / 2);
	debugC(3, kDebugGraphics, "[%s] %s: (%d, %d)", debugName(), __func__, targetX, targetY);
	moveTo(targetX, targetY);
}

void SpatialEntity::setBounds(const Common::Rect &bounds) {
	if (_boundingBox == bounds) {
		// We aren't actually moving anywhere.
		return;
	}

	if (isVisible()) {
		invalidateLocalBounds();
	}
	_originalBoundingBox = bounds;
	setAdjustedBounds(kWrapNone);
	if (isVisible()) {
		invalidateLocalBounds();
	}
	if (interactsWithMouse()) {
		invalidateMouse();
	}
}

void SpatialEntity::setZIndex(int zIndex) {
	if (_zIndex == zIndex) {
		// We aren't actually moving anywhere.
		return;
	}

	_zIndex = zIndex;
	invalidateLocalZIndex();
	if (interactsWithMouse()) {
		invalidateMouse();
	}
}

void SpatialEntity::setMousePosition(int16 x, int16 y) {
	if (_parentStage) {
		_parentStage->setMousePosition(x, y);
	}
}

void SpatialEntity::setDissolveFactor(double dissolveFactor) {
	dissolveFactor = CLIP(dissolveFactor, 0.0, 1.0);
	if (dissolveFactor != _dissolveFactor) {
		_dissolveFactor = dissolveFactor;
		invalidateLocalBounds();
	}
}

void SpatialEntity::invalidateLocalBounds() {
	if (_parentStage != nullptr) {
		_parentStage->setAdjustedBounds(kWrapNone);
		_parentStage->invalidateRect(getBbox());
	} else {
		warning("[%s] %s: No parent stage", debugName(), __func__);
	}
}

void SpatialEntity::invalidateLocalZIndex() {
	if (_parentStage != nullptr) {
		_parentStage->invalidateZIndexOf(this);
	}
}

void SpatialEntity::setAdjustedBounds(CylindricalWrapMode alignmentMode) {
	_boundingBox = _originalBoundingBox;
	if (_parentStage == nullptr) {
		return;
	}

	Common::Point offset(0, 0);
	Common::Point stageExtent = _parentStage->extent();
	switch (alignmentMode) {
	case kWrapRight: {
		offset.x = stageExtent.x;
		offset.y = 0;
		break;
	}

	case kWrapLeft: {
		offset.x = -stageExtent.x;
		offset.y = 0;
		break;
	}

	case kWrapBottom: {
		offset.x = 0;
		offset.y = stageExtent.y;
		break;
	}

	case kWrapLeftTop: {
		offset.x = 0;
		offset.y = -stageExtent.y;
		break;
	}

	case kWrapTop: {
		offset.x = stageExtent.x;
		offset.y = stageExtent.y;
		break;
	}

	case kWrapRightBottom: {
		offset.x = -stageExtent.x;
		offset.y = -stageExtent.y;
		break;
	}

	case kWrapRightTop: {
		offset.x = -stageExtent.x;
		offset.y = stageExtent.y;
		break;
	}

	case kWrapLeftBottom: {
		offset.x = stageExtent.x;
		offset.y = -stageExtent.y;
		break;
	}

	case kWrapNone:
	default:
		// No offset adjustment.
		break;
	}

	if (alignmentMode != kWrapNone) {
		// TODO: Implement this once we have a title that actually uses it.
		warning("[%s] %s: Wrapping mode %d not handled yet: (%d, %d, %d, %d) -= (%d, %d)", debugName(),  __func__,
			static_cast<uint>(alignmentMode), PRINT_RECT(_boundingBox), offset.x, offset.y);
	}

	if (_scaleX != 0.0 || _scaleY != 0.0) {
		// TODO: Implement this once we have a title that actually uses it.
		warning("[%s] %s: Scale not handled yet (scaleX: %f, scaleY: %f)", debugName(), __func__, _scaleX, _scaleY);
	}
}

} // End of namespace MediaStation
