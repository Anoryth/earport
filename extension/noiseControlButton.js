/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Noise control mode button of the Quick Settings menu
 */

import Atk from 'gi://Atk';
import Clutter from 'gi://Clutter';
import GObject from 'gi://GObject';
import St from 'gi://St';

/* Noise control mode button */
export const NoiseControlButton = GObject.registerClass(
class NoiseControlButton extends St.Button {
    constructor(mode, label, accessibleName, gicon) {
        super({
            style_class: 'earport-nc-button',
            can_focus: true,
            accessible_name: accessibleName,
            accessible_role: Atk.Role.TOGGLE_BUTTON,
            child: new St.BoxLayout({
                vertical: true,
                x_align: Clutter.ActorAlign.CENTER,
            }),
        });

        this._mode = mode;

        const icon = new St.Icon({
            gicon,
            icon_size: 18,
            style_class: 'earport-nc-icon',
        });

        const labelWidget = new St.Label({
            text: label,
            style_class: 'earport-nc-label',
            x_align: Clutter.ActorAlign.CENTER,
        });

        this.child.add_child(icon);
        this.child.add_child(labelWidget);
    }

    get mode() {
        return this._mode;
    }

    /* Checked state is exposed to screen readers and styled with :checked */
    setActive(active) {
        this.checked = active;
    }
});
