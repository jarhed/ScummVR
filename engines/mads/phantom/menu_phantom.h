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

#ifndef MADS_MENU_PHANTOM_H
#define MADS_MENU_PHANTOM_H

#include "common/scummsys.h"
#include "mads/game.h"
#include "mads/menu_views.h"
#include "mads/msurface.h"
#include "mads/phantom/dialogs_phantom.h"

namespace MADS {

class MADSEngine;

namespace Phantom {

enum MADSGameAction {
	START_GAME, RESUME_GAME, RESTORE_GAME, SHOW_INTRO, EXIT, CREDITS
};

class MainMenu: public MenuView {
	struct MenuItem {
		SpriteAsset *_sprites = nullptr;
		int _handle = -1;
		bool _active = false;
		int _status = 0;
	};
private:
	MenuItem _menuItems[7];
	int _menuItemIndex = -1;
	int _frameIndex = -1;
	uint32 _delayTimeout = 0;
	bool _skipFlag = false;

	/**
	 * Currently highlighted menu item
	 */
	int _highlightedIndex = -1;

	/**
	 * Flag for mouse button being pressed
	 */
	bool _buttonDown = false;

	/**
	 * Stores menu item selection
	 */
	int _selectedIndex = -1;

	/**
	 * Get the highlighted menu item under the cursor
	 */
	int getHighlightedItem(const Common::Point &pt);

	/**
	 * Un-highlight a currently highlighted item
	 */
	void unhighlightItem();

	/**
	 * Execute a given menuitem
	 */
	void handleAction(MADSGameAction action);

	/**
	 * Add a sprite slot for the current menuitem frame
	 */
	void addSpriteSlot();

protected:
	/**
	 * Display the menu
	 */
	void display() override;

	/**
	 * Handle the menu item animations
	 */
	void doFrame() override;

	/**
	 * Event handler
	 */
	bool onEvent(Common::Event &event) override;
public:
	MainMenu(MADSEngine *vm);

	~MainMenu() override;
};

class AdvertView : public EventTarget {
private:
	/**
	 * Engine reference
	 */
	MADSEngine *_vm;

	/**
	 * Signals when to close the dialog
	 */
	bool _breakFlag;
protected:
	/**
	* Event handler
	*/
	bool onEvent(Common::Event &event) override;
public:
	AdvertView(MADSEngine *vm);

	~AdvertView() override {}

	/**
	 * Show the dialog
	 */
	void show();
};

class PhantomAnimationView : public AnimationView {
protected:
	void scriptDone() override;
public:
	PhantomAnimationView(MADSEngine *vm) : AnimationView(vm) {}
};

class PhantomTextView : public TextView {
public:
	PhantomTextView(MADSEngine *vm) : TextView(vm) {}
};

} // namespace Phantom
} // namespace MADS

#endif
