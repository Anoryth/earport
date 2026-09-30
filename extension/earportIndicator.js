/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Panel indicator (icon and battery pill) and the connection to the daemon
 */

import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GObject from 'gi://GObject';
import St from 'gi://St';

import * as QuickSettings from 'resource:///org/gnome/shell/ui/quickSettings.js';

import {AirPodsInterface, BUS_NAME, OBJECT_PATH} from './dbusInterface.js';
import {EarPortToggle} from './earportToggle.js';

const AirPodsProxy = Gio.DBusProxy.makeProxyWrapper(AirPodsInterface);

/* Quick Settings indicator */
export const EarPortIndicator = GObject.registerClass(
class EarPortIndicator extends QuickSettings.SystemIndicator {
    constructor(extensionObject) {
        super();

        this._indicator = this._addIndicator();
        this._indicator.icon_name = 'audio-headphones-symbolic';
        this._indicator.visible = false;

        /* Battery pill for the panel: bolt when charging + lowest level */
        this._batteryPill = new St.BoxLayout({
            style_class: 'earport-panel-battery',
            y_align: Clutter.ActorAlign.CENTER,
            visible: false,
        });
        this._chargingIcon = new St.Icon({
            gicon: Gio.icon_new_for_string(`${extensionObject.path}/icons/earport-charging-symbolic.svg`),
            style_class: 'earport-panel-charging-icon',
            y_align: Clutter.ActorAlign.CENTER,
            visible: false,
        });
        this._batteryLabel = new St.Label({
            text: '',
            y_align: Clutter.ActorAlign.CENTER,
        });
        this._batteryPill.add_child(this._chargingIcon);
        this._batteryPill.add_child(this._batteryLabel);

        /* Add pill after indicator icon */
        this.add_child(this._batteryPill);

        this._proxy = null;
        this._propertiesChangedId = 0;
        this._extensionObject = extensionObject;

        /* Create toggle immediately so it's available for addExternalIndicator */
        this._toggle = new EarPortToggle(extensionObject);
        this.quickSettingsItems.push(this._toggle);

        /* Create proxy asynchronously */
        this._createProxy();
    }

    _createProxy() {
        /* Cancelled on destroy: the answer may come after the extension is
         * disabled */
        this._cancellable = new Gio.Cancellable();
        try {
            this._proxy = new AirPodsProxy(
                Gio.DBus.session,
                BUS_NAME,
                OBJECT_PATH,
                (proxy, error) => {
                    if (error) {
                        if (error.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                            return;
                        console.error('EarPort: Failed to connect to daemon:', error.message);
                        this._toggle.setServiceMissing(true);
                        return;
                    }

                    this._onProxyReady();
                },
                this._cancellable
            );
        } catch (e) {
            console.error('EarPort: Error creating proxy:', e.message);
        }
    }

    _onProxyReady() {
        /* Pass proxy to toggle */
        this._toggle.setProxy(this._proxy);

        /* Connect to property changes for panel indicator */
        this._propertiesChangedId = this._proxy.connect('g-properties-changed', () => {
            this._updateIndicator();
        });

        /* No owner: the daemon is not installed (or failed to start) */
        this._nameOwnerId = this._proxy.connect('notify::g-name-owner', () => {
            this._toggle.setServiceMissing(!this._proxy.g_name_owner);
        });
        this._toggle.setServiceMissing(!this._proxy.g_name_owner);

        this._updateIndicator();
    }

    _updateIndicator() {
        if (!this._proxy)
            return;

        const connected = this._proxy.Connected;
        this._indicator.visible = connected;
        this._batteryPill.visible = connected;

        if (connected) {
            const isHeadphones = this._proxy.IsHeadphones || false;
            const left = this._proxy.BatteryLeft;
            const right = this._proxy.BatteryRight;

            /* Get the lowest battery level */
            let lowestBattery = -1;
            if (isHeadphones) {
                lowestBattery = left;
            } else {
                if (left >= 0 && right >= 0) {
                    lowestBattery = Math.min(left, right);
                } else if (left >= 0) {
                    lowestBattery = left;
                } else if (right >= 0) {
                    lowestBattery = right;
                }
            }

            if (lowestBattery >= 0) {
                this._batteryLabel.text = `${lowestBattery}%`;

                const charging = isHeadphones
                    ? this._proxy.ChargingLeft
                    : (this._proxy.ChargingLeft || this._proxy.ChargingRight);

                this._chargingIcon.visible = charging;

                /* Update style based on battery level */
                this._batteryPill.remove_style_class_name('low');
                this._batteryPill.remove_style_class_name('critical');
                this._batteryPill.remove_style_class_name('charging');

                if (charging) {
                    this._batteryPill.add_style_class_name('charging');
                } else if (lowestBattery <= 10) {
                    this._batteryPill.add_style_class_name('critical');
                } else if (lowestBattery <= 20) {
                    this._batteryPill.add_style_class_name('low');
                }
            } else {
                this._batteryLabel.text = '';
                this._batteryPill.visible = false;
            }
        }
    }

    cycleNoiseControlMode() {
        this._toggle?.cycleNoiseControlMode();
    }

    destroy() {
        this._cancellable?.cancel();
        if (this._proxy && this._propertiesChangedId > 0) {
            this._proxy.disconnect(this._propertiesChangedId);
        }
        if (this._proxy && this._nameOwnerId > 0)
            this._proxy.disconnect(this._nameOwnerId);
        this.quickSettingsItems.forEach(item => item.destroy());
        this._toggle = null;
        super.destroy();
    }
});
