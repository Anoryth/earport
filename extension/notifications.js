/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Connection and low battery notifications
 */

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as MessageTray from 'resource:///org/gnome/shell/ui/messageTray.js';

import {gettext as _} from 'resource:///org/gnome/shell/extensions/extension.js';

/* What the daemon reports before it knows the model, or once its state has
 * been reset */
const UNKNOWN_NAME = 'Unknown AirPods';

export class Notifications {
    constructor(settings) {
        this._settings = settings;
        this._source = null;
        /* Low battery notified, armed again above the threshold */
        this._lowBatteryNotified = {left: false, right: false};
    }

    /* cachedName: the proxy's DisplayName, which may still be the stale
     * fallback on the first connection after daemon startup; name: the
     * Bluetooth name from the signal */
    connected(cachedName, name, model) {
        this._lowBatteryNotified = {left: false, right: false};

        if (!this._settings.get_boolean('enable-connection-notifications'))
            return;

        const displayName = cachedName && cachedName !== UNKNOWN_NAME
            ? cachedName
            : name || model || 'AirPods';
        this._show(_('%s Connected').replace('%s', displayName));
    }

    disconnected(cachedName, name) {
        if (!this._settings.get_boolean('enable-connection-notifications'))
            return;

        const displayName = cachedName && cachedName !== UNKNOWN_NAME
            ? cachedName
            : name || 'AirPods';
        this._show(_('%s Disconnected').replace('%s', displayName));
    }

    /* AirPods Max have a single battery, reported as the left one */
    checkLowBattery(left, right, isHeadphones, displayName) {
        if (!this._settings.get_boolean('enable-low-battery-notifications'))
            return;

        const threshold = this._settings.get_int('low-battery-threshold');
        const messages = [];

        if (this._crossedLowThreshold('left', left, threshold))
            messages.push(`${isHeadphones ? _('Battery') : _('Left')}: ${left}%`);
        if (!isHeadphones && this._crossedLowThreshold('right', right, threshold))
            messages.push(`${_('Right')}: ${right}%`);

        if (messages.length > 0) {
            this._show(
                _('%s Low Battery').replace('%s', displayName),
                messages.join(', '),
                true);
        }
    }

    /* The installation command was copied to the clipboard */
    commandCopied(update) {
        this._show(_('Command Copied'),
            update
                ? _('Paste it in a terminal to update the EarPort service.')
                : _('Paste it in a terminal to install the EarPort service.'));
    }

    destroy() {
        this._source?.destroy();
        this._source = null;
    }

    /* Whether a battery just went below the threshold: true once per
     * crossing, armed again when the level goes back above it */
    _crossedLowThreshold(pod, level, threshold) {
        if (level > threshold) {
            this._lowBatteryNotified[pod] = false;
            return false;
        }
        if (level <= 0 || this._lowBatteryNotified[pod])
            return false;

        this._lowBatteryNotified[pod] = true;
        return true;
    }

    _getSource() {
        if (this._source === null) {
            this._source = new MessageTray.Source({
                title: 'EarPort',
                iconName: 'audio-headphones-symbolic',
            });
            /* Reset our reference if the source is destroyed externally */
            this._source.connect('destroy', () => {
                this._source = null;
            });
            Main.messageTray.add(this._source);
        }
        return this._source;
    }

    _show(title, body = '', urgent = false) {
        const source = this._getSource();
        const notification = new MessageTray.Notification({
            source,
            title,
            body,
            iconName: 'audio-headphones-symbolic',
        });
        if (urgent)
            notification.urgency = MessageTray.Urgency.HIGH;
        source.addNotification(notification);
    }
}
