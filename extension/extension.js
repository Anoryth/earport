/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * EarPort Shell Extension
 */

import Meta from 'gi://Meta';
import Shell from 'gi://Shell';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

import {EarPortIndicator} from './earportIndicator.js';

/* Extension class */
export default class EarPortExtension extends Extension {
    enable() {
        this._indicator = new EarPortIndicator(this);

        Main.panel.statusArea.quickSettings.addExternalIndicator(this._indicator);

        /* Keyboard shortcut to cycle noise control modes */
        Main.wm.addKeybinding(
            'cycle-noise-mode-shortcut',
            this.getSettings(),
            Meta.KeyBindingFlags.IGNORE_AUTOREPEAT,
            Shell.ActionMode.NORMAL | Shell.ActionMode.OVERVIEW,
            () => this._indicator?.cycleNoiseControlMode()
        );
    }

    disable() {
        Main.wm.removeKeybinding('cycle-noise-mode-shortcut');

        if (this._indicator) {
            this._indicator.destroy();
            this._indicator = null;
        }
    }
}
