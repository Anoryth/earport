/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 LibrePods Contributors
 *
 * LibrePods GNOME Shell Extension
 */

import GObject from 'gi://GObject';
import Gio from 'gi://Gio';
import St from 'gi://St';
import Clutter from 'gi://Clutter';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as QuickSettings from 'resource:///org/gnome/shell/ui/quickSettings.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import * as MessageTray from 'resource:///org/gnome/shell/ui/messageTray.js';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

/* D-Bus interface definition */
const AirPodsInterface = `
<node>
  <interface name="org.librepods.AirPods1">
    <property name="Connected" type="b" access="read"/>
    <property name="DeviceName" type="s" access="read"/>
    <property name="DeviceModel" type="s" access="read"/>
    <property name="DisplayName" type="s" access="read"/>
    <property name="IsHeadphones" type="b" access="read"/>
    <property name="SupportsANC" type="b" access="read"/>
    <property name="SupportsAdaptive" type="b" access="read"/>
    <property name="BatteryLeft" type="i" access="read"/>
    <property name="BatteryRight" type="i" access="read"/>
    <property name="BatteryCase" type="i" access="read"/>
    <property name="ChargingLeft" type="b" access="read"/>
    <property name="ChargingRight" type="b" access="read"/>
    <property name="ChargingCase" type="b" access="read"/>
    <property name="NoiseControlMode" type="s" access="read"/>
    <property name="ConversationalAwareness" type="b" access="read"/>
    <property name="LeftInEar" type="b" access="read"/>
    <property name="RightInEar" type="b" access="read"/>
    <property name="AdaptiveNoiseLevel" type="i" access="read"/>
    <method name="SetNoiseControlMode">
      <arg type="s" name="mode" direction="in"/>
    </method>
    <method name="SetConversationalAwareness">
      <arg type="b" name="enabled" direction="in"/>
    </method>
    <method name="SetAdaptiveNoiseLevel">
      <arg type="i" name="level" direction="in"/>
    </method>
    <signal name="DeviceConnected">
      <arg type="s" name="address"/>
      <arg type="s" name="name"/>
    </signal>
    <signal name="DeviceDisconnected">
      <arg type="s" name="address"/>
      <arg type="s" name="name"/>
    </signal>
    <signal name="BatteryChanged">
      <arg type="i" name="left"/>
      <arg type="i" name="right"/>
      <arg type="i" name="case_battery"/>
    </signal>
    <signal name="NoiseControlModeChanged">
      <arg type="s" name="mode"/>
    </signal>
  </interface>
</node>
`;

const AirPodsProxy = Gio.DBusProxy.makeProxyWrapper(AirPodsInterface);

/* Icon size for battery indicators */
const BATTERY_ICON_SIZE = 32;

/* Battery indicator widget with symbolic icon and progress bar */
const BatteryIndicator = GObject.registerClass(
class BatteryIndicator extends St.BoxLayout {
    _init(type, label) {
        super._init({
            style_class: 'librepods-battery-indicator',
            vertical: true,
            x_align: Clutter.ActorAlign.CENTER,
        });

        this._type = type; // 'left', 'right', 'case'
        this._label = label;

        /* Determine icon based on type */
        let iconName;
        if (this._type === 'case') {
            iconName = 'battery-symbolic';
        } else {
            iconName = 'audio-headphones-symbolic';
        }

        /* Icon widget */
        this._icon = new St.Icon({
            icon_name: iconName,
            icon_size: BATTERY_ICON_SIZE,
            style_class: 'librepods-battery-icon',
        });

        /* Progress bar container */
        this._progressContainer = new St.BoxLayout({
            style_class: 'librepods-progress-container',
            x_align: Clutter.ActorAlign.CENTER,
        });

        /* Progress bar fill */
        this._progressFill = new St.Widget({
            style_class: 'librepods-progress-fill',
        });
        this._progressContainer.add_child(this._progressFill);

        this._levelLabel = new St.Label({
            text: '--',
            style_class: 'librepods-battery-level',
        });

        this._nameLabel = new St.Label({
            text: label,
            style_class: 'librepods-battery-name',
        });

        this.add_child(this._icon);
        this.add_child(this._progressContainer);
        this.add_child(this._levelLabel);
        this.add_child(this._nameLabel);

        this._level = -1;
        this._charging = false;
        this._currentStyleState = null;
    }

    setHeadphonesMode(isHeadphones, deviceModel = null) {
        this._isHeadphones = isHeadphones;

        if (this._type === 'left') {
            /* For headphones, left indicator becomes the unified battery */
            if (isHeadphones) {
                this._nameLabel.text = deviceModel || 'Headphones';
                this._icon.icon_name = 'audio-headphones-symbolic';
            } else {
                this._nameLabel.text = this._label;
                this._icon.icon_name = 'audio-headphones-symbolic';
            }
            this.visible = true;
        } else if (this._type === 'right' || this._type === 'case') {
            /* Hide right and case for headphones (AirPods Max) */
            this.visible = !isHeadphones;
        }
    }

    setLevel(level, charging = false) {
        /* Skip update if nothing changed */
        if (this._level === level && this._charging === charging)
            return;

        this._level = level;
        this._charging = charging;

        if (level < 0) {
            this._levelLabel.text = '--';
            this._icon.opacity = 128;
            this._progressContainer.opacity = 128;
            this._progressFill.width = 0;
            this._setStyleState(null);
        } else {
            this._levelLabel.text = `${level}%`;
            this._icon.opacity = 255;
            this._progressContainer.opacity = 255;
            this._progressFill.width = Math.round(level * 0.4);

            if (charging) {
                this._setStyleState('charging');
            } else if (level <= 20) {
                this._setStyleState('low');
            } else if (level <= 50) {
                this._setStyleState('medium');
            } else {
                this._setStyleState(null);
            }
        }
    }

    _setStyleState(state) {
        /* Only update if state changed */
        if (this._currentStyleState === state)
            return;

        /* Remove previous state */
        if (this._currentStyleState) {
            if (this._currentStyleState === 'charging') {
                this._levelLabel.remove_style_class_name('charging');
                this._progressFill.remove_style_class_name('charging');
            } else if (this._currentStyleState === 'low') {
                this._levelLabel.remove_style_class_name('low-battery');
                this._progressFill.remove_style_class_name('low');
            } else if (this._currentStyleState === 'medium') {
                this._progressFill.remove_style_class_name('medium');
            }
        }

        /* Apply new state */
        if (state === 'charging') {
            this._levelLabel.add_style_class_name('charging');
            this._progressFill.add_style_class_name('charging');
        } else if (state === 'low') {
            this._levelLabel.add_style_class_name('low-battery');
            this._progressFill.add_style_class_name('low');
        } else if (state === 'medium') {
            this._progressFill.add_style_class_name('medium');
        }

        this._currentStyleState = state;
    }
});

/* Noise control mode button */
const NoiseControlButton = GObject.registerClass(
class NoiseControlButton extends St.Button {
    _init(mode, label, iconName) {
        super._init({
            style_class: 'librepods-nc-button',
            can_focus: true,
            child: new St.BoxLayout({
                vertical: true,
                x_align: Clutter.ActorAlign.CENTER,
            }),
        });

        this._mode = mode;

        const icon = new St.Icon({
            icon_name: iconName,
            icon_size: 18,
            style_class: 'librepods-nc-icon',
        });

        const labelWidget = new St.Label({
            text: label,
            style_class: 'librepods-nc-label',
            x_align: Clutter.ActorAlign.CENTER,
        });

        this.child.add_child(icon);
        this.child.add_child(labelWidget);
    }

    get mode() {
        return this._mode;
    }

    setActive(active) {
        if (active) {
            this.add_style_class_name('active');
        } else {
            this.remove_style_class_name('active');
        }
    }
});

/* Main Quick Settings toggle */
const LibrePodsToggle = GObject.registerClass(
class LibrePodsToggle extends QuickSettings.QuickMenuToggle {
    _init(extensionObject) {
        super._init({
            title: 'AirPods',
            subtitle: 'Disconnected',
            iconName: 'audio-headphones-symbolic',
            toggleMode: false,
        });

        this._extensionObject = extensionObject;
        this._proxy = null;
        this._propertiesChangedId = 0;
        this._signalIds = [];

        /* Load settings */
        this._settings = extensionObject.getSettings();

        /* Track low battery notification state */
        this._lowBatteryNotified = {left: false, right: false};

        /* Create notification source */
        this._notificationSource = null;

        this._createMenu();
    }

    setProxy(proxy) {
        this._proxy = proxy;
        this._connectProxySignals();
    }

    _getNotificationSource() {
        if (this._notificationSource === null) {
            this._notificationSource = new MessageTray.Source({
                title: 'LibrePods',
                iconName: 'audio-headphones-symbolic',
            });
            Main.messageTray.add(this._notificationSource);
        }
        return this._notificationSource;
    }

    _showNotification(title, body) {
        const source = this._getNotificationSource();
        const notification = new MessageTray.Notification({
            source: source,
            title: title,
            body: body,
            iconName: 'audio-headphones-symbolic',
        });
        source.addNotification(notification);
    }

    _createMenu() {
        /* Header - will be updated when connected */
        this.menu.setHeader('audio-headphones-symbolic', 'AirPods');

        /* Battery section */
        this._batteryBox = new St.BoxLayout({
            style_class: 'librepods-battery-box',
            x_expand: true,
            x_align: Clutter.ActorAlign.CENTER,
        });

        this._leftBattery = new BatteryIndicator('left', 'Left');
        this._rightBattery = new BatteryIndicator('right', 'Right');
        this._caseBattery = new BatteryIndicator('case', 'Case');

        this._batteryBox.add_child(this._leftBattery);
        this._batteryBox.add_child(this._rightBattery);
        this._batteryBox.add_child(this._caseBattery);

        const batteryItem = new PopupMenu.PopupBaseMenuItem({
            reactive: false,
            can_focus: false,
        });
        batteryItem.add_child(this._batteryBox);
        this.menu.addMenuItem(batteryItem);

        /* Separator */
        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());

        /* Noise control section */
        this._ncBox = new St.BoxLayout({
            style_class: 'librepods-nc-box',
            x_expand: true,
            x_align: Clutter.ActorAlign.CENTER,
        });

        this._ncButtons = {
            off: new NoiseControlButton('off', 'Off', 'audio-speakers-symbolic'),
            anc: new NoiseControlButton('anc', 'ANC', 'microphone-sensitivity-muted-symbolic'),
            transparency: new NoiseControlButton('transparency', 'Hear', 'audio-input-microphone-high-symbolic'),
            adaptive: new NoiseControlButton('adaptive', 'Auto', 'emblem-synchronizing-symbolic'),
        };

        for (const [mode, button] of Object.entries(this._ncButtons)) {
            button.connect('clicked', () => this._setNoiseControlMode(mode));
            this._ncBox.add_child(button);
        }

        const ncItem = new PopupMenu.PopupBaseMenuItem({
            reactive: false,
            can_focus: false,
        });
        ncItem.add_child(this._ncBox);
        this.menu.addMenuItem(ncItem);

        /* Separator */
        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());

        /* Settings button */
        this._settingsItem = new PopupMenu.PopupImageMenuItem(
            'Advanced Settings',
            'emblem-system-symbolic'
        );
        this._settingsItem.connect('activate', () => this._openSettings());
        this.menu.addMenuItem(this._settingsItem);

        /* Set initial disconnected state */
        this._updateDisconnectedState();
    }

    _connectProxySignals() {
        if (!this._proxy)
            return;

        /* Connect to property changes */
        this._propertiesChangedId = this._proxy.connect(
            'g-properties-changed',
            this._onPropertiesChanged.bind(this)
        );

        /* Connect to signals */
        this._signalIds.push(
            this._proxy.connectSignal('DeviceConnected', this._onDeviceConnected.bind(this))
        );
        this._signalIds.push(
            this._proxy.connectSignal('DeviceDisconnected', this._onDeviceDisconnected.bind(this))
        );
        this._signalIds.push(
            this._proxy.connectSignal('BatteryChanged', this._onBatteryChanged.bind(this))
        );
        this._signalIds.push(
            this._proxy.connectSignal('NoiseControlModeChanged', this._onNoiseControlChanged.bind(this))
        );

        /* Initial state update */
        this._updateState();
    }

    _onPropertiesChanged(proxy, changed, invalidated) {
        this._updateState();
    }

    _onDeviceConnected(proxy, sender, [address, name]) {
        console.log(`LibrePods: Device connected - ${name}`);

        /* Reset low battery notification state */
        this._lowBatteryNotified = {left: false, right: false};

        /* Show connection notification with display name */
        if (this._settings.get_boolean('enable-connection-notifications')) {
            const displayName = this._proxy?.DisplayName || this._proxy?.DeviceModel || name || 'AirPods';
            this._showNotification(`${displayName} Connected`, '');
        }

        this._updateState();
    }

    _onDeviceDisconnected(proxy, sender, [address, name]) {
        console.log(`LibrePods: Device disconnected - ${name}`);

        /* Show disconnection notification with display name */
        if (this._settings.get_boolean('enable-connection-notifications')) {
            const displayName = this._proxy?.DisplayName || this._proxy?.DeviceModel || name || 'AirPods';
            this._showNotification(`${displayName} Disconnected`, '');
        }

        this._updateDisconnectedState();
    }

    _onBatteryChanged(proxy, sender, [left, right, caseBattery]) {
        this._leftBattery.setLevel(left, this._proxy.ChargingLeft);
        this._rightBattery.setLevel(right, this._proxy.ChargingRight);
        this._caseBattery.setLevel(caseBattery, this._proxy.ChargingCase);

        /* Check for low battery notifications */
        this._checkLowBattery(left, right);
    }

    _checkLowBattery(left, right) {
        if (!this._settings.get_boolean('enable-low-battery-notifications'))
            return;

        const threshold = this._settings.get_int('low-battery-threshold');
        const isHeadphones = this._proxy?.IsHeadphones || false;
        const displayName = this._proxy?.DisplayName || this._proxy?.DeviceModel || 'AirPods';

        if (isHeadphones) {
            /* For AirPods Max, only check left (main battery) */
            if (left >= 0 && left <= threshold && !this._lowBatteryNotified.left) {
                this._lowBatteryNotified.left = true;
                this._showNotification(`${displayName} Low Battery`, `Battery at ${left}%`);
            } else if (left > threshold) {
                this._lowBatteryNotified.left = false;
            }
        } else {
            /* For earbuds, check both */
            let messages = [];

            if (left >= 0 && left <= threshold && !this._lowBatteryNotified.left) {
                this._lowBatteryNotified.left = true;
                messages.push(`Left: ${left}%`);
            } else if (left > threshold) {
                this._lowBatteryNotified.left = false;
            }

            if (right >= 0 && right <= threshold && !this._lowBatteryNotified.right) {
                this._lowBatteryNotified.right = true;
                messages.push(`Right: ${right}%`);
            } else if (right > threshold) {
                this._lowBatteryNotified.right = false;
            }

            if (messages.length > 0) {
                this._showNotification(`${displayName} Low Battery`, messages.join(', '));
            }
        }
    }

    _onNoiseControlChanged(proxy, sender, [mode]) {
        this._updateNoiseControlButtons(mode);
    }

    _updateState() {
        if (!this._proxy)
            return;

        const connected = this._proxy.Connected;

        if (connected) {
            const isHeadphones = this._proxy.IsHeadphones || false;
            const supportsANC = this._proxy.SupportsANC || false;
            const supportsAdaptive = this._proxy.SupportsAdaptive || false;
            const deviceModel = this._proxy.DeviceModel || null;
            const displayName = this._proxy.DisplayName || this._proxy.DeviceModel || 'AirPods';

            this.subtitle = displayName;
            this.checked = true;
            this._batteryBox.opacity = 255;

            /* Update menu header with display name */
            this.menu.setHeader('audio-headphones-symbolic', displayName);

            /* Update layout based on device type */
            this._leftBattery.setHeadphonesMode(isHeadphones, deviceModel);
            this._rightBattery.setHeadphonesMode(isHeadphones);
            this._caseBattery.setHeadphonesMode(isHeadphones);

            /* Update battery */
            const batteryLeft = this._proxy.BatteryLeft;
            const batteryRight = this._proxy.BatteryRight;
            this._leftBattery.setLevel(batteryLeft, this._proxy.ChargingLeft);
            if (!isHeadphones) {
                this._rightBattery.setLevel(batteryRight, this._proxy.ChargingRight);
                this._caseBattery.setLevel(this._proxy.BatteryCase, this._proxy.ChargingCase);
            }

            /* Check for low battery on state update */
            this._checkLowBattery(batteryLeft, batteryRight);

            /* Update noise control buttons visibility based on features */
            this._updateNoiseControlVisibility(supportsANC, supportsAdaptive);

            /* Update noise control */
            this._updateNoiseControlButtons(this._proxy.NoiseControlMode);
        } else {
            this._updateDisconnectedState();
        }
    }

    _updateDisconnectedState() {
        this.subtitle = 'Disconnected';
        this.checked = false;
        this._batteryBox.opacity = 128;
        this._ncBox.opacity = 128;

        /* Reset menu header to default */
        this.menu.setHeader('audio-headphones-symbolic', 'AirPods');

        /* Reset to earbuds mode (show all indicators) */
        this._leftBattery.setHeadphonesMode(false);
        this._rightBattery.setHeadphonesMode(false);
        this._caseBattery.setHeadphonesMode(false);

        this._leftBattery.setLevel(-1);
        this._rightBattery.setLevel(-1);
        this._caseBattery.setLevel(-1);

        /* Show all noise control buttons and section when disconnected */
        this._ncBox.visible = true;
        for (const button of Object.values(this._ncButtons)) {
            button.visible = true;
            button.setActive(false);
        }
    }

    _updateNoiseControlVisibility(supportsANC, supportsAdaptive) {
        /* Show/hide noise control section based on features */
        const hasNoiseControl = supportsANC;

        this._ncBox.visible = hasNoiseControl;

        if (hasNoiseControl) {
            /* Always show Off button if ANC is supported */
            this._ncButtons.off.visible = true;
            this._ncButtons.anc.visible = true;
            this._ncButtons.transparency.visible = true;
            this._ncButtons.adaptive.visible = supportsAdaptive;
        }
    }

    _updateNoiseControlButtons(mode) {
        for (const [buttonMode, button] of Object.entries(this._ncButtons)) {
            button.setActive(buttonMode === mode);
        }
    }

    _setNoiseControlMode(mode) {
        if (!this._proxy || !this._proxy.Connected)
            return;

        this._proxy.SetNoiseControlModeRemote(mode, (result, error) => {
            if (error) {
                console.error('LibrePods: Failed to set noise control mode:', error.message);
            }
        });
    }

    _openSettings() {
        /* Open advanced settings panel */
        if (this._extensionObject) {
            this._extensionObject.openPreferences();
        }
    }

    destroy() {
        if (this._proxy) {
            if (this._propertiesChangedId > 0) {
                this._proxy.disconnect(this._propertiesChangedId);
            }

            for (const id of this._signalIds) {
                this._proxy.disconnectSignal(id);
            }
        }

        if (this._notificationSource) {
            this._notificationSource.destroy();
            this._notificationSource = null;
        }

        super.destroy();
    }
});

/* Quick Settings indicator */
const LibrePodsIndicator = GObject.registerClass(
class LibrePodsIndicator extends QuickSettings.SystemIndicator {
    _init(extensionObject) {
        super._init();

        this._indicator = this._addIndicator();
        this._indicator.icon_name = 'audio-headphones-symbolic';
        this._indicator.visible = false;

        /* Create battery label for panel */
        this._batteryLabel = new St.Label({
            text: '',
            y_align: Clutter.ActorAlign.CENTER,
            style_class: 'librepods-panel-battery',
        });
        this._batteryLabel.visible = false;

        /* Add label after indicator icon */
        this.add_child(this._batteryLabel);

        this._proxy = null;
        this._propertiesChangedId = 0;
        this._extensionObject = extensionObject;

        /* Create toggle immediately so it's available for addExternalIndicator */
        this._toggle = new LibrePodsToggle(extensionObject);
        this.quickSettingsItems.push(this._toggle);

        /* Create proxy asynchronously */
        this._createProxy();
    }

    _createProxy() {
        try {
            this._proxy = new AirPodsProxy(
                Gio.DBus.session,
                'org.librepods.Daemon',
                '/org/librepods/AirPods',
                (proxy, error) => {
                    if (error) {
                        console.error('LibrePods: Failed to connect to daemon:', error.message);
                        return;
                    }

                    this._onProxyReady();
                }
            );
        } catch (e) {
            console.error('LibrePods: Error creating proxy:', e.message);
        }
    }

    _onProxyReady() {
        /* Pass proxy to toggle */
        this._toggle.setProxy(this._proxy);

        /* Connect to property changes for panel indicator */
        this._propertiesChangedId = this._proxy.connect('g-properties-changed', () => {
            this._updateIndicator();
        });

        this._updateIndicator();
    }

    _updateIndicator() {
        if (!this._proxy)
            return;

        const connected = this._proxy.Connected;
        this._indicator.visible = connected;
        this._batteryLabel.visible = connected;

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

                /* Update style based on battery level */
                this._batteryLabel.remove_style_class_name('low');
                this._batteryLabel.remove_style_class_name('critical');

                if (lowestBattery <= 10) {
                    this._batteryLabel.add_style_class_name('critical');
                } else if (lowestBattery <= 20) {
                    this._batteryLabel.add_style_class_name('low');
                }
            } else {
                this._batteryLabel.text = '';
            }
        }
    }

    destroy() {
        if (this._proxy && this._propertiesChangedId > 0) {
            this._proxy.disconnect(this._propertiesChangedId);
        }
        if (this._toggle) {
            this._toggle.destroy();
        }
        super.destroy();
    }
});

/* Extension class */
export default class LibrePodsExtension extends Extension {
    enable() {
        this._indicator = new LibrePodsIndicator(this);

        Main.panel.statusArea.quickSettings.addExternalIndicator(this._indicator);
    }

    disable() {
        if (this._indicator) {
            this._indicator.destroy();
            this._indicator = null;
        }
    }
}
