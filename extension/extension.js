/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * EarPort Shell Extension
 */

import GObject from 'gi://GObject';
import Gio from 'gi://Gio';
import St from 'gi://St';
import Clutter from 'gi://Clutter';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import Cairo from 'cairo';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as QuickSettings from 'resource:///org/gnome/shell/ui/quickSettings.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import * as MessageTray from 'resource:///org/gnome/shell/ui/messageTray.js';

import {Extension, gettext as _} from 'resource:///org/gnome/shell/extensions/extension.js';

import {AirPodsInterface, BUS_NAME, OBJECT_PATH} from './dbusInterface.js';


const AirPodsProxy = Gio.DBusProxy.makeProxyWrapper(AirPodsInterface);

/* Ring geometry (logical px; the widget size comes from CSS) */
const RING_LINE_WIDTH = 4;
const RING_ICON_SIZE = 20;

/* Battery indicator widget: circular progress ring around a symbolic icon,
 * with percentage and name below. The ring color comes from the widget's
 * themed foreground color, so all state styling lives in the stylesheet. */
const BatteryIndicator = GObject.registerClass(
class BatteryIndicator extends St.BoxLayout {
    _init(type, label, gicon) {
        super._init({
            style_class: 'earport-battery-indicator',
            vertical: true,
            x_align: Clutter.ActorAlign.CENTER,
        });

        this._type = type; // 'left', 'right', 'case'
        this._label = label;
        this._defaultGicon = gicon;

        this._ring = new St.DrawingArea({
            style_class: 'earport-battery-ring',
        });
        this._ring.connect('repaint', area => this._drawRing(area));
        this._ring.connect('style-changed', () => this._ring.queue_repaint());

        this._icon = new St.Icon({
            gicon,
            icon_size: RING_ICON_SIZE,
            style_class: 'earport-battery-icon',
            x_align: Clutter.ActorAlign.CENTER,
            y_align: Clutter.ActorAlign.CENTER,
            x_expand: true,
            y_expand: true,
        });

        /* Stack the icon on top of the ring */
        this._ringBin = new St.Widget({
            layout_manager: new Clutter.BinLayout(),
            x_align: Clutter.ActorAlign.CENTER,
        });
        this._ringBin.add_child(this._ring);
        this._ringBin.add_child(this._icon);

        this._levelLabel = new St.Label({
            text: '--',
            style_class: 'earport-battery-level',
        });

        this._nameLabel = new St.Label({
            text: label,
            style_class: 'earport-battery-name',
            opacity: 160,
        });

        this.add_child(this._ringBin);
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
                this._nameLabel.text = deviceModel || _('Headphones');
                this._icon.gicon = Gio.ThemedIcon.new('audio-headphones-symbolic');
            } else {
                this._nameLabel.text = this._label;
                this._icon.gicon = this._defaultGicon;
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
            this._ringBin.opacity = 128;
            this._setStyleState(null);
        } else {
            this._levelLabel.text = `${level}%`;
            this._ringBin.opacity = 255;

            if (charging) {
                this._setStyleState('charging');
            } else if (level <= 10) {
                this._setStyleState('critical');
            } else if (level <= 20) {
                this._setStyleState('low');
            } else {
                this._setStyleState(null);
            }
        }

        this._ring.queue_repaint();
    }

    _setStyleState(state) {
        if (this._currentStyleState === state)
            return;

        if (this._currentStyleState)
            this._ring.remove_style_class_name(this._currentStyleState);
        if (state)
            this._ring.add_style_class_name(state);

        this._currentStyleState = state;
    }

    _drawRing(area) {
        const cr = area.get_context();
        const themeNode = area.get_theme_node();
        const [width, height] = area.get_surface_size();

        const cx = width / 2;
        const cy = height / 2;
        const radius = Math.min(width, height) / 2 - RING_LINE_WIDTH / 2;
        const color = themeNode.get_foreground_color();

        cr.setLineWidth(RING_LINE_WIDTH);

        /* Track: themed color, well faded */
        cr.setSourceRGBA(color.red / 255, color.green / 255, color.blue / 255,
            (color.alpha / 255) * 0.25);
        cr.arc(cx, cy, radius, 0, 2 * Math.PI);
        cr.stroke();

        /* Progress arc, clockwise from the top */
        if (this._level >= 0) {
            const fraction = Math.min(this._level, 100) / 100;
            cr.setSourceRGBA(color.red / 255, color.green / 255,
                color.blue / 255, color.alpha / 255);
            cr.setLineCap(Cairo.LineCap.ROUND);
            cr.arc(cx, cy, radius,
                -Math.PI / 2, -Math.PI / 2 + 2 * Math.PI * fraction);
            cr.stroke();
        }

        cr.$dispose();
    }
});

/* Noise control mode button */
const NoiseControlButton = GObject.registerClass(
class NoiseControlButton extends St.Button {
    _init(mode, label, gicon) {
        super._init({
            style_class: 'earport-nc-button',
            can_focus: true,
            accessible_name: label,
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

    setActive(active) {
        if (active) {
            this.add_style_class_name('active');
        } else {
            this.remove_style_class_name('active');
        }
    }
});

/* Main Quick Settings toggle */
const EarPortToggle = GObject.registerClass(
class EarPortToggle extends QuickSettings.QuickMenuToggle {
    _init(extensionObject) {
        super._init({
            title: 'AirPods',
            subtitle: _('Disconnected'),
            iconName: 'audio-headphones-symbolic',
            toggleMode: false,
        });

        this._extensionObject = extensionObject;
        this._proxy = null;
        this._propertiesChangedId = 0;
        this._signalIds = [];

        /* Custom symbolic icons shipped with the extension */
        const iconsDir = `${extensionObject.path}/icons`;
        this._modeIcons = {
            off: Gio.icon_new_for_string(`${iconsDir}/earport-nc-off-symbolic.svg`),
            anc: Gio.icon_new_for_string(`${iconsDir}/earport-nc-anc-symbolic.svg`),
            transparency: Gio.icon_new_for_string(`${iconsDir}/earport-nc-transparency-symbolic.svg`),
            adaptive: Gio.icon_new_for_string(`${iconsDir}/earport-nc-adaptive-symbolic.svg`),
        };
        this._batteryIcons = {
            left: Gio.icon_new_for_string(`${iconsDir}/earport-bud-left-symbolic.svg`),
            right: Gio.icon_new_for_string(`${iconsDir}/earport-bud-right-symbolic.svg`),
            case: Gio.icon_new_for_string(`${iconsDir}/earport-case-symbolic.svg`),
        };

        /* Load settings */
        this._settings = extensionObject.getSettings();

        /* Track low battery notification state */
        this._lowBatteryNotified = {left: false, right: false};

        /* Create notification source */
        this._notificationSource = null;

        this._createMenu();

        /* Clicking the tile cycles through noise control modes */
        this.connect('clicked', () => this.cycleNoiseControlMode());
    }

    setProxy(proxy) {
        this._proxy = proxy;
        this._connectProxySignals();
    }

    _getNotificationSource() {
        if (this._notificationSource === null) {
            this._notificationSource = new MessageTray.Source({
                title: 'EarPort',
                iconName: 'audio-headphones-symbolic',
            });
            /* Reset our reference if the source is destroyed externally */
            this._notificationSource.connect('destroy', () => {
                this._notificationSource = null;
            });
            Main.messageTray.add(this._notificationSource);
        }
        return this._notificationSource;
    }

    _showNotification(title, body = '', urgent = false) {
        const source = this._getNotificationSource();
        const notification = new MessageTray.Notification({
            source: source,
            title: title,
            body: body,
            iconName: 'audio-headphones-symbolic',
        });
        if (urgent)
            notification.urgency = MessageTray.Urgency.HIGH;
        source.addNotification(notification);
    }

    _createMenu() {
        /* Header - will be updated when connected */
        this.menu.setHeader('audio-headphones-symbolic', 'AirPods');

        /* Battery section */
        this._batteryBox = new St.BoxLayout({
            style_class: 'earport-battery-box',
            x_expand: true,
            x_align: Clutter.ActorAlign.CENTER,
        });

        this._leftBattery = new BatteryIndicator('left', _('Left'), this._batteryIcons.left);
        this._rightBattery = new BatteryIndicator('right', _('Right'), this._batteryIcons.right);
        this._caseBattery = new BatteryIndicator('case', _('Case'), this._batteryIcons.case);

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
            style_class: 'earport-nc-box',
            x_expand: true,
            x_align: Clutter.ActorAlign.CENTER,
        });

        this._ncButtons = {
            off: new NoiseControlButton('off', _('Off'), this._modeIcons.off),
            anc: new NoiseControlButton('anc', _('ANC'), this._modeIcons.anc),
            transparency: new NoiseControlButton('transparency', _('Hear'), this._modeIcons.transparency),
            adaptive: new NoiseControlButton('adaptive', _('Auto'), this._modeIcons.adaptive),
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
            _('Advanced Settings'),
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
        console.log(`EarPort: Device connected - ${name}`);

        /* Reset low battery notification state */
        this._lowBatteryNotified = {left: false, right: false};

        /* Show connection notification with display name. The proxy's cached
         * DisplayName may still be the stale "Unknown AirPods" fallback on
         * the first connection after daemon startup, so prefer the bluetooth
         * name from the signal in that case. */
        if (this._settings.get_boolean('enable-connection-notifications')) {
            const cached = this._proxy?.DisplayName;
            const displayName = (cached && cached !== 'Unknown AirPods')
                ? cached
                : (name || this._proxy?.DeviceModel || 'AirPods');
            this._showNotification(_('%s Connected').replace('%s', displayName));
        }

        this._updateState();
    }

    _onDeviceDisconnected(proxy, sender, [address, name]) {
        console.log(`EarPort: Device disconnected - ${name}`);

        /* Show disconnection notification with display name. Skip the
         * "Unknown AirPods" fallback that the daemon reports once its state
         * has already been reset. */
        if (this._settings.get_boolean('enable-connection-notifications')) {
            const cached = this._proxy?.DisplayName;
            const displayName = (cached && cached !== 'Unknown AirPods')
                ? cached
                : (name || 'AirPods');
            this._showNotification(_('%s Disconnected').replace('%s', displayName));
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
            if (left > 0 && left <= threshold && !this._lowBatteryNotified.left) {
                this._lowBatteryNotified.left = true;
                this._showNotification(
                    _('%s Low Battery').replace('%s', displayName),
                    `${_('Battery')}: ${left}%`,
                    true);
            } else if (left > threshold) {
                this._lowBatteryNotified.left = false;
            }
        } else {
            /* For earbuds, check both */
            let messages = [];

            if (left > 0 && left <= threshold && !this._lowBatteryNotified.left) {
                this._lowBatteryNotified.left = true;
                messages.push(`${_('Left')}: ${left}%`);
            } else if (left > threshold) {
                this._lowBatteryNotified.left = false;
            }

            if (right > 0 && right <= threshold && !this._lowBatteryNotified.right) {
                this._lowBatteryNotified.right = true;
                messages.push(`${_('Right')}: ${right}%`);
            } else if (right > threshold) {
                this._lowBatteryNotified.right = false;
            }

            if (messages.length > 0) {
                this._showNotification(
                    _('%s Low Battery').replace('%s', displayName),
                    messages.join(', '),
                    true);
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
            this._ncBox.opacity = 255;

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
        this.subtitle = _('Disconnected');
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
                console.error('EarPort: Failed to set noise control mode:', error.message);
            }
        });
    }

    /* Cycle to the next noise control mode, following the long-press
     * configuration (same modes as the stem gesture). Used by the tile
     * click and the keyboard shortcut. */
    cycleNoiseControlMode() {
        if (!this._proxy || !this._proxy.Connected || !this._proxy.SupportsANC)
            return;

        const order = ['anc', 'transparency', 'adaptive', 'off'];
        const enabled = {
            /* Treat missing properties (older daemon) as enabled */
            anc: this._proxy.ListeningModeANC !== false,
            transparency: this._proxy.ListeningModeTransparency !== false,
            adaptive: this._proxy.ListeningModeAdaptive !== false &&
                (this._proxy.SupportsAdaptive || false),
            off: this._proxy.ListeningModeOff === true,
        };

        const cycle = order.filter(mode => enabled[mode]);
        if (cycle.length < 2)
            return;

        const currentIndex = cycle.indexOf(this._proxy.NoiseControlMode);
        const next = cycle[(currentIndex + 1) % cycle.length];

        this._setNoiseControlMode(next);
        this._updateNoiseControlButtons(next);
        this._showModeOsd(next);
    }

    _showModeOsd(mode) {
        const labels = {
            off: _('Noise Control Off'),
            anc: _('Noise Cancellation'),
            transparency: _('Transparency'),
            adaptive: _('Adaptive'),
        };

        Main.osdWindowManager.show(-1,
            this._modeIcons[mode] ?? Gio.ThemedIcon.new('audio-headphones-symbolic'),
            labels[mode] ?? mode);
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
const EarPortIndicator = GObject.registerClass(
class EarPortIndicator extends QuickSettings.SystemIndicator {
    _init(extensionObject) {
        super._init();

        this._indicator = this._addIndicator();
        this._indicator.icon_name = 'audio-headphones-symbolic';
        this._indicator.visible = false;

        /* Create battery label for panel */
        this._batteryLabel = new St.Label({
            text: '',
            y_align: Clutter.ActorAlign.CENTER,
            style_class: 'earport-panel-battery',
        });
        this._batteryLabel.visible = false;

        /* Add label after indicator icon */
        this.add_child(this._batteryLabel);

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
        try {
            this._proxy = new AirPodsProxy(
                Gio.DBus.session,
                BUS_NAME,
                OBJECT_PATH,
                (proxy, error) => {
                    if (error) {
                        console.error('EarPort: Failed to connect to daemon:', error.message);
                        return;
                    }

                    this._onProxyReady();
                }
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

                const charging = isHeadphones
                    ? this._proxy.ChargingLeft
                    : (this._proxy.ChargingLeft || this._proxy.ChargingRight);

                /* Update style based on battery level */
                this._batteryLabel.remove_style_class_name('low');
                this._batteryLabel.remove_style_class_name('critical');
                this._batteryLabel.remove_style_class_name('charging');

                if (charging) {
                    this._batteryLabel.add_style_class_name('charging');
                } else if (lowestBattery <= 10) {
                    this._batteryLabel.add_style_class_name('critical');
                } else if (lowestBattery <= 20) {
                    this._batteryLabel.add_style_class_name('low');
                }
            } else {
                this._batteryLabel.text = '';
                this._batteryLabel.visible = false;
            }
        }
    }

    cycleNoiseControlMode() {
        this._toggle?.cycleNoiseControlMode();
    }

    destroy() {
        if (this._proxy && this._propertiesChangedId > 0) {
            this._proxy.disconnect(this._propertiesChangedId);
        }
        this.quickSettingsItems.forEach(item => item.destroy());
        this._toggle = null;
        super.destroy();
    }
});

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
