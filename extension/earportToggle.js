/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Quick Settings tile and its menu: battery, listening time, noise control
 */

import Atk from 'gi://Atk';
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GObject from 'gi://GObject';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import * as QuickSettings from 'resource:///org/gnome/shell/ui/quickSettings.js';

import {gettext as _} from 'resource:///org/gnome/shell/extensions/extension.js';

import {BatteryIndicator} from './batteryIndicator.js';
import {ConversationVolume} from './conversationVolume.js';
import {NoiseControlButton} from './noiseControlButton.js';
import {Notifications} from './notifications.js';

/* Last service version without the Version property */
const UNVERSIONED_SERVICE = '0.4.0';

/* Major and minor only: a patch release never needs a service update */
function minorVersion(version) {
    const [major = 0, minor = 0] = String(version).split('.').map(n => parseInt(n, 10) || 0);
    return major * 1000 + minor;
}

/* Main Quick Settings toggle */
export const EarPortToggle = GObject.registerClass(
class EarPortToggle extends QuickSettings.QuickMenuToggle {
    constructor(extensionObject) {
        super({
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
        this._modeNames = {
            off: _('Noise Control Off'),
            anc: _('Noise Cancellation'),
            transparency: _('Transparency'),
            adaptive: _('Adaptive'),
        };
        this._batteryIcons = {
            left: Gio.icon_new_for_string(`${iconsDir}/earport-bud-left-symbolic.svg`),
            right: Gio.icon_new_for_string(`${iconsDir}/earport-bud-right-symbolic.svg`),
            case: Gio.icon_new_for_string(`${iconsDir}/earport-case-symbolic.svg`),
        };
        this._chargingIcon = Gio.icon_new_for_string(`${iconsDir}/earport-charging-symbolic.svg`);

        this._settings = extensionObject.getSettings();
        this._notifications = new Notifications(this._settings);
        this._conversationVolume = new ConversationVolume(this._settings);

        this._createMenu();

        /* Clicking the tile cycles through noise control modes */
        this.connect('clicked', () => this.cycleNoiseControlMode());
    }

    setProxy(proxy) {
        this._proxy = proxy;
        this._connectProxySignals();
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

        this._leftBattery = new BatteryIndicator('left', _('Left'), this._batteryIcons.left, this._chargingIcon);
        this._rightBattery = new BatteryIndicator('right', _('Right'), this._batteryIcons.right, this._chargingIcon);
        this._caseBattery = new BatteryIndicator('case', _('Case'), this._batteryIcons.case, this._chargingIcon);

        this._batteryBox.add_child(this._leftBattery);
        this._batteryBox.add_child(this._rightBattery);
        this._batteryBox.add_child(this._caseBattery);

        /* Not clickable, but reachable with the keyboard so screen readers
         * can read the battery levels */
        this._batteryItem = new PopupMenu.PopupBaseMenuItem({
            reactive: false,
            can_focus: true,
        });
        this._batteryItem.accessible_role = Atk.Role.LABEL;
        /* Estimated listening time, below the gauges */
        this._listeningTimeLabel = new St.Label({
            style_class: 'earport-listening-time',
            x_align: Clutter.ActorAlign.CENTER,
            visible: false,
        });

        const batteryColumn = new St.BoxLayout({
            vertical: true,
            x_expand: true,
        });
        batteryColumn.add_child(this._batteryBox);
        batteryColumn.add_child(this._listeningTimeLabel);
        this._batteryItem.add_child(batteryColumn);
        this.menu.addMenuItem(this._batteryItem);

        /* Separator */
        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());

        /* Noise control section */
        this._ncBox = new St.BoxLayout({
            style_class: 'earport-nc-box',
            x_expand: true,
            x_align: Clutter.ActorAlign.CENTER,
        });

        /* The buttons share a narrow row: short labels on screen, full
         * mode names for screen readers */
        this._ncButtons = {
            /* Translators: short noise control button label, keep it under ~8 characters */
            off: new NoiseControlButton('off', _('Off'), this._modeNames.off, this._modeIcons.off),
            /* Translators: short noise control button label, keep it under ~8 characters */
            anc: new NoiseControlButton('anc', _('ANC'), this._modeNames.anc, this._modeIcons.anc),
            /* Translators: short noise control button label for Transparency mode, keep it under ~8 characters */
            transparency: new NoiseControlButton('transparency', _('Hear'), this._modeNames.transparency,
                this._modeIcons.transparency),
            /* Translators: short noise control button label for Adaptive mode, keep it under ~8 characters */
            adaptive: new NoiseControlButton('adaptive', _('Auto'), this._modeNames.adaptive, this._modeIcons.adaptive),
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

        /* Shown when the daemon is not installed: the extension alone can't
         * talk to the AirPods */
        this._installItem = new PopupMenu.PopupImageMenuItem(
            _('How to Install the EarPort Service'),
            'help-browser-symbolic'
        );
        this._installItem.connect('activate', () => {
            const url = `${this._extensionObject.metadata.url}#installation`;
            Gio.AppInfo.launch_default_for_uri(url, null);
        });
        this._installItem.visible = false;
        this.menu.addMenuItem(this._installItem);

        /* The same command installs and updates the service: copy it, to be
         * pasted in a terminal (nothing is run from here) */
        this._copyInstallItem = new PopupMenu.PopupImageMenuItem(
            _('Copy the Installation Command'),
            'edit-copy-symbolic'
        );
        this._copyInstallItem.connect('activate', () => this._copyInstallCommand(false));
        this._copyInstallItem.visible = false;
        this.menu.addMenuItem(this._copyInstallItem);

        /* The extension is updated by extensions.gnome.org, the service is
         * not: say when it is too old for this version */
        this._updateServiceItem = new PopupMenu.PopupImageMenuItem(
            _('Update the EarPort Service'),
            'software-update-available-symbolic'
        );
        this._updateServiceItem.connect('activate', () => this._copyInstallCommand(true));
        this._updateServiceItem.visible = false;
        this.menu.addMenuItem(this._updateServiceItem);

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
        this._signalIds.push(
            this._proxy.connectSignal('SpeakingChanged', this._onSpeakingChanged.bind(this))
        );

        /* Initial state update */
        this._updateState();
    }

    _onPropertiesChanged() {
        this._updateState();
    }

    _onDeviceConnected(_proxy, _sender, [_address, name]) {
        this._notifications.connected(this._proxy?.DisplayName, name, this._proxy?.DeviceModel);
        this._updateState();
    }

    _onDeviceDisconnected(_proxy, _sender, [_address, name]) {
        this._notifications.disconnected(this._proxy?.DisplayName, name);
        this._updateDisconnectedState();
    }

    _onSpeakingChanged(proxy, sender, [speaking]) {
        if (speaking)
            this._conversationVolume.lower(this._proxy?.DeviceAddress);
        else
            this._conversationVolume.restore();
        this._notifications.holdForConversation(speaking);
    }

    _onBatteryChanged(proxy, sender, [left, right, caseBattery]) {
        this._leftBattery.setLevel(left, this._proxy.ChargingLeft);
        this._rightBattery.setLevel(right, this._proxy.ChargingRight);
        this._caseBattery.setLevel(caseBattery, this._proxy.ChargingCase);
        this._updateBatteryAccessibleName();

        /* Check for low battery notifications */
        this._checkLowBattery(left, right);
    }

    _checkLowBattery(left, right) {
        this._notifications.checkLowBattery(left, right,
            this._proxy?.IsHeadphones || false,
            this._proxy?.DisplayName || this._proxy?.DeviceModel || 'AirPods');
    }

    _onNoiseControlChanged(proxy, sender, [mode]) {
        this._updateNoiseControlButtons(mode);
    }

    _updateState() {
        if (!this._proxy)
            return;

        this._updateServiceVersion();

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

            this._updateListeningTime(this._proxy.ListeningTimeRemaining);
            this._updateBatteryAccessibleName();

            /* Check for low battery on state update */
            this._checkLowBattery(batteryLeft, batteryRight);

            /* Update noise control buttons visibility based on features */
            this._updateNoiseControlVisibility(supportsANC, supportsAdaptive);

            /* Update noise control */
            this._setNoiseControlSensitive(true);
            this._updateNoiseControlButtons(this._proxy.NoiseControlMode);
        } else {
            this._updateDisconnectedState();
        }
    }

    setServiceMissing(missing) {
        this._serviceMissing = missing;
        this._installItem.visible = missing;
        this._copyInstallItem.visible = missing;
        this._settingsItem.visible = !missing;
        this._updateServiceVersion();
        if (missing) {
            /* Gone mid-conversation: no SpeakingChanged(false) will come */
            this._onSpeakingChanged(null, null, [false]);
            this._updateDisconnectedState();
        }
    }

    _updateServiceVersion() {
        const service = this._proxy?.Version || UNVERSIONED_SERVICE;
        const extension = this._extensionObject.metadata['version-name'];
        const outdated = !this._serviceMissing && extension !== undefined &&
            minorVersion(service) < minorVersion(extension);
        this._updateServiceItem.visible = outdated;
    }

    _copyInstallCommand(update) {
        const script = `${this._extensionObject.metadata.url}/releases/latest/download/install-daemon.sh`;
        St.Clipboard.get_default().set_text(St.ClipboardType.CLIPBOARD, `curl -fsSL ${script} | bash`);
        this._notifications.commandCopied(update);
    }

    _updateDisconnectedState() {
        this.subtitle = this._serviceMissing ? _('Service not running') : _('Disconnected');
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
        this._updateListeningTime(-1);
        this._batteryItem.accessible_name = _('Disconnected');

        /* Show all noise control buttons and section when disconnected */
        this._ncBox.visible = true;
        for (const button of Object.values(this._ncButtons)) {
            button.visible = true;
            button.setActive(false);
        }
        this._setNoiseControlSensitive(false);

        this._showNearbyBattery();
    }

    /* Not connected here, but seen nearby: on the iPhone, or idle. Empty
     * with an older service. */
    _showNearbyBattery() {
        if (this._serviceMissing || !this._proxy)
            return;

        const nearby = this._proxy.get_cached_property('NearbyBattery')?.recursiveUnpack() ?? {};
        if (nearby.Left === undefined)
            return;

        if (nearby.Host !== 'none')
            this.subtitle = _('Other device');
        if (nearby.Name)
            this.menu.setHeader('audio-headphones-symbolic', nearby.Name);
        this._batteryBox.opacity = 255;

        /* AirPods Max: a single battery, in whichever slot it comes */
        const headphones = nearby.Headphones ?? false;
        this._leftBattery.setHeadphonesMode(headphones);
        this._rightBattery.setHeadphonesMode(headphones);
        this._caseBattery.setHeadphonesMode(headphones);
        if (headphones && nearby.Left < 0)
            this._leftBattery.setLevel(nearby.Right, nearby.RightCharging);
        else
            this._leftBattery.setLevel(nearby.Left, nearby.LeftCharging);
        this._rightBattery.setLevel(nearby.Right, nearby.RightCharging);
        this._caseBattery.setLevel(nearby.Case, nearby.CaseCharging);
        this._updateBatteryAccessibleName();
    }

    _updateBatteryAccessibleName() {
        const parts = [this._leftBattery, this._rightBattery, this._caseBattery]
            .filter(indicator => indicator.visible)
            .map(indicator => indicator.accessibleText);
        if (this._listeningTimeLabel.visible)
            parts.push(this._listeningTimeLabel.text);
        this._batteryItem.accessible_name = parts.join(', ');
    }

    /* The estimate is only accurate to about 10%: round it accordingly */
    _updateListeningTime(minutes) {
        /* null when the daemon is older than the extension */
        if (typeof minutes !== 'number' || minutes < 0) {
            this._listeningTimeLabel.visible = false;
            return;
        }

        let text;
        if (minutes < 5) {
            text = _('Less than 5 min of listening left');
        } else if (minutes < 60) {
            text = _('About %d min of listening left')
                .replace('%d', Math.round(minutes / 5) * 5);
        } else {
            const rounded = Math.round(minutes / 10) * 10;
            /* Translators: hours∶minutes, same format as the shell's battery */
            text = _('About %d∶%02d of listening left')
                .replace('%d', Math.floor(rounded / 60))
                .replace('%02d', String(rounded % 60).padStart(2, '0'));
        }

        this._listeningTimeLabel.text = text;
        this._listeningTimeLabel.visible = true;
    }

    /* Keyboard focus must not land on buttons that do nothing */
    _setNoiseControlSensitive(sensitive) {
        for (const button of Object.values(this._ncButtons)) {
            button.reactive = sensitive;
            button.can_focus = sensitive;
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
        Main.osdWindowManager.show(-1,
            this._modeIcons[mode] ?? Gio.ThemedIcon.new('audio-headphones-symbolic'),
            this._modeNames[mode] ?? mode);
    }

    _openSettings() {
        /* Open advanced settings panel */
        if (this._extensionObject) {
            this._extensionObject.openPreferences();
        }
    }

    destroy() {
        this._conversationVolume.destroy();

        if (this._proxy) {
            if (this._propertiesChangedId > 0) {
                this._proxy.disconnect(this._propertiesChangedId);
            }

            for (const id of this._signalIds) {
                this._proxy.disconnectSignal(id);
            }
        }

        this._notifications.destroy();

        super.destroy();
    }
});
