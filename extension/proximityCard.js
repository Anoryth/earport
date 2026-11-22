/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Proximity card, as on an iPhone: opening the AirPods' case near this
 * computer, while they are not connected here, shows their batteries and a
 * Connect button. The idea comes from the OscarHickman/earport fork.
 *
 * The daemon scans in bursts (every 10 s while the computer is in use), so
 * the card comes up to 10 s after the lid opens: scanning all the time would
 * keep bluetoothd busy.
 */

import Atk from 'gi://Atk';
import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {gettext as _} from 'resource:///org/gnome/shell/extensions/extension.js';

import {BatteryIndicator, showNearbyLevels} from './batteryIndicator.js';

const SHOWN_S = 12;
/* Not again sooner, should the lid open and close in a row */
const AGAIN_S = 25;
const BOTTOM_MARGIN = 64;
const FADE_MS = 200;

export class ProximityCard {
    /* icons: {left, right, case}; onConnect: the Connect button */
    constructor(settings, icons, chargingIcon, onConnect) {
        this._settings = settings;
        this._icons = icons;
        this._chargingIcon = chargingIcon;
        this._onConnect = onConnect;
        this._card = null;
        this._wasOpen = false;
        this._shownUs = 0;
        this._timeoutId = 0;
    }

    /* nearby: the NearbyBattery property; connected: the AirPods are
     * connected here */
    update(nearby, connected) {
        const open = !connected && nearby.LidOpen === true && nearby.Left !== undefined;
        if (open && !this._wasOpen && !this._card &&
            this._settings.get_boolean('proximity-card') &&
            GLib.get_monotonic_time() - this._shownUs > AGAIN_S * GLib.USEC_PER_SEC)
            this._show();
        this._wasOpen = open;

        if (!this._card)
            return;
        if (open)
            this._fill(nearby);
        else
            this._hide();
    }

    destroy() {
        this._stopTimeout();
        this._card?.destroy();
        this._card = null;
    }

    _show() {
        this._shownUs = GLib.get_monotonic_time();

        this._card = new St.BoxLayout({
            style_class: 'popup-menu-content earport-proximity-card',
            /* GNOME 46 and 47 only have the legacy vertical property */
            ...('orientation' in St.BoxLayout.prototype
                ? {orientation: Clutter.Orientation.VERTICAL}
                : {vertical: true}),
            reactive: true,
            accessible_role: Atk.Role.NOTIFICATION,
            opacity: 0,
        });

        const header = new St.BoxLayout({style_class: 'earport-proximity-card-header'});
        this._title = new St.Label({
            style_class: 'earport-proximity-card-title',
            x_expand: true,
            y_align: Clutter.ActorAlign.CENTER,
        });
        const close = new St.Button({
            style_class: 'button earport-proximity-card-close',
            child: new St.Icon({icon_name: 'window-close-symbolic'}),
            accessible_name: _('Close'),
            can_focus: true,
        });
        close.connect('clicked', () => this._hide());
        header.add_child(this._title);
        header.add_child(close);

        this._batteries = [
            new BatteryIndicator('left', _('Left'), this._icons.left, this._chargingIcon),
            new BatteryIndicator('right', _('Right'), this._icons.right, this._chargingIcon),
            new BatteryIndicator('case', _('Case'), this._icons.case, this._chargingIcon),
        ];
        const batteryBox = new St.BoxLayout({
            style_class: 'earport-battery-box',
            x_align: Clutter.ActorAlign.CENTER,
        });
        this._batteries.forEach(battery => batteryBox.add_child(battery));

        const connect = new St.Button({
            style_class: 'button earport-proximity-card-connect',
            label: _('Connect'),
            can_focus: true,
            x_expand: true,
        });
        connect.connect('clicked', () => {
            this._onConnect();
            this._hide();
        });

        this._card.add_child(header);
        this._card.add_child(batteryBox);
        this._card.add_child(connect);
        Main.layoutManager.addTopChrome(this._card);

        this._timeoutId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, SHOWN_S, () => {
            this._timeoutId = 0;
            this._hide();
            return GLib.SOURCE_REMOVE;
        });
    }

    _fill(nearby) {
        this._title.text = nearby.Name || 'AirPods';
        showNearbyLevels(this._batteries, nearby);
        this._card.accessible_name = [this._title.text, ...this._batteries
            .filter(battery => battery.visible)
            .map(battery => battery.accessibleText)].join(', ');

        /* Placed once filled: its size depends on the name and levels */
        if (this._card.opacity === 0) {
            this._place();
            this._card.ease({
                opacity: 255,
                duration: FADE_MS,
                mode: Clutter.AnimationMode.EASE_OUT_QUAD,
            });
        }
    }

    /* Bottom center of the primary monitor, as on an iPhone */
    _place() {
        const monitor = Main.layoutManager.primaryMonitor;
        if (!monitor)
            return;
        const [, width] = this._card.get_preferred_width(-1);
        const [, height] = this._card.get_preferred_height(width);
        const scale = St.ThemeContext.get_for_stage(global.stage).scale_factor;
        this._card.set_position(
            Math.floor(monitor.x + (monitor.width - width) / 2),
            Math.floor(monitor.y + monitor.height - height - BOTTOM_MARGIN * scale));
    }

    _hide() {
        this._stopTimeout();
        const card = this._card;
        this._card = null;
        card?.ease({
            opacity: 0,
            duration: FADE_MS,
            mode: Clutter.AnimationMode.EASE_OUT_QUAD,
            onComplete: () => card.destroy(),
        });
    }

    _stopTimeout() {
        if (this._timeoutId) {
            GLib.source_remove(this._timeoutId);
            this._timeoutId = 0;
        }
    }
}
